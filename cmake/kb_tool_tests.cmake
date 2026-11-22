# Tests of the kb tool (tools/kb) and of the configurator's resolver against the shared vectors (ADR-0014):
#   kb_cli_unit             the Python unit tests and every vector (skipped, code 77, when PyYAML is missing)
#   kb_resolver_js_vectors  every vector through configurator/resolver.mjs (when node is installed)
#   kb_layout_unit          tools/layout: the layout file, CMake and Python agree, the scan for retired locations
#   kb_layout_root          no unexpected top-level entry (the allowlist in tools/layout/paths.yml)
#   kb_layout_stale         no tracked file names a retired location
#   kb_examples_unit        tools/examples: the checks on throwaway trees
#   kb_examples_index       examples/examples.yml, the folders, the README, the doc snippets and the catalogue agree
#   kb_board_gen_host       kb board gen on both example boards; the generated files compile against kb::dma (needs the dma part)
#   kb_language_floor       configuring with KB_CPP_STANDARD below the floor in kb.yml fails with its message
#   kb_port_sdk             the port SDK asset (tools/release): content, checks, and a minimal port built from the
#                           extracted archive alone, running the DMA conformance suite (docs/port-sdk.md)
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
  # The port SDK: the archive is built from the checkout, extracted, and a minimal port is configured against it.
  # Needs git (the file list is the tracked files) and cmake.
  if(EXISTS "${KB_ROOT}/.git")
    add_test(NAME kb_port_sdk COMMAND ${Python3_EXECUTABLE} ${KB_TOOLS_DIR}/release/tests/run.py)
    set_tests_properties(kb_port_sdk PROPERTIES LABELS tools TIMEOUT 600
      ENVIRONMENT "KB_PORT_SDK_CXX=${CMAKE_CXX_COMPILER}")
  endif()
  # The vulnerability scan and its renderer, answered from fixtures (no network) and checked against the snapshot.
  add_test(NAME kb_vuln_unit COMMAND ${Python3_EXECUTABLE} ${KB_TOOLS_DIR}/vuln/tests/run.py)
  set_tests_properties(kb_vuln_unit PROPERTIES LABELS tools)
  add_test(NAME kb_examples_index COMMAND ${Python3_EXECUTABLE} ${KB_TOOLS_DIR}/examples/examples.py check)
  set_tests_properties(kb_examples_index PROPERTIES LABELS tools)
endif()

# The generated board files (kb board gen, docs/design/board-dma.md) compile against kb::dma with the host compiler.
if(Python3_Interpreter_FOUND AND TARGET kb_dma)
  add_test(NAME kb_board_gen_host
    COMMAND bash ${KB_TOOLS_DIR}/preflight/board_gen_host.sh ${Python3_EXECUTABLE} ${CMAKE_CXX_COMPILER}
            ${KB_CPP_STANDARD} ${CMAKE_BINARY_DIR}/kb_board_gen
            $<TARGET_PROPERTY:kb_dma,INTERFACE_INCLUDE_DIRECTORIES>
            $<TARGET_PROPERTY:kb_utils,INTERFACE_INCLUDE_DIRECTORIES>
    COMMAND_EXPAND_LISTS)
  set_tests_properties(kb_board_gen_host PROPERTIES SKIP_RETURN_CODE 77 LABELS tools)
endif()

# The language floor (cmake/kb_language.cmake): a standard below kb.yml's is refused at configure time.
add_test(NAME kb_language_floor
  COMMAND ${CMAKE_COMMAND} -S ${KB_ROOT} -B ${CMAKE_BINARY_DIR}/kb_language_floor -DKB_CPP_STANDARD=17)
set_tests_properties(kb_language_floor PROPERTIES LABELS tools
  PASS_REGULAR_EXPRESSION "is below kb-core's language floor C\\+\\+20")

find_program(KB_NODE node)
if(KB_NODE)
  add_test(NAME kb_resolver_js_vectors
    COMMAND ${KB_NODE} ${KB_TESTS_DIR}/resolver/run_js_vectors.mjs)
  set_tests_properties(kb_resolver_js_vectors PROPERTIES LABELS tools)
else()
  message(STATUS "[kb] node not found: kb_resolver_js_vectors is not added")
endif()
