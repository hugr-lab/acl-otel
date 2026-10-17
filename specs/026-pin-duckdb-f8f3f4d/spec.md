# Spec 026: re-pin duckdb to f8f3f4d

- **Status**: implemented
- **Date**: 2026-10-17

duckdb moves from 7304f7b to f8f3f4d, the pin of duckdb-acl's spec 133 (#233). The base and this extension load into
one duckdb, so they move the same day. `ACL_BASE` names duckdb-acl 23a8b18 (#233 merged), built at the same duckdb.

- **Nothing in `src/` changes.**
- **duckdb-ext-common stays v0.12.0.** The contracts and hooks headers the shared-header checks compare are
  byte-identical to v0.13.0's.
- **The distribution build** now hands the build a prebuilt duckdb (extension-ci-tools a90d6d4). Its version is the
  release's alpha (`v2.0.0-alpha48730`), while the loadable built beside it names the branch (`v2.0-cyanoptera`). The
  test binary refused to LOAD acl_otel, and six sqllogic files failed on linux_amd64 (found on this PR). The Makefile
  overrides `DUCKDB_PREBUILT_LIBRARY` / `_EXTENSIONS` and builds duckdb from the pin, as duckdb-acl does (its spec
  133).

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
