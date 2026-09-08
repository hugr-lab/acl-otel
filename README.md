# acl_otel

OpenTelemetry for [duckdb-acl](https://github.com/hugr-lab/duckdb-acl)'s audit: a duckdb extension
loaded beside `acl` on the same instance that turns the base's audit events and counters into OTel
**logs** and **metrics**, decides the audit level **per role, per user and per door**, samples,
and reports its own health. It contains no enforcement, changes no decision, and can never slow or
stop one.

It compiles against one header of the base, `acl_audit.hpp`, and reaches the base through duckdb's
object cache (spec 069 of duckdb-acl). The contract it meets is
[`specs/069-audit/extension-requirements.md`](https://github.com/hugr-lab/duckdb-acl/blob/main/specs/069-audit/extension-requirements.md)
in the base; what is built so far is in [`specs/`](specs/README.md).

## Status

- **Spec 001 (the wiring)**: the sink and the level policy attach to the base's registry in either
  load order (a registry stamped by another revision of the contract header is refused, and the
  status says so - `attach_error`), `acl_otel_level_rules` decides a session's level (first match: role / subject /
  issuer / door), `acl_otel_status()` reports the extension's own numbers, `acl_otel_stop()` /
  `acl_otel_start()` detach and re-attach.
- **Spec 002 (OTLP logs)**: every audit event is one OpenTelemetry log record - severity from the
  verdict, a body of `<kind> <verdict>: <reason>`, every field an `acl.*` attribute, the W3C trace
  context from `traceparent`, the node as the resource - exported over OTLP HTTP/protobuf or gRPC by
  the OpenTelemetry C++ SDK, batch by batch from the extension's own worker. A failed export is
  counted and named in the status, never retried across a restart.
- Not yet: metrics (spec 003), per-connection logging (004), enrichment and sampling (005), the
  self-metrics and `strict` mode (006), rules kept in the policy catalog (007).

```sql
LOAD acl;
LOAD acl_otel;
SET GLOBAL acl_otel_endpoint = 'http://otel-collector:4318';   -- /v1/logs is appended; 'host:4317' + protocol grpc
SET GLOBAL acl_otel_level_rules = '[{"role": "analyst", "door": "flight", "level": "all"}, {"level": "denied"}]';
SELECT acl_otel_status();
SELECT acl_otel_flush();      -- export what is queued now and wait for it
```

### Settings (all `GLOBAL`, all `acl_otel_*`)

| Setting | Default | What |
| --- | --- | --- |
| `acl_otel_endpoint` | `''` | `http(s)://host:4318` for http/protobuf, `host:4317` for grpc; `''` = `OTEL_EXPORTER_OTLP_ENDPOINT` / `_LOGS_ENDPOINT`, and with neither nothing is exported (counted as dropped) |
| `acl_otel_protocol` | `''` | `http/protobuf` or `grpc`; `''` = `OTEL_EXPORTER_OTLP_PROTOCOL`, else http/protobuf |
| `acl_otel_timeout` | `0` | seconds an export may take; `0` = `OTEL_EXPORTER_OTLP_TIMEOUT`, else the SDK's 10 |
| `acl_otel_insecure` | `false` | grpc without TLS; `false` = the SDK decides (the endpoint's scheme, `OTEL_EXPORTER_OTLP_INSECURE`) |
| `acl_otel_certificate` | `''` | a CA certificate file for TLS; `''` = `OTEL_EXPORTER_OTLP_CERTIFICATE`, else the system's |
| `acl_otel_service_name` | `duckdb-acl` | the resource's `service.name`; `service.instance.id` is the base's `acl_node_id` |
| `acl_otel_resource_attributes` | `''` | extra resource attributes, `k=v,k=v` |
| `acl_otel_level_rules` | `''` | a JSON array of `{role, subject, issuer, door, level}` rules, first match wins |
| `acl_otel_queue_size` / `_batch_size` / `_flush_interval` | `10000` / `512` / `5` | the sink's queue, batch and flush seconds (at the next `acl_otel_start()`) |

A setting at its default means the standard environment decides, so a container configured the
OpenTelemetry way needs no SET; a setting that is set wins over its variable.
**Headers - a bearer token for a collector or a vendor - come from the environment only**
(`OTEL_EXPORTER_OTLP_HEADERS=authorization=Bearer …,x-tenant=…`): no setting carries a secret, and
the status lists header names, never values. `deploy/otel-collector-azuremonitor.yaml` is a
reference Collector configuration that forwards these records to Application Insights.

## Building

```sh
git submodule update --init --recursive
make vcpkg-setup                     # once: the OpenTelemetry SDK (otlp-http, otlp-grpc) comes from vcpkg
GEN=ninja make                       # release build of duckdb + the extension
build/release/test/unittest 'test/sql/*'
GEN=ninja make test-cpp              # the standalone tests + the OTLP test against in-process receivers
ACL_EXT=/path/to/acl.duckdb_extension build/release/test/unittest test/sql/acl_otel_beside_acl.test
```

A box that already has duckdb-acl's vcpkg checkout can point at it instead of a second one -
`VCPKG_TOOLCHAIN_PATH=…/duckdb-acl/vcpkg/scripts/buildsystems/vcpkg.cmake GEN=ninja make` - and
reuse its binary cache (grpc, protobuf, abseil and curl are the same ports the Flight door builds).

The pins ride duckdb-acl's: duckdb's 2.0 release branch `v2.0-cyanoptera`, extension-ci-tools
`main`, the base's `main` for the contract header (`duckdb-acl/` submodule, headers only).

## License

Business Source License 1.1 - see [LICENSE](LICENSE). Production use is permitted except as a hosted
or managed service offered to third parties; each version becomes Apache-2.0 four years after its
release. The parameters (Additional Use Grant, Change Date, Change License) are the licensor's and
may be revised before the first release.
