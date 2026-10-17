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

#include "src/trace_processor/importers/json/json_compact_args.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "perfetto/ext/base/string_utils.h"
#include "src/trace_processor/importers/common/args_tracker.h"
#include "src/trace_processor/importers/common/global_args_tracker.h"
#include "src/trace_processor/importers/json/json_trace_tokenizer.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/slice_tables_py.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/types/variadic.h"
#include "src/trace_processor/util/json_args.h"
#include "src/trace_processor/util/json_parser.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor {
namespace {

enum class Mode { kJson, kMinifiedJson, kCompact };

// Adds each object in |args| to the args of one slice, with
// json::AddJsonValueToArgs() on the JSON or on the minified JSON, or by
// encoding and replaying it. Returns the args table followed by the strings of
// the string pool, in id order: the latter checks that strings are interned in
// the same order.
std::string AddArgs(const std::vector<std::string>& args, Mode mode) {
  TraceProcessorContext context;
  context.storage = std::make_unique<TraceStorage>();
  context.global_args_tracker =
      std::make_unique<GlobalArgsTracker>(context.storage.get());
  TraceStorage* storage = context.storage.get();
  SliceId slice = storage->mutable_slice_table()->Insert({}).id;
  {
    ArgsTracker::BoundInserter inserter =
        ArgsTracker(&context).AddArgsTo(slice);
    JsonCompactArgs compact_args(storage);
    json::Iterator it;
    std::vector<uint8_t> encoded;
    for (size_t i = 0; i < args.size(); ++i) {
      const std::string& a = args[i];
      if (mode == Mode::kJson || mode == Mode::kMinifiedJson) {
        std::string json = a;
        if (mode == Mode::kMinifiedJson) {
          EXPECT_TRUE(MinifyJson(a, &json));
        }
        json::AddJsonValueToArgs(it, json.data(), json.data() + json.size(),
                                 "args", "args", storage, &inserter);
        continue;
      }
      // Only the first object goes to an inserter without array indices.
      EXPECT_TRUE(compact_args.Encode(a, /*allow_arrays=*/i == 0, &encoded));
      compact_args.AddArgs(
          std::string_view(reinterpret_cast<const char*>(encoded.data()),
                           encoded.size()),
          &inserter);
    }
  }
  std::string out;
  const auto& table = storage->arg_table();
  for (uint32_t i = 0; i < table.row_count(); ++i) {
    auto row = table[i];
    out += std::to_string(row.arg_set_id()) + " ";
    out += storage->GetString(row.flat_key()).ToStdString() + " ";
    out += storage->GetString(row.key()).ToStdString() + " ";
    out += storage->GetString(row.value_type()).ToStdString() + " ";
    if (auto v = row.int_value(); v) {
      out += std::to_string(*v);
    }
    if (auto v = row.real_value(); v) {
      out += base::StackString<64>("%.17g", *v).ToStdString();
    }
    if (auto v = row.string_value(); v) {
      out += "'" + storage->GetString(*v).ToStdString() + "'";
    }
    out += "\n";
  }
  out += "strings:";
  for (auto it = storage->string_pool().CreateSmallStringIterator(); it; ++it) {
    out += " '" + it.StringView().ToStdString() + "'";
  }
  return out;
}

void ExpectSameArgs(const std::vector<std::string>& args) {
  std::string json = AddArgs(args, Mode::kJson);
  EXPECT_EQ(json, AddArgs(args, Mode::kMinifiedJson));
  EXPECT_EQ(json, AddArgs(args, Mode::kCompact));
  EXPECT_NE(json.find('\n'), std::string::npos) << json;
}

TEST(JsonCompactArgsTest, Scalars) {
  ExpectSameArgs({R"({"int": -12, "big": 9223372036854775807,
      "small": -9223372036854775808, "overflow": 12345678901234567890,
      "real": 1.25, "exp": 3e-7, "t": true, "f": false, "null": null,
      "str": "s", "empty": "", "escaped": "a\"b\\c\nd\u00e9",
      "k\"ey\u0041": 1})"});
}

TEST(JsonCompactArgsTest, Nested) {
  ExpectSameArgs({R"({"External id": 7, "a": {"b": {"c": "d"}, "e": {}},
      "Input Dims": [[1024, 1024], [], [3]], "Input type": ["float", ""],
      "objs": [{"k": 1}, {"k": [2, {"z": null}]}], "nulls": [null, 5, null],
      "empty": []})"});
}

TEST(JsonCompactArgsTest, RepeatedKeys) {
  // Repeated keys update the arg in place, and arrays with the same key
  // continue the numbering of the earlier ones.
  ExpectSameArgs({R"({"a": [1, 2], "b": 1, "a": [3], "b": "x",
      "c": {"d": [1]}, "c": {"d": [2]}})"});
}

TEST(JsonCompactArgsTest, LongStrings) {
  std::string long_str(1000, 'x');
  ExpectSameArgs({"{\"a\": \"" + long_str + "\", \"b\": [\"" + long_str +
                  "\"], \"c\": \"" + long_str + "\"}"});
}

TEST(JsonCompactArgsTest, MergedIntoSameInserter) {
  // Like the args of an end event, merged with those of the begin event.
  ExpectSameArgs({R"({"a": [1, 2], "b": 1})", R"({"b": 2, "c": {"d": 3}})"});
}

TEST(JsonCompactArgsTest, MinifiedMalformedJson) {
  // json::AddJsonValueToArgs() keeps the args before the error.
  for (const char* args :
       {R"({"a": 1, "b" 2})", R"({"a": [1  2], "b": 3})", R"({"a": 1  x})",
        R"({"a": {"b": true  }, "c": "d"  "e"})"}) {
    EXPECT_EQ(AddArgs({args}, Mode::kJson),
              AddArgs({args}, Mode::kMinifiedJson))
        << args;
  }
}

TEST(JsonCompactArgsTest, RejectsWhatCannotBeReplayedExactly) {
  TraceStorage storage;
  JsonCompactArgs compact_args(&storage);
  std::vector<uint8_t> out;
  EXPECT_TRUE(compact_args.Encode(R"({"a": [1]})", true, &out));
  EXPECT_FALSE(compact_args.Encode(R"({"a": [1]})", false, &out));
  EXPECT_FALSE(compact_args.Encode(R"({"a": {"b": [1]}})", false, &out));
  EXPECT_TRUE(compact_args.Encode(R"({"a": {"b": 1}})", false, &out));
  // json::AddJsonValueToArgs() would add "args.a" before failing.
  EXPECT_FALSE(compact_args.Encode(R"({"a": 1, "b" 2})", true, &out));
  EXPECT_FALSE(compact_args.Encode(R"({"a": 1, "b": tru})", true, &out));
}

}  // namespace
}  // namespace perfetto::trace_processor
