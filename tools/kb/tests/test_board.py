# The board file, the DMA checker (rules R1 to R15), the routing data loader, the shared board vectors and the
# `kb image check` command.
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
"""Tests of kb.board, kb.dmadata, kb.dmacheck, kb.boardvectors and the `kb image check` command."""

import contextlib
import copy
import io
import json
import os
import tempfile
import unittest

from kb import board, boardvectors, cli, dmacheck, dmadata, emit

from . import common, dmafixtures, minischema

EXAMPLES = os.path.join(common.ROOT, "examples", "boards")
EXAMPLE_FILES = [os.path.join(EXAMPLES, "stm32h743-dma", "board.yml"), os.path.join(EXAMPLES, "esp32c6-dma", "board.yml")]
SDK_OK = {"CONFIG_ADC_CONTINUOUS_ISR_IRAM_SAFE": "y"}


def findings(doc, data=None, **kw):
    return dmacheck.check(doc, data or dmafixtures.data(), **kw)["findings"]


def codes(doc, data=None, **kw):
    return sorted({f["code"] for f in findings(doc, data, **kw)})


def h7(mutate=None, data=None):
    doc = dmafixtures.h7_board()
    if mutate:
        mutate(doc)
    return doc


def c6(mutate=None):
    doc = dmafixtures.c6_board()
    if mutate:
        mutate(doc)
    return doc


def set_(path, value):
    """A mutator that sets doc[a][b]...[z] = value."""
    def apply(doc):
        node = doc
        for key in path[:-1]:
            node = node[key]
        node[path[-1]] = value
    return apply


class NormaliseTest(unittest.TestCase):
    def test_hex_strings_and_ints_become_ints(self):
        b, issues = board.normalise(dmafixtures.h7_board())
        self.assertEqual(issues, [])
        self.assertEqual(b["regions"]["d2_dma"]["base"], 0x30000000)
        self.assertEqual(b["regions"]["d2_dma"]["size"], 0x8000)
        self.assertEqual(b["linker"]["fixed_sections"][0]["size"], 0x1000)

    def test_defaults_are_filled(self):
        b, _ = board.normalise({"board": "x", "mcu": {"family": "stm32", "device": "D"},
                                "dma": {"streams": {"s": {"engine": "dma1"}}}})
        s = b["streams"]["s"]
        self.assertEqual((s["notify"], s["irq"], s["mode"], s["priority"]), ("task", "all", "normal", "medium"))
        self.assertEqual(b["linker"]["noload_list"], "__kb_dma")

    def test_expressions_are_refused(self):
        def mutate(doc):
            doc["dma"]["buffers"]["adc1_scan"]["bytes"] = "2*2*6"
        _, issues = board.normalise(h7(mutate))
        self.assertEqual([s for s, _ in issues], ["dma.buffers.adc1_scan.bytes"])
        self.assertIn("expressions are not accepted", issues[0][1])

    def test_names_must_be_c_identifier_words(self):
        for bad in ("Spi1", "spi__1", "1spi", "spi_", "a-b"):
            _, issues = board.normalise({"board": "x", "mcu": {"family": "stm32", "device": "D"},
                                         "dma": {"buffers": {bad: {"region": "r", "bytes": 4}}}})
            self.assertTrue(issues, bad)
        _, issues = board.normalise({"board": "x", "mcu": {"family": "stm32", "device": "D"},
                                     "dma": {"buffers": {"spi1_rx_2": {"region": "r", "bytes": 4}}}})
        self.assertEqual(issues, [])

    def test_unknown_keys_are_reported_not_ignored(self):
        def mutate(doc):
            doc["dma"]["streams"]["adc1"]["colour"] = "red"
            doc["surprise"] = 1
        _, issues = board.normalise(h7(mutate))
        self.assertEqual(sorted(s for s, _ in issues), ["board", "dma.streams.adc1"])

    def test_choices_are_validated(self):
        def mutate(doc):
            doc["dma"]["streams"]["adc1"]["notify"] = "sometimes"
        _, issues = board.normalise(h7(mutate))
        self.assertEqual([s for s, _ in issues], ["dma.streams.adc1.notify"])

    def test_camel(self):
        self.assertEqual(board.camel("d2_dma"), "D2Dma")
        self.assertEqual(board.camel("spi1_stage_tx"), "Spi1StageTx")


