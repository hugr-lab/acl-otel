# Spec 002: OTLP logs - every audit event becomes one OpenTelemetry log record

- **Status**: implemented
- **Date**: 2026-09-08
- **Author**: hugr lab

## Summary

Spec 001 left the transport out: the sink batches events and hands them to an `Exporter`, and the
one that stands in counts what it drops. This spec plugs OTLP in. Every event the base delivers
becomes one OpenTelemetry `LogRecord` (R1.1) - timestamp, severity from the verdict, a body of
`<kind> <verdict>` and the reason, every field an `acl.`-prefixed attribute - with the W3C trace
context from `traceparent` (R1.2) and the node's resource attributes (R1.3), exported over OTLP
**HTTP/protobuf** or **gRPC** (R1.4) by the OpenTelemetry C++ SDK from vcpkg (§5 of the contract).
Endpoint, protocol and TLS come from `acl_otel_*` settings and the standard `OTEL_EXPORTER_OTLP_*`
environment (settings win); **headers - the place a token lives - come from the environment only**
(R9.2: never from a setting `current_setting()` could show). The SDK's exporter is driven directly
from our worker, batch by batch (R1.5): no SDK processor, no second queue, no second thread - the
sink of spec 001 keeps its semantics, and an export that fails is counted as spec 001 counts it.
Attributes are flat and stable and `acl.objects` is a JSON string, which is what makes the records
read naturally in Application Insights through the Collector's `azuremonitor` exporter (R1.6).

## Problem

An audit trail nobody reads is a promise, not a control. The base records decisions into a ring
and a file (spec 069); a fleet's operators read OpenTelemetry - a Collector, Grafana, Azure
Monitor - and want the node's decisions there, each as a record they can filter by verdict, by
door, by subject, joined to the request that caused it by its trace. The mapping has to be one
the backend can index (flat attributes, bounded names), the transport one every backend speaks
(OTLP), and the credentials must not leak through the very audit they authenticate.

## Design

### The exporter

- `OtlpExporter : Exporter` (`src/acl_otel_otlp.cpp`) wraps one SDK `LogRecordExporter` -
  `OtlpHttpLogRecordExporter` or `OtlpGrpcLogRecordExporter`, by protocol - and drives it from the
  sink's worker: for each batch, one `Recordable` per event (`MakeRecordable()`), filled from the
  event, exported in one `Export(span)` call. **The SDK's verdict is not enough**: its HTTP log
  exporter (1.24) answers `kSuccess` whatever its client said - a refused connection or a 4xx from
  the collector reaches only the SDK's internal log. So the exporter installs the SDK's global log
  handler once per process (`QuietLogHandler`: keeps the FIRST error line since the last export,
  drops everything else - a database server does not write to stderr) and an error the SDK logged
  during the call IS the failure: counted as spec 001 counts an export error, its text the
  `last_error`. The SDK's own retry policy runs inside that call; ours adds nothing on top - a batch
  that failed is counted and dropped, never replayed (§6 of the contract: the backend is the
  durable copy).
- Created and swapped by the settings' callbacks: each transport setting's callback calls
  `OtelState::Reconfigure(db, setting, value)` with the value being set (a callback runs before
  the value is stored), which builds the exporter from the settings and hands it to the sink
  (`SetExporter`: a batch in flight finishes on the transport it started with); an empty endpoint
  with no `OTEL_EXPORTER_OTLP_ENDPOINT` / `_LOGS_ENDPOINT` in the environment → the stand-in.
  `acl_otel_start()` after a stop rebuilds it from the settings; a config the SDK refuses (a bad
  protocol) fails the SET. The callbacks are plain function pointers at our pin, so the seven
  transport settings are seven instances of one template (`TransportSet<SETTING>`).
