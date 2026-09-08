# Spec 001: the architecture - one header, one registry, and the wiring that proves it

- **Status**: implemented (the wiring); the transports and derivations follow, spec by spec
- **Date**: 2026-09-08
- **Author**: hugr lab

## Summary

`acl_otel` is the observability extension the base's spec 069 left room for: loaded beside `acl` on
the same instance, it consumes the audit stream the base delivers to registered sinks, decides the
audit level per role / user / door when a session opens, and exports OpenTelemetry logs and metrics.
This spec is the founding one: it fixes how the extension reaches the base (one header, one
registry, either load order), what lives where, and the slice built first - the **wiring** (sink,
policy, settings, status, start/stop) - so that every later spec adds a transport or a derivation to
a skeleton that already stands, is tested, and ships through CI.

## Problem

The base's requirements document (`duckdb-acl/specs/069-audit/extension-requirements.md`) is a
contract with eight promises (C1–C8) and ten requirement groups (R1–R10). Building it in one go
would mean a first PR that carries the OpenTelemetry C++ SDK, gRPC and protobuf through vcpkg, the
log and metric mappings, histograms, level rules, sampling and health - none of it reviewable
against a skeleton that does not exist yet. The order matters: the part that touches the base's
thread (the sink) and the base's session-open path (the policy) has the tightest performance and
safety envelope (R10, C3, C6) and must be right before anything is exported.

## Design

### How the extension reaches the base

- **One header.** `acl_audit.hpp` from the `duckdb-acl` submodule (headers only, nothing linked):
  `AuditEvent`, `AuditLevel`, `AuditSink`, `SessionPolicy`, `AuditCounters`/`AuditGauges`,
  `AuditHooks`, plus `Principal` from `acl_policy.hpp` it includes. The submodule pins the base's
  `main`; the duckdb submodule pins the base's duckdb (`v2.0-cyanoptera`), because the two
  extensions load on one instance and must agree on every duckdb type they pass.
- **One registry, either order** (C2). `OtelState::Start` does
  `db.GetObjectCache().GetOrCreate<acl::AuditHooks>("acl_audit_hooks")`: whoever loads first
  creates it, the other adopts it. The state holds the registry's `shared_ptr`, so removing our sink
  at shutdown never touches a freed registry.
- **Per instance** (R8.3). The extension's own state is an `ObjectCacheEntry` too
  (`acl_otel_state`), never evicted; two `DatabaseInstance`s are two states, two sinks, two workers.

### What the wiring is

- **The sink** (`OtelSink`, R1.5 / R6.2 / R10.1): `OnEvent` copies the event onto a bounded queue and
  returns - no allocation beyond the push, no I/O, no wait; a full queue drops and counts. One
  worker thread pops batches (`acl_otel_batch_size`, or whatever is there every
  `acl_otel_flush_interval` seconds) and hands them to an `Exporter`. `Flush()` - the base calls it
  on a level change and at shutdown, on its audit thread - asks the worker to drain now and waits
  at most two seconds: a stuck transport cannot hold the base. `Stop` joins the worker; what was
  still queued is counted as dropped.
- **The exporter seam** (`Exporter::Export(batch, error)`): the one place a transport plugs in. This
  spec ships `NoneExporter` - it stands in while `acl_otel_endpoint` is empty and every batch it
  receives is counted as dropped for "no exporter", distinct from an export error. Spec 002 plugs
  OTLP in; the seam is a `shared_ptr`, so a batch in flight finishes on the transport it started
  with when a setting swaps it.
