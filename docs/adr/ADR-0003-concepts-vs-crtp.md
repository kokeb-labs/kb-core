# ADR-0003: Concepts for contracts, CRTP only for shared behaviour

Status: Superseded by [ADR-0006](ADR-0006-concepts-only.md).

This record is kept as written so the history of the decision stays readable. ADR-0006 keeps its core (concepts
for every contract, no virtual functions, CRTP only where a base adds shared behaviour), makes it C++20 only, adds
a register of the permitted base classes with the reason for each, names how a HAL makes a run-time choice
without a virtual function, and records how kb-core relates to handbook ADR-0016.

## Context

The drivers define their ports as a CRTP base plus a concept that checks exact hook signatures, with no virtual
functions (handbook ADR-0003). Earlier layered stacks used abstract base classes with virtual functions for buses
and CRTP adapter bases for driver hooks. kb-core needs contracts for buses and pins that cost nothing at run time
and fail at compile time when a port is incomplete.

## Decision

- Every kb contract (`kb::iface::I2cBus`, `SpiDevice`, `UartPort`, ...) is a **concept only**, with no base class.
  A port satisfies it; a conformance `static_assert` per port proves it.
- **CRTP is used only where the base adds shared behaviour**: the per-bus hook bases in `kb::handlers`, the
  `kb::rtos` thread base and the handler base. A base that would only forward calls is not written.
- No virtual functions in any part. Where type erasure is unavoidable it is a function pointer plus a context
  pointer, at one named place.

## Consequences

- Static dispatch everywhere; errors in a port show at compile time with the concept's name.
- Generic code over contracts is templated; build times are watched, and headers stay small.

## Alternatives considered

Abstract base classes (virtual dispatch, a vtable per port, against the profile); CRTP everywhere (forwarding
boilerplate with no behaviour, and harder diagnostics than concepts).
