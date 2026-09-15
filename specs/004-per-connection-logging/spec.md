# Spec 004: logging on a connection - the level of a session, and where it came from

- **Status**: closed 2026-09-15 - the base answered it, and the answer needed no code here
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

## Closed 2026-09-15 - by a fourth option nobody had listed

The base took neither (a), (b) nor (c): it put the two missing facts on **its own** listing. Since
duckdb-acl's spec 069 addendum, `acl_sessions()` carries `level` (the level in force) and
`level_source` - `instance`, `policy` or `override` - beside the door it was opened through. So the
operator's question, *why is this session verbose*, is answered in one place by the component that
knows: `policy` means a rule of ours decided it.

What that costs and buys, written down because it is the part a reader will want to argue with:

- **No `CONTRACT_VERSION` bump**, no `SessionsReader`, no nested connection. Option (a) would have
  bought every future consumer of the contract a session list; nobody has asked for one, and the
  bump is not free.
- **`acl_otel_sessions()` is not built**, as the design below already argued for its sibling
  `acl_otel_session_level`: a function of ours over facts of the base's is a second place to look.
- **What we lose** is the one detail the design wanted and the base does not carry: *which* of our
  rules matched. `level_source = policy` says a rule did; `acl_otel_level_rules` says which rules
  exist. Joining those two by eye is the residue, and it is small enough to leave.

Pinned where the two halves meet: `test/sql/acl_otel_beside_acl.test` sets a rule, opens a session
through the base, and asserts the base's own listing answers `level: all`, `level_source: policy`.
Without the extension attached the same session reads `instance` - which is what makes the assertion
worth having.

## The open decision (as it stood, for the record)

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
