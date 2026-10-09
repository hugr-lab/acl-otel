# Spec 021: re-pin duckdb-ext-common to v0.12.0

- **Status**: implemented
- **Date**: 2026-10-09

duckdb-ext-common moves from v0.11.0 to v0.12.0, the tag duckdb-acl's spec 112 builds against (its
spec 015). duckdb stays at 4fbae43.

- The new tag adds one contract, `contracts/acl_lineage_sources.hpp` (`ACLS 1`): a registry through
  which a platform extension names a source in lineage. duckdb-acl reads it; acl_otel neither reads
  nor provides it.
- The contracts acl_otel reads (`acl_audit.hpp` v3, `acl_principal.hpp`, `acl_connection.hpp`) are
  unchanged.
- Nothing in `src/` changes.

## Testing

On the new pin:

- the SQL suite: 230 assertions in 8 test cases;
- test-cpp: 12/12;
- `smoke_load.sh`, alone and beside acl (an acl built from duckdb-acl's spec 112 branch): a shared
  registry;
- the Marquez e2e runs in CI's Linux job (locally its port is held by another Marquez).
