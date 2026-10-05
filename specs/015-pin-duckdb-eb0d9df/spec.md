# Spec 015: re-pin duckdb to eb0d9df

- **Status**: implemented
- **Date**: 2026-10-05

This moves duckdb from a2af0a7 to eb0d9df, the same commit as duckdb-acl's spec 100 (and tresor).

- acl_otel calls none of the parser API that moved upstream (`ParserOptions::Builtin()`, a parser is
  an object); nothing in `src/` changes.
- The CLI gained an agent mode: with an AI coding agent's variable set and stdout not a terminal it
  changes its output format. `scripts/ci/smoke_load.sh` reads the CLI's output, so it passes
  `-no-agent`.
- The beside-acl file used duckdb-acl's issuer API from before its spec 095 (pasted HS256 keys). It
  now uses the 095 model: the public discovery fixture `test/idp/s` (copied from duckdb-acl; public
  key only), read relative to the repository root under `acl_jwks_locations = 'test/idp/'`, and an
  RS256 token of that key (subject `u-acme`). CI's beside-acl step fetches duckdb-acl's latest green
  main artifact, which carries spec 095 - so the next run would have failed there too.

## Testing

On the new pin, with `ACL_EXT` built from duckdb-acl at eb0d9df (spec 100):

- the SQL suite (273 assertions, the beside-acl file included);
- test-cpp: 10/10;
- `smoke_load.sh` alone and beside acl;
- `test_acl_otel_contract` and `acl_otel_test_traces_otlp` beside acl.
