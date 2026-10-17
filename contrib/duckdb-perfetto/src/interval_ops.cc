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

#include "contrib/duckdb-perfetto/src/interval_ops.h"

#include <algorithm>
#include <cinttypes>
#include <numeric>
#include <utility>

#include "perfetto/base/logging.h"

namespace perfetto::duckdb_ext {
namespace {

// A row produces at most this many buckets in QuantizeIntervals: more is
// almost certainly a unit mistake (e.g. a width in seconds, not ns).
constexpr int64_t kMaxBucketsPerRow = int64_t{1} << 30;

// [start, end), with start < end.
struct Range {
  int64_t start;
  int64_t end;
};

// The coverage of the intervals (not the points) of |rows|: disjoint,
// non-touching ranges ordered by start.
std::vector<Range> Coverage(const std::vector<Interval>& rows) {
  std::vector<Range> ranges;
  for (const Interval& r : rows) {
    if (r.dur > 0)
      ranges.push_back({r.ts, r.end()});
  }
  std::sort(ranges.begin(), ranges.end(),
            [](const Range& a, const Range& b) { return a.start < b.start; });
  std::vector<Range> merged;
  for (const Range& r : ranges) {
    if (!merged.empty() && r.start <= merged.back().end) {
      merged.back().end = std::max(merged.back().end, r.end);
    } else {
      merged.push_back(r);
    }
  }
  return merged;
}

// The indices of |rows| ordered by ts, or by end if |by_end|.
std::vector<size_t> SortedOrder(const std::vector<Interval>& rows,
                                bool by_end) {
  std::vector<size_t> order(rows.size());
  std::iota(order.begin(), order.end(), 0);
  std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
    return by_end ? rows[a].end() < rows[b].end() : rows[a].ts < rows[b].ts;
  });
  return order;
}

// Operand intervals sorted by ts, answering the queries every relationship
// is built from:
//  * Active(p): operands which start strictly before the instant p and end
//    strictly after it (os < p < oe). This is a sweep: successive calls must
//    not decrease p. Each call costs O(matches + newly expired operands).
//  * Starting(lo, hi): operands with lo <= os < hi.
//  * StartingAt(p): operands with os == p.
// For example, the operands overlapping [s, e) are Active(s) + Starting(s,
// e), and those containing the instant p are Active(p) + StartingAt(p).
class OperandIndex {
 public:
  explicit OperandIndex(const std::vector<Interval>& operand)
      : sorted_(operand) {
    std::stable_sort(
        sorted_.begin(), sorted_.end(),
        [](const Interval& a, const Interval& b) { return a.ts < b.ts; });
  }

  OperandIndex(const OperandIndex&) = delete;
  OperandIndex& operator=(const OperandIndex&) = delete;

  void Active(int64_t p, std::vector<const Interval*>* out) {
    PERFETTO_DCHECK(!last_p_ || p >= *last_p_);
    last_p_ = p;
    while (next_ < sorted_.size() && sorted_[next_].ts < p)
      active_.push_back(next_++);
    size_t kept = 0;
    for (size_t k = 0; k < active_.size(); ++k) {
      const Interval& o = sorted_[active_[k]];
      if (o.end() > p) {
        active_[kept++] = active_[k];
        out->push_back(&o);
      }
    }
    active_.resize(kept);
  }

  void Starting(int64_t lo,
                int64_t hi,
                std::vector<const Interval*>* out) const {
    for (auto it = LowerBound(lo); it != sorted_.end() && it->ts < hi; ++it)
      out->push_back(&*it);
  }

  void StartingAt(int64_t p, std::vector<const Interval*>* out) const {
    for (auto it = LowerBound(p); it != sorted_.end() && it->ts == p; ++it)
      out->push_back(&*it);
  }

 private:
  std::vector<Interval>::const_iterator LowerBound(int64_t ts) const {
    return std::lower_bound(
        sorted_.begin(), sorted_.end(), ts,
        [](const Interval& i, int64_t value) { return i.ts < value; });
  }

