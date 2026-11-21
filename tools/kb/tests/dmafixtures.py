# Small inline routing data and boards for the unit tests of the DMA checker and generator: no file under
# tools/kb/data/dma/ is read, so a test states exactly the facts it depends on. The shared vectors
# (tests/resolver/vectors/board/) run against the real data files.
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
"""Tiny data sets (an H7-like device, an F4-like device, three ESP32 targets) and two valid boards."""

import copy

H7 = {
    "device": "STM32TEST7",
    "family": "h7",
    "cores": ["cm7", "cm4"],
    "nvic_priority_bits": 4,
    "engines": {
        "dma1": {"id": 1, "master_bit": 0, "units": "streams", "first": 0, "count": 8, "mux": "dmamux1"},
        "dma2": {"id": 2, "master_bit": 1, "units": "streams", "first": 0, "count": 8, "mux": "dmamux1"},
        "bdma": {"id": 3, "master_bit": 2, "units": "channels", "first": 0, "count": 8, "mux": "dmamux2"},
        "mdma": {"id": 4, "master_bit": 3, "units": "channels", "first": 0, "count": 16, "mux": "none"},
    },
    "routing": "mux",
    "mux": {
        "dmamux1": {"ADC1": 9, "ADC2": 10, "SPI1_RX": 37, "SPI1_TX": 38, "SPI2_RX": 39, "SPI2_TX": 40,
                    "USART1_RX": 41, "USART1_TX": 42},
        "dmamux2": {"LPUART1_RX": 9, "LPUART1_TX": 10, "SPI6_RX": 11, "SPI6_TX": 12, "ADC3": 17},
    },
    "memories": {
        "axi_sram": {"base": "0x24000000", "size": "0x80000", "masters": ["dma1", "dma2", "mdma"], "cached": True},
        "sram1": {"base": "0x30000000", "size": "0x20000", "masters": ["dma1", "dma2", "mdma"], "cached": True},
        "sram4": {"base": "0x38000000", "size": "0x10000", "masters": ["bdma", "dma1", "dma2", "mdma"],
                  "cached": True},
        "dtcm": {"base": "0x20000000", "size": "0x20000", "masters": ["mdma"], "cached": False},
        "sdram": {"base": "0xC0000000", "size": "0x10000000", "masters": ["dma1", "dma2", "mdma"], "cached": True,
                  "external": True},
    },
    "mpu": {"regions": 16, "min_size": 32},
    "dcache_line": 32,
    "errata": [{"id": "ES-TEST-1", "rule": "no_dmamux_sync", "text": "DMAMUX sync flags"}],
}

F4 = {
    "device": "STM32TEST4",
    "family": "f4",
    "cores": ["cm4"],
    "nvic_priority_bits": 4,
    "engines": {
        "dma1": {"id": 1, "master_bit": 0, "units": "streams", "first": 0, "count": 8, "mux": "none"},
        "dma2": {"id": 2, "master_bit": 1, "units": "streams", "first": 0, "count": 8, "mux": "none"},
    },
    "routing": "fixed",
    "fixed": {
        "SPI1_RX": [{"engine": "dma2", "unit": 0, "select": 3}, {"engine": "dma2", "unit": 2, "select": 3}],
        "SPI1_TX": [{"engine": "dma2", "unit": 3, "select": 3}, {"engine": "dma2", "unit": 5, "select": 3}],
        "ADC1": [{"engine": "dma2", "unit": 0, "select": 0}, {"engine": "dma2", "unit": 4, "select": 0}],
        "SPI2_RX": [{"engine": "dma1", "unit": 3, "select": 0}],
    },
    "memories": {"sram": {"base": "0x20000000", "size": "0x20000", "masters": ["dma1", "dma2"], "cached": False}},
    "mpu": {"regions": 8, "min_size": 32},
    "dcache_line": 0,
    "errata": [{"id": "ES-TEST-F4", "rule": "f4_dma2_ahb_apb2", "ahb": ["ADC1"], "apb2": ["SPI1"], "text": "x"}],
}

