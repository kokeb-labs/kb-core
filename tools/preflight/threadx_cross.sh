#!/usr/bin/env bash
# The ThreadX backend and the pinned kernel (third_party/threadx) cross-compiled for Cortex-M0+, M4F and M7F with Arm
# GCC and kb-core's strict flags, on the kernel's own ports (cortex_m0, cortex_m4, cortex_m7). Run by the pre-flight
# (tools/preflight.sh --container, in the CI image) and by CI (ci-cross-build.yml, job ThreadX Cortex-M).
#
#   tools/preflight/threadx_cross.sh [SRC]     SRC: a checkout with third_party/threadx initialised (default .)
#   BUILD_ROOT (default /tmp/kb-threadx-cross): one build folder per core, removed after each core
#   THREADX_DIR (default third_party/threadx): another ThreadX checkout, e.g. a certified release
#   PORTS (default "cortex_m0 cortex_m4 cortex_m7"): the ports to build
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
set -uo pipefail
cd "${1:-.}" || exit 2
tx_dir="${THREADX_DIR:-$PWD/third_party/threadx}"
[ -f "$tx_dir/common/inc/tx_api.h" ] || {
  echo "$tx_dir is not a ThreadX checkout: git submodule update --init third_party/threadx"; exit 2; }
ports=" ${PORTS:-cortex_m0 cortex_m4 cortex_m7} "
root="${BUILD_ROOT:-/tmp/kb-threadx-cross}"
mkdir -p "$root"
rc=0
for spec in "cortex_m0:cortex-m0plus -mthumb -mfloat-abi=soft" \
            "cortex_m4:cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16" \
            "cortex_m7:cortex-m7 -mthumb -mfloat-abi=hard -mfpu=fpv5-d16"; do
  port="${spec%%:*}"; flags="-mcpu=${spec#*:}"
  case "$ports" in *" $port "*) ;; *) continue ;; esac
  tc="$root/$port.cmake"; build="$root/$port"
  {
    echo "set(CMAKE_SYSTEM_NAME Generic)"
    echo "set(CMAKE_SYSTEM_PROCESSOR arm)"
    echo "set(CMAKE_C_COMPILER arm-none-eabi-gcc)"
    echo "set(CMAKE_CXX_COMPILER arm-none-eabi-g++)"
    echo "set(CMAKE_ASM_COMPILER arm-none-eabi-gcc)"
    echo "set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)"
    echo "set(CMAKE_C_FLAGS_INIT \"$flags -ffunction-sections -fdata-sections\")"
    echo "set(CMAKE_CXX_FLAGS_INIT \"$flags -ffunction-sections -fdata-sections\")"
    echo "set(CMAKE_ASM_FLAGS_INIT \"$flags -x assembler-with-cpp\")"
  } >"$tc"
  rm -rf "$build"
  if cmake -S . -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=MinSizeRel -DCMAKE_TOOLCHAIN_FILE="$tc" -DKB_MCU=HOST \
       -DKB_RTOS=THREADX -DKB_THREADX_PORT="$port" -DKB_THREADX_DIR="$tx_dir" -DKB_PARTS=rtos >"$root/$port.log" 2>&1 &&
     cmake --build "$build" --target kb_rtos_backend_threadx threadx >>"$root/$port.log" 2>&1; then
    size="$(arm-none-eabi-size -t "$build/components/lib/rtos/backends/threadx/libkb_rtos_backend_threadx.a" | tail -1 |
      awk '{print "text " $1 ", data " $2 ", bss " $3}')"
    echo "PASS $port ($flags): backend $size"
  else
    echo "FAIL $port ($flags): $root/$port.log"; grep -E "error|Error" "$root/$port.log" | head -15; rc=1
  fi
  rm -rf "$build"
done
exit $rc
