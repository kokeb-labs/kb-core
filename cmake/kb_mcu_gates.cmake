# The MCU gate table: drivers that work only on some MCU families because they need one MCU peripheral. Each entry
# is "<driver name>:<MCU>:<peripheral>". Enabling a gated driver for another MCU stops the configure with a message.
# The entry must agree with the driver's driver.yml (mcus, peripheral); pin hygiene checks that in CI (PIN-007).
# A driver with no entry works on every MCU (driver.yml mcus: [any]).
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
include_guard(GLOBAL)

set(KB_MCU_GATES
    "ws2812_rmt:ESP32:rmt"
)
