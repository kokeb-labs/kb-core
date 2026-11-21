# as5047u through its binding, over the driver's own chip model (components/drivers/driver-as5047u/tests).
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
kb_add_binding_test(as5047u SOURCES test_as5047u.cpp MODEL_DIRS "${KB_DRIVER_AS5047U_DIR}/tests")