_ESP_COMMON = {
    "max_dma_frame_bytes": 4092,
    "max_frame_no_dma_bytes": 64,
    "adc_data_bytes_per_conv": 4,
    "sdkconfig": {"adc_continuous_iram_safe": "CONFIG_ADC_CONTINUOUS_ISR_IRAM_SAFE",
                  "spi_master_isr_iram": "CONFIG_SPI_MASTER_ISR_IN_IRAM"},
}
_GDMA_KINDS = {"spi_master": "gdma", "adc_continuous": "gdma", "uhci": "gdma", "rmt": False, "i2s": "gdma"}
ESP32 = {
    "esp32c6": {**_ESP_COMMON, "gdma": {"groups": [{"kind": "ahb", "pairs": 3}], "pairs_total": 3},
                "dedicated_dma": {}, "master_bits": {"gdma": 0}, "spi_hosts_with_dma": ["SPI2"],
                "pair_consumer_kinds": dict(_GDMA_KINDS), "spi_rx_length_multiple": 1, "buffer_align_bytes": 4,
                "data_cache_line_bytes": 0, "errata": []},
    "esp32": {**_ESP_COMMON, "gdma": {"groups": [], "pairs_total": 0},
              "dedicated_dma": {"spi_dma": {"hosts": ["SPI2", "SPI3"], "channels": [1, 2]}, "i2s_dma": {}},
              "master_bits": {"spi_dma": 1, "i2s_dma": 2}, "spi_hosts_with_dma": ["SPI2", "SPI3"],
              "pair_consumer_kinds": {"spi_master": "dedicated", "adc_continuous": "dedicated", "uhci": False,
                                      "rmt": False, "i2s": "dedicated"},
              "spi_rx_length_multiple": 4, "buffer_align_bytes": 4, "data_cache_line_bytes": 0,
              "errata": [{"id": "known-1", "rule": "esp32_half_duplex_dma_both_phases", "applies_to": ["spi_master"]}]},
    "esp32p4": {**_ESP_COMMON, "gdma": {"groups": [{"kind": "ahb", "pairs": 3}, {"kind": "axi", "pairs": 3}],
                                        "pairs_total": 6},
                "dedicated_dma": {}, "master_bits": {"gdma": 0, "axi_gdma": 3}, "spi_hosts_with_dma": ["SPI2", "SPI3"],
                "pair_consumer_kinds": {**_GDMA_KINDS, "rmt": "gdma"}, "spi_rx_length_multiple": 64,
                "buffer_align_bytes": 64, "data_cache_line_bytes": 64, "errata": []},
}

DATA = {"schema": 1, "stm32": {"STM32TEST7": H7, "STM32TEST4": F4}, "esp32": ESP32}

H7_BOARD = {
    "board": "test-board-h7",
    "mcu": {"family": "stm32", "device": "STM32TEST7XI", "core": "cm7"},
    "rtos": {"name": "freertos", "max_syscall_priority": 5, "priority_bits": 4},
    "memory": {"regions": {
        "d2_dma": {"base": "0x30000000", "size": "0x8000", "cache": "none", "mpu_region": 0, "masters": ["dma1", "dma2"]},
        "axi_dma": {"base": "0x24060000", "size": "0x10000", "cache": "maintained", "line": 32,
                    "masters": ["dma1", "dma2", "mdma"]},
        "dtcm": {"base": "0x20000000", "size": "0x20000", "cache": "none", "masters": ["mdma"]},
    }},
    "dma": {
        "buffers": {
            "spi1_stage_tx": {"region": "d2_dma", "bytes": 1028},
            "spi1_stage_rx": {"region": "d2_dma", "bytes": 1028},
            "adc1_scan": {"region": "axi_dma", "bytes": 24},
        },
        "streams": {
            "spi1_rx": {"engine": "dma1", "stream": 0, "request": "SPI1_RX", "priority": "high", "irq_priority": 5,
                        "notify": "task", "buffer": "spi1_stage_rx"},
            "spi1_tx": {"engine": "dma1", "stream": 1, "request": "SPI1_TX", "priority": "high", "irq_priority": 5,
                        "notify": "task", "buffer": "spi1_stage_tx", "irq": "errors_only"},
            "adc1": {"engine": "dma1", "stream": 2, "request": "ADC1", "mode": "circular", "frames": 2,
                     "element_bytes": 2, "elements": 6, "irq_priority": 4, "notify": "none", "buffer": "adc1_scan",
                     "trigger": "tim6_trgo", "rate_hz": 1000, "stall_periods": 10,
                     "fallback": {"kind": "oneshot", "cpu_percent": 2}},
        },
    },
    "buses": {"spi1": {"kind": "spi", "model": "owner", "dma": ["spi1_rx", "spi1_tx"],
                        "devices": [{"driver": "bno08x", "max_frame_bytes": 1024}]}},
    "handlers": {"adc1_handler": {"stream": "adc1", "frame_bytes": 12, "frames": 2, "conversions": 6}},
    "budget": {"fallback_cpu_percent": 10},
    "linker": {"fixed_sections": [{"name": ".bss", "base": "0x24000000", "size": "0x1000"}],
               "noload_list": "__board_dma"},
}

C6_BOARD = {
    "board": "test-board-c6",
    "mcu": {"family": "esp32", "target": "esp32c6"},
    "memory": {"regions": {"internal": {"cache": "none", "align": 4}}},
    "dma": {"streams": {
        "spi2": {"driver": "spi_master", "host": "SPI2"},
        "adc": {"driver": "adc_continuous", "frame_bytes": 256, "frames": 4, "iram_safe": True},
    }},
    "buses": {"spi2": {"kind": "spi", "model": "owner", "dma": True, "staging_bytes": 1028,
                        "devices": [{"driver": "bno08x", "max_frame_bytes": 1024}]}},
}


def h7_board():
    return copy.deepcopy(H7_BOARD)


def c6_board():
    return copy.deepcopy(C6_BOARD)


def data():
    return copy.deepcopy(DATA)
