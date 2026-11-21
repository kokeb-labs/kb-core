# ADR-0005: No code is ported from older stacks

Status: Accepted (owner decision, 2026-11-14).

## Context

Older embedded stacks exist that hold useful ideas (one build entry point with an MCU gate table, adapter bases
for driver hooks, lazy initialisation, compiled documentation snippets) and known defect classes (platform leaks,
round-down delays, heap use, several handler shapes, untested ports). Their code was not written under the current
pipeline, standard or licence.

## Decision

kb-core code is written fresh under the driver pipeline: design, build with tests and fuzz targets, self-review
against the defect catalogue, the local gate, independent review, fixes. Older code is at most a source of ideas
and requirements; its defect classes become test cases. No file, function or block is copied from it, and nothing
that identifies it, its products or its people appears in kb-core.

## Consequences

- Every line of kb-core has the same provenance, licence (MIT, "The kb-core authors") and evidence trail, which
  configuration management and independent review need.
- Delivery is slower than a port; the migration order (utils, rtos, interfaces, bindings, handlers) delivers
  usable parts early.
- Hardware evidence from older code is used as requirements (timings, failure modes), not as proof for new code;
  new code needs its own bench runs.

## Alternatives considered

A scripted rename port (fast, but carries every known defect and an unclear provenance); a partial port of "good"
files (no clean line between good and bad, and review would have to treat every file as new anyway).
