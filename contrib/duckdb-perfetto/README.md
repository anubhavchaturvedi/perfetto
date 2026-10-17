# PerfettoSQL inside DuckDB (prototype)

**Status:** Exploratory prototype. Not yet a proposed contrib/ project: it has
no OWNERS.github and has not gone through the contrib/ proposal process (see
[../README.md](../README.md)).

A DuckDB extension which embeds Perfetto's Trace Processor (TP) so that traces
can be attached to a DuckDB database, and so that **PerfettoSQL can be written
inline in DuckDB queries, without string literals, and executed entirely
inside TP**.

```sql
LOAD 'perfetto';  -- or LOAD '/abs/path/to/perfetto.duckdb_extension'

-- A trace is a read-only database. Every TP table is a DuckDB table.
ATTACH 'trace.pftrace' AS t (TYPE perfetto);

SELECT p.name, sum(s.dur) AS cpu_ns
FROM t.sched s JOIN t.thread USING (utid) JOIN t.process p USING (upid)
GROUP BY 1 ORDER BY 2 DESC;

-- A PERFETTO block is PerfettoSQL: stdlib, INCLUDE, macros, SPAN_JOIN, and
-- multiple statements all work. The whole block runs in TP; DuckDB only
-- sees the result rows, and treats the block like any other table.
SELECT s.package, s.dur, d.build_id
FROM PERFETTO(t) (
  INCLUDE PERFETTO MODULE android.startup.startups;
  SELECT * FROM android_startups WHERE dur > 100e6
) AS s
JOIN device_metadata d USING (package);

-- Anything TP creates becomes visible in the catalog.
SELECT count(*) FROM t.android_startups;

-- Materialize: plain DuckDB.
CREATE TABLE slices AS FROM t.slice;
COPY (PERFETTO(t) (SELECT * FROM thread_state)) TO 'thread_state.parquet';

-- Bulk: one PerfettoSQL query over many traces, in parallel.
SELECT trace, count(*), max(dur)
FROM perfetto_query('traces/*.pftrace', 'SELECT dur FROM slice WHERE depth = 0')
GROUP BY trace;
```

## Design

### Why a block, not implicit routing

The goal is to write PerfettoSQL directly, with no string subqueries. There
are two ways to get there:

1. **Implicit routing.** Write DuckDB SQL, and have the extension work out
   which parts can run in TP and send them there.
2. **An explicit block.** `PERFETTO(t) ( ... )` marks a region that is
   PerfettoSQL.

This prototype does (2). Implicit routing is unsafe: the same SQL text means
different things in DuckDB and SQLite (integer `/`, `||` with NULL, NULL
ordering, collation, and `LIKE` case sensitivity), so silently moving part of a
query between engines changes results. PerfettoSQL is also heading away from
DuckDB's grammar (pipe syntax, interval/tree/graph operators; see RFC-0040),
so DuckDB can't even parse it. A block boundary makes it obvious which
semantics apply, while everything inside the block is full, unrestricted
PerfettoSQL.

Composition comes free because a block is just a table expression. It works
with joins, CTEs, `CREATE TABLE AS`, `COPY`, `INSERT`, subqueries, and every
DuckDB client.

### How the syntax is implemented

DuckDB has two parser hooks for extensions:

* `parse_function` (fallback) only runs for statements DuckDB's parser
  rejects, **after** DuckDB has split the input on `;`. A block containing
  `INCLUDE ...; SELECT ...` would be cut in half before the extension saw it.
* `parser_override` sees the whole query string first and returns parsed
  statements. **This is the one we use.** It is also how DuckDB's own new
  PEG parser plugs in.

In [`block_rewriter.cc`](src/block_rewriter.cc), each dialect is handled by
its own engine's front end:

* **The outer query** is tokenized by **DuckDB's own tokenizer**
  (`Parser::Tokenize`). This finds each `PERFETTO(alias) (` opener and the
  token before it, with DuckDB's exact rules for `$$` strings, nested
  comments, and `E''` strings.
* **The block body** is tokenized by **syntaqlite's PerfettoSQL tokenizer**
  to find the matching close paren. syntaqlite is TP's own SQL front end, so
  `;`, strings, comments, quoted identifiers, and macro calls inside the block
  lex exactly as TP will lex them. Outer tokenization resumes after each
  block's closing paren.
