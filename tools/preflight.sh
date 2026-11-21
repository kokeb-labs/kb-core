#!/usr/bin/env bash
# Every check CI runs that can run on a workstation, before every push: CI minutes are not for finding what this
# finds. Push only when the last line says "0 failure(s)".
#
#   tools/preflight.sh                 the local checks (below) with whatever is installed; skipped ones are named
#   tools/preflight.sh --container     also the Linux gates in the CI images: coverage, sanitizers, libFuzzer
#                                      (tools/preflight/gates.sh), the Cortex-M cross build (with the ThreadX
#                                      backend on the pinned kernel's ports, tools/preflight/threadx_cross.sh),
#                                      the ThreadX Linux-port tests, cppcheck, lizard, and
#                                      the MISRA scan (tools/preflight/misra.sh, CodeQL; slow)
#   tools/preflight.sh --container --no-misra
#
# Local checks: the layout and example-index checks (tools/layout, tools/examples); GCC (CXX_GCC, default g++-15)
# and Clang (CXX_CLANG) builds and tests, examples included; the RISC-V ESP32 compiler
# (RISCV_CXX) in -fsyntax-only mode over every header check; clang-format and clang-tidy 20 (CLANG_FORMAT,
# CLANG_TIDY); misra_lint, eval_order_check and recursion_check (tools/lint/); the handbook check (CI_TOOLS_DIR);
# Python tool tests; yamllint, markdownlint and the strict MkDocs build (VENV). JOBS caps every build, test and
# clang-tidy run (and is passed to the container gates); unset, builds use every core and tests and tidy 8.
# Images: kokeb-ci:local and the MISRA image (MISRA_IMAGE, default kokeb-ci-misra:v1.19: driver-template
# tools/ci-container/Dockerfile.misra built with CI_TOOLS_REF set to the ci-tools commit that @v1 points at, so the
# local scan matches CI). Every docker call is preceded by a 20 s `docker info` probe.
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
set -uo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root" || exit 2
container=0; misra=1
for a in "$@"; do
  case "$a" in
    --container) container=1 ;;
    --no-misra) misra=0 ;;
    *) echo "unknown option $a"; exit 2 ;;
  esac
done
work="$(mktemp -d "${TMPDIR:-/tmp}/kb-preflight.XXXXXX")"
fails=(); skipped=()
step() { local n="$1"; shift; printf -- '-- %s\n' "$n"; if "$@" >"$work/$n.log" 2>&1; then echo "   pass"; else echo "   FAIL ($work/$n.log)"; tail -25 "$work/$n.log" | sed 's/^/     /'; fails+=("$n"); fi; }
have() { command -v "$1" >/dev/null 2>&1; }

# Where things live comes from tools/layout/paths.yml (the one place that names it), never from this script.
layout() { python3 -I tools/layout/layout.py get "$1"; }
LIB="$(layout lib)"; PORTS="$(layout ports)"; DRIVERS="$(layout drivers)"; HOST_PORT="$(layout host_port)"
TESTS="$(layout tests)"; EXAMPLES="$(layout examples)"

CXX_GCC="${CXX_GCC:-g++-15}"
JOBS="${JOBS:-}"
CXX_CLANG="${CXX_CLANG:-$(command -v /opt/homebrew/opt/llvm/bin/clang++ || command -v clang++)}"
VENV="${VENV:-$root/../venv}"
CLANG_FORMAT="${CLANG_FORMAT:-$VENV/bin/clang-format}"
CLANG_TIDY="${CLANG_TIDY:-$VENV/bin/clang-tidy}"
CI_TOOLS_DIR="${CI_TOOLS_DIR:-$root/../ci-tools}"
RISCV_CXX="${RISCV_CXX:-$(ls -d "$HOME"/.espressif/tools/riscv32-esp-elf/*/riscv32-esp-elf/bin/riscv32-esp-elf-g++ 2>/dev/null | tail -1)}"
# The ports tree includes the host port; the esp32 example project's vendored scripts hold no C++.
sources() {
  git ls-files -co --exclude-standard -- "$LIB/*.hpp" "$LIB/*.cpp" "$LIB/*.ipp" "$TESTS/*.hpp" "$TESTS/*.cpp" \
    "$PORTS/*.hpp" "$PORTS/*.cpp" "$EXAMPLES/*.hpp" "$EXAMPLES/*.cpp"
}
library() { git ls-files -co --exclude-standard -- "$LIB/*/inc/*.hpp" "$LIB/*/src/*.ipp" "$LIB/rtos/backends/*.cpp" "$LIB/rtos/backends/*.hpp" "$LIB/interfaces/host/*.hpp" "$LIB/interfaces/host/*.cpp" | grep -v '/tests/'; }

