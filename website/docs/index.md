---
slug: /
---

# acl-otel

OpenTelemetry for [duckdb-acl](https://hugr-lab.github.io/duckdb-acl/)'s audit. acl-otel is a DuckDB
extension loaded beside `acl` on the same node. It turns what acl decides and measures into
OpenTelemetry:

- **logs**: one record for every access decision, session, door, ingest and policy event;
- **metrics**: acl's own counters and gauges, histograms built from the events, and opt-in series by
  role, object, subject or claim;
- **traces**: a decision as a span under the request that caused it, and a statement's execution
  profile as a span beside it.

It also decides the audit level **per role, per user and per door**, samples allowed statements, and
reports its own health. It enforces nothing and changes no decision. It can never slow or stop a
decision either, because acl emits every event after the decision is made.

Since spec 011 it carries [tresor](https://hugr-lab.github.io/tresor/)'s audit the same way: tresor is
the secrets service's client on the node, and its events are logins, secret lookups and grants.

> **Status: pre-release**, released with duckdb-acl after DuckDB 2.0.

## How it attaches

acl-otel compiles against one header of acl, `acl_audit.hpp`, from the shared repository
duckdb-ext-common. It reaches acl through DuckDB's object cache, never through a symbol, so the two
load in either order. A registry built from another revision of the contract is refused, and the
status says why (`attach_error`).

## Where to start

- [Getting started](getting-started.md): load it beside acl, point it at a collector, see a record.
- [Levels and rules](levels-and-rules.md): which sessions are audited at which level.
- Signals: [logs](logs.md), [metrics](metrics.md), [traces](traces.md), and [tresor's audit](tresor.md).
- [Reference](reference.md): every function and setting.
- [Deployment](deployment.md): collectors, Application Insights, a local Grafana stack.
