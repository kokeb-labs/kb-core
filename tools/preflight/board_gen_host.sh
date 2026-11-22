#!/usr/bin/env bash
# Generate the files of each example board (examples/boards/*/board.yml) with `kb board gen` and compile them with the
# host compiler against kb::dma: board_dma.cpp, and a unit that includes board_dma.hpp, so the generated
# static_asserts run. Run by ctest (kb_board_gen_host) and the pre-flight.
#
#   tools/preflight/board_gen_host.sh PYTHON CXX STD OUT_DIR INCLUDE_DIR...
# Exit 77 (the ctest skip code) when PyYAML is missing and CI is not set.
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
set -uo pipefail
py="$1"; cxx="$2"; std="$3"; out="$4"; shift 4
root="$(cd "$(dirname "$0")/../.." && pwd)"
incs=()
for d in "$@"; do incs+=("-I$d"); done
if ! "$py" -c "import yaml" 2>/dev/null; then
  if [ -n "${CI:-}" ] || [ "${KB_REQUIRE_PYYAML:-}" = 1 ]; then echo "PyYAML is missing"; exit 1; fi
  echo "PyYAML is not installed: kb_board_gen_host skipped"; exit 77
fi
rm -rf "$out"; mkdir -p "$out"
rc=0
for board in "$root"/examples/boards/*/board.yml; do
  name="$(basename "$(dirname "$board")")"
  gen="$out/$name"
  if ! "$py" "$root/tools/kb/kb.py" board gen "$board" -o "$gen"; then echo "FAIL $name: kb board gen"; rc=1; continue; fi
  flags=(-std="c++$std" -Wall -Wextra -Werror -fno-exceptions -fno-rtti "${incs[@]}" -I"$gen")
  if "$cxx" "${flags[@]}" -c "$gen/board_dma.cpp" -o "$gen/board_dma.o" &&
     cp "$root/tests/resolver/board_dma_tu.cpp.in" "$gen/board_dma_tu.cpp" &&
     "$cxx" "${flags[@]}" -c "$gen/board_dma_tu.cpp" -o "$gen/tu.o"; then
    echo "PASS $name"
  else
    echo "FAIL $name: compile"; rc=1
  fi
done
exit $rc