# The kb tool tests need PyYAML: use the venv's Python for them and make a missing PyYAML a failure, not a skip.
KB_PYTHON="${KB_PYTHON:-$VENV/bin/python}"
py_args=(); if [ -x "$KB_PYTHON" ]; then py_args=("-DPython3_EXECUTABLE=$KB_PYTHON"); fi
build() {  # build <dir> <cxx> [cmake args]: configure, build and test
  local dir="$1" cxx="$2"; shift 2
  cmake -S . -B "$dir" -G Ninja -DCMAKE_CXX_COMPILER="$cxx" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DKB_BUILD_TESTS=ON \
    -DKB_BUILD_EXAMPLES=ON ${py_args[@]+"${py_args[@]}"} "$@" && cmake --build "$dir" --parallel ${JOBS:+"$JOBS"} && \
    KB_REQUIRE_PYYAML=1 ctest --test-dir "$dir" --output-on-failure --parallel "${JOBS:-8}"
}
if have "$CXX_GCC"; then step gcc build "$work/gcc" "$CXX_GCC"; else skipped+=("gcc"); fi
if [ -n "$CXX_CLANG" ]; then step clang build "$work/clang" "$CXX_CLANG"; else skipped+=("clang"); fi

riscv() {  # every header check and platform-free source as ESP32-C3/C6 code, syntax and warnings only
  local f n=0
  for f in "$work/clang/kb_header_checks"/*.cpp $(git ls-files -co --exclude-standard -- "$LIB/rtos/backends/none/*.cpp"); do
    "$RISCV_CXX" -std=c++20 -fsyntax-only -fno-exceptions -fno-rtti -Wall -Wextra -Wpedantic -Wformat=2 -Wmain \
      -Wconversion -Wsign-conversion -Wshadow -Werror $(for p in "$LIB"/*/inc; do printf -- '-I%s ' "$p"; done) \
      -I"$LIB/rtos/backends/none/inc" "$f" || return 1
    n=$((n + 1))
  done
  echo "$n file(s) checked"
}
if [ -x "$RISCV_CXX" ] && [ -d "$work/clang" ]; then step riscv riscv; else skipped+=("riscv"); fi

fmt() { sources | sort -u | xargs "$CLANG_FORMAT" --style="file:_config/.clang-format" --dry-run --Werror; }
if [ -x "$CLANG_FORMAT" ]; then step clang-format fmt; else skipped+=("clang-format"); fi

tidy() {  # clang-tidy 20 with the org configuration over kb-core's own translation units (not driver headers)
  python3 - "$work/clang/compile_commands.json" "$root" "$LIB/" "$HOST_PORT/" "$EXAMPLES/" <<'EOF' | sort -u >"$work/tidy-files.txt"
import json, os, sys
root, prefixes = sys.argv[2], tuple(sys.argv[3:])
for e in json.load(open(sys.argv[1])):
    f = os.path.relpath(e["file"], root)
    if (f.startswith(prefixes) or "kb_header_checks" in f) and "/compile_fail/" not in f:
        print(e["file"])
EOF
  local sdk=(); if have xcrun; then sdk=("--extra-arg=-isysroot$(xcrun --show-sdk-path)"); fi
  xargs -P "${JOBS:-8}" -n 1 "$CLANG_TIDY" -p "$work/clang" --config-file=_config/.clang-tidy --quiet "${sdk[@]}" <"$work/tidy-files.txt" \
    >"$work/tidy-out.txt" 2>&1
  grep -E "(warning|error):" "$work/tidy-out.txt" | grep -v "/drivers/\|/build" | sort -u | tee "$work/tidy-findings.txt"
  [ ! -s "$work/tidy-findings.txt" ]
}
if [ -x "$CLANG_TIDY" ] && [ -f "$work/clang/compile_commands.json" ]; then step clang-tidy tidy; else skipped+=("clang-tidy"); fi

