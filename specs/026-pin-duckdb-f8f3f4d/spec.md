# Spec 026: re-pin duckdb to f8f3f4d

- **Status**: implemented
- **Date**: 2026-10-17

duckdb moves from 7304f7b to f8f3f4d, the pin of duckdb-acl's spec 133 (#233). The base and this extension load into
one duckdb, so they move the same day. `ACL_BASE` names duckdb-acl 23a8b18 (#233 merged), built at the same duckdb.

- **Nothing in `src/` changes.**
- **duckdb-ext-common stays v0.12.0.** The contracts and hooks headers the shared-header checks compare are
  byte-identical to v0.13.0's.
- **The distribution build** now links a prebuilt duckdb (extension-ci-tools a90d6d4). acl_otel's test target runs
  only the suite, so this needs no change here. duckdb-acl opted out, because its test-cpp needs a libduckdb.

## Testing

On the new pin, with `ACL_EXT` (an acl built at f8f3f4d):

- the SQL suite and test-cpp;
- the smoke load, alone and beside acl;
- clang-format, clang-tidy;
- the Marquez e2e.

Run on macOS, all green:

- the suite: 9 files, 293 assertions, the beside-acl file included;
- test-cpp: 12, the contract test included;
- the smoke load, alone and beside acl;
- clang-format and clang-tidy;
- the Marquez e2e (`MARQUEZ_PORT=5057`).
