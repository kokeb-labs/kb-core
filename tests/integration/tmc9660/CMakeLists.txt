# tmc9660 through its binding, over the driver's own chip model (tests/mock_tmc9660_bus.hpp).
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
kb_add_binding_test(tmc9660 SOURCES test_tmc9660.cpp MODEL_DIRS "${KB_DRIVER_TMC9660_DIR}/tests")
