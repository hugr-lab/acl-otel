# Levels and rules

acl records each event at a **level**: `off`, `denied`, `decisions` or `all`. Its instance-wide
level is `acl_audit_level`. acl-otel is acl's **session policy**: when a session opens, acl asks it
for the session's level, and the first rule that matches decides.

## Rules

A rule names any of `role`, `subject`, `issuer` and `door`, and a `level`. A field that is empty or
`*` matches anything. The first match wins, and when no rule matches the instance's level applies.

```sql
SET GLOBAL acl_otel_level_rules = '[
  {"role": "analyst", "door": "flight", "level": "all"},
  {"issuer": "https://login.example.com/partners", "level": "decisions"},
  {"level": "denied"}
]';
```

`acl_sessions()` in acl shows each session's level and where it came from; `policy` means a rule
here decided it.

A rule may also carry `profile` (`off`, `sampled`, `all`) beside or instead of `level`. That is the
session's execution-profile level (acl spec 074): which statements are profiled and exported as the
execution span. A rule about profiling never shadows one about the audit, because each question
walks only the rules that carry its field.

## The rules a fleet writes once

On a fleet, write the rules once in a table that every node reads:

```sql
SET GLOBAL acl_otel_rules_table = 'fleet.main.acl_otel_rules';
SELECT acl_otel_create_rules_table();   -- by hand, once: (seq, role, subject, issuer, door, level)
INSERT INTO fleet.main.acl_otel_rules VALUES (1, 'analyst', NULL, NULL, 'flight', 'all');
```

- Every node reads the table every `acl_otel_rules_interval` seconds, on a connection of its own.
  Rows apply in `seq` order.
- A read that fails or returns a malformed row leaves the rules in force, and the status says so.
- A node an operator is debugging can still set `acl_otel_level_rules`, which wins while it is set.
- For profiling, add a `profile VARCHAR` column.
- The extension never creates the table by itself. A background thread may read another database,
  but writing to it takes a statement with a person behind it.

## Sampling

Allowed statements can be thinned, overall or per role:

```sql
SET GLOBAL acl_otel_sample_allowed = '0.1';                        -- keep one in ten
SET GLOBAL acl_otel_sample_allowed = '{"analyst": 0.05, "*": 0.5}'; -- per role, `*` for the rest
```

- **What is never sampled.** A refusal, and every session, door, ingest, policy or keys event.
- **Determinism.** Sampling depends on the event's sequence number, so two nodes with the same
  setting keep the same statements.
- **Counters stay exact.** Counters and histograms see every event, so a rate stays exact while the
  records thin.
- **Several roles.** A principal with several roles gets the highest ratio any of them names.
