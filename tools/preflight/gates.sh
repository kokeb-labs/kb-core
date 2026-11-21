#!/usr/bin/env bash
# The CI gates of ci-quality.yml that need Linux, on a copy of the checkout inside the CI container image
# (kokeb-labs driver-template tools/ci-container/Dockerfile: GCC 14, gcovr, clang 18): coverage of the library code
# (GCC --coverage with condition coverage, gcovr), the tests under ASan and UBSan, and every libFuzzer target.
#
#   tools/preflight/gates.sh [SRC]          inside the image; SRC defaults to /src
#   FUZZ_SECONDS=20 (per target), JOBS (default: every core), KB_LIBRARY_PATHS (default: ci-quality.yml's library_paths)
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
set -uo pipefail
cd "${1:-/src}" || exit 2
paths="${KB_LIBRARY_PATHS:-$(sed -n 's/^ *library_paths: *"\(.*\)"/\1/p' .github/workflows/ci-quality.yml)}"
jobs="${JOBS:-$(nproc)}"
out=/tmp/gates; rm -rf "$out"; mkdir -p "$out"; fails=()
run() { local n="$1"; shift; if "$@" >"$out/$n.log" 2>&1; then echo "PASS $n"; else echo "FAIL $n"; tail -30 "$out/$n.log" | sed 's/^/     /'; fails+=("$n"); fi; }
cov() {
  local f="--coverage -O0 -g -fcondition-coverage"
  cmake -S . -B "$out/cov" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=g++-14 -DCMAKE_CXX_FLAGS="$f" \
    -DCMAKE_EXE_LINKER_FLAGS=--coverage -DKB_BUILD_TESTS=ON &&
  cmake --build "$out/cov" --parallel "$jobs" && ctest --test-dir "$out/cov" --output-on-failure --timeout 300 --parallel "$jobs" || return 1
  mapfile -t d < <(find "$out/cov" -name '*.gcda' -printf '%h\n' | sort -u)
  local filters=(); for p in $paths; do filters+=(--filter "^${p%/}/"); done
  gcovr --root . --gcov-executable gcov-14 "${filters[@]}" --exclude-unreachable-branches --exclude-throw-branches \
    --merge-lines --txt "$out/coverage.txt" --txt-metric branch "${d[@]}" >/dev/null &&
  gcovr --root . --gcov-executable gcov-14 "${filters[@]}" --exclude-unreachable-branches --exclude-throw-branches \
    --merge-lines --txt "$out/coverage-lines.txt" "${d[@]}" >/dev/null &&
  gcovr --root . --gcov-executable gcov-14 "${filters[@]}" --exclude-unreachable-branches --exclude-throw-branches \
    --merge-lines --txt "$out/coverage-conditions.txt" --txt-metric decision "${d[@]}" >/dev/null 2>&1
  gcovr --root . --gcov-executable gcov-14 "${filters[@]}" --exclude-unreachable-branches --exclude-throw-branches \
    --merge-lines --json-summary "$out/summary.json" "${d[@]}" >/dev/null
  cat "$out/coverage.txt"
}
san() {
  local f="-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -O1 -g"
  cmake -S . -B "$out/san" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=clang++-18 -DCMAKE_CXX_FLAGS="$f" \
    -DCMAKE_EXE_LINKER_FLAGS="$f" -DKB_BUILD_TESTS=ON && cmake --build "$out/san" --parallel "$jobs" &&
  ctest --test-dir "$out/san" --output-on-failure --timeout 300 --parallel "$jobs"
}
fuzz() {
  cmake -S . -B "$out/fz" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=clang++-18 -DKB_BUILD_TESTS=ON \
    -DKB_BUILD_FUZZERS=ON && cmake --build "$out/fz" --parallel "$jobs" || return 1
  local t n seeds; while IFS= read -r t; do n="$(basename "$t")"; mkdir -p "$out/corpus/$n"
    seeds="$(find "$(python3 -I tools/layout/layout.py get lib)" -type d -path "*/fuzz/corpus/$n" | head -1)"
    "$t" -max_total_time="${FUZZ_SECONDS:-20}" -artifact_prefix="$out/crash-$n-" "$out/corpus/$n" ${seeds:+"$seeds"} || return 1
  done < <(find "$out/fz" -type f -perm -u+x -name 'fuzz_*' ! -name 'fuzz_replay_*' | sort)
}
run coverage cov; [ -f "$out/summary.json" ] && python3 -c "
import json; s=json.load(open('$out/summary.json'))
print('coverage: lines %s%%, branches %s%%, conditions %s%%' % (s.get('line_percent'), s.get('branch_percent'), s.get('condition_percent', '-')))"
run sanitizers san
run fuzz fuzz
line="== ${#fails[@]} gate failure(s)${fails[*]:+: ${fails[*]}} (logs: $out)"
echo "$line"; [ -n "${PREFLIGHT_OUT:-}" ] && { mkdir -p "$PREFLIGHT_OUT"; cp "$out"/*.txt "$out"/*.json "$PREFLIGHT_OUT"/ 2>/dev/null; echo "$line" >"$PREFLIGHT_OUT/gates.line"; }
[ ${#fails[@]} -eq 0 ]
