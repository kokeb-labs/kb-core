# tools/kb/data/dma/

Routing and reach data for the board checker (`kb image check`, rules R1 to R15) and, later, the configurator. Plain
YAML: maps, lists, integers and strings; addresses and sizes are hex strings (`"0x30000000"`). The tables are written
by hand from the vendors' documents (owner decision 4): they are not generated from ST's open pin data, whose licence
and coverage are still to be checked. Where a table could only be cross-checked, its `verified` field says so.

## Files

| File | What |
| --- | --- |
| `stm32/stm32h743.yml` | DMA1, DMA2 (DMAMUX1), BDMA (DMAMUX2), MDMA of the STM32H743, memory reach, errata |
| `stm32/stm32h747.yml` | The same blocks on the dual-core STM32H747 (Cortex-M7 and Cortex-M4), per-core reach |
| `stm32/stm32g474.yml` | DMA1, DMA2 (8 channels each) and DMAMUX1 of the STM32G474 |
| `stm32/stm32f407.yml` | DMA1, DMA2 (8 streams, 8 channel selections) of the STM32F407, fixed request tables |
| `stm32/stm32l476.yml` | DMA1, DMA2 (7 channels each, `CSELR`) of the STM32L476, fixed request tables (incomplete) |
| `esp32/targets.yml` | GDMA groups and pairs, buffer rules and SDK options of ESP32, ESP32-S3, ESP32-C6, ESP32-P4 |

## Numbering shared with the ports

| | STM32 | ESP32 |
| --- | --- | --- |
| Master bits (`kb::dma::MasterBit(n)`) | `dma1` 0, `dma2` 1, `bdma` 2, `mdma` 3 | `gdma` 0, `spi_dma` 1, `i2s_dma` 2, `axi_gdma` 3 |
| Engine number (`StreamAssignment.engine`) | 0 none, 1 DMA1, 2 DMA2, 3 BDMA, 4 MDMA | not used |
| `stream` | 0-7 on F4 and H7 (streams); 1-8 on G4, 1-7 on L4 (channels) | not used |
| `request` | DMAMUX request line (H7, G4); channel selection 0-7 (F4); `CSELR` value 0-7 (L4) | not used |

The master names `dma1`, `dma2`, `bdma`, `mdma` are the keys of `engines`; a memory lists the masters that reach it.

## STM32 file layout

```yaml
device: STM32H743
family: h7                 # h7 | g4 | f4 | l4
source: {document: RM0433, revision: "unchecked", sections: [...]}
verified: {mux: hal-header, memories: unverified, errata: unverified}   # per table
cores: [cm7]               # h747: [cm7, cm4]
nvic_priority_bits: 4
engines:                   # name -> id, master_bit, units (streams|channels), first, count, mux, mux_channel_base
routing: mux               # mux (H7, G4) | fixed (F4, L4)
mux_groups:                # routing: mux. group -> channels, engines, max_request, generators
mux:                       # routing: mux. group -> request name -> request line
fixed:                     # routing: fixed. request name -> list of {engine, unit, select}
memories:                  # name -> base, size, masters, cached [, external]
mpu: {regions: 16, min_size: 32}
dcache_line: 32            # 0 when the core has no data cache
errata:                    # id, rule, applies_to, text, verified
```

- `units` is the kind of unit the engine numbers; `first` the number of the first one; `count` how many there are.
- `mux_channel_base`: the DMAMUX channel of unit `first` of the engine; unit `u` uses channel `base + (u - first)`. A
  DMAMUX channel is used by one unit only, so two engines of one group do not collide.
- `mux.<group>.GENERATOR<n>` are the DMAMUX request generator lines. They are in the table because they are lines of
  the manual; rule R11 refuses them while the DMAMUX erratum is open (`no_dmamux_sync`).
- A request table is the table of the manual's family. Some lines belong to peripherals that the device lacks (H743:
  CRYP and HASH exist on the H753 only; G474: AES exists on the G483 and G484 only). A board cannot use them anyway.
- `fixed.<request>`: every `(engine, unit, select)` that can serve the request. On F4 `unit` is the stream and
  `select` the channel (`CHSEL`); on L4 `unit` is the channel and `select` the `CSELR` field. The request names are
  the peripheral and direction as in the manual (`SPI1_RX`, `TIM2_CH3`, `DAC1_CH1`); a manual cell that names several
  requests (`TIM2_UP/TIM2_CH3`) is several entries with the same `(engine, unit, select)`.
- `complete: false` (L476): the table lists only part of the manual's requests, and `omitted` names the families
  left out. A request that is not in the table is "not in the table", not "not routable".
- `memories.<name>.masters` are the engines that reach the memory (bus matrix); `cached` is true when the Cortex-M7
  may cache it (reset-default attributes or the usual MPU set-up; the SDRAM entry is cacheable under the usual set-up,
  its reset default is Device). `external: true` marks a memory outside the chip. A memory with `masters: []` is not
  reachable by any DMA (STM32F407 CCM RAM, G474 CCM SRAM, L476 SRAM2 at its code-bus address).
- H747 only: `cpu_reach` lists the memories each core reaches, `mpu_by_core` and `dcache_line_by_core` hold the
  per-core values (the top-level `mpu` and `dcache_line` are those of the Cortex-M7).
