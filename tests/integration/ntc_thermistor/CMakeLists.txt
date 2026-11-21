# ntc_thermistor through its binding, over the driver's own chip model (tests/mock_ntc_thermistor.hpp).
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
kb_add_binding_test(ntc_thermistor SOURCES test_ntc_thermistor.cpp MODEL_DIRS "${KB_DRIVER_NTC_THERMISTOR_DIR}/tests")
