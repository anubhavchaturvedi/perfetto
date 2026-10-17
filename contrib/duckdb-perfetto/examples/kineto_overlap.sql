-- Copyright (C) 2026 The Android Open Source Project
--
-- Licensed under the Apache License, Version 2.0 (the "License");
-- you may not use this file except in compliance with the License.
-- You may obtain a copy of the License at
--
--      http://www.apache.org/licenses/LICENSE-2.0
--
-- Unless required by applicable law or agreed to in writing, software
-- distributed under the License is distributed on an "AS IS" BASIS,
-- WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
-- See the License for the specific language governing permissions and
-- limitations under the License.

-- GPU busy time per stream, compute / communication overlap and exposed
-- communication for a Kineto (PyTorch profiler) trace, with the interval_*
-- functions; then the same numbers in pure PerfettoSQL, inside TP, as a
-- cross-check. Expects the extension loaded and the trace attached as `t`:
--
--   duckdb -unsigned \
--     -cmd "LOAD '$PWD/out/duckdb_ext/perfetto.duckdb_extension'" \
--     -cmd "ATTACH 'rank-26.pt.trace.pftrace' AS t (TYPE perfetto)" \
--     -f contrib/duckdb-perfetto/examples/kineto_overlap.sql
--
-- (Inside PERFETTO blocks, lines must not end in ';': the CLI would split the
-- statement there. See README.md, Limitations.)

.timer on

-- GPU activity. Kineto traces put each CUDA stream on a gpu_render_stage
-- track named "Channel #<stream>"; its "ugpu" dimension is the device. NCCL
-- kernels are communication, memory copies and sets are neither.
CREATE TEMP TABLE gpu_ops AS
SELECT s.id, s.ts, s.dur, s.name,
       g.int_value AS gpu,
       CAST(regexp_extract(tr.name, '#(\d+)', 1) AS INTEGER) AS stream,
       CASE
         WHEN lower(s.name) LIKE '%nccl%' THEN 'comm'
         WHEN s.name LIKE 'Memcpy%' OR s.name LIKE 'Memset%' THEN 'copy'
         ELSE 'compute'
       END AS kind
FROM t.slice s
JOIN t.track tr ON s.track_id = tr.id
LEFT JOIN t.args g ON g.arg_set_id = tr.dimension_arg_set_id AND g.key = 'ugpu'
WHERE tr.type = 'gpu_render_stage' AND tr.name LIKE 'Channel #%';

SELECT kind, count(*) AS n, sum(dur) AS dur_ns FROM gpu_ops GROUP BY ALL ORDER BY 1;

-- 1. Busy time per stream: the coverage of each stream's activity.
SELECT gpu, stream, count(*) AS islands, sum(dur) AS busy_ns,
       round(100 * sum(dur) / (max(ts + dur) - min(ts)), 2) AS busy_pct_of_span
FROM interval_merge(gpu_ops, per := [gpu, stream])
GROUP BY ALL ORDER BY busy_ns DESC;

-- ... and of the whole GPU (any stream busy).
SELECT gpu, sum(dur) AS gpu_busy_ns FROM interval_merge(gpu_ops, per := gpu)
GROUP BY ALL;

-- 2. Compute / communication overlap, and exposed communication: the
-- communication time no compute kernel hides. On merged communication, so
-- that concurrent NCCL kernels count once.
CREATE TEMP VIEW comm AS FROM gpu_ops WHERE kind = 'comm';
CREATE TEMP VIEW compute AS FROM gpu_ops WHERE kind = 'compute';
CREATE TEMP TABLE comm_cov AS FROM interval_merge(comm, per := gpu);

CREATE TEMP TABLE summary AS
SELECT gpu, sum(dur) AS comm_ns, sum(overlap_dur) AS overlap_ns,
       sum(exposed_dur) AS exposed_ns,
       round(100 * sum(exposed_dur) / sum(dur), 2) AS exposed_pct
FROM interval_exposure(comm_cov, compute, per := gpu)
GROUP BY ALL;
FROM summary;

-- The exposed spans themselves, which add up to exposed_ns.
SELECT gpu, count(*) AS spans, sum(dur) AS exposed_ns
FROM interval_subtract(comm_cov, compute, per := gpu) GROUP BY ALL;

