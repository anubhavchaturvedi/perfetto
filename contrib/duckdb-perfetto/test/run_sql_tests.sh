#!/bin/bash
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

# DuckDB-level tests, run through the stock DuckDB CLI: the SQL checks in
# interval_functions.sql, plus queries which must fail with a given error.
# Usage: contrib/duckdb-perfetto/test/run_sql_tests.sh [duckdb] [extension]

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
DUCKDB="${1:-$ROOT/out/duckdb}"
EXT="${2:-$ROOT/out/duckdb_ext/perfetto.duckdb_extension}"
EXT="$(cd "$(dirname "$EXT")" && pwd)/$(basename "$EXT")"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

PASSED=0
FAILED=0

# Expands the placeholders of a test file.
expand() {
  sed -e "s#__TMP__#$TMP#g" -e "s#__TRACE__#$HERE/tiny_trace.json#g" \
      -e "s#__PKG__#$HERE/sql_package#g" "$1"
}

run_cli() {  # $1: SQL file
  "$DUCKDB" -unsigned -bail -noheader -list -f "$1" 2>&1
}

run_file() {
  local name="$1"
  local sql="$TMP/$name"
  { echo "LOAD '$EXT';"; cat "$HERE/prelude.sql"; expand "$HERE/$name"; } > "$sql"
  local out
  out="$(run_cli "$sql")"
  local status=$?
  echo "$out" | grep '^ok: ' | sed 's/^/  /'
  local oks
  oks="$(echo "$out" | grep -c '^ok: ')"
  PASSED=$((PASSED + oks))
  if [ $status -ne 0 ] || echo "$out" | grep -q 'Error'; then
    echo "FAILED in $name:"
    echo "$out" | grep -v '^ok: ' | sed 's/^/  /'
    FAILED=$((FAILED + 1))
  fi
}

# Runs |sql| after the setup of interval_functions.sql and expects an error
# containing |expected|.
SETUP="$TMP/setup.sql"
expect_error() {
  local sql="$1" expected="$2"
  local file="$TMP/error.sql"
  { echo "LOAD '$EXT';"; cat "$SETUP"; echo "$sql"; } | \
      sed -e "s#__TRACE__#$HERE/tiny_trace.json#g" \
          -e "s#__PKG__#$HERE/sql_package#g" > "$file"
  local out
  out="$(run_cli "$file")"
  if echo "$out" | grep -qF -- "$expected"; then
    echo "  ok: error '$expected'"
    PASSED=$((PASSED + 1))
  else
    echo "FAILED: expected an error containing '$expected' from:"
    echo "  $sql"
    echo "$out" | sed 's/^/  got: /'
    FAILED=$((FAILED + 1))
  fi
}

echo "interval_functions.sql"
run_file interval_functions.sql

# The tables of interval_functions.sql, for the error cases.
sed -n '/^CREATE TABLE a AS/,/^) v(ts, dur, lane, tag);/p' \
    "$HERE/interval_functions.sql" > "$SETUP"

echo "errors"
expect_error "SELECT * FROM interval_merge('SELECT 1 AS ts');" \
    'has no "dur" column'
expect_error "SELECT * FROM interval_merge('SELECT 1.5 AS ts, 2 AS dur');" \
    'not an integer'
expect_error "SELECT * FROM interval_merge('SELECT 0 AS ts, -1 AS dur');" \
    'has dur = -1 (an open interval): pass open_end'
expect_error "SELECT * FROM interval_exposure('SELECT 0 AS ts, -5 AS dur', b);" \
    'has negative dur -5'
expect_error "SELECT * FROM interval_merge(a, per := nope);" \
    'has no PER column "nope"'
expect_error "SELECT * FROM interval_subtract(a, 'SELECT 0 AS ts, 1 AS dur', per := lane);" \
    'the operand has no PER column "lane"'
expect_error "SELECT * FROM interval_join(a, b, relationship := 'near');" \
    "unknown relationship 'near'"
expect_error "SELECT * FROM interval_clip(a, 10, 5);" \
    'is after window_end'
expect_error "SELECT * FROM interval_quantize(a, 0);" \
    'width must be positive'
expect_error "SELECT * FROM interval_set_bounds('SELECT 5 AS b, 1 AS e', begin_at := b, end_at := e);" \
    'end_at is before begin_at'
expect_error "SELECT * FROM interval_set_bounds(a, begin_at := ts);" \
    'exactly one of end_at'
expect_error "SELECT * FROM interval_fill_gaps('SELECT 1 AS x, 2 AS y, 0 AS ts, 1 AS dur', 'SELECT 1 AS x, 0 AS ts, 9 AS dur', per := [x, y]);" \
    'all of the PER columns'
expect_error "SELECT * FROM interval_merge(no_such_table);" \
    'no_such_table does not exist'

echo
echo "$PASSED passed, $FAILED failed"
[ $FAILED -eq 0 ]