* **The body is then parsed** with **syntaqlite's PerfettoSQL parser**, from
  a pool of parsers reused across queries and connections.
  Syntax errors are reported against the original query, with DuckDB's usual
  caret, before anything runs:

  ```
  Parser Error: PerfettoSQL syntax error in PERFETTO block: syntax error near 'FROM'

  LINE 1: SELECT 1 FROM PERFETTO(t) (SELECT 1; SELECT FROM slice);
                                                      ^
  ```

Each block is then replaced with a `perfetto_query('alias', '<body>')` table
function call, and the rewritten text is handed back to DuckDB's parser.
Queries without blocks fall straight through, untouched.

| Form | Rewritten to |
| ---- | ------------ |
| `FROM PERFETTO(t) (...)`, `JOIN PERFETTO(t) (...)` | `perfetto_query('t', '...')` |
| `PERFETTO(t) (...)` as a statement, CTE body, `AS ...` | `FROM perfetto_query('t', '...')` |
| `PERFETTO (...)` (no alias) | uses the only attached perfetto database |

The rewriter takes the outer tokenizer as a parameter and is unit tested
([`block_rewriter_unittest.cc`](src/block_rewriter_unittest.cc)), with
syntaqlite standing in for DuckDB's tokenizer.

### Does DuckDB 2 solve this?

Partly, eventually. DuckDB's roadmap lists "using the new PEG parser by
default". That parser is runtime-extensible: grammar rules live in `.gram`
files (`extension/autocomplete/grammar/` in v1.5.6), and it is installed
through the same `parser_override` hook we use. Once it is the default and
extensions can contribute rules, `PERFETTO(alias) ( <opaque balanced body> )`
becomes a real grammar rule instead of a lexical pre-pass. The benefits:

* the whole query is parsed in one pass;
* the CLI's statement splitter can understand blocks (see Limitations);
* DuckDB's syntax highlighting and autocomplete can understand blocks.

As of v1.5.6 the PEG parser is opt-in and has no public API for extensions to
add grammar, so the rewriter stays isolated in one file that can be swapped
out later.

### Catalog: `ATTACH ... (TYPE perfetto)`

A DuckDB `StorageExtension` (the mechanism used by the sqlite and postgres
scanners) provides a read-only catalog with one schema, `main`:

* **Lazy table entries.** Table entries are created on lookup from TP's own
  catalog (`pragma_table_info`). So tables and views a block creates (via
  `INCLUDE PERFETTO MODULE` or `CREATE PERFETTO TABLE`) show up as `t.<name>`
  with no refresh step.
* **Lifetime.** The trace's lifetime is tied to the attached database:
  `DETACH t` frees it.
* **Extra PerfettoSQL packages.** `SQL_PACKAGE` registers directories of
  PerfettoSQL modules (`TraceProcessor::RegisterSqlPackage`) before the trace
  loads, so private modules work without being upstreamed. For example, with
  a (hypothetical) `/src/kineto_sql/gpu/kernels.sql`:

  ```sql
  ATTACH 'rank-0.pt.trace.pftrace' AS t
    (TYPE perfetto, SQL_PACKAGE '/src/kineto_sql@com.meta.pytorch');
  FROM PERFETTO(t) (
    INCLUDE PERFETTO MODULE com.meta.pytorch.gpu.kernels;
    SELECT * FROM pytorch_gpu_kernels
  );
  ```

  The rules are those of `trace_processor_shell --add-sql-package`, whose
  code is reused: `<dir>[@<package name>]`, the name defaulting to the
  directory's; every `.sql` file below it is a module named
  `<package>.<path with / as .>`; package names may be dotted. A `LIST`
  registers several packages. `perfetto_query(paths, sql, sql_package :=
  ...)` does the same for traces loaded from paths.
* **Projection pushdown.** TP is only asked for the columns DuckDB needs.
* **Filter pushdown** via `pushdown_complex_filter`. Column-vs-constant
  comparisons (`= != < <= > >=`), `IS [NOT] NULL` and `IN (...)` on columns
  become a PerfettoSQL `WHERE`, so TP's indexes and dataframe planner apply.
  Anything else stays in DuckDB, so pushdown never changes results.
  `EXPLAIN` shows what was pushed:

  ```
  PERFETTO_SCAN
    Table: slice
    PerfettoSQL Filters: "dur" > 1000000
  ```