  std::vector<Interval> sorted_;
  std::vector<size_t> active_;  // Indices into sorted_.
  size_t next_ = 0;             // First operand never added to active_.
  std::optional<int64_t> last_p_;
};

// The operands overlapping |r| (see the conventions in the header).
void Overlapping(OperandIndex* index,
                 const Interval& r,
                 std::vector<const Interval*>* out) {
  index->Active(r.ts, out);
  if (r.dur > 0) {
    index->Starting(r.ts, r.end(), out);
  } else {
    index->StartingAt(r.ts, out);
  }
}

template <typename Pred>
void KeepIf(std::vector<const Interval*>* v, Pred pred) {
  v->erase(std::remove_if(v->begin(), v->end(),
                          [&](const Interval* o) { return !pred(*o); }),
           v->end());
}

int64_t FloorDiv(int64_t a, int64_t b) {
  PERFETTO_DCHECK(b > 0);
  int64_t q = a / b;
  if (a % b != 0 && a < 0)
    --q;
  return q;
}

}  // namespace

base::StatusOr<int64_t> NormalizeDur(int64_t ts,
                                     int64_t dur,
                                     std::optional<int64_t> open_end) {
  if (dur == -1) {
    if (!open_end) {
      return base::ErrStatus(
          "row at ts %" PRId64
          " has dur = -1 (an open interval): pass open_end := <ts> to close "
          "open intervals there, or filter them out",
          ts);
    }
    if (*open_end <= ts)
      return int64_t{0};
    if (__builtin_sub_overflow(*open_end, ts, &dur))
      return base::ErrStatus("open_end - ts overflows int64 at ts %" PRId64,
                             ts);
  } else if (dur < 0) {
    return base::ErrStatus("row at ts %" PRId64 " has negative dur %" PRId64,
                           ts, dur);
  }
  int64_t end;
  if (__builtin_add_overflow(ts, dur, &end)) {
    return base::ErrStatus("row at ts %" PRId64 " with dur %" PRId64
                           " ends beyond INT64_MAX",
                           ts, dur);
  }
  return dur;
}

std::vector<Island> MergeIntervals(std::vector<Interval> rows) {
  std::sort(rows.begin(), rows.end(), [](const Interval& a, const Interval& b) {
    return a.ts < b.ts || (a.ts == b.ts && a.dur < b.dur);
  });
  std::vector<Island> islands;
  for (const Interval& r : rows) {
    if (!islands.empty() && r.ts <= islands.back().ts + islands.back().dur) {
      Island& last = islands.back();
      last.dur = std::max(last.ts + last.dur, r.end()) - last.ts;
      ++last.row_count;
    } else {
      islands.push_back({r.ts, r.dur, 1});
    }
  }
  return islands;
}

std::vector<Span> GapsBetween(std::vector<Interval> rows) {
  std::vector<Island> islands = MergeIntervals(std::move(rows));
  std::vector<Span> gaps;
  for (size_t i = 1; i < islands.size(); ++i) {
    int64_t start = islands[i - 1].ts + islands[i - 1].dur;
    gaps.push_back({start, islands[i].ts - start});
  }
  return gaps;
}

std::vector<Piece> SubtractIntervals(const std::vector<Interval>& rows,
                                     const std::vector<Interval>& operand,
                                     bool keep_points) {
  std::vector<Range> cov = Coverage(operand);
  std::vector<Piece> pieces;
  for (const Interval& r : rows) {
    // The first range ending after r starts: earlier ones cannot touch r.
    auto it = std::upper_bound(
        cov.begin(), cov.end(), r.ts,
        [](int64_t ts, const Range& range) { return ts < range.end; });
    if (r.dur == 0) {
      bool covered = it != cov.end() && it->start <= r.ts;
      if (keep_points && !covered)
        pieces.push_back({r.id, r.ts, 0});
      continue;
    }
    int64_t cursor = r.ts;
    int64_t end = r.end();
    for (; it != cov.end() && it->start < end && cursor < end; ++it) {
      if (it->start > cursor)
        pieces.push_back({r.id, cursor, it->start - cursor});
      cursor = std::max(cursor, it->end);
    }
    if (cursor < end)
      pieces.push_back({r.id, cursor, end - cursor});
  }
  return pieces;
}

