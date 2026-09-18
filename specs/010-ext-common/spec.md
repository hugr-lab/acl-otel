# Spec 010: the contract from the shared repository - duckdb-ext-common replaces the duckdb-acl submodule

- **Status**: implemented
- **Date**: 2026-09-18
- **Author**: hugr lab

## Summary

The audit contract this extension compiles against (`acl_audit.hpp`, with `Principal` beside it in
`acl_principal.hpp`) now lives in `hugr-lab/duckdb-ext-common` (its spec 002; the base's spec 076).
This extension takes that repository as its submodule and drops `duckdb-acl`, which it carried
whole - with a second duckdb, extension-ci-tools and quack inside - to read one header. The layout of
the contract is unchanged (`CONTRACT_VERSION` 2), which the two-loadable contract test proves
against the base built before the move and after.

## Problem

Spec 001's architecture: one header, one registry, nothing of acl's linked. The header came from
the producer's whole tree; a checkout here pulled duckdb-acl's nested submodules, and the second
use of that submodule - `fetch_base.sh` reading `HEAD:duckdb-acl` as the base commit whose CI
artifact to prefer - tied a build-time pin to an e2e preference.

## Design

- `.gitmodules`: `duckdb-ext-common` in, `duckdb-acl` out. Include path `duckdb-ext-common/contracts`
  (`CMakeLists.txt`, the Makefile's `TEST_CPP_INCLUDES`). Every `#include "acl_audit.hpp"` resolves
  there unchanged; `Principal` is `acl_principal.hpp`'s, included by the contract.
- **`ACL_BASE`**: one line, the base commit whose artifact `fetch_base.sh` tries first (the
  fallback - the newest green run built against the same duckdb - is unchanged). Bumping it is what
  bumping the submodule used to be; it lands in the same commit that removes the submodule, so the
  beside-acl step never loses its base.
- A `CONTRACT_VERSION` bump in the base is now: the shared repository's PR first, its tag, this
  repository's re-pin and `ACL_BASE` bump, the same day.

## Enforcement & security

Nothing at runtime changes: the same header, the same registry key and stamp (`ACLA`, 2), the same
refusal on a mismatch. `test_acl_otel_contract` against the base's artifact from **before** the move
(a038265, contract 2 from `src/include/`) and from **after** (the migration commit, contract 2 from
`contracts/`): both attach, both deliver events with the same fields - the layout did not drift.

## Testing

- `make test-cpp` on the new include path (`test_acl_otel_contract` beside the rest).
- `ACL_EXT=<before> ...` and `ACL_EXT=<after> ...`: the contract test and `acl_otel_beside_acl.test`
  against both artifacts.
- CI: the beside-acl step with `fetch_base.sh` reading `ACL_BASE`; a checkout no longer pulls
  duckdb-acl's tree.

## Alternatives considered

- Keep both submodules - the header would exist twice, and the whole point was the tree.
- Derive `ACL_BASE` from the shared repository's tag - a tag says which contract, not which base
  artifact; the e2e wants a base commit.

## Follow-ups

- When the base moves its contract onto the shared `hooks/` (a bump), the same three-step order.
