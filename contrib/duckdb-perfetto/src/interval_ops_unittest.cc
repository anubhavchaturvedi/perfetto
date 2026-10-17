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
#include <cstdint>
#include <limits>
#include <random>
#include <tuple>
#include <vector>

#include "test/gtest_and_gmock.h"

namespace perfetto::duckdb_ext {
namespace {

using ::testing::ElementsAre;
using ::testing::IsEmpty;

using Tuple2 = std::tuple<int64_t, int64_t>;
using Tuple3 = std::tuple<int64_t, int64_t, int64_t>;
using Tuple4 = std::tuple<int64_t, int64_t, int64_t, int64_t>;

constexpr int64_t kNone = kNoMatch;

// Rows with ids 1, 2, ... in order.
std::vector<Interval> Rows(std::initializer_list<Tuple2> ts_dur) {
  std::vector<Interval> rows;
  int64_t id = 1;
  for (const auto& [ts, dur] : ts_dur)
    rows.push_back({ts, dur, id++});
  return rows;
}

// Operand rows with ids 101, 102, ... in order.
std::vector<Interval> Operand(std::initializer_list<Tuple2> ts_dur) {
  std::vector<Interval> rows = Rows(ts_dur);
  for (Interval& r : rows)
    r.id += 100;
  return rows;
}

std::vector<Tuple3> Islands(const std::vector<Interval>& rows) {
  std::vector<Tuple3> res;
  for (const Island& i : MergeIntervals(rows))
    res.emplace_back(i.ts, i.dur, i.row_count);
  return res;
}

std::vector<Tuple2> Gaps(const std::vector<Interval>& rows) {
  std::vector<Tuple2> res;
  for (const Span& s : GapsBetween(rows))
    res.emplace_back(s.ts, s.dur);
  return res;
}

std::vector<Tuple3> Subtract(const std::vector<Interval>& rows,
                             const std::vector<Interval>& operand,
                             bool keep_points = true) {
  std::vector<Tuple3> res;
  for (const Piece& p : SubtractIntervals(rows, operand, keep_points))
    res.emplace_back(p.id, p.ts, p.dur);
  std::sort(res.begin(), res.end());
  return res;
}

// (id, operand_id, ts, dur), sorted.
std::vector<Tuple4> Split(const std::vector<Interval>& rows,
                          const std::vector<Interval>& operand) {
  std::vector<Tuple4> res;
  for (const SplitPiece& p : SplitIntervals(rows, operand))
    res.emplace_back(p.id, p.operand_id, p.ts, p.dur);
  std::sort(res.begin(), res.end());
  return res;
}

std::vector<Tuple2> Join(const std::vector<Interval>& rows,
                         const std::vector<Interval>& operand,
                         Relationship rel,
                         bool left = false) {
  std::vector<Tuple2> res;
  for (const Match& m : JoinIntervals(rows, operand, rel, left))
    res.emplace_back(m.id, m.operand_id);
  std::sort(res.begin(), res.end());
  return res;
}

std::vector<Tuple2> Overlap(const std::vector<Interval>& rows,
                            const std::vector<Interval>& operand) {
  std::vector<Tuple2> res;
  for (const Exposure& e : OverlapWithCoverage(rows, operand))
    res.emplace_back(e.id, e.overlap_dur);
  return res;
}

std::vector<Tuple3> Clip(const std::vector<Interval>& rows,
                         int64_t start,
                         int64_t end) {
  std::vector<Tuple3> res;
  for (const Piece& p : ClipIntervals(rows, start, end))
    res.emplace_back(p.id, p.ts, p.dur);
  return res;
}

std::vector<Tuple4> Quantize(const std::vector<Interval>& rows,
                             int64_t width,
                             int64_t origin) {
  auto pieces = QuantizeIntervals(rows, width, origin);
  EXPECT_TRUE(pieces.ok()) << pieces.status().message();
  std::vector<Tuple4> res;
  if (!pieces.ok())
    return res;
  for (const BucketPiece& p : *pieces)
    res.emplace_back(p.id, p.ts, p.dur, p.bucket);
  return res;
}

// Brute-force definitions of the relationships, straight from the header,
// to check the sweep in JoinIntervals against.
bool ContainsInstant(const Interval& o, int64_t t) {
  return o.dur > 0 ? o.ts <= t && t < o.end() : o.ts == t;
}

bool Related(const Interval& r, const Interval& o, Relationship rel) {
  switch (rel) {
    case Relationship::kOverlapping:
      if (r.dur > 0 && o.dur > 0)
        return std::max(r.ts, o.ts) < std::min(r.end(), o.end());
      if (r.dur > 0)
        return ContainsInstant(r, o.ts);
      return ContainsInstant(o, r.ts);
    case Relationship::kCoveringBegin:
      return ContainsInstant(o, r.ts);
    case Relationship::kCoveringEnd:
      return ContainsInstant(o, r.end());
    case Relationship::kCoveringBounds:
      if (r.dur == 0)
        return ContainsInstant(o, r.ts);
      return o.dur > 0 && o.ts <= r.ts && r.end() <= o.end();
    case Relationship::kWithin:
      if (r.dur == 0)
        return o.dur == 0 && o.ts == r.ts;
      return o.dur > 0 ? r.ts <= o.ts && o.end() <= r.end()
                       : ContainsInstant(r, o.ts);
  }
  return false;
}

// Random rows on a small grid, so that touching, nested, equal and point
// intervals are all frequent.
std::vector<Interval> RandomRows(std::minstd_rand* rnd,
                                 size_t n,
                                 int64_t id_base) {
  std::uniform_int_distribution<int64_t> ts(0, 40);
  std::uniform_int_distribution<int64_t> dur(0, 12);
  std::vector<Interval> rows;
  for (size_t i = 0; i < n; ++i) {
    int64_t d = dur(*rnd);
    rows.push_back(
        {ts(*rnd), d < 3 ? 0 : d, id_base + static_cast<int64_t>(i)});
  }
  return rows;
}

// Length of the union of the intervals of |rows| intersected with [s, e).
int64_t CoveredLength(const std::vector<Interval>& rows, int64_t s, int64_t e) {
  int64_t covered = 0;
  for (int64_t t = s; t < e; ++t) {
    for (const Interval& o : rows) {
      if (o.dur > 0 && o.ts <= t && t < o.end()) {
        ++covered;
        break;
      }
    }
  }
  return covered;
}

TEST(IntervalOpsTest, NormalizeDur) {
  EXPECT_EQ(*NormalizeDur(10, 5, std::nullopt), 5);
  EXPECT_EQ(*NormalizeDur(10, 0, std::nullopt), 0);
  // Open intervals need an end.
  EXPECT_FALSE(NormalizeDur(10, -1, std::nullopt).ok());
  EXPECT_EQ(*NormalizeDur(10, -1, 25), 15);
  EXPECT_EQ(*NormalizeDur(10, -1, 10), 0);
  EXPECT_EQ(*NormalizeDur(10, -1, 3), 0);
  EXPECT_FALSE(NormalizeDur(10, -2, 25).ok());
  constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
  EXPECT_FALSE(NormalizeDur(kMax - 5, 10, std::nullopt).ok());
  EXPECT_EQ(*NormalizeDur(kMax - 5, 5, std::nullopt), 5);
}

TEST(IntervalOpsTest, MergeOverlappingTouchingAndNested) {
  EXPECT_THAT(Islands({}), IsEmpty());
  EXPECT_THAT(Islands(Rows({{0, 10}, {5, 10}})), ElementsAre(Tuple3{0, 15, 2}));
  // Touching intervals coalesce.
  EXPECT_THAT(Islands(Rows({{0, 10}, {10, 10}})),
              ElementsAre(Tuple3{0, 20, 2}));
  // Nested, unsorted, and a separate island.
  EXPECT_THAT(Islands(Rows({{50, 5}, {0, 30}, {10, 5}, {29, 2}})),
              ElementsAre(Tuple3{0, 31, 3}, Tuple3{50, 5, 1}));
}

TEST(IntervalOpsTest, MergePoints) {
  // A point inside or touching an island joins it; an isolated one is an
  // island of its own; points at the same ts merge.
  EXPECT_THAT(Islands(Rows({{0, 10}, {5, 0}, {10, 0}, {12, 0}, {12, 0}})),
              ElementsAre(Tuple3{0, 10, 3}, Tuple3{12, 0, 2}));
  // A point touching the start of an interval.
  EXPECT_THAT(Islands(Rows({{20, 0}, {20, 5}})), ElementsAre(Tuple3{20, 5, 2}));
}

TEST(IntervalOpsTest, Gaps) {
  EXPECT_THAT(Gaps(Rows({{0, 10}, {20, 10}, {25, 15}})),
              ElementsAre(Tuple2{10, 10}));
  EXPECT_THAT(Gaps(Rows({{0, 10}, {10, 10}})), IsEmpty());
  // Points are islands, so they bound gaps.
  EXPECT_THAT(Gaps(Rows({{0, 10}, {15, 0}, {30, 5}})),
              ElementsAre(Tuple2{10, 5}, Tuple2{15, 15}));
  EXPECT_THAT(Gaps(Rows({{0, 10}})), IsEmpty());
}

TEST(IntervalOpsTest, Subtract) {
  EXPECT_THAT(
      Subtract(Rows({{0, 100}}), Operand({{10, 10}, {50, 10}})),
      ElementsAre(Tuple3{1, 0, 10}, Tuple3{1, 20, 30}, Tuple3{1, 60, 40}));
  // Self-overlapping, unsorted operand: its coverage is what is removed.
  EXPECT_THAT(Subtract(Rows({{0, 100}}), Operand({{40, 30}, {10, 40}})),
              ElementsAre(Tuple3{1, 0, 10}, Tuple3{1, 70, 30}));
  // Touching operands remove nothing; covering ones remove everything.
  EXPECT_THAT(Subtract(Rows({{10, 10}}), Operand({{0, 10}, {20, 5}})),
              ElementsAre(Tuple3{1, 10, 10}));
  EXPECT_THAT(Subtract(Rows({{10, 10}}), Operand({{0, 50}})), IsEmpty());
  // Operand points have no extent.
  EXPECT_THAT(Subtract(Rows({{0, 10}}), Operand({{5, 0}})),
              ElementsAre(Tuple3{1, 0, 10}));
  EXPECT_THAT(Subtract(Rows({{0, 10}}), {}), ElementsAre(Tuple3{1, 0, 10}));
}

TEST(IntervalOpsTest, SubtractPoints) {
  // A point survives unless an operand interval contains it ([a, b) is
  // half-open, so a point at b survives).
  auto rows = Rows({{5, 0}, {10, 0}, {0, 0}, {20, 0}});
  EXPECT_THAT(Subtract(rows, Operand({{0, 10}})),
              ElementsAre(Tuple3{2, 10, 0}, Tuple3{4, 20, 0}));
  EXPECT_THAT(Subtract(rows, Operand({{0, 10}}), /*keep_points=*/false),
              IsEmpty());
}

TEST(IntervalOpsTest, Split) {
  EXPECT_THAT(Split(Rows({{0, 100}}), Operand({{10, 10}, {15, 15}})),
              ElementsAre(Tuple4{1, kNone, 0, 10}, Tuple4{1, kNone, 30, 70},
                          Tuple4{1, 101, 10, 5}, Tuple4{1, 101, 15, 5},
                          Tuple4{1, 102, 15, 5}, Tuple4{1, 102, 20, 10}));
  // Operands sticking out of the row are clipped to it.
  EXPECT_THAT(Split(Rows({{10, 10}}), Operand({{0, 15}, {18, 100}})),
              ElementsAre(Tuple4{1, kNone, 15, 3}, Tuple4{1, 101, 10, 5},
                          Tuple4{1, 102, 18, 2}));
  // Operand points do not cut; touching operands do not attach.
  EXPECT_THAT(Split(Rows({{10, 10}}), Operand({{15, 0}, {0, 10}, {20, 3}})),
              ElementsAre(Tuple4{1, kNone, 10, 10}));
  // A point row is paired with every operand row it overlaps.
  EXPECT_THAT(Split(Rows({{15, 0}, {50, 0}}), Operand({{10, 10}, {15, 0}})),
              ElementsAre(Tuple4{1, 101, 15, 0}, Tuple4{1, 102, 15, 0},
                          Tuple4{2, kNone, 50, 0}));
}

TEST(IntervalOpsTest, SplitTilesRowsWhenOperandDoesNotSelfOverlap) {
  std::minstd_rand rnd(42);
  for (int iter = 0; iter < 200; ++iter) {
    std::vector<Interval> rows = RandomRows(&rnd, 20, 1);
    // A non-overlapping operand: a random tiling with holes.
    std::vector<Interval> operand;
    int64_t t = 0;
    for (int64_t id = 1000; t < 60; ++id) {
      int64_t len = 1 + static_cast<int64_t>(rnd() % 7);
      if (rnd() % 3)
        operand.push_back({t, len, id});
      t += len;
    }
    std::vector<int64_t> covered(rows.size() + 1, 0);
    for (const SplitPiece& p : SplitIntervals(rows, operand)) {
      const Interval& r = rows[static_cast<size_t>(p.id - 1)];
      ASSERT_GE(p.ts, r.ts);
      ASSERT_LE(p.ts + p.dur, r.end());
      covered[static_cast<size_t>(p.id)] += p.dur;
      if (p.operand_id != kNoMatch) {
        auto it = std::find_if(
            operand.begin(), operand.end(),
            [&](const Interval& x) { return x.id == p.operand_id; });
        ASSERT_NE(it, operand.end());
        ASSERT_LE(it->ts, p.ts);
        ASSERT_GE(it->end(), p.ts + p.dur);
      }
    }
    for (const Interval& r : rows)
      EXPECT_EQ(covered[static_cast<size_t>(r.id)], r.dur);
  }
}

TEST(IntervalOpsTest, JoinRelationships) {
  auto rows = Rows({{10, 10}});
  auto op = Operand({{0, 10},    // 101: touches the start
                     {0, 15},    // 102: covers the begin
                     {15, 5},    // 103: within, ends at the end
                     {15, 10},   // 104: covers the end
                     {5, 20},    // 105: covers the bounds
                     {20, 0},    // 106: point at the end
                     {10, 0},    // 107: point at the begin
                     {12, 0}});  // 108: point inside
  EXPECT_THAT(Join(rows, op, Relationship::kOverlapping),
              ElementsAre(Tuple2{1, 102}, Tuple2{1, 103}, Tuple2{1, 104},
                          Tuple2{1, 105}, Tuple2{1, 107}, Tuple2{1, 108}));
  EXPECT_THAT(Join(rows, op, Relationship::kCoveringBegin),
              ElementsAre(Tuple2{1, 102}, Tuple2{1, 105}, Tuple2{1, 107}));
  EXPECT_THAT(Join(rows, op, Relationship::kCoveringEnd),
              ElementsAre(Tuple2{1, 104}, Tuple2{1, 105}, Tuple2{1, 106}));
  EXPECT_THAT(Join(rows, op, Relationship::kCoveringBounds),
              ElementsAre(Tuple2{1, 105}));
  EXPECT_THAT(Join(rows, op, Relationship::kWithin),
              ElementsAre(Tuple2{1, 103}, Tuple2{1, 107}, Tuple2{1, 108}));
}

TEST(IntervalOpsTest, JoinPointRowsAndLeft) {
  auto rows = Rows({{10, 0}, {100, 0}});
  auto op = Operand({{10, 0}, {5, 5}, {10, 5}, {0, 50}});
  EXPECT_THAT(Join(rows, op, Relationship::kOverlapping, /*left=*/true),
              ElementsAre(Tuple2{1, 101}, Tuple2{1, 103}, Tuple2{1, 104},
                          Tuple2{2, kNone}));
  EXPECT_THAT(Join(rows, op, Relationship::kWithin, /*left=*/true),
              ElementsAre(Tuple2{1, 101}, Tuple2{2, kNone}));
  EXPECT_THAT(Join(rows, {}, Relationship::kOverlapping, /*left=*/false),
              IsEmpty());
}

TEST(IntervalOpsTest, JoinMatchesBruteForce) {
  std::minstd_rand rnd(7);
  const Relationship kAll[] = {
      Relationship::kOverlapping, Relationship::kCoveringBegin,
      Relationship::kCoveringEnd, Relationship::kCoveringBounds,
      Relationship::kWithin};
  for (int iter = 0; iter < 300; ++iter) {
    std::vector<Interval> rows = RandomRows(&rnd, 15, 1);
    std::vector<Interval> op = RandomRows(&rnd, 15, 1000);
    for (Relationship rel : kAll) {
      for (bool left : {false, true}) {
        std::vector<Tuple2> expected;
        for (const Interval& r : rows) {
          bool any = false;
          for (const Interval& o : op) {
            if (Related(r, o, rel)) {
              expected.emplace_back(r.id, o.id);
              any = true;
            }
          }
          if (!any && left)
            expected.emplace_back(r.id, kNone);
        }
        std::sort(expected.begin(), expected.end());
        ASSERT_EQ(Join(rows, op, rel, left), expected)
            << "relationship " << static_cast<int>(rel) << " left " << left
            << " iteration " << iter;
      }
    }
  }
}

TEST(IntervalOpsTest, OverlapWithCoverage) {
  // Overlapping operand rows count once.
  EXPECT_THAT(Overlap(Rows({{0, 100}, {95, 0}, {200, 10}}),
                      Operand({{10, 10}, {15, 15}, {90, 100}})),
              ElementsAre(Tuple2{1, 30}, Tuple2{2, 0}, Tuple2{3, 0}));
  EXPECT_THAT(Overlap(Rows({{0, 10}}), {}), ElementsAre(Tuple2{1, 0}));
  // Touching is not overlapping.
  EXPECT_THAT(Overlap(Rows({{10, 10}}), Operand({{0, 10}, {20, 10}})),
              ElementsAre(Tuple2{1, 0}));
}

TEST(IntervalOpsTest, OverlapMatchesBruteForceAndSubtract) {
  std::minstd_rand rnd(3);
  for (int iter = 0; iter < 300; ++iter) {
    std::vector<Interval> rows = RandomRows(&rnd, 15, 1);
    std::vector<Interval> op = RandomRows(&rnd, 15, 1000);
    std::vector<int64_t> remaining(rows.size() + 1, 0);
    for (const Piece& p : SubtractIntervals(rows, op, /*keep_points=*/true))
      remaining[static_cast<size_t>(p.id)] += p.dur;
    for (const Exposure& e : OverlapWithCoverage(rows, op)) {
      const Interval& r = rows[static_cast<size_t>(e.id - 1)];
      ASSERT_EQ(e.overlap_dur, CoveredLength(op, r.ts, r.end()));
      // Exposure and subtraction agree: the overlap and what is left of a
      // row add up to the row.
      ASSERT_EQ(e.overlap_dur + remaining[static_cast<size_t>(e.id)], r.dur);
    }
  }
}

TEST(IntervalOpsTest, Clip) {
  auto rows = Rows({{0, 100},   // Cut on both sides.
                    {20, 10},   // Inside.
                    {50, 20},   // Cut at the end.
                    {0, 10},    // Touches the start: dropped.
                    {60, 5},    // Touches the end: dropped.
                    {10, 0},    // Point at the start: kept.
                    {50, 0},    // Point inside: kept.
                    {60, 0}});  // Point at the end: dropped.
  EXPECT_THAT(
      Clip(rows, 10, 60),
      ElementsAre(Tuple3{1, 10, 50}, Tuple3{2, 20, 10}, Tuple3{3, 50, 10},
                  Tuple3{6, 10, 0}, Tuple3{7, 50, 0}));
  EXPECT_THAT(Clip(rows, 10, 10), IsEmpty());
}

TEST(IntervalOpsTest, Quantize) {
  EXPECT_THAT(Quantize(Rows({{5, 20}}), 10, 0),
              ElementsAre(Tuple4{1, 5, 5, 0}, Tuple4{1, 10, 10, 1},
                          Tuple4{1, 20, 5, 2}));
  // Aligned rows stay whole; buckets before the origin are negative.
  EXPECT_THAT(Quantize(Rows({{10, 10}, {-5, 10}}), 10, 0),
              ElementsAre(Tuple4{1, 10, 10, 1}, Tuple4{2, -5, 5, -1},
                          Tuple4{2, 0, 5, 0}));
  // A custom origin, and a point.
  EXPECT_THAT(Quantize(Rows({{3, 10}, {12, 0}, {13, 0}}), 10, 3),
              ElementsAre(Tuple4{1, 3, 10, 0}, Tuple4{2, 12, 0, 0},
                          Tuple4{3, 13, 0, 1}));
  EXPECT_FALSE(QuantizeIntervals(Rows({{0, 10}}), 0, 0).ok());
  EXPECT_FALSE(QuantizeIntervals(Rows({{0, int64_t{1} << 40}}), 1, 0).ok());
}

TEST(IntervalOpsTest, QuantizeTilesRows) {
  std::minstd_rand rnd(11);
  for (int iter = 0; iter < 200; ++iter) {
    std::vector<Interval> rows = RandomRows(&rnd, 10, 1);
    int64_t width = 1 + static_cast<int64_t>(rnd() % 9);
    int64_t origin = static_cast<int64_t>(rnd() % 20) - 10;
    std::vector<int64_t> covered(rows.size() + 1, 0);
    for (const auto& [id, ts, dur, bucket] : Quantize(rows, width, origin)) {
      const Interval& r = rows[static_cast<size_t>(id - 1)];
      ASSERT_GE(ts, r.ts);
      ASSERT_LE(ts + dur, r.end());
      ASSERT_GE(ts, origin + bucket * width);
      ASSERT_LE(ts + dur, origin + (bucket + 1) * width);
      covered[static_cast<size_t>(id)] += dur;
    }
    for (const Interval& r : rows)
      EXPECT_EQ(covered[static_cast<size_t>(r.id)], r.dur);
  }
}

}  // namespace
}  // namespace perfetto::duckdb_ext
