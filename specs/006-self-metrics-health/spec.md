# Spec 006: the extension's own numbers, and a gauge an orchestrator can act on

- **Status**: implemented
- **Date**: 2026-09-09
- **Author**: hugr lab

## Summary

R7: the extension reports on itself the same way it reports on the base. `acl_otel_status()` already
carries the numbers (spec 001-005); this spec **exports** them as `acl_otel.*` metrics on every
scrape (R7.2) and adds the one an orchestrator reads: `acl_otel.healthy`, a 0/1 gauge, with
`acl_otel_healthy()` beside it for a probe that would rather not parse JSON. **Strict mode is not
refusing statements** - the base emits after the decision, so nothing can be refused post hoc
(R7.3). It is the gauge growing teeth: with `acl_otel_strict = true`, a node that is losing events
reports `healthy = 0`, and a deployment that prefers unavailability to unrecorded decisions drains
it on that signal.

## Problem

A sink that silently stops delivering is worse than no sink: the dashboards stay green and the
record stops. The numbers exist (`acl_otel_status()`), but a fleet does not read a JSON string per
node - it reads a metric, and it wants one metric to alert on rather than five to correlate.

## Design

### The self-metrics (R7.2)

On every tick of spec 003's scrape, beside the base's numbers:

| metric | kind | from |
| --- | --- | --- |
| `acl_otel.received` | counter | events the sink was handed |
| `acl_otel.exported` | counter | events that reached a backend |
| `acl_otel.dropped{why}` | counter | `queue` (full), `no_exporter` (nothing configured), `export_error` (the transport refused), `sampled` (spec 005 - a policy, and counted apart) |
| `acl_otel.export_errors` | counter | batches that failed |
| `acl_otel.queue_fill` | gauge | events waiting right now |
| `acl_otel.metrics_ticks` / `.metrics_errors` | counter | the scrape's own |
| `acl_otel.healthy` | gauge | 0/1, below |

They are ours, so they are named `acl_otel.` and never `acl.` - a dashboard must be able to tell
the node's numbers from its reporter's.

### `healthy` (R7.2, R7.3)

- **Losing** means an audit event that was recorded and never delivered: the queue was full
  (`queue`), nothing was configured to send it (`no_exporter`), or a backend refused the batch
  (`export_error`). A **metric scrape a backend refused** counts too - the node's numbers did not
  arrive either. A scrape that had no endpoint to send to does not: a quiet node with no endpoint
  has lost nothing. Sampling never makes a node unhealthy: it is what the operator asked for.
- A node is **unhealthy** while `acl_otel_strict` is on and either it is losing or the sink is not
  attached at all (a registry from another contract revision, spec 069's stamp). It stays unhealthy
  for `acl_otel_health_window` seconds (default 60) after the last loss, so a single failed export
  is visible to a scrape that arrives a moment later, and a node that recovers says so on its own.
- With `acl_otel_strict` off - the default - `healthy` is always 1: an operator who has not asked
  for the signal is not given an alert.
- `acl_otel_healthy()` answers the same boolean, for a readiness probe. `acl_otel_status()` grows
  `strict`, `healthy` and `losing_since_us`.

The judgement is a pure function of the counters, the clock and the window (`Health::Losing`), so
the tests do not sleep.

### What strict does NOT do

It does not refuse a statement, delay one, or change a decision. The base emits an event after the
decision is made; there is nothing to refuse afterwards, and an extension that could refuse would be
enforcement, which this repo does not contain (C1's whole point). Draining a node on `healthy = 0`
is the orchestrator's action, at the orchestrator's speed, through the base's own
`acl_drain()` (spec 066).

## Enforcement & security

- Read-only, and the numbers are counts: no principal, no object, no claim appears in a self-metric.
- `acl_otel_healthy()` is `acl_otel_`-named, so the base's function gate denies it to a principal
  like the rest of the surface (C8).
- The health rule can only make a node look worse than it is, never better: a drop that has not been
  judged yet leaves `healthy` at its last value, and the next judgement sees the higher counter.

## Testing

- **C++** (`test_acl_otel_health.cpp`): `Health::Losing` - the first judgement of a node that has
  already dropped is losing; the window expires with the clock the test hands it; a further drop
  extends it; a counter that does not move recovers; sampling's counter is not part of the sum.
- **C++ against the fake receiver**: one tick carries `acl_otel.received`, `.exported`,
  `.dropped{why}` per reason, `.queue_fill` and `.healthy`, with the `acl_otel.` prefix and the
  right kinds.
- **sqllogictest**: `acl_otel_healthy()` is true by default and stays true under strict on a quiet
  node; with strict on and an endpoint nobody listens on, a flushed export makes it false and the
  status says since when; `acl_otel_strict` and `acl_otel_health_window` refuse a session scope.

## Alternatives considered

- **Unhealthy without strict**: an operator who never configured an endpoint would get a red gauge
  for a node that is behaving exactly as configured.
- **A rate rather than a window** (`dropped per second over the last minute`): needs history and a
  clock on the emitting side; a window since the last loss is one integer and answers the same
  question.
- **Refusing statements in strict mode**: impossible by construction (R7.3) and undesirable by
  design - this extension cannot become an enforcement path.
