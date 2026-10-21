# Spec 027: re-pin duckdb to the v2.0.0 tag

- **Status**: in progress
- **Date**: 2026-10-21

DuckDB v2.0.0 was released on 2026-10-21. duckdb moves from f8f3f4d to the tag, d54cbbe, 77 commits on, with duckdb-acl's
spec 141: the base and this extension load into one duckdb, so they move the same day. `ACL_BASE` names duckdb-acl's
main once spec 141 merges, built at the same duckdb.

- **The distribution builds the tag**: `duckdb_version: v2.0.0`, as community-extensions builds for 2.0.0 (the 2.0
  branch has moved past it).
- **extension-ci-tools 39ffc46 → fe955ed**, main's head, as the base. Its `v2.0-cyanoptera` branch lacks #432.
- **duckdb-ext-common stays v0.12.0**, unless the base's spec 141 moves it.

## Testing

On the new pin, with `ACL_EXT` (an acl built at d54cbbe):

- the SQL suite and test-cpp;
- the smoke load, alone and beside acl;
- clang-format, clang-tidy;
- the Marquez e2e.

Run on macOS, green so far:

- the build at d54cbbe;
- the suite: 8 files, 230 assertions (the beside-acl file skipped alone, run next);
- test-cpp: 12;
- beside acl: 63 assertions, with `ACL_EXT` the acl of duckdb-acl's spec 141 build.
