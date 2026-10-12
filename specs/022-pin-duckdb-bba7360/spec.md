# Spec 022: re-pin duckdb to bba7360

- **Status**: implemented
- **Date**: 2026-10-12

duckdb moves from 4fbae43 to bba7360, the pin of duckdb-acl's spec 123 (#217): the base and this extension load
into one duckdb, so they move the same day. `ACL_BASE` names duckdb-acl 05dd910 (#217 merged), built at the same
duckdb.

- What changed under us that acl_otel could feel - duckdb #26880 (no `ResultEagerness`; `Submit` decides nothing,
  a result closed before its end is rolled back), `ParseMemoryLimit`'s `optional_idx`: acl_otel uses none of it
  (its service connection runs `Query`, which still retains). Nothing in `src/` changes.
- duckdb-ext-common stays v0.12.0; the contracts are unchanged.

## Testing

On the new pin: the SQL suite (with `ACL_EXT`, an acl built at bba7360), test-cpp, the Marquez e2e.
