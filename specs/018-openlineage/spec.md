# Spec 018: OpenLineage transport - a node's lineage facts to a lineage backend

- **Status**: draft
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
- Optionally the key comes from a **tresor secret** (`acl_otel_lineage_secret = '<name>'`). It is read
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
  `approximate`, `truncated`, each only when set. It carries `_producer`
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

## Open questions for the owner

1. The key: environment only, or also from tresor (`acl_otel_lineage_secret`), as proposed?
2. `NAMESPACE` as a `DatasetEvent` with a `datasource` facet, or not sent at all?
3. The bootstrap re-send on start: on by default?

## Follow-ups

- DataHub / OpenMetadata e2e besides Marquez.
- Kafka transport, if asked for.
