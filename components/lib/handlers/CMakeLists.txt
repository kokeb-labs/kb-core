# kb::handlers: the handler core, the bus domains and access facades, the hook bases (inc/kb/handlers/common/), and
# one binding with its handler per driver (inc/kb/handlers/bindings/<chip>.hpp; components/lib/handlers/README.md). Only the
# HAL links kb::handlers.
#
# Drivers: each enabled driver (KB_ENABLE_<NAME>) comes from its checkout (components/drivers/driver-<slug> or
# KB_DRIVER_<NAME>_DIR) as <name>::<name> and is linked to kb::handlers. With KB_BUILD_INTEGRATION every driver whose
# checkout is present is also added, so its binding header is compiled on its own (header checks, cross build, MISRA
# scan) and tested (tests/integration). KB_CHECK_BINDINGS does the same without the tests (what the MISRA scan
# builds).
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
kb_add_part(handlers DEPENDS interfaces rtos utils)

# The headers without a selected RTOS backend and without drivers: what the tests, header checks and the cross build
# compile against (each test picks its backend).
add_library(kb_handlers_api INTERFACE)
target_compile_features(kb_handlers_api INTERFACE ${KB_CXX_FLOOR_FEATURE})
target_include_directories(kb_handlers_api INTERFACE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/inc>")
target_link_libraries(kb_handlers_api INTERFACE kb_interfaces_contracts kb_rtos_api)

kb_header_checks(handlers _kb_handlers_checks HEADERS
  kb/handlers/handlers.hpp kb/handlers/common/async_poller.hpp kb/handlers/common/bus_domain.hpp
  kb/handlers/common/driver_result.hpp kb/handlers/common/facade.hpp kb/handlers/common/guarded.hpp
  kb/handlers/common/handler_core.hpp kb/handlers/common/health.hpp kb/handlers/common/health_journal.hpp
  kb/handlers/common/hooks.hpp kb/handlers/common/liveness.hpp kb/handlers/common/owner_access.hpp
  kb/handlers/common/request_channel.hpp kb/handlers/common/seqlock.hpp kb/handlers/common/service_key.hpp
  kb/handlers/common/types.hpp)
set_property(GLOBAL APPEND PROPERTY KB_CROSS_SOURCES ${_kb_handlers_checks})
set_property(GLOBAL APPEND PROPERTY KB_CROSS_LIBS kb_handlers_api)

# kb_handlers_add_driver(<name>): the driver's target from its checkout, once.
function(kb_handlers_add_driver name)
  string(TOUPPER "${name}" upper)
  if(NOT TARGET ${name}::${name})
    add_subdirectory("${KB_DRIVER_${upper}_DIR}" "${CMAKE_BINARY_DIR}/drivers/${name}" EXCLUDE_FROM_ALL SYSTEM)
  endif()
endfunction()

foreach(_name IN LISTS KB_ENABLED_DRIVERS)
  kb_handlers_add_driver(${_name})
  target_link_libraries(kb_handlers INTERFACE ${_name}::${_name})
endforeach()

# The bindings this build compiles: the enabled drivers, and with KB_BUILD_INTEGRATION every checked-out driver.
set(KB_HANDLERS_BOUND "")
foreach(_entry IN LISTS KB_KNOWN_DRIVERS)
  string(REPLACE ":" ";" _parts "${_entry}")
  list(GET _parts 0 _name)
  string(TOUPPER "${_name}" _upper)
  set(_binding "${CMAKE_CURRENT_SOURCE_DIR}/inc/kb/handlers/bindings/${_name}.hpp")
  set(_want OFF)
  if(KB_ENABLE_${_upper})
    set(_want ON)
  elseif((KB_BUILD_INTEGRATION OR KB_CHECK_BINDINGS)
         AND EXISTS "${KB_DRIVER_${_upper}_DIR}/driver.yml")
    set(_want ON)
  endif()
  if(_want AND EXISTS "${_binding}")
    kb_handlers_add_driver(${_name})
    list(APPEND KB_HANDLERS_BOUND ${_name})
  endif()
endforeach()
set(KB_HANDLERS_BOUND "${KB_HANDLERS_BOUND}" CACHE INTERNAL "Drivers whose binding this build compiles")
if(KB_HANDLERS_BOUND)
  set(_kb_binding_headers "")
  foreach(_name IN LISTS KB_HANDLERS_BOUND)
    list(APPEND _kb_binding_headers "kb/handlers/bindings/${_name}.hpp")
    set_property(GLOBAL APPEND PROPERTY KB_CROSS_LIBS ${_name}::${_name})
  endforeach()
  kb_header_checks(handlers _kb_binding_checks HEADERS ${_kb_binding_headers})
  set_property(GLOBAL APPEND PROPERTY KB_CROSS_SOURCES ${_kb_binding_checks})
endif()

if(KB_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
  add_subdirectory(tests)
endif()
if(KB_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fuzz/CMakeLists.txt")
  add_subdirectory(fuzz)
endif()
