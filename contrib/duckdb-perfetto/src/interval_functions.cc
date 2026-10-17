/*
 * Copyright (C) 2026 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// PROTOTYPE: interval primitives over any DuckDB relation.
//
// The RFC-0041 interval operators which TraceProcessor does not implement yet,
// as DuckDB table functions, so that they apply alike to TP tables, PERFETTO
// blocks, Polars / pandas DataFrames, parquet files...:
//
//   SELECT * FROM interval_merge(kernels, per := stream);
//
// Like perfetto_extension.cc, this file includes DuckDB headers and is built
// with exceptions and RTTI. The interval logic itself is in interval_ops.cc.
//
// Every function has two layers:
//  * A bind_replace which rewrites the call into a DuckDB query. The query
//    groups the input rows into lanes (the PER key) with GROUP BY, packs each
//    lane's (ts, dur, row id) triples into a LIST and calls
//    __perfetto_interval_kernel on the lanes. Row-preserving operators then
//    join the row ids the kernel returns back to the input, to recover the
//    payload columns.
//  * __perfetto_interval_kernel, a table in-out function which runs one of
//    the kernels of interval_ops.h on each lane.
//
// Why whole lanes in LISTs: DuckDB feeds an in-out function from several
// threads, so it never sees all of its input, while these operators need
// whole lanes. A lane row is self-contained, so any thread can process it.
// For the same reason inputs are relation names (or query strings), not
// subqueries: a table function only takes a subquery as an in-out function.

#include <algorithm>
#include <cctype>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/selection_vector.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/planner/binder.hpp"

#include "contrib/duckdb-perfetto/src/interval_functions.h"
#include "contrib/duckdb-perfetto/src/interval_ops.h"

namespace duckdb {
namespace perfetto_ext {
namespace {

namespace iv = ::perfetto::duckdb_ext;

constexpr const char* kKernel = "__perfetto_interval_kernel";

enum class Op {
  kMerge,
  kGaps,
  kSubtract,
  kFillGaps,
  kSplit,
  kJoin,
  kExposure,
  kClip,
  kQuantize,
};

struct OpInfo {
  Op op;
  const char* name;      // The kernel's `op :=` value.
  const char* function;  // The user-facing function, for error messages.
  bool binary;           // Whether lanes also carry an operand list.
};

constexpr OpInfo kOps[] = {
    {Op::kMerge, "merge", "interval_merge", false},
    {Op::kGaps, "gaps", "interval_gaps", false},
    {Op::kSubtract, "subtract", "interval_subtract", true},
    {Op::kFillGaps, "fill_gaps", "interval_fill_gaps", true},
    {Op::kSplit, "split", "interval_split", true},
    {Op::kJoin, "join", "interval_join", true},
    {Op::kExposure, "exposure", "interval_exposure", true},
    {Op::kClip, "clip", "interval_clip", false},
    {Op::kQuantize, "quantize", "interval_quantize", false},
};

const OpInfo* FindOp(const std::string& name) {
  for (const OpInfo& info : kOps) {
    if (name == info.name)
      return &info;
  }
  return nullptr;
}

const OpInfo& GetOp(Op op) {
  for (const OpInfo& info : kOps) {
    if (info.op == op)
      return info;
  }
  throw InternalException("unknown interval op");
}

std::optional<Value> Named(const TableFunctionBindInput& input,
                           const char* name) {
  auto it = input.named_parameters.find(name);
  if (it == input.named_parameters.end() || it->second.IsNull())
    return std::nullopt;
  return it->second;
}

iv::Relationship ParseRelationship(const std::string& raw,
                                   const char* function) {
  std::string s;
  for (char c : StringUtil::Lower(raw)) {
    char ch = c == '_' ? ' ' : c;
    if (ch == ' ' && (s.empty() || s.back() == ' '))
      continue;
    s += ch;
  }
  if (!s.empty() && s.back() == ' ')
    s.pop_back();
  if (s == "overlapping" || s == "overlapping bounds")
    return iv::Relationship::kOverlapping;
  if (s == "covering begin")
    return iv::Relationship::kCoveringBegin;
  if (s == "covering end")
    return iv::Relationship::kCoveringEnd;
  if (s == "covering bounds")
    return iv::Relationship::kCoveringBounds;
  if (s == "within" || s == "within bounds")
    return iv::Relationship::kWithin;
  throw BinderException(
      "%s: unknown relationship '%s'; expected one of 'overlapping', "
      "'covering begin', 'covering end', 'covering bounds', 'within'",
      function, raw);
}

// ---------------------------------------------------------------------------
// __perfetto_interval_kernel(<lanes>, op := ...): runs a kernel per lane.
//
// Each input row is one lane: any number of pass-through columns (the PER
// key, copied to the output by merge and gaps), then one LIST(STRUCT(ts, dur,
// id)) column with the lane's rows, then for binary ops a second one with
// the lane's operand rows.
// ---------------------------------------------------------------------------

struct KernelBindData : public TableFunctionData {
  const OpInfo* op = nullptr;
  idx_t passthrough = 0;
  std::optional<int64_t> open_end;
  iv::Relationship relationship = iv::Relationship::kOverlapping;
  bool left = false;
  int64_t window_start = 0;
  int64_t window_end = 0;
  int64_t width = 0;
  int64_t origin = 0;

  unique_ptr<FunctionData> Copy() const override {
    auto res = make_uniq<KernelBindData>();
    res->op = op;
    res->passthrough = passthrough;
    res->open_end = open_end;
    res->relationship = relationship;
    res->left = left;
    res->window_start = window_start;
    res->window_end = window_end;
    res->width = width;
    res->origin = origin;
    return std::move(res);
  }
  bool Equals(const FunctionData& other) const override {
    return this == &other;
  }
};

void CheckLaneType(const LogicalType& type) {
  bool ok = type.id() == LogicalTypeId::LIST &&
            ListType::GetChildType(type).id() == LogicalTypeId::STRUCT;
  if (ok) {
    const auto& fields =
        StructType::GetChildTypes(ListType::GetChildType(type));
    const char* kNames[] = {"ts", "dur", "id"};
    ok = fields.size() == 3;
    for (idx_t i = 0; ok && i < 3; ++i) {
      ok = fields[i].first == kNames[i] &&
           fields[i].second.id() == LogicalTypeId::BIGINT;
    }
  }
  if (!ok) {
    throw BinderException(
        "%s: expected LIST(STRUCT(ts BIGINT, dur BIGINT, id BIGINT)) lanes, "
        "got %s. This function is internal to the interval_* functions",
        kKernel, type.ToString());
  }
}

unique_ptr<FunctionData> KernelBind(ClientContext&,
                                    TableFunctionBindInput& input,
                                    vector<LogicalType>& types,
                                    vector<string>& names) {
  auto data = make_uniq<KernelBindData>();
  auto op = Named(input, "op");
  if (!op)
    throw BinderException("%s: op := is required", kKernel);
  data->op = FindOp(StringValue::Get(*op));
  if (!data->op) {
    throw BinderException("%s: unknown op '%s'", kKernel,
                          StringValue::Get(*op));
  }
  const char* fn = data->op->function;
  if (auto v = Named(input, "open_end"))
    data->open_end = v->GetValue<int64_t>();
  if (auto v = Named(input, "relationship"))
    data->relationship = ParseRelationship(StringValue::Get(*v), fn);
  if (auto v = Named(input, "left"))
    data->left = BooleanValue::Get(*v);
  if (auto v = Named(input, "window_start"))
    data->window_start = v->GetValue<int64_t>();
  if (auto v = Named(input, "window_end"))
    data->window_end = v->GetValue<int64_t>();
  if (auto v = Named(input, "width"))
    data->width = v->GetValue<int64_t>();
  if (auto v = Named(input, "origin"))
    data->origin = v->GetValue<int64_t>();

  const auto& in_types = input.input_table_types;
  idx_t lists = data->op->binary ? 2 : 1;
  if (in_types.size() < lists)
    throw BinderException("%s: expected %d lane columns", kKernel, lists);
  data->passthrough = in_types.size() - lists;
  for (idx_t i = data->passthrough; i < in_types.size(); ++i)
    CheckLaneType(in_types[i]);

  auto add = [&](const std::string& name) {
    names.push_back(name);
    types.push_back(LogicalType::BIGINT);
  };
  for (idx_t i = 0; i < data->passthrough; ++i) {
    names.push_back(input.input_table_names[i]);
    types.push_back(in_types[i]);
  }
  switch (data->op->op) {
    case Op::kMerge:
      add("ts");
      add("dur");
      add("row_count");
      break;
    case Op::kGaps:
      add("ts");
      add("dur");
      break;
    case Op::kSubtract:
    case Op::kFillGaps:
    case Op::kClip:
      add("id");
      add("ts");
      add("dur");
      break;
    case Op::kSplit:
      add("id");
      add("operand_id");
      add("ts");
      add("dur");
      break;
    case Op::kJoin:
      add("id");
      add("operand_id");
      break;
    case Op::kExposure:
      add("id");
      add("overlap_dur");
      add("exposed_dur");
      break;
    case Op::kQuantize:
      add("id");
      add("ts");
      add("dur");
      add("bucket");
      break;
  }
  return std::move(data);
}

// One output row: up to four BIGINT values, and the lane it came from.
struct OutRow {
  int64_t v[4];
  uint32_t null_mask;  // Bit i set: v[i] is NULL.
  uint32_t lane;       // The input row (lane) within the current chunk.
};

struct RawRow {
  int64_t ts;
  int64_t dur;
  int64_t id;
  bool null;  // ts or dur is NULL: the row covers no time.
};

struct KernelLocalState : public LocalTableFunctionState {
  std::vector<OutRow> rows;  // Output for the current input chunk.
  size_t pos = 0;            // Rows of |rows| already returned.
  bool computed = false;     // Whether |rows| is for the current chunk.

  // Scratch, reused across lanes.
  std::vector<RawRow> raw_a, raw_b;
  std::vector<iv::Interval> a, b;
  std::vector<int64_t> a_null_ids;
};

unique_ptr<LocalTableFunctionState> KernelInitLocal(ExecutionContext&,
                                                    TableFunctionInitInput&,
                                                    GlobalTableFunctionState*) {
  return make_uniq<KernelLocalState>();
}

// Reads the lanes of one LIST(STRUCT(ts, dur, id)) column.
class LaneColumn {
 public:
  LaneColumn(Vector& list, idx_t count) {
    list.ToUnifiedFormat(count, list_);
    entries_ = UnifiedVectorFormat::GetData<list_entry_t>(list_);
    Vector& child = ListVector::GetEntry(list);
    idx_t size = ListVector::GetListSize(list);
    if (child.GetVectorType() != VectorType::FLAT_VECTOR)
      child.Flatten(size);
    child.ToUnifiedFormat(size, struct_);
    auto& fields = StructVector::GetEntries(child);
    fields[0]->ToUnifiedFormat(size, ts_);
    fields[1]->ToUnifiedFormat(size, dur_);
    fields[2]->ToUnifiedFormat(size, id_);
  }

  LaneColumn(const LaneColumn&) = delete;
  LaneColumn& operator=(const LaneColumn&) = delete;

  void Read(idx_t lane, std::vector<RawRow>* out) const {
    out->clear();
    idx_t li = list_.sel->get_index(lane);
    if (!list_.validity.RowIsValid(li))
      return;
    const list_entry_t& entry = entries_[li];
    auto* ts = UnifiedVectorFormat::GetData<int64_t>(ts_);
    auto* dur = UnifiedVectorFormat::GetData<int64_t>(dur_);
    auto* id = UnifiedVectorFormat::GetData<int64_t>(id_);
    for (idx_t k = entry.offset; k < entry.offset + entry.length; ++k) {
      if (!struct_.validity.RowIsValid(struct_.sel->get_index(k)))
        continue;
      idx_t ti = ts_.sel->get_index(k);
      idx_t di = dur_.sel->get_index(k);
      idx_t ii = id_.sel->get_index(k);
      RawRow r{0, 0, 0, true};
      r.id = id_.validity.RowIsValid(ii) ? id[ii] : 0;
      if (ts_.validity.RowIsValid(ti) && dur_.validity.RowIsValid(di)) {
        r.ts = ts[ti];
        r.dur = dur[di];
        r.null = false;
      }
      out->push_back(r);
    }
  }

 private:
  UnifiedVectorFormat list_;
  const list_entry_t* entries_ = nullptr;
  UnifiedVectorFormat struct_;
  UnifiedVectorFormat ts_;
  UnifiedVectorFormat dur_;
  UnifiedVectorFormat id_;
};

// Validates and normalizes rows (see NormalizeDur). Rows covering no time
// (NULL ts or dur) go to |null_ids|, if given.
void Normalize(const std::vector<RawRow>& raw,
               std::optional<int64_t> open_end,
               const char* fn,
               std::vector<iv::Interval>* out,
               std::vector<int64_t>* null_ids) {
  out->clear();
  if (null_ids)
    null_ids->clear();
  for (const RawRow& r : raw) {
    if (r.null) {
      if (null_ids)
        null_ids->push_back(r.id);
      continue;
    }
    auto dur = iv::NormalizeDur(r.ts, r.dur, open_end);
    if (!dur.ok())
      throw InvalidInputException("%s: %s", fn, dur.status().message());
    out->push_back({r.ts, *dur, r.id});
  }
}

void Push(std::vector<OutRow>* rows,
          idx_t lane,
          std::initializer_list<std::optional<int64_t>> values) {
  OutRow row{{0, 0, 0, 0}, 0, static_cast<uint32_t>(lane)};
  uint32_t i = 0;
  for (const auto& v : values) {
    if (v) {
      row.v[i] = *v;
    } else {
      row.null_mask |= 1u << i;
    }
    ++i;
  }
  rows->push_back(row);
}

std::optional<int64_t> OperandId(int64_t id) {
  return id == iv::kNoMatch ? std::nullopt : std::optional<int64_t>(id);
}

void ComputeLane(const KernelBindData& bind, idx_t lane, KernelLocalState* st) {
  const char* fn = bind.op->function;
  std::vector<OutRow>* out = &st->rows;
  bool keep_nulls =
      bind.op->op == Op::kExposure || (bind.op->op == Op::kJoin && bind.left);
  if (bind.op->op == Op::kFillGaps) {
    // The background first: an open input row covers to its end.
    Normalize(st->raw_b, bind.open_end, fn, &st->b, nullptr);
    if (st->b.empty())
      return;
    int64_t bg_end = st->b[0].end();
    for (const iv::Interval& i : st->b)
      bg_end = std::max(bg_end, i.end());
    Normalize(st->raw_a, bind.open_end ? bind.open_end : bg_end, fn, &st->a,
              nullptr);
    for (const iv::Piece& p :
         iv::SubtractIntervals(st->b, st->a, /*keep_points=*/false)) {
      Push(out, lane, {p.id, p.ts, p.dur});
    }
    return;
  }
  Normalize(st->raw_a, bind.open_end, fn, &st->a,
            keep_nulls ? &st->a_null_ids : nullptr);
  if (bind.op->binary)
    Normalize(st->raw_b, bind.open_end, fn, &st->b, nullptr);
  switch (bind.op->op) {
    case Op::kMerge:
      for (const iv::Island& i : iv::MergeIntervals(st->a))
        Push(out, lane, {i.ts, i.dur, i.row_count});
      break;
    case Op::kGaps:
      for (const iv::Span& s : iv::GapsBetween(st->a))
        Push(out, lane, {s.ts, s.dur});
      break;
    case Op::kSubtract:
      for (const iv::Piece& p :
           iv::SubtractIntervals(st->a, st->b, /*keep_points=*/true)) {
        Push(out, lane, {p.id, p.ts, p.dur});
      }
      break;
    case Op::kSplit:
      for (const iv::SplitPiece& p : iv::SplitIntervals(st->a, st->b))
        Push(out, lane, {p.id, OperandId(p.operand_id), p.ts, p.dur});
      break;
    case Op::kJoin:
      for (const iv::Match& m :
           iv::JoinIntervals(st->a, st->b, bind.relationship, bind.left)) {
        Push(out, lane, {m.id, OperandId(m.operand_id)});
      }
      if (bind.left) {
        for (int64_t id : st->a_null_ids)
          Push(out, lane, {id, std::nullopt});
      }
      break;
    case Op::kExposure: {
      std::vector<iv::Exposure> res = iv::OverlapWithCoverage(st->a, st->b);
      for (size_t i = 0; i < res.size(); ++i) {
        Push(
            out, lane,
            {res[i].id, res[i].overlap_dur, st->a[i].dur - res[i].overlap_dur});
      }
      for (int64_t id : st->a_null_ids)
        Push(out, lane, {id, std::nullopt, std::nullopt});
      break;
    }
    case Op::kClip:
      for (const iv::Piece& p :
           iv::ClipIntervals(st->a, bind.window_start, bind.window_end)) {
        Push(out, lane, {p.id, p.ts, p.dur});
      }
      break;
    case Op::kQuantize: {
      auto pieces = iv::QuantizeIntervals(st->a, bind.width, bind.origin);
      if (!pieces.ok())
        throw InvalidInputException("%s: %s", fn, pieces.status().message());
      for (const iv::BucketPiece& p : *pieces)
        Push(out, lane, {p.id, p.ts, p.dur, p.bucket});
      break;
    }
    case Op::kFillGaps:
      break;  // Handled above.
  }
}

