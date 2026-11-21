# The repository layout, read from tools/layout/paths.yml (the one file that names where things live):
#   kb_layout(<name> <out-var>)           absolute path of a layout entry, e.g. kb_layout(lib KB_LIB_DIR)
#   kb_layout_relative(<name> <out-var>)  the same relative to the repository root
# Including this file also defines KB_<NAME>_DIR (absolute) and KB_<NAME>_REL (relative) for every entry, in the
# includer's scope: KB_LIB_DIR, KB_PORTS_DIR, KB_DRIVERS_DIR, KB_HOST_PORT_DIR, KB_SCHEMAS_DIR, KB_EXAMPLES_DIR, ...
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
include_guard(GLOBAL)

get_filename_component(_kb_layout_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(_kb_layout_file "${_kb_layout_root}/tools/layout/paths.yml")
set_property(GLOBAL PROPERTY KB_LAYOUT_ROOT "${_kb_layout_root}")
if(NOT EXISTS "${_kb_layout_file}")
  message(FATAL_ERROR "[kb] ${_kb_layout_file} is missing: cmake/kb_layout.cmake reads it. A copy of kb-core that "
                      "vendors cmake/ must vendor tools/layout/paths.yml with it (docs/layout.md).")
endif()
# A change of the layout file re-runs the configure step.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_kb_layout_file}")

# The `layout:` section: two-space indented `name: path` lines.
file(STRINGS "${_kb_layout_file}" _kb_layout_lines)
set(_kb_in_layout FALSE)
foreach(_line IN LISTS _kb_layout_lines)
  if(_line MATCHES "^layout:[ ]*$")
    set(_kb_in_layout TRUE)
  elseif(_line MATCHES "^[A-Za-z_]+:")
    set(_kb_in_layout FALSE)
  elseif(_kb_in_layout AND _line MATCHES "^  ([A-Za-z_]+):[ ]*([^ #]+)[ ]*$")
    set_property(GLOBAL PROPERTY KB_LAYOUT_${CMAKE_MATCH_1} "${CMAKE_MATCH_2}")
    string(TOUPPER "${CMAKE_MATCH_1}" _kb_upper)
    set(KB_${_kb_upper}_REL "${CMAKE_MATCH_2}")
    set(KB_${_kb_upper}_DIR "${_kb_layout_root}/${CMAKE_MATCH_2}")
  endif()
endforeach()

function(kb_layout_relative name out)
  get_property(_rel GLOBAL PROPERTY KB_LAYOUT_${name})
  if("${_rel}" STREQUAL "")
    message(FATAL_ERROR "[kb] tools/layout/paths.yml has no layout entry '${name}'")
  endif()
  set(${out} "${_rel}" PARENT_SCOPE)
endfunction()

function(kb_layout name out)
  kb_layout_relative(${name} _rel)
  get_property(_root GLOBAL PROPERTY KB_LAYOUT_ROOT)
  set(${out} "${_root}/${_rel}" PARENT_SCOPE)
endfunction()
