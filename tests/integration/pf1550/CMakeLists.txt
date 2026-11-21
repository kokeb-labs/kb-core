# pf1550 through its binding, over the driver's own register model (tests/mock_pf1550_bus.hpp).
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
kb_add_binding_test(pf1550 SOURCES test_pf1550.cpp MODEL_DIRS "${KB_DRIVER_PF1550_DIR}/tests")
