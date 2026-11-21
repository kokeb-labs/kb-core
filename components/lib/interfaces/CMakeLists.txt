# kb::interfaces: the contracts (C++20 concepts and *Ref values, ADR-0006) over kb::rtos and kb::utils, and the
# platform-free host test port in host/ (components/lib/interfaces/README.md). MCU ports are separate CMake projects in
# components/ports/<mcu>/ (ADR-0007) and are never added from here. Only the HAL, kb::handlers and the MCU ports use this target.
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
kb_add_part(interfaces DEPENDS rtos utils)

kb_header_checks(interfaces _kb_interfaces_checks HEADERS
  kb/interfaces/adc.hpp kb/interfaces/async.hpp kb/interfaces/can.hpp kb/interfaces/gpio.hpp kb/interfaces/i2c.hpp
  kb/interfaces/interfaces.hpp kb/interfaces/log.hpp kb/interfaces/pwm.hpp kb/interfaces/refs.hpp kb/interfaces/spi.hpp
  kb/interfaces/time.hpp kb/interfaces/types.hpp kb/interfaces/uart.hpp)
set_property(GLOBAL APPEND PROPERTY KB_CROSS_SOURCES ${_kb_interfaces_checks})
set_property(GLOBAL APPEND PROPERTY KB_CROSS_LIBS kb_interfaces_contracts)

# The contracts alone, without a selected RTOS backend: what the header checks and the cross build compile.
add_library(kb_interfaces_contracts INTERFACE)
target_compile_features(kb_interfaces_contracts INTERFACE ${KB_CXX_FLOOR_FEATURE})
target_include_directories(kb_interfaces_contracts INTERFACE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/inc>")
target_link_libraries(kb_interfaces_contracts INTERFACE kb::utils)

add_subdirectory(host)

if(KB_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
  add_subdirectory(tests)
endif()
