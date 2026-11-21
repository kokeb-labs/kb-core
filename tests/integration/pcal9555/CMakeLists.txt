# pcal9555 through its binding, over the driver's own protocol model (tests/mock_pcal9555.hpp).
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
kb_add_binding_test(pcal9555 SOURCES test_pcal9555.cpp MODEL_DIRS "${KB_DRIVER_PCAL9555_DIR}/tests")