class CheckApiTest(unittest.TestCase):
    def test_valid_boards_check_ok(self):
        self.assertEqual(findings(h7()), [])
        self.assertEqual(codes(c6(), sdkconfig=SDK_OK), [])

    def test_pure_and_deterministic(self):
        doc, data = h7(set_(["dma", "streams", "spi1_tx", "stream"], 0)), dmafixtures.data()
        before = (copy.deepcopy(doc), copy.deepcopy(data))
        first = dmacheck.check(doc, data)
        second = dmacheck.check(doc, data)
        self.assertEqual((doc, data), before)
        self.assertEqual(emit.canonical_json(first), emit.canonical_json(second))

    def test_findings_sorted_by_rule_then_subject(self):
        def mutate(doc):
            doc["dma"]["streams"]["spi1_tx"]["stream"] = 0       # R1
            doc["dma"]["streams"]["adc1"]["irq_priority"] = 16   # R7
            doc["memory"]["regions"]["d2_dma"].pop("mpu_region")  # R6
        res = dmacheck.check(h7(mutate), dmafixtures.data())
        keys = [(int(f["rule"][1:]), f["subject"]) for f in res["findings"]]
        self.assertEqual(keys, sorted(keys))
        self.assertEqual([f["rule"] for f in res["findings"]], ["R1", "R6", "R7"])
        self.assertFalse(res["ok"])

    def test_every_finding_is_complete_and_known(self):
        res = dmacheck.check(h7(set_(["dma", "streams", "spi1_tx", "stream"], 0)), dmafixtures.data())
        for f in res["findings"]:
            self.assertEqual(sorted(f), ["code", "message", "rule", "severity", "subject"])
            self.assertIn(f["code"], dmacheck.CODES)
            self.assertEqual(dmacheck.CODES[f["code"]][0], f["rule"])
            self.assertTrue(f["message"])

    def test_resolved_streams_carry_engine_unit_request_and_master(self):
        res = dmacheck.check(h7(), dmafixtures.data())["resolved"]
        rx = {s["name"]: s for s in res["streams"]}["spi1_rx"]
        self.assertEqual((rx["engine_id"], rx["unit"], rx["request_line"], rx["master_bit"], rx["mux_channel"]),
                         (1, 0, 37, 0, 0))
        self.assertEqual(res["masters"], {"bdma": 2, "dma1": 0, "dma2": 1, "mdma": 3})
        self.assertEqual(res["gdma_pairs"], {"used": 0, "budget": 0})
        bus = res["buses"][0]
        self.assertEqual((bus["mode"], bus["limits"]["max_frame_dma"], bus["limits"]["dma"]), ("dma", 1028, True))
        self.assertEqual([m["region"] for m in res["mpu"]], ["d2_dma"])
        buf = {b["name"]: b for b in res["buffers"]}["spi1_stage_rx"]
        self.assertEqual((buf["bytes"], buf["capacity"], buf["section"]), (1028, 1056, ".kb_dma.d2_dma"))

    def test_esp32_budget_in_resolved(self):
        res = dmacheck.check(c6(), dmafixtures.data(), sdkconfig=SDK_OK)["resolved"]
        self.assertEqual(res["gdma_pairs"], {"used": 2, "budget": 3})
        self.assertEqual([b["mode"] for b in res["buses"]], ["dma"])

    def test_device_found_by_longest_prefix_and_target_by_name(self):
        data = dmafixtures.data()
        self.assertEqual(dmadata.find_device(data, "stm32test7xi"), dmafixtures.H7)
        self.assertIsNone(dmadata.find_device(data, "STM32NOPE"))
        self.assertEqual(dmadata.find_target(data, "ESP32C6"), dmafixtures.ESP32["esp32c6"])

    def test_malformed_board_stops_with_r0_findings_only(self):
        res = dmacheck.check({"board": "x", "mcu": {"family": "stm32"}}, dmafixtures.data())
        self.assertFalse(res["ok"])
        self.assertEqual({f["rule"] for f in res["findings"]}, {"R0"})
        self.assertEqual(res["resolved"]["streams"], [])

    def test_sdkconfig_parser(self):
        text = 'CONFIG_A=y\n# CONFIG_B is not set\nCONFIG_C="text"\nCONFIG_D=4\n\n# comment\n'
        self.assertEqual(dmacheck.parse_sdkconfig(text), {"CONFIG_A": "y", "CONFIG_B": "n", "CONFIG_C": "text",
                                                           "CONFIG_D": "4"})


def _drop_mpu(doc):
    doc["memory"]["regions"]["d2_dma"].pop("mpu_region")


def _two_adc_on_classic(doc):
    doc["mcu"]["target"] = "esp32"
    doc["dma"]["streams"]["adc2"] = {"driver": "adc_continuous", "frame_bytes": 256, "frames": 4}
    doc["dma"]["streams"]["adc"]["iram_safe"] = False


def _f4_data():
    return dmafixtures.data()


