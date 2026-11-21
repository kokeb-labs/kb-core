# pca9685 through its binding, over the driver's own chip model (tests/mock_pca9685_bus.hpp).
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
kb_add_binding_test(pca9685 SOURCES test_pca9685.cpp MODEL_DIRS "${KB_DRIVER_PCA9685_DIR}/tests")