-- 3. Exposure per NCCL kernel (one row per kernel; kernels overlapping each
-- other are each charged).
SELECT regexp_extract(name, '^[A-Za-z_]+') AS kernel, count(*) AS n,
       sum(dur) AS dur_ns, sum(exposed_dur) AS exposed_ns,
       round(100 * sum(exposed_dur) / sum(dur), 1) AS exposed_pct
FROM interval_exposure(comm, compute, per := gpu)
GROUP BY ALL ORDER BY exposed_ns DESC LIMIT 10;

-- 4. Exposed communication over time: 1 s buckets from the first kernel.
SET VARIABLE t0 = (SELECT min(ts) FROM gpu_ops);
SELECT bucket, sum(dur) AS exposed_ns
FROM interval_quantize(
  'SELECT * FROM interval_subtract(comm_cov, compute, per := gpu)',
  1000000000, origin := getvariable('t0'))
GROUP BY ALL ORDER BY bucket;

-- 5. Cross-check: the same totals in pure PerfettoSQL, run by TP itself,
-- with the pipe operators TP already has (INTERVAL FLATTEN and INTERVAL
-- INTERSECTION OF), and no interval_* function.
CREATE TEMP TABLE tp AS SELECT * FROM PERFETTO(t) (
  PERFETTO PRAGMA pipelines = 1
  ; CREATE OR REPLACE PERFETTO TABLE xc_ops AS
    SELECT s.ts, s.dur, extract_arg(tr.dimension_arg_set_id, 'ugpu') AS gpu,
      CAST(substr(tr.name, 10) AS INT) AS stream,
      CASE WHEN lower(s.name) GLOB '*nccl*' THEN 'comm'
        WHEN s.name GLOB 'Memcpy*' OR s.name GLOB 'Memset*' THEN 'copy'
        ELSE 'compute' END AS kind
    FROM slice s JOIN track tr ON s.track_id = tr.id
    WHERE tr.type = 'gpu_render_stage' AND tr.name GLOB 'Channel #*'
  ; CREATE OR REPLACE PERFETTO TABLE xc_stream AS
    FROM xc_ops |> INTERVAL FLATTEN PER gpu, stream AGGREGATE COUNT(*) AS n
  ; CREATE OR REPLACE PERFETTO TABLE xc_gpu AS
    FROM xc_ops |> INTERVAL FLATTEN PER gpu AGGREGATE COUNT(*) AS n
  ; CREATE OR REPLACE PERFETTO TABLE xc_comm AS
    FROM (SELECT ts, dur, gpu FROM xc_ops WHERE kind = 'comm')
    |> INTERVAL FLATTEN PER gpu AGGREGATE COUNT(*) AS n
  ; CREATE OR REPLACE PERFETTO TABLE xc_compute AS
    FROM (SELECT ts, dur, gpu FROM xc_ops WHERE kind = 'compute')
    |> INTERVAL FLATTEN PER gpu AGGREGATE COUNT(*) AS n
  ; CREATE OR REPLACE PERFETTO TABLE xc_overlap AS
    INTERVAL INTERSECTION OF (xc_comm AS c, xc_compute AS k) PER gpu
    |> AGGREGATE SUM(dur) AS overlap_ns GROUP BY gpu
  ; SELECT 'stream ' || stream AS what, SUM(dur) AS ns FROM xc_stream GROUP BY gpu, stream
  UNION ALL SELECT 'gpu busy', SUM(dur) FROM xc_gpu
  UNION ALL SELECT 'comm', SUM(dur) FROM xc_comm
  UNION ALL SELECT 'overlap', SUM(overlap_ns) FROM xc_overlap
  UNION ALL SELECT 'exposed', (SELECT SUM(dur) FROM xc_comm) - (SELECT SUM(overlap_ns) FROM xc_overlap)
);

CREATE TEMP TABLE duck AS
SELECT 'stream ' || stream AS what, sum(dur) AS ns
FROM interval_merge(gpu_ops, per := [gpu, stream]) GROUP BY ALL
UNION ALL SELECT 'gpu busy', sum(dur) FROM interval_merge(gpu_ops, per := gpu)
UNION ALL SELECT 'comm', sum(comm_ns) FROM summary
UNION ALL SELECT 'overlap', sum(overlap_ns) FROM summary
UNION ALL SELECT 'exposed', sum(exposed_ns) FROM summary;

SELECT what, duck.ns AS duckdb_ns, tp.ns AS perfettosql_ns,
       duck.ns = tp.ns AS agree
FROM duck FULL JOIN tp USING (what) ORDER BY what;
