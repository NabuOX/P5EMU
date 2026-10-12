#!/usr/bin/env bash
# Builds the AArch64 execution harness for the independent execution oracle.
#
# TEST INFRASTRUCTURE ONLY. The harness is a separate program, cross-compiled
# for AArch64 and run under an external AArch64 emulator (qemu-aarch64). It is
# not linked into P5EMU and p5emu_core does not depend on it. See PROVENANCE.md.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT_DIR="${1:-${ROOT}/build-aarch64-harness}"
mkdir -p "${OUT_DIR}"

CROSS_CC="${P5EMU_A64_CROSS_CC:-aarch64-linux-gnu-gcc}"
if ! command -v "${CROSS_CC}" >/dev/null 2>&1; then
  echo "cross compiler not found: ${CROSS_CC}" >&2
  exit 1
fi

"${CROSS_CC}" -static -O2 -Wall -Wextra -Werror \
  -I"${ROOT}/core/include" \
  "${ROOT}/aarch64/harness/p5emu_a64_harness.c" \
  -o "${OUT_DIR}/p5emu_a64_harness"

echo "${OUT_DIR}/p5emu_a64_harness"
