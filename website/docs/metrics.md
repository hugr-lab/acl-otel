# Metrics

Every `acl_otel_metrics_interval` seconds (default 15) the extension sends acl's numbers and its own
over OTLP, with cumulative temporality. `acl_otel_metrics = false` turns this off, and
`acl_otel_metrics_flush()` sends one tick right away.

## acl's own numbers

acl's counters and gauges go out under the names acl gives them. None is re-derived here.

- **Counters:** decisions (`acl.decisions`), session opens, refusals and closes, audit events, drops
  and sink errors.
- **Gauges:** live sessions per door, the audit queue and ring fills, the policy version and its
  staleness, JWKS ages, the doors' state, the stream budget.

Their attributes come from bounded sets only.

## Histograms built here

acl deliberately keeps no histograms. The extension builds them from the events it already
receives:

| instrument | from |
| --- | --- |
| `acl.rewrite.duration` | a decision's rewrite cost |
| `acl.session.duration` | a session, from open to close |
| `acl.ingest.rows` | the rows of a bulk load |
| `acl.exec.duration`, `acl.exec.peak_memory` | an execution profile |
| `acl.exec.source.duration`, `.rows`, `.pushdown_filters` | the profile per source |

`acl_otel_histogram_buckets` replaces the bounds per instrument as JSON, for example
`{"acl.rewrite.duration": [25, 50, 100]}`. `acl_otel_histogram_sums` (default on) adds the
`_sum` / `_count` pair beside each histogram, for a bridge that cannot ingest histograms.

## Series by name

High-cardinality series exist only when the operator names them:

```sql
SET GLOBAL acl_otel_series = 'by_role,by_object';   -- also by_subject, by_claim
SET GLOBAL acl_otel_claim_dimension = 'tenant';     -- the one claim by_claim counts by
SET GLOBAL acl_otel_max_series = 1000;              -- distinct values before the `other` fold
SET GLOBAL acl_otel_series_allowlist = '{"acl.decisions.by_claim": ["acme", "globex"]}';   -- instrument -> values kept exact
```

These produce `acl.decisions.by_role` and its siblings.

- **The cap.** Past `acl_otel_max_series` distinct values, everything folds into one series named
  `other`, and the fold is counted.
- **The allowlist.** An allowlisted value is always kept exact and does not use up the cap.

## The extension's own numbers, and health

Each tick also carries `acl_otel.*` metrics, named apart from the node's:

- `.received`, `.exported`, `.dropped{why}`, `.export_errors`, `.queue_fill`;
- the span lane's numbers;
- `acl_otel.attached`;
- `acl_otel.healthy`.

With `acl_otel_strict` on, `healthy` is 0 while the node is losing events, and for
`acl_otel_health_window` seconds after the last loss (default 60). It is also 0 while the extension
is not attached at all.

- **Not a loss:** a drop caused by sampling, or an unsampled span.
- **A loss:** a failed export, or an event that arrived with nothing configured to send it.

`acl_otel_healthy()` answers the same question for a readiness probe. Strict never refuses a
statement, because acl emits after deciding.
