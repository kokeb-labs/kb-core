# isf15acp4 through its binding, over the driver's own model (tests/mock_isf15acp4_bus.hpp).
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
kb_add_binding_test(isf15acp4 SOURCES test_isf15acp4.cpp MODEL_DIRS "${KB_DRIVER_ISF15ACP4_DIR}/tests")
