# at25040b through its binding, over the driver's own chip model (tests/mock_at25040b_bus.hpp).
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
kb_add_binding_test(at25040b SOURCES test_at25040b.cpp MODEL_DIRS "${KB_DRIVER_AT25040B_DIR}/tests")
