# bmp581 through its binding, over the driver's own chip model (components/drivers/driver-bmp581/tests).
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
kb_add_binding_test(bmp581 SOURCES test_bmp581.cpp MODEL_DIRS "${KB_DRIVER_BMP581_DIR}/tests")
