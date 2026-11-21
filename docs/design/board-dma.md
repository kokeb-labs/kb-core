# Board DMA: board.yml, the checker and the generator

A board declares its DMA memory regions, buffers and streams once, in `board.yml`. `kb image check` refuses every
conflict the routing data can see (rules R1 to R15), and `kb board gen` writes the C++ and linker files the firmware
builds with. The record is [ADR-0033](../adr/ADR-0033-dma-resources-declared-once.md); the run-time types are in
`kb::dma` ([DMA](../parts/dma.md)).

```text
board.yml --> kb image check --> findings (R1..R15) + resolved
                                      |
                                      +--> kb board gen --> board_dma.hpp  board_dma.cpp  kb_dma.ld
                                                            board_mpu.hpp  board_dma_report.{md,json}
```

The data behind the rules is plain YAML under `tools/kb/data/dma/` (one file per STM32 device, one for the ESP32
targets). The checker is a pure function of the board and that data: no clock, no file, no network.

## Commands

```bash
python3 tools/kb/kb.py image check BOARD [--sdkconfig FILE] [--drivers FILE] [--json] [--strict]
python3 tools/kb/kb.py image check --selftest
python3 tools/kb/kb.py board gen BOARD -o OUT_DIR
python3 tools/kb/dmadata.py --export-json dma-data.json
```

| Exit | Meaning |
| --- | --- |
| 0 | The board checks (warnings may be printed) |
| 1 | An error finding (with `--strict`, a warning too); `board gen` writes nothing |
| 2 | Bad arguments, or a file that cannot be read or parsed |

Findings go to standard error, one per line: `error dma.stream_used_twice dma1/0: <message>`. `--json` prints the
whole result (below) to standard output instead. `--sdkconfig` is an ESP-IDF `sdkconfig` or `sdkconfig.defaults`
(rule R13); `--drivers` is a JSON map `{driver: {"max_frame_bytes": n}}` that supplies the frame size of a device
whose `max_frame_bytes` the board leaves out (rule R9). `--selftest` runs every board vector and exits 0 only if each
one holds and every rule has a pass and a fail vector.

## board.yml

