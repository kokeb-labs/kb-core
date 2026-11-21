# kb::utils: depends on nothing (components/lib/utils/README.md). Header-only.
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
kb_add_part(utils)

kb_header_checks(utils _kb_utils_checks HEADERS
  kb/utils/crc.hpp kb/utils/endian.hpp kb/utils/fault_journal.hpp kb/utils/fixed_string.hpp kb/utils/numeric.hpp kb/utils/result.hpp
  kb/utils/ring_buffer.hpp kb/utils/span.hpp kb/utils/static_vector.hpp kb/utils/status.hpp kb/utils/time.hpp
  kb/utils/utils.hpp)
set_property(GLOBAL APPEND PROPERTY KB_CROSS_SOURCES ${_kb_utils_checks})
set_property(GLOBAL APPEND PROPERTY KB_CROSS_LIBS kb::utils)

if(KB_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
  add_subdirectory(tests)
endif()
if(KB_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fuzz/CMakeLists.txt")
  add_subdirectory(fuzz)
endif()