OperatorResultType KernelInOut(ExecutionContext&,
                               TableFunctionInput& data,
                               DataChunk& input,
                               DataChunk& output) {
  auto& bind = data.bind_data->Cast<KernelBindData>();
  auto& st = data.local_state->Cast<KernelLocalState>();
  if (!st.computed) {
    st.rows.clear();
    st.pos = 0;
    idx_t count = input.size();
    LaneColumn a(input.data[bind.passthrough], count);
    std::unique_ptr<LaneColumn> b;
    if (bind.op->binary)
      b = std::make_unique<LaneColumn>(input.data[bind.passthrough + 1], count);
    for (idx_t lane = 0; lane < count; ++lane) {
      a.Read(lane, &st.raw_a);
      if (b)
        b->Read(lane, &st.raw_b);
      ComputeLane(bind, lane, &st);
    }
    st.computed = true;
  }

  idx_t n = std::min<idx_t>(STANDARD_VECTOR_SIZE, st.rows.size() - st.pos);
  const OutRow* rows = st.rows.data() + st.pos;
  if (bind.passthrough > 0 && n > 0) {
    SelectionVector sel(STANDARD_VECTOR_SIZE);
    for (idx_t i = 0; i < n; ++i)
      sel.set_index(i, rows[i].lane);
    for (idx_t c = 0; c < bind.passthrough; ++c)
      VectorOperations::Copy(input.data[c], output.data[c], sel, n, 0, 0);
  }
  for (idx_t c = bind.passthrough; c < output.ColumnCount(); ++c) {
    uint32_t bit = 1u << (c - bind.passthrough);
    Vector& vec = output.data[c];
    auto* values = FlatVector::GetData<int64_t>(vec);
    auto& validity = FlatVector::Validity(vec);
    for (idx_t i = 0; i < n; ++i) {
      if (rows[i].null_mask & bit) {
        validity.SetInvalid(i);
      } else {
        validity.SetValid(i);
        values[i] = rows[i].v[c - bind.passthrough];
      }
    }
  }
  output.SetCardinality(n);
  st.pos += n;
  if (st.pos < st.rows.size())
    return OperatorResultType::HAVE_MORE_OUTPUT;
  st.computed = false;
  return OperatorResultType::NEED_MORE_INPUT;
}

