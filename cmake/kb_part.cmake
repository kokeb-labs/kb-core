# kb_add_part(<part> DEPENDS <part>...): one part as an INTERFACE target kb_<part> with the alias kb::<part>, its
# include directory, C++20, and the install and export rules of the package kb (component <part>).
# Header-only parts stay INTERFACE; a part that gains boundary .cpp files gets a separate PRIVATE port or backend
# target, never public sources (ADR-0001).
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
include_guard(GLOBAL)
include(GNUInstallDirs)

function(kb_add_part part)
  cmake_parse_arguments(PARSE_ARGV 1 ARG "" "" "DEPENDS")
  set(target "kb_${part}")
  add_library(${target} INTERFACE)
  add_library(kb::${part} ALIAS ${target})
  set_target_properties(${target} PROPERTIES EXPORT_NAME ${part})
  target_compile_features(${target} INTERFACE cxx_std_20)
  target_include_directories(${target} INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/inc>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
  foreach(dep IN LISTS ARG_DEPENDS)
    target_link_libraries(${target} INTERFACE kb::${dep})
  endforeach()
  install(TARGETS ${target} EXPORT kb_${part}Targets)
  install(DIRECTORY inc/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} FILES_MATCHING PATTERN "*.hpp")
  install(DIRECTORY src/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/kb/${part}/src FILES_MATCHING PATTERN "*.ipp")
  install(EXPORT kb_${part}Targets NAMESPACE kb:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/kb)
endfunction()
