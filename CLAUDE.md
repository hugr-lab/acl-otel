# acl_otel — Development Guidelines

**OpenTelemetry for duckdb-acl's audit.** A duckdb extension loaded beside `acl` on the same
instance: it turns the base's audit events and counters into OTel logs and metrics, decides the
audit level per role / user / door (`SessionPolicy`), samples, and reports its own health. It
contains **no enforcement**, changes no decision, and can never slow or stop one. It compiles
against ONE header of the base (`acl_audit.hpp`, header-only) and reaches the base through duckdb's
object cache (`AuditHooks`, `GetOrCreate` by type string, either load order) — a loadable is
dlopen'd RTLD_LOCAL, so nothing here ever resolves a symbol of acl's.

Read **the contract first**: `duckdb-acl/specs/069-audit/extension-requirements.md` (C1–C8 promises,
R1–R10 requirements, §5 decisions) and `duckdb-acl/src/include/acl_audit.hpp`. Then
[specs/001-architecture/spec.md](specs/001-architecture/spec.md) for how the requirements map onto
specs here.

## Technology

- **Language**: C++17 (DuckDB extension standard); style = duckdb's (tabs, ≤120 cols, `unique_ptr`/
  `shared_ptr`, no raw pointers, `clang-format` 11.0.1 — the repo's `.clang-format` is a real copy of
  duckdb's, checked in sync by CI).
- **Pins** — ONE line, bumped together with the base, never mixed:

  | Piece | Where | Pin |
  | --- | --- | --- |
  | duckdb | submodule `duckdb/` | branch `v2.0-cyanoptera` (the base's; `v2.0.0` when tagged) |
  | duckdb-acl | submodule `duckdb-acl/` (HEADERS ONLY: `src/include/acl_audit.hpp` + what it includes) | `main` |
  | extension-ci-tools | submodule `extension-ci-tools/` | `main` |
  | CI reusable workflows | `.github/workflows/distribution.yml` | `@main`, `duckdb_version: v2.0-cyanoptera` |

  The base and this extension load on the same instance: **same duckdb pin, always**. A bump of
  the base's pin is a bump here in the same day.
- **Dependencies**: `opentelemetry-cpp[otlp-http,otlp-grpc]` (spec 002) through vcpkg — the
  merged-manifest flow duckdb-acl uses for Arrow/gRPC, no vendoring (§5 of the contract).
  `make vcpkg-setup` once, or `VCPKG_TOOLCHAIN_PATH` at the base's checkout (same ports, its binary
  cache). The Makefile's guard names the goals that NEED the toolchain, never the exceptions.
