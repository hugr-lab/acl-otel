# Spec 008: OTLP traces - the decision as a span, hung under the request that caused it

- **Status**: draft
- **Date**: 2026-09-14
- **Author**: hugr lab

## Summary

The base already carries the client's trace context on every event (`traceparent`, `correlation_id`
- spec 069's `TRACE '<id>' [PARENT '<tp>']` markers, which every door composes). Spec 002 puts that
context on a log record, so a decision can be *found* from a request. This spec makes it a **span**,
so the decision can be *seen* in the request: a child of the caller's span, with the rewrite's own
duration, its verdict, and the objects it touched. One more OTLP signal on the transport that
already carries logs and metrics, off by default, and never a span that claims to be something it is
not.

## Problem

The base's own contract lists traces as a non-requirement, and gives the reason:

> Traces / spans per statement: the base emits at decision time and has no execution hook; a span
> needs both ends.

That is right about the statement and wrong as a conclusion about spans. The base does not know when
the *query* finished - it hands the rewritten statement to the binder and never hears the end - but
it knows exactly how long **its own decision** took, because it measures it: `rewrite_us` is on every
statement and admin event. A session knows both ends too: `duration_us` arrives on the close event.

So the question is not "can we have a span" but "which spans are honest". A span that claims to be
the statement's execution would be a lie told in a UI where people read latency; a span that is the
ACL decision is true, cheap, and is the thing an operator actually wants to see next to a slow
request - *was this the gate?*

Today the only way to answer that is to find the log record by correlation id and read `rewrite_us`
off it by hand.

## Design

### What becomes a span

| event | span | why |
| --- | --- | --- |
| `statement`, `admin` | **yes** - `[ts_us - rewrite_us, ts_us]` | both ends are known and measured |
| `session` close | opt-in - `[ts_us - duration_us, ts_us]` | both ends known; a span minutes or hours long is noise under a request trace, so it is a separate switch |
| `ingest`, `door`, `policy`, `keys` | no | one end only. They stay log records (spec 002) |

Nothing is derived, guessed or padded. An event whose duration the base does not measure does not
become a zero-length span, because a zero-length span in a waterfall reads as "instant", which is a
claim we cannot make.

### Building a span after the fact

The SDK's `sdk::trace::Recordable` is writable in any order and does not need a live `Tracer`, which
is what makes this possible at all: `SetIdentity(SpanContext, parent_span_id)`, `SetName`,
`SetStartTime`, `SetDuration`, `SetAttribute`, `SetStatus`, `SetSpanKind`, then straight to
`OtlpHttpExporter` / `OtlpGrpcExporter` (the trace pair of the exporters spec 002 already uses, same
endpoint family, `/v1/traces`). No `TracerProvider`, no `BatchSpanProcessor`, no sampler of the
SDK's - the same shape as spec 003's metrics, and for the same reason: we are re-exporting somebody
else's already-finished work, not instrumenting our own.

### Identity, and the flag we must honour

- `traceparent` parses exactly as it does for logs (R1.2, one parser, shared): version, 16-byte trace
  id, 8-byte span id, flags. Our span takes that trace id, and the incoming span id becomes our
  **parent**.
- Our own span id is 8 random bytes. It needs to be unique, not unguessable - so the ordinary RNG,
  never the base's crypto one (that one exists for session tokens, spec 063).
- **The `sampled` flag is the caller's decision and we obey it.** A `traceparent` with `00` flags
  means the caller's tracer is not recording this trace; a span of ours under it would be an orphan
  in most backends and a surprise in the bill. Not recorded, counted as sampled-out.
- `correlation_id` is **not** a trace id. It is an arbitrary string a gateway chose; hashing it into
  16 bytes would produce a trace that joins nothing. It rides as the attribute `acl.correlation_id`,
  exactly as on the log record.

### `acl_otel_traces` - off, linked, all

| value | meaning |
| --- | --- |
| `off` (default) | no spans, no trace exporter, nothing allocated |
| `linked` | only events carrying a usable `traceparent`: the span always has a parent, and a node nobody traces costs nothing |
| `all` | every recorded decision; without a parent it roots its own trace |

Default `off` because a third signal is a third bill, and an operator should turn it on deliberately.
`linked` is the value we recommend in the README: it is free on untraced traffic and complete on
traced traffic, which is what a request-level trace actually needs.

`acl_otel_session_spans` (default false) adds the session-close span, under the trace the session's
own record carries.

Settings are GLOBAL with a callback that refuses any other scope (C8), like every other one here.

### The span itself