- `errata`: `rule` is the name the checker's R11 uses (`bdma_individual_flag_clear`, `no_dmamux_sync`,
  `f4_dma2_ahb_apb2`, `cm4_sdram_byte_access`, `h7_dual_core_stream_both_cores`); `applies_to` is the engines, DMAMUX
  groups or cores it concerns. `kind: architecture` marks a rule of the manual that is not an erratum. The F4 erratum
  carries `ahb_requests` and `apb2_requests`: DMA2 serving one of each at the same time is the failing case.
  `no_dmamux_sync` also stands for the request generators.

## ESP32 file layout

`common` holds what is the same on all four targets (`max_dma_frame_bytes` 4092, `max_frame_no_dma_bytes` 64,
`adc_data_bytes_per_conv` 4, the drivers that take a GDMA pair, the SDK option names for IRAM-safe ADC continuous and
SPI master). `targets.<name>` holds:

| Field | Meaning |
| --- | --- |
| `gdma.groups`, `gdma.pairs_total` | GDMA groups (`ahb`, `axi`) and their transmit/receive pairs; ESP32 has none |
| `dedicated_dma` | ESP32: SPI2/SPI3 own DMA channels 1 and 2, ADC continuous on I2S0 |
| `master_bits` | the bits of the ESP32 column above that this target has |
| `spi_hosts_with_dma` | the SPI hosts that can use DMA |
| `pair_consumer_kinds` | per driver: `gdma` (one pair), `dedicated`, or `false` (no DMA on this target) |
| `spi_rx_length_multiple` | a DMA SPI receive length must be a multiple of this (ESP32 4, P4 64, others 1) |
| `buffer_align_bytes` | start and length alignment of a DMA buffer (4; P4 64) |
| `data_cache_line_bytes` | data cache line between the CPU and internal RAM (P4 64; others 0) |
| `psram_cache_line_bytes`, `psram_cache_line_default` | line sizes of the external RAM cache, by Kconfig |
| `psram_dma` | the GDMA can reach external RAM |
| `errata` | ESP32: half-duplex SPI with DMA and both phases (`esp32_half_duplex_dma_both_phases`) |

## Sources

| Table | Source | `verified` |
| --- | --- | --- |
| H743 `mux` (DMAMUX1, DMAMUX2) | STM32Cube HAL `stm32h7xx_hal_dma.h`, `stm32h7xx-hal-driver` v1.11.7, commit `65e5c7e40456f4d76d79f972738a16cb3a17762c` (the commit the STM32 port pins), restricted to the lines of RM0433 (DMAMUX1 1-115, DMAMUX2 1-17); the same numbers as the RM0433 DMAMUX request tables | `hal-header` |
| H747 `mux` | the same header; RM0399 lists the same lines (not read) | `hal-header` |
| G474 `mux` | `stm32g4xx_hal_dma.h`, `stm32g4xx-hal-driver` v1.2.7, commit `f5929f431f9effe45fbe18f5337e4753ced9ac92`; the RM0440 DMAMUX request table | `hal-header` |
| F407 `fixed` | RM0090 Tables 42 and 43, written from knowledge; agrees with the ChibiOS STM32F4xx registry (commit `fd2e59c34878b1ef93ef5358f11fb5cc830027ff`) and the `stm32f4xx-hal` crate (commit `0eb673dd2fc657a052e824f2693dcbd4e1a1b83d`); the requests neither covers are in `not_cross_checked` | `unverified` |
| L476 `fixed` | RM0351 Tables 41 and 42, the requests confirmed by the ChibiOS STM32L4xx registry; incomplete | `unverified` |
| Memories (all) | base and size from the CMSIS device headers pinned by the STM32 port (`cmsis-device-h7` v1.10.8, `-g4` v1.2.6, `-f4` v2.6.11, `-l4` v1.7.5); reach from the bus-matrix tables as summarised in the design (`kb-dma-architecture` 3.1) and from knowledge of the other manuals | `unverified` |
| Errata | ES0392 (H743), ES0445 (H747), ES0430 (G474), ES0182 (F407), as cited by the design; section numbers are not checked | `unverified` |
| ESP32 `targets` | ESP-IDF `release/v5.5` commit `a45de8ccca14543ede6e6342f1e16931a9c601ac` (branch head of 2026-11-15; tag v5.5.6 was the newest): `components/soc/<target>/include/soc/soc_caps.h`, `components/hal/include/hal/dma_types.h`, `components/esp_system/port/soc/<target>/Kconfig.cache`, `components/esp_adc/Kconfig`, `components/esp_driver_spi/Kconfig`, `docs/en/api-reference/peripherals/spi_master.rst` (Known Issues 1 and 5), `docs/en/api-reference/peripherals/adc_continuous.rst` (IRAM Safe) | `soc_caps` |

The ST reference manuals and errata sheets could not be downloaded when the tables were written (st.com refused the
requests), so no manual is recorded here with a hash, and every `revision` reads `"unchecked"`. A table marked
`unverified` must be read against the manual before an assurance document cites it. Document numbers: RM0433 and
RM0399 (STM32H7), RM0440 (STM32G4), RM0090 (STM32F405/407/415/417), RM0351 (STM32L47x/48x/49x/4Ax); AN4838 and AN4839
for the MPU and cache rules the board checker applies on top of this data.

`python3 tools/kb/tests/run.py` runs `tools/kb/tests/test_dma_data.py`: every file loads, request lines are unique and
in range inside a DMAMUX group, DMAMUX channels do not collide, every fixed entry names a real engine and unit, memories
do not overlap, master names exist, and about ten well-known entries per device are pinned.
