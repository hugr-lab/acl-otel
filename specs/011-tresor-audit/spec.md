# Spec 011: tresor's audit as OpenTelemetry

- **Status**: implemented
- **Date**: 2026-09-24
- **Contract**: duckdb-ext-common v0.7.1: `contracts/tresor_audit.hpp` (TRSA 1, its spec 008) on
  `hooks/ext_hooks.hpp` (base version 1), plus the audit gauges fix of its spec 009
- **Producer**: tresor (its spec 011, PR #14). tresor is the duckdb client of the external secrets
  service, loaded beside acl on a node.
- **Decisions (owner, 2026-09-24)**: logs one record per event; a span under a valid traceparent;
  tresor's counters in the metrics; the same resource, with scope `tresor`.

## Problem

tresor logs in, looks up secrets, writes, grants, and holds delegation grants for acl sessions. It
publishes each of these as an audit event through a registry in duckdb's object cache, the same way
acl does. No consumer carries them out, so a node that exports acl's decisions to OpenTelemetry
leaves out what its secrets service did for those decisions.

## Design

**Attach** (`OtelState::Start`, `AttachTresorLocked`).

- Start calls `TresorAuditHooks::Reach(db.GetObjectCache(), why)` beside acl's registry. Either
  side creates the registry, so the load order does not matter.
- A registry stamped with another TRSA version or another hooks base version is refused. Nothing of
  ours goes on it, and the status says why (`tresor.error`), as ACLA's refusal does.
- The setting `acl_otel_tresor` (default `true`, GLOBAL) attaches or detaches immediately.
- tresor starts its delivery thread only while a sink is registered.

**The lanes.** `EventQueue` and `Exporter` became templates over the event type:
`EventQueueOf<E>` and `ExporterOf<E>`, with `EventQueue` and `Exporter` kept as the base's aliases.
tresor's events therefore ride lanes that behave exactly like acl's: bounded, drops counted,
flushes bounded. `TresorOtelSink::OnEvent` is O(1) with no I/O.

- Every event goes to the records' lane.
- The span lane, which exists only while `acl_otel_traces` is not `off`, takes an event only if both
  hold:
  - the event carries the caller's traceparent, and that traceparent is valid and sampled;
  - the event measured a call (`duration_us >= 0`).

  A traceparent with the `00` flags is counted as `unsampled`.
- The sink never touches the object cache, because tresor's last delivery runs inside the cache's
  teardown. It owns everything it touches, because tresor may call it once more after `RemoveSink`.

**The record** (`FillTresorRecord`, through the logs' transport under scope `tresor`):

- severity WARN for `denied` / `error`, INFO otherwise;
- body `tresor <kind> <outcome>[: <reason>]`;
- the caller's trace context when the traceparent is valid;
- every field as a `tresor.*` attribute, only when present:
  - `tresor.cached` on a lookup or refresh;
  - `tresor.dynamic` when a secret is named;
  - `tresor.duration_us` only when a call was made.

**The span** (`FillTresorSpan`, through the spans' transport under scope `tresor`):

- name `tresor.<kind>`, kind internal;
- the caller's trace, with the caller's span as parent;
- interval `[ts_us - duration_us, ts_us]`;
- status kError on `denied` / `error`, with the reason code as its message;
- the same attributes as the record;
- span id `SpanIdFor(node, seq, "tres" salt)`. It is derived like spec 009's ids, and the salt means
  it never collides with a decision span of the same node and seq (tresor numbers its own events).

There is no span without a traceparent, and none per session (the owner's call). A span is never a
guess: an unmeasured event never becomes a zero-length span.

**Metrics.** The scrape adds the counters tresor gives the registry, with tresor's own names and
labels:

- `tresor.events` {kind, outcome, cached}, counted only while a sink is registered;
- `tresor.audit.dropped` and `tresor.audit.sink_failed`.

The scrape also adds our own numbers for tresor's events:

- `acl_otel.tresor.received` / `.exported` / `.dropped{why}`;
- `acl_otel.tresor.spans.*`;
- the gauge `acl_otel.tresor.attached`.

Strict health counts tresor's lost records and spans as losses.

**Status.** A `tresor` object with `enabled`, `attached`, `error`, `events`,
`records{sent, lost_queue, lost_no_exporter, lost_failed}` and `spans{sent, unsampled}`. It uses names
of its own: the status is matched by name, so the records' and spans' `received` / `exported` /
`export_errors` must stay the only ones. The beside-acl test caught two collisions before these
names were chosen.

**Transports.** tresor's transports are built from the same settings as the base's: endpoint,
protocol, headers and TLS. A change to those settings rebuilds them along with the base's.

## Enforcement & security

- The event carries no material, token, handle, grant id or statement text (the TRSA contract), and
  nothing here adds any.
- `reason` is tresor's own words (a status and fixed texts).
- Metric labels come only from tresor's bounded counters. The unbounded fields (secret, user,
  session) are record and span attributes, never labels.

## Tests

`test/cpp/test_acl_otel_tresor_otlp.cpp` runs against a fake HTTP receiver in-process:

- **The span gate:**
  - a traced, measured lookup is a span;
  - a cached lookup, a login without a trace, and a malformed traceparent are not;
  - the `00` flags make an event `unsampled`;
  - a tresor span id is not the decision span's.
- **The record transport:**
  - scope `tresor`, INFO/WARN, the body;
  - every `tresor.*` attribute, and the trace context;
  - no duration when no call was made;
  - the target of a grant.
- **The span transport:**
  - name, parent and trace, interval, status;
  - a batch holding an unmeasured event is refused.
- **The state:**
  - it attaches beside the base to a registry it creates, and is fed the way tresor delivers: six
    events give six records and three spans, and the unsampled one is counted;
  - `tresor.events` appears in the scrape, and the status names the numbers;
  - turning the setting off takes the sink off the registry;
  - a registry stamped with version 99 is refused, with the reason, and nothing goes on it.

The whole suite, the beside-acl test and the other test-cpp targets pass, and tidy and format are
clean.

## Follow-ups

- tresor publishes no gauges yet. When it does, they join the scrape the same way.
- A real tresor loadable beside acl and acl-otel end to end belongs to tresor's CI (actor.sql) and to
  a beside-tresor step here, once tresor publishes an artifact built against our duckdb pin.