- **Name**: `acl <statement class>` - `acl SELECT`, `acl MANAGEMENT`, `acl NATIVE`. The class is the
  base's bounded `statement` field, never the SQL text (C5). A session span is `acl session`.
- **Kind**: `kInternal`. The door's RPC is the server span and it is not ours; this is work inside
  the node.
- **Attributes**: the same bounded set the log record carries - `acl.verdict`, `acl.reason_code`,
  `acl.door`, `acl.session`, `acl.node`, `acl.roles`, `acl.subject`, `acl.issuer`, `acl.objects`,
  `acl.rewrite_us`, plus a claim only when the operator named it (R5.1). No new surface: whatever
  spec 002 is allowed to put on a record, this puts on a span, and nothing else.
- **Status**: `kUnset` for an allowed decision **and for a refusal by policy**; `kError` only when
  the reason code says our side failed (`source_error`, `policy_error`). This is the decision an
  operator's alerting hinges on, so it is written here rather than left to taste: a refusal is the
  system working. A dashboard that lit up red every time a role was told no would be retrained into
  ignoring red.

### Sampling

Spec 005's sampler decides, deterministically in `seq`, and the same rules hold: a refusal is never
sampled away, an admin decision is never sampled away. A record and its span are sampled together -
they are one decision, and a trace that disagrees with the log about whether something happened is
worse than either alone.

### Cost

The span is built on the audit thread, from an event the sink already has (R10.1: `OnEvent` stays
O(1) and does no I/O). Its own queue and batch, sized like the logs' (R10.3). `acl_otel_traces_flush()`
for tests and for a node about to exit. Self-metrics gain `acl_otel.spans` (`exported` / `dropped`
by why), beside the record and metric counters of spec 006.

## Enforcement & security

- **A span carries nothing a log record may not** (C5): no statement text, no claim value that is not
  allowlisted, no handle - the session's *ops id*, like everywhere else.
- **No parent is invented.** Without a usable `traceparent` we either skip (`linked`) or root a new
  trace (`all`); we never synthesise a trace id from a correlation id, because a fabricated join is
  worse than no join.
- **The caller's sampling decision wins.** We do not upgrade an unsampled trace.
- **Fail-closed on the transport**: an export that fails is counted and dropped, never retried across
  a restart, never buffered to disk (spec 002's rule, unchanged).
- **Version mismatch**: nothing here touches the contract struct, so this ships without a
  `CONTRACT_VERSION` bump. It reads `traceparent`, `rewrite_us` and `duration_us`, all of which the
  base has carried since spec 069.

## Testing

- **C++, fake OTLP receiver** (the pattern spec 002 and 003 already use, HTTP and gRPC): a statement
  event with a known `traceparent` produces one span whose trace id is the caller's, whose parent is
  the caller's span id, whose duration is `rewrite_us`, and whose attributes match the record's.
- **The flag**: the same event with `00` flags produces no span and a sampled-out count.
- **`linked` vs `all`**: an event with no `traceparent` produces nothing under `linked` and a root
  span under `all`.
- **Status**: a `capability` refusal is `kUnset`; a `source_error` refusal is `kError`.
- **Beside acl** (the two-loadable test, `ACL_EXT`): a real statement through the base under
  `ACL ... TRACE 'x' PARENT '<tp>'`, asserting the span the receiver sees hangs under `<tp>`. This is
  the one test that proves the marker, the base's event and our span agree - the others stub the
  event.
- **Local stack** (`deploy/local-stack`): the Collector config gains a `traces` pipeline and the
  README a line about seeing the decision inside a request in Grafana. The metrics pipeline was added
  after the same omission was found by hand (spec 003), so the check is: send one traced statement,
  look at the trace, see the span.

## Alternatives considered

- **A span for the statement's execution.** What everybody wants, and the base cannot give it: there
  is no execution hook, and inventing one means holding state from the decision until the result is
  closed - state the base deliberately does not keep, on a path that must stay O(1). If duckdb ever
  grows a statement-completed hook, this is the first customer.
- **Hashing `correlation_id` into a trace id.** Gives a span that joins nothing and looks like it
  joins something.
- **Using the SDK's `TracerProvider` and a real `Tracer`.** Would mean starting a span at decision
  time and ending it immediately, which buys the SDK's machinery and none of its value: we already
  know both timestamps.
- **On by default.** A third exporter that nobody asked for is a surprise in somebody's bill.

## Follow-ups

- If the base ever exposes an execution hook, revisit the alternative above - and the base's own
  non-requirement note, which points here.
- `acl_otel_session_spans` is the narrow case; if it proves noisy in practice, it can become a
  duration threshold instead of a boolean.
