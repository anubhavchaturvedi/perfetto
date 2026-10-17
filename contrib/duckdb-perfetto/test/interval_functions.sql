-- DuckDB-level tests for the interval_* table functions, on small tables
-- whose answers are worked out by hand. Run by run_sql_tests.sh, after
-- prelude.sql.

-- Lane-tagged intervals. Lane NULL is a lane of its own.
CREATE TABLE a AS SELECT * FROM (VALUES
  ('a1', 0, 10, 1),
  ('a2', 5, 10, 1),     -- overlaps a1
  ('a3', 15, 5, 1),     -- touches a2's end
  ('a4', 30, 0, 1),     -- an isolated point
  ('a5', 40, 10, 2),
  ('a6', 50, 0, 2),     -- a point touching a5's end
  ('a7', 0, 7, NULL),
  ('a8', 3, 2, NULL),   -- nested in a7
  ('a9', NULL, 5, 1)    -- covers no time
) v(name, ts, dur, lane);

CREATE TABLE b AS SELECT * FROM (VALUES
  (2, 4, 1, 'b1'),      -- [2, 6)
  (12, 5, 1, 'b2'),     -- [12, 17)
  (45, 0, 2, 'b3'),     -- a point: removes nothing
  (0, 100, 3, 'b4')     -- lane 3 only exists here
) v(ts, dur, lane, tag);

-- interval_merge ------------------------------------------------------------

SELECT check_eq('merge per lane',
  (SELECT string_agg(s(lane) || ':' || ts || '+' || dur || '#' || row_count, ' '
                     ORDER BY lane NULLS LAST, ts)
   FROM interval_merge(a, per := lane)),
  '1:0+20#3 1:30+0#1 2:40+10#2 NULL:0+7#2');

SELECT check_eq('merge without per',
  (SELECT string_agg(ts || '+' || dur || '#' || row_count, ' ' ORDER BY ts)
   FROM interval_merge('a')),
  '0+20#5 30+0#1 40+10#2');

SELECT check_eq('merge open intervals closed at open_end',
  (SELECT string_agg(ts || '+' || dur, ' ' ORDER BY ts)
   FROM interval_merge('SELECT 90 AS ts, -1 AS dur UNION ALL SELECT 50, 10',
                       open_end := 100)),
  '50+10 90+10');

SELECT check_eq('merge of an empty relation',
  (SELECT s(count(*)) FROM interval_merge('SELECT * FROM a WHERE false')), '0');

SELECT check_eq('merge of a CTE, a query string and a list of PER columns',
  (WITH k AS (SELECT * FROM a WHERE lane = 2)
   SELECT string_agg(ts || '+' || dur, ' ') FROM interval_merge(k)) || ' / ' ||
  (SELECT string_agg(ts || '+' || dur, ' ' ORDER BY ts)
   FROM interval_merge('FROM a WHERE lane = 1', per := [lane, name])
   WHERE name = 'a2'),
  '40+10 / 5+10');

-- interval_gaps -------------------------------------------------------------

SELECT check_eq('gaps per lane',
  (SELECT string_agg(lane || ':' || ts || '+' || dur, ' ' ORDER BY lane, ts)
   FROM interval_gaps(a, per := [lane])),
  '1:20+10');

-- interval_subtract ---------------------------------------------------------

SELECT check_eq('subtract per lane keeps the payload',
  (SELECT string_agg(name || ':' || ts || '+' || dur, ' ' ORDER BY name, ts)
   FROM interval_subtract(a, b, per := lane)),
  'a1:0+2 a1:6+4 a2:6+6 a3:17+3 a4:30+0 a5:40+10 a6:50+0 a7:0+7 a8:3+2');

SELECT check_eq('subtract without per, operand as a query string',
  (SELECT string_agg(name || ':' || ts || '+' || dur, ' ' ORDER BY name, ts)
   FROM interval_subtract(a, 'SELECT * FROM b WHERE lane < 3')),
  'a1:0+2 a1:6+4 a2:6+6 a3:17+3 a4:30+0 a5:40+10 a6:50+0 a7:0+2 a7:6+1');

