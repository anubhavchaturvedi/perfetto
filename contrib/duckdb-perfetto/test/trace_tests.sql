-- Tests which need a trace: SQL_PACKAGE / sql_package, and the interval
-- functions over TP tables and PERFETTO blocks. tiny_trace.json has a "main"
-- thread and two "stream" threads standing in for CUDA streams (times are in
-- us there, ns here):
--   stream 7: gemm [0, 40), gemm [50, 80), Memcpy [85, 90)
--   stream 8: ncclDevKernel [30, 70), ncclDevKernel [90, 110)
-- Lines inside PERFETTO blocks must not end in ';' (CLI statement splitting).

-- SQL_PACKAGE ---------------------------------------------------------------

ATTACH '__TRACE__' AS t (TYPE perfetto, SQL_PACKAGE '__PKG__/acme');

SELECT check_eq('SQL_PACKAGE: a module which includes another one',
  (SELECT thread_name || ':' || slice_count || ':' || total_dur
   FROM PERFETTO(t) (INCLUDE PERFETTO MODULE acme.slices.top; SELECT * FROM acme_top_thread)),
  'main:2:120000');

SELECT check_eq('SQL_PACKAGE: included tables appear in the catalog',
  (SELECT string_agg(thread_name || ':' || slice_count, ' ' ORDER BY thread_name)
   FROM t.acme_thread_stats),
  'main:2 stream 7:3 stream 8:2');

SELECT check_eq('SQL_PACKAGE: macros',
  (SELECT s(v) FROM PERFETTO(t) (INCLUDE PERFETTO MODULE acme.macros; SELECT acme_double!(21) AS v)),
  '42');

DETACH t;

ATTACH '__TRACE__' AS t2 (TYPE perfetto,
  SQL_PACKAGE ['__PKG__/acme@com.meta.pytorch']);

SELECT check_eq('SQL_PACKAGE: a LIST, and <dir>@<package name>',
  (SELECT s(n) || ':' || s(v)
   FROM PERFETTO(t2) (INCLUDE PERFETTO MODULE com.meta.pytorch.slices.stats; INCLUDE PERFETTO MODULE com.meta.pytorch.macros; SELECT count(*) AS n, acme_double!(4) AS v FROM acme_thread_stats)),
  '3:8');

DETACH t2;

SELECT check_eq('perfetto_query: sql_package for traces loaded from paths',
  (SELECT string_agg(s(n), ' ')
   FROM perfetto_query(['__TRACE__', '__TRACE__'], 'INCLUDE PERFETTO MODULE acme.slices.stats; SELECT sum(slice_count) AS n FROM acme_thread_stats', sql_package := '__PKG__/acme')),
  '7 7');

-- Interval functions over a trace -------------------------------------------

ATTACH '__TRACE__' AS k (TYPE perfetto);

CREATE TABLE ops AS
SELECT s.ts, s.dur, s.name, th.name AS stream,
       lower(s.name) LIKE '%nccl%' AS is_comm, s.name LIKE 'Memcpy%' AS is_copy
FROM k.slice s
JOIN k.thread_track tt ON s.track_id = tt.id
JOIN k.thread th USING (utid)
WHERE th.name LIKE 'stream%';
CREATE VIEW comm AS FROM ops WHERE is_comm;
CREATE VIEW compute AS FROM ops WHERE NOT is_comm AND NOT is_copy;

SELECT check_eq('per-stream busy time',
  (SELECT string_agg(stream || ':' || busy, ' ' ORDER BY stream)
   FROM (SELECT stream, sum(dur) AS busy FROM interval_merge(ops, per := stream)
         GROUP BY 1)),
  'stream 7:75000 stream 8:60000');

SELECT check_eq('compute/communication overlap, exposed communication',
  (SELECT sum(overlap_dur) || ':' || sum(exposed_dur)
   FROM interval_exposure(comm, compute)),
  '30000:30000');

SELECT check_eq('a TP table by its qualified name',
  (SELECT s(count(*)) FROM interval_merge(k.slice, per := track_id)),
  '6');

SELECT check_eq('a PERFETTO block through a CTE',
  (WITH g AS (PERFETTO(k) (SELECT ts, dur, track_id FROM slice))
   SELECT s(count(*)) FROM interval_merge(g, per := track_id)),
  '6');
