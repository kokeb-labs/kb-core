# kb::dma: the chip-neutral DMA vocabulary (regions, buffers, leases, cache hooks, streams) over kb::utils, and the
# host DMA-and-cache model with the conformance suite in host/ (components/lib/dma/README.md). Header-only. MCU ports are
# separate CMake projects in components/ports/<mcu>/ (ADR-0007) and are never added from here.
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
kb_add_part(dma DEPENDS utils)

kb_header_checks(dma _kb_dma_checks HEADERS
  kb/dma/buffer.hpp kb/dma/cache.hpp kb/dma/capabilities.hpp kb/dma/completion.hpp kb/dma/dma.hpp
  kb/dma/region.hpp kb/dma/stream.hpp)
set_property(GLOBAL APPEND PROPERTY KB_CROSS_SOURCES ${_kb_dma_checks})
set_property(GLOBAL APPEND PROPERTY KB_CROSS_LIBS kb::dma)

add_subdirectory(host)

if(KB_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
  add_subdirectory(tests)
endif()
if(KB_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fuzz/CMakeLists.txt")
  add_subdirectory(fuzz)
endif()
