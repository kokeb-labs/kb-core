# dynamixel through its binding, over the driver's own model (tests/mock_servo_bus.hpp).
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
kb_add_binding_test(dynamixel SOURCES test_dynamixel.cpp MODEL_DIRS "${KB_DRIVER_DYNAMIXEL_DIR}/tests")
