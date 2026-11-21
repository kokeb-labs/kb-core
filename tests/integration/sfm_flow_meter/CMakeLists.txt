# sfm_flow_meter through its binding, over the driver's own chip model (tests/model_sfm.hpp).
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
kb_add_binding_test(sfm_flow_meter SOURCES test_sfm_flow_meter.cpp MODEL_DIRS "${KB_DRIVER_SFM_FLOW_METER_DIR}/tests")
