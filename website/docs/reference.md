# Reference

Everything this extension registers is named `acl_otel_*`. acl's function gate denies every
`acl_`-prefixed function to a principal, so the whole surface belongs to the operator. Every setting
refuses any scope but `GLOBAL`: a session-scoped value would show in `current_setting()` and change
nothing.

## Functions

| function | what it does |
| --- | --- |
| `acl_otel_status()` | one JSON document: attached, the transports, the queue, every counter, the metrics object, health, where the rules came from, the span lane |
| `acl_otel_healthy()` | the readiness probe's boolean - see `acl_otel_strict` |
| `acl_otel_start()` / `acl_otel_stop()` | attach to the base's registry, or detach and flush; idempotent, and the only way out (duckdb never unloads an extension) |
| `acl_otel_flush()` | export the queued events now and wait for them, bounded |
| `acl_otel_metrics_flush()` | export one metrics tick now |
| `acl_otel_traces_flush()` | export the queued spans now and wait for them, bounded; false while traces are off |
| `acl_otel_rules_refresh()` | read the central rules table now; answers how many rules are in force |
| `acl_otel_create_rules_table()` | create that table, once, by hand, where `acl_otel_rules_table` points |
| `acl_otel_version()` | the build |

## Settings

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
| `acl_otel_traces` | `off` | `linked` = a span for every decision carrying a usable `traceparent`, under the caller's span; `all` = every decision, rooting its own trace without one; `off` allocates nothing |
| `acl_otel_session_spans` | `false` | also a span per session, from open to close, by how it ended |
| `acl_otel_tresor` | `true` | spec 011: carry tresor's audit (the secrets service's client, TRSA 1) the same way - a record per event (scope `tresor`, `tresor.*` attributes, WARN for denied/error), a `tresor.<kind>` span under the caller's sampled trace when the event measured a call (behind `acl_otel_traces`), and tresor's counters (`tresor.events`, `tresor.audit.*`) in the scrape |
| `acl_otel_profile_plan` / `_profile_spans` | `true` / `false` | spec 009, once the base profiles (`acl_profile_level`): whether a profile's plan travels (the record's body, the `acl.operator` span events), and the plan as child spans of the execution span, labelled cumulative thread time |
| `acl_otel_rules_table` / `_rules_interval` / `_max_rules` | `''` / `30` / `1000` | the table the level rules are read from, how often, and how many at most; a rule (JSON or a row) may carry `profile` = `off` / `sampled` / `all` beside or instead of `level` - the base's per-session profile level (spec 074), a `profile VARCHAR` column the operator adds to the table |

A setting at its default means that the standard OpenTelemetry environment decides
(`OTEL_EXPORTER_OTLP_ENDPOINT`, `_PROTOCOL`, `_TIMEOUT`, `_INSECURE`, `_CERTIFICATE`, and the
per-signal variants). A setting that is set wins over its variable.

**Headers come from the environment only.** A bearer token for a collector or a vendor goes in
`OTEL_EXPORTER_OTLP_HEADERS=authorization=Bearer …,x-tenant=…`. No setting ever carries a secret, and
the status lists header names, never values.
