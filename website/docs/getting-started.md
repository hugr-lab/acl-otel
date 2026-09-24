# Getting started

## Load it beside acl

acl-otel is a loadable extension for the same DuckDB build as acl. Load both on the node:

```sql
LOAD acl;
LOAD acl_otel;
```

On load it attaches to acl's audit registry. `acl_otel_stop()` and `acl_otel_start()` detach and
re-attach it; DuckDB never unloads an extension.

## Point it at a collector

```sql
SET GLOBAL acl_otel_endpoint = 'http://otel-collector:4318';   -- /v1/logs, /v1/metrics, /v1/traces are appended
-- or gRPC: SET GLOBAL acl_otel_endpoint = 'otel-collector:4317'; SET GLOBAL acl_otel_protocol = 'grpc';
```

Every setting at its default means that the standard OpenTelemetry environment decides
(`OTEL_EXPORTER_OTLP_ENDPOINT`, `_PROTOCOL`, `_HEADERS`, …). A container that is already configured
the OpenTelemetry way needs no `SET`. **Headers come from the environment only**, so no setting ever
carries a bearer token.

## See it work

```sql
SET GLOBAL acl_audit_level = 'all';        -- acl's own level (off / denied / decisions / all)
-- ... statements under a principal ...
SELECT acl_audit_flush();                  -- acl hands its queue to the sinks
SELECT acl_otel_flush();                   -- we export what is queued, and wait
SELECT acl_otel_status();                  -- what arrived, what was dropped and why
```

The status is one JSON document. It shows whether the extension is attached, the transports, the
queue, every counter, the metrics, health, where the rules came from, the span lane, and tresor.

To look at the records, metrics and spans instead of reasoning about them, the repository has a local
stack: a Collector, Loki, Prometheus, Tempo and Grafana. See [Deployment](deployment.md#a-local-stack).

## Next

- Audit some roles more than others: [Levels and rules](levels-and-rules.md).
- Turn on spans: `SET GLOBAL acl_otel_traces = 'linked'` ([Traces](traces.md)).
