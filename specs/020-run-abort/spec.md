# Spec 020: a rolled-back write is ABORT

- **Status**: implemented
- **Date**: 2026-10-09
- **Author**: duckdb-acl spec 112's follow-up on this side

## Summary

duckdb-acl spec 112 holds the runs of a client's explicit transaction until it ends and sends the
ones a `ROLLBACK` undid as `event_type = "RUN_ABORT"`. This extension renders that as OpenLineage's
`ABORT` RunEvent.

## Problem

The renderer knew `RUN_COMPLETE` and `RUN_FAIL` only. A `RUN_ABORT` fact would have been dropped as an
unknown kind, and a rolled-back write would leave no trace in the backend.

## Design

- `RUN_ABORT` renders as a RunEvent with `eventType: ABORT`, the same run, job and facets as any run.
- Like `FAIL`, its outputs are named without `columnLineage` or a lifecycle change: nothing was written.
- No contract change: `event_type` was already a string (`acl_audit` v3, ext-common spec 014).

## Enforcement & security

Nothing new is read or sent; the event carries what a `COMPLETE` of the same write would.

## Testing

`test/cpp/test_acl_otel_lineage.cpp`: a `RUN_ABORT` fact renders `ABORT`, its output named without
lineage.

## Alternatives considered

- Rendering it as `FAIL`: a rollback is not a failure of the write; OpenLineage has `ABORT` for it.
