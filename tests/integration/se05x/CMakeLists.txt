# se05x through its binding, over the driver's own chip model chain (tests/model: T=1 target, SCP target, IoT
# applet model). Built with every checked-out driver.
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
kb_add_binding_test(se05x SOURCES test_se05x.cpp MODEL_DIRS "${KB_DRIVER_SE05X_DIR}/tests")
