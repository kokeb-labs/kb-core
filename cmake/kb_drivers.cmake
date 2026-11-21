# The drivers kb-core knows: the driver.yml name (also the KB_ENABLE_<NAME> switch, upper-cased) and the repository
# folder under components/drivers/. A driver is used only when its switch is ON; its checkout is components/drivers/driver-<slug> (a
# submodule, ADR-0002) unless KB_DRIVER_<NAME>_DIR points elsewhere.
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
include_guard(GLOBAL)

# name:repository-folder, simplest first (the order bindings are written in).
set(KB_KNOWN_DRIVERS
    "mcp9700:driver-mcp9700"
    "ntc_thermistor:driver-ntc-thermistor"
    "at25040b:driver-at25040b"
    "pca9685:driver-pca9685"
    "pcal9555:driver-pcal9555"
    "ads7952:driver-ads7952"
    "as5047u:driver-as5047u"
    "bmp581:driver-bmp581"
    "ads9324:driver-ads9324"
    "max22200:driver-max22200"
    "tle92466ed:driver-tle92466ed"
    "sfm_flow_meter:driver-sfm-flow-meter"
    "pf1550:driver-pf1550"
    "ws2812_rmt:driver-ws2812-rmt"
    "fdo2:driver-fdo2"
    "alicat_basis2:driver-alicat-basis2"
    "dynamixel:driver-dynamixel"
    "canopen_client:driver-canopen-client"
    "isf15acp4:driver-isf15acp4"
    "tmc5160:driver-tmc5160"
    "bno08x:driver-bno08x"
    "se05x:driver-se05x"
    "tmc9660:driver-tmc9660"
)
