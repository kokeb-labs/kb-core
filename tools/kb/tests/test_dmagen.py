# The generator of `kb board gen`: board_dma.hpp/.cpp, kb_dma.ld, board_mpu.hpp and the reports.
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
"""Tests of kb.dmagen."""

import json
import unittest

from kb import dmacheck, dmagen

from . import dmafixtures

SDK_OK = {"CONFIG_ADC_CONTINUOUS_ISR_IRAM_SAFE": "y"}


def gen(doc, **kw):
    return dmagen.generate(doc, dmacheck.check(doc, dmafixtures.data(), **kw))


def h7_files():
    return gen(dmafixtures.h7_board())


def c6_files():
    return gen(dmafixtures.c6_board(), sdkconfig=SDK_OK)


class GenerateTest(unittest.TestCase):
    def test_stm32_file_set(self):
        self.assertEqual(sorted(h7_files()), ["board_dma.cpp", "board_dma.hpp", "board_dma_report.json",
                                              "board_dma_report.md", "board_mpu.hpp", "kb_dma.ld"])

    def test_esp32_has_no_linker_script_or_mpu(self):
        self.assertEqual(sorted(c6_files()), ["board_dma.cpp", "board_dma.hpp", "board_dma_report.json",
                                              "board_dma_report.md"])

    def test_no_mpu_file_without_a_data_cache(self):
        def no_cache(doc):
            doc["memory"]["regions"]["d2_dma"].pop("mpu_region")
            doc["memory"]["regions"]["d2_dma"]["cache"] = "coherent"
        doc = dmafixtures.h7_board()
        no_cache(doc)
        data = dmafixtures.data()
        data["stm32"]["STM32TEST7"]["dcache_line"] = 0
        files = dmagen.generate(doc, dmacheck.check(doc, data))
        self.assertNotIn("board_mpu.hpp", files)

    def test_byte_stable(self):
        self.assertEqual(h7_files(), h7_files())
        self.assertEqual(c6_files(), c6_files())

    def test_files_end_with_one_newline_and_use_lf(self):
        for files in (h7_files(), c6_files()):
            for name, text in files.items():
                self.assertTrue(text.endswith("\n") and not text.endswith("\n\n"), name)
                self.assertNotIn("\r", text, name)

    def test_header_carries_masters_regions_buffers_streams_and_checks(self):
        hpp = h7_files()["board_dma.hpp"]
        for needle in ("namespace board::dma {", "kDma1 = kb::dma::MasterBit(0U)", "kMdma = kb::dma::MasterBit(3U)",
                       "struct D2Dma {", "CachePolicy::NonCacheable", "KB_DMA_SECTION(\".kb_dma.d2_dma\")",
                       "kChecksPass", "RequireReachable", "RequireWholeUnits", "RequireInterruptTier",
                       "RequireBusCarries", "static_assert(kb::dma::Region<AxiDma>)"):
            self.assertIn(needle, hpp)
        self.assertIn("Rtos", hpp)

    def test_section_attribute_precedes_extern(self):
        hpp = h7_files()["board_dma.hpp"]
        line = next(ln for ln in hpp.splitlines() if "spi1_stage_rx" in ln and "extern" in ln)
        self.assertLess(line.index("KB_DMA_SECTION"), line.index("extern"))

    def test_no_rtos_without_a_board_rtos(self):
        doc = dmafixtures.h7_board()
        doc.pop("rtos")
        for s in doc["dma"]["streams"].values():
            s["notify"] = "none"
        self.assertNotIn("Rtos", gen(doc)["board_dma.hpp"])

    def test_esp32_section_macro_is_empty(self):
        hpp = c6_files()["board_dma.hpp"]
        self.assertIn("#define KB_DMA_SECTION(name)", hpp)
        self.assertNotIn("[[gnu::section", hpp)

    def test_cpp_defines_the_buffers_and_noload_ranges(self):
        files = h7_files()
        self.assertIn("kb::dma::Buffer<region::D2Dma, 1028U> spi1_stage_rx;", files["board_dma.cpp"])
        hpp = files["board_dma.hpp"]
        self.assertIn("extern std::byte __board_dma_d2_dma_start[];", hpp)
        self.assertIn("NoloadRanges", hpp)

    def test_linker_script_places_each_used_region_noload_with_asserts(self):
        ld = h7_files()["kb_dma.ld"]
        self.assertIn(".kb_dma.d2_dma 0x30000000 (NOLOAD) : ALIGN(32)", ld)
        self.assertIn("KEEP(*(.kb_dma.d2_dma))", ld)
        self.assertIn("does not fit its region d2_dma", ld)
        self.assertIn("is not inside its MPU region 0", ld)
        self.assertIn("overlaps .bss", ld)
        self.assertNotIn(".kb_dma.dtcm", ld)  # no buffer lives there

    def test_mpu_table_lists_the_non_cacheable_regions(self):
        mpu = h7_files()["board_mpu.hpp"]
        self.assertIn("namespace board::mpu", mpu)
        self.assertIn("0x30000000U", mpu)
        self.assertNotIn("0x24060000U", mpu)

    def test_report_json_is_canonical_and_complete(self):
        text = h7_files()["board_dma_report.json"]
        report = json.loads(text)
        self.assertEqual(report["board"], "test-board-h7")
        self.assertEqual([b["name"] for b in report["buffers"]], ["adc1_scan", "spi1_stage_rx", "spi1_stage_tx"])
        self.assertEqual(text, json.dumps(report, indent=2, sort_keys=True, ensure_ascii=False) + "\n")

    def test_report_markdown_names_the_board_and_the_budget(self):
        self.assertIn("test-board-h7", h7_files()["board_dma_report.md"])
        self.assertIn("2 of 3", c6_files()["board_dma_report.md"])

    def test_header_comment_says_generated(self):
        for files in (h7_files(), c6_files()):
            self.assertIn("generated by kb board gen", files["board_dma.hpp"].splitlines()[0])

    def test_refuses_a_board_with_errors(self):
        doc = dmafixtures.h7_board()
        doc["dma"]["streams"]["spi1_tx"]["stream"] = 0
        with self.assertRaises(dmagen.GenerateError):
            gen(doc)

    def test_refuses_a_malformed_board(self):
        with self.assertRaises(dmagen.GenerateError):
            dmagen.generate({"board": "x"}, {"ok": True, "findings": [], "resolved": {}})

    def test_esp32_limits_and_bus_modes(self):
        hpp = c6_files()["board_dma.hpp"]
        self.assertIn("kb::dma::BusLimits kBus{\n      true, 1028U, 64U, 1U, 4U};", hpp)

    def test_stm32_it_limit_is_unbounded(self):
        self.assertIn("std::numeric_limits<std::size_t>::max()", h7_files()["board_dma.hpp"])


if __name__ == "__main__":
    unittest.main()
