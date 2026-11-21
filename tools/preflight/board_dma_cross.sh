#!/usr/bin/env bash
# Generate the STM32H743 example board, compile its board_dma.cpp (and a unit that includes board_dma.hpp) with Arm GCC
# for a Cortex-M7, and link them with a minimal linker script that INCLUDEs the generated kb_dma.ld: the real linker
# proves the sections and the ASSERTs. Run by CI (ci-cross-build.yml, job board-dma) and, in the CI image, the pre-flight.
#
#   tools/preflight/board_dma_cross.sh [OUT_DIR]     OUT_DIR default: /tmp/kb-board-dma
# Needs python3 with PyYAML and arm-none-eabi-g++ on the PATH.
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
out="${1:-/tmp/kb-board-dma}"
gen="$out/gen"
rm -rf "$out"; mkdir -p "$gen"
python3 "$root/tools/kb/kb.py" board gen "$root/examples/boards/stm32h743-dma/board.yml" -o "$gen"
cpu=(-mcpu=cortex-m7 -mthumb -mfpu=fpv5-d16 -mfloat-abi=hard)
flags=("${cpu[@]}" -std=c++20 -Os -Wall -Wextra -Werror -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections
       -I"$root/components/lib/dma/inc" -I"$root/components/lib/utils/inc" -I"$gen")
arm-none-eabi-g++ "${flags[@]}" -c "$gen/board_dma.cpp" -o "$out/board_dma.o"
arm-none-eabi-g++ "${flags[@]}" -c "$root/tests/resolver/board_dma_tu.cpp" -o "$out/tu.o"
arm-none-eabi-g++ "${cpu[@]}" -nostartfiles -nostdlib -Wl,--gc-sections -Wl,-Map,"$out/board.map" \
  -T "$root/tests/resolver/board_dma_cross.ld" -L"$gen" "$out/board_dma.o" "$out/tu.o" -o "$out/board.elf"
arm-none-eabi-readelf -S "$out/board.elf" | grep -E "kb_dma|Name" || true
# The sections sit where the board said: the symbols the generated fragment defines are the region bases.
sym() { arm-none-eabi-nm "$out/board.elf" | awk -v n="$1" '$3 == n { print $1 }'; }
[ "$(sym __board_dma_d2_dma_start)" = "30000000" ] || { echo "FAIL: d2_dma section is not at 0x30000000"; exit 1; }
[ "$(sym __board_dma_axi_dma_start)" = "24060000" ] || { echo "FAIL: axi_dma section is not at 0x24060000"; exit 1; }
arm-none-eabi-size "$out/board.elf"
echo "PASS board-dma: generated sources compile for cortex-m7 and link with kb_dma.ld"
