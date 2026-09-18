# Spec 009: the execution profile - the span everybody wanted, the sources as events, the tree as a record, the histograms

- **Status**: implemented (2026-09-18)
- **Date**: 2026-09-18
- **Author**: hugr lab

## Summary

The base now measures what happens *after* the decision: spec 074 (duckdb-acl) emits one `profile`
event per decided statement executed - its wall time, the rows and bytes, the memory peaks, a
rollup per attached source with the pushdown's shape, and the plan, bounded - linked to the
decision by `decision_seq`, and never carrying the statement's text. Audit contract v2. This spec
turns that event into the three signals this extension already speaks: an **execution span**
beside the decision span, under the request that caused both, with the sources as span events;
a **log record** whose body is the profile whole, the tree kept a tree; and **histograms** per
source - how long each attached catalog took, how many rows it returned, how much of the
predicate it took on itself. Off until the base profiles (its `acl_profile_level`, default `off`),
sampled together with the decision, and never a number the base did not measure.

## Problem

Spec 008 closed with the honest limitation: the decision span shows the gate took 0.3 ms; the
request took 4 s; between them nothing, because the base had no execution hook. It has one now
(`ClientContextState::QueryEnd`, spec 074), and the event it emits has both ends measured. What an
operator wants to see next to a slow request is now in the event: 3.7 s in `postgres_scan` of
catalog `pg`, two filters pushed, twelve million rows scanned, forty thousand returned. Today that
event reaches this extension as a log record with a generic body (`profile allowed`) and none of
its numbers as attributes, no span, no histogram - the shape of a decision, not of an execution.

## Design

### What the base hands us (contract v2)

