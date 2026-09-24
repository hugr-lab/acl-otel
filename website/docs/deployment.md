# Deployment

The node exports to an OpenTelemetry Collector, or to any OTLP endpoint. One endpoint carries all
three signals: `/v1/logs`, `/v1/metrics` and `/v1/traces` under the same host for HTTP, and one
`host:4317` for gRPC.

## Configure it the OpenTelemetry way

Leave the settings at their defaults and set the environment:

```sh
OTEL_EXPORTER_OTLP_ENDPOINT=https://collector.example.com:4318
OTEL_EXPORTER_OTLP_HEADERS=authorization=Bearer <token>
OTEL_EXPORTER_OTLP_CERTIFICATE=/etc/ssl/collector-ca.pem
```

Headers carry credentials, so they come only from the environment; no setting can hold them. A
Collector that faces a public network should terminate TLS on its receivers.

## Application Insights

[`deploy/otel-collector-azuremonitor.yaml`](https://github.com/hugr-lab/acl-otel/blob/main/deploy/otel-collector-azuremonitor.yaml)
is a reference Collector configuration. It forwards the records to Application Insights through the
contrib distribution's `azuremonitor` exporter. The mapping needs no rewrite:

| OpenTelemetry | Application Insights |
| --- | --- |
| `service.name` | `cloud_RoleName` |
| `service.instance.id` | `cloud_RoleInstance` |
| `acl.*` attributes | `customDimensions` |
| INFO / WARN / ERROR | `severityLevel` 1 / 2 / 3 |
| W3C trace context | `operation_Id` / `operation_ParentId` |

The connection string is a secret. Keep it in the Collector's environment.

## A local stack

[`deploy/local-stack/`](https://github.com/hugr-lab/acl-otel/tree/main/deploy/local-stack) runs five containers so you can see what a
node sends:

- an OpenTelemetry Collector;
- Loki for the logs;
- Prometheus for the metrics;
- Tempo for the traces;
- Grafana to look at all three.

```sh
cd deploy/local-stack && docker compose up -d     # Grafana on http://localhost:3000
```

```sql
LOAD acl;
LOAD acl_otel;
SET GLOBAL acl_otel_endpoint = 'http://127.0.0.1:4318';
SET GLOBAL acl_otel_service_name = 'acl-node-demo';
SET GLOBAL acl_otel_traces = 'linked';
SET GLOBAL acl_audit_level = 'all';
-- ... statements under a principal, some with TRACE '<id>' PARENT '<traceparent>' ...
SELECT acl_audit_flush(); SELECT acl_otel_flush(); SELECT acl_otel_metrics_flush(); SELECT acl_otel_traces_flush();
```

Where to look in Grafana:

- **Explore → Loki**: `{service_name="acl-node-demo"}` shows the decisions with their `acl.*`
  attributes.
- **Explore → Prometheus**: `acl_decisions_total` or `acl_otel_healthy`.
- **Explore → Tempo**: search by the trace id. The decision is a bar named `acl SELECT` inside the
  request.

## A readiness probe

With `acl_otel_strict` on, `SELECT acl_otel_healthy()` returns false while the node is losing
audit events or is not attached ([Metrics](metrics.md#the-extensions-own-numbers-and-health)). An
orchestrator can take such a node out of rotation.
