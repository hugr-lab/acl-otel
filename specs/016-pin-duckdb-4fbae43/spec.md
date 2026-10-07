# Spec 016: re-pin duckdb to 4fbae43, duckdb-ext-common to v0.10.0

- **Status**: implemented
- **Date**: 2026-10-07

This moves duckdb from eb0d9df to 4fbae43. That is the head of `v2.0-cyanoptera` on 2026-10-07, and
the same commit as duckdb-acl's spec 104 (tresor follows). duckdb-ext-common moves from v0.7.1 to
v0.10.0, the tag tresor already uses.

- The new tag adds the keychain module, the OIDC core for services and its HTTP transport. acl_otel
  compiles none of them.
- The contracts it reads (`acl_audit.hpp`, `acl_principal.hpp`, `acl_connection.hpp`) are unchanged.
- Nothing in `src/` changes.

CI's beside-acl step loads duckdb-acl's latest green main artifact, so it needs duckdb-acl's spec 104
merged first: an acl built for eb0d9df does not load into a 4fbae43 duckdb.

## Testing

On the new pins, with `ACL_EXT` built from duckdb-acl's spec 104 branch:

- the SQL suite: 273 assertions;
- test-cpp: 10/10, the contract test beside acl included;
- `smoke_load.sh`, alone and beside acl.
