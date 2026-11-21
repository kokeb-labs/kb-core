# ads9324 through its binding, over the driver's own chip model (components/drivers/driver-ads9324/tests).
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
kb_add_binding_test(ads9324 SOURCES test_ads9324.cpp MODEL_DIRS "${KB_DRIVER_ADS9324_DIR}/tests")
