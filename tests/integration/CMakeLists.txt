# Integration tests (tests/integration/README.md): each bound driver through its kb::handlers binding and handler,
# over kb::interfaces host ports backed by the driver's own chip model, in both thread models, on the std::thread
# RTOS backend. Built with KB_BUILD_TESTS and KB_BUILD_INTEGRATION; one folder per driver.
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
if(NOT (KB_BUILD_TESTS AND KB_BUILD_INTEGRATION))
  return()
endif()
if(NOT TARGET kb_rtos_backend_host)
  message(STATUS "[kb] integration tests skipped: they need the host RTOS backend (a host build)")
  return()
endif()

# kb_add_binding_test(<driver> SOURCES ... [MODEL_DIRS ...] [LIBS ...]): kb_binding_<driver>, with the driver's own
# test folders (chip models, mocks) as system include directories, so their headers are not held to kb's flags.
function(kb_add_binding_test name)
  cmake_parse_arguments(PARSE_ARGV 1 ARG "" "" "SOURCES;MODEL_DIRS;LIBS")
  set(target kb_binding_${name})
  kb_add_unit_test(${target} SOURCES ${ARG_SOURCES}
    LIBS kb_handlers_api kb_interfaces_host kb_rtos_backend_host ${name}::${name} ${ARG_LIBS})
  # GCC's -O2 flow analysis sees into the drivers' own test models (std::vector code inlined there) and reports
  # -Wnull-dereference on code that is not kb-core's; the binding headers are held to it in the header checks.
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(${target}_o2 PRIVATE -Wno-null-dereference)
  endif()
  foreach(t IN ITEMS ${target} ${target}_o2)
    target_include_directories(${t} PRIVATE "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/common")
    target_include_directories(${t} SYSTEM PRIVATE ${ARG_MODEL_DIRS})
  endforeach()
endfunction()

foreach(_name IN LISTS KB_HANDLERS_BOUND)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_name}/CMakeLists.txt")
    add_subdirectory(${_name})
  endif()
endforeach()
