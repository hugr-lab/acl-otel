# Traces

A decision becomes a span, hung under the request that caused it. The span is built after the fact
from what acl measured. It is never a guess, and never zero-length.

```sql
SET GLOBAL acl_otel_traces = 'linked';   -- off (default) | linked | all
```

The modes:

- **`linked`** (recommended) takes every decision whose statement carries a usable `traceparent`.
  - The span's trace id is the caller's, and the caller's span is its parent.
  - It is named after the statement class (`acl SELECT`, `acl MANAGE`, …) and lasts as long as the
    rewrite took (`rewrite_us`).
  - It costs nothing on untraced traffic.
- **`all`** also roots a trace of its own for a decision with no parent.
- **`off`** allocates nothing.

A statement carries its trace through the prefix markers `TRACE '<id>' PARENT '<traceparent>'`. Every
door composes them from the client's `traceparent` / `x-correlation-id` headers or its session
settings.

- **The caller decides sampling.** A `00` sampled flag is obeyed and counted as `unsampled`; it is
  never upgraded.
- **Status.** A refusal by policy is the system working, so its status is `kUnset`. Only a failure
  on our side (`source_error`, `policy_error`) is `kError`.
- **Ids.** No trace id is ever invented from a correlation id.
- **Session spans.** `acl_otel_session_spans` adds a span per session, from open to close. It is a
  separate switch because a span minutes long is noise under a request.

## Execution profiles

With profiling on in acl (`acl_profile_level`, or a rule's `profile`), each executed statement also
produces a profile event, and the extension turns it into signals:

- **An execution span** `acl exec <CLASS>`, over `[ts_us - exec_us, ts_us]`.
  - It is a sibling of the decision span under the caller's span, and linked to it.
  - Span ids are derived from the node and the event's sequence, so the link needs no state.
- **Span events.** `acl.source` for each source the statement read, and `acl.operator` for each plan
  node when `acl_otel_profile_plan` is on (the default).
- **A record** whose body is the whole profile as JSON, with the plan kept as a tree.
- **Histograms:** `acl.exec.*` with `source` / `kind` labels ([Metrics](metrics.md)).
- **Operator spans (opt-in).** `acl_otel_profile_spans` makes each plan node a child span. It is
  opt-in because an operator's number is summed thread time, not an interval.

The sampler keeps or drops a profile together with its decision, and never drops a failed one.
