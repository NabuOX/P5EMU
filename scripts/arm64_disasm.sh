#!/usr/bin/env bash
# Disassembles raw AArch64 machine-code bytes for P5EMU developers.
#
# This is a DIAGNOSTIC aid, not part of the emulator. It wraps an independent
# disassembler so the emitted bytes can be read back as instructions:
#
#   IR dump  (p5emu_tests Arm64Debug. or any codegen caller)
#     -> emitted AArch64 bytes (written to $P5EMU_A64_DUMP_DIR)
#     -> this script
#     -> disassembly
#
# The disassembler is an EXTERNAL tool used only for inspection; it is not
# linked into or distributed with P5EMU. See PROVENANCE.md.
#
# Usage: scripts/arm64_disasm.sh <raw-aarch64.bin>
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "usage: $0 <raw-aarch64.bin>" >&2
  exit 2
fi

INPUT="$1"
if [[ ! -f "${INPUT}" ]]; then
  echo "no such file: ${INPUT}" >&2
  exit 1
fi

if command -v aarch64-linux-gnu-objdump >/dev/null 2>&1; then
  exec aarch64-linux-gnu-objdump -D -b binary -m aarch64 "${INPUT}"
elif command -v llvm-objdump >/dev/null 2>&1; then
  echo "note: using llvm-objdump" >&2
  exec llvm-objdump -D --triple=aarch64 -b binary "${INPUT}"
else
  echo "no AArch64 disassembler found (aarch64-linux-gnu-objdump or llvm-objdump)" >&2
  exit 1
fi