Kind `profile`: `decision_seq` (the statement event's `seq`; -1 unlinked), `error` (the execution
failed; `detail` is the error's class, never its text), `exec_us` (wall time, begin to end),
`cpu_us` (thread time summed over operators - may exceed the wall time), `rows_scanned`, `rows_out`,
`bytes_read`, `bytes_written`, `peak_memory`, `memory_allocated`, `blocked_us`, `truncated` (the
plan was cut at the base's limits), `sources[]` (`source` = the attached database the rewrite
resolved the scan to, `kind` = the scanner, `scans`, `rows`, `rows_scanned`, `timing_us`, `bytes`,
`filters`, `projections`, `dynamic_filters`), `plan[]` (`id`, `parent`, `depth`, `type`, `kind`,
`source`, the same numbers, `peak_memory_observed`). Plus what every event carries: the statement
class, the objects, door, session, principal, trace context, node, `ts_us`. A number the base did
not measure is -1 and stays absent here, as R1.1 says for every field.

### The execution span (spec 008's lane)

| what | value | why |
| --- | --- | --- |
| candidate | a `profile` event, under the trace mode's rule (`linked`: a usable traceparent; `all`: any) | both ends measured: `exec_us` is begin to end |
| interval | `[ts_us - exec_us, ts_us]` | as the decision span, from the base's own stamps |
| name | `acl exec <STATEMENT>` - `acl exec SELECT` | beside `acl SELECT`, never the text |
| kind | `kInternal` | work inside the node |
| status | `kError` when the execution failed, with the error's **class** as the message; `kUnset` otherwise | a failed query is a failed span, whatever failed it; a class is bounded, a text is not (C5) |
| parent | the caller's span (the traceparent) - a **sibling** of the decision span, since the execution began after the decision ended; with no traceparent under `all`, the decision span itself, in the trace the decision rooted | a waterfall reads `[decision][execution]` under the request |
| link | to the decision span, `acl.link = decision` | the two are one statement; a link survives a backend that re-roots |
| attributes | R1.1's set (`FillAttributes`) plus `acl.decision_seq`, `acl.exec.us`, `acl.exec.cpu_us`, `acl.exec.rows_scanned`, `acl.exec.rows_out`, `acl.exec.bytes_read`, `acl.exec.bytes_written`, `acl.exec.peak_memory`, `acl.exec.memory_allocated`, `acl.exec.blocked_us`, `acl.exec.truncated`, `acl.exec.sources` (the rollup as a JSON list, bounded like `acl.objects`) | what a TraceQL query filters on |
| events | one `acl.source` per `sources[]` entry (attributes: `source`, `kind`, `scans`, `rows`, `rows_scanned`, `timing_us`, `bytes`, `filters`, `projections`, `dynamic_filters`) and one `acl.operator` per `plan[]` node (`id`, `parent`, `depth`, `type`, `kind`, `source`, `rows`, `rows_scanned`, `timing_us`, `bytes`, `peak_memory_observed`, `filters`, `projections`, `dynamic_filters`), all stamped `ts_us` - the moment the profile was collected, which is true | TraceQL finds `event.acl.source = "pg"`; the plan is bounded by the base (256 nodes) |
| verdict attribute | `acl.verdict` = `ok` / `error` for a profile | as the base's own ring reads it; `allowed` / `denied` are a decision's words |

**Linking needs an id both sides can compute.** Spec 008 gave a decision span a random id; the
execution event names its decision only by `decision_seq`. So a decision span's id becomes
**derived**: a 64-bit mix of the node id and `seq` (`SpanIdFor(node, seq)`), unique within a
node's run, which is what a span id must be - and the execution span links to
`SpanIdFor(node, decision_seq)` without ever having seen the decision. An orphan decision under
`all` roots a trace whose id is derived the same way (`TraceIdFor(node, seq)`), so its execution
lands in it. This amends spec 008's "fresh random" for decisions; the execution span's own id is
`SpanIdFor(node, seq)` of the profile event, by the same rule. Two runs of a node repeat their
`seq`, and so their ids - in different traces, which is where uniqueness is judged.

**`acl_otel_profile_spans`** (default `false`): the plan as **child spans** of the execution span,
one per operator, nested exactly as the plan's `parent` says, each `[ts_us - timing_us, ts_us]`
and labelled `acl.timing_kind = cumulative_thread_time`, because that is what the number is: a
scan on eight threads is eight seconds under a one-second statement, and a waterfall reader reads
intervals we do not have. Opt-in for that reason (the owner's decision, 2026-09-17); the events
above are the default because an event claims no interval.

**The caller's sampling flag** is obeyed as for any span; the base's own `sampled` level already
keys profiling on it, so a node at `sampled` profiles exactly the statements this lane records.

### The log record (spec 002's lane)

Every event is a record, and this one already reaches the lane. What changes is its shape:

- body: the profile **whole**, as one JSON document - the scalars, `sources`, `plan` - so the tree
  stays a tree in the one signal that keeps a document. The SDK's record body is an
  `AttributeValue` (a string, not a nested map), so the tree travels as text a backend parses
  (Loki, Elastic, the Collector's transforms all do); a nested `AnyValue` body is not in the SDK's
  Recordable, and a JSON string body is the honest shape available.
- severity: `INFO` for `ok`, `WARN` for `error` (something failed; not a refusal, not our side).
- attributes: R1.1's set plus the `acl.exec.*` scalars and `acl.decision_seq`, exactly as on the
  span; never the plan as an attribute (a 256-node plan is tens of KB, past the 8 KB a
  customDimension survives - the body carries it, the attribute would be silently cut).

**`acl_otel_profile_plan`** (default `true`): whether the plan rides at all - in the record's body
and as `acl.operator` span events. `false` keeps the rollup (`sources`) and drops the plan from both;
the volume knob for a node that profiles every statement. Both settings are GLOBAL, judged at the
SET, and take effect on the next batch.

### The histograms (spec 003's accumulators, R2.2)

| instrument | unit | labels | fed by |
| --- | --- | --- | --- |
| `acl.exec.duration` | us | `door`, `statement`, `result` (`ok` / `error`) | `exec_us` |
| `acl.exec.source.duration` | us | `source`, `kind` | each `sources[]` entry's `timing_us` |
| `acl.exec.source.rows` | rows | `source`, `kind` | each entry's `rows` |
| `acl.exec.source.pushdown_filters` | {filter} | `source`, `kind` | each entry's `filters` |
| `acl.exec.peak_memory` | By | `door` | `peak_memory` |

Label sets are bounded by the base's own rule: the doors, the statement classes, the attached
catalogs and the scanner kinds. The node is on every point already - the resource's
`service.instance.id` (R1.3), which is how OTLP says "which node" - so a fleet whose catalogs share
names still tells its nodes apart by resource, and a Prometheus bridge that flattens the resource
gets it as a label; no second `node` label is added to what the resource says. Buckets have
defaults (below) and follow `acl_otel_histogram_buckets` like the three of spec 003; `_sum` /
`_count` pairs follow R2.5.

Defaults: duration `{1000, 5000, 10000, 50000, 100000, 500000, 1e6, 5e6, 1e7, 6e7}` us; rows
`{100, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8}`; pushdown filters `{0, 1, 2, 4, 8, 16}`; peak memory
`{1 MiB, 16 MiB, 64 MiB, 256 MiB, 1 GiB, 4 GiB, 16 GiB, 64 GiB}` bytes.

### Sampling (spec 005)

A profile is sampled **with its decision**: `Sampler::Keep` judges a `profile` event by
`decision_seq` (the same mix, the same ratio for its roles), so the record, the decision span and
the execution span are kept or thinned as one statement, and two nodes agree. An unlinked profile
(`decision_seq` -1) is judged by its own `seq`. A profile whose execution **failed** is never
sampled away - it is the record the operator wants, as a refusal is.

### Status and self-metrics

`acl_otel_status()` gains `"profile": {"plan": <bool>, "spans": <bool>, "events": <n>}` - the two
settings in force and how many profile events the sink was handed (`events`, because the lanes'
`received` counts are what the existing checks match by name). The span lane's own numbers
count execution spans beside decision spans; nothing new is counted apart, because they are the
same lane with the same losses. `acl_otel.spans.*` (spec 006) is unchanged.

## Enforcement & security

- **Nothing new leaves the node** beyond what the base already put in the event (C5 holds at the
  base: no text, no literal, no path, no claim value in a profile; `detail` is an error's class).
  The attributes are R1.1's set plus numbers; the body is the event's own documents.
- **No interval is invented.** An operator child span carries cumulative thread time and says so;
  it is opt-in. A span event claims no interval. The execution span's ends are the base's stamps.
- **No parent is invented.** Under `linked` a profile without a usable traceparent is skipped,
  under `all` it hangs under its own decision in the trace that decision rooted; a link names a
  span id derived from what both sides know, never a guess.
- **The caller's sampling flag wins**, as in spec 008; the sampler keeps a profile and its
  decision together, and never thins a failed execution.
- **Volume is the operator's**: the base's `acl_profile_level` decides whether profiles exist at
  all, `acl_otel_profile_plan` whether the plan travels, `acl_otel_profile_spans` whether operators
  become spans, and the sampler thins the rest. A failed export is counted and dropped (R6.2).
- **Contract**: this reads the v2 fields and nothing else; a base at another revision never
  attaches (C2a), so a v1 base never hands us a half-filled event.

## Testing

- **C++, fake OTLP receivers** (`test_acl_otel_traces_otlp`): a profile event with a traceparent
  produces one execution span: the caller's trace id, the caller's span as its parent, id
  `SpanIdFor(node, seq)`, a link to `SpanIdFor(node, decision_seq)`, `[ts_us - exec_us, ts_us]`,
  `acl exec SELECT`, `kInternal`, `kUnset`; the `acl.exec.*` attributes; one `acl.source` event per
  source and one `acl.operator` per node with their attributes; a failed execution is `kError` with
  the class; the decision span's id is now `SpanIdFor(node, seq)` (deterministic across two
  exports); an orphan profile under `all` hangs under its decision's derived id in the derived
  trace; `acl_otel_profile_spans` yields one child span per operator nested as the plan says,
  labelled `cumulative_thread_time`; `acl_otel_profile_plan = false` drops the operator events.
- **The record** (`test_acl_otel_otlp`): the body is the JSON document with `sources` and `plan`,
  severity INFO / WARN, the exec attributes present, the plan absent from the attributes; with the
  plan off the body has no `plan`.
- **The histograms** (`test_acl_otel_metrics`): one profile with two sources shapes the five
  instruments with the labels above; the defaults exist; `acl_otel_histogram_buckets` retunes them.
- **The sampler** (`test_acl_otel_sampling`): a profile is kept iff its decision is; a failed one
  always; an unlinked one by its own seq.
- **The candidate rule** (`SpanCandidate`): a profile is a span under `linked` with a traceparent,
  under `all` without; never under `off`; the caller's `00` flag counts as unsampled.
- **Beside acl** (`ACL_EXT`, the round trip): `SET GLOBAL acl_profile_level = 'all'`, one real
  statement under `TRACE ... PARENT`: the receiver sees the decision span and the execution span in
  the caller's trace, the execution linked to the decision's id, with a `pg`-less but real
  `memory` source event. The SQL side (`acl_otel_beside_acl.test`) asserts the status counted the
  profile and the span lane exported two.
- **The settings** (`acl_otel_traces.test`): GLOBAL, judged at the SET, shown in the status.

## Alternatives considered

- **The tree as a nested body** - the SDK's Recordable takes an `AttributeValue`; a JSON string is
  what can travel, and every backend we deploy to parses it.
- **Operator spans by default** - cumulative thread time is not an interval; a span that reads as
  one lies where people read latency (spec 008's own rule). Opt-in, labelled.
- **A separate lane for profiles** - the same transport, the same losses, the same operator; a
  fourth queue buys separate counts and nothing else. The decision and the execution ride together.
- **A `node` label on the per-source histograms** - the resource already says it on every point;
  a second copy is what a bridge would then show twice.
- **Random span ids kept, the link dropped** - the link is the one thing that ties a 4 s execution
  to the 0.3 ms decision when a backend re-roots or the caller's span is gone. Derived ids cost
  nothing and are unique where a span id has to be.

## Addendum (2026-09-18): the profile level as a rule

The base's `SessionPolicy::ProfileFor(principal, door, out)` (spec 074) is answered by the same
rules that answer `LevelFor`: a rule in `acl_otel_level_rules` may carry `profile` = `off` /
`sampled` / `all` beside `level`, or instead of it; the central table (spec 007) carries it as an
optional `profile` column. Each lookup walks the rules that carry the level it asks for, first
match wins, so a rule about profiling never shadows one about the audit and the other way round.
Where it lands: the base's precedence is the operator's `acl_session_profile` override, then this
answer, then the connection's and the node's `acl_profile_level` - `acl_sessions()` says
`profile_source = policy` when a rule decided. Tested in the rules test (both lookups over mixed
rules, the row-to-rule mapping with the optional column), the central rules test (the column
added, a row read, a bad profile naming its row) and beside acl (a profile rule profiles the
analyst's statement while the node is off, and the base's listing says `policy`).

## Follow-ups

- The local stack: a Tempo query that shows the two spans, and a Grafana panel over
  `acl.exec.source.duration` by catalog.
