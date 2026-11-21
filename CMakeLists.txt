# kb-core: utils, rtos, dma, interfaces and handlers, one version (cmake/kb_version.cmake). Host build:
#   cmake -S . -B build                       KB_MCU=HOST, KB_RTOS=NONE, every part, no driver
#   cmake -S . -B build -DKB_PARTS=rtos       one part and its dependencies (utils)
#   cmake -S . -B build -DKB_ENABLE_PF1550=ON a driver binding (needs the pf1550 driver checkout, see
#                                                KB_DRIVER_<NAME>_DIR; its default is under the drivers folder of paths.yml)
# Under ESP-IDF this file is the kb component (ESP_PLATFORM); KB_MCU becomes ESP32 and KB_RTOS FREERTOS. The ESP32
# port is a separate component in components/ports/esp32/ (ADR-0007); this file never adds the MCU ports. Where
# things live is named once, in tools/layout/paths.yml (cmake/kb_layout.cmake reads it).
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
cmake_minimum_required(VERSION 3.24)

if(ESP_PLATFORM)
  include("${CMAKE_CURRENT_LIST_DIR}/cmake/kb_language.cmake")
  include("${CMAKE_CURRENT_LIST_DIR}/cmake/kb_build_settings.cmake")
  # Absolute paths: the component may be registered through a forwarding folder named `kb`
  # (components/ports/esp32/examples/components/kb), whose own directory is not this one.
  set(_kb_idf_includes "")
  foreach(_part IN LISTS KB_PARTS)
    if(EXISTS "${KB_LIB_DIR}/${_part}/inc/kb/${_part}")
      list(APPEND _kb_idf_includes "${KB_LIB_DIR}/${_part}/inc")
    endif()
  endforeach()
  # kb::rtos on ESP-IDF is the FreeRTOS backend: its two boundary files are the component's only sources.
  set(_kb_idf_sources "")
  if("rtos" IN_LIST KB_PARTS)
    list(APPEND _kb_idf_sources "${KB_LIB_DIR}/rtos/backends/freertos/freertos_boundary.cpp"
         "${KB_LIB_DIR}/rtos/backends/freertos/freertos_isr.cpp")
  endif()
  idf_component_register(SRCS ${_kb_idf_sources} INCLUDE_DIRS ${_kb_idf_includes} REQUIRES ${KB_IDF_REQUIRES})
  if(_kb_idf_sources)
    target_compile_features(${COMPONENT_LIB} PUBLIC ${KB_CXX_FLOOR_FEATURE})
    set_source_files_properties(${_kb_idf_sources} PROPERTIES COMPILE_OPTIONS
      "-std=c++${KB_CPP_STANDARD};-fno-exceptions;-fno-rtti")
  else()
    target_compile_features(${COMPONENT_LIB} INTERFACE ${KB_CXX_FLOOR_FEATURE})
  endif()
  return()
endif()

include("${CMAKE_CURRENT_LIST_DIR}/cmake/kb_version.cmake")
project(kb_core VERSION ${KB_VERSION} DESCRIPTION "kb-core: utils, RTOS, DMA, interfaces and handlers" LANGUAGES CXX)

include("${CMAKE_CURRENT_LIST_DIR}/cmake/kb_language.cmake")
set(CMAKE_CXX_STANDARD ${KB_CPP_STANDARD})
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(KB_BUILD_TESTS "Build the unit tests of the selected parts" OFF)
option(KB_THREADX_HOST_TESTS "With KB_BUILD_TESTS on Linux: also test the ThreadX backend on the pinned kernel's Linux port (third_party/threadx)" OFF)
option(KB_BUILD_INTEGRATION "Build the driver integration tests (every checked-out driver)" OFF)
option(KB_CHECK_BINDINGS "Compile every checked-out driver's binding header on its own (no tests)" OFF)
option(KB_BUILD_FUZZERS "Build the libFuzzer targets (Clang)" OFF)
option(KB_BUILD_EXAMPLES "Build the host examples" OFF)
option(KB_INSTALL "Generate the install and package rules" ${PROJECT_IS_TOP_LEVEL})

include("${CMAKE_CURRENT_LIST_DIR}/cmake/kb_build_settings.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/cmake/kb_part.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/cmake/kb_testing.cmake")
if(KB_BUILD_TESTS OR KB_BUILD_EXAMPLES)
  enable_testing()
endif()

foreach(_part IN LISTS KB_PARTS)
  add_subdirectory("${KB_LIB_DIR}/${_part}")
endforeach()

# kb_parts_cross: every public header of the selected parts on its own, plus the platform-free boundary sources the
# parts register (KB_CROSS_SOURCES), with the strict flags. The Cortex-M cross build and the MISRA Cortex-M scan
# build only this target; it needs no test framework and no host library.
get_property(_kb_cross_sources GLOBAL PROPERTY KB_CROSS_SOURCES)
get_property(_kb_cross_libs GLOBAL PROPERTY KB_CROSS_LIBS)
get_property(_kb_cross_includes GLOBAL PROPERTY KB_CROSS_INCLUDES)
get_property(_kb_cross_system_includes GLOBAL PROPERTY KB_CROSS_SYSTEM_INCLUDES)
add_library(kb_parts_cross OBJECT EXCLUDE_FROM_ALL ${_kb_cross_sources})
kb_strict_target(kb_parts_cross)
target_link_libraries(kb_parts_cross PRIVATE ${_kb_cross_libs})
target_include_directories(kb_parts_cross PRIVATE ${_kb_cross_includes})
target_include_directories(kb_parts_cross SYSTEM PRIVATE ${_kb_cross_system_includes})

# Test folders get their CMakeLists.txt with the first source; until then they are only described.
foreach(_dir IN ITEMS integration standalone install_consumer docs)
  if(EXISTS "${KB_TESTS_DIR}/${_dir}/CMakeLists.txt")
    add_subdirectory("${KB_TESTS_DIR}/${_dir}")
  endif()
endforeach()

# Examples (examples/README.md): real programs, each built here when KB_BUILD_EXAMPLES is ON.
if(KB_BUILD_EXAMPLES)
  add_subdirectory("${KB_EXAMPLES_DIR}" "${CMAKE_BINARY_DIR}/examples")
endif()

include("${CMAKE_CURRENT_LIST_DIR}/cmake/kb_tool_tests.cmake")

if(KB_INSTALL)
  include(CMakePackageConfigHelpers)
  configure_package_config_file(cmake/kbConfig.cmake.in "${CMAKE_CURRENT_BINARY_DIR}/kbConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/kb)
  # Before 1.0 a minor release may break; after it, SameMajorVersion.
  write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/kbConfigVersion.cmake"
    VERSION ${KB_VERSION} COMPATIBILITY SameMinorVersion)
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/kbConfig.cmake" "${CMAKE_CURRENT_BINARY_DIR}/kbConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/kb)
endif()
