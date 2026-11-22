# The DMA routing data under tools/kb/data/dma/ is well formed and agrees with the sources it cites.
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
"""Tests of the DMA data files (tools/kb/data/dma)."""

import os
import re
import unittest

import yaml

from . import common

DATA = os.path.join(common.ROOT, "tools", "kb", "data", "dma")
STM32_DEVICES = ["stm32h743", "stm32h747", "stm32g474", "stm32f407", "stm32l476"]
ESP32_TARGETS = ["esp32", "esp32s3", "esp32c6", "esp32p4"]
VERIFIED = {"rm", "hal-header", "unverified"}
ENGINE_IDS = {"dma1": 1, "dma2": 2, "bdma": 3, "mdma": 4}          # StreamAssignment.engine
MASTER_BITS = {"dma1": 0, "dma2": 1, "bdma": 2, "mdma": 3}         # kb::dma::MasterBit(n), STM32 port
ESP_MASTER_BITS = {"gdma": 0, "spi_dma": 1, "i2s_dma": 2, "axi_gdma": 3}   # ESP32 port
ERRATA_RULES = {"bdma_individual_flag_clear", "no_dmamux_sync", "f4_dma2_ahb_apb2", "cm4_sdram_byte_access",
                "h7_dual_core_stream_both_cores", "esp32_half_duplex_dma_both_phases"}
ROUTING = {"h7": "mux", "g4": "mux", "f4": "fixed", "l4": "fixed"}


def load(*parts):
    with open(os.path.join(DATA, *parts), encoding="utf-8") as fh:
        return yaml.safe_load(fh)


def devices():
    return {name: load("stm32", name + ".yml") for name in STM32_DEVICES}


def fixed_set(dev, request):
    """{(engine, unit, select)} of one fixed-routing request."""
    return {(e["engine"], e["unit"], e["select"]) for e in dev["fixed"][request]}


