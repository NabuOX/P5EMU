// SPDX-License-Identifier: Apache-2.0
//
// P5EMU is an independent, open-source project. See LICENSE and
// PROVENANCE.md before adding code here.
//
// ============================================================================
// AArch64 BACKEND
// ============================================================================
//
// The first real execution backend behind the Phase-1 Backend contract. It
// consumes the SAME validated P5EMU IR the ReferenceInterpreter executes, and
// returns state in the same comparable form, so the differential harness can
// compare it without any ARM64-specific logic.
//
// It generates AArch64, then executes it through an Arm64Runner. On the x86-64
// development host the only runner is the external qemu-aarch64 one, so the
// instruction semantics come from an independent implementation of the AArch64
// ISA.
//
// HONESTY RULES BAKED IN HERE:
//
//   * IR outside the Phase-2 subset makes code generation return
//     kUnimplementedInstruction, and the backend reports a COVERAGE GAP. It
//     never falls back to interpreting the program, which would make a
//     differential "agreement" meaningless.
//   * No runner configured, or the runner cannot run, is likewise a coverage
//     gap, never a fabricated success.
// ============================================================================

#ifndef P5EMU_CPU_ARM64_BACKEND_H
#define P5EMU_CPU_ARM64_BACKEND_H

#include <cstdint>
#include <memory>

#include "p5emu/cpu/Arm64CodeGen.h"
#include "p5emu/cpu/Backend.h"
#include "p5emu/cpu/Arm64Runner.h"

namespace p5emu::cpu {

// Builds an AArch64 backend.
//
// `runner` is borrowed and must outlive the backend. Passing nullptr builds a
// backend that generates code but cannot execute it; Execute then reports a
// coverage gap. This is what makes the backend usable (and its codegen testable)
// even where no AArch64 runner exists.
std::unique_ptr<Backend> MakeArm64Backend(std::uint64_t step_limit,
                                          Arm64Runner* runner,
                                          Arm64CodeGenOptions options = {});

}  // namespace p5emu::cpu

#endif  // P5EMU_CPU_ARM64_BACKEND_H
