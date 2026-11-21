# ads7952 through its binding, over the driver's own chip model (tests/mock_ads7952_bus.hpp).
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
kb_add_binding_test(ads7952 SOURCES test_ads7952.cpp MODEL_DIRS "${KB_DRIVER_ADS7952_DIR}/tests")
