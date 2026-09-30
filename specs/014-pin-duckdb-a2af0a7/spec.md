# Spec 014: re-pin duckdb to a2af0a7

- **Status**: implemented
- **Date**: 2026-09-30

This moves duckdb from 96063b9 to a2af0a7, the same commit as duckdb-acl's spec 091 and tresor.

duckdb #26189 makes linking opt-in: only `duckdb_extension_statically_link` links an extension, and
`DONT_LINK` is a FATAL_ERROR. acl_otel was never linked (a linked extension loads at every database
startup, with a worker thread and a sink), and that is now the default, so the `DONT_LINK` word is
dropped.

## Testing

On the new pin, with `ACL_EXT` built from duckdb-acl's spec 091:

- the SQL suite (272 assertions) and the beside-acl file (62);
- test-cpp: 10/10;
- `smoke_load.sh`;
- `make tidy-ci`.
