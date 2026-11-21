# mcp9700 through its binding, over the driver's own chip model (tests/mock_mcp9700.hpp).
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
kb_add_binding_test(mcp9700 SOURCES test_mcp9700.cpp MODEL_DIRS "${KB_DRIVER_MCP9700_DIR}/tests")
