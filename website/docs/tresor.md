# tresor's audit

[tresor](https://hugr-lab.github.io/tresor/) attaches a company's secrets service to DuckDB, so a
node running acl resolves secrets as the session's user. tresor publishes its own audit through the
shared repository duckdb-ext-common: the contract `tresor_audit.hpp`, TRSA 1. The events cover a
login, a secret lookup or refresh, a write, drop or annotate, a grant or revoke, and a session's
delegation grant. acl-otel carries them out the same way it carries acl's (spec 011).

```sql
SET GLOBAL acl_otel_tresor = true;   -- the default: attach wherever tresor loads
```

The extension attaches at start, beside acl's registry. The load order does not matter: whichever
side comes first creates the registry. A registry built from another TRSA version, or on another
version of the hooks base, is refused. Nothing is attached, and `acl_otel_status()` says why under
`tresor.error`.

## The record

- **Severity and body.** WARN for `denied` and `error`, INFO otherwise. The body is
  `tresor <kind> <outcome>`, followed by `: <reason>` in tresor's own words.
- **Scope.** The records go out under the instrumentation scope `tresor`, with the node as the
  resource, as acl's do.
- **Attributes.** Every field is a `tresor.*` attribute, present only when set:
  - the event: `kind`, `outcome`, `reason_code`, `reason`, `seq`, `detail`;
  - the service: `service`, `host`, `login`, `principal`;
  - the user: `user`, `acl_session`, `correlation_id`;
  - the secret: `secret`, `secret_type`, `cached` (on a lookup or refresh), `dynamic`;
  - `target` on a grant or revoke;
  - `duration_us`, only when a service call was made.
- **Trace context.** The caller's, when the event's `traceparent` parses.

The event never carries secret material, tokens, handles, grant ids or statement text.

## The span

With `acl_otel_traces` on, an event becomes a span `tresor.<kind>` only when both hold:

- the caller's `traceparent` is valid and sampled;
- tresor measured a service call.

The span:

- is a child of the caller's span, in the caller's trace;
- covers `[ts_us - duration_us, ts_us]`;
- has status `kError` on `denied` / `error`.

A lookup served from tresor's cache made no call, so it is a record, never a zero-length span.
Unsampled traces are counted. The span's id is derived with a salt of its own, so it never collides
with a decision span of the same node and sequence.

## The numbers

The scrape adds tresor's own counters under tresor's names:

- `tresor.events` {kind, outcome, cached}, counted only while a sink listens;
- `tresor.audit.dropped` and `tresor.audit.sink_failed`.

It also adds the extension's numbers for tresor's events:

- `acl_otel.tresor.received`, `.exported`, `.dropped{why}`;
- `acl_otel.tresor.spans.*`;
- `acl_otel.tresor.attached`.

Strict health counts tresor's lost records and spans as losses.
