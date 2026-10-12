// SPDX-License-Identifier: LicenseRef-P5EMU-Proprietary
//
// P5EMU source is independently authored and proprietary. See LICENSE.md and
// PROVENANCE.md before adding code here.
//
// ============================================================================
// THE CENTRAL ARCHITECTURAL CONTRACT
// ============================================================================
//
// This header used to declare the Phase-0 placeholder types (CpuState,
// GuestMemoryPort, Decoder, ExecutionBackend). Phase 1 replaced all of them with
// real implementations, and keeping the placeholders would have left two
// competing definitions of the same concepts -- which is precisely the kind of
// drift that makes an architecture document worth less than the code.
//
// The authoritative declarations now live in:
//
//   GuestMemory.h          guest address space; the memory port contract
//   X64Decoder.h           guest bytes -> a decoded instruction description
//   X64Encode.h            canonical re-encoding, for round-trip testing
//   Ir.h                   P5EMU SSA IR, and ValidateIr
//   IrBuilder.h            IR construction
//   TranslateToIr.h        decode -> IR (x86 SEMANTICS live here)
//   ReferenceInterpreter.h machine state; the reference execution engine
//   Backend.h              the backend contract future backends implement
//   Differential.h         cross-backend comparison
//
// The pipeline, and the separation that makes it trustworthy:
//
//   guest bytes
//        |
//        v
//   X64Decoder            knows x86-64 ENCODING ONLY
//        |                (prefixes, opcode, ModRM, SIB, displacement)
//        v
//   TranslateToIr         knows x86-64 SEMANTICS ONLY
//        |                (what MOV does, which flags ADD sets, when a 32-bit
//        |                 write zero-extends to 64)
//        v
//   Ir::Program           P5EMU SSA IR -- the shared contract
//        |
//        v
//   Ir::ValidateIr        rejects malformed IR BEFORE any backend sees it
//        |
//        +--> ReferenceInterpreter ----+
//        |                             |  must agree on every field
//        +--> Arm64Backend (Phase 2) --+
//
// HARD RULES, and why each is load-bearing:
//
//  1. The interpreter executes IR only. It cannot see a decoded instruction.
//     A backend that can also see the encoding may implement an encoding quirk
//     instead of the IR's semantics, and the differential comparison then
//     proves nothing.
//
//  2. Guest memory is reached ONLY through GuestMemoryPort. Guest VA == host VA
//     is an OPTIMISATION a port may advertise, never an assumption the CPU
//     core depends on. See GuestMemory.h.
//
//  3. ValidateIr runs before execution, always, in every build. A malformed IR
//     reaching a native-code backend is memory corruption, not a graceful
//     failure. The interpreter also validates, so that it and any future backend
//     agree on what "valid" means.
//
//  4. The interpreter is always available, on every host, in every build. On
//     iOS it is the ONLY permitted execution tier, because a JIT cannot ship
//     through the App Store. It is not a debug mode.
//
//  5. Unknown is a valid state. Unsupported encodings and instructions return
//     kUnimplementedInstruction. Nothing ever guesses.
//
//  6. State is compared field by field, never with memcmp. A memcmp would pass
//     with a wrong EFLAGS.
//
// This header exists to state those rules in one place and to include the real
// declarations, so that a reader arriving at "the CPU model" finds the contract
// rather than a stub.
// ============================================================================

#ifndef P5EMU_CPU_CPU_MODEL_H
#define P5EMU_CPU_CPU_MODEL_H

#include "p5emu/cpu/Backend.h"
#include "p5emu/cpu/Differential.h"
#include "p5emu/cpu/GuestMemory.h"
#include "p5emu/cpu/Ir.h"
#include "p5emu/cpu/IrBuilder.h"
#include "p5emu/cpu/ReferenceInterpreter.h"
#include "p5emu/cpu/TranslateToIr.h"
#include "p5emu/cpu/X64Decoder.h"
#include "p5emu/cpu/X64Encode.h"

namespace p5emu::cpu {

// Host architectures P5EMU's execution layer targets.
//
// `kReference` is always available. `kArm64` is the performance backend and
// does not exist yet; it is declared so that reports and coverage accounting can
// name it as a planned gap rather than omitting it silently.
enum class ExecutionTier : std::uint8_t {
  kReferenceInterpreter = 0,
  kArm64,
};

const char* ExecutionTierName(ExecutionTier tier) noexcept;

}  // namespace p5emu::cpu

#endif  // P5EMU_CPU_CPU_MODEL_H
