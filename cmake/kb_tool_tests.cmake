# Tests of the kb tool (tools/kb) and of the configurator's resolver against the shared vectors (ADR-0014):
#   kb_cli_unit             the Python unit tests and every vector (skipped, code 77, when PyYAML is missing)
#   kb_resolver_js_vectors  every vector through configurator/resolver.mjs (when node is installed)
#   kb_layout_unit          tools/layout: the layout file, CMake and Python agree, the scan for retired locations
#   kb_layout_root          no unexpected top-level entry (the allowlist in tools/layout/paths.yml)
#   kb_layout_stale         no tracked file names a retired location
#   kb_examples_unit        tools/examples: the checks on throwaway trees
#   kb_examples_index       examples/examples.yml, the folders, the README, the doc snippets and the catalogue agree
# Included from the root CMakeLists.txt; adds nothing unless KB_BUILD_TESTS is ON.
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
include_guard(GLOBAL)

if(NOT KB_BUILD_TESTS)
  return()
endif()
enable_testing()

find_package(Python3 3.10 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  add_test(NAME kb_cli_unit COMMAND ${Python3_EXECUTABLE} ${KB_TOOLS_DIR}/kb/tests/run.py)
  set_tests_properties(kb_cli_unit PROPERTIES SKIP_RETURN_CODE 77 LABELS tools)
else()
  message(STATUS "[kb] Python 3.10 not found: kb_cli_unit is not added")
endif()

# The layout and examples checks need no PyYAML. The two repository checks need a git checkout (a copy of the
# sources without .git, such as a consumer's vendored snapshot, skips them).
if(Python3_Interpreter_FOUND)
  add_test(NAME kb_layout_unit COMMAND ${Python3_EXECUTABLE} ${KB_TOOLS_DIR}/layout/tests/run.py)
  add_test(NAME kb_examples_unit COMMAND ${Python3_EXECUTABLE} ${KB_TOOLS_DIR}/examples/tests/run.py)
  set_tests_properties(kb_layout_unit kb_examples_unit PROPERTIES LABELS tools)
  if(EXISTS "${KB_ROOT}/.git")
    add_test(NAME kb_layout_root COMMAND ${Python3_EXECUTABLE} -I ${KB_TOOLS_DIR}/layout/layout.py root)
    add_test(NAME kb_layout_stale COMMAND ${Python3_EXECUTABLE} -I ${KB_TOOLS_DIR}/layout/layout.py stale)
    set_tests_properties(kb_layout_root kb_layout_stale PROPERTIES LABELS tools)
  endif()
  add_test(NAME kb_examples_index COMMAND ${Python3_EXECUTABLE} ${KB_TOOLS_DIR}/examples/examples.py check)
  set_tests_properties(kb_examples_index PROPERTIES LABELS tools)
endif()

find_program(KB_NODE node)
if(KB_NODE)
  add_test(NAME kb_resolver_js_vectors
    COMMAND ${KB_NODE} ${KB_TESTS_DIR}/resolver/run_js_vectors.mjs)
  set_tests_properties(kb_resolver_js_vectors PROPERTIES LABELS tools)
else()
  message(STATUS "[kb] node not found: kb_resolver_js_vectors is not added")
endif()