std::vector<SplitPiece> SplitIntervals(const std::vector<Interval>& rows,
                                       const std::vector<Interval>& operand) {
  OperandIndex index(operand);
  std::vector<SplitPiece> pieces;
  std::vector<const Interval*> cands;
  std::vector<const Interval*> live;
  std::vector<int64_t> cuts;
  for (size_t i : SortedOrder(rows, /*by_end=*/false)) {
    const Interval& r = rows[i];
    cands.clear();
    Overlapping(&index, r, &cands);
    if (r.dur == 0) {
      if (cands.empty())
        pieces.push_back({r.id, kNoMatch, r.ts, 0});
      for (const Interval* o : cands)
        pieces.push_back({r.id, o->id, r.ts, 0});
      continue;
    }
    // Points have no extent: they neither cut nor cover an interval.
    KeepIf(&cands, [](const Interval& o) { return o.dur > 0; });
    auto clipped_start = [&](const Interval* o) {
      return std::max(o->ts, r.ts);
    };
    cuts.assign({r.ts, r.end()});
    for (const Interval* o : cands) {
      cuts.push_back(clipped_start(o));
      cuts.push_back(std::min(o->end(), r.end()));
    }
    std::sort(cuts.begin(), cuts.end());
    cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
    std::stable_sort(cands.begin(), cands.end(),
                     [&](const Interval* a, const Interval* b) {
                       return clipped_start(a) < clipped_start(b);
                     });
    // Sweep the pieces left to right. Every operand boundary inside r is a
    // cut, so an operand live at the start of a piece covers all of it.
    live.clear();
    size_t next = 0;
    for (size_t k = 0; k + 1 < cuts.size(); ++k) {
      int64_t start = cuts[k];
      int64_t dur = cuts[k + 1] - start;
      while (next < cands.size() && clipped_start(cands[next]) <= start)
        live.push_back(cands[next++]);
      KeepIf(&live, [&](const Interval& o) { return o.end() > start; });
      if (live.empty())
        pieces.push_back({r.id, kNoMatch, start, dur});
      for (const Interval* o : live)
        pieces.push_back({r.id, o->id, start, dur});
    }
  }
  return pieces;
}

std::vector<Match> JoinIntervals(const std::vector<Interval>& rows,
                                 const std::vector<Interval>& operand,
                                 Relationship relationship,
                                 bool left) {
  OperandIndex index(operand);
  std::vector<Match> matches;
  std::vector<const Interval*> cands;
  bool by_end = relationship == Relationship::kCoveringEnd;
  for (size_t i : SortedOrder(rows, by_end)) {
    const Interval& r = rows[i];
    cands.clear();
    switch (relationship) {
      case Relationship::kOverlapping:
        Overlapping(&index, r, &cands);
        break;
      case Relationship::kCoveringBegin:
        index.Active(r.ts, &cands);
        index.StartingAt(r.ts, &cands);
        break;
      case Relationship::kCoveringEnd:
        index.Active(r.end(), &cands);
        index.StartingAt(r.end(), &cands);
        break;
      case Relationship::kCoveringBounds:
        index.Active(r.ts, &cands);
        index.StartingAt(r.ts, &cands);
        KeepIf(&cands, [&](const Interval& o) {
          return o.end() >= r.end() && (o.dur > 0 || r.dur == 0);
        });
        break;
      case Relationship::kWithin:
        if (r.dur > 0) {
          index.Starting(r.ts, r.end(), &cands);
          KeepIf(&cands, [&](const Interval& o) { return o.end() <= r.end(); });
        } else {
          index.StartingAt(r.ts, &cands);
          KeepIf(&cands, [](const Interval& o) { return o.dur == 0; });
        }
        break;
    }
    if (cands.empty() && left)
      matches.push_back({r.id, kNoMatch});
    for (const Interval* o : cands)
      matches.push_back({r.id, o->id});
  }
  return matches;
}