The file is YAML (or JSON). Names are lower-case words joined by single underscores (`spi1_stage_rx`); integers may be
written `0x30000000`. Expressions are refused (`bytes: 2*2*6` is an error: write 24). Unknown keys are errors, not
ignored. The schema is `catalog/schemas/board.schema.json`; the examples are
[`examples/boards/stm32h743-dma`](https://github.com/kokeb-labs/kb-core/tree/main/examples/boards/stm32h743-dma) and
[`esp32c6-dma`](https://github.com/kokeb-labs/kb-core/tree/main/examples/boards/esp32c6-dma).

| Key | Meaning |
| --- | --- |
| `board` | The board's name (a label in the generated files) |
| `mcu` | `{family: stm32, device: STM32H743, core: cm7}` or `{family: esp32, target: esp32c6}`; `core` is a core name or a list |
| `rtos` | `{name, max_syscall_priority, priority_bits}`; without it no stream may use `notify: task` |
| `memory.regions.<name>` | `{base, size, cache: none or maintained or coherent, line, align, mpu_region, masters}` |
| `dma.buffers.<name>` | `{region, bytes}` (an integer) |
| `dma.streams.<name>` (STM32) | `{engine, stream, request, priority, irq_priority, notify, irq, mode, buffer, frames, element_bytes, elements, frames_per_pass, trigger, rate_hz, stall_periods, fallback, core}` |
| `dma.streams.<name>` (ESP32) | `{driver: spi_master, adc_continuous, uhci, rmt or i2s, host, frame_bytes, frames, iram_safe}` |
| `buses.<name>` | `{kind: spi, uart or i2c, model, dma: [streams] or true or false, staging_bytes, devices: [{driver, max_frame_bytes}]}` |
| `handlers.<name>` | `{stream, frame_bytes, frames, conversions}`: what a handler expects of the stream (R10) |
| `budget` | `{fallback_cpu_percent}` (R15) |
| `errata` | `{workarounds: [rule ids]}`: a rule the board has worked around (R11) |
| `linker` | `{fixed_sections: [{name, base, size}], noload_list: <symbol prefix>}` |

Details that matter:

- A region's `cache: none` becomes `NonCacheable` on a core with a data cache and `Coherent` on one without. A `none`
  region on a cached core needs an `mpu_region`. `line` defaults to the core's data-cache line, `align` to the larger
  of the line and the DMA unit.
- `masters` lists the engines that use the region (`dma1`, `dma2`, `bdma`, `mdma`; `gdma` on ESP32). Without it the
  checker takes every engine the region's memory reaches.
- A stream with `frames` and `elements` is a ring (`mode: circular`): its buffer holds `frames * element_bytes *
  elements` bytes at least. `notify` is `task` (the interrupt wakes a task, so its priority must be at or below the
  RTOS limit) or `none`; `irq` is `all` or `errors_only`; `mode` is `normal` or `circular`; `priority` is `low`,
  `medium`, `high` or `very_high`.
- An ESP32 stream names a `driver`, not an engine: the checker counts GDMA channel pairs per target and the dedicated
  engines of the classic ESP32. A bus with `dma: true` needs `staging_bytes`.

## Rules

| Rule | What it refuses |
| --- | --- |
| R0 | A malformed board, an unknown device or target, a name that does not exist |
| R1 | A stream or DMAMUX channel used twice; an engine or unit that does not exist |
| R2 | A request that cannot reach the chosen stream (fixed tables on F4 and L4; DMAMUX groups on H7) |
| R3 | A buffer in a region the stream's engine cannot reach |
| R4 | A BDMA buffer outside SRAM4 and backup memory |
| R5 | Buffer size and alignment against the line, the master's rule, the ring and (ESP32) the SPI RX multiple and the ADC frame |
| R6 | A buffer in cacheable SDRAM, or in a `cache: none` region with no MPU region, on a core with a data cache |
| R7 | `notify: task` above the RTOS limit; a peripheral's streams at different priorities; a priority the part does not implement |
| R8 | More GDMA channel pairs, or dedicated engines, than the target has |
| R9 | A device's `max_frame_bytes` above what the bus carries (64 bytes without DMA on ESP32, the staging size with DMA) |
| R10 | A handler's frame size, frames or conversions that differ from its stream |
| R11 | Known errata: F4 DMA2 serving AHB and APB2 peripherals, ESP32 half-duplex DMA in both phases, DMAMUX synchronisation, an H7 stream used from both cores |
| R12 | A stream or buffer used from the wrong core; a CM4 buffer in SDRAM |
| R13 | `iram_safe` without the sdkconfig option (a warning when no sdkconfig is given) |
| R14 | Regions that overlap each other or a fixed linker section; an MPU region that is not a power of two, not aligned to its size or out of range |
| R15 | Fallback CPU load above `budget.fallback_cpu_percent` (a warning) |

### Codes

A finding is `{rule, code, severity, subject, message}`. The code is stable: the vectors pin it, and a tool may match on
it. The subject names the board item (`dma1/0`, a stream, a region, a bus). Messages name the item and the fix and may
change.

| Rule | Code | Severity | Meaning |
| --- | --- | --- | --- |
| R0 | `dma.board_invalid` | error | The board file is malformed or lacks a field the rules need |
| R0 | `dma.device_unknown` | error | No routing data for the device or target |
| R0 | `dma.master_unknown` | error | A region lists a master the device does not have |
| R0 | `dma.reference_unknown` | error | A name refers to a region, buffer, stream or bus that is not declared |
| R1 | `dma.buffer_shared` | error | Two streams of different transfers share one buffer |
| R1 | `dma.engine_unknown` | error | The engine is not one of the device's |
| R1 | `dma.mux_channel_used_twice` | error | Two streams use one DMAMUX channel |
| R1 | `dma.request_used_twice` | error | Two streams carry one request |
| R1 | `dma.stream_in_two_buses` | error | One stream is listed by two buses |
| R1 | `dma.stream_out_of_range` | error | The stream or channel number is outside the engine's |
| R1 | `dma.stream_used_twice` | error | Two streams use one stream or channel of one engine |
| R2 | `dma.request_not_routable` | error | The request cannot be routed to the chosen engine or stream |
| R2 | `dma.request_unknown` | error | The request is in no routing table of the device |
| R3 | `dma.region_masters_missing` | error | The region does not list the stream's engine as a master |
| R3 | `dma.region_masters_overclaim` | error | The region lists a master that cannot reach its memory |
| R3 | `dma.region_unknown_memory` | error | The region is not inside a memory of the device |
| R3 | `dma.region_unreachable` | error | The stream's engine cannot reach the memory of its buffer |
| R4 | `dma.bdma_buffer_outside_sram4` | error | A BDMA buffer is outside the D3 SRAM and backup memories |
| R5 | `dma.adc_frame_not_multiple` | error | The ADC frame is not a multiple of the conversion size |
| R5 | `dma.buffer_not_element_multiple` | error | A buffer is not a whole number of the stream's elements |
| R5 | `dma.buffer_too_small` | error | A stream's ring does not fit its buffer |
| R5 | `dma.region_align_below_dma_unit` | error | The region's alignment is below what the DMA needs |
| R5 | `dma.region_base_unaligned` | error | The region does not start on its alignment |
| R5 | `dma.region_line_below_cache_line` | error | A maintained region's line is below the core's cache line |
| R5 | `dma.region_line_invalid` | error | The region's line or alignment is not a power of two, or align < line |
| R5 | `dma.spi_rx_length_multiple` | error | The SPI staging size is not a multiple of the receive rounding |
| R6 | `dma.buffer_in_cacheable_sdram` | error | A DMA buffer is in cacheable SDRAM |
| R6 | `dma.noncacheable_without_mpu` | error | A cache: none region on a cached core has no MPU region |
| R6 | `dma.region_cache_policy` | error | The region's cache policy does not suit the target's memory |
| R7 | `dma.notify_above_rtos_limit` | error | A stream that notifies a task is more urgent than the RTOS allows |
| R7 | `dma.peripheral_priority_mismatch` | error | The streams of one peripheral run at different priorities |
| R7 | `dma.priority_out_of_range` | error | An interrupt priority is outside the implemented bits |
| R7 | `dma.rtos_missing` | error | A stream notifies a task but the board declares no rtos |
| R7 | `dma.rtos_priority_bits` | error | rtos.priority_bits differs from the device's implemented bits |
| R8 | `dma.bus_without_stream` | error | A bus has DMA but no stream declares it, so its pair is not counted |
| R8 | `dma.dedicated_dma_exceeded` | error | More users of a dedicated DMA channel than the target has |
| R8 | `dma.driver_without_dma` | error | The driver has no DMA on this target |
| R8 | `dma.gdma_pairs_exceeded` | error | More GDMA pairs are needed than the target has |
| R9 | `dma.driver_frame_exceeds_bus` | error | A driver's largest frame exceeds what its bus carries |
| R9 | `dma.staging_exceeds_buffer` | error | The staging size is larger than a stream's buffer |
| R9 | `dma.staging_missing` | error | A bus with DMA has no staging size |
| R9 | `dma.staging_too_large` | error | The staging size is above the target's largest DMA frame |
| R9 | `dma.staging_too_small` | error | The staging size is below the target's smallest DMA frame |
| R10 | `dma.handler_conversions_mismatch` | error | A handler's conversion count differs from the stream's |
| R10 | `dma.handler_frame_mismatch` | error | A handler's frame size differs from the stream's |
| R10 | `dma.handler_frames_mismatch` | error | A handler's frame count differs from the stream's |
| R10 | `dma.stream_shape_invalid` | error | A stream's frames, element size or elements are inconsistent |
| R11 | `dma.errata_dmamux_sync` | error | DMAMUX synchronisation or request generators are not supported |
| R11 | `dma.errata_esp32_half_duplex_dma` | error | Half-duplex SPI DMA with both phases on the ESP32 |
| R11 | `dma.errata_f4_dma2_ahb_apb2` | error | DMA2 serves an AHB and an APB2 peripheral at once (F4) |
| R11 | `dma.errata_stream_both_cores` | error | One stream is used from both cores |
| R12 | `dma.buffer_core_cannot_reach` | error | The image's core cannot reach the buffer's memory |
| R12 | `dma.cm4_buffer_in_sdram` | error | A CM4 DMA buffer is in external SDRAM (ES0445) |
| R12 | `dma.stream_wrong_core` | error | A stream belongs to the other core than this image's |
| R13 | `dma.iram_safe_option_missing` | error | iram_safe is declared without the matching sdkconfig option |
| R13 | `dma.sdkconfig_not_checked` | warning | iram_safe is declared but no sdkconfig was given |
| R14 | `dma.mpu_base_not_aligned` | error | An MPU region does not start on its size |
| R14 | `dma.mpu_region_out_of_range` | error | The MPU region number is not one of the device's |
| R14 | `dma.mpu_region_used_twice` | error | Two regions use one MPU region number |
| R14 | `dma.mpu_size_not_power_of_two` | error | An MPU region's size is not a power of two |
| R14 | `dma.mpu_size_too_small` | error | An MPU region is smaller than the MPU's minimum |
| R14 | `dma.region_overlap` | error | Two regions overlap |
| R14 | `dma.region_overlaps_fixed_section` | error | A region overlaps a fixed linker section |
| R14 | `dma.region_too_small` | error | The buffers of a region do not fit in it |
| R15 | `dma.fallback_cpu_over_budget` | warning | The declared fallbacks cost more CPU than the budget |

## Generated files

`kb board gen BOARD -o OUT` writes these files; a board that has an error finding writes none. They start with a
`generated by kb board gen from <board> - do not edit` line and are byte-stable (sorted, no clock).

| File | For | Content |
| --- | --- | --- |
| `board_dma.hpp` | all | `board::dma`: masters, one tag per region (`kPolicy`, `kLineBytes`, `kAlignBytes`, `kMasters`, `kBase`, `kSize`), one tag per stream (`kAssignment`, `kShape`), `Rtos` limits (only with an `rtos` key), one tag per bus (`kBus`), the `extern` buffers and `kChecksPass`: `static_assert`s of `RequireInterruptTier`, `RequireReachable`, `RequireWholeUnits`, `RequireSamePriority` and `RequireBusCarries`, so a board file that drifted from the checker does not build |
| `board_dma.cpp` | all | The `kb::dma::Buffer<Region, Bytes>` definitions, each in its region's section through `KB_DMA_SECTION(".kb_dma.<region>")` (an attribute on ELF targets for STM32, empty elsewhere): the one generated deviation from "no object in a named section" |
| `kb_dma.ld` | STM32 | One `NOLOAD` output section per region that holds a buffer, with start and end symbols (`<prefix>_<region>_start`, `_end`), and `ASSERT`s: inside the region, inside its MPU window, clear of `linker.fixed_sections` |
| `board_mpu.hpp` | cores with a data cache | `board::mpu::kRegions`: number, base, size and attributes of each non-cacheable window, for start-up to apply and the port to read back |
| `board_dma_report.md`, `board_dma_report.json` | all | Bus modes, bytes per region, GDMA pairs used, streams with priorities, findings |

Include `kb_dma.ld` in the board's linker script outside `SECTIONS`; the section is `NOLOAD`, so start-up zeroes it
(every buffer is valid all-zero) and may include it in an ECC scrub through the symbols.

Notes on limits. A bus without DMA carries 64 bytes on ESP32; on STM32 the interrupt path has no limit the checker
knows (the port states its own through `SpiLimits`). The buses of the ESP32 report `polled` when DMA is off and `dma`
otherwise; STM32 buses report `irq` or `dma`.

## Configurator API

The configurator (view V6, board checks in the browser) calls the same rules. Three things are published for it; the
site itself is not changed by this page.

### 1. The data bundle

```bash
python3 tools/kb/dmadata.py --export-json dma-data.json
```

writes every STM32 device and ESP32 target as one JSON file the site can fetch (sorted keys, two-space indent,
final newline; the same data gives the same bytes):

```json
{"schema": 1, "stm32": {"STM32H743": {...}}, "esp32": {"esp32c6": {...}}}
```

The ESP32 `common` facts are merged into every target. A part name finds the device whose key is the longest prefix
of it (`STM32H743XI` finds `STM32H743`).

### 2. The result

`check(board, data, sdkconfig=None, drivers=None)` returns:

```json
{"ok": true, "findings": [], "resolved": {}}
```

| Field | Content |
| --- | --- |
| `ok` | `true` when no finding has severity `error` |
| `findings` | Sorted by rule number, then subject; each `{rule, code, severity, subject, message}`; `severity` is `error` or `warning` |
| `resolved.regions` | Per region: `name`, `base`, `size`, `cache`, `policy` (`Coherent`, `NonCacheable` or `Maintained`), `line`, `align`, `mpu_region`, `masters`, `master_bits`, `memory` |
| `resolved.buffers` | `name`, `region`, `bytes`, `capacity` (padded to whole lines), `align`, `section` |
| `resolved.streams` | STM32: `name`, `engine`, `engine_id`, `unit`, `request`, `request_line`, `master_bit`, `mux`, `mux_channel`, `priority`, `irq_priority`, `notify`, `mode`, `buffer`, `shape`, and the rest of the declaration; ESP32: `name`, `driver`, `host`, `frame_bytes`, `frames`, `iram_safe`, `shape` |
| `resolved.buses` | `name`, `kind`, `mode` (`dma`, `irq` or `polled`), `streams`, `limits` (`dma`, `max_frame_dma`, `max_frame_no_dma`, `rx_length_multiple`, `align_bytes`), `devices`, `staging_bytes` |
| `resolved.gdma_pairs` | `{used, budget}` |
| `resolved.mpu` | One entry per non-cacheable window: `region`, `number`, `base`, `size`, `tex`, `c`, `b`, `s`, `xn` |
| `resolved.masters` | The master numbering used in `master_bits` (`dma1` 0, `dma2` 1, `bdma` 2, `mdma` 3; `gdma` 0 on ESP32) |
| `resolved.cached_core` | Whether the image runs on a core with a data cache |

`resolved` is what the emitters and the board views read. The codes table above is part of the interface.

### 3. The vectors are the contract

`tests/resolver/vectors/board/R<nn>-<pass|fail>[-<case>].json` hold a board, optionally `sdkconfig` (a map), `drivers`
and `strict`, and the expectation:

```json
{"description": "...", "board": {}, "expect": {"ok": false, "exit": 1,
  "findings": [{"rule": "R1", "code": "dma.stream_used_twice", "severity": "error", "subject": "dma1/0"}]}}
```

Every rule R1 to R15 has at least one pass and one fail vector, and every fail vector is a valid board with one seeded
conflict. The Python checker runs them (`kb image check --selftest`, the unit tests, and every fail vector through the
command line must exit 1). A future `configurator/boardcheck.mjs` must give the same `ok` and the same findings (the
four fields `rule`, `code`, `severity`, `subject`, in the same order) for every vector, and the same `resolved` for the
passing ones, as the resolver vectors already hold `resolver.mjs` to the Python resolver. Messages are free text and
are not compared.

## Build checks

- ctest `kb_board_gen_host` generates both example boards and compiles `board_dma.cpp` and a unit that includes
  `board_dma.hpp` with the host compiler against `kb::dma`, so the `static_assert`s run.
- CI job `board-dma` (`ci-cross-build.yml`) generates the H7 example, compiles `board_dma.cpp` for
  `-mcpu=cortex-m7`, and links it with a minimal linker script that includes `kb_dma.ld`: the sections and the
  `ASSERT`s are proven by the real linker.

## Limits of the data

The routing data is written by hand from the reference manuals. An entry that has not been compared with the manual
for the exact device is marked so in `tools/kb/data/dma/README.md`; a board for such a device is checked against the
best known table, not a verified one.