class Stm32DataTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.dev = devices()

    def test_every_file_loads_and_has_the_header(self):
        for name, d in self.dev.items():
            with self.subTest(name):
                self.assertEqual(d["device"].lower(), name)
                self.assertIn(d["family"], ROUTING)
                self.assertEqual(d["routing"], ROUTING[d["family"]])
                self.assertEqual(d["nvic_priority_bits"], 4)
                self.assertIn(d["dcache_line"], (0, 32))
                self.assertTrue(set(d["cores"]) <= {"cm7", "cm4"})
                self.assertTrue(d["source"]["document"].startswith("RM"))
                self.assertTrue(set(d["verified"].values()) <= VERIFIED, d["verified"])

    def test_engine_ids_and_master_bits_follow_the_port_convention(self):
        for name, d in self.dev.items():
            ids, bits = set(), set()
            for eng, e in d["engines"].items():
                with self.subTest(name + "/" + eng):
                    self.assertEqual(e["id"], ENGINE_IDS[eng])
                    self.assertEqual(e["master_bit"], MASTER_BITS[eng])
                    self.assertIn(e["units"], ("streams", "channels"))
                    self.assertGreater(e["count"], 0)
                    ids.add(e["id"])
                    bits.add(e["master_bit"])
            self.assertEqual(len(ids), len(d["engines"]), name)
            self.assertEqual(len(bits), len(d["engines"]), name)
        # F4 and H7 number streams from 0, G4 and L4 number channels from 1 (StreamAssignment.stream)
        for name in ("stm32h743", "stm32h747", "stm32f407"):
            self.assertEqual(self.dev[name]["engines"]["dma1"]["first"], 0, name)
        for name in ("stm32g474", "stm32l476"):
            self.assertEqual(self.dev[name]["engines"]["dma1"]["first"], 1, name)
        self.assertEqual(self.dev["stm32l476"]["engines"]["dma1"]["count"], 7)
        self.assertEqual(self.dev["stm32g474"]["engines"]["dma2"]["count"], 8)

    def test_mux_lines_are_unique_and_in_range(self):
        for name, d in self.dev.items():
            if d["routing"] != "mux":
                self.assertNotIn("mux", d, name)
                continue
            for group, reqs in d["mux"].items():
                with self.subTest(name + "/" + group):
                    info = d["mux_groups"][group]
                    lines = list(reqs.values())
                    self.assertEqual(len(lines), len(set(lines)), "a request line is used twice")
                    self.assertTrue(all(isinstance(v, int) and 1 <= v <= info["max_request"] for v in lines))
                    self.assertEqual(max(lines), info["max_request"])
                    gens = [n for n in reqs if n.startswith("GENERATOR")]
                    self.assertEqual(sorted(reqs[n] for n in gens), list(range(1, info["generators"] + 1)))
                    for eng in info["engines"]:
                        self.assertEqual(d["engines"][eng]["mux"], group)
            groups = {e["mux"] for e in d["engines"].values()} - {"none"}
            self.assertEqual(groups, set(d["mux"]), name)

    def test_mux_channels_do_not_collide(self):
        for name, d in self.dev.items():
            if d["routing"] != "mux":
                continue
            used = {}
            for eng, e in d["engines"].items():
                if e["mux"] == "none":
                    continue
                for u in range(e["count"]):
                    ch = (e["mux"], e["mux_channel_base"] + u)
                    self.assertNotIn(ch, used, f"{name}: {eng} unit {u} shares DMAMUX channel {ch} with {used.get(ch)}")
                    used[ch] = eng
            for group, info in d["mux_groups"].items():
                chans = [c for (g, c) in used if g == group]
                self.assertEqual(len(chans), info["channels"], f"{name}/{group}")

    def test_fixed_entries_name_real_engines_and_units(self):
        for name, d in self.dev.items():
            if d["routing"] != "fixed":
                self.assertNotIn("fixed", d, name)
                continue
            for req, entries in d["fixed"].items():
                with self.subTest(name + "/" + req):
                    self.assertTrue(entries)
                    seen = set()
                    for e in entries:
                        eng = d["engines"][e["engine"]]
                        self.assertTrue(eng["first"] <= e["unit"] < eng["first"] + eng["count"], e)
                        self.assertTrue(0 <= e["select"] <= 7, e)
                        # a timer request may sit on one stream under two channel selections (F4 TIM1_CH3)
                        self.assertNotIn((e["engine"], e["unit"], e["select"]), seen, "an entry twice")
                        seen.add((e["engine"], e["unit"], e["select"]))

    def test_one_unit_has_one_request_per_selection(self):
        # On a fixed table a (engine, unit, select) names one request line; two different peripherals can only share
        # it when they are the same hardware request (a timer's UP and CH requests of one stream share a select).
        shared_ok = re.compile(r"^(TIM\d+_|I2S\d_EXT_|DAC1_)")
        for name, d in self.dev.items():
            if d["routing"] != "fixed":
                continue
            by_slot = {}
            for req, entries in d["fixed"].items():
                for e in entries:
                    by_slot.setdefault((e["engine"], e["unit"], e["select"]), []).append(req)
            for slot, reqs in by_slot.items():
                with self.subTest(name + str(slot)):
                    others = [r for r in reqs if not shared_ok.match(r)]
                    self.assertLessEqual(len(others), 1, reqs)

    def test_memories_do_not_overlap_and_name_real_masters(self):
        for name, d in self.dev.items():
            spans = []
            for mem, m in d["memories"].items():
                with self.subTest(name + "/" + mem):
                    self.assertIsInstance(m["base"], str)
                    self.assertTrue(m["base"].startswith("0x") and m["size"].startswith("0x"))
                    base, size = int(m["base"], 16), int(m["size"], 16)
                    self.assertGreater(size, 0)
                    self.assertLessEqual(base + size, 1 << 32)
                    self.assertTrue(set(m["masters"]) <= set(d["engines"]), m["masters"])
                    self.assertIsInstance(m["cached"], bool)
                    if d["dcache_line"] == 0:
                        self.assertFalse(m["cached"], "a part without a data cache has no cached memory")
                    spans.append((base, base + size, mem))
            spans.sort()
            for (a0, a1, an), (b0, b1, bn) in zip(spans, spans[1:]):
                self.assertLessEqual(a1, b0, f"{name}: {an} overlaps {bn}")

    def test_errata_are_well_formed(self):
        for name, d in self.dev.items():
            ids = [e["id"] for e in d["errata"]]
            self.assertEqual(len(ids), len(set(ids)), name)
            for e in d["errata"]:
                self.assertIn(e["rule"], ERRATA_RULES)
                self.assertTrue(e["text"].strip())
                self.assertIn(e["verified"], VERIFIED)
                self.assertTrue(set(e["applies_to"]) <= (set(d["engines"]) | set(d.get("mux_groups", {})) |
                                                         set(d["cores"])), e["applies_to"])

    def test_dual_core_part(self):
        d = self.dev["stm32h747"]
        self.assertEqual(d["cores"], ["cm7", "cm4"])
        self.assertEqual(d["dcache_line_by_core"], {"cm7": 32, "cm4": 0})
        self.assertNotIn("dtcm", d["cpu_reach"]["cm4"])
        self.assertIn("dtcm", d["cpu_reach"]["cm7"])
        self.assertTrue(set(d["cpu_reach"]["cm7"]) | set(d["cpu_reach"]["cm4"]) <= set(d["memories"]))
        rules = {e["rule"] for e in d["errata"]}
        self.assertTrue({"cm4_sdram_byte_access", "h7_dual_core_stream_both_cores"} <= rules)
        self.assertEqual(self.dev["stm32h743"]["cores"], ["cm7"])
        # the two H7 parts share the request tables
        self.assertEqual(self.dev["stm32h743"]["mux"], d["mux"])
        self.assertEqual(self.dev["stm32h743"]["memories"], d["memories"])

    # Spot checks. H7 and G4 values are the DMA_REQUEST_* / BDMA_REQUEST_* defines of the STM32Cube HAL headers
    # stm32h7xx_hal_dma.h (v1.11.7) and stm32g4xx_hal_dma.h (v1.2.7), the same numbers as the DMAMUX request tables of
    # RM0433 and RM0440. The F4 and L4 values are confirmed by two independent transcriptions (see the file
    # headers) and are still to be read against RM0090 Tables 42/43 and RM0351 Tables 41/42.
    def test_spot_checks_h7(self):
        for name in ("stm32h743", "stm32h747"):
            m1, m2 = self.dev[name]["mux"]["dmamux1"], self.dev[name]["mux"]["dmamux2"]
            for req, line in (("ADC1", 9), ("ADC2", 10), ("SPI1_RX", 37), ("SPI1_TX", 38), ("USART1_RX", 41),
                              ("TIM1_CH1", 11), ("SPI3_RX", 61), ("UART4_RX", 63), ("DAC1_CH1", 67), ("ADC3", 115)):
                self.assertEqual(m1[req], line, f"{name} DMAMUX1 {req}")
            for req, line in (("LPUART1_RX", 9), ("LPUART1_TX", 10), ("SPI6_RX", 11), ("SPI6_TX", 12),
                              ("I2C4_RX", 13), ("ADC3", 17)):
                self.assertEqual(m2[req], line, f"{name} DMAMUX2 {req}")
            self.assertNotIn(54, m1.values(), "line 54 is reserved")
            self.assertEqual(self.dev[name]["engines"]["mdma"]["count"], 16)
            self.assertEqual(self.dev[name]["memories"]["sram4"]["masters"], ["dma1", "dma2", "bdma", "mdma"])
            self.assertEqual(self.dev[name]["memories"]["dtcm"]["masters"], ["mdma"])
            self.assertNotIn("bdma", self.dev[name]["memories"]["axi_sram"]["masters"])
            self.assertEqual(self.dev[name]["memories"]["axi_sram"]["base"], "0x24000000")

    def test_spot_checks_g4(self):
        m = self.dev["stm32g474"]["mux"]["dmamux1"]
        for req, line in (("ADC1", 5), ("DAC1_CH1", 6), ("SPI1_RX", 10), ("SPI1_TX", 11), ("I2C1_RX", 16),
                          ("USART1_RX", 24), ("UART4_RX", 30), ("ADC2", 36), ("QUADSPI", 40), ("TIM1_CH1", 42)):
            self.assertEqual(m[req], line, req)
        self.assertEqual(self.dev["stm32g474"]["mux_groups"]["dmamux1"]["channels"], 16)

    def test_spot_checks_f407(self):
        d = self.dev["stm32f407"]
        self.assertEqual(fixed_set(d, "SPI1_RX"), {("dma2", 0, 3), ("dma2", 2, 3)})
        self.assertEqual(fixed_set(d, "SPI1_TX"), {("dma2", 3, 3), ("dma2", 5, 3)})
        self.assertEqual(fixed_set(d, "ADC1"), {("dma2", 0, 0), ("dma2", 4, 0)})
        self.assertEqual(fixed_set(d, "ADC2"), {("dma2", 2, 1), ("dma2", 3, 1)})
        self.assertEqual(fixed_set(d, "ADC3"), {("dma2", 0, 2), ("dma2", 1, 2)})
        self.assertEqual(fixed_set(d, "SDIO"), {("dma2", 3, 4), ("dma2", 6, 4)})
        self.assertEqual(fixed_set(d, "USART1_RX"), {("dma2", 2, 4), ("dma2", 5, 4)})
        self.assertEqual(fixed_set(d, "USART2_RX"), {("dma1", 5, 4)})
        self.assertEqual(fixed_set(d, "USART3_TX"), {("dma1", 3, 4), ("dma1", 4, 7)})
        self.assertEqual(fixed_set(d, "SPI3_TX"), {("dma1", 5, 0), ("dma1", 7, 0)})
        self.assertEqual(fixed_set(d, "I2C1_RX"), {("dma1", 0, 1), ("dma1", 5, 1)})
        self.assertEqual(fixed_set(d, "DAC1_CH1"), {("dma1", 5, 7)})
        self.assertEqual(d["memories"]["ccm_ram"]["masters"], [], "the CCM RAM is not reachable by DMA")
        self.assertEqual({e["rule"] for e in d["errata"]}, {"f4_dma2_ahb_apb2"})
        erratum = d["errata"][0]
        self.assertFalse(set(erratum["ahb_requests"]) & set(erratum["apb2_requests"]))
        self.assertTrue(set(erratum["ahb_requests"]) | set(erratum["apb2_requests"]) <= set(d["fixed"]) | {"TIM1_COM"})

    def test_spot_checks_l476(self):
        d = self.dev["stm32l476"]
        self.assertFalse(d["complete"])
        self.assertEqual(fixed_set(d, "ADC1"), {("dma1", 1, 0), ("dma2", 3, 0)})
        self.assertEqual(fixed_set(d, "SPI1_RX"), {("dma1", 2, 1), ("dma2", 3, 4)})
        self.assertEqual(fixed_set(d, "SPI1_TX"), {("dma1", 3, 1), ("dma2", 4, 4)})
        self.assertEqual(fixed_set(d, "USART1_TX"), {("dma1", 4, 2), ("dma2", 6, 2)})
        self.assertEqual(fixed_set(d, "USART1_RX"), {("dma1", 5, 2), ("dma2", 7, 2)})
        self.assertEqual(fixed_set(d, "I2C1_RX"), {("dma1", 7, 3), ("dma2", 6, 5)})
        self.assertEqual(fixed_set(d, "SDMMC1"), {("dma2", 4, 7), ("dma2", 5, 7)})
        self.assertEqual(fixed_set(d, "DAC1_CH1"), {("dma1", 3, 6), ("dma2", 4, 3)})
        self.assertEqual(fixed_set(d, "QUADSPI"), {("dma1", 5, 5), ("dma2", 7, 3)})
        self.assertEqual(d["memories"]["sram2"]["masters"], [])


