# Spec 004: logging on a connection - the level of a session, and where it came from

- **Status**: draft (one decision open, below)
- **Date**: 2026-09-08
- **Author**: hugr lab

## Summary

R4 asks for one place where an operator turns logging on for a connection: the rule that decided a
session's level when it opened (spec 001), the override for a session already open, and a listing
that says which of the two - or neither - is in force. The first is done. The override is the
base's own `acl_session_audit_level(<ops id>, <level>)`, which needs nothing from us. The listing is
the piece that does not exist, and it is the piece that needs a decision: **the extension cannot see
the base's sessions**, because the contract (spec 069) carries events, counters and gauges, and a
session list is none of the three.

## Problem

"Turn on full logging for the connection that is misbehaving, and tell me why this other one is
already verbose." The first half works today: `acl_session_audit_level` is the base's, the
`acl_otel_level_rules` are ours, and a level change takes effect on the session's next statement
(R4.2, by the base's construction). The second half - *why* a session sits at the level it does -
needs the session's identity beside the rule that matched it, and only the base knows the first.

## The open decision

**(a) The base exposes its sessions through the contract** - a reader registered in `AuditHooks`
(`SessionsReader`, the shape `AuditGauges::RegisterDynamic` already uses: attribute tuples plus a
value, or a small struct: ops id, subject, roles, door, level, where the level came from). The
extension reads it on demand and renders `acl_otel_sessions()`. This is what the registry is for,
it stays header-only, and it is one `CONTRACT_VERSION` bump - the stamp of spec 069's addendum
exists precisely so such a bump is loud rather than silent.

**(b) The extension runs SQL** - a table function whose bind opens a second `Connection` on the
instance and reads the base's `acl_sessions()`. No base change, but a nested connection while the
caller's query is in flight, on a node whose whole point is not to surprise anybody. Cheap to write,
unpleasant to reason about.

**(c) Nothing** - the operator reads `acl_sessions()` and `acl_otel_status()` side by side and joins
them by eye. Honest, and what works today.

Recommendation: **(a)**, with (c) as the answer until the base's side lands. What (a) costs the base
is a reader and a version bump; what it buys is that every future consumer of the contract - not
only this extension - can see the sessions it is deciding levels for.

## Design (under (a))

- The base registers a reader when the store is built: for each live session, its ops id, the
  subject and issuer, the roles, the door, the level in force and its source (`instance`,
  `policy`, `override`). Never the handle, never a claim value - the same rule as an event.
- `acl_otel_sessions()` (a table function, `acl_otel_`-named so the base's gate denies it to a
  principal) renders that, plus the rule of ours that matched when the source is `policy`: an
  operator sees `analyst -> all (rule 2)` rather than `all`.
- `acl_otel_session_level(id, level)` is **not** added: it would be a name over the base's own
  function, and a wrapper that only renames is a second place to look. The docs point at
  `acl_session_audit_level`.

## Enforcement & security

- Read-only, operator-only, and nothing here changes a decision or a level by itself.
- The listing never carries a session handle (a bearer credential) or a claim value.
- (b) is rejected in part for this: a nested connection is a second place where an error could
  surface something the caller's own rights would not have shown.

## Testing

- Under (a): the base's own test for the reader; here, `acl_otel_sessions()` beside acl with two
  sessions at different levels - one from a rule, one from an override - and the source named for
  each; the function refused to a principal (the base's gate, asserted).
- Under (c): nothing to test; the docs change only.

## Alternatives considered

- A gauge per session (`acl.sessions.level{session=...}`): the cardinality the metrics side spends
  R2.3's whole budget avoiding, and a gauge is a number, not a row.
- Mirroring the base's session map in the extension by watching `session` events: a second copy of
  state the base owns, wrong the moment an event is dropped.
