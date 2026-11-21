# kb-core examples (examples/README.md): real programs, built when KB_BUILD_EXAMPLES is ON and run as ctest tests
# named example_<folder>, held to the same strict flags as the library. Every folder under host/ with a CMakeLists.txt
# is added; examples/examples.yml lists them all and tools/examples/check.py keeps the list, this folder, the README
# and the documentation snippets in step. The ESP-IDF examples are the ESP32 port's own project, not added here.
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
# kb_add_example(<name> [REQUIRES <target>...] [SOURCES <file>...] [LIBS <target>...]): the executable example_<name>
# from main.cpp (or SOURCES), linked with LIBS, and its test. An example whose REQUIRES targets do not exist in this
# configuration (for example a part left out of KB_PARTS) is skipped with a status line, not silently dropped.
function(kb_add_example name)
  cmake_parse_arguments(PARSE_ARGV 1 ARG "" "" "REQUIRES;SOURCES;LIBS")
  foreach(_req IN LISTS ARG_REQUIRES)
    if(NOT TARGET ${_req})
      message(STATUS "[kb] example ${name} skipped: target ${_req} is not part of this configuration")
      return()
    endif()
  endforeach()
  if(NOT ARG_SOURCES)
    set(ARG_SOURCES main.cpp)
  endif()
  set(target example_${name})
  add_executable(${target} ${ARG_SOURCES})
  kb_strict_target(${target})
  target_link_libraries(${target} PRIVATE ${ARG_LIBS})
  add_test(NAME ${target} COMMAND ${target})
  set_tests_properties(${target} PROPERTIES LABELS example TIMEOUT 60)
endfunction()

file(GLOB _kb_host_examples LIST_DIRECTORIES true RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}/host"
     CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/host/*")
list(SORT _kb_host_examples)
foreach(_example IN LISTS _kb_host_examples)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/host/${_example}/CMakeLists.txt")
    add_subdirectory("host/${_example}")
  endif()
endforeach()
