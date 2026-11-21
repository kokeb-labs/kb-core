# Host tests of kb::interfaces (components/lib/interfaces/tests/README.md): conformance of the fakes and *Ref values,
# compile-fail tests for misuse, and the proof that the contracts serve three drivers' own port concepts through
# thin adapters (built when the pinned driver submodules are checked out: git submodule update --init components/drivers/...).
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
kb_add_unit_test(kb_interfaces_tests SOURCES test_async.cpp test_contracts.cpp test_ref_copies.cpp LIBS kb_interfaces_host)
kb_add_compile_fail_tests(interfaces "${CMAKE_CURRENT_SOURCE_DIR}/compile_fail"
  SOURCES clock_32_bit.cpp hook_not_noexcept.cpp ref_from_nonconforming.cpp uart_read_without_timeout.cpp
          wrong_result_type.cpp
  LIBS kb_interfaces_contracts)
add_library(kb_interfaces_header_checks OBJECT ${_kb_interfaces_checks})
kb_strict_target(kb_interfaces_header_checks)
target_link_libraries(kb_interfaces_header_checks PRIVATE kb_interfaces_contracts)

set(_kb_port_drivers tmc5160 bno08x pcal9555)
set(_kb_port_drivers_found ON)
foreach(_d IN LISTS _kb_port_drivers)
  string(TOUPPER "${_d}" _u)
  if(NOT EXISTS "${KB_DRIVER_${_u}_DIR}/CMakeLists.txt")
    set(_kb_port_drivers_found OFF)
  endif()
endforeach()
if(_kb_port_drivers_found)
  foreach(_d IN LISTS _kb_port_drivers)
    string(TOUPPER "${_d}" _u)
    if(NOT TARGET ${_d}::${_d})
      add_subdirectory("${KB_DRIVER_${_u}_DIR}" "${CMAKE_BINARY_DIR}/drivers/${_d}" EXCLUDE_FROM_ALL SYSTEM)
    endif()
  endforeach()
  kb_add_unit_test(kb_interfaces_driver_ports SOURCES driver_ports/test_driver_ports.cpp
    LIBS kb_interfaces_host tmc5160::tmc5160 bno08x::bno08x pcal9555::pcal9555)
  target_include_directories(kb_interfaces_driver_ports PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/driver_ports")
  target_include_directories(kb_interfaces_driver_ports_o2 PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/driver_ports")
else()
  message(STATUS "[kb] driver port proofs skipped: the tmc5160, bno08x and pcal9555 submodules are not checked out")
endif()
