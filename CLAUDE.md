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
- **Dependencies**: none yet. Spec 002 brings `opentelemetry-cpp[otlp-grpc,otlp-http]` through
  vcpkg (the merged-manifest flow duckdb-acl uses for Arrow/gRPC; no vendoring — §5 of the contract).
- **Platforms**: where the base ships (Linux, macOS, Windows); **no wasm** (an exporter is sockets).
- **License**: BUSL 1.1 (the parameters in `LICENSE` are the licensor's to revise before release).

## Project structure

```text
src/
  acl_otel_extension.cpp   # entry: settings (acl_otel_*, GLOBAL only), the operator's functions, Start at load
  acl_otel_rules.cpp       # the level rules (R3): JSON → rules, first-match LevelFor
  acl_otel_sink.cpp        # the sink (R1.5/R10.1): bounded queue, one worker, batches to an Exporter
  acl_otel_state.cpp       # per-instance state in the object cache: attach/detach, status JSON
  include/acl_otel.hpp     # the types above; acl_otel_extension.hpp the Extension class
test/sql/                  # sqllogictests; LOAD by build path (DONT_LINK loadable, invisible to `require`)
test/cpp/                  # standalone C++ tests (make test-cpp): the rules engine, the sink
specs/                     # one spec per feature (specs/README.md); design/ is local research (gitignored)
scripts/ci/                # smoke_load (the artifact LOADS), assert_ran (a suite that skipped is red)
```

## Commands

```sh
git submodule update --init --recursive
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
  export-error drops separately.
- **Never the keys, the rows, the statement text, a claim value not allowlisted** (C5, R5.1): the
  event is safe by the base's construction; the exporter must keep it so.
- **The extension never writes to the policy catalog and never calls a policy-changing `acl_*`
  function** (R9.3).
- Process: a spec per feature (`specs/NNN-slug/spec.md` from `specs/TEMPLATE.md`), tests with it,
  a self-review before "done" (adversarial passes over the diff), CI green before merge.

## Reference repos (local)

`~/projects/hugr-lab/duckdb-acl` (the base: the contract, the CI recipe, `make tidy`),
`~/projects/hugr-lab/mssql-ducklake` (the scaffold conventions).
