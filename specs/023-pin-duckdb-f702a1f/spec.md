# Spec 023: re-pin duckdb to f702a1f

- **Status**: implemented
- **Date**: 2026-10-14

duckdb moves from bba7360 to f702a1f, the pin of duckdb-acl's spec 128 (#227): the base and this extension load
into one duckdb, so they move the same day. `ACL_BASE` names duckdb-acl 1714d05 (#227 merged), built at the same
duckdb.

- What changed under us that acl_otel could feel - duckdb #27108 (a statement that expands into several runs as one
  query; `StreamsByDefault` takes a result; `Prepare` accepts such a statement with its metadata deferred), #26914
  (`ExpressionStatement` moved to the shell): acl_otel uses none of it. Nothing in `src/` changes.
- **The CLI's agent output mode**: in an agent's shell (`AI_AGENT`, `CLAUDECODE`, … set) the CLI prints its agent
  layout; the Marquez e2e reads the CLI's csv, so its two calls pass `-no-agent` (duckdb-acl spec 128 found the same
  in its e2e). CI sets none of the markers.
- **The Marquez port**: `MARQUEZ_PORT` (default 5050) moves the e2e's Marquez when another one holds the port (a
  developer's standing Marquez did, the run here used 5057).
- duckdb-ext-common stays v0.12.0; the contracts are unchanged.

## Testing

On the new pin: the SQL suite (with `ACL_EXT`, an acl built at f702a1f), test-cpp (with `ACL_EXT`), the smoke load,
clang-format, clang-tidy, the Marquez e2e.
Run (macOS): the suite - 9 files, 293 assertions, the beside-acl file included; test-cpp - 5 programs, the contract one
included; the smoke load (alone and beside acl); clang-format; clang-tidy clean; the Marquez e2e.
