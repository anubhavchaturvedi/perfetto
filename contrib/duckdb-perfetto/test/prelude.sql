-- Shared by the SQL tests (see run_sql_tests.sh). Every check prints
-- "ok: <name>", or fails the run through error() with the actual value.
CREATE MACRO s(x) AS coalesce(CAST(x AS VARCHAR), 'NULL');
CREATE MACRO check_eq(name, actual, expected) AS
  CASE WHEN coalesce(actual, '') = expected THEN 'ok: ' || name
  ELSE error('FAIL: ' || name || chr(10) || '  got:      ' || s(actual) ||
             chr(10) || '  expected: ' || expected) END;
