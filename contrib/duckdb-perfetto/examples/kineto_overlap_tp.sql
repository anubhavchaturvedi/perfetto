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

-- The totals of kineto_overlap.{sql,py} in pure PerfettoSQL, with the pipe
-- operators TP already has (INTERVAL FLATTEN, INTERVAL INTERSECTION OF) and
-- no DuckDB: the cross-check. Runs in trace_processor_shell
--
--   trace_processor_shell query -f kineto_overlap_tp.sql <trace>
--
-- and, as a PERFETTO block, from kineto_overlap.py.

PERFETTO PRAGMA pipelines = 1;
CREATE OR REPLACE PERFETTO TABLE xc_ops AS
  SELECT s.ts, s.dur, extract_arg(tr.dimension_arg_set_id, 'ugpu') AS gpu,
    CAST(substr(tr.name, 10) AS INT) AS stream,
    CASE WHEN lower(s.name) GLOB '*nccl*' THEN 'comm'
      WHEN s.name GLOB 'Memcpy*' OR s.name GLOB 'Memset*' THEN 'copy'
      ELSE 'compute' END AS kind
  FROM slice s JOIN track tr ON s.track_id = tr.id
  WHERE tr.type = 'gpu_render_stage' AND tr.name GLOB 'Channel #*';
CREATE OR REPLACE PERFETTO TABLE xc_stream AS
  FROM xc_ops |> INTERVAL FLATTEN PER gpu, stream AGGREGATE COUNT(*) AS n;
CREATE OR REPLACE PERFETTO TABLE xc_gpu AS
  FROM xc_ops |> INTERVAL FLATTEN PER gpu AGGREGATE COUNT(*) AS n;
CREATE OR REPLACE PERFETTO TABLE xc_comm AS
  FROM (SELECT ts, dur, gpu FROM xc_ops WHERE kind = 'comm')
  |> INTERVAL FLATTEN PER gpu AGGREGATE COUNT(*) AS n;
CREATE OR REPLACE PERFETTO TABLE xc_compute AS
  FROM (SELECT ts, dur, gpu FROM xc_ops WHERE kind = 'compute')
  |> INTERVAL FLATTEN PER gpu AGGREGATE COUNT(*) AS n;
CREATE OR REPLACE PERFETTO TABLE xc_overlap AS
  INTERVAL INTERSECTION OF (xc_comm AS c, xc_compute AS k) PER gpu
  |> AGGREGATE SUM(dur) AS overlap_ns GROUP BY gpu;
SELECT 'stream ' || stream AS what, SUM(dur) AS ns FROM xc_stream GROUP BY gpu, stream
UNION ALL SELECT 'gpu busy', SUM(dur) FROM xc_gpu
UNION ALL SELECT 'comm', SUM(dur) FROM xc_comm
UNION ALL SELECT 'overlap', SUM(overlap_ns) FROM xc_overlap
UNION ALL SELECT 'exposed', (SELECT SUM(dur) FROM xc_comm) - (SELECT SUM(overlap_ns) FROM xc_overlap);
