// SPDX-License-Identifier: Apache-2.0
//
// P5EMU is an independent, open-source project. See LICENSE and
// PROVENANCE.md before adding code here.
//
// ============================================================================
// AArch64 CODE GENERATOR
// ============================================================================
//
// Lowers validated P5EMU IR to a single AArch64 function. It consumes the SAME
// IR the ReferenceInterpreter executes and defines no semantics of its own: it
// either translates an IR op to AArch64 with the identical effect, or it fails
// with kUnimplementedInstruction. There is deliberately NO fallback path that
// quietly produces a plausible result -- a backend that silently degrades would
// make differential agreement meaningless.
//
// The generated function follows core/include/p5emu/cpu/Arm64Abi.h.
//
// STRATEGY (Phase 2, correctness first)
//
//   * No register allocator. Every IR value lives in a 16-byte stack slot.
//   * Guest GPRs live in the context block, loaded/stored per access.
//   * Guest memory goes through helper calls. Guest VA == host VA is never
//     assumed.
//   * Flags are synthesised bit-by-bit; AArch64 NZCV is used only as a local
//     convenience inside one arithmetic sequence.
//
// This is slow on purpose. See docs/architecture/arm64-backend.md.
// ============================================================================

#ifndef P5EMU_CPU_ARM64_CODEGEN_H
#define P5EMU_CPU_ARM64_CODEGEN_H

#include <cstdint>
#include <string>
#include <vector>

#include "p5emu/core/Status.h"
#include "p5emu/cpu/Ir.h"

namespace p5emu::cpu {

struct Arm64CodeGenOptions {
  // TEST-ONLY. Emits ADD where the IR says subtract, or vice versa, to prove the
  // differential harness can catch a real code-generation defect. Never set in
  // any normal path; the negative-proof test sets it and expects a divergence.
  bool inject_add_sub_swap = false;

  // TEST-ONLY. Clears the ZF bit when synthesising arithmetic flags, modelling a
  // backend that computes flags wrongly. Same purpose as above.
  bool inject_clear_zf = false;

  // TEST-ONLY. A 32-bit register write does NOT zero-extend the upper 32 bits,
  // modelling a backend with a broken partial-register width rule. Only diverges
  // when the written value has bits above 32, so the fixture must supply one.
  bool inject_no_zero_extend = false;

  // TEST-ONLY. Masks a shift count to 6 bits for EVERY operand size, modelling a
  // backend that ignores x86's 5-bit masking for 8/16/32-bit operands.
  bool inject_wide_shift_count = false;
};

struct Arm64CodeGenResult {
  Status status = StatusCode::kOk;
  std::vector<std::uint8_t> code;
  std::string reason;        // on kUnimplementedInstruction: what was unsupported
  std::uint64_t frame_bytes = 0;
  std::uint64_t value_count = 0;
};

// Generates one AArch64 function for `program`.
//
// Returns kUnimplementedInstruction (with `reason` filled in and no code) when
// the program uses an IR feature outside the Phase-2 subset. Returns
// kInvalidArgument for a structurally unusable program.
Status GenerateArm64(const ir::Program& program,
                     const Arm64CodeGenOptions& options,
                     Arm64CodeGenResult* out);

// Convenience: default options.
inline Status GenerateArm64(const ir::Program& program,
                            Arm64CodeGenResult* out) {
  return GenerateArm64(program, Arm64CodeGenOptions{}, out);
}

}  // namespace p5emu::cpu

#endif  // P5EMU_CPU_ARM64_CODEGEN_H
