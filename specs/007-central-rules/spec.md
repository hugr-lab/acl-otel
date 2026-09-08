# Spec 007: the level rules a fleet writes once

- **Status**: implemented
- **Date**: 2026-09-09
- **Author**: hugr lab

## Summary

§5 of the contract: the level rules may live in a table instead of a setting, so a fleet writes them
once with plain SQL and every node picks them up. This spec adds that reader - a table the operator
names (`acl_otel_rules_table`), read on `acl_otel_rules_interval` (30 s) by a thread of ours on its
own connection, parsed into the same `LevelRule`s spec 001 already applies at session open. The JSON
setting stays for a single node and **wins when both are set**. The node is a read-only consumer:
the only statement that ever writes is `acl_otel_create_rules_table()`, which an operator runs by
hand, once.

## Problem

`acl_otel_level_rules` is a JSON document per node. A fleet of thirty nodes then has thirty copies
of the same decision, drifting apart at the speed of whoever deploys. The base already has a
database every node reads - its policy catalog - and the rules are exactly the kind of thing that
belongs beside it: written once, read by all, changed without a restart.

## Design

### Where the rules live

- `acl_otel_rules_table` (VARCHAR, default `''`): the **fully qualified** table, e.g.
  `store.acl_otel.level_rules`. Empty - the default - means no reader, no connection, no thread.
  Naming it is what turns the feature on.
- Its shape, which `acl_otel_create_rules_table()` creates:

  | column | meaning |
  | --- | --- |
  | `seq` INTEGER | the order rules are judged in; first match wins, as in the JSON document |
  | `role`, `subject`, `issuer`, `door` VARCHAR | the match, `NULL` or `''` or `*` meaning "any" |
  | `level` VARCHAR | `off`, `denied`, `decisions` or `all` |

- Reading is `SELECT seq, role, subject, issuer, door, level FROM <table> ORDER BY seq` on a
  connection of ours, at most `acl_otel_max_rules` (1000) rows. A row with an unknown level, or a
  read that fails, leaves the rules **as they were** and is reported in the status: a fleet's
  configuration mistake must not silently switch a node's auditing off.

### Why the extension does not create the table by itself

§5 says "created by the extension at first use". We do not: a background thread that runs DDL on the
operator's database, unasked, is the kind of surprise this whole family of specs exists to avoid.
`acl_otel_create_rules_table()` is one statement an operator runs once, it is
`acl_otel_`-named (denied to a principal, C8), and it creates only our own schema and table -
never touching the base's `acl` schema or its migrations (R9.3).

### Precedence and reload

- `acl_otel_level_rules` non-empty wins: a node an operator is debugging keeps the last word.
- Otherwise the table's rules apply, replaced whole on every successful read. A read that finds no
  rows means no rules - which is the fleet saying "the instance level governs", not an error.
- `acl_otel_rules_refresh()` reads now and answers what it found, for an operator who does not want
  to wait out the interval.

### What the reader costs in the artifact

Measured, because it is not free: opening a `Connection` and running a query pulls duckdb's own
query path into the loadable, and the artifact grows from **28 MB to 52 MB**. The base's own `acl`
is of that order for exactly the same reason - its catalog validation and its drain thread both
query on their own connections - so this is the price of the pattern rather than a surprise of this
spec. It is still the whole cost of a feature that stays off until a table is named, and worth
knowing before turning it on becomes the default anywhere.

### The reader's thread

Its own, not the scrape's: rules must refresh whether or not metrics are on. It sleeps the interval,
opens a `Connection` on the instance for each read (the base's own pattern for off-path work), and
is stopped before anything else in `OtelState::Stop()` - a thread that queries a database must never
outlive the database. `LevelFor` itself is untouched and still answers from memory in constant time
(R3.2).

### What the status says

`rules_source` (`setting`, `table` or `none`), `rules` (the count in force), `rules_table`,
`rules_read_us` and `rules_error` (the last failed read, `null` when the last one worked).

## Enforcement & security

- The node reads one table it was pointed at and writes nothing but that table's own creation, by
  hand (R9.3).
- A failed or malformed read never lowers a level: the rules in force stay in force, and the failure
  is visible in the status and in the extension's own numbers.
- The table holds no secret: roles, subjects, issuers, doors and levels - the same words the JSON
  document holds today.
- A principal can neither read nor write it through us: the function and the settings are
  `acl_otel_*`, which the base's gate denies (C8).

## Testing

- **sqllogictest** (beside acl, where a catalog database exists): `acl_otel_create_rules_table()`
  creates the schema and table; rows in it become the rules on the next `acl_otel_rules_refresh()`;
  a row with an unknown level leaves the previous rules in force and shows in `rules_error`; the
  JSON setting wins while it is set and the table takes over when it is cleared; a table name that
  does not exist is an error in the status and not a crash; the settings refuse a session scope.
- **C++**: the row-to-rule mapping (NULL, `''` and `*` all meaning "any"; `seq` deciding the order),
  and the cap.

## Alternatives considered

- **The extension creating the table when it first reads**: DDL from a background thread on somebody
  else's database. No.
- **Finding the catalog database by asking the base**: the contract carries events, counters and
  gauges - not the store's configuration - and a fourth thing to add to `AuditHooks` for this alone
  is not worth a `CONTRACT_VERSION` bump. Naming the table is one setting and reads better.
- **A single rules row of JSON**: a table a human edits with `UPDATE` beats a document a human edits
  with string surgery.