// ---------------------------------------------------------------------------
// The interval_* functions: bind_replace into a query over the kernel.
// ---------------------------------------------------------------------------

std::string QuoteIdent(const std::string& s) {
  return "\"" + StringUtil::Replace(s, "\"", "\"\"") + "\"";
}

std::string QuoteLiteral(const std::string& s) {
  return "'" + StringUtil::Replace(s, "'", "''") + "'";
}

bool IsIdentifier(const std::string& s) {
  if (s.empty() || !(isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_'))
    return false;
  for (char c : s) {
    if (!(isalnum(static_cast<unsigned char>(c)) || c == '_'))
      return false;
  }
  return true;
}

// An input relation, as SQL text to put after FROM.
struct Relation {
  std::string sql;
  std::string alias;  // Default name for its columns packed as a struct.
};

// Accepts a table, view, CTE or DataFrame name (optionally qualified, e.g.
// t.slice), a query string (SELECT ..., FROM ..., WITH ..., PERFETTO(t)
// (...)), or anything else DuckDB can scan by name, like a parquet path.
Relation ParseRelation(const Value& v, const char* fn, const char* what) {
  if (v.IsNull())
    throw BinderException("%s: the %s must not be NULL", fn, what);
  std::string s = v.ToString();
  StringUtil::Trim(s);
  if (s.empty())
    throw BinderException("%s: the %s must not be empty", fn, what);
  size_t word_end = 0;
  while (word_end < s.size() &&
         (isalpha(static_cast<unsigned char>(s[word_end])) ||
          s[word_end] == '_')) {
    ++word_end;
  }
  std::string word = StringUtil::Lower(s.substr(0, word_end));
  bool query =
      s[0] == '(' || (word_end < s.size() && !IsIdentifier(s) &&
                      (word == "select" || word == "from" || word == "with" ||
                       word == "values" || word == "perfetto"));
  if (query)
    return {"(" + s + ")", "operand"};
  std::vector<std::string> parts = StringUtil::Split(s, '.');
  bool name = !parts.empty() && parts.size() <= 3;
  for (const std::string& p : parts)
    name = name && IsIdentifier(p);
  if (!name)
    return {QuoteLiteral(s), "operand"};
  std::string sql;
  for (const std::string& p : parts)
    sql += (sql.empty() ? "" : ".") + QuoteIdent(p);
  return {sql, parts.back()};
}

std::vector<std::string> ParsePer(const TableFunctionBindInput& input,
                                  const char* fn) {
  std::vector<std::string> per;
  auto v = Named(input, "per");
  if (!v)
    return per;
  std::vector<Value> items;
  if (v->type().id() == LogicalTypeId::LIST) {
    items = ListValue::GetChildren(*v);
  } else {
    items.push_back(*v);
  }
  for (const Value& item : items) {
    if (item.IsNull())
      throw BinderException("%s: PER columns must not be NULL", fn);
    std::string col = item.ToString();
    if (col.empty())
      throw BinderException("%s: PER column names must not be empty", fn);
    for (const std::string& seen : per) {
      if (StringUtil::CIEquals(seen, col))
        throw BinderException("%s: PER column \"%s\" is repeated", fn, col);
    }
    per.push_back(col);
  }
  return per;
}

struct Schema {
  vector<string> names;
  vector<LogicalType> types;

  std::optional<idx_t> Find(const std::string& name) const {
    for (idx_t i = 0; i < names.size(); ++i) {
      if (StringUtil::CIEquals(names[i], name))
        return i;
    }
    return std::nullopt;
  }
};

// Binds `SELECT * FROM <rel>` with a child of the caller's binder (so CTEs of
// the calling query are visible) to learn the relation's columns.
Schema Describe(ClientContext& context,
                TableFunctionBindInput& input,
                const Relation& rel) {
  Parser parser(context.GetParserOptions());
  parser.ParseQuery("SELECT * FROM " + rel.sql);
  if (parser.statements.size() != 1 ||
      parser.statements[0]->type != StatementType::SELECT_STATEMENT) {
    throw BinderException("not a relation: %s", rel.sql);
  }
  auto& select = parser.statements[0]->Cast<SelectStatement>();
  auto binder = Binder::CreateBinder(context, input.binder);
  BoundStatement bound = binder->Bind(*select.node);
  return {bound.names, bound.types};
}

void CheckIntervals(const Schema& s,
                    const char* fn,
                    const char* what,
                    const std::vector<std::string>& per) {
  for (const char* col : {"ts", "dur"}) {
    auto i = s.Find(col);
    if (!i) {
      throw BinderException(
          "%s: the %s has no \"%s\" column; interval relations need integer "
          "ts and dur columns, in ns",
          fn, what, col);
    }
    if (!s.types[*i].IsIntegral()) {
      throw BinderException("%s: column \"%s\" of the %s is %s, not an integer",
                            fn, col, what, s.types[*i].ToString());
    }
  }
  for (const std::string& col : per) {
    if (!s.Find(col))
      throw BinderException("%s: the %s has no PER column \"%s\"", fn, what,
                            col);
  }
}

// `prefix"a", prefix"b", ` (with a trailing separator), or "".
std::string Columns(const std::vector<std::string>& cols,
                    const std::string& prefix) {
  std::string res;
  for (const std::string& c : cols)
    res += prefix + QuoteIdent(c) + ", ";
  return res;
}

std::string GroupBy(const std::vector<std::string>& per) {
  std::string cols = Columns(per, "");
  if (cols.empty())
    return "";
  return " GROUP BY " + cols.substr(0, cols.size() - 2);
}

std::string LaneStruct(const std::string& prefix, const std::string& id) {
  return "{'ts': CAST(" + prefix + "\"ts\" AS BIGINT), 'dur': CAST(" + prefix +
         "\"dur\" AS BIGINT), 'id': CAST(" + id + " AS BIGINT)}";
}

// One row per lane of |from|: the PER columns if |passthrough|, then the
// lane's rows as a list.
std::string UnaryLanes(const std::string& from,
                       const std::vector<std::string>& per,
                       const std::string& id,
                       bool passthrough) {
  return "SELECT " + (passthrough ? Columns(per, "") : std::string()) +
         "list(" + LaneStruct("", id) + ") AS __iv_a FROM " + from +
         GroupBy(per);
}

// One branch of BinaryLanes: |from|'s rows with the PER key, ts, dur, an id
// and which side they are on.
std::string Branch(const std::vector<std::string>& per,
                   const std::string& prefix,
                   const std::string& from,
                   const std::string& id,
                   int side) {
  return "SELECT " + Columns(per, prefix) + "CAST(" + prefix +
         "\"ts\" AS BIGINT) AS ts, CAST(" + prefix +
         "\"dur\" AS BIGINT) AS dur, CAST(" + id + " AS BIGINT) AS __iv_id, " +
         std::to_string(side) + " AS __iv_side FROM " + from;
}

// One row per lane: the rows of |a_branch|, then those of |b_branch|, as two
// lists. A lane missing from one side has NULL (empty) there.
std::string BinaryLanes(const std::string& a_branch,
                        const std::string& b_branch,
                        const std::vector<std::string>& per) {
  const char* kItem = "{'ts': ts, 'dur': dur, 'id': __iv_id}";
  return std::string("SELECT list(") + kItem +
         ") FILTER (WHERE __iv_side = 0) AS __iv_a, list(" + kItem +
         ") FILTER (WHERE __iv_side = 1) AS __iv_b FROM (" + a_branch +
         " UNION ALL " + b_branch + ")" + GroupBy(per);
}

std::string KernelCall(const std::string& lanes,
                       const OpInfo& op,
                       const std::string& options) {
  return std::string("SELECT * FROM ") + kKernel + "((" + lanes + "), op := '" +
         op.name + "'" + options + ")";
}

std::string OpenEndOption(const TableFunctionBindInput& input) {
  auto v = Named(input, "open_end");
  return v ? ", open_end := " + std::to_string(v->GetValue<int64_t>()) : "";
}

// The input with a row id, materialized so that ids are stable.
std::string NumberedCte(const char* name, const std::string& from) {
  return std::string(name) +
         " AS MATERIALIZED (SELECT row_number() OVER () AS __iv_id, * FROM " +
         from + ")";
}

unique_ptr<TableRef> Finish(ClientContext& context,
                            const TableFunctionBindInput& input,
                            const std::string& sql) {
  std::string text = sql;
  if (auto v = Named(input, "debug_sql")) {
    if (BooleanValue::Get(*v))
      text = "SELECT " + QuoteLiteral(sql) + " AS sql";
  }
  Parser parser(context.GetParserOptions());
  parser.ParseQuery(text);
  if (parser.statements.size() != 1 ||
      parser.statements[0]->type != StatementType::SELECT_STATEMENT) {
    throw InternalException("interval functions generated a non-query: %s",
                            text);
  }
  return make_uniq<SubqueryRef>(unique_ptr_cast<SQLStatement, SelectStatement>(
      std::move(parser.statements[0])));
}

unique_ptr<TableRef> MergeOrGaps(ClientContext& context,
                                 TableFunctionBindInput& input,
                                 Op op) {
  const OpInfo& info = GetOp(op);
  Relation rel = ParseRelation(input.inputs[0], info.function, "input");
  std::vector<std::string> per = ParsePer(input, info.function);
  CheckIntervals(Describe(context, input, rel), info.function, "input", per);
  return Finish(context, input,
                KernelCall(UnaryLanes(rel.sql, per, "0", true), info,
                           OpenEndOption(input)));
}

unique_ptr<TableRef> MergeReplace(ClientContext& context,
                                  TableFunctionBindInput& input) {
  return MergeOrGaps(context, input, Op::kMerge);
}

unique_ptr<TableRef> GapsReplace(ClientContext& context,
                                 TableFunctionBindInput& input) {
  return MergeOrGaps(context, input, Op::kGaps);
}

// The CTEs shared by the binary row-preserving functions: __iv_a (the
// numbered input), __iv_b (the numbered operand, packed into a struct
// __iv_row, if |operand_payload|) and __iv_k (the kernel's output).
std::string BinaryCtes(ClientContext& context,
                       TableFunctionBindInput& input,
                       const OpInfo& info,
                       bool operand_payload,
                       const std::string& extra_options) {
  Relation rel = ParseRelation(input.inputs[0], info.function, "input");
  Relation operand = ParseRelation(input.inputs[1], info.function, "operand");
  std::vector<std::string> per = ParsePer(input, info.function);
  CheckIntervals(Describe(context, input, rel), info.function, "input", per);
  CheckIntervals(Describe(context, input, operand), info.function, "operand",
                 per);
  std::string ctes = "WITH " + NumberedCte("__iv_a", rel.sql) + ", ";
  std::string b_branch;
  if (operand_payload) {
    ctes +=
        "__iv_b AS MATERIALIZED (SELECT row_number() OVER () AS __iv_id, "
        "__iv_row FROM " +
        operand.sql + " AS __iv_row), ";
    b_branch = Branch(per, "__iv_row.", "__iv_b", "__iv_id", 1);
  } else {
    b_branch = Branch(per, "", operand.sql, "0", 1);
  }
  std::string lanes =
      BinaryLanes(Branch(per, "", "__iv_a", "__iv_id", 0), b_branch, per);
  ctes += "__iv_k AS (" +
          KernelCall(lanes, info, OpenEndOption(input) + extra_options) + ") ";
  return ctes;
}

std::string OperandAlias(const TableFunctionBindInput& input,
                         const OpInfo& info) {
  if (auto v = Named(input, "alias"))
    return v->ToString();
  return ParseRelation(input.inputs[1], info.function, "operand").alias;
}

constexpr const char* kJoinA =
    " FROM __iv_k JOIN __iv_a ON __iv_a.__iv_id = __iv_k.id";
constexpr const char* kJoinB =
    " LEFT JOIN __iv_b ON __iv_b.__iv_id = __iv_k.operand_id";
constexpr const char* kReplaceBounds =
    "__iv_a.* EXCLUDE (__iv_id) REPLACE (__iv_k.ts AS ts, __iv_k.dur AS dur)";

unique_ptr<TableRef> SubtractReplace(ClientContext& context,
                                     TableFunctionBindInput& input) {
  const OpInfo& info = GetOp(Op::kSubtract);
  std::string sql = BinaryCtes(context, input, info, false, "") + "SELECT " +
                    kReplaceBounds + kJoinA;
  return Finish(context, input, sql);
}

unique_ptr<TableRef> SplitReplace(ClientContext& context,
                                  TableFunctionBindInput& input) {
  const OpInfo& info = GetOp(Op::kSplit);
  std::string sql = BinaryCtes(context, input, info, true, "") + "SELECT " +
                    kReplaceBounds + ", __iv_b.__iv_row AS " +
                    QuoteIdent(OperandAlias(input, info)) + kJoinA + kJoinB;
  return Finish(context, input, sql);
}

unique_ptr<TableRef> JoinReplace(ClientContext& context,
                                 TableFunctionBindInput& input) {
  const OpInfo& info = GetOp(Op::kJoin);
  std::string options;
  if (auto v = Named(input, "relationship")) {
    ParseRelationship(v->ToString(), info.function);  // Validate early.
    options += ", relationship := " + QuoteLiteral(v->ToString());
  }
  if (auto v = Named(input, "left"))
    options += BooleanValue::Get(*v) ? ", left := true" : ", left := false";
  std::string sql = BinaryCtes(context, input, info, true, options) +
                    "SELECT __iv_a.* EXCLUDE (__iv_id), __iv_b.__iv_row AS " +
                    QuoteIdent(OperandAlias(input, info)) + kJoinA + kJoinB;
  return Finish(context, input, sql);
}

unique_ptr<TableRef> ExposureReplace(ClientContext& context,
                                     TableFunctionBindInput& input) {
  const OpInfo& info = GetOp(Op::kExposure);
  std::string sql = BinaryCtes(context, input, info, false, "") +
                    "SELECT __iv_a.* EXCLUDE (__iv_id), __iv_k.overlap_dur, "
                    "__iv_k.exposed_dur" +
                    kJoinA;
  return Finish(context, input, sql);
}

unique_ptr<TableRef> FillGapsReplace(ClientContext& context,
                                     TableFunctionBindInput& input) {
  const OpInfo& info = GetOp(Op::kFillGaps);
  const char* fn = info.function;
  Relation rel = ParseRelation(input.inputs[0], fn, "input");
  Relation bg = ParseRelation(input.inputs[1], fn, "background");
  std::vector<std::string> per = ParsePer(input, fn);
  CheckIntervals(Describe(context, input, rel), fn, "input", per);
  Schema bg_schema = Describe(context, input, bg);
  size_t present = 0;
  for (const std::string& col : per)
    present += bg_schema.Find(col) ? 1 : 0;
  std::string bg_from = bg.sql;
  if (present == per.size()) {
    // The lanes are the background's, so lanes without input are filled.
    CheckIntervals(bg_schema, fn, "background", per);
  } else if (present == 0) {
    // Every background row applies to every lane of the input.
    CheckIntervals(bg_schema, fn, "background", {});
    std::string cols = Columns(per, "");
    bg_from = "(SELECT * FROM (SELECT DISTINCT " +
              cols.substr(0, cols.size() - 2) + " FROM " + rel.sql +
              ") AS __iv_lanes, " + bg.sql + " AS __iv_bg)";
  } else {
    throw BinderException(
        "%s: the background must have all of the PER columns (its own lanes) "
        "or none of them (one background for every lane)",
        fn);
  }
  std::string lanes = BinaryLanes(Branch(per, "", rel.sql, "0", 0),
                                  Branch(per, "", "__iv_b", "__iv_id", 1), per);
  std::string sql =
      "WITH " + NumberedCte("__iv_b", bg_from) + ", __iv_k AS (" +
      KernelCall(lanes, info, OpenEndOption(input)) + ") SELECT * FROM " +
      rel.sql +
      " UNION ALL BY NAME SELECT __iv_b.* EXCLUDE (__iv_id) REPLACE "
      "(__iv_k.ts AS ts, __iv_k.dur AS dur) FROM __iv_k JOIN __iv_b ON "
      "__iv_b.__iv_id = __iv_k.id";
  return Finish(context, input, sql);
}

// The CTEs of the unary row-preserving functions (clip, quantize).
std::string UnaryCtes(ClientContext& context,
                      TableFunctionBindInput& input,
                      const OpInfo& info,
                      const std::string& options) {
  Relation rel = ParseRelation(input.inputs[0], info.function, "input");
  CheckIntervals(Describe(context, input, rel), info.function, "input", {});
  return "WITH " + NumberedCte("__iv_a", rel.sql) + ", __iv_k AS (" +
         KernelCall(UnaryLanes("__iv_a", {}, "__iv_id", false), info,
                    OpenEndOption(input) + options) +
         ") ";
}

int64_t RequiredInt(const Value& v, const char* fn, const char* what) {
  if (v.IsNull())
    throw BinderException("%s: %s must not be NULL", fn, what);
  return v.GetValue<int64_t>();
}

unique_ptr<TableRef> ClipReplace(ClientContext& context,
                                 TableFunctionBindInput& input) {
  const OpInfo& info = GetOp(Op::kClip);
  int64_t start = RequiredInt(input.inputs[1], info.function, "window_start");
  int64_t end = RequiredInt(input.inputs[2], info.function, "window_end");
  if (start > end) {
    throw BinderException("%s: window_start (%lld) is after window_end (%lld)",
                          info.function, static_cast<long long>(start),
                          static_cast<long long>(end));
  }
  std::string options = ", window_start := " + std::to_string(start) +
                        ", window_end := " + std::to_string(end);
  std::string sql = UnaryCtes(context, input, info, options) + "SELECT " +
                    kReplaceBounds + kJoinA;
  return Finish(context, input, sql);
}

unique_ptr<TableRef> QuantizeReplace(ClientContext& context,
                                     TableFunctionBindInput& input) {
  const OpInfo& info = GetOp(Op::kQuantize);
  int64_t width = RequiredInt(input.inputs[1], info.function, "width");
  if (width <= 0)
    throw BinderException("%s: width must be positive", info.function);
  int64_t origin = 0;
  if (auto v = Named(input, "origin"))
    origin = v->GetValue<int64_t>();
  std::string options = ", width := " + std::to_string(width) +
                        ", origin := " + std::to_string(origin);
  std::string sql = UnaryCtes(context, input, info, options) + "SELECT " +
                    kReplaceBounds + ", __iv_k.bucket" + kJoinA;
  return Finish(context, input, sql);
}

// No kernel needed: new bounds are plain expressions over each row.
unique_ptr<TableRef> SetBoundsReplace(ClientContext& context,
                                      TableFunctionBindInput& input) {
  const char* fn = "interval_set_bounds";
  Relation rel = ParseRelation(input.inputs[0], fn, "input");
  auto begin = Named(input, "begin_at");
  auto end = Named(input, "end_at");
  auto dur = Named(input, "dur");
  if (!begin || (!end == !dur)) {
    throw BinderException(
        "%s: pass begin_at := <expr> and exactly one of end_at := <expr> or "
        "dur := <expr>",
        fn);
  }
  std::string b = "(" + begin->ToString() + ")";
  std::string ts_expr = "CAST(" + b + " AS BIGINT)";
  std::string dur_expr;
  if (end) {
    std::string e = "(" + end->ToString() + ")";
    dur_expr = "CAST(CASE WHEN " + e + " < " + b +
               " THEN error('interval_set_bounds: end_at is before begin_at') "
               "ELSE " +
               e + " - " + b + " END AS BIGINT)";
  } else {
    std::string d = "(" + dur->ToString() + ")";
    dur_expr = "CAST(CASE WHEN " + d +
               " < -1 THEN error('interval_set_bounds: negative dur') ELSE " +
               d + " END AS BIGINT)";
  }
  Schema schema = Describe(context, input, rel);
  std::string replace;
  std::string extra;
  for (const auto& [col, expr] :
       {std::make_pair("ts", ts_expr), std::make_pair("dur", dur_expr)}) {
    if (schema.Find(col)) {
      replace += (replace.empty() ? "" : ", ") + expr + " AS " + col;
    } else {
      extra += ", " + expr + " AS " + col;
    }
  }
  std::string sql = "SELECT *" +
                    (replace.empty() ? "" : " REPLACE (" + replace + ")") +
                    extra + " FROM " + rel.sql;
  return Finish(context, input, sql);
}

}  // namespace

void RegisterIntervalFunctions(ExtensionLoader& loader) {
  TableFunction kernel(kKernel, {LogicalType::TABLE}, nullptr, KernelBind,
                       nullptr, KernelInitLocal);
  kernel.in_out_function = KernelInOut;
  kernel.order_preservation_type = OrderPreservationType::NO_ORDER;
  kernel.named_parameters["op"] = LogicalType::VARCHAR;
  kernel.named_parameters["open_end"] = LogicalType::BIGINT;
  kernel.named_parameters["relationship"] = LogicalType::VARCHAR;
  kernel.named_parameters["left"] = LogicalType::BOOLEAN;
  kernel.named_parameters["window_start"] = LogicalType::BIGINT;
  kernel.named_parameters["window_end"] = LogicalType::BIGINT;
  kernel.named_parameters["width"] = LogicalType::BIGINT;
  kernel.named_parameters["origin"] = LogicalType::BIGINT;
  loader.RegisterFunction(kernel);

  struct Def {
    const char* name;
    vector<LogicalType> args;
    table_function_bind_replace_t replace;
    bool per;
  };
  const LogicalType kRel = LogicalType::VARCHAR;
  const Def defs[] = {
      {"interval_merge", {kRel}, MergeReplace, true},
      {"interval_gaps", {kRel}, GapsReplace, true},
      {"interval_subtract", {kRel, kRel}, SubtractReplace, true},
      {"interval_fill_gaps", {kRel, kRel}, FillGapsReplace, true},
      {"interval_split", {kRel, kRel}, SplitReplace, true},
      {"interval_join", {kRel, kRel}, JoinReplace, true},
      {"interval_exposure", {kRel, kRel}, ExposureReplace, true},
      {"interval_clip",
       {kRel, LogicalType::BIGINT, LogicalType::BIGINT},
       ClipReplace,
       false},
      {"interval_quantize",
       {kRel, LogicalType::BIGINT},
       QuantizeReplace,
       false},
      {"interval_set_bounds", {kRel}, SetBoundsReplace, false},
  };
  for (const Def& def : defs) {
    TableFunction f(def.name, def.args, nullptr);
    f.bind_replace = def.replace;
    f.named_parameters["debug_sql"] = LogicalType::BOOLEAN;
    if (def.per)
      f.named_parameters["per"] = LogicalType::ANY;
    if (def.replace != SetBoundsReplace)
      f.named_parameters["open_end"] = LogicalType::BIGINT;
    if (def.replace == SplitReplace || def.replace == JoinReplace)
      f.named_parameters["alias"] = LogicalType::VARCHAR;
    if (def.replace == JoinReplace) {
      f.named_parameters["relationship"] = LogicalType::VARCHAR;
      f.named_parameters["left"] = LogicalType::BOOLEAN;
    }
    if (def.replace == QuantizeReplace)
      f.named_parameters["origin"] = LogicalType::BIGINT;
    if (def.replace == SetBoundsReplace) {
      f.named_parameters["begin_at"] = LogicalType::VARCHAR;
      f.named_parameters["end_at"] = LogicalType::VARCHAR;
      f.named_parameters["dur"] = LogicalType::VARCHAR;
    }
    loader.RegisterFunction(f);
  }
}

}  // namespace perfetto_ext
}  // namespace duckdb
