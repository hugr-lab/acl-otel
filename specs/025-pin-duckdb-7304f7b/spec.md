# Spec 025: re-pin duckdb to 7304f7b

- **Status**: implemented
- **Date**: 2026-10-15

duckdb moves from f702a1f to 7304f7b, the pin of duckdb-acl's spec 131 (#231). The base and this extension load into
one duckdb, so they move the same day. `ACL_BASE` names duckdb-acl ce1d417: spec 131 (#231) plus spec 130 (#230,
duckdb-ext-common v0.13.0), built at the same duckdb.

- **Changes under us.** 554 commits. acl_otel uses none of the shapes that moved. duckdb #27257 redacts
  `extra_http_headers` when an http secret is shown; acl_otel's key is its own `openlineage` secret, read as values.
  Nothing in `src/` changes.
- **duckdb-ext-common stays v0.12.0.** v0.13.0 changed only the OIDC core, which acl_otel does not compile. The
  contracts and hooks headers are byte-identical, so the shared-header checks in hugr-node hold.

## Testing

On the new pin:

- the SQL suite and test-cpp, both with `ACL_EXT` (an acl built at 7304f7b);
- the smoke load, alone and beside acl;
- clang-format, clang-tidy;
- the Marquez e2e.

Run on macOS, all green:

- the suite: 9 files, 293 assertions, the beside-acl file included;
- test-cpp: 12 programs, the contract one included;
- the smoke load, alone and beside acl;
- clang-format and clang-tidy, clean;
- the Marquez e2e, on `MARQUEZ_PORT=5057` because another Marquez held 5050.
