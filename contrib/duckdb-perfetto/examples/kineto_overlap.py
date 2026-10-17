#!/usr/bin/env python3
# Copyright (C) 2026 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
"""kineto_overlap.sql from Python, with Polars on both sides of DuckDB.

GPU busy time per stream, compute / communication overlap and exposed
communication for a Kineto (PyTorch profiler) trace:

  * the GPU activity comes out of TP as a Polars DataFrame (`.pl()`);
  * it is classified with Polars expressions;
  * the interval_* functions run on those DataFrames, which DuckDB finds by
    their variable names (replacement scans), and return Polars again;
  * the totals are cross-checked against pure PerfettoSQL run inside TP
    (kineto_overlap_tp.sql, as a PERFETTO block).

Needs `pip install duckdb==1.5.6 polars pyarrow`. Usage:

  kineto_overlap.py <trace> [path/to/perfetto.duckdb_extension]
"""

import argparse
import os
import time

import duckdb
import polars as pl

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..', '..'))
DEFAULT_EXT = os.path.join(ROOT, 'out', 'duckdb_ext',
                           'perfetto.duckdb_extension')

TIMINGS = []


class step:
  """Times a block of work."""

  def __init__(self, name):
    self.name = name

  def __enter__(self):
    self.t0 = time.perf_counter()

  def __exit__(self, *exc):
    TIMINGS.append((self.name, time.perf_counter() - self.t0))


def main():
  parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
  parser.add_argument('trace')
  parser.add_argument('extension', nargs='?', default=DEFAULT_EXT)
  args = parser.parse_args()
  pl.Config.set_tbl_rows(30)
  pl.Config.set_fmt_str_lengths(40)

  con = duckdb.connect(config={'allow_unsigned_extensions': 'true'})
  con.execute(f"LOAD '{os.path.abspath(args.extension)}'")
  with step('ATTACH trace'):
    con.execute(f"ATTACH '{os.path.abspath(args.trace)}' AS t (TYPE perfetto)")

  # TP tables -> Polars. Kineto puts each CUDA stream on a gpu_render_stage
  # track named "Channel #<stream>", whose "ugpu" dimension is the device.
  with step('GPU activity from TP to Polars (.pl())'):
    gpu_ops = con.sql("""
      SELECT s.ts, s.dur, s.name, g.int_value AS gpu,
             CAST(regexp_extract(tr.name, '#(\\d+)', 1) AS INTEGER) AS stream
      FROM t.slice s
      JOIN t.track tr ON s.track_id = tr.id
      LEFT JOIN t.args g
        ON g.arg_set_id = tr.dimension_arg_set_id AND g.key = 'ugpu'
      WHERE tr.type = 'gpu_render_stage' AND tr.name LIKE 'Channel #%'
    """).pl()

  # Classification in Polars: NCCL kernels are communication; memory copies
  # and sets are neither communication nor compute.
  name = pl.col('name')
  is_comm = name.str.to_lowercase().str.contains('nccl')
  is_copy = name.str.starts_with('Memcpy') | name.str.starts_with('Memset')
  gpu_ops = gpu_ops.with_columns(
      kind=pl.when(is_comm).then(pl.lit('comm')).when(is_copy).then(
          pl.lit('copy')).otherwise(pl.lit('compute')))
  comm = gpu_ops.filter(pl.col('kind') == 'comm')  # noqa: F841 (used in SQL)
  compute = gpu_ops.filter(pl.col('kind') == 'compute')  # noqa: F841
  print(
      gpu_ops.group_by('kind').agg(
          pl.len().alias('n'),
          pl.col('dur').sum().alias('dur_ns')).sort('kind'))

  # Polars DataFrames -> interval_* functions -> Polars. `gpu_ops`, `comm`
  # and `compute` are found by name. DuckDB's sum() of BIGINT is a HUGEINT,
  # which Polars sees as a decimal: cast it back.
  with step('busy time per stream (interval_merge on a DataFrame)'):
    busy = con.sql("""
      SELECT gpu, stream, count(*) AS islands,
             CAST(sum(dur) AS BIGINT) AS busy_ns
      FROM interval_merge(gpu_ops, per := [gpu, stream])
      GROUP BY ALL ORDER BY busy_ns DESC
    """).pl()
  print(busy)

  with step('GPU busy time (interval_merge)'):
    gpu_busy = con.sql("""
      SELECT gpu, CAST(sum(dur) AS BIGINT) AS gpu_busy_ns
      FROM interval_merge(gpu_ops, per := gpu) GROUP BY ALL
    """).pl()
  print(gpu_busy)

  with step('comm coverage + exposure (interval_merge, interval_exposure)'):
    comm_cov = con.sql(  # noqa: F841 (used in SQL)
        'SELECT * FROM interval_merge(comm, per := gpu)').pl()
    exposed = con.sql(
        'SELECT * FROM interval_exposure(comm_cov, compute, per := gpu)').pl()
  # Back in Polars: aggregate the per-island exposure.
  summary = exposed.group_by('gpu').agg(
      pl.col('dur').sum().alias('comm_ns'),
      pl.col('overlap_dur').sum().alias('overlap_ns'),
      pl.col('exposed_dur').sum().alias('exposed_ns'),
  ).with_columns(
      exposed_pct=(100 * pl.col('exposed_ns') / pl.col('comm_ns')).round(2))
  print(summary)

  with step('exposure per NCCL kernel (interval_exposure)'):
    per_kernel = con.sql(
        'SELECT * FROM interval_exposure(comm, compute, per := gpu)').pl()
  print(
      per_kernel.with_columns(
          kernel=pl.col('name').str.extract(r'^([A-Za-z_]+)')).group_by(
              'kernel').agg(
                  pl.len().alias('n'),
                  pl.col('dur').sum().alias('dur_ns'),
                  pl.col('exposed_dur').sum().alias('exposed_ns'),
              ).sort('exposed_ns', descending=True))

  # Cross-check against pure PerfettoSQL, executed by TP.
  with open(os.path.join(HERE, 'kineto_overlap_tp.sql')) as f:
    tp_sql = f.read()
  with step('cross-check: pure PerfettoSQL in a PERFETTO block'):
    tp = con.execute(f'SELECT * FROM PERFETTO(t) ({tp_sql})').pl()
  ours = pl.concat([
      busy.select(
          what=pl.format('stream {}', pl.col('stream')), ns=pl.col('busy_ns')),
      pl.DataFrame({
          'what': ['gpu busy', 'comm', 'overlap', 'exposed'],
          'ns': [
              int(gpu_busy['gpu_busy_ns'].sum()),
              int(summary['comm_ns'].sum()),
              int(summary['overlap_ns'].sum()),
              int(summary['exposed_ns'].sum()),
          ],
      }),
  ])
  check = ours.join(
      tp.select('what',
                pl.col('ns').alias('perfettosql_ns')),
      on='what',
      how='full',
      coalesce=True).with_columns(
          agree=pl.col('ns') == pl.col('perfettosql_ns')).sort('what')
  print(check)
  print('cross-check:', 'all agree' if check['agree'].all() else 'MISMATCH')

  print('\ntimings (wall, s):')
  for name, secs in TIMINGS:
    print(f'  {secs:8.3f}  {name}')


if __name__ == '__main__':
  main()
