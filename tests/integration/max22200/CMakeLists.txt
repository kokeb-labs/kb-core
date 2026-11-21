# max22200 through its binding, over the driver's own chip model (components/drivers/driver-max22200/tests).
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
kb_add_binding_test(max22200 SOURCES test_max22200.cpp MODEL_DIRS "${KB_DRIVER_MAX22200_DIR}/tests")
