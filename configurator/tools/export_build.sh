#!/usr/bin/env bash
# Builds one starter project the configurator exported (tools/export_matrix.mjs OUT, then OUT/<row>/project):
#
#   tools/export_build.sh PROJECT host|cortex-m|esp-idf [KB_CORE_DIR]
#
# 1. git init, then the project's own kb-fetch.sh: kb-core and every driver become submodules under external/ at
#    the locked tags, each checked against its commit, exactly as the README tells a user.
# 2. With KB_CORE_DIR (a kb-core checkout, CI's pull request tree): external/kb-core is moved to that tree's HEAD,
#    so a change to the generator or the libraries is built before it is released.
# 3. The build the README names:
#    host      cmake --preset <image> with KB_CHECK_BINDINGS: the app, every public header of the parts and the
#              binding of each selected driver on its own (kb_parts_cross, strict flags), then the app runs;
#    cortex-m  the same project cross-compiled with arm-none-eabi-g++ (Cortex-M4, hard float): kb_parts_cross and
#              the board library;
#    esp-idf   idf.py set-target <target> build, with ESP-IDF in the environment (IDF_PATH, export.sh sourced).
# Exit 0 when the build (and the app) succeed. Needs git, CMake 3.25, a C++20 compiler; the toolchain of the kind.
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
set -euo pipefail
project="$(cd "$1" && pwd)"; kind="$2"; kb_core="${3:-}"
cd "$project"
image="$(sed -n 's/^image: *"\{0,1\}\([a-z0-9-]*\)"\{0,1\}$/\1/p' kb-image.yml | head -1)"
target="$(sed -n 's/.*target: *"\{0,1\}\([a-z0-9]*\)"\{0,1\}[,}].*/\1/p' kb-image.yml | head -1)"
jobs="${CMAKE_BUILD_PARALLEL_LEVEL:-4}"

if [ ! -d .git ]; then
  git init -q
  git -c user.name=kb -c user.email=kb@invalid commit -q --allow-empty -m "starter project"
fi
sh kb-fetch.sh
sh kb-fetch.sh --verify
if [ -n "$kb_core" ]; then
  head="$(git -C "$kb_core" rev-parse HEAD)"
  git -C external/kb-core fetch -q "$kb_core" "$head"
  git -C external/kb-core -c advice.detachedHead=false checkout -q "$head"
  echo "export_build: external/kb-core moved to the tree under test, $head"
fi

case "$kind" in
  host)
    cmake --preset "$image" -DKB_CHECK_BINDINGS=ON
    cmake --build "build/$image" --parallel "$jobs" --target app kb_parts_cross
    "build/$image/app"
    echo "export_build: $image built and ran on the host"
    ;;
  cortex-m)
    tc="$project/build/cortex-m4.cmake"
    mkdir -p build
    cat >"$tc" <<'EOF'
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(CMAKE_C_COMPILER arm-none-eabi-gcc)
set(CMAKE_CXX_COMPILER arm-none-eabi-g++)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_CXX_FLAGS_INIT "-mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16 -ffunction-sections -fdata-sections -fno-exceptions -fno-rtti")
EOF
    cmake --preset "$image" -B "build/$image-cortex-m4" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$tc" -DCMAKE_BUILD_TYPE=MinSizeRel \
      -DKB_CHECK_BINDINGS=ON
    cmake --build "build/$image-cortex-m4" --parallel "$jobs" --target board kb_parts_cross
    arm-none-eabi-size -t "build/$image-cortex-m4/libboard.a" | tail -1
    echo "export_build: $image cross-compiled for Cortex-M4"
    ;;
  esp-idf)
    : "${IDF_PATH:?export_build: ESP-IDF is not in the environment (source export.sh first)}"
    idf.py -C "$project" -B "$project/build/esp-idf" set-target "$target"
    idf.py -C "$project" -B "$project/build/esp-idf" build
    echo "export_build: $image built with ESP-IDF for $target"
    ;;
  *)
    echo "export_build: unknown kind $kind (host, cortex-m, esp-idf)" >&2
    exit 2
    ;;
esac
