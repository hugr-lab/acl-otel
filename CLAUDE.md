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
- **Platforms**: linux amd64/arm64, osx arm64, windows amd64 and MinGW. **No wasm** (an exporter is
  sockets) and **no osx_amd64** (the owner's decision, 2026-09-09: nobody deploys a server on an
  Intel Mac, and this extension exists for servers).
  The base still builds osx_amd64, so that is a deliberate asymmetry, not drift.
- **License**: BUSL 1.1 (the parameters in `LICENSE` are the licensor's to revise before release).

## Project structure

```text
src/
  acl_otel_extension.cpp   # entry: settings (acl_otel_*, GLOBAL only), the operator's functions, Start at load
  acl_otel_rules.cpp       # the level rules (R3): JSON → rules, first-match LevelFor
  acl_otel_sink.cpp        # the sink (R1.5/R10.1): bounded queue, one worker, batches to an Exporter
  acl_otel_state.cpp       # per-instance state in the object cache: attach/detach, Reconfigure, status JSON
  acl_otel_otlp.cpp        # spec 002: event → LogRecord mapping, the SDK's HTTP/gRPC exporter, the quiet SDK log
  acl_otel_traces.cpp      # spec 008 without the SDK: the trace mode, the traceparent parser, which event is a span
  acl_otel_otlp_traces.cpp # spec 008: event → Span (the caller's trace, ours as a child), the SDK's trace exporters
  include/acl_otel.hpp     # the types above; acl_otel_otlp.hpp the exporter; acl_otel_extension.hpp the Extension
test/sql/                  # sqllogictests; LOAD by build path (DONT_LINK loadable, invisible to `require`)
test/cpp/                  # C++ tests (make test-cpp): rules + sink Makefile-compiled; the OTLP ones are CMake
                           #   targets acl_otel_test_{otlp,metrics_otlp,traces_otlp} (they link the SDK) with fake
                           #   receivers in-process; the traces one also runs the two-loadable span round trip under ACL_EXT
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
GEN=ninja make test-cpp                         # C++ tests (two of them CMake targets that link the SDK)
scripts/ci/smoke_load.sh                        # the built artifact loads out of the tree
make tidy-ci                                    # clang-tidy, and a finding fails - CI runs this
find src test/cpp \( -name '*.cpp' -o -name '*.hpp' \) | xargs clang-format --dry-run --Werror
make -C duckdb format-check T="--workdir $PWD --directories src test"   # what the community pipeline runs
```

**Before saying a change is done**: all of the above, with `ACL_EXT` set so the beside-acl file and
`test_acl_otel_contract` actually run rather than skip. A suite that skipped the one test that
proves two loadables share a registry has proven nothing.

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
- **A red beside-acl step usually means the BASE moved.** CI fetches a green `main` artifact of
  duckdb-acl, which may be ahead of the `duckdb-acl/` submodule pinned here: when the base bumps
  `CONTRACT_VERSION`, our extension refuses to attach to its registry and the step goes red until
  this repo bumps the submodule. That is the early warning working, not a flake - and the order to
  merge in is base first, then here, the same day.
- **`fetch_base.sh` takes the artifact built against the duckdb WE pin**, not simply the newest
  green one: an extension loads only into the duckdb it was built against, and the base publishes
  its `duckdb` pin per commit, so each candidate run is checked before it is downloaded. The run of
  the commit our `duckdb-acl` submodule names is tried first. When no green run matches, the failure
  names both pins - the usual cause is a base bump whose own CI has not finished yet, and the cure
  is to re-run the job once it is green. A `LOAD` refusing the base's artifact for "DuckDB version
  ..." means that check was bypassed, and the answer is never a metadata-mismatch override.
- **The beside-acl test is the proof, and CI runs it**: two loadables on one instance, the base's
  real artifact. A change that passes only the alone-suite is not done.
- **Never the keys, the rows, the statement text, a claim value not allowlisted** (C5, R5.1): the
  event is safe by the base's construction; the exporter must keep it so. Two settings are the only
  doors a claim value leaves by - `acl_otel_claim_attributes` (logs) and `acl_otel_claim_dimension`
  (metrics) - and both are the operator's.
- **Sampling never touches a refusal** (R6.1), and never the counters: it runs in `OnEvent` AFTER
  the metrics accumulators have seen the event, so a rate stays exact while the records thin. A
  record and its span are sampled together: the span lane (spec 008) is fed after the sampler.
- **A span is never a guess** (spec 008): only an event whose both ends the base measured becomes
  one (`rewrite_us`, or `duration_us` behind `acl_otel_session_spans`); no trace id is ever made
  from a `correlation_id`; the caller's `sampled` flag is obeyed, never upgraded; a refusal by
  policy is `kUnset`, not an error. The lane's transport refuses a batch with an unmeasured event
  rather than export a zero-length span.
- **The execution profile** (spec 009, contract v2's `profile` event): the execution span
  `[ts_us - exec_us, ts_us]`, `acl exec <CLASS>`, a sibling of the decision under the caller's span
  and **linked** to it - span ids are DERIVED (`SpanIdFor(node, seq)`, `TraceIdFor` for an orphan's
  own trace), never random, so the link needs no state; `acl.source` / `acl.operator` span events;
  the record's body is the profile whole as JSON (the tree kept a tree; the SDK's body is an
  `AttributeValue`, no nested map); five `acl.exec.*` histograms with `source` / `kind` labels (the
  node is the resource); the sampler keeps a profile with its decision (by `decision_seq`), never
  thins a failed one. `acl_otel_profile_plan` (the plan travels at all), `acl_otel_profile_spans`
  (operators as child spans, labelled `cumulative_thread_time` - opt-in: not an interval).
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

## The vcpkg binary cache (on since 2026-09-09)

The distribution matrix builds the OpenTelemetry SDK (grpc, protobuf, abseil, curl) for six
triplets, which is hours from cold: a full run without the cache measured **2h55m–3h40m**, the
MinGW job alone 84–153 minutes. So this repository now carries the same cache duckdb-acl uses —
the repository variable `VCPKG_BINARY_SOURCES` and four `VCPKG_CACHING_AWS_*` secrets, pointing at
**the same bucket** (`duckdb-acl-vcpkg-cache`; its `docs/vcpkg-cache-r2.md` explains it).

Sharing one bucket is deliberate: vcpkg addresses every archive by an ABI hash (port, version,
features, triplet, toolchain, dependency hashes), so two repositories either produce the same hash
and legitimately share the artifact or produce different ones and coexist. Both take vcpkg from
duckdb's pin through extension-ci-tools — the same commit, verified — so the expensive shared ports
(grpc, protobuf, abseil, curl, openssl) really are reused; only `opentelemetry-cpp` is ours alone.

No lifecycle rule for now, on purpose: R2 can only expire by age since creation and vcpkg never
touches an object on a hit, so any window eventually evicts a hot archive and forces a rebuild.
While two duckdb lines are alive both halves of the cache are hot. The bucket was 3.96 GB against
R2's 10 GB free tier when this was written; the moment to clean up is when a line is retired - one
deliberate purge of its archives, not a rule.

## The one decision still open

Spec 004 (per-connection logging): the extension cannot see the base's sessions, because the
contract carries events, counters and gauges and a session list is none of the three. The spec draft
holds the three ways out - a sessions reader in `AuditHooks` plus a `CONTRACT_VERSION` bump
(recommended), a table function that opens a second connection, or nothing - and is waiting for the
owner rather than for code.

## Reference repos (local)

`~/projects/hugr-lab/duckdb-acl` (the base: the contract, the CI recipe, `make tidy`),
`~/projects/hugr-lab/mssql-ducklake` (the scaffold conventions).
