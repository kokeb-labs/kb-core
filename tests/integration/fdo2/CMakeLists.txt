# fdo2 through its binding, over the driver's own module model (tests/mock_fdo2.hpp).
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
kb_add_binding_test(fdo2 SOURCES test_fdo2.cpp MODEL_DIRS "${KB_DRIVER_FDO2_DIR}/tests")
