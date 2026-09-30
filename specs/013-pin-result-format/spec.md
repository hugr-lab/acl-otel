# Spec 013: re-pin duckdb to the result-format world

- **Status**: implemented
- **Date**: 2026-09-30

## Summary

duckdb d4e7256 → 96063b9, the same day as duckdb-acl's spec 090: the base and this extension load
on one instance, so they share a pin. duckdb's ResultFormat (7b7ee6f) removed
`QueryResult::GetValue`. The replacement, `Collection().GetValue(c, r)`, rebuilds every row on each
call, so the level-rules load (`acl_otel_state.cpp`) now builds the rows once
(`Collection().GetRows()`) before its loop. Two tests read a single cell through
`Collection().GetValue(0, 0)`.

## Testing

All of the following pass on the new pin, with `ACL_EXT` set to a duckdb-acl build of its spec 090:

- the SQL suite and the beside-acl file;
- test-cpp (10);
- `smoke_load.sh`;
- `make tidy-ci`;
- clang-format.
