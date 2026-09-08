# Spec 005: enrichment and sampling - the one claim an operator asked for, and the volume they can afford

- **Status**: implemented
- **Date**: 2026-09-09
- **Author**: hugr lab

## Summary

Two small things the contract asks for, both about what leaves the node. **Enrichment** (R5): a
claim value is exported only when the operator names its claim in `acl_otel_claim_attributes`, as
`acl.claim.<name>`; every other claim the event carries in memory is dropped before export, and
nothing is added that is not in the event or in our own configuration - no catalog read, no table.
**Sampling** (R6.1): an `allowed` statement may be sampled away by ratio, per role or overall;
a refusal, a session, a door, a policy or a keys event never is. Every event sampled away is
counted (R6.2), like every other drop, and the counters and histograms of spec 003 still see it -
sampling thins what a backend stores, never what the node knows.

## Problem

An audit backend costs money per record, and a busy node's `allowed` statements are most of the
volume and least of the information. Turning the level down loses the objects and the timings with
it; sampling keeps the shape of the allowed traffic at a fraction of the records, and keeps every
refusal.

The other half: a decision's claims are in memory (`principal.claims`) and the base never exports
them (C5). A fleet that routes by tenant wants exactly one of them on the record, and wants to say
which - not to have the extension decide.

## Design

### Enrichment (R5)

- `acl_otel_claim_attributes` (VARCHAR, default `''`): comma-separated claim names. For each name
  the event carries, one attribute `acl.claim.<name>` with its value; a name the event does not
  carry adds nothing. Everything else in `principal.claims` never reaches a record.
- The list is part of the transport's configuration (`OtlpConfig`), so it is read where the
  endpoint is, refused nowhere (a claim name is not a document), and changing it takes effect on
  the next export - the mapping reads the list, not a snapshot of it.
- **Bounded, like every other attribute**: at most 16 names (`acl.claim.*` is a dimension in a
  backend, and R2.5's ten-dimension limit is a warning about what a record can carry), each value
  truncated at 256 bytes with an ellipsis. A claim is an identifier in practice; a claim that is a
  document is a mistake we make visible rather than forward.
- R5.2 holds by construction: the mapping reads the event and this list, nothing else.

### Sampling (R6.1)

- `acl_otel_sample_allowed` (VARCHAR, default `'1'`): either a ratio - `'1'` keeps everything,
  `'0.1'` keeps a tenth - or a JSON object per role, `{"analyst": 0.05, "*": 0.5}`, where `*` is
  the ratio for a role the object does not name. A principal with several roles gets the **highest**
  ratio any of its roles names: sampling is a cost control, and the role an operator kept at 1
  should not be thinned by another the principal also holds.
- Only an `allowed` **statement** is eligible. A refusal, an **admin decision** (a change to the
  policy is the most audit-relevant allowed event there is), a session open/close/refuse, an ingest,
  a door event, a policy event and a keys event are never sampled - they are the record the audit
  exists for.
- The decision is **deterministic in the event's `seq`** (a 64-bit mix, kept below the ratio's
  millionth), so it costs nothing, needs no state, and two nodes with the same setting sample the
  same way. No RNG, no lock.
- Where: in `OtelSink::OnEvent`, **after** the metrics accumulators have seen the event (spec 003)
  and before the queue. The counters and the histograms therefore stay exact and only the log
  stream thins - which is the point: a rate is a metric's job, a record is a log's.

### What the status says

`sampled` beside the other drops in `acl_otel_status()`: how many `allowed` events were sampled
away, and the ratio in force. The `dropped` object keeps its shape (`queue`, `no_exporter`), and
`sampled` sits beside it rather than inside: it is a policy, not a failure.

Spec 006 exports these numbers as `acl_otel.*` metrics (R6.2's "in the extension's own metrics");
until then the status is where they are read.

## Enforcement & security

- A claim value leaves the node only through `acl_otel_claim_attributes` (logs) or
  `acl_otel_claim_dimension` (metrics, spec 003) - two settings, both the operator's, both GLOBAL,
  neither reachable by a principal (C8).
- Sampling can never drop a refusal, and the base's counters count every decision whatever this
  setting says: an operator who samples cannot accidentally stop counting.
- The truncation of a long claim value is on our side of the wire, so a backend never sees a record
  it would have refused whole.

## Testing

- **C++** (`test_acl_otel_sampling.cpp`, no SDK): the ratio parsed in both shapes and refused when
  it is neither; the per-role maximum; determinism (the same seq decided the same way twice) and
  distribution (a ratio of 0.1 keeps between 5% and 15% of 10000 events); never a refusal, a
  session, an admin decision, a door, a policy or a keys event; `'1'` keeps everything and `'0'`
  keeps no statement but still lets every ineligible event through.
- **C++ against the fake receiver** (extending spec 002's test): a claim on the list becomes
  `acl.claim.<name>`, one not on the list is absent, a value past 256 bytes is truncated with an
  ellipsis, and more than 16 names is refused at the SET.
- **sqllogictest**: the settings refuse a session scope; a malformed ratio is refused at the SET;
  beside acl, a burst of allowed statements at ratio `'0'` leaves `received` unchanged in the sink
  while `acl.decisions` in `acl_metrics()` still counts them all, and `sampled` says how many.

## Alternatives considered

- **Sampling on the base's side** (a level between `decisions` and `all`): it would lose the events
  for the counters too, and the base deliberately keeps levels as a small ordered set.
- **A random sampler**: a lock or thread-local state on the audit thread, and two nodes that
  disagree about the same statement. The seq mix costs an integer multiply.
- **Sampling denied events too, at a separate ratio**: a refusal nobody recorded is the failure
  this whole spec 069 family exists to prevent.
- **Exporting every claim and filtering in the Collector**: the values would have left the node.