# The layout (tools/layout/paths.yml) and the examples (examples/examples.yml): fast, no build.
step layout-root python3 -I tools/layout/layout.py root
step layout-stale python3 -I tools/layout/layout.py stale
step examples-index python3 tools/examples/examples.py check
step layout-tool-tests bash -c "python3 tools/layout/tests/run.py && python3 tools/examples/tests/run.py && python3 tools/vuln/tests/run.py"
step misra-lint python3 -I tools/lint/misra_lint.py --root . --ci-tools "$CI_TOOLS_DIR" --strict --strict-precedence \
  --void-casts $(library)
step eval-order bash -c "python3 -I tools/lint/eval_order_check.py --root . \$(git ls-files -co --exclude-standard -- \"$LIB/*/tests/*.cpp\" \"$LIB/*/fuzz/*.cpp\" \"$HOST_PORT/*.cpp\" \"$EXAMPLES/*.cpp\")"
step recursion python3 -I tools/lint/recursion_check.py --root . $(library)
if [ -f "$CI_TOOLS_DIR/tools/handbook_check/handbook_check.py" ] && [ -x "$VENV/bin/python" ]; then
  step handbook "$VENV/bin/python" "$CI_TOOLS_DIR/tools/handbook_check/handbook_check.py" --repo .
else skipped+=("handbook"); fi
if [ -x "$VENV/bin/yamllint" ]; then step yamllint "$VENV/bin/yamllint" -c _config/.yamllint .github/workflows docs/.nav.yml \
  deviations catalog "$DRIVERS"; else skipped+=("yamllint"); fi
if have npx; then step markdownlint npx --yes markdownlint-cli2@0.18.1 --config _config/.markdownlint.json \
  "**/*.md" "#$DRIVERS" "#third_party" "#node_modules" "#$PORTS/esp32/examples/scripts" "#**/node_modules"; else skipped+=("markdownlint"); fi
step figure-policy python3 -I tools/docs/check_figures.py .
if [ -x "$VENV/bin/kokeb-docs" ]; then KOKEB_DOCS_CONFIG=_config/mkdocs.yml step figures "$VENV/bin/kokeb-docs" check
else skipped+=("figures"); fi
if [ -x "$VENV/bin/mkdocs" ]; then step mkdocs "$VENV/bin/mkdocs" build -f _config/mkdocs.yml --strict -d "$work/site"
else skipped+=("mkdocs"); fi

probe() { perl -e 'alarm 20; exec @ARGV' docker info >/dev/null 2>&1; }
if [ "$container" = 1 ] && ! probe; then
  echo "docker does not answer within 20 s: restart Docker Desktop and rerun"; fails+=("docker"); container=0
