# A local stack: see what this extension actually sends

Four containers - an OpenTelemetry Collector, Loki for the logs, Prometheus for the metrics and
Grafana to look at both - so the records and numbers a node produces can be *seen* rather than
reasoned about. Nothing here is a deployment recipe; it is the bench used to check specs 002, 003
and 006 by eye, kept so the next person does not rebuild it.

```sh
cd deploy/local-stack && docker compose up -d     # Grafana on http://localhost:3000 (anonymous admin)
```

Point a node at it and make some decisions:

```sql
LOAD acl;
LOAD acl_otel;
SET GLOBAL acl_otel_endpoint = 'http://127.0.0.1:4318';
SET GLOBAL acl_otel_service_name = 'acl-node-demo';
SET GLOBAL acl_audit_level = 'all';
-- ... your catalog, roles and grants, then some statements under a principal ...
SELECT acl_audit_flush();      -- the base hands its queue to the sinks
SELECT acl_otel_flush();       -- we export what is queued, and wait
SELECT acl_otel_metrics_flush();
```

In Grafana: **Explore → Loki**, `{service_name="acl-node-demo"}` for the decisions, each with its
`acl.*` attributes; **Explore → Prometheus**, `acl_decisions_total` or `acl_otel_healthy` for
the numbers. A statement sent with `TRACE '<id>' PARENT '<traceparent>'` carries its trace context
onto the record, which is what ties an ACL decision to the request that caused it.

## What this bench found

- **`unit: "1"` became a `_ratio` suffix** - fixed on both sides the same day, and the reason this
  bench exists. The OTel-to-Prometheus convention turns a gauge whose unit is `1` into
  `<name>_ratio`, so `acl.node.info` arrived as `acl_node_info_ratio` and `acl.sessions.live` as
  `acl_sessions_live_ratio` - neither is a ratio, and a dashboard built on those names reads wrong.
  Thirteen metric names were affected, ten of the base's (eleven registrations - `acl.door.state`
  is registered by each door) and three of ours. Each is fixed where it is
  produced: a UCUM annotation (`{session}`, `{door}`, `{stream}`, `{event}`) where the number counts
  something, no unit at all where it is a flag or a version. Counters keep `"1"` - the same
  convention gives a monotonic sum `_total`, which is the name everybody already uses.
- **`otel/opentelemetry-collector-contrib:0.116.0` does not run on arm64** - the image is tagged
  `linux/arm64` and its binary fails to exec. The Azure reference config used to name that version;
  it names a working one now.

## Azure instead of Grafana

`../otel-collector-azuremonitor.yaml` is the same Collector pointed at Application Insights. The
mapping was verified against a stand-in ingestion endpoint: `service.name` → `ai.cloud.role`,
`service.instance.id` → `ai.cloud.roleInstance`, every `acl.*` attribute → `customDimensions`,
`denied` → `severityLevel: 2`, and the W3C trace context → `ai.operation.id` / `ai.operation.parentId`.
