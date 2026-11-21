# libFuzzer targets of kb::utils and their corpus replays (components/lib/utils/fuzz/README.md, cmake/kb_testing.cmake).
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
kb_add_fuzz_targets(utils "${CMAKE_CURRENT_SOURCE_DIR}" TARGETS fuzz_utils_bytes fuzz_utils_crc fuzz_utils_journal
  fuzz_utils_ring
  LIBS kb::utils)