- `acl_otel_flush()`: export what is queued now and wait for it (bounded by the sink's 2 s), true
  when it drained - before a shutdown, or to read in the status what the collector answered.
- Ownership: the exporter holds the SDK objects; the sink holds the exporter by `shared_ptr`; the
  worker is the only caller. Shutdown = `Stop` (spec 001) then the exporter's destruction, which
  closes the SDK client.

### The mapping (R1.1 - R1.3)

| record field | from |
| --- | --- |
| timestamp | `ts_us` |
| observed timestamp | export time |
| severity | `allowed` → INFO; `!allowed` → WARN; `reason_code = source_error`, or `kind ∈ {policy, keys}` with `!allowed` → ERROR |
| body | `<kind> <allowed|denied>` + `: <reason>` when there is one |
| trace id / span id | `traceparent` (`00-<32 hex>-<16 hex>-<2 hex>`, W3C); a malformed value sets neither and is not exported as an attribute |
| attributes | `acl.kind`, `acl.statement`, `acl.verdict`, `acl.reason_code`, `acl.reason`, `acl.door`, `acl.session`, `acl.subject`, `acl.issuer`, `acl.roles` (a JSON array string), `acl.objects` (a JSON array of `{name, capability}`, as a string), `acl.correlation_id`, `acl.rewrite_us`, `acl.rows`, `acl.duration_us`, `acl.detail`, `acl.level`, `acl.seq`, `acl.node` - each only when the event carries it (an empty string or a `-1` is absent, not exported) |
| resource | `service.name` (`acl_otel_service_name`, default `duckdb-acl`), `service.instance.id` (the base's `acl_node_id` when acl is loaded, else `<hostname>:<pid>`), `duckdb.version`, `acl_otel.version`, plus `acl_otel_resource_attributes` (`k=v,k=v`) |
| instrumentation scope | `acl_otel`, its version |

Claim values (`principal.claims`) are **not** exported in this spec - spec 005's allowlist is the
only way one ever is (C5, R5.1).

### Transport and configuration (R1.4, R9.2)

- `acl_otel_endpoint`: the base URL, `http://host:4318` or `https://…` for HTTP/protobuf
  (`/v1/logs` appended, the OTLP convention), `host:4317` for gRPC; `acl_otel_protocol`:
  `http/protobuf` or `grpc`; `acl_otel_timeout` (seconds); `acl_otel_insecure` (gRPC without TLS);
  `acl_otel_certificate` (a CA file path for TLS).
- **A setting at its default means the standard environment decides**: the SDK's option defaults
  read `OTEL_EXPORTER_OTLP_ENDPOINT` / `_LOGS_ENDPOINT`, `_HEADERS` / `_LOGS_HEADERS`, `_TIMEOUT`,
  `_CERTIFICATE`, `_INSECURE`, and we read `_PROTOCOL` / `_LOGS_PROTOCOL` ourselves (the exporter
  class is chosen here; `http/json` is served as protobuf - what every collector takes). So the
  defaults are `''` / `0` / `false`, not values of their own: a container configured the
  OpenTelemetry way exports without a SET, and a setting that is set wins over its variable.
  `RESET` runs the same callback with the default, so it is the environment's value again.
- The status masks a credential written into the endpoint URL (`scheme://***@host`), an operator's
  choice against R9.2 that still never prints.
- **Headers only from the environment** (`OTEL_EXPORTER_OTLP_HEADERS`, `k=v,k2=v2`): a bearer token
  for a Collector or a vendor is a secret, and no `acl_otel_*` setting carries one - a setting is a
  `current_setting()` away (the base denies that to a principal, but an operator's `SELECT` should
  not print a token either). `acl_otel_status()` reports which header **names** are configured,
  never a value. A duckdb-secret form (`CREATE SECRET … TYPE acl_otel`) is a follow-up.

### What the status says (R7.1)

`exporter` becomes `otlp/http -> <url>` (the `/v1/logs` URL as sent) / `otlp/grpc -> <endpoint>`;
`headers` lists the configured header names; `last_error` the last export failure's text - ours
(`the export failed (<exporter>)`) with the SDK's first error line after it, e.g. the connection
refused or the HTTP status the collector answered. The counters of spec 001 apply unchanged.

### Application Insights (R1.6)

By construction: `service.name` → `cloud_RoleName`, `service.instance.id` → `cloud_RoleInstance`,
flat `acl.*` attributes → `customDimensions`, severity INFO/WARN/ERROR → `severityLevel` 1/2/3,
`traceparent` → `operation_Id` / `operation_ParentId`. `deploy/otel-collector-azuremonitor.yaml`
ships the reference Collector configuration (receiver otlp, exporter azuremonitor with the
connection string from the environment); wiring an integration test through it is spec 006's.

### Build

`opentelemetry-cpp[otlp-http,otlp-grpc]` from vcpkg through the merged-manifest flow (the base's
Makefile pattern: `make vcpkg-setup`, the fail-fast guard naming the goals that need a toolchain).
grpc, protobuf, abseil and curl are what the base's Flight door already builds, so a developer box
with the base's vcpkg checkout reuses its binary cache (`VCPKG_TOOLCHAIN_PATH=…/duckdb-acl/vcpkg/…`).
The loadable links the SDK statically, as the base links Arrow. wasm stays excluded. CI (`ci.yml`)
bootstraps vcpkg and keeps its binary cache in `actions/cache` keyed on `.gitmodules` + `vcpkg.json`,
pruned of two-week-old archives before each save; the distribution workflow gets the same through
extension-ci-tools.

## Enforcement & security

- Nothing on the decision path changes: the sink's `OnEvent` is spec 001's; the SDK runs on our
  worker only.
- The event is safe by the base's construction (C5) and this mapping adds nothing to it: no claim
  values, no statement text, no physical names beyond what the base put in `reason`. `acl.reason`
  is the base's own refusal text, already bounded (spec 069).
- Credentials: never in a setting; the environment is the process owner's; the status prints names,
  not values.
- A failed export is counted and dropped - the node never blocks on the backend, and never buffers
  across a restart (§6).
- The SDK's log is process-wide: an error another exporter logs during an export - the old
  transport shutting down at a reconfigure - is attributed to the batch in flight, which is then
  counted as failed although it arrived. A mis-count of one batch at a SET, never a lost event;
  the counter is the operator's signal, not an accounting.

## Testing

- **C++ (`make test-cpp`; the CMake target `acl_otel_test_otlp`, built on demand, because the test
  links the SDK, its protobuf classes and gRPC - the Makefile-compiled tests of spec 001 stay as
  they are)**: `test_acl_otel_otlp.cpp` runs a fake OTLP receiver in-process - HTTP/protobuf on
  the bundled httplib (`POST /v1/logs`, the request decoded with the SDK's protobuf classes) and
  gRPC on the SDK's generated `LogsService` - and exports a synthetic batch through `OtlpExporter`
  against each:
  - every attribute of R1.1 for an allowed statement, a denied one, a `keys refresh_failed`
    (severity ERROR), a session close (`duration_us`), an ingest (`rows`); what an event does not
    carry is absent; a claim value is never there;
  - the trace context: a valid `traceparent` sets trace and span id, a malformed one sets neither;
  - the resource: `service.name`, `service.instance.id`, `duckdb.version`, the extra attributes; the
    scope `acl_otel`;
  - headers from `OTEL_EXPORTER_OTLP_HEADERS` reach the receiver (HTTP); the status names them, not
    the values;
  - a port nobody listens on, over each protocol: `Export` returns false with the SDK's reason in
    the text; a protocol outside the two is refused at construction.
- **sqllogictest** `acl_otel_otlp.test` (alone): `SET GLOBAL acl_otel_endpoint` to a closed port
  switches the status to `otlp/http -> …/v1/logs`, the protocol switch rebuilds on the endpoint as it
  stands, a URL that names the path is kept, `''` goes back to the stand-in with the other settings
  intact; `acl_otel_protocol` refuses anything but the two values; the settings refuse a session
  scope. `acl_otel_beside_acl.test` (with `ACL_EXT`): a session opened under the base with the
  endpoint on a closed port, `acl_otel_flush()` true, the status counts `export_errors` and carries
  the SDK's `127.0.0.1 port 1` reason; the flush answers false after `acl_otel_stop()`. CI runs it
  on every PR against the base's own artifact (`acl-linux_amd64` of duckdb-acl's latest green
  `main` run), so the two loadables on one instance - the whole point of spec 001 - is what is
  proven, not assumed.
- The base's `rewrite_cost.py` "with audit" column (R10.1) is unchanged by construction: nothing of
  this runs on the base's thread.

## Alternatives considered

- **The SDK's `LoggerProvider` + `BatchLogRecordProcessor`**: a second queue and a second thread
  beside spec 001's, with the SDK's own drop policy - two places to lose an event and count it;
  driving the exporter from our worker keeps one.
- **Vendoring the SDK**: rejected in the contract (§5); vcpkg it is, and the base's cache makes it
  cheap.
- **A `acl_otel_headers` setting** for convenience: rejected - R9.2.
- **JSON over HTTP** (`OTEL_EXPORTER_OTLP_PROTOCOL=http/json`): protobuf is what every Collector
  accepts and what the Azure bridge expects; JSON is a debugging aid at best.

## Follow-ups

- The SDK's HTTP log exporter answering success on a failed export is worth an upstream issue
  (opentelemetry-cpp 1.24, `OtlpHttpLogRecordExporter::Export` returns `kSuccess` after logging
  the client's failure); the log-handler rule here stays right whichever way it goes.

- Spec 003: metrics over the same exporter family (`OtlpHttpMetricExporter` / gRPC).
- The duckdb-secret form for headers (`CREATE SECRET … TYPE acl_otel`).
- `OTEL_EXPORTER_OTLP_LOGS_*` per-signal overrides beyond what the SDK reads on its own.
