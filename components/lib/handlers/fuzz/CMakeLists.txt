# Fuzz targets of kb::handlers (components/lib/handlers/fuzz/README.md): the owner-model request queue against a model of its
# slot protocol, on the deterministic none backend.
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
kb_add_fuzz_targets(handlers "${CMAKE_CURRENT_SOURCE_DIR}" TARGETS fuzz_handlers_channel
  LIBS kb_handlers_api kb_rtos_backend_none)