std::vector<Exposure> OverlapWithCoverage(
    const std::vector<Interval>& rows,
    const std::vector<Interval>& operand) {
  std::vector<Range> cov = Coverage(operand);
  // prefix[i] is the total length of cov[0, i).
  std::vector<int64_t> prefix(cov.size() + 1, 0);
  for (size_t i = 0; i < cov.size(); ++i)
    prefix[i + 1] = prefix[i] + (cov[i].end - cov[i].start);
  // The length of the coverage before the instant |t|.
  auto covered_before = [&](int64_t t) -> int64_t {
    size_t n = static_cast<size_t>(
        std::lower_bound(cov.begin(), cov.end(), t,
                         [](const Range& range, int64_t value) {
                           return range.start < value;
                         }) -
        cov.begin());
    if (n == 0)
      return 0;
    const Range& last = cov[n - 1];
    return prefix[n - 1] + (std::min(t, last.end) - last.start);
  };
  std::vector<Exposure> out;
  out.reserve(rows.size());
  for (const Interval& r : rows) {
    int64_t overlap =
        r.dur == 0 ? 0 : covered_before(r.end()) - covered_before(r.ts);
    out.push_back({r.id, overlap});
  }
  return out;
}

std::vector<Piece> ClipIntervals(const std::vector<Interval>& rows,
                                 int64_t window_start,
                                 int64_t window_end) {
  PERFETTO_DCHECK(window_start <= window_end);
  std::vector<Piece> pieces;
  for (const Interval& r : rows) {
    if (r.dur == 0) {
      if (window_start <= r.ts && r.ts < window_end)
        pieces.push_back({r.id, r.ts, 0});
      continue;
    }
    int64_t start = std::max(r.ts, window_start);
    int64_t end = std::min(r.end(), window_end);
    if (start < end)
      pieces.push_back({r.id, start, end - start});
  }
  return pieces;
}

base::StatusOr<std::vector<BucketPiece>> QuantizeIntervals(
    const std::vector<Interval>& rows,
    int64_t width,
    int64_t origin) {
  if (width <= 0)
    return base::ErrStatus("width must be positive, got %" PRId64, width);
  std::vector<BucketPiece> pieces;
  for (const Interval& r : rows) {
    // Work relative to the origin, with inclusive last instants so that
    // nothing overflows at the edges of the int64 range.
    int64_t first_ns;
    int64_t last_ns;
    if (__builtin_sub_overflow(r.ts, origin, &first_ns) ||
        __builtin_add_overflow(first_ns, r.dur == 0 ? 0 : r.dur - 1,
                               &last_ns)) {
      return base::ErrStatus(
          "ts %" PRId64 " - origin %" PRId64 " overflows int64", r.ts, origin);
    }
    int64_t first = FloorDiv(first_ns, width);
    if (r.dur == 0) {
      pieces.push_back({r.id, r.ts, 0, first});
      continue;
    }
    int64_t last = FloorDiv(last_ns, width);
    if (last - first >= kMaxBucketsPerRow) {
      return base::ErrStatus("row at ts %" PRId64 " with dur %" PRId64
                             " spans more than 2^30 buckets of width %" PRId64,
                             r.ts, r.dur, width);
    }
    for (int64_t b = first; b <= last; ++b) {
      int64_t bucket_first;
      int64_t bucket_last;
      if (__builtin_mul_overflow(b, width, &bucket_first) ||
          __builtin_add_overflow(bucket_first, width - 1, &bucket_last)) {
        return base::ErrStatus("bucket %" PRId64 " overflows int64", b);
      }
      int64_t piece_first = std::max(first_ns, bucket_first);
      int64_t piece_last = std::min(last_ns, bucket_last);
      pieces.push_back({r.id, r.ts + (piece_first - first_ns),
                        piece_last - piece_first + 1, b});
    }
  }
  return pieces;
}

}  // namespace perfetto::duckdb_ext
