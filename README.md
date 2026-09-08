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
- **Spec 003 (OTLP metrics)**: the base's counters and gauges scraped every
  `acl_otel_metrics_interval` and pushed under their own names, three histograms built here from the
  events (rewrite duration, session duration, ingest rows) with bounds an operator may replace, and
  the high-cardinality series (by role, object, subject, one claim) that exist only when named -
  each capped, with everything past the cap folded into `other` and counted.
- **Spec 005 (enrichment and sampling)**: a claim value reaches a record only when the operator
  names its claim (`acl_otel_claim_attributes`, as `acl.claim.<name>`, bounded at sixteen names and
  256 bytes each), and `allowed` statements may be thinned by ratio - overall or per role - while
  refusals, sessions, doors, policy and keys events never are. Sampling is deterministic in the
  event's sequence number, so two nodes with the same setting keep the same statements, and the
  counters and histograms still see everything.
- **Spec 006 (its own numbers, and health)**: every tick also carries `acl_otel.received`,
  `.exported`, `.dropped{why}`, `.export_errors`, `.queue_fill`, `.attached` and `.healthy` - named
  apart from the node's own. `acl_otel_strict` gives the last one teeth: a node losing audit events,
  or not attached to the base's registry at all, reports `healthy = 0` for
  `acl_otel_health_window` seconds after the last loss, and `acl_otel_healthy()` answers the same
  for a readiness probe. Strict never refuses a statement - the base emits after the decision, so
  there is nothing left to refuse.
- **Spec 007 (the rules a fleet writes once)**: point `acl_otel_rules_table` at a table
  (`acl_otel_create_rules_table()` creates it, by hand, once) and every node reads it every
  `acl_otel_rules_interval` seconds on a connection of its own. A failed or malformed read leaves
  the rules in force and says so in the status; `acl_otel_level_rules` wins while it is set, for a
  node an operator is debugging.
- Not yet: per-connection logging (004), which has one design decision open.

```sql
LOAD acl;
LOAD acl_otel;
SET GLOBAL acl_otel_endpoint = 'http://otel-collector:4318';   -- /v1/logs is appended; 'host:4317' + protocol grpc
SET GLOBAL acl_otel_level_rules = '[{"role": "analyst", "door": "flight", "level": "all"}, {"level": "denied"}]';
SELECT acl_otel_status();
SELECT acl_otel_flush();      -- export what is queued now and wait for it
```

### The surface

Everything is `acl_otel_`-named, which is what keeps it the operator's: the base's function gate
denies every `acl_`-prefixed function to a principal, and every setting here refuses any scope but
`GLOBAL`.

| function | what it does |
| --- | --- |
| `acl_otel_status()` | one JSON document: attached, the transports, the queue, every counter, the metrics object, health, where the rules came from |
| `acl_otel_healthy()` | the readiness probe's boolean - see `acl_otel_strict` |
| `acl_otel_start()` / `acl_otel_stop()` | attach to the base's registry, or detach and flush; idempotent, and the only way out (duckdb never unloads an extension) |
| `acl_otel_flush()` | export the queued events now and wait for them, bounded |
| `acl_otel_metrics_flush()` | export one metrics tick now |
| `acl_otel_rules_refresh()` | read the central rules table now; answers how many rules are in force |
| `acl_otel_create_rules_table()` | create that table, once, by hand, where `acl_otel_rules_table` points |
| `acl_otel_version()` | the build |

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
| `acl_otel_metrics` / `_metrics_interval` | `true` / `15` | export metrics at all, and the seconds between scrapes |
| `acl_otel_histogram_buckets` / `_histogram_sums` | `''` / `true` | the bounds per histogram as JSON, and the `_sum` / `_count` pair beside each |
| `acl_otel_series` / `_claim_dimension` | `''` / `''` | the opt-in series (`by_role,by_object,by_subject,by_claim`) and the one claim `by_claim` counts by |
| `acl_otel_max_series` / `_series_allowlist` | `1000` / `''` | distinct label values before the `other` fold, and the values kept exact regardless |
| `acl_otel_claim_attributes` | `''` | the claims whose values may be exported, as `acl.claim.<name>`; at most 16 |
| `acl_otel_sample_allowed` | `1` | the ratio of ALLOWED statements exported, or a JSON object per role; a refusal is never sampled |
| `acl_otel_strict` / `_health_window` | `false` / `60` | report `acl_otel.healthy = 0` while events are being lost, and for how long after the last one |
| `acl_otel_rules_table` / `_rules_interval` / `_max_rules` | `''` / `30` / `1000` | the table the level rules are read from, how often, and how many at most |

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