- **Platforms**: where the base ships (Linux, macOS, Windows); **no wasm** (an exporter is sockets).
- **License**: BUSL 1.1 (the parameters in `LICENSE` are the licensor's to revise before release).

## Project structure

```text
src/
  acl_otel_extension.cpp   # entry: settings (acl_otel_*, GLOBAL only), the operator's functions, Start at load
  acl_otel_rules.cpp       # the level rules (R3): JSON → rules, first-match LevelFor
  acl_otel_sink.cpp        # the sink (R1.5/R10.1): bounded queue, one worker, batches to an Exporter
  acl_otel_state.cpp       # per-instance state in the object cache: attach/detach, Reconfigure, status JSON
  acl_otel_otlp.cpp        # spec 002: event → LogRecord mapping, the SDK's HTTP/gRPC exporter, the quiet SDK log
  include/acl_otel.hpp     # the types above; acl_otel_otlp.hpp the exporter; acl_otel_extension.hpp the Extension
test/sql/                  # sqllogictests; LOAD by build path (DONT_LINK loadable, invisible to `require`)
test/cpp/                  # C++ tests (make test-cpp): rules + sink Makefile-compiled; the OTLP one is the CMake
                           #   target acl_otel_test_otlp (it links the SDK) with fake receivers in-process
deploy/                    # reference Collector configuration (otlp → azuremonitor)
specs/                     # one spec per feature (specs/README.md); design/ is local research (gitignored)
scripts/ci/                # smoke_load (the artifact LOADS), assert_ran (a suite that skipped is red), prune_vcpkg_cache
```

## Commands

```sh
git submodule update --init --recursive
make vcpkg-setup                                # once (or VCPKG_TOOLCHAIN_PATH=…/duckdb-acl/vcpkg/scripts/buildsystems/vcpkg.cmake)
GEN=ninja make                                  # release build of duckdb + the extension
build/release/test/unittest 'test/sql/*'        # the whole suite (the beside-acl file needs ACL_EXT)
ACL_EXT=../duckdb-acl/build/release/extension/acl/acl.duckdb_extension build/release/test/unittest test/sql/acl_otel_beside_acl.test
GEN=ninja make test-cpp                         # standalone C++ tests
scripts/ci/smoke_load.sh                        # the built artifact loads out of the tree
find src test/cpp \( -name '*.cpp' -o -name '*.hpp' \) | xargs clang-format --dry-run --Werror
```

## Rules that hold here

- **Everything registered in SQL is `acl_otel_*`** (C8): the base's function gate denies the prefix
  to a principal, so the surface is the operator's by construction. Every setting is
  `SetScope::GLOBAL` with a callback that refuses any other scope (a session-scoped value would show
  in `current_setting()` and change nothing).
- **`OnEvent` is O(1), no I/O, no wait** (R10.1). The queue is bounded; a full queue drops and
  counts. `Flush()` is bounded too (the base calls it on its audit thread).
- **Every drop is counted, never silent** (R6.2): `acl_otel_status()` shows queue / no-exporter /
  export-error drops separately. The SDK's HTTP log exporter answers success whatever its client
  said (1.24): an error the SDK logged during the export IS the failure (`QuietLogHandler`,
  spec 002) — never trust `kSuccess` alone.
- **Never a secret in a setting** (R9.2): OTLP headers come from `OTEL_EXPORTER_OTLP_HEADERS`
  only; the status prints header names, never values. The SDK never writes to stderr from here
  (the handler swallows what it does not keep).
- **The contract is stamped** (C2a): `AuditHooks::Reach` is the only way to the registry; a refusal
  means do not attach (`attach_error` in the status, `acl_otel_start()` false). A bump of the base's
  `CONTRACT_VERSION` is a submodule bump here the same day. `test_acl_otel_contract` (needs
  `ACL_EXT`) is the two-loadable round trip the base cannot stage itself.
- **A red beside-acl step usually means the BASE moved.** CI fetches duckdb-acl's latest green
  `main` artifact, which may be ahead of the `duckdb-acl/` submodule pinned here: when the base
  bumps `CONTRACT_VERSION`, our extension refuses to attach to its registry and the step goes red
  until this repo bumps the submodule. That is the early warning working, not a flake - and the
  order to merge in is base first, then here, the same day.
- **The beside-acl test is the proof, and CI runs it**: two loadables on one instance, the base's
  real artifact (downloaded from duckdb-acl's latest green `main` run). A change that passes only
  the alone-suite is not done.
- **Never the keys, the rows, the statement text, a claim value not allowlisted** (C5, R5.1): the
  event is safe by the base's construction; the exporter must keep it so. Two settings are the only
  doors a claim value leaves by - `acl_otel_claim_attributes` (logs) and `acl_otel_claim_dimension`
  (metrics) - and both are the operator's.
- **Sampling never touches a refusal** (R6.1), and never the counters: it runs in `OnEvent` AFTER
  the metrics accumulators have seen the event, so a rate stays exact while the records thin.
- **The extension never writes to the policy catalog and never calls a policy-changing `acl_*`
  function** (R9.3).
- **A setting is read twice, or it is read wrong**: in its SET callback (so a change applies to what
  is running) *and* wherever the thing it configures is built (`OtelState::Start`), or an operator
  who sets it while the scrape is stopped watches it come back to the default. Spec 003 shipped that
  bug; the tests now turn a setting off with the scrape stopped and start it again.
- **Nothing of ours runs DDL unasked.** Spec 007 reads a table a fleet writes; the table is created
  by `acl_otel_create_rules_table()`, which a person runs, never by a background thread on somebody
  else's database. A background thread may read; writing is a statement with a human behind it.
- **A `Connection` costs 24 MB in the artifact.** Running SQL pulls duckdb's query path into the
  loadable (28 MB -> 52 MB; the base's own `acl` is of that order for the same reason). Worth it for
  spec 007's pull; worth thinking about before the next feature reaches for a query.
- Process: a spec per feature (`specs/NNN-slug/spec.md` from `specs/TEMPLATE.md`), tests with it,
  a self-review before "done" (adversarial passes over the diff), CI green before merge.

## What the owner still has to switch on

The distribution matrix builds the OpenTelemetry SDK (grpc, protobuf, abseil, curl) for nine
triplets — hours from cold. `distribution.yml` already passes a vcpkg binary cache through; it only
needs the credentials: copy the repository variable `VCPKG_BINARY_SOURCES` and the four
`VCPKG_CACHING_AWS_*` secrets from `duckdb-acl` (its `docs/vcpkg-cache-r2.md` explains the bucket).
Until then the runs fall back to duckdb's public read-only cache and are merely slow.

## Reference repos (local)

`~/projects/hugr-lab/duckdb-acl` (the base: the contract, the CI recipe, `make tidy`),
`~/projects/hugr-lab/mssql-ducklake` (the scaffold conventions).
