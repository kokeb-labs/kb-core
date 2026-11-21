# alicat_basis2 through its binding, over the driver's own model (tests/mock_basis2.hpp).
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
kb_add_binding_test(alicat_basis2 SOURCES test_alicat_basis2.cpp MODEL_DIRS "${KB_DRIVER_ALICAT_BASIS2_DIR}/tests")