- **The policy** (`OtelPolicy`, R3): `acl_otel_level_rules` is a JSON array of rules, each any of
  `role`, `subject`, `issuer`, `door` (exact, `*` or absent = any) and a `level`; first match wins;
  no match = no opinion (the instance's level applies, C6). Parsed **at the SET** - a malformed
  document, a bad level, an unknown key (`roles`) are refused there - and hot-reloaded into the
  policy; `LevelFor` is O(rules) with no allocation and no I/O (R3.2, R10.2). The operator's
  per-session override is the base's and is not re-applied (C6, R3.3).
- **The settings** (R9.1): `acl_otel_endpoint`, `acl_otel_level_rules`, `acl_otel_queue_size`,
  `acl_otel_batch_size`, `acl_otel_flush_interval`, `acl_otel_service_name` - all GLOBAL with a
  callback refusing any other scope (`… is global - use SET GLOBAL`), all `acl_otel_*` so the
  base's spec 068 gate refuses them to a principal.
- **The functions** (C8): `acl_otel_version()`, `acl_otel_status()` (a JSON document: attached,
  acl_loaded, endpoint, exporter, rules, queue fill/size, received, exported, batches, dropped by
  why, export_errors, last_export_us), `acl_otel_start()` / `acl_otel_stop()` (R8.2: flush, detach
  the sink and the policy, idempotent; re-attach). Registered volatile and fallible.
- **Lifecycle** (R8): `Load` creates the state, registers everything and attaches; the state's
  destructor (the object cache's teardown seam) stops the worker and detaches.

### The map from the requirements to the specs here

| requirement | spec |
| --- | --- |
| C1–C8, R3, R8, R9.1, R10.1–R10.3, the drop accounting of R6.2, R7.1's numbers | **001** (this) |
| R1 OTel logs over OTLP (gRPC + HTTP), R1.6 the App Insights shape, R1.3 resource attributes | 002 |
| R2 metrics: the base's counters/gauges scraped, the histograms, the bounded high-cardinality series | 003 |
| R4 logging on a connection: `acl_otel_session_level`, `acl_otel_sessions()` | 004 |
| R5 enrichment (claim allowlist), R6.1 sampling | 005 |
| R7.2–R7.3 self-metrics, `acl_otel.healthy`, strict mode | 006 |
| §5 level rules in the policy catalog (`acl_otel.level_rules`), R3.1's central reload | 007 |
| R9.2 secrets for OTLP headers | with 002 |

## Enforcement & security

- Nothing enforces: the extension has no hook on a decision, only on its record. `OnEvent` and
  `LevelFor` are the two places the base calls into, and both are bounded by construction (a queue
  push; a scan of ≤ `acl_otel_max_rules` - the cap arrives with 007's central rules, the JSON
  setting is the operator's own document).
- The surface is the operator's: `acl_otel_*` names, GLOBAL settings, the base's gates. A principal
  sees none of it.
- Nothing is exported yet; when it is (002), the event is already safe (C5) and R5.1 keeps claim
  values out unless allowlisted.
- The worker is the only thread; it is joined at stop and at instance teardown.

## Testing

- `test/cpp/test_acl_otel_rules.cpp`: parsing (empty, array, every refusal), `*`/absent = any,
  first-match order across role / subject / issuer / door, role membership case-insensitive, no
  opinion without rules.
- `test/cpp/test_acl_otel_sink.cpp`: batches in seq order at the configured size; twenty `OnEvent`
  calls behind a stalled exporter return in well under a second and the overflow is counted;
  `Flush` behind the stalled exporter returns within its bound; a refusing exporter counts export
  errors; no exporter counts "no exporter"; a swapped-in exporter gets the next event; after `Stop`
  every event is exported or dropped, never lost uncounted.
- `test/sql/acl_otel.test`: loads alone, version, status (attached, the registry exists), the rules
  setting accepted / refused (four shapes) / cleared, every setting refuses a session scope,
  stop/start idempotent with the status following.
- `test/sql/acl_otel_beside_acl.test` (`require-env ACL_EXT`, the base's built extension): loaded
  beside acl, a rule at `all` for the analyst makes that session's open event recorded while the
  instance sits at `denied`, and the sink received events. Locally against the base's build; in CI
  once the base publishes an artifact the job can fetch (the base's `v0.1.0-rc1` dry run).
- `scripts/ci/smoke_load.sh`: the artifact loads out of the tree, attaches, detaches; beside acl
  when `ACL_EXT` is set.

## Alternatives considered

- **Everything in the first PR**: rejected - see Problem.
- **Linking the base** (a shared library, an rpath): a loadable is `RTLD_LOCAL` and the base's
  internals are hidden on purpose; the header-only contract is the whole interface (C1).
- **Building the base inside this repo's tests** (`duckdb_extension_load(acl SOURCE_DIR …)`): the
  base's Flight door pulls Arrow through vcpkg - an hour of CI for an e2e the base's own artifact
  serves better. The beside-acl test takes the artifact by path.

## Follow-ups

- The vcpkg flow (merged manifests, the fail-fast guard, `make vcpkg-setup`) arrives with 002.
- `make tidy` as in the base, once the tree has enough code to lint beyond format.


## Addendum 2026-09-08 - the contract stamp (C2a)

Header-only cuts both ways: two copies of the contract's types built from different revisions of
`acl_audit.hpp` would read one registry through two layouts. The base now stamps the registry
(`AuditHooks::CONTRACT_MAGIC` + `CONTRACT_VERSION`, its first members) and `AuditHooks::Reach` is
how both sides get it. Here: `OtelState::Start` refuses to attach when `Reach` refuses - nothing of
ours on that registry, `acl_otel_status()` says `attached: false` with `attach_error`,
`acl_otel_start()` answers false. `test_acl_otel_contract` (with `ACL_EXT`) stages the round trip
with two real loadables: a stale registry first, acl auditing privately and naming the stamp it
found, acl_otel refusing; then a fresh instance sharing one.
