#!/usr/bin/env bash
# P5EMU local verification.
#
# Usage:
#   scripts/verify.sh                 # configure, build, test (RelWithDebInfo)
#   scripts/verify.sh --asan          # same, under AddressSanitizer
#   scripts/verify.sh --release       # -O2 -DNDEBUG, tests still run
#
# Deliberately strict: warnings-as-errors ON by default. A build that only
# passes with warnings is not a verified build.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_TYPE="RelWithDebInfo"
SANITIZER="none"
EXTRA_CXX_FLAGS=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --asan)    SANITIZER="address"; BUILD_TYPE="RelWithDebInfo"; shift ;;
    --ubsan)   SANITIZER="undefined"; BUILD_TYPE="RelWithDebInfo"; shift ;;
    --tsan)    SANITIZER="thread"; BUILD_TYPE="RelWithDebInfo"; shift ;;
    --release) BUILD_TYPE="Release"; shift ;;
    --debug)   BUILD_TYPE="Debug"; shift ;;
    -h|--help) sed -n '2,12p' "${BASH_SOURCE[0]}"; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done

BUILD_DIR="${ROOT}/build-${BUILD_TYPE,,}${SANITIZER:+-$SANITIZER}"

echo "== P5EMU verify =="
echo "   build type : ${BUILD_TYPE}"
echo "   sanitizer  : ${SANITIZER}"
echo "   build dir  : ${BUILD_DIR}"
echo

cmake -S "${ROOT}" -B "${BUILD_DIR}" -G Ninja \
  -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
  -DP5EMU_SANITIZER="${SANITIZER}" \
  -DP5EMU_WERROR=ON \
  -DP5EMU_BUILD_TESTS=ON \
  -DP5EMU_BUILD_CLI=ON

cmake --build "${BUILD_DIR}" --parallel

echo
echo "== ctest =="
ctest --test-dir "${BUILD_DIR}" --output-on-failure

echo
echo "== cli smoke =="
"${BUILD_DIR}/bin/p5emu-cli"

echo
echo "P5EMU verify: OK"