-- interval_fill_gaps --------------------------------------------------------

CREATE TABLE st AS SELECT * FROM (VALUES
  (1, 10, 5, 7), (1, 30, -1, 8), (2, 0, 3, 9)) v(cpu, ts, dur, mw);
CREATE TABLE bounds AS SELECT 0 AS ts, 50 AS dur, 0 AS mw, true AS filled;
CREATE TABLE cpu_bounds AS SELECT * FROM (VALUES (1, 0, 20), (3, 0, 10))
  v(cpu, ts, dur);

SELECT check_eq('fill_gaps: one background for every lane',
  (SELECT string_agg(cpu || ':' || ts || '+' || dur || ':' || mw || ':' ||
                     s(filled), ' ' ORDER BY cpu, ts)
   FROM interval_fill_gaps(st, bounds, per := cpu)),
  '1:0+10:0:true 1:10+5:7:NULL 1:15+15:0:true 1:30+-1:8:NULL ' ||
  '2:0+3:9:NULL 2:3+47:0:true');

SELECT check_eq('fill_gaps: lanes from the background',
  (SELECT string_agg(cpu || ':' || ts || '+' || dur || ':' || s(mw), ' '
                     ORDER BY cpu, ts)
   FROM interval_fill_gaps(st, cpu_bounds, per := cpu)),
  '1:0+10:NULL 1:10+5:7 1:15+5:NULL 1:30+-1:8 2:0+3:9 3:0+10:NULL');

-- interval_split ------------------------------------------------------------

CREATE TABLE ss AS SELECT 's1' AS name, 0 AS ts, 100 AS dur, 1 AS utid;
CREATE TABLE thread_state AS SELECT * FROM (VALUES
  (10, 20, 1, 'R'), (30, 10, 1, 'S'), (95, 20, 1, 'D'), (0, 50, 2, 'X'))
  v(ts, dur, utid, state);

SELECT check_eq('split attaches the operand as a struct',
  (SELECT string_agg(name || ':' || ts || '+' || dur || ':' || s(st.state), ' '
                     ORDER BY ts) || ' sum=' || sum(dur)
   FROM interval_split(ss, thread_state, per := utid, alias := st)),
  's1:0+10:NULL s1:10+20:R s1:30+10:S s1:40+55:NULL s1:95+5:D sum=100');

SELECT check_eq('split: default alias is the operand name',
  (SELECT string_agg(s(thread_state.state), ' ' ORDER BY ts)
   FROM interval_split(ss, thread_state, per := utid)),
  'NULL R S NULL D');

-- interval_join -------------------------------------------------------------

SELECT check_eq('join overlapping',
  (SELECT string_agg(name || ':' || b.tag, ' ' ORDER BY name, b.tag)
   FROM interval_join(a, b, per := lane)),
  'a1:b1 a2:b1 a2:b2 a3:b2 a5:b3');

SELECT check_eq('left join keeps unmatched and NULL rows',
  (SELECT string_agg(name || ':' || s(op.tag), ' ' ORDER BY name, op.tag)
   FROM interval_join(a, b, per := lane, left := true, alias := op)),
  'a1:b1 a2:b1 a2:b2 a3:b2 a4:NULL a5:b3 a6:NULL a7:NULL a8:NULL a9:NULL');

SELECT check_eq('join covering begin',
  (SELECT string_agg(name || ':' || b.tag, ' ' ORDER BY name, b.tag)
   FROM interval_join(a, b, per := lane, relationship := 'covering begin')),
  'a2:b1 a3:b2');

SELECT check_eq('join covering end',
  (SELECT string_agg(name || ':' || b.tag, ' ' ORDER BY name, b.tag)
   FROM interval_join(a, b, per := lane, relationship := 'COVERING_END')),
  'a2:b2');

