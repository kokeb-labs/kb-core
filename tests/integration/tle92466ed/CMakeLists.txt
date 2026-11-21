# tle92466ed through its binding, over the driver's own chip model (components/drivers/driver-tle92466ed/tests).
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
kb_add_binding_test(tle92466ed SOURCES test_tle92466ed.cpp MODEL_DIRS "${KB_DRIVER_TLE92466ED_DIR}/tests")
