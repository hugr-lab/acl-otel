# Spec 018: OpenLineage transport - a node's lineage facts to a lineage backend

- **Status**: implemented
- **Decisions (owner, 2026-10-08)**: the key from tresor as well as the environment; `NAMESPACE` sent as
  a `DatasetEvent` with a `datasource` facet; the bootstrap re-send on by default
- **Date**: 2026-10-08
- **Contract**: duckdb-ext-common v0.11.0, `contracts/acl_audit.hpp` `CONTRACT_VERSION` 3 (its spec 014)
- **Producer**: duckdb-acl spec 107 (PR #188) - the `lineage` event kind

## Summary

duckdb-acl now produces lineage facts:
- the definition of each virtual object;
- per-role visibility;
- each write;
- each source attached.

It hands them to sinks that ask for them, and it does not send them anywhere itself. This spec makes
acl-otel such a sink. It renders each fact as an OpenLineage event and POSTs it to a lineage backend
(Marquez, DataHub, OpenMetadata, Purview's OpenLineage endpoint), on its own lane, off every query's
path.

## Problem

- Without a transport the facts live only in `acl_lineage_events()`, a bounded ring on the node, and
  no catalog sees them.
- Each event must arrive with an outcome: delivered, or counted as dropped.
- The re-pin to v0.11.0 is part of this spec. acl-otel is pinned to ext-common v0.10.0 (contract
  v2), and a v3 base refuses a v2 registry, so until the re-pin acl-otel does not attach to a node
  running 107.

## Design

**Re-pin.** duckdb-ext-common goes to v0.11.0. duckdb-acl goes to the merge commit of #188, on the
same duckdb pin. The contract check stays as it is: one version on both sides, or no attachment.

**The lane.**
- A fourth lane, `EventQueueOf<LineageEvent>`, with the same shape as the others: bounded, batched,
  drops counted, flushes bounded.
- The sink's `WantsLineage()` is true only while lineage is configured. A node that does not export
  lineage keeps the base from making the events at all.
- `OnEvent` copies the `shared_ptr<const AuditLineage>` (no deep copy) and returns.
- Settings: `acl_otel_lineage_queue_size` and `acl_otel_lineage_batch_size`, both GLOBAL.

**Configuration** (GLOBAL, applied at the next `acl_otel_start`, like the other transports):

| setting | default | |
| --- | --- | --- |
| `acl_otel_lineage` | `''` | the backend's base URL; `''` = `OPENLINEAGE_URL` from the environment; neither = lineage off |
| `acl_otel_lineage_endpoint` | `api/v1/lineage` | the path under it (Marquez's; DataHub's is `openapi/openlineage/api/v1/lineage`) |
| `acl_otel_lineage_timeout` | 5 s | per POST |
| `acl_otel_lineage_physical` | `true` | `false` drops physical datasets and their edges, on top of the producer's own switch |

**Credentials.**
- They never come from a setting, the same rule as R9.2 for OTLP.
- The API key comes from `OPENLINEAGE_API_KEY` in the environment, sent as `Authorization: Bearer`.
  That is OpenLineage's own convention, so an orchestrator configures it like any other client.
- Or the key comes from a **tresor secret** (it wins over the environment when set) (`acl_otel_lineage_secret = '<name>'`). It is read
  at use through the attached tresor, on the node's own connection, and cached for at most
  `acl_otel_lineage_secret_ttl` seconds (300). The key then rotates in the secrets service without a
  restart, is bound to the node rather than to any user, and is written nowhere on the node.
- The status names the source of the key (env / secret), never its value.

**Rendering.** The `AuditLineage` maps one to one onto OpenLineage 2-0-2:
- **`RUN_COMPLETE` / `RUN_FAIL`** become a `RunEvent` with eventType `COMPLETE` / `FAIL`. One event
  per run, because the run is already over when the node knows it, so there is no `START`.
  - `run.runId` = `run_id`;
  - the run facets are `parent` (`parent` + `root_parent`) and the custom `acl`;
  - `job` = `{job_ns, job_name}`, with the `jobType` facet `{BATCH, DUCKDB_ACL, QUERY}` and the `sql`
    facet when the producer sent one;
  - inputs and outputs carry the facets `schema`, `columnLineage` (on the outputs), `tags` and
    `lifecycleStateChange`.
- **`DATASET`** becomes a `DatasetEvent` of the defined dataset, with the facets:
  - `schema`;
  - `columnLineage` (its edges to the physical sources);
  - `tags` (the per-role visibility);
  - `lifecycleStateChange`;
  - `datasetType` (TABLE / VIEW / TABLE_FUNCTION).
- **`NAMESPACE`** becomes a `DatasetEvent` for the source's namespace, with a `datasource` facet
  `{name: <alias>, uri: <namespace>}` and the source's type as `datasetType`. The catalog then shows
  the source, and the operator relates it to its real address there.
- **`columnLineage`**:
  - the edges are grouped by target field into `fields.<f>.inputFields[]`, each with
    `{namespace, name, field, transformations[{type, subtype, masking}]}`;
  - the edges to the whole target (an empty `target_field`) go into the facet's `dataset[]`.
- **The custom `acl` run facet**: `client`, `issuer`, `roles`, `subject`, `node_group`,
  `approximate`, `truncated`, each only when set. (As built, the facet's key is `duckdb_acl`.) It carries `_producer`
  (`https://github.com/hugr-lab/acl-otel`) and a `_schemaURL`, served from the docs site.
- `eventTime` is the event's `ts_us`. `producer` is the acl-otel version.

**Delivery.**
- Each batch is POSTed event by event, as a JSON body. The HTTP client is the OTel SDK's (curl), so
  the extension gains no new dependency.
- A 2xx is delivered.
- A 5xx, 429 or connection error is retried with backoff: 3 tries, 0.5 / 1 / 2 s. After that the
  event is dropped and counted.
- A 4xx is dropped at once, counted as `rejected`, with the response's first line in the status.
  A backend that refuses one event's shape never blocks the lane.
- **The first catalog.** On `acl_otel_start` with lineage on, and on every switch of the endpoint,
  acl-otel runs `SELECT acl_lineage_resend()` once on a service connection, so a backend starts
  with every object's definition. This can be turned off with `acl_otel_lineage_bootstrap = false`.
- Counters (spec 006's self-metrics): `acl_otel.lineage.exported`, `.rejected`, `.retried`,
  `.dropped{reason: queue|retries}`, and `.last_error` in `acl_otel_status()`.

## Enforcement & security

- What leaves the node is only what the producer put in the payload. That never includes a value, a
  claim, a token or a handle (spec 014). The SQL facet is there only when the operator turned it on
  at the node.
- `physical` datasets can be dropped on either side.
- A contract mismatch means no attachment, so no lineage, and the status says so.
- The key is in neither settings nor logs; the status names its source.
- TLS follows the URL's scheme and `acl_otel_certificate`.

## Testing

- **C++** (`test_acl_otel_lineage.cpp`):
  - the mapping of every payload shape to OpenLineage JSON, checked against the 2-0-2 JSON schema
    vendored for the test: RunEvent, DatasetEvent, the namespace's DatasetEvent, a parent and root,
    field paths, masking, whole-target edges, an empty event;
  - the retry and drop policy against a fake HTTP server (2xx / 4xx / 5xx / a hang past the timeout).
- **sqllogictest** (`acl_otel_lineage.test`, beside acl): a definition, a grant and a write reach a
  file-backed test exporter (the stand-in the other lanes use), and the bootstrap re-sends.
- **e2e** (`test/e2e/marquez.sh`):
  - Marquez in docker;
  - a node with acl + acl-otel defines a virtual view, grants it, and writes through a role;
  - Marquez's API then shows the namespaces, the datasets with their column lineage, the role tags,
    and the run under its parent.

## Alternatives considered

- **Kafka or a file transport.** A file adds nothing over the node's own ring. Kafka needs librdkafka,
  a new dependency, for a deployment nobody has asked for. Either can be added beside HTTP later.
- **Sending through OTLP (logs).** No lineage backend reads OTLP. OpenLineage's HTTP API is the one
  they all speak.
- **The node POSTing itself.** duckdb-acl spec 107 keeps transport out of the base: the BUSL
  extension owns delivery, the base only produces facts.

## As built (2026-10-08)

- **The lane**: `LineageQueue` (`EventQueueOf<LineageEvent>`, the event a shared pointer to the
  payload and its time) on the sink (`SetLineage`; `WantsLineage()` true only while it is set). A
  `lineage` event goes there alone: never a record, a metric or a span.
- **Rendering** (`acl_otel_lineage.cpp`, SDK-free): RunEvent / DatasetEvent as above. With only a
  root run, the root is the parent. An edge without a source field goes to the facet's `dataset`, as an
  edge to the whole target does. A kind this build does not know is not sent.
- **Delivery** (`acl_otel_lineage_http.cpp`): the SDK's async curl client with a per-request timeout
  and a bound on the callback. The curl session's base ends in `/`, so the request's path goes
  without a leading one; `//api/...` is a 404 at Marquez. `Export` answers true, and each event's
  outcome is in `LineageDeliveryStats`.
- **The key** (`acl_otel_lineage_key.cpp`, SDK-free): `OPENLINEAGE_API_KEY`, or the secret named by
  `acl_otel_lineage_secret`. The secret is of our type `openlineage` (`api_key` redacted), read through
  `SecretManager::GetSecretByName` from the tresor catalog only, and cached for the TTL. A configured
  key that cannot be read means the event is not sent (`lost_key`).
- **Bootstrap**: a one-shot thread runs `SELECT acl_lineage_resend()` on a service connection. Its
  outcome is kept in a holder shared with the thread, because the instance's teardown can run on that
  thread. It is joined at stop.
- **Marquez**: a job with a parent is filed as `<parent job>.<job>`. A DatasetEvent's `columnLineage`
  is kept as a facet; the column-lineage graph API is built from runs only. Its `fields` list keeps
  one copy per DatasetEvent version.
- **Not in strict health**: the lineage lane's losses are in its status, not in `acl_otel_healthy`.
  That stays a follow-up.

**After the three review passes (2026-10-08):**

- **Security.**
  - A backend's error has the key cut out before it reaches the status.
  - A key goes over https, or plain http to loopback only.
  - A URL with userinfo is refused at the SET; at load, the status says why instead.
  - A key with a control character is not sent.
  - A 401 or 403 re-reads the secret once and retries.
  - `memory`, `local_file` and `__transaction` are never a service, and the entry read must come from
    the service's own storage (`storage_mode`).
  - Invalid UTF-8 becomes U+FFFD.
- **Concurrency.**
  - `ApplyLineage` decides under `lock` and never waits. `FinishLineage` does the flush, cancel, join
    and transport release after the lock.
  - The exporter's `Cancel()` cuts retries and the rest of a batch short (`lost_stopped`). Its backoff
    waits on the cancel flag.
  - The delivery numbers are shared across rebuilt exporters.
  - The key read never drops the instance's last reference on the lane's worker: it hands that to a
    thread of its own.
  - The bootstrap has its own lock and a stop flag, and it waits for the node (every 5 s, up to
    5 minutes).
- **OpenLineage.**
  - A non-UUID parent or root is left out (`parent_dropped`).
  - The first try and 3 retries, as documented.
  - A run whose every dataset was dropped is not sent.
  - An edge without a source field is not sent.
  - An input field is listed once, with all its transformations.
  - A `FAIL` names its outputs without lineage or lifecycle.
  - The datasource `uri` is sent only when the namespace is a URI.
  - The custom facet is `duckdb_acl`.
- **duckdb-acl follow-up:** refuse a non-UUID run id in `LINEAGE PARENT` / `ROOT`, the session
  settings and the headers, so the client learns at once rather than seeing its parent dropped
  downstream.

**Tests**:
- `test/cpp/test_acl_otel_lineage.cpp`: every payload shape and the delivery policy against a fake
  poster;
- `test/cpp/test_acl_otel_lineage_key.cpp`: the key from a fake tresor service, rotation, two
  services, the node's memory secret never read, redaction;
- `test/sql/acl_otel_lineage.test`: the settings, the status, the secret type;
- `test/e2e/marquez/run.sh`: Marquez in docker, also in CI beside the base's artifact.

**Docs**: `website/docs/lineage.md`, and the `acl` facet's schema at
`website/static/schemas/AclRunFacet.json`.

## Follow-ups

- DataHub / OpenMetadata e2e besides Marquez.
- Kafka transport, if asked for.
