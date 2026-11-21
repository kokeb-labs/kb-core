# canopen_client through its binding, over the driver's own model (tests/mock_canopen_client.hpp).
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
kb_add_binding_test(canopen_client SOURCES test_canopen_client.cpp MODEL_DIRS "${KB_DRIVER_CANOPEN_CLIENT_DIR}/tests")