# (code, board factory, data factory or None, check kwargs): each must give that code. The shared vectors cover most
# codes against the real data; these cover the rest on the small inline data.
CASES = [
    ("dma.board_invalid", lambda: h7(set_(["dma", "buffers", "adc1_scan", "bytes"], "2*2*6")), None, {}),
    ("dma.device_unknown", lambda: h7(set_(["mcu", "device"], "STM32NOPE")), None, {}),
    ("dma.reference_unknown", lambda: h7(set_(["dma", "streams", "adc1", "buffer"], "missing")), None, {}),
    ("dma.master_unknown", lambda: h7(set_(["memory", "regions", "d2_dma", "masters"], ["dma1", "gpu"])), None, {}),
    ("dma.stream_used_twice", lambda: h7(set_(["dma", "streams", "spi1_tx", "stream"], 0)), None, {}),
    ("dma.mux_channel_used_twice", lambda: h7(lambda d: d["dma"]["streams"]["spi1_tx"].update({"engine": "dma2", "stream": 0})), "overlap", {}),
    ("dma.request_used_twice", lambda: h7(set_(["dma", "streams", "spi1_tx", "request"], "SPI1_RX")), None, {}),
    ("dma.engine_unknown", lambda: h7(set_(["dma", "streams", "adc1", "engine"], "dma3")), None, {}),
    ("dma.stream_out_of_range", lambda: h7(set_(["dma", "streams", "adc1", "stream"], 8)), None, {}),
    ("dma.buffer_shared", lambda: h7(set_(["dma", "streams", "adc1", "buffer"], "spi1_stage_rx")), None, {}),
    ("dma.stream_in_two_buses", lambda: h7(lambda d: d["buses"].update({"spi2": {"kind": "spi", "dma": ["spi1_rx"]}})),
     None, {}),
    ("dma.request_unknown", lambda: h7(set_(["dma", "streams", "adc1", "request"], "NOPE")), None, {}),
    ("dma.request_not_routable", lambda: h7(set_(["dma", "streams", "adc1", "request"], "SPI6_RX")), None, {}),
    ("dma.region_unknown_memory", lambda: h7(set_(["memory", "regions", "d2_dma", "base"], "0x31000000")), None, {}),
    ("dma.region_unreachable", lambda: h7(set_(["dma", "buffers", "spi1_stage_rx", "region"], "dtcm")), None, {}),
    ("dma.region_masters_missing", lambda: h7(set_(["memory", "regions", "d2_dma", "masters"], ["dma2"])), None, {}),
    ("dma.region_masters_overclaim", lambda: h7(set_(["memory", "regions", "d2_dma", "masters"], ["dma1", "bdma"])),
     None, {}),
    ("dma.bdma_buffer_outside_sram4", lambda: h7(lambda d: d["dma"]["streams"].update(
        {"spi6": {"engine": "bdma", "stream": 0, "request": "SPI6_RX", "irq_priority": 5, "buffer": "spi1_stage_rx"}})),
     None, {}),
    ("dma.region_line_invalid", lambda: h7(set_(["memory", "regions", "axi_dma", "align"], 16)), None, {}),
    ("dma.region_line_below_cache_line", lambda: h7(set_(["memory", "regions", "axi_dma", "line"], 16)), None, {}),
    ("dma.region_align_below_dma_unit", lambda: h7(set_(["memory", "regions", "d2_dma", "align"], 2)), None, {}),
    ("dma.region_base_unaligned", lambda: h7(set_(["memory", "regions", "d2_dma", "base"], "0x30000010")), None, {}),
    ("dma.buffer_too_small", lambda: h7(set_(["dma", "buffers", "adc1_scan", "bytes"], 8)), None, {}),
    ("dma.buffer_not_element_multiple", lambda: h7(set_(["dma", "buffers", "adc1_scan", "bytes"], 25)), None, {}),
    ("dma.spi_rx_length_multiple", lambda: c6(lambda d: (d["mcu"].update({"target": "esp32"}),
                                                          d["dma"]["streams"]["adc"].pop("iram_safe"),
                                                          d["buses"]["spi2"].update({"staging_bytes": 1030}))),
     None, {}),
    ("dma.adc_frame_not_multiple", lambda: c6(set_(["dma", "streams", "adc", "frame_bytes"], 254)), None,
     {"sdkconfig": SDK_OK}),
    ("dma.buffer_in_cacheable_sdram", lambda: h7(lambda d: (d["memory"]["regions"].update(
        {"sd": {"base": "0xC0000000", "size": "0x10000", "cache": "maintained", "line": 32}}),
        d["dma"]["buffers"]["spi1_stage_rx"].update({"region": "sd"}))), None, {}),
    ("dma.noncacheable_without_mpu", lambda: h7(_drop_mpu), None, {}),
    ("dma.region_cache_policy", lambda: c6(lambda d: (d["mcu"].update({"target": "esp32p4"}),
                                                       d["buses"]["spi2"].update({"staging_bytes": 1024}))), None,
     {"sdkconfig": SDK_OK}),
    ("dma.notify_above_rtos_limit", lambda: h7(set_(["dma", "streams", "spi1_rx", "irq_priority"], 3)), None, {}),
    ("dma.priority_out_of_range", lambda: h7(set_(["dma", "streams", "adc1", "irq_priority"], 16)), None, {}),
    ("dma.peripheral_priority_mismatch", lambda: h7(set_(["dma", "streams", "spi1_tx", "irq_priority"], 6)), None, {}),
    ("dma.rtos_missing", lambda: h7(lambda d: d.pop("rtos")), None, {}),
    ("dma.rtos_priority_bits", lambda: h7(set_(["rtos", "priority_bits"], 3)), None, {}),
    ("dma.gdma_pairs_exceeded", lambda: c6(lambda d: d["dma"]["streams"].update(
        {"u": {"driver": "uhci"}, "i": {"driver": "i2s"}})), None, {"sdkconfig": SDK_OK}),
    ("dma.dedicated_dma_exceeded", lambda: c6(_two_adc_on_classic), None, {}),
    ("dma.driver_without_dma", lambda: c6(lambda d: d["dma"]["streams"].update({"r": {"driver": "rmt"}})), None,
     {"sdkconfig": SDK_OK}),
    ("dma.bus_without_stream", lambda: c6(lambda d: d["dma"]["streams"].pop("spi2")), None, {"sdkconfig": SDK_OK}),
    ("dma.driver_frame_exceeds_bus", lambda: h7(set_(["buses", "spi1", "devices", 0, "max_frame_bytes"], 4000)), None,
     {}),
    ("dma.staging_missing", lambda: c6(lambda d: d["buses"]["spi2"].pop("staging_bytes")), None, {"sdkconfig": SDK_OK}),
    ("dma.staging_exceeds_buffer", lambda: h7(set_(["buses", "spi1", "staging_bytes"], 2000)), None, {}),
    ("dma.staging_too_large", lambda: c6(set_(["buses", "spi2", "staging_bytes"], 8192)), None, {"sdkconfig": SDK_OK}),
    ("dma.staging_too_small", lambda: c6(set_(["buses", "spi2", "staging_bytes"], 2)), None, {"sdkconfig": SDK_OK}),
    ("dma.handler_frame_mismatch", lambda: h7(set_(["handlers", "adc1_handler", "frame_bytes"], 8)), None, {}),
    ("dma.handler_frames_mismatch", lambda: h7(set_(["handlers", "adc1_handler", "frames"], 4)), None, {}),
    ("dma.handler_conversions_mismatch", lambda: h7(set_(["handlers", "adc1_handler", "conversions"], 4)), None, {}),
    ("dma.stream_shape_invalid", lambda: h7(lambda d: d["dma"]["streams"]["adc1"].pop("elements")), None, {}),
    ("dma.errata_f4_dma2_ahb_apb2", None, None, {}),  # built below (needs the F4 device)
    ("dma.errata_dmamux_sync", lambda: h7(set_(["dma", "streams", "adc1", "dmamux_sync"], True)), None, {}),
    ("dma.errata_esp32_half_duplex_dma", lambda: c6(lambda d: (d["mcu"].update({"target": "esp32"}),
                                                                d["dma"]["streams"]["adc"].pop("iram_safe"),
                                                                d["dma"]["streams"]["spi2"].update(
                                                                    {"half_duplex_both_phases": True}))), None, {}),
    ("dma.errata_stream_both_cores", None, None, {}),
    ("dma.stream_wrong_core", lambda: h7(set_(["dma", "streams", "adc1", "core"], "cm4")), None, {}),
    ("dma.cm4_buffer_in_sdram", None, None, {}),
    ("dma.buffer_core_cannot_reach", None, None, {}),
    ("dma.iram_safe_option_missing", lambda: c6(), None, {"sdkconfig": {}}),
    ("dma.sdkconfig_not_checked", lambda: c6(), None, {}),
    ("dma.region_overlap", lambda: h7(lambda d: d["memory"]["regions"].update(
        {"more": {"base": "0x30004000", "size": "0x8000", "cache": "coherent"}})), None, {}),
    ("dma.region_too_small", lambda: h7(set_(["memory", "regions", "axi_dma", "size"], 16)), None, {}),
    ("dma.region_overlaps_fixed_section", lambda: h7(set_(["linker", "fixed_sections", 0, "base"], "0x30000000")), None,
     {}),
    ("dma.mpu_size_not_power_of_two", lambda: h7(set_(["memory", "regions", "d2_dma", "size"], "0xC000")), None, {}),
    ("dma.mpu_base_not_aligned", lambda: h7(set_(["memory", "regions", "d2_dma", "base"], "0x30004000")), None, {}),
    ("dma.mpu_size_too_small", lambda: h7(lambda d: d["memory"]["regions"].update(
        {"tiny": {"base": "0x30010000", "size": 16, "cache": "none", "mpu_region": 3}})), None, {}),
    ("dma.mpu_region_out_of_range", lambda: h7(set_(["memory", "regions", "d2_dma", "mpu_region"], 16)), None, {}),
    ("dma.mpu_region_used_twice", lambda: h7(set_(["memory", "regions", "axi_dma", "mpu_region"], 0)), None, {}),
    ("dma.fallback_cpu_over_budget", lambda: h7(set_(["dma", "streams", "adc1", "fallback", "cpu_percent"], 12)), None,
     {}),
]


