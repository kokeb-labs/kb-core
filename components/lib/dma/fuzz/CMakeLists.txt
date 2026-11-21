# Fuzz targets of kb::dma (components/lib/dma/fuzz/README.md): the stream's ring against a model of its delivery rules.
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
kb_add_fuzz_targets(dma "${CMAKE_CURRENT_SOURCE_DIR}" TARGETS fuzz_dma_stream LIBS kb_dma)