### Execution

`perfetto_query(source, sql)` runs the SQL with `TraceProcessor::ExecuteQuery`
and streams the rows into DuckDB vectors. `source` can be:

* an attached alias, which runs against the resident TP; or
* a path, a glob, or a `LIST` of those. Each worker thread loads its own TP
  instance, so many traces are processed in parallel, and a `trace` column
  identifies each row's origin.

Result types come from the declared types for tables. For arbitrary queries
they are inferred from the first 2048 rows (see Limitations).

### Build

* **Toolchain.** The extension builds with the standard Perfetto GN
  toolchain through a separate dotfile ([`.gn`](.gn)) rooted at this
  directory, so contrib/ never touches the root `BUILD.gn`. `BUILD.gn` uses
  relative labels. The one exception is three `configs -=` entries, which must
  match the absolute labels in `default_configs`.
* **No DuckDB build needed.** It compiles against DuckDB's headers (a sparse
  checkout of the matching tag) and links with `-undefined dynamic_lookup`.
  The released DuckDB binaries export their C++ symbols, which is also how
  official extensions load.
* **Exceptions and RTTI are confined to the glue files.** DuckDB's C++ API
  needs both, and only [`perfetto_extension.cc`](src/perfetto_extension.cc)
  and [`interval_functions.cc`](src/interval_functions.cc) include DuckDB
  headers, so only they are built with them. Everything that touches TP
  ([`trace_session.cc`](src/trace_session.cc)) and the interval kernels
  ([`interval_ops.cc`](src/interval_ops.cc)) use Perfetto's usual flags, and
  errors cross the boundary as `base::Status`.

```sh
contrib/duckdb-perfetto/build.sh                  # extension + unit tests
out/duckdb_ext/perfetto_duckdb_unittests          # rewriter + interval kernels
contrib/duckdb-perfetto/test/run_sql_tests.sh     # SQL tests via the DuckDB CLI
duckdb -unsigned -c "LOAD '$PWD/out/duckdb_ext/perfetto.duckdb_extension'; ..."
```

`build.sh` pins DuckDB `v1.5.6`. On macOS, `LOAD` needs an absolute path.

## Primitives: interval operators over any relation

PerfettoSQL Next (RFC-0040, operators in RFC-0041) adds interval, tree and
graph pipe operators to TP. Some have landed (`INTERVAL INTERSECTION OF`,
`INTERVAL FLATTEN`, `TREE ACCUMULATE`, `AGGREGATE`, behind
`PERFETTO PRAGMA pipelines = 1`) and already work inside PERFETTO blocks (see
Results). The RFC-0041 interval operators which have not landed are
available today as DuckDB table functions, `interval_*`. They are native
(C++ kernels, [`interval_ops.cc`](src/interval_ops.cc)) and work on any
DuckDB relation: attached TP tables, PERFETTO blocks, Polars DataFrames and
Arrow tables, parquet and CSV files, DuckDB tables, views and CTEs.

```sql
-- Busy time per CUDA stream.
SELECT stream, sum(dur) AS busy_ns
FROM interval_merge(gpu_ops, per := stream) GROUP BY stream;

-- Communication time which no compute kernel hides, per GPU.
SELECT gpu, sum(exposed_dur) AS exposed_ns
FROM interval_exposure(comm, compute, per := gpu) GROUP BY gpu;
```

### Conventions

* **Relations.** An interval relation has integer `ts` and `dur` columns, in
  ns; other columns are payload. A relation argument is a name: a table,
  view, CTE, DataFrame (Python variable), attached TP table (`t.slice`), or a
  file DuckDB can scan (`'kernels.parquet'`). Bare identifiers need no quotes.
  It can also be a query string (`'SELECT ...'`, `'FROM ...'`, `'WITH ...'`).
  Subqueries are not accepted (see How it works); use a CTE instead:
  `WITH k AS (PERFETTO(t) (...)) SELECT * FROM interval_merge(k)`.
* **Lanes.** `per := col` or `per := [col1, col2]` is RFC-0041's `PER`: rows
  only interact with rows of the same lane. As in `GROUP BY` (and TP's
  `INTERSECTION OF` / `FLATTEN`), NULL keys form a lane of their own. Without
  `per` the whole relation is one lane.
