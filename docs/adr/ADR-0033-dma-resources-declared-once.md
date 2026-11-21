# ADR-0033: DMA resources are declared once and checked before hardware

Status: Accepted (owner decision, 2026-11-15). The number is provisional until the record is merged.

## Context

DMA resources are scarce and constrained per device: fixed request tables on some STM32 families, a request
multiplexer on others, domain limits (a basic DMA reaches only the third memory domain, the general engines never reach
tightly coupled memory), cache and MPU attributes, ESP32 channel-pair counts, interrupt priorities against the RTOS
limit. Discovering these at run time means failing on the bench.

## Decision

1. `board.yml` declares regions, buffers and streams. `kb image check` and the configurator refuse conflicts (rules
   R1 to R15, [Board DMA](../design/board-dma.md)).
2. The generator (`kb board gen`) emits `constexpr` tables, the buffer definitions, a linker fragment with `ASSERT`s
   and the MPU table as data. Bindings `static_assert` driver frame sizes against port limits, so a board file that
   drifted from the checker does not build.
3. Ports verify placement and MPU attributes at start and refuse to start on a mismatch. There is no run-time stream
   allocation.
4. On ESP-IDF the port uses only the public peripheral drivers for DMA; the checker counts their channel pairs per
   target.
5. The routing data (which request reaches which stream, which memory each engine reaches, which errata apply) is
   plain data under `tools/kb/data/dma/`, written by hand from the reference manuals, and is exported as one JSON
   bundle for the configurator.
6. Every rule has at least one pass and one fail vector (`tests/resolver/vectors/board/`); the vectors are the
   contract that any other implementation of the checker must meet byte for byte.

## Consequences

- One source of truth per board; conflicts appear in CI or in the configurator, not on the bench.
- Port code stays free of allocation logic.
- Routing data must be maintained per device, and an entry that has not been checked against the reference manual
  is marked in the data.
- Generated board files carry one section-attribute deviation (`KB_DMA_SECTION`), noted in the generated file.

## Alternatives considered

- Run-time allocators: late failure, non-deterministic assignment.
- CubeMX or sdkconfig as the source: vendor-specific and a second source of truth.
