# kb::dma_host: the host DMA-and-cache model (memory, cache lines, a DMA engine, a violation log) and a reference
# port over it, header-only test code outside the MISRA claim.
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
add_library(kb_dma_host INTERFACE)
add_library(kb::dma_host ALIAS kb_dma_host)
target_compile_features(kb_dma_host INTERFACE cxx_std_20)
target_include_directories(kb_dma_host INTERFACE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/inc>")
target_link_libraries(kb_dma_host INTERFACE kb::dma)
