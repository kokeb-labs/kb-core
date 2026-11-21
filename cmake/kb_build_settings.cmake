# kb-core build settings: the one place a consumer (a HAL, an example, CI) selects the MCU family, the RTOS, the
# parts and the drivers. Set the variables before including this file, or pass them with -D.
#
#   KB_MCU               HOST (default off target) | ESP32 (default under ESP-IDF) | STM32 (later); selects the MCU
#                        gate and the default RTOS. No MCU source is compiled here: ports are own projects (ADR-0007)
#   KB_RTOS              NONE (default on HOST) | HOST (std::thread, PC tests) | FREERTOS (default on ESP32) |
#                        THREADX (Eclipse ThreadX, ADR-0015: needs KB_THREADX_INCLUDE_DIRS or KB_THREADX_PORT,
#                        components/lib/rtos/backends/threadx/README.md)
#   KB_PARTS             any of utils;rtos;dma;interfaces;handlers (default: all); dependencies are added
#   KB_ENABLE_<DRIVER>   ON to bind a driver (names in cmake/kb_drivers.cmake); adds the handlers part
#   KB_DRIVER_<DRIVER>_DIR  where that driver's checkout is (default: <drivers folder>/driver-<slug>; the folders
#                        are named in tools/layout/paths.yml)
#
# Results: KB_MCU, KB_RTOS, KB_PARTS (resolved, in dependency order), KB_ENABLED_DRIVERS, KB_IDF_REQUIRES.
# No platform macro is produced on purpose: code never tests the MCU (ADR-0001); the build picks the folders.
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
include_guard(GLOBAL)

