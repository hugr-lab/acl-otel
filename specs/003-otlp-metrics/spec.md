# Spec 003: OTLP metrics - the base's counters and gauges, our histograms, bounded

- **Status**: draft
- **Date**: 2026-09-08
- **Author**: hugr lab

## Summary

Spec 002 exports the record of every decision. This spec exports the *shape* of them: the base's
counters and gauges (`AuditCounters::Snapshot()` / `AuditGauges::Snapshot()`, C7) scraped on a timer
and pushed as OTLP metrics under their own names (R2.1), the histograms the base deliberately does
not keep - rewrite duration, session duration, ingest rows - built here from the events the sink
already receives (R2.2), and the high-cardinality series (by role, object, subject, one claim) that
only exist when an operator asks for them, each capped with an `other` fold so a tenant explosion
costs one series and not a backend (R2.3). One more thread, its own timer, the same transport
family as the logs (`OtlpHttpMetricExporter` / `OtlpGrpcMetricExporter`, driven directly, no SDK
`MeterProvider`), and the same rule: an export that fails is counted and dropped, never retried
across a restart.

## Problem

The audit log answers "what happened to this statement". Operations asks the other question - how
many, how fast, how many are being refused right now, is the queue filling - and asks it of a
dashboard, not a query. The base already keeps the numbers (`acl_metrics()`, spec 069) and a node's
`GET /metrics` serves them to a Prometheus that can reach it; a fleet behind a Collector cannot be
scraped and wants them pushed. Nothing in the base derives distributions, because a histogram is a
choice of buckets and the base refuses to make one for everybody.

## Design

### The scraper

- `OtelMetrics` (`src/acl_otel_metrics.cpp`): one thread, waking every `acl_otel_metrics_interval`
  seconds (default 15). Each tick: `Snapshot()` both registries, fold in the histograms accumulated
  since the last tick, build one `sdk::metrics::ResourceMetrics` and hand it to the exporter in one
  `Export(data)` call. Owned by `OtelState`, started and stopped with the sink (one `acl_otel_start`
  / `acl_otel_stop` for both), and never running when there is no transport.
- The exporter is the metrics twin of spec 002's: `OtlpHttpMetricExporterFactory::Create(options)`
  (URL `<endpoint>/v1/metrics`) or `OtlpGrpcMetricExporterFactory::Create(options)`, built from the
  same `OtlpConfig`, so one endpoint, one protocol, one set of headers from the environment. The
  SDK's `PushMetricExporter::Export` answers a real `ExportResult` for gRPC and the same
  answers-success-anyway for HTTP as the logs do, so the `QuietLogHandler` of spec 002 is the
  failure signal here too (one handler, one process).
- Temporality: **cumulative**. The base's counters are monotonic totals since load and its gauges
  are states; cumulative is what they are, and what Prometheus-shaped backends expect. `start_ts` is
  the moment `OtelMetrics` started, the same for every point of a run.

### The mapping (R2.1, R2.4)

| base | OTLP |
| --- | --- |
| `AuditCounters::Snapshot()` entry | `MetricData` with `InstrumentType::kCounter`, `SumPointData{ is_monotonic = true }`, one point per attribute tuple, the base's name unchanged (`acl.decisions`, `acl.denials`, `acl.audit.dropped`, ...) |
| `AuditGauges::Snapshot()` entry | `LastValuePointData`, `InstrumentType::kObservableGauge`, the base's name (`acl.sessions.live`, `acl.node.uptime`, `acl.door.state`, `acl.audit.contract`, ...) |
| unit / description | the base's, verbatim (`AuditMetric::unit` / `description`) |

A series the base stopped reporting (a gauge whose owner deregistered - a door that stopped) simply
stops appearing; nothing here remembers a value the node no longer has. Counters are never
re-derived from events (R2.4): the base's number IS the exported number.

### The histograms (R2.2)

Built here, from the events the sink receives - no second subscription, no second queue. The base
calls a sink only for the events its level records, so **a histogram sees what the level records**:
at `denied` there is no distribution of allowed statements, and the base's counters (which count
whatever the level) are the ones that still answer "how many". `Observe` runs on the base's audit
thread, never on a decision: O(bounds) and one short lock. Each is an `InstrumentType::kHistogram` with `HistogramPointData` (bounds, per-bucket counts,
sum, min, max), accumulated cumulatively under one small lock and emitted at every tick:

| instrument | from | attributes | default bounds |
| --- | --- | --- | --- |
| `acl.rewrite.duration` (us) | `rewrite_us` of statement/admin events | `kind`, `verdict` | 25, 50, 100, 250, 500, 1000, 2500, 5000, 10000 µs |
| `acl.session.duration` (s) | session close `duration_us` | `door`, `how` (the event's `detail`) | 1, 5, 15, 60, 300, 900, 3600, 14400, 86400 s |
| `acl.ingest.rows` (rows) | ingest completions' `rows` | `door` | 100, 1000, 10000, 100000, 1e6, 1e7 |

`acl_otel_histogram_buckets` is one JSON document (`{"acl.rewrite.duration": [25, 50, ...]}`),
parsed and refused at the SET like the level rules; an instrument the document does not name keeps
its defaults. R2.5's fallback - the `_sum` / `_count` pair as plain counters beside each histogram,
for a bridge that cannot ingest an OTLP histogram - is `acl_otel_histogram_sums` (default on: the
Azure bridge is the reason this exists).

### The opt-in series (R2.3)

`acl.decisions.by_role` (`role`), `.by_object` (`object`, `capability`), `.by_subject` (`subject`),
`.by_claim` (one claim named by `acl_otel_claim_dimension`, e.g. `tenant`) - counters we derive from
events, each off by default and switched on by name in `acl_otel_series` (`'by_role,by_claim'`).
Each has a cap: `acl_otel_max_series` (default 1000) distinct label values per instrument, first
come first kept, everything after folded into the single value `other`; `acl_otel_series_allowlist`
(a JSON map from instrument to values) pins the values an operator wants exact whatever the order of
arrival. The fold is counted (`folded` in the status) and never silent - a dashboard that shows
`other` climbing is the signal to raise the cap or narrow the series.

**A claim value leaves the node only here** (C5, R5.1) and only when the operator names the claim:
`acl_otel_claim_dimension = 'tenant'` says "tenant is a dimension of my dashboards". The value is a
label, never a log attribute (spec 005 decides that separately), and the cap applies to it like any
other.

### Configuration

| setting | default | what |
| --- | --- | --- |
| `acl_otel_metrics` | `true` | export metrics at all (logs are independent) |
| `acl_otel_metrics_interval` | `15` | seconds between scrapes |
| `acl_otel_histogram_buckets` | `''` | JSON: instrument → bounds |
| `acl_otel_histogram_sums` | `true` | also emit `_sum` / `_count` counters (R2.5) |
| `acl_otel_series` | `''` | the opt-in series, by name, comma separated |
| `acl_otel_claim_dimension` | `''` | the one claim `acl.decisions.by_claim` uses |
| `acl_otel_max_series` | `1000` | distinct label values per instrument before the `other` fold |
| `acl_otel_series_allowlist` | `''` | JSON: instrument → the values kept exact |

All `GLOBAL`, all `acl_otel_*`, all refusing a session scope (R9.1). The endpoint, protocol, TLS and
headers are spec 002's - one transport configuration for both signals.

### What the status says

`acl_otel_status()` grows a `metrics` object: `on`, `interval`, `exported` (ticks), `errors`,
`last_export_us`, `last_error`, `series` (how many points went out last tick), `folded` (label
values in `other`, per instrument), `histograms` (the instruments and their bucket counts).

### Application Insights (R2.5)

Dimensions per instrument stay at or below 10 by construction (the widest is `by_object` with two
plus the resource's); names are stable; values are bounded by the base's own rule and, for the
opt-in series, by the cap. Histograms reach `customMetrics` through the Collector's `azuremonitor`
exporter; where a bridge drops them, `acl_otel_histogram_sums` keeps a rate and a mean alive.

## Enforcement & security

- Nothing new on the decision path: the scrape runs on our thread, the histograms are folded in the
  worker that already has the batch, and both registries' `Snapshot()` are the base's own read-side
  contract (C7).
- A gauge reader belongs to the base and may take the store's lock; a tick therefore does what
  `acl_metrics()` does, no more, and never while holding a lock of ours.
- The only new value that leaves the node is the claim of `acl_otel_claim_dimension`, named by the
  operator, bounded by the cap.
- A failed export is counted and dropped; the node never blocks on the backend.

## Testing

- **C++** (`test_acl_otel_metrics.cpp`, Makefile-built: no SDK needed for the accumulation): the
  histogram accumulator - bucket edges inclusive-upper, sum/min/max, a value above the last bound in
  the overflow bucket; the series cap - the 1001st distinct role folds into `other` and is counted,
  an allowlisted value stays exact whenever it arrives; the bucket document parsed and refused.
- **C++ against the fake receiver** (extending spec 002's `acl_otel_test_otlp` target): one tick
  exported over HTTP and gRPC, decoded from the protobuf - the base's counters and gauges by name,
  attributes and unit; a histogram with its bounds and counts; the `_sum` / `_count` pair when the
  setting is on; cumulative temporality and a stable `start_ts` across two ticks.
- **sqllogictest** `acl_otel_metrics.test`: the settings refuse a session scope, a malformed bucket
  document is refused at the SET, the status carries the `metrics` object; beside acl - a burst of
  refusals moves `acl.denials` in `acl_metrics()` and the same number leaves as a point (R2.4,
  proven by the receiver test, asserted here as "the tick happened").
- The base's own numbers are the reference: any disagreement is a bug in this spec's code, never a
  second opinion.

## Alternatives considered

- **The SDK's `MeterProvider` with instruments we record into**: the natural OTel shape, and wrong
  here - the base's counters already exist and would be recorded twice (once by the base, once by
  us), with the SDK's own periodic reader as a second timer we do not control. Scraping a snapshot
  is what a bridge does.
- **Delta temporality**: smaller on the wire and what some backends prefer, but the base's counters
  are cumulative totals; converting means keeping our own previous-value map and inventing a reset
  policy the base does not have.
- **Deriving counters from events** rather than scraping: two sources of the same number, guaranteed
  to disagree the day an event is dropped (R2.4 exists to forbid exactly this).
- **A histogram of `acl.decisions` by object**: cardinality without a question behind it; the opt-in
  series answer the same need under a cap.

## Follow-ups

- Spec 006's `strict` mode covers metrics too (a node that refuses to serve when it cannot report).
- Exemplars (a trace id on a histogram bucket) once the SDK's exemplar API settles.