* **Bounds.** Intervals are half-open, `[ts, ts + dur)`: intervals which only
  touch (`[0, 10)` and `[10, 20)`) do not overlap (`interval_merge` still
  coalesces them, as RFC-0041's `MERGE OVERLAPPING` does). `dur = 0` is a
  point: it is inside `[a, b)` if `a <= ts < b`, and overlaps a point at the
  same `ts`. Points have no extent, so they never cover, cut or remove time.
* **NULL and open rows.** A row with a NULL `ts` or `dur` covers no time:
  row-preserving operators (`interval_exposure`, `interval_join(...,
  left := true)`) keep it with NULL results, the others drop it. `dur = -1`,
  a slice still open when the trace ended, is an error unless
  `open_end := <ts>` closes such rows there (`interval_fill_gaps` closes them
  at the end of the background, as RFC-0041 says). Any other negative `dur`
  is an error. `ts` may be negative.
* **Output.** Row order is unspecified. Operators which cut rows return the
  input's columns with new `ts` / `dur`, one row per piece. Operand columns
  are attached as one STRUCT column named `alias` (default: the operand's
  name), so RFC-0041's `alias.col` reads the same: `st.state`. Where no
  operand row matches, its fields are NULL.
* `debug_sql := true` returns the generated query instead of running it.

### Functions

| Function | Returns | RFC-0041 operator | In TP today |
| -------- | ------- | ----------------- | ----------- |
| `interval_merge(rel [, per, open_end])` | `per..., ts, dur, row_count` | `INTERVAL MERGE OVERLAPPING [PER] AGGREGATE COUNT(*)` | no |
| `interval_gaps(rel [, per, open_end])` | `per..., ts, dur` | `INTERVAL SUBTRACT` of the lane from its extent | no |
| `interval_fill_gaps(rel, background [, per, open_end])` | `rel` rows + fillers | `INTERVAL FILL GAPS WITH` | parses, "not supported yet" |
| `interval_subtract(rel, operand [, per, open_end])` | `rel` columns, per piece | `INTERVAL SUBTRACT` | no |
| `interval_split(rel, operand [, per, alias, open_end])` | `rel` columns per piece + `alias` | `INTERVAL SPLIT operand AS alias` | no |
| `interval_join(rel, operand [, per, relationship, left, alias, open_end])` | `rel` columns + `alias`, per match | `[LEFT] INTERVAL JOIN operand AS alias <relationship>` | no |
| `interval_exposure(rel, operand [, per, open_end])` | `rel` columns + `overlap_dur`, `exposed_dur` | none: `SPLIT` against the merged operand + `AGGREGATE` per row | no |
| `interval_clip(rel, window_start, window_end [, open_end])` | `rel` columns, clipped | §9 "mid-pipe clip" candidate; `INTERSECTION OF` with a window | as `INTERSECTION OF` |
| `interval_set_bounds(rel, begin_at := e, end_at := e \| dur := e)` | `rel` with new `ts`, `dur` | `INTERVAL SET BOUNDS BEGIN AT e END AT e \| FOR e` | no |
| `interval_quantize(rel, width [, origin, open_end])` | `rel` columns per piece + `bucket` | `INTERVAL QUANTIZE width ALIGNED TO origin` | no |

* **`interval_merge`** returns the coverage of each lane as maximal islands.
  Overlapping and touching rows coalesce; a point inside or touching an island
  joins it, an isolated point is an island with `dur = 0`. `row_count` is the
  number of rows merged. For rows which do not overlap within a lane (e.g.
  thread states), `per := [utid, state]` is `MERGE CONSECUTIVE BY state PER
  utid`.
* **`interval_gaps`** returns the holes between consecutive islands of a lane,
  nothing before the first or after the last. For gaps against explicit
  bounds, subtract instead: `interval_subtract(bounds, rel, per := ...)`.
* **`interval_subtract`** returns the pieces of each row which the operand's
  coverage leaves. Operand points remove nothing; a point row survives unless
  an operand interval contains it; rows in lanes without operand rows pass
  through.
* **`interval_fill_gaps`** follows RFC-0041: input rows pass through unchanged
  (even `dur = -1` ones), and each span of `background` the input does not
  cover becomes a filler row with the background row's payload, matched by
  name (`UNION ALL BY NAME`: input-only columns are NULL on fillers,
  background-only ones NULL on input rows). If the background has all the
  `per` columns, the lanes are its own, so lanes without input are filled; if
  it has none, every background row applies to every lane of the input. No
  zero-width fillers; input outside the background passes through. The
  background must not self-overlap within a lane.
* **`interval_split`** cuts each row at the boundaries of the operand rows
  overlapping it and pairs each piece with every operand row covering it, or
  with NULLs. If the operand does not self-overlap, a row's pieces tile it
  exactly. Operand points neither cut nor attach to interval rows; a point row
  pairs with every operand row it overlaps.
* **`interval_join`** attaches each matching operand row, keeping the row's
  bounds; `left := true` keeps unmatched rows. `relationship` (case and `_`
  don't matter) is, for a row `[s, e)` and an operand `[os, oe)`:
  `'overlapping'` (default) — they overlap; `'covering begin'` / `'covering
  end'` — the operand contains the instant `s` / `e`; `'covering bounds'` —
  `os <= s` and `e <= oe`; `'within'` — `s <= os` and `oe <= e` (a point
  operand: `s <= os < e`).
* **`interval_exposure`**: for each row, `overlap_dur`, its overlap with the
  coverage of the operand (overlapping operand rows count once), and
  `exposed_dur = dur - overlap_dur`. Exactly one output row per input row:
  this is the per-target aggregate which a join would multiply. Points get 0.
* **`interval_clip`** clips rows to `[window_start, window_end)`. Rows outside,
  or only touching it, are dropped; points are kept if inside.
* **`interval_set_bounds`** sets `ts` from the `begin_at` expression and
  `dur` from `end_at - begin_at` (an error if negative) or from `dur`;
  expressions are SQL strings over the row (`begin_at := 'client_ts + 10'`;
  a bare column needs no quotes). It adds `ts` / `dur` if the relation lacks
  them.
* **`interval_quantize`** cuts rows at `origin + k * width` and tags each
  piece with `bucket = floor((piece ts - origin) / width)`; a point is one
  piece. `origin` defaults to 0: RFC-0041's default, `TRACE START`, must be
  passed explicitly (`origin := getvariable('trace_start')` after
  `SET VARIABLE trace_start = (SELECT start_ts FROM t.trace_bounds)`).

### From Polars

```python
import duckdb, polars as pl

con = duckdb.connect(config={'allow_unsigned_extensions': 'true'})
con.execute("LOAD '/abs/path/out/duckdb_ext/perfetto.duckdb_extension'")
con.execute("ATTACH 'rank-26.pt.trace.pftrace' AS t (TYPE perfetto)")

# TP -> Polars.
kernels = con.sql("""
  SELECT s.ts, s.dur, s.name FROM t.slice s JOIN t.track tr ON s.track_id = tr.id
  WHERE tr.name LIKE 'Channel #%'""").pl()
# Polars expressions.
is_comm = pl.col('name').str.to_lowercase().str.contains('nccl')
is_copy = pl.col('name').str.starts_with('Memcpy')
comm, compute = kernels.filter(is_comm), kernels.filter(~is_comm & ~is_copy)
# Polars -> interval_* -> Polars. DataFrames are found by variable name.
exposure = con.sql('FROM interval_exposure(comm, compute)').pl()
print(exposure['exposed_dur'].sum())
```

DataFrames are resolved like `FROM df` (DuckDB replacement scans), or by the
name given to `con.register()`. DuckDB's `sum()` of a `BIGINT` is a
`HUGEINT`, which Polars receives as a decimal: cast sums to `BIGINT`.
[`examples/kineto_overlap.py`](examples/kineto_overlap.py) is a full
example.

### How it works

Each function is a `bind_replace` which rewrites the call into a DuckDB
query (`debug_sql := true` shows it):

1. the rows are grouped into lanes with `GROUP BY <per>`, and each lane's
   `(ts, dur, row id)` triples are packed into a `LIST`;
2. `__perfetto_interval_kernel`, a table in-out function, runs one of the
   kernels of [`interval_ops.h`](src/interval_ops.h) on each lane;
3. operators which keep payload join the row ids the kernel returns back to
   the input (materialized, numbered with `row_number()`).

Lanes travel whole because DuckDB runs an in-out function on several threads
with no barrier between them: it never sees all of its input, while these
operators need whole lanes. A lane row is self-contained, so lanes are
processed in parallel. For the same reason relations are passed by name: a
DuckDB table function can only take a subquery as an in-out function.

The kernels have no DuckDB dependency, are unit tested on their own
(including brute-force cross-checks of every relationship), and sort-and-sweep:
`O(n log n)` per lane plus output, with joins output-sensitive except for
`'covering bounds'` and `'within'`, which filter candidates.

### Moving to TP's operators

Each function corresponds to one RFC-0041 operator (table above), with the
same argument order, `PER` lanes and payload rules, so an analysis over TP
tables can move into a PERFETTO block as the operators land in TP:

```sql
-- Today, on TP tables (or anything else):
SELECT * FROM interval_subtract(t.comm, t.compute, per := gpu);
-- Once INTERVAL SUBTRACT lands in TP (RFC-0041 syntax):
FROM PERFETTO(t) (FROM comm |> INTERVAL SUBTRACT compute PER gpu);
```

Relations which live in DuckDB (DataFrames, files) cannot be passed into a
block yet (see Possible next steps), so for them the functions stay the way
in.

Where RFC-0041 is silent, the functions follow TP's landed operators
(half-open bounds, points, NULL `PER` keys). Known differences: TP's
`FLATTEN` and `INTERSECTION OF` reject negative `ts` and `dur = -1`, while
the functions accept negative `ts` and close open rows with `open_end`;
`interval_merge` only aggregates `COUNT(*)` (RFC-0041 takes an `AGGREGATE`
clause: for other aggregates, attach the rows to their islands with
`interval_join(islands, rel, relationship := 'within')` and group by
island); `interval_quantize` defaults to `ALIGNED TO ZERO`.

## Results (M-series Mac, 8 cores, release build, DuckDB 1.5.6 CLI and Python)

Verified working in the stock DuckDB CLI and the `duckdb` Python package:

* `ATTACH`, then joins across `t.*` tables;
* multi-statement blocks with stdlib `INCLUDE`;
* blocks joined with DuckDB tables, inside CTEs, and in `CREATE TABLE AS` /
  `COPY`;
* `INCLUDE`d tables appearing in the catalog;
* `DETACH`;
* read-only enforcement;
* TP error messages surfacing as DuckDB errors.

| Workload | Time |
| -------- | ---- |
| `ATTACH` a 15 MB Android trace | 150 ms |
| `CREATE TABLE AS FROM t.slice` (123k rows x 15 cols) | 42 ms |
| `CREATE TABLE AS SELECT ts, dur FROM t.slice` (projection pushdown) | 6 ms |
| `t.slice WHERE track_id = X`, filter pushed into TP | 0.19 ms |
| same predicate, not pushable (`abs(track_id) = X`) | 5.3 ms |
| 5-predicate filter on `t.slice`, pushed / not pushed | 0.33 / 15.1 ms |
| same as a `PERFETTO(t) (...)` block | 0.12 ms |
| `perfetto_query` over 16 copies of the trace (2M rows), 1 / 10 threads | 2.67 / 0.74 s |

Pushed and non-pushed filters return identical results.

### Kineto traces, and the interval functions

Taken on an M1 Max (10 cores) while four other builds shared it (load
average ~300): wall times are inflated and noisy. Instructions retired
(`/usr/bin/time -l`) are the stable measure.

The prototype as it was (first three commits), on PyTorch profiler (Kineto)
traces:

* `ATTACH` of the 41.5 MB rank-26 pftrace (2.0M slices, 102 tables): 31-47 s
  wall, 7 s CPU; `trace_processor_shell` takes 23 s wall, 8.5 s CPU. Joins
  across `t.*`, filter pushdown, blocks with a stdlib `INCLUDE`
  (`slices.with_context`) and `INCLUDE`d tables in the catalog all work, from
  the CLI and from Python.
* `perfetto_query` over a corpus of six Kineto traces: 50 s for the pftraces,
  54 s for their `json.gz` versions, one TP per thread.
* TP's pipe operators inside blocks, after `PERFETTO PRAGMA pipelines = 1`
  (once per attached trace: the setting persists):

  | Works | Fails |
  | ----- | ----- |
  | `FROM` and `INTERVAL INTERSECTION OF ... [PER]` sources | without the pragma: "Pipelines are not enabled" |
  | `SELECT`, `EXTEND`, `DROP`, `RENAME`, `SET`, `AS` | `\|> WHERE`, `\|> DISTINCT`, `\|> INTERVAL FILL GAPS WITH`: "not supported yet" |
  | `ORDER BY` numeric columns | `ORDER BY` a string column, `AGGREGATE AVG`: "not supported yet" |
  | `AGGREGATE COUNT(*), SUM, MIN, MAX ... GROUP BY` | `INTERVAL MERGE / SUBTRACT / SPLIT / JOIN` (RFC only): syntax error from the block's parser |
  | `INTERVAL FLATTEN [PER] AGGREGATE COUNT(*), SUM, MAX` | a pipeline as a subquery: syntax error |
  | `TREE ACCUMULATE UP / DOWN SUM` | `dur = -1` rows into `FLATTEN`: "a row's ts or dur is below zero" |
  | `CREATE PERFETTO TABLE ... AS <pipeline>` | the same through Python's `duckdb.sql()`: "already exists", as the block runs twice; `con.execute()` or `CREATE OR REPLACE` work |

The demo, [`examples/kineto_overlap.sql`](examples/kineto_overlap.sql) (CLI)
and [`examples/kineto_overlap.py`](examples/kineto_overlap.py) (Polars), on
the rank-26 trace:

| | |
| - | - |
| GPU activity | 236,163 rows on 6 streams: 97,067 compute kernels (1.22 s), 2,680 NCCL kernels (17.26 s), 136,416 memory copies (2.40 s) |
| Busy per stream | stream 2: 10.71 s (43% of its span), 4: 7.99 s, 12: 1.93 s, 10: 0.23 s, 6: 16 ms, 13: 0.19 ms |
| GPU busy (any stream) | 20.00 s |
| Communication | 17.26 s, of which 0.63 s overlaps compute |
| Exposed communication | 16.63 s (96.4%): AllReduce 7.11 s, SendRecv 6.61 s, AllGather 2.91 s |

All ten totals (six streams, GPU busy, communication, overlap, exposed) are
identical, to the nanosecond, to those of
[`examples/kineto_overlap_tp.sql`](examples/kineto_overlap_tp.sql), the
same analysis in pure PerfettoSQL with `INTERVAL FLATTEN` and
`INTERVAL INTERSECTION OF`, run both as a PERFETTO block and by
`trace_processor_shell`.

| Workload (236k GPU rows unless noted) | Instructions | Wall |
| -------- | ------------ | ---- |
| `interval_merge` per stream | 0.29 G | 0.11 s |
| the same with window functions, in DuckDB SQL | 1.12 G | 0.18 s |
| `interval_merge` per GPU | 0.26 G | 0.11 s |
| `interval_exposure` of communication islands vs compute | 0.19 G | 0.07 s |
| `interval_exposure` per NCCL kernel | 0.32 G | 0.07 s |
| `interval_subtract`, exposed spans (40k) | 0.20 G | 0.06 s |
| `interval_join`, NCCL x compute (37k matches) | 0.51 G | 0.12 s |
| `interval_split`, NCCL by compute (77k pieces) | 0.62 G | 0.16 s |
| `interval_quantize`, 1 ms buckets (257k pieces) | 0.54 G | 0.12 s |
| `interval_merge` of all 2.0M slices per track | 2.19 G | 0.16 s |
| `interval_exposure` of 2.0M slices vs 236k GPU rows (one lane) | 4.14 G | 1.65 s |
| pure PerfettoSQL cross-check, all ten totals, in `trace_processor_shell` | | 0.92 s |

Instructions are over a scan of the same input (from parquet); wall times
are for the whole CLI process. In the demo itself (inputs from the attached
trace) the busy-per-stream query takes 0.11 s and the PERFETTO block
cross-check 0.5-1.6 s; the first `interval_*` call on a Polars DataFrame
takes 0.3-0.9 s, later ones under 0.1 s.

## Limitations

* **CLI statement splitting.** The DuckDB CLI decides a statement is complete
  when a line ends in `;` (outside quotes and comments), before any parser
  runs. It doesn't track parens, so in the *interactive or piped CLI*, a line
  inside a block must not end in `;`. Put the `;` mid-line, or start the next
  line with it. Python, JDBC, Node etc. pass the whole string and are
  unaffected. The real fix is the PEG grammar rule, or a small upstream change
  to make `SQLIsComplete` track paren depth.
* **Exact DuckDB version.** C++ extensions only load into the DuckDB version
  they were built for, so a release means one binary per DuckDB version and
  platform. A C-API-only extension avoids this but cannot provide
  `parser_override` or storage extensions.
* **The override setting.** The extension sets
  `allow_parser_override_extension = 'strict'` on load. Without it,
  `parser_override` hooks are ignored. `strict` (rather than `fallback`) is
  needed for our error messages, e.g. "PERFETTO block is missing its closing
  parenthesis", to surface. Queries without blocks are unaffected either way.
* **Result types for blocks.** Types are inferred from a 2048-row sample of
  the first trace. Columns that are all NULL there become `VARCHAR`, and a
  later value that doesn't fit raises an error suggesting a `CAST`. The
  proper fix is a TP API that describes a statement's output types without
  running it. PerfettoSQL's pipeline planner is the natural place to get
  static output types from.
* **Runtime errors in blocks** (e.g. "no such table") come from TP at bind
  time and quote the block's PerfettoSQL.
* **Concurrency.** Each attached trace is a single TP instance behind its own
  mutex, so concurrent scans of *one* trace are serialized; different traces
  run fully in parallel. That requires SQLite in multi-thread mode: standalone
  Perfetto builds use `SQLITE_THREADSAFE=0`, which forbids using SQLite from
  two threads at once even through different connections, so the extension's
  dotfile sets the `perfetto_sqlite_threadsafe` GN arg (default 0) to 2. Peak memory for bulk queries is roughly
  `threads x trace size`.
* **Block side effects.** Blocks run at bind time, which DuckDB needs for
  the result schema. So `DESCRIBE` / `EXPLAIN` of a block containing
  `CREATE PERFETTO TABLE` executes it.
* **Local files only.** Traces are read through TP's `ReadTrace`, not
  DuckDB's filesystem, so httpfs and S3 don't work yet.
* Interrupting a DuckDB query is not wired to
  `TraceProcessor::InterruptQuery`, and there is no progress reporting.
* **Interval functions take relations by name** (or as query strings), not
  as subqueries: DuckDB only passes a subquery to a table in-out function,
  which never sees whole lanes (see How it works). Use a CTE or a view.
* **Lanes are materialized.** Each lane's `(ts, dur, id)` triples are in
  memory at once, and a relation without `per` is a single lane, processed
  by one thread. Operators which keep payload also materialize their input,
  to join it back by row id.
* **Interval coverage gaps.** `interval_merge` aggregates only `COUNT(*)`.
  There is no `INTERVAL FIND`, `KEEP IF` / `DROP IF`, `INTERVALS FROM EVENTS`,
  `UNION OF` or `PER ROLLUP`; `INTERSECTION OF` and `FLATTEN` exist only in
  TP, so not for DataFrames.
* **SQL packages** are registered per attached trace (or per
  `perfetto_query` call), without `allow_override`: a package cannot replace
  a stdlib one, and package names cannot be prefixes of each other.

## Possible next steps

1. **Values in, tables in.** Pass DuckDB values into a block, e.g.
   `PERFETTO(t, min_dur := 100e6) (...)`. This can't reuse `$x`, which
   already means macro arguments in PerfettoSQL. Expose DuckDB relations to
   a block, e.g. `PERFETTO(t USING my_table) (...)`, by registering them in
   TP for the duration of the query.
2. **Static result types** from TP (see Limitations), which also removes
   bind-time execution.
3. **A columnar scan path.** Hand TP dataframe columns to DuckDB vectors
   directly instead of iterating SQLite rows.
4. **Upstream.** Propose paren-aware `SQLIsComplete` to DuckDB, and track the
   PEG parser's extension API.
5. **Distribution.** Publish via DuckDB community extensions, which needs a
   CMake shim around the GN build.