get_filename_component(KB_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
include("${CMAKE_CURRENT_LIST_DIR}/kb_layout.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/kb_version.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/kb_drivers.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/kb_mcu_gates.cmake")

# -- MCU family ------------------------------------------------------------------------------------------------------
set(_KB_SUPPORTED_MCUS HOST ESP32)
set(_KB_PLANNED_MCUS STM32)
if(NOT DEFINED KB_MCU OR KB_MCU STREQUAL "")
  if(DEFINED ESP_PLATFORM OR DEFINED IDF_TARGET)
    set(KB_MCU ESP32)
  else()
    set(KB_MCU HOST)
  endif()
endif()
string(TOUPPER "${KB_MCU}" KB_MCU)
if(KB_MCU IN_LIST _KB_PLANNED_MCUS)
  message(FATAL_ERROR "[kb] KB_MCU=${KB_MCU} is planned but has no port yet. Supported: ${_KB_SUPPORTED_MCUS}")
elseif(NOT KB_MCU IN_LIST _KB_SUPPORTED_MCUS)
  message(FATAL_ERROR "[kb] Unknown KB_MCU '${KB_MCU}'. Supported: ${_KB_SUPPORTED_MCUS}")
endif()

# -- RTOS ------------------------------------------------------------------------------------------------------------
set(_KB_SUPPORTED_RTOS NONE HOST FREERTOS THREADX)
set(_KB_PLANNED_RTOS) # none planned now; a future backend is listed here first
if(NOT DEFINED KB_RTOS OR KB_RTOS STREQUAL "")
  if(KB_MCU STREQUAL "ESP32")
    set(KB_RTOS FREERTOS)
  else()
    set(KB_RTOS NONE)
  endif()
endif()
string(TOUPPER "${KB_RTOS}" KB_RTOS)
if(_KB_PLANNED_RTOS AND KB_RTOS IN_LIST _KB_PLANNED_RTOS)
  message(FATAL_ERROR "[kb] KB_RTOS=${KB_RTOS}: not yet; the backend is planned. Supported: ${_KB_SUPPORTED_RTOS}")
elseif(NOT KB_RTOS IN_LIST _KB_SUPPORTED_RTOS)
  message(FATAL_ERROR "[kb] Unknown KB_RTOS '${KB_RTOS}'. Supported: ${_KB_SUPPORTED_RTOS}")
endif()
if(KB_RTOS STREQUAL "HOST" AND NOT KB_MCU STREQUAL "HOST")
  message(FATAL_ERROR "[kb] KB_RTOS=HOST (std::thread) is a PC test backend; it needs KB_MCU=HOST")
endif()
if(KB_MCU STREQUAL "ESP32" AND NOT KB_RTOS STREQUAL "FREERTOS")
  message(FATAL_ERROR "[kb] KB_MCU=ESP32 runs ESP-IDF, which requires KB_RTOS=FREERTOS (got ${KB_RTOS})")
endif()

# -- Drivers ---------------------------------------------------------------------------------------------------------
set(KB_ENABLED_DRIVERS "")
set(_KB_DRIVER_NAMES "")
foreach(_entry IN LISTS KB_KNOWN_DRIVERS)
  string(REPLACE ":" ";" _parts "${_entry}")
  list(GET _parts 0 _name)
  list(GET _parts 1 _folder)
  string(TOUPPER "${_name}" _upper)
  list(APPEND _KB_DRIVER_NAMES "${_upper}")
  option(KB_ENABLE_${_upper} "Bind the ${_name} driver (${_folder})" OFF)
  if(NOT DEFINED KB_DRIVER_${_upper}_DIR)
    set(KB_DRIVER_${_upper}_DIR "${KB_DRIVERS_DIR}/${_folder}")
  endif()
  set(_KB_FOLDER_${_upper} "${_folder}")
  if(KB_ENABLE_${_upper})
    list(APPEND KB_ENABLED_DRIVERS "${_name}")
  endif()
endforeach()
# A KB_ENABLE_<X> that names no known driver is a typo, not a silent no-op.
get_cmake_property(_kb_vars VARIABLES)
foreach(_var IN LISTS _kb_vars)
  if(_var MATCHES "^KB_ENABLE_(.+)$")
    if(NOT CMAKE_MATCH_1 IN_LIST _KB_DRIVER_NAMES AND NOT CMAKE_MATCH_1 MATCHES "^(WARNINGS)$")
      message(FATAL_ERROR "[kb] ${_var} names no known driver (cmake/kb_drivers.cmake)")
    endif()
  endif()
endforeach()

# -- MCU gate --------------------------------------------------------------------------------------------------------
foreach(_gate IN LISTS KB_MCU_GATES)
  string(REPLACE ":" ";" _parts "${_gate}")
  list(GET _parts 0 _name)
  list(GET _parts 1 _mcu)
  list(GET _parts 2 _peripheral)
  if(_name IN_LIST KB_ENABLED_DRIVERS AND NOT KB_MCU STREQUAL _mcu)
    string(TOUPPER "${_name}" _upper)
    message(FATAL_ERROR
      "[kb] Driver '${_name}' needs the ${_mcu} ${_peripheral} peripheral, but KB_MCU=${KB_MCU}. "
      "Turn KB_ENABLE_${_upper} off or select KB_MCU=${_mcu}.")
  endif()
endforeach()

# -- Driver checkouts (after the gate, so a gated driver gets the gate message first) -----------------------------------
foreach(_name IN LISTS KB_ENABLED_DRIVERS)
  string(TOUPPER "${_name}" _upper)
  if(NOT EXISTS "${KB_DRIVER_${_upper}_DIR}/driver.yml")
    message(FATAL_ERROR
      "[kb] KB_ENABLE_${_upper}=ON but ${KB_DRIVER_${_upper}_DIR} is not a driver checkout. Run "
      "'git submodule update --init ${KB_DRIVERS_REL}/${_KB_FOLDER_${_upper}}' or set KB_DRIVER_${_upper}_DIR.")
  endif()
endforeach()

# -- Parts (dependency closure, in dependency order) -----------------------------------------------------------------
set(_KB_ALL_PARTS utils rtos dma interfaces handlers)
set(_KB_DEPS_utils "")
set(_KB_DEPS_rtos utils)
set(_KB_DEPS_dma utils)
set(_KB_DEPS_interfaces rtos utils)
set(_KB_DEPS_handlers interfaces rtos utils)
if(NOT DEFINED KB_PARTS OR KB_PARTS STREQUAL "")
  set(KB_PARTS ${_KB_ALL_PARTS})
endif()
if(KB_ENABLED_DRIVERS AND NOT "handlers" IN_LIST KB_PARTS)
  list(APPEND KB_PARTS handlers)
endif()
set(_requested ${KB_PARTS})
set(KB_PARTS "")
foreach(_part IN LISTS _KB_ALL_PARTS)
  set(_want OFF)
  foreach(_req IN LISTS _requested)
    if(NOT _req IN_LIST _KB_ALL_PARTS)
      message(FATAL_ERROR "[kb] Unknown part '${_req}' in KB_PARTS. Parts: ${_KB_ALL_PARTS}")
    endif()
    if(_req STREQUAL _part OR _part IN_LIST _KB_DEPS_${_req})
      set(_want ON)
    endif()
  endforeach()
  if(_want)
    list(APPEND KB_PARTS "${_part}")
  endif()
endforeach()

# -- ESP-IDF component requirements of the kb component (consumed by the ESP-IDF component registration, later) ------
# Only the FreeRTOS backend touches ESP-IDF here; the peripheral components belong to the MCU port, which is its own
# component in the esp32 folder of the ports (ADR-0007).
set(KB_IDF_REQUIRES "")
if(KB_MCU STREQUAL "ESP32")
  if("rtos" IN_LIST KB_PARTS)
    list(APPEND KB_IDF_REQUIRES freertos)
  endif()
endif()

# -- Summary ---------------------------------------------------------------------------------------------------------
if(NOT KB_QUIET)
  list(LENGTH KB_ENABLED_DRIVERS _n)
  string(REPLACE ";" ", " _parts_text "${KB_PARTS}")
  if(_n EQUAL 0)
    set(_drivers_text "none")
  else()
    string(REPLACE ";" ", " _drivers_text "${KB_ENABLED_DRIVERS}")
  endif()
  message(STATUS "[kb] kb-core ${KB_VERSION}")
  message(STATUS "[kb]   MCU:     ${KB_MCU}")
  message(STATUS "[kb]   RTOS:    ${KB_RTOS}")
  message(STATUS "[kb]   Parts:   ${_parts_text}")
  message(STATUS "[kb]   Drivers: ${_drivers_text}")
  if(KB_IDF_REQUIRES)
    string(REPLACE ";" ", " _idf_text "${KB_IDF_REQUIRES}")
    message(STATUS "[kb]   ESP-IDF: ${_idf_text}")
  endif()
endif()
