# tmc5160 through its binding, over the driver's own chip model (tests/mock_tmc5160_bus.hpp).
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
kb_add_binding_test(tmc5160 SOURCES test_tmc5160.cpp MODEL_DIRS "${KB_DRIVER_TMC5160_DIR}/tests")