SELECT check_eq('join within bounds',
  (SELECT string_agg(name || ':' || b.tag, ' ' ORDER BY name, b.tag)
   FROM interval_join(a, b, per := lane, relationship := 'within bounds')),
  'a1:b1 a5:b3');

SELECT check_eq('join covering bounds, no PER',
  (SELECT string_agg(name || ':' || b.tag, ' ' ORDER BY name, b.tag)
   FROM interval_join(a, b, relationship := 'covering bounds')),
  'a1:b4 a2:b4 a3:b4 a4:b4 a5:b4 a6:b4 a7:b4 a8:b1 a8:b4');

-- interval_exposure ---------------------------------------------------------

SELECT check_eq('exposure: one row per input row',
  (SELECT string_agg(name || ':' || s(overlap_dur) || ':' || s(exposed_dur),
                     ' ' ORDER BY name)
   FROM interval_exposure(a, b, per := lane)),
  'a1:4:6 a2:4:6 a3:2:3 a4:0:0 a5:0:10 a6:0:0 a7:0:7 a8:0:2 a9:NULL:NULL');

SELECT check_eq('exposure counts overlapping operand rows once',
  (SELECT overlap_dur || ':' || exposed_dur FROM interval_exposure(
     'SELECT 0 AS ts, 10 AS dur',
     'SELECT * FROM (VALUES (2, 4), (3, 5)) v(ts, dur)')),
  '6:4');

-- interval_clip -------------------------------------------------------------

SELECT check_eq('clip to a window',
  (SELECT string_agg(name || ':' || ts || '+' || dur, ' ' ORDER BY name)
   FROM interval_clip(a, 5, 45)),
  'a1:5+5 a2:5+10 a3:15+5 a4:30+0 a5:40+5 a7:5+2');

-- interval_quantize ---------------------------------------------------------

SELECT check_eq('quantize',
  (SELECT string_agg(name || ':' || ts || '+' || dur || '@' || bucket, ' '
                     ORDER BY name, ts)
   FROM interval_quantize(a, 10)),
  'a1:0+10@0 a2:5+5@0 a2:10+5@1 a3:15+5@1 a4:30+0@3 a5:40+10@4 a6:50+0@5 ' ||
  'a7:0+7@0 a8:3+2@0');

SELECT check_eq('quantize with an origin',
  (SELECT string_agg(ts || '+' || dur || '@' || bucket, ' ' ORDER BY ts)
   FROM interval_quantize('SELECT 0 AS ts, 10 AS dur', 10, origin := 3)),
  '0+3@-1 3+7@0');

-- interval_set_bounds -------------------------------------------------------

CREATE TABLE binder AS SELECT * FROM (VALUES ('x', 10, 25), ('y', 5, 5))
  v(name, client_ts, server_ts);

SELECT check_eq('set_bounds adds ts and dur',
  (SELECT string_agg(name || ':' || ts || '+' || dur, ' ' ORDER BY name)
   FROM interval_set_bounds(binder, begin_at := client_ts,
                            end_at := server_ts)),
  'x:10+15 y:5+0');

SELECT check_eq('set_bounds replaces ts and dur from expressions',
  (SELECT string_agg(name || ':' || ts || '+' || dur, ' ')
   FROM interval_set_bounds('SELECT * FROM a WHERE name = ''a1''',
                            begin_at := 'ts + 1', dur := 'dur * 2')),
  'a1:1+20');

-- Other inputs --------------------------------------------------------------

COPY (SELECT * FROM a) TO '__TMP__/a.parquet' (FORMAT parquet);
SELECT check_eq('a parquet file',
  (SELECT string_agg(s(lane) || ':' || ts || '+' || dur, ' '
                     ORDER BY lane NULLS LAST, ts)
   FROM interval_merge('__TMP__/a.parquet', per := lane)),
  '1:0+20 1:30+0 2:40+10 NULL:0+7');

SELECT check_eq('debug_sql shows the generated query',
  (SELECT CAST(sql LIKE '%__perfetto_interval_kernel%' AS VARCHAR)
   FROM interval_merge(a, debug_sql := true)),
  'true');
