# ws2812_rmt through its binding, over the driver's own RMT channel model (tests/mock_ws2812_rmt.hpp).
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
kb_add_binding_test(ws2812_rmt SOURCES test_ws2812_rmt.cpp MODEL_DIRS "${KB_DRIVER_WS2812_RMT_DIR}/tests")
