# bno08x through its binding, over the driver's own hub model (tests/model/bno08x_hub_model.hpp); owner model only.
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
kb_add_binding_test(bno08x SOURCES test_bno08x.cpp
  MODEL_DIRS "${KB_DRIVER_BNO08X_DIR}/tests" "${KB_DRIVER_BNO08X_DIR}/tests/model")
kb_add_compile_fail_tests(integration_bno08x "${CMAKE_CURRENT_SOURCE_DIR}/compile_fail" SOURCES bno08x_guarded.cpp
  LIBS kb_handlers_api kb_interfaces_host kb_rtos_backend_host bno08x::bno08x)