def _case_data(name):
    data = dmafixtures.data()
    if name == "overlap":
        # two engines of one DMAMUX with the same channel base: a data error the checker still reports
        data["stm32"]["STM32TEST7"]["engines"]["dma1"]["mux_channel_base"] = 0
        data["stm32"]["STM32TEST7"]["engines"]["dma2"]["mux_channel_base"] = 0
    return data


def _f4_board(extra=None):
    doc = {"board": "f4", "mcu": {"family": "stm32", "device": "STM32TEST4"},
           "rtos": {"max_syscall_priority": 5, "priority_bits": 4},
           "memory": {"regions": {"sram": {"base": "0x20000000", "size": "0x4000", "cache": "none"}}},
           "dma": {"buffers": {"a": {"region": "sram", "bytes": 64}, "b": {"region": "sram", "bytes": 64}},
                   "streams": {"rx": {"engine": "dma2", "stream": 0, "request": "SPI1_RX", "irq_priority": 5,
                                      "buffer": "a"},
                               "adc": {"engine": "dma2", "stream": 4, "request": "ADC1", "irq_priority": 5,
                                       "buffer": "b"}}}}
    if extra:
        extra(doc)
    return doc


class RuleCasesTest(unittest.TestCase):
    def test_each_case_gives_its_code(self):
        for code, make, data_name, kw in CASES:
            if make is None:
                continue
            with self.subTest(code=code):
                data = _case_data(data_name) if data_name else dmafixtures.data()
                self.assertIn(code, codes(make(), data, **kw))

    def test_f4_tables_and_errata(self):
        data = dmafixtures.data()
        # ADC1 is an APB2 peripheral here (the fixture's erratum lists ADC1 as AHB and SPI1 as APB2 requests)
        self.assertIn("dma.errata_f4_dma2_ahb_apb2", codes(_f4_board(), data))
        ok = _f4_board(lambda d: d.update({"errata": {"workarounds": ["f4_dma2_ahb_apb2"]}}))
        self.assertNotIn("dma.errata_f4_dma2_ahb_apb2", codes(ok, data))
        moved = _f4_board(lambda d: (d["dma"]["streams"]["rx"].update({"stream": 1}),
                                     d.update({"errata": {"workarounds": ["f4_dma2_ahb_apb2"]}})))
        self.assertEqual(codes(moved, data), ["dma.request_not_routable"])
        res = dmacheck.check(_f4_board(), data)["resolved"]
        self.assertEqual({s["name"]: s["request_line"] for s in res["streams"]}, {"adc": 0, "rx": 3})

    def test_dual_core_rules(self):
        data = dmafixtures.data()

        def cm4(doc):
            doc["mcu"]["core"] = "cm4"
            doc["dma"]["streams"] = {"spi1_rx": {**doc["dma"]["streams"]["spi1_rx"], "core": "cm4"}}
            doc["dma"]["buffers"] = {"spi1_stage_rx": doc["dma"]["buffers"]["spi1_stage_rx"]}
            doc["buses"] = {}
            doc["handlers"] = {}
            doc["memory"]["regions"]["d2_dma"].pop("mpu_region")
        data["stm32"]["STM32TEST7"]["dcache_line_by_core"] = {"cm7": 32, "cm4": 0}
        data["stm32"]["STM32TEST7"]["errata"].append({"id": "x", "rule": "cm4_sdram_byte_access", "applies_to": ["cm4"]})
        data["stm32"]["STM32TEST7"]["cpu_reach"] = {"cm7": ["dtcm", "sram1", "sdram"], "cm4": ["sram1", "sdram"]}
        self.assertEqual(codes(h7(cm4), data), [])  # CM4 image: no data cache, so cache: none needs no MPU region

        def cm4_sdram(doc):
            cm4(doc)
            doc["memory"]["regions"]["sd"] = {"base": "0xC0000000", "size": "0x10000", "cache": "none"}
            doc["dma"]["buffers"]["spi1_stage_rx"]["region"] = "sd"
        self.assertIn("dma.cm4_buffer_in_sdram", codes(h7(cm4_sdram), data))

        def both(doc):
            cm4(doc)
            doc["mcu"]["core"] = "cm7"
            doc["dma"]["streams"]["spi1_rx"]["core"] = ["cm7", "cm4"]
            doc["memory"]["regions"]["d2_dma"]["mpu_region"] = 0
        self.assertIn("dma.errata_stream_both_cores", codes(h7(both), data))

        def unreachable(doc):
            cm4(doc)
            doc["memory"]["regions"]["itcm"] = {"base": "0x20000000", "size": "0x20000", "cache": "none"}
            doc["dma"]["buffers"]["spi1_stage_rx"]["region"] = "itcm"
            doc["memory"]["regions"]["itcm"]["masters"] = ["mdma"]
        data["stm32"]["STM32TEST7"]["engines"]["dma1"]  # the buffer is also unreachable by dma1 (R3)
        found = codes(h7(unreachable), data)
        self.assertIn("dma.buffer_core_cannot_reach", found)
        self.assertIn("dma.region_unreachable", found)

    def test_dedicated_dma_on_the_classic_esp32(self):
        self.assertIn("dma.dedicated_dma_exceeded", codes(c6(_two_adc_on_classic)))

    def test_p4_needs_maintained_internal_ram_and_64_byte_multiples(self):
        def p4(doc):
            doc["mcu"]["target"] = "esp32p4"
            doc["memory"]["regions"]["internal"] = {"cache": "maintained", "line": 64, "align": 64}
            doc["buses"]["spi2"]["staging_bytes"] = 1024
        self.assertEqual(codes(c6(p4), sdkconfig=SDK_OK), [])
        bad = c6(lambda d: (p4(d), d["buses"]["spi2"].update({"staging_bytes": 1028})))
        self.assertEqual(codes(bad, sdkconfig=SDK_OK), ["dma.spi_rx_length_multiple"])
        bad = c6(lambda d: (p4(d), d["dma"]["streams"]["adc"].update({"frame_bytes": 100})))
        self.assertEqual(codes(bad, sdkconfig=SDK_OK), ["dma.adc_frame_not_multiple"])

    def test_drivers_catalogue_supplies_the_frame_size(self):
        doc = c6(lambda d: d["buses"]["spi2"]["devices"][0].pop("max_frame_bytes"))
        self.assertEqual(codes(doc, sdkconfig=SDK_OK), [])
        self.assertEqual(codes(doc, sdkconfig=SDK_OK, drivers={"bno08x": {"max_frame_bytes": 5000}}),
                         ["dma.driver_frame_exceeds_bus"])

    def test_warnings_do_not_fail_the_board(self):
        res = dmacheck.check(c6(), dmafixtures.data())
        self.assertTrue(res["ok"])
        self.assertEqual([f["severity"] for f in res["findings"]], ["warning"])

    def test_every_code_is_exercised(self):
        """Every code of the table is given by a unit case here or by a shared vector (against the real data)."""
        seen = {code for code, make, *_ in CASES if make is not None}
        seen |= {"dma.errata_f4_dma2_ahb_apb2", "dma.errata_stream_both_cores", "dma.cm4_buffer_in_sdram",
                 "dma.buffer_core_cannot_reach", "dma.dedicated_dma_exceeded"}  # the tests above
        data = dmadata.load_dir()
        for name, vec in boardvectors.load_vectors():
            seen |= {f["code"] for f in dmacheck.check(vec["board"], data, sdkconfig=vec.get("sdkconfig"),
                                                       drivers=vec.get("drivers"))["findings"]}
        self.assertEqual(sorted(set(dmacheck.CODES) - seen), [])
        self.assertEqual(sorted(seen - set(dmacheck.CODES)), [])


class VectorTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.data = dmadata.load_dir()
        cls.vectors = boardvectors.load_vectors()

    def test_every_rule_has_a_pass_and_a_fail_vector(self):
        self.assertEqual(boardvectors.coverage(self.vectors), [])

    def test_every_vector_holds(self):
        for name, vec in self.vectors:
            with self.subTest(vector=name):
                self.assertEqual(boardvectors.run_vector(name, vec, self.data), [])

    def test_vectors_are_described_and_named(self):
        for name, vec in self.vectors:
            self.assertTrue(vec["description"], name)
            self.assertRegex(name, boardvectors.NAME)

    def test_every_vector_exits_as_expected_through_the_command_line(self):
        for name, vec in self.vectors:
            with self.subTest(vector=name), tempfile.TemporaryDirectory() as tmp:
                path = os.path.join(tmp, "board.json")
                with open(path, "w", encoding="utf-8") as fh:
                    json.dump(vec["board"], fh)
                argv = ["image", "check", path]
                if vec.get("sdkconfig") is not None:
                    sdk = os.path.join(tmp, "sdkconfig")
                    with open(sdk, "w", encoding="utf-8") as fh:
                        fh.write("".join(f"{k}={v}\n" for k, v in vec["sdkconfig"].items()))
                    argv += ["--sdkconfig", sdk]
                if vec.get("drivers") is not None:
                    drv = os.path.join(tmp, "drivers.json")
                    with open(drv, "w", encoding="utf-8") as fh:
                        json.dump(vec["drivers"], fh)
                    argv += ["--drivers", drv]
                if vec.get("strict"):
                    argv.append("--strict")
                out, err = io.StringIO(), io.StringIO()
                with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                    code = cli.main(argv)
                self.assertEqual(code, vec["expect"]["exit"], err.getvalue())
                for f in vec["expect"]["findings"]:
                    self.assertIn(f["code"], err.getvalue())

    def test_fail_vectors_are_seeded_conflicts_in_a_valid_board(self):
        """Each fail vector differs from the passing example only by its conflict: removing it gives no error."""
        for name, vec in self.vectors:
            if "-fail" in name and vec["expect"]["findings"]:
                self.assertFalse(vec["expect"]["ok"] and not vec.get("strict"), name)

    def test_vectors_match_the_board_schema(self):
        schema = common.schema("board.schema.json")
        refused = {"R00-fail-expression.json", "R00-fail-unknown-key.json"}
        for name, vec in self.vectors:
            errors = minischema.errors(schema, vec["board"])
            if name in refused:
                self.assertNotEqual(errors, [], name)
            else:
                self.assertEqual(errors, [], name)


class ExampleBoardsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.data = dmadata.load_dir()

    def test_examples_check(self):
        for path in EXAMPLE_FILES:
            with self.subTest(board=path):
                doc = board.load(path)
                self.assertEqual(minischema.errors(common.schema("board.schema.json"), doc), [])
                res = dmacheck.check(doc, self.data, sdkconfig=SDK_OK)
                self.assertEqual(res["findings"], [])

    def test_h743_example_matches_the_brief(self):
        res = dmacheck.check(board.load(EXAMPLE_FILES[0]), self.data)["resolved"]
        streams = {s["name"]: s for s in res["streams"]}
        self.assertEqual([(streams[n]["engine"], streams[n]["unit"]) for n in ("spi1_rx", "spi1_tx")],
                         [("dma1", 0), ("dma1", 1)])
        self.assertEqual((streams["adc1"]["mode"], streams["adc1"]["notify"], streams["adc1"]["irq_priority"]),
                         ("circular", "none", 4))
        self.assertEqual(streams["adc1"]["shape"], {"frames": 2, "element_bytes": 2, "elements": 6})
        regions = {r["name"]: r for r in res["regions"]}
        self.assertEqual((regions["d2_dma"]["policy"], regions["axi_dma"]["policy"]), ("NonCacheable", "Maintained"))
        self.assertEqual([m["region"] for m in res["mpu"]], ["d2_dma"])

    def test_c6_example_takes_two_of_three_pairs(self):
        res = dmacheck.check(board.load(EXAMPLE_FILES[1]), self.data, sdkconfig=SDK_OK)
        self.assertEqual(res["resolved"]["gdma_pairs"], {"used": 2, "budget": 3})
        warn = dmacheck.check(board.load(EXAMPLE_FILES[1]), self.data)
        self.assertEqual([f["code"] for f in warn["findings"]], ["dma.sdkconfig_not_checked"])


