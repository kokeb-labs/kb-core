# ADR-0011: Assurance level `high`; drivers and handlers are not the safety layer

Status: Accepted (owner decision, 2026-11-13). Sets the level used by [ADR-0004](ADR-0004-ci-strategy.md).

## Context

The first skeleton declared kb-core at the handbook's `safety` level. That level asks for a commercial MISRA
checker run, requirements traced to code and tests, every reachable branch covered and a signed evidence package
per release, and the handbook allows it only per component, by a decision record, when a product team intends to
qualify that component. The handbook puts drivers and kb-core at `high` from their first release. No commercial
checker is available today. More importantly, a safety function must not depend on a bus transfer succeeding: a
driver or a handler can fail, block until its timeout or report stale data, so neither can be the layer that
keeps a product safe.

## Decision

1. kb-core declares level **`high`** (`.kokeb/handbook.yml`), and every reusable workflow it calls runs at `high`.
2. **Drivers and handlers are not the safety layer.** Safety functions (reaching the safe state, independent
   monitoring, the decision to kick a watchdog) belong to the product's safety layer, which must reach its safe
   state without any driver or handler call succeeding (it cuts outputs first and never waits on a bus). kb-core
   provides building blocks with documented, bounded behaviour (bounded waits, health states, liveness beats) that
   a safety layer may use as inputs, at the evidence level of `high`.
3. **MISRA C++:2023 evidence at `high`**: the CodeQL MISRA C++:2023 pack and the org's `kokeb-cpp20` profile pack
   on host and Cortex-M builds, plus kb-core's **compliance matrix** (handbook `misra_matrix.py`) that states, for
   every guideline, how it is met: CodeQL, the profile pack, compiler warnings, sanitizers, coverage, fuzzing,
   mutation testing, or review. One recorded library deviation above the boundary files, DEV-KBC-004 (see the
   amendment below); each boundary file's findings are recorded deviations.
4. **`safety` stays a later option**, to be taken per component by a new ADR when a product intends to qualify it
   and a MISRA checker accepted for that purpose has been obtained. Candidates to price (the owner decides): a
   commercial MISRA C++:2023 checker with a perpetual or low-cost licence, run locally before a release and
   reconciled with the CodeQL results in the compliance matrix. Until then nothing in kb-core claims the `safety`
   level or a safety class.
5. Drivers stay at `high`; pin hygiene requires `high` or above at every pinned tag.

## Consequences

- The CI configuration is `level: high` in `ci-misra.yml`, `ci-quality.yml` and `ci-mutation.yml`; the
  coverage, complexity and fuzz limits are the handbook's for `high`. kb-core's own exit criteria per part may be
  stricter (see the [plan](../design/plan.md)), as goals, not as a level claim.
- The assurance records (`docs/assurance/`) are kept as good practice: SOUP list, traceability per part, review
  records, equivalent mutants, problem reports.
- A product that needs a qualified component later starts from complete `high` evidence rather than from scratch.

## Alternatives considered

| Option | Why not |
| --- | --- |
| `safety` from v0.1.0 | Needs a checker the project does not have, and claims more than the code's role justifies |
| `standard` until v1.0 | Below the handbook's floor for code others build products on |
| Make handlers part of a safety function | A bus-backed call cannot guarantee reaching a safe state |

## Amendment (2026-11-14, owner decision)

The claim of zero repository deviations above the boundary files becomes: **one recorded library deviation,
DEV-KBC-004, confined to the `*Ref` adapters in one header (`components/lib/interfaces/inc/kb/interfaces/refs.hpp`),
with a
compile-time check that the context type matches.** ADR-0006 rule 5 makes run-time choice a type-erased `*Ref`
(a context pointer and a constant table of function pointers, no virtual function); erasing the type needs the
context as `void*`, and getting it back is a conversion that MISRA C++:2023 Rule 8.2.6 forbids. The conversion is
one helper (`detail::ContextAs<T>`), called only from tables generated for `T`; each `*Ref` constructor is
constrained by the concept and `static_assert`s it, so a context reaches only the functions of its own type. Every
other library file stays deviation-free.

