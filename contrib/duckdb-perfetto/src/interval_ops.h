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

#ifndef CONTRIB_DUCKDB_PERFETTO_SRC_INTERVAL_OPS_H_
#define CONTRIB_DUCKDB_PERFETTO_SRC_INTERVAL_OPS_H_

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_or.h"

namespace perfetto::duckdb_ext {

// The interval kernels behind the interval_* DuckDB table functions. They are
// the RFC-0041 interval operators which TraceProcessor does not implement
// yet, as plain C++ over (ts, dur, id) triples: no DuckDB, no SQL. The DuckDB
// glue (interval_functions.cc) partitions rows into lanes (PER) and feeds each
// lane through one of these functions.
//
// Conventions shared by every kernel:
//  * An interval is [ts, ts + dur), half-open. dur == 0 is a point (an
//    instant). Callers normalize dur first (NormalizeDur): kernels never see
//    a negative dur.
//  * A point is inside [a, b) when a <= ts < b, and overlaps another point
//    at the same ts. Intervals which only touch ([0, 10) and [10, 20)) do not
//    overlap.
//  * The coverage of a set of intervals is their union. Points have no
//    extent, so they never cover anything.
//  * |id| identifies the row an interval came from. It is opaque to the
//    kernels and copied to their outputs. kNoMatch is reserved.
//  * Inputs need not be sorted, and outputs are in no particular order
//    unless documented otherwise.

struct Interval {
  int64_t ts = 0;
  int64_t dur = 0;
  int64_t id = 0;

  int64_t end() const { return ts + dur; }
};

// An operand id meaning "no operand row" (e.g. an unmatched row of a LEFT
// join, or a piece of a split row which no operand covers).
constexpr int64_t kNoMatch = std::numeric_limits<int64_t>::min();

// Validates a raw (ts, dur) pair and returns the duration to use:
//  * dur >= 0 is returned as is.
//  * dur == -1 is an open interval, e.g. a slice which had not ended when
//    the trace stopped. It is closed at |open_end| if that is set, giving
//    max(0, open_end - ts), and is an error otherwise.
//  * Any other negative dur is an error, as is an end beyond INT64_MAX.
base::StatusOr<int64_t> NormalizeDur(int64_t ts,
                                     int64_t dur,
                                     std::optional<int64_t> open_end);

// RFC-0041 INTERVAL MERGE OVERLAPPING ... AGGREGATE COUNT(*).
//
// The coverage of |rows| as maximal islands, ordered by ts. Overlapping and
// touching intervals coalesce. A point merges into an island it lies in or
// touches, and is an island of its own (dur 0) otherwise. |row_count| is the
// number of rows in the island.
struct Island {
  int64_t ts = 0;
  int64_t dur = 0;
  int64_t row_count = 0;
};
std::vector<Island> MergeIntervals(std::vector<Interval> rows);

// The uncovered spans between consecutive islands of MergeIntervals(rows),
// ordered by ts. Equivalent to subtracting |rows| from [min ts, max end).
struct Span {
  int64_t ts = 0;
  int64_t dur = 0;
};
std::vector<Span> GapsBetween(std::vector<Interval> rows);

// A piece of the row |id|, with new bounds.
struct Piece {
  int64_t id = 0;
  int64_t ts = 0;
  int64_t dur = 0;
};

// RFC-0041 INTERVAL SUBTRACT.
//
// The pieces of each row which the coverage of |operand| leaves uncovered,
// in order within each row. Operand points remove nothing. A point row is
// kept, as a dur 0 piece, unless an operand interval contains it, or
// |keep_points| is false.
std::vector<Piece> SubtractIntervals(const std::vector<Interval>& rows,
                                     const std::vector<Interval>& operand,
                                     bool keep_points);

// RFC-0041 INTERVAL SPLIT.
//
// Cuts each row at the boundaries of the operand intervals which overlap it,
// and pairs every piece with each operand row covering it, or with kNoMatch
// if none does. When the operand does not self-overlap the pieces of a row
// tile it exactly. Operand points neither cut nor cover an interval; a point
// row is paired with every operand row it overlaps.
struct SplitPiece {
  int64_t id = 0;
  int64_t operand_id = kNoMatch;
  int64_t ts = 0;
  int64_t dur = 0;
};
std::vector<SplitPiece> SplitIntervals(const std::vector<Interval>& rows,
                                       const std::vector<Interval>& operand);

// RFC-0041 relationship phrases, from the point of view of a row r = [s, e)
// and an operand o = [os, oe):
enum class Relationship {
  // OVERLAPPING BOUNDS: r and o overlap.
  kOverlapping,
  // COVERING BEGIN: o contains the instant s.
  kCoveringBegin,
  // COVERING END: o contains the instant e (os <= e < oe; a point at e
  // counts).
  kCoveringEnd,
  // COVERING BOUNDS: o contains all of r: os <= s and e <= oe. For a point
  // row, the same as kCoveringBegin.
  kCoveringBounds,
  // WITHIN BOUNDS: o lies inside r: s <= os and oe <= e. A point operand is
  // inside r if r contains it.
  kWithin,
};

// RFC-0041 [LEFT] INTERVAL JOIN.
//
// One match per (row, operand row) pair in |relationship|. With |left|, a row
// without any match yields one (id, kNoMatch) match.
struct Match {
  int64_t id = 0;
  int64_t operand_id = kNoMatch;
};
std::vector<Match> JoinIntervals(const std::vector<Interval>& rows,
                                 const std::vector<Interval>& operand,
                                 Relationship relationship,
                                 bool left);

// Exposure: for every row, the length of its overlap with the coverage of
// |operand| (so overlapping operand rows count once). Exactly one result per
// row, in the order of |rows|; points overlap nothing.
//
// Not an RFC-0041 operator of its own: it is INTERVAL SPLIT against the
// merged operand followed by an AGGREGATE per row.
struct Exposure {
  int64_t id = 0;
  int64_t overlap_dur = 0;
};
std::vector<Exposure> OverlapWithCoverage(const std::vector<Interval>& rows,
                                          const std::vector<Interval>& operand);

// Clips each row to [window_start, window_end). Rows outside the window, and
// intervals which only touch it, are dropped. A point is kept if the window
// contains it. Requires window_start <= window_end.
std::vector<Piece> ClipIntervals(const std::vector<Interval>& rows,
                                 int64_t window_start,
                                 int64_t window_end);

// RFC-0041 INTERVAL QUANTIZE width ALIGNED TO origin.
//
// Cuts each row at multiples of |width| from |origin| and tags each piece
// with its bucket, floor((piece ts - origin) / width). A point falls in one
// bucket. Requires width > 0.
struct BucketPiece {
  int64_t id = 0;
  int64_t ts = 0;
  int64_t dur = 0;
  int64_t bucket = 0;
};
base::StatusOr<std::vector<BucketPiece>> QuantizeIntervals(
    const std::vector<Interval>& rows,
    int64_t width,
    int64_t origin);

}  // namespace perfetto::duckdb_ext

#endif  // CONTRIB_DUCKDB_PERFETTO_SRC_INTERVAL_OPS_H_