fi
if [ "$container" = 1 ]; then
  step gates docker run --rm -v "$root:/src:ro" -e FUZZ_SECONDS="${FUZZ_SECONDS:-20}" -e JOBS="$JOBS" kokeb-ci:local bash -c \
    'cp -a /src /tmp/w && cd /tmp/w && tools/preflight/gates.sh /tmp/w'
  step cross docker run --rm -v "$root:/src:ro" kokeb-ci:local bash -c '
    cp -a /src /tmp/w && cd /tmp/w && for core in "cortex-m0plus -mthumb -mfloat-abi=soft" \
      "cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16" "cortex-m7 -mthumb -mfloat-abi=hard -mfpu=fpv5-d16"; do
      printf "set(CMAKE_SYSTEM_NAME Generic)\nset(CMAKE_CXX_COMPILER arm-none-eabi-g++)\nset(CMAKE_C_COMPILER arm-none-eabi-gcc)\nset(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)\nset(CMAKE_CXX_FLAGS_INIT \"-mcpu=$core -ffunction-sections -fdata-sections\")\n" >/tmp/tc.cmake
      rm -rf /tmp/x && cmake -S . -B /tmp/x -G Ninja -DCMAKE_BUILD_TYPE=MinSizeRel -DCMAKE_TOOLCHAIN_FILE=/tmp/tc.cmake \
        -DKB_MCU=HOST -DKB_RTOS=NONE >/dev/null && cmake --build /tmp/x --target kb_parts_cross || exit 1
      rm -rf /tmp/x && cmake -S . -B /tmp/x -G Ninja -DCMAKE_TOOLCHAIN_FILE=/tmp/tc.cmake -DKB_BUILD_TESTS=ON >/dev/null &&
        cmake --build /tmp/x --target kb_parts_cross || exit 1
    done && tools/preflight/threadx_cross.sh /tmp/w'
  step threadx-linux docker run --rm -v "$root:/src:ro" kokeb-ci:local bash -c 'cp -a /src /tmp/w && cd /tmp/w &&
    for hz in 1000 100; do rm -rf /tmp/t && cmake -S . -B /tmp/t -G Ninja -DCMAKE_CXX_COMPILER=g++-14 \
      -DCMAKE_C_COMPILER=gcc-14 -DKB_PARTS=rtos -DKB_BUILD_TESTS=ON -DKB_THREADX_HOST_TESTS=ON \
      -DKB_THREADX_TICK_HZ=$hz >/dev/null && cmake --build /tmp/t --target kb_rtos_tests_threadx_linux &&
      echo "tick $hz Hz" && timeout 300 /tmp/t/components/lib/rtos/tests/kb_rtos_tests_threadx_linux || exit 1; done'
  # As the "Repository checks" job (ru-repo-checks, strict): style included; a finding of any severity but
  # information fails. Compile-fail tests are wrong on purpose and are left out, as in ci-repo-checks.yml.
  step cppcheck docker run --rm -v "$root:/src:ro" kokeb-ci:local bash -c 'cd /src &&
    ex=$(sed -n "/cppcheck_exclude:/,/cppcheck_std:/p" .github/workflows/ci-repo-checks.yml | grep -o "components/[^ ]*") &&
    args=$(for e in $ex; do printf -- "-i %s " "$e"; done) &&
    o=$(cppcheck --enable=warning,style,performance,portability,information --suppress=missingIncludeSystem \
      --suppress=unmatchedSuppression --inline-suppr --std=c++20 $args \
      --template="{file}:{line}: {severity}: {id}: {message}" -q "'"$LIB"'" 2>&1) ;
    echo "$o" | grep -E ": (error|warning|style|performance|portability):" ; [ -z "$(echo "$o" | grep -E ": (error|warning|style|performance|portability):")" ]'
  step lizard docker run --rm -v "$root:/src:ro" kokeb-ci:local bash -c 'cd /src && p=$(sed -n "s/^ *library_paths: *\"\(.*\)\"/\1/p" .github/workflows/ci-quality.yml) &&
    files=$(find $p -type f \( -name "*.hpp" -o -name "*.cpp" -o -name "*.ipp" \)) &&
    o=$(lizard -l cpp -m -w -C 15 -L 60 -a 6 $files) ; echo "$o"; [ -z "$(echo "$o" | grep ": warning: ")" ]'
  if [ "$misra" = 1 ]; then
    step misra docker run --rm --platform linux/amd64 -v "$root:/src:ro" "${MISRA_IMAGE:-kokeb-ci-misra:v1.19}" bash -c \
      'cp -a /src /tmp/w && cd /tmp/w && bash tools/preflight/misra.sh /tmp/w'
  else skipped+=("misra(--no-misra)"); fi
else
  skipped+=("container gates (--container)")
fi
echo "== ${#fails[@]} failure(s)${fails[*]:+: ${fails[*]}}${skipped[*]:+; skipped: ${skipped[*]}}; logs: $work"
[ ${#fails[@]} -eq 0 ]
