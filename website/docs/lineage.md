# Lineage

acl-otel can send a node's lineage facts to a lineage backend. The node is duckdb-acl, and the facts
are what its [spec 107](https://hugr-lab.github.io/duckdb-acl/docs/lineage) makes:
- what each virtual table, view and table function is made of;
- which roles see which fields;
- every write;
- every attached source.

Each fact becomes an [OpenLineage](https://openlineage.io) 2-0-2 event and is POSTed to the backend:
Marquez, DataHub, OpenMetadata, or Purview's OpenLineage endpoint.

```sql
SET GLOBAL acl_lineage_level = 'on';                       -- the node makes the facts (duckdb-acl)
SET GLOBAL acl_otel_lineage = 'https://marquez.corp:5000'; -- acl-otel sends them
```

Lineage runs on its own lane, like the other signals. Nothing a statement does waits for it.

## What is sent

| the node's fact | OpenLineage |
| --- | --- |
| a write (INSERT / UPDATE / DELETE / MERGE / CTAS, Flight ingest) | `RunEvent` `COMPLETE` with `columnLineage` on its output; a failed one is `FAIL`, and one whose transaction rolled back is `ABORT` (duckdb-acl spec 112) - both name their output without lineage |
| a SELECT declared as a step of an external job | `RunEvent` with its inputs |
| a definition: a virtual table, view or table function | `DatasetEvent` with `schema`, `columnLineage`, `tags`, `lifecycleStateChange`, `datasetType` |
| a grant that changes what roles see | `DatasetEvent` with the new `tags` |
| `ATTACH` / `DETACH` of a source | `DatasetEvent` of the source: `datasource`, `datasetType` `SOURCE` |

**On a run:**
- **job** - the job the client declared (`LINEAGE JOB`, `x-openlineage-job`), or `sql:<hash>`. Its
  facets are `jobType` and, when the node allows it, `sql` (normalized: every constant a `?`).
- **`parent`** - the external run the statement declared itself a step of (`OPENLINEAGE_PARENT_ID`),
  with its root. Marquez files such a job as `<parent job>.<job>`.
  - OpenLineage run ids are UUIDs. A parent whose run id is not one is left out of the event (a strict
    backend refuses the whole event over it), and the status counts it as `parent_dropped`.
- **`duckdb_acl`** - our own facet (a custom facet carries its producer's prefix), schema
  [`AclRunFacet.json`](https://hugr-lab.github.io/acl-otel/schemas/AclRunFacet.json):
  - `client` - the door: flight, quack, session or gateway;
  - `issuer`, `roles`, `subject`, `node_group` - only what the node's `acl_lineage_identity` allows;
  - `approximate` and `truncated`.

**Role tags** are OpenLineage tags, one per role:
- `acl.role.<role>` = its capabilities, on the dataset;
- `acl.role.<role>` = `visible` or `masked`, per field path;
- `acl.role.<role>.rls` = `true`.

Marquez keeps the tags and the column lineage of a `DatasetEvent` as facets. Its column-lineage
graph is built from runs only.

An input field is listed once, with every transformation it goes through. An edge from a source
taken whole (an opaque function, no field to name) is not sent; the event says `approximate`.

## The first catalog

When acl-otel starts with lineage on, and whenever the backend changes, it calls
`acl_lineage_resend()` on a service connection. A backend therefore starts with every definition the
node has.

- **Waiting for the node.** At load, acl may not be loaded yet, its policy catalog not chosen, its
  lineage not on. The bootstrap asks every 5 seconds for up to 5 minutes, then gives up.
- **Status.** The outcome is under `bootstrap`: `sent N`, `waiting: …`, `gave up: …`, or `error: …`.
  After a give-up, `SELECT acl_lineage_resend()` does the same by hand.
- **Off.** `acl_otel_lineage_bootstrap = false` turns it off.

## The key

A key is never a setting. It comes from one of two places:

- **`OPENLINEAGE_API_KEY`** in the environment, OpenLineage's own convention;
- **a secret in the node's secrets service**, an attached `tresor` catalog. This is the way to rotate
  the key without a restart: it is bound to the node rather than a person, and it is written nowhere
  on the node.

  ```sql
  CREATE PERSISTENT SECRET ol_key IN corp (TYPE openlineage, API_KEY '…');   -- in the service
  SET GLOBAL acl_otel_lineage_secret = 'ol_key';                            -- or 'corp.ol_key'
  ```

  - **Reading.** It is read when an event is sent, on the node's own connection, and kept for
    `acl_otel_lineage_secret_ttl` seconds (300). A key rotated in the service is used once the TTL
    runs out.
  - **Where it may live.** A secret in the node's own memory or `local_file` storage is never read.
    With several services attached, the setting must name one.

The key is sent as `Authorization: Bearer`, and only over `https://`, or plain `http://` to a loopback
host. To any other plain-http URL the event is not sent (`lost_key`).

- **Rotation.** A key refused with 401 or 403 is read again from the secret at once, and the event is
  retried once with it, so a key rotated in the service costs no events.
- **Never in a URL.** A URL with `user:password@` is refused at the SET.
- **Never shown.** The status names the key's source (`env`, `secret`, `none`), never the key. An
  error a backend answers has the key cut out.

## Delivery

- **Success.** Each event is one POST. A 2xx is `sent`.
- **Retries.** A 5xx, a 429 or no answer is retried 3 times, 0.5, 1 and 2 seconds apart, then counted
  as `lost_retries`.
- **Stopping.** Stopping the lane (a SET that switches it off, `acl_otel_stop()`, the database
  closing) flushes what it can within 2 seconds. It then cuts the retries short, counting what was
  left as `lost_stopped`. It never waits out a backend that does not answer.
- **Rejection.** A 4xx is `rejected` at once, with its first line as `last_error`. A backend that
  refuses one event's shape never holds up the rest.
- **The queue.** A full queue drops the event and counts it as `lost_queue`.

```sql
SELECT acl_otel_lineage_flush();                 -- send what is queued now (bounded)
SELECT acl_otel_status()::JSON->'lineage';       -- url, key source, sent / rejected / lost_*, bootstrap
```

## Settings

| setting | default | |
| --- | --- | --- |
| `acl_otel_lineage` | `''` | the backend's base URL; `''` = `OPENLINEAGE_URL`; neither = off |
| `acl_otel_lineage_endpoint` | `api/v1/lineage` | the path (DataHub: `openapi/openlineage/api/v1/lineage`) |
| `acl_otel_lineage_timeout` | `5` | seconds per POST |
| `acl_otel_lineage_physical` | `true` | `false` drops physical datasets and their edges |
| `acl_otel_lineage_secret` | `''` | `[service.]name` of an `openlineage` secret |
| `acl_otel_lineage_secret_ttl` | `300` | seconds a key read from the secret is reused |
| `acl_otel_lineage_bootstrap` | `true` | re-send the definitions on start and on a new backend |
| `acl_otel_lineage_queue_size` | `10000` | events the lane holds, from the next `acl_otel_start` |
| `acl_otel_lineage_batch_size` | `64` | events per wake, from the next `acl_otel_start` |

TLS follows the URL's scheme. `acl_otel_certificate` names a CA file.