class Esp32DataTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.data = load("esp32", "targets.yml")
        cls.targets = cls.data["targets"]

    def test_targets_and_common_values(self):
        self.assertEqual(sorted(self.targets), sorted(ESP32_TARGETS))
        c = self.data["common"]
        self.assertEqual(c["max_dma_frame_bytes"], 4092)
        self.assertEqual(c["max_frame_no_dma_bytes"], 64)
        self.assertEqual(c["adc_data_bytes_per_conv"], 4)
        self.assertEqual(self.data["source"]["branch"], "release/v5.5")
        self.assertRegex(self.data["source"]["commit"], r"^[0-9a-f]{40}$")
        for key in ("adc_continuous_iram_safe", "spi_master_isr_iram", "spi_master_iram"):
            self.assertRegex(c["sdkconfig"][key], r"^CONFIG_[A-Z0-9_]+$")

    def test_gdma_pairs_add_up(self):
        for name, t in self.targets.items():
            with self.subTest(name):
                self.assertEqual(sum(g["pairs"] for g in t["gdma"]["groups"]), t["gdma"]["pairs_total"])
                self.assertTrue({g["kind"] for g in t["gdma"]["groups"]} <= {"ahb", "axi"})
                self.assertTrue(set(t["master_bits"]) <= set(ESP_MASTER_BITS))
                for key, bit in t["master_bits"].items():
                    self.assertEqual(bit, ESP_MASTER_BITS[key])
                self.assertEqual(set(t["pair_consumer_kinds"]), set(self.data["common"]["pair_consumers"]))
                for kind in t["pair_consumer_kinds"].values():
                    self.assertIn(kind, ("gdma", "dedicated", False))
                self.assertGreaterEqual(t["buffer_align_bytes"], 4)
                self.assertEqual(t["buffer_align_bytes"] & (t["buffer_align_bytes"] - 1), 0)
                if t["gdma"]["pairs_total"] == 0:
                    self.assertNotIn("gdma", t["master_bits"])
                    self.assertNotIn("gdma", t["pair_consumer_kinds"].values())

    # Each value is the macro of the target's soc_caps.h at ESP-IDF release/v5.5 (commit in the file).
    def test_spot_checks(self):
        t = self.targets
        self.assertEqual(t["esp32c6"]["gdma"]["pairs_total"], 3)       # SOC_GDMA_PAIRS_PER_GROUP_MAX 3, one group
        self.assertEqual(t["esp32s3"]["gdma"]["pairs_total"], 5)       # SOC_GDMA_PAIRS_PER_GROUP 5, one group
        self.assertEqual([g["pairs"] for g in t["esp32p4"]["gdma"]["groups"]], [3, 3])   # AHB and AXI
        self.assertEqual([g["kind"] for g in t["esp32p4"]["gdma"]["groups"]], ["ahb", "axi"])
        self.assertEqual(t["esp32"]["gdma"]["pairs_total"], 0)
        self.assertEqual(t["esp32"]["dedicated_dma"]["spi_dma"]["channels"], [1, 2])   # SOC_SPI_DMA_CHAN_NUM 2
        self.assertEqual(t["esp32"]["spi_rx_length_multiple"], 4)      # spi_master.rst Known Issues 5
        self.assertEqual(t["esp32p4"]["spi_rx_length_multiple"], 64)
        self.assertEqual(t["esp32p4"]["buffer_align_bytes"], 64)
        self.assertEqual(t["esp32p4"]["data_cache_line_bytes"], 64)    # CONFIG_CACHE_L1_CACHE_LINE_SIZE 64
        self.assertEqual(t["esp32s3"]["psram_cache_line_bytes"], [16, 32, 64])
        self.assertEqual(t["esp32s3"]["psram_cache_line_default"], 32)
        self.assertEqual(t["esp32c6"]["spi_hosts_with_dma"], ["SPI2"])  # SOC_SPI_PERIPH_NUM 2
        self.assertIs(t["esp32c6"]["pair_consumer_kinds"]["rmt"], False)   # no SOC_RMT_SUPPORT_DMA
        self.assertEqual(t["esp32s3"]["pair_consumer_kinds"]["rmt"], "gdma")
        self.assertEqual(t["esp32p4"]["pair_consumer_kinds"]["rmt"], "gdma")
        self.assertIs(t["esp32"]["pair_consumer_kinds"]["uhci"], False)
        self.assertEqual(t["esp32"]["errata"][0]["rule"], "esp32_half_duplex_dma_both_phases")
        self.assertTrue(all(not t[n]["errata"] for n in ("esp32s3", "esp32c6", "esp32p4")))


if __name__ == "__main__":
    unittest.main()