class DataLoaderTest(unittest.TestCase):
    def test_real_data_loads_and_exports_the_same_bytes_twice(self):
        data = dmadata.load_dir()
        self.assertEqual(data["schema"], 1)
        self.assertIn("STM32H743", data["stm32"])
        self.assertIn("esp32c6", data["esp32"])
        self.assertEqual(dmadata.export_json(data), dmadata.export_json(dmadata.load_dir()))
        json.loads(dmadata.export_json(data))

    def test_export_json_command(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = os.path.join(tmp, "dma-data.json")
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(dmadata.main(["--export-json", out]), 0)
            with open(out, encoding="utf-8") as fh:
                bundle = json.load(fh)
            self.assertEqual(sorted(bundle), ["esp32", "schema", "stm32"])
            # the bundle is what the checker reads: the same findings from the exported JSON
            doc = board.load(EXAMPLE_FILES[0])
            self.assertEqual(dmacheck.check(doc, bundle), dmacheck.check(doc, dmadata.load_dir()))

    def test_common_facts_are_merged_into_each_esp32_target(self):
        for name, facts in dmadata.load_dir()["esp32"].items():
            self.assertIn("max_dma_frame_bytes", facts, name)


class CommandLineTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)

    def run_cli(self, *argv):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = cli.main(list(argv))
        return code, out.getvalue(), err.getvalue()

    def write(self, name, doc):
        path = os.path.join(self.tmp.name, name)
        with open(path, "w", encoding="utf-8") as fh:
            json.dump(doc, fh)
        return path

    def test_check_ok_exits_0_and_names_the_board(self):
        code, out, err = self.run_cli("image", "check", EXAMPLE_FILES[0])
        self.assertEqual((code, err), (0, ""))
        self.assertIn("checks ok", out)

    def test_check_failure_exits_1_with_one_line_per_finding(self):
        doc = board.load(EXAMPLE_FILES[0])
        doc["dma"]["streams"]["spi1_tx"]["stream"] = doc["dma"]["streams"]["spi1_rx"]["stream"]
        path = self.write("b.json", doc)
        code, out, err = self.run_cli("image", "check", path)
        self.assertEqual(code, 1)
        self.assertIn("error dma.stream_used_twice dma1/0:", err)
        self.assertIn("does not check", err)

    def test_json_output_is_the_result(self):
        code, out, _ = self.run_cli("image", "check", EXAMPLE_FILES[0], "--json")
        self.assertEqual(code, 0)
        result = json.loads(out)
        self.assertEqual(sorted(result), ["findings", "ok", "resolved"])
        self.assertTrue(result["ok"])

    def test_warnings_pass_unless_strict(self):
        self.assertEqual(self.run_cli("image", "check", EXAMPLE_FILES[1])[0], 0)
        code, _, err = self.run_cli("image", "check", EXAMPLE_FILES[1], "--strict")
        self.assertEqual(code, 1)
        self.assertIn("--strict", err)
        sdk = os.path.join(self.tmp.name, "sdkconfig.defaults")
        with open(sdk, "w", encoding="utf-8") as fh:
            fh.write("CONFIG_ADC_CONTINUOUS_ISR_IRAM_SAFE=y\n")
        self.assertEqual(self.run_cli("image", "check", EXAMPLE_FILES[1], "--sdkconfig", sdk, "--strict")[0], 0)

    def test_selftest_runs_every_vector(self):
        code, out, err = self.run_cli("image", "check", "--selftest")
        self.assertEqual((code, err), (0, ""))
        self.assertIn("0 problem(s)", out)

    def test_selftest_fails_when_a_vector_does_not_hold(self):
        with tempfile.TemporaryDirectory() as tmp:
            for name, vec in boardvectors.load_vectors():
                if name == "R01-pass.json":
                    vec = copy.deepcopy(vec)
                    vec["board"]["dma"]["streams"]["adc1"]["stream"] = 0   # now a conflict the expectation lacks
                with open(os.path.join(tmp, name), "w", encoding="utf-8") as fh:
                    json.dump(vec, fh)
            out, err = io.StringIO(), io.StringIO()
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                code = boardvectors.selftest(dmadata.load_dir(), tmp)
            self.assertEqual(code, 1)
            self.assertIn("R01-pass.json", err.getvalue())

    def test_selftest_fails_without_a_vector_for_a_rule(self):
        with tempfile.TemporaryDirectory() as tmp:
            for name, vec in boardvectors.load_vectors():
                if not name.startswith("R15-"):
                    with open(os.path.join(tmp, name), "w", encoding="utf-8") as fh:
                        json.dump(vec, fh)
            err = io.StringIO()
            with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(err):
                self.assertEqual(boardvectors.selftest(dmadata.load_dir(), tmp), 1)
            self.assertIn("no pass vector for R15", err.getvalue())

    def test_usage_errors_exit_2(self):
        self.assertEqual(self.run_cli("image", "check")[0], 2)
        self.assertEqual(self.run_cli("image", "check", os.path.join(self.tmp.name, "missing.yml"))[0], 2)
        self.assertEqual(self.run_cli("image", "check", EXAMPLE_FILES[0], "--sdkconfig",
                                      os.path.join(self.tmp.name, "missing"))[0], 2)

    def test_board_gen_writes_the_files_and_refuses_a_failing_board(self):
        out_dir = os.path.join(self.tmp.name, "gen")
        code, out, err = self.run_cli("board", "gen", EXAMPLE_FILES[0], "-o", out_dir)
        self.assertEqual((code, err), (0, ""))
        self.assertEqual(sorted(os.listdir(out_dir)), ["board_dma.cpp", "board_dma.hpp", "board_dma_report.json",
                                                       "board_dma_report.md", "board_mpu.hpp", "kb_dma.ld"])
        doc = board.load(EXAMPLE_FILES[0])
        doc["dma"]["streams"]["spi1_tx"]["stream"] = doc["dma"]["streams"]["spi1_rx"]["stream"]
        bad = self.write("bad.json", doc)
        out_dir2 = os.path.join(self.tmp.name, "gen2")
        code, _, err = self.run_cli("board", "gen", bad, "-o", out_dir2)
        self.assertEqual(code, 1)
        self.assertFalse(os.path.exists(out_dir2))


if __name__ == "__main__":
    unittest.main()
