# kb-core's language floor and the standard a build uses (docs/language-floor.md):
#   KB_CPP_STANDARD_FLOOR  the `language: standard:` of kb.yml (20 when the file is missing, the registry default)
#   KB_CPP_STANDARD        cache: the standard this build compiles with; defaults to the floor, may be 20, 23 or 26,
#                          and may never be below the floor (configure fails with a message that names both)
#   KB_CXX_FLOOR_FEATURE   cxx_std_<floor>: what every kb target requires of its consumers (INTERFACE); with the
#                          floor of today that is cxx_std_20 (handbook KB-CPP-001)
#   KB_CXX_BUILD_FEATURE   cxx_std_<KB_CPP_STANDARD>: what kb-core's own tests, examples and boundary files build as
#   KB_CPLUSPLUS           the __cplusplus value of KB_CPP_STANDARD, asserted by the header checks
# A project that resolves an image with a `language:` block gets KB_CPP_STANDARD from its kb_image.cmake.
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
include_guard(GLOBAL)

set(_kb_language_file "${CMAKE_CURRENT_LIST_DIR}/../kb.yml")
set(KB_CPP_STANDARD_FLOOR 20)
if(EXISTS "${_kb_language_file}")
  file(STRINGS "${_kb_language_file}" _kb_language_lines)
  set(_kb_in_language FALSE)
  foreach(_line IN LISTS _kb_language_lines)
    if(_line MATCHES "^language:")
      set(_kb_in_language TRUE)
    elseif(_line MATCHES "^[A-Za-z_]")
      set(_kb_in_language FALSE)
    elseif(_kb_in_language AND _line MATCHES "^  standard: *([0-9]+)")
      set(KB_CPP_STANDARD_FLOOR "${CMAKE_MATCH_1}")
    endif()
  endforeach()
else()
  message(STATUS "[kb] ${_kb_language_file} not found: language floor C++20 (the registry default)")
endif()

set(KB_CPP_STANDARD "${KB_CPP_STANDARD_FLOOR}" CACHE STRING
  "C++ standard kb-core builds with: 20, 23 or 26, never below the floor in kb.yml (docs/language-floor.md)")
set_property(CACHE KB_CPP_STANDARD PROPERTY STRINGS 20 23 26)
if(KB_CPP_STANDARD MATCHES "^[0-9]+$" AND KB_CPP_STANDARD LESS KB_CPP_STANDARD_FLOOR)
  message(FATAL_ERROR "[kb] KB_CPP_STANDARD=${KB_CPP_STANDARD} is below kb-core's language floor C++${KB_CPP_STANDARD_FLOOR} "
    "(kb.yml): build with ${KB_CPP_STANDARD_FLOOR} or newer, or use an older kb-core release (docs/language-floor.md)")
endif()
if(NOT KB_CPP_STANDARD MATCHES "^(20|23|26)$")
  message(FATAL_ERROR "[kb] KB_CPP_STANDARD=${KB_CPP_STANDARD}: use 20, 23 or 26 (docs/language-floor.md)")
endif()
set(KB_CXX_FLOOR_FEATURE "cxx_std_${KB_CPP_STANDARD_FLOOR}")
set(KB_CXX_BUILD_FEATURE "cxx_std_${KB_CPP_STANDARD}")
set(_kb_cplusplus_20 202002L)
set(_kb_cplusplus_23 202302L)
set(_kb_cplusplus_26 202400L)  # GCC 14 and Clang 18 report 202400L for -std=c++26 until the standard is published
set(KB_CPLUSPLUS "${_kb_cplusplus_${KB_CPP_STANDARD}}")
