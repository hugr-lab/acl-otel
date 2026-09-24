# Logs

Every audit event acl records reaches the collector as one OpenTelemetry log record. The exporter
batches and sends records from its own worker, over OTLP HTTP/protobuf or gRPC, using the
OpenTelemetry C++ SDK. acl's decision path never waits on the network.

## The record

- **Severity.**
  - INFO for an allowed event.
  - WARN for a refusal.
  - ERROR when the source of the decision failed rather than the principal: a `source_error` or
    `policy_error`, or a policy or keys event.
- **Body.** `<kind> <allowed|denied>` followed by `: <reason>`. A refusal's reason is a fixed
  sentence, so it never contains a claim value or statement text. For an execution profile the body
  is the profile as one JSON document ([Traces](traces.md#execution-profiles)).
- **Trace context.** Taken from the statement's `traceparent` when it parses. A malformed value sets
  nothing.
- **Resource.**
  - `service.name` comes from `acl_otel_service_name`, default `duckdb-acl`;
  - `service.instance.id` is the node's id (acl's `acl_node_id`);
  - `acl_otel_resource_attributes` adds `k=v` pairs.

**Attributes.** Every field is an `acl.*` attribute, present only when the event carries it:

- the event: `acl.kind`, `acl.statement`, `acl.verdict`, `acl.reason_code`, `acl.reason`, `acl.detail`;
- where and who: `acl.door`, `acl.session`, `acl.subject`, `acl.issuer`;
- `acl.roles` and `acl.objects` as JSON arrays. A list is cut at whole elements, and the last
  element says how many did not fit;
- the trace: `acl.correlation_id`;
- measurements: `acl.rewrite_us`, `acl.duration_us`, `acl.rows`;
- `acl.level`, `acl.seq`, `acl.node`;
- for a profile: `acl.decision_seq` and `acl.exec.*`.

A record never carries rows, statement text or keys. It carries claim values only when the
operator names the claim.

## Claims

A claim's value leaves the node only when its name is listed:

```sql
SET GLOBAL acl_otel_claim_attributes = 'tenant,department';   -- as acl.claim.tenant, acl.claim.department
```

You can list at most sixteen names, and each value is cut at 256 bytes.

## What was lost

Every drop is counted, never silent. `acl_otel_status()` separates the causes:

- the queue was full;
- no exporter was configured;
- the backend refused the export, with the error the SDK gave (`last_error`).

The SDK's HTTP exporter reports success even when its client failed, so an error the SDK logged
during an export is treated as the failure. Export failures are never retried after a restart.
