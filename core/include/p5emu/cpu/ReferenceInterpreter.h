// SPDX-License-Identifier: Apache-2.0
//
// P5EMU is an independent, open-source project. See LICENSE and
// PROVENANCE.md before adding code here.
//
// ============================================================================
// ReferenceInterpreter
// ============================================================================
//
// THIS IS THE SPECIFICATION.
//
// P5EMU's entire correctness argument is differential: the reference interpreter
// defines what correct means, and every other backend must agree with it. That
// argument only holds if the interpreter is:
//
//   1. available everywhere, always, in every configuration. It is not a debug
//      build and it is not a fallback: on iOS it is the ONLY permitted
//      execution tier, because a JIT cannot ship through the App Store.
//   2. obviously correct. Readability beats cleverness, always.
//   3. deterministic. Same IR + same initial state => same result, always.
//
// Accordingly this file optimises for being *easy to check against the Intel
// SDM*, not for speed. Every flag computation is written the long way, with the
// architectural rule named in a comment, because a fast-but-subtly-different
// flag rule is the single most expensive defect P5EMU could ship.
//
// It executes P5EMU IR only. It has no knowledge of x86-64 encodings and cannot
// execute a DecodedInstruction. That separation is architectural, not stylistic:
// a backend that can see the encoding can accidentally implement encoding
// quirks rather than the IR's semantics, and the differential comparison then
// proves nothing.
//
// Execution is bounded by an explicit step budget. A runaway guest program must
// terminate deterministically with kExecutionLimit, never hang and never
// exhaust the host.
// ============================================================================

#ifndef P5EMU_CPU_REFERENCE_INTERPRETER_H
#define P5EMU_CPU_REFERENCE_INTERPRETER_H

#include <cstdint>

#include "p5emu/core/Status.h"
#include "p5emu/cpu/ExternalCall.h"
#include "p5emu/cpu/GuestMemory.h"
#include "p5emu/cpu/Ir.h"

namespace p5emu::cpu {

// ---------------------------------------------------------------------------
// Machine state
// ---------------------------------------------------------------------------
// The guest architectural state the Phase-1 IR can observe or modify.
//
// Fields are plain, named, and grouped by role. Deliberately NOT a byte-packed
// struct: a packed struct invites memcmp-based comparison in tests, which would
// pass with a wrong EFLAGS. Field-by-field comparison is enforced by
// CompareCpuState() below, and used by the differential harness.
//
// Layout stability is NOT a Phase-1 concern. Phase 2's ARM64 backend will map
// this state into host registers, and the map can change then. Correctness first.
// ---------------------------------------------------------------------------
struct CpuState {
  std::uint64_t rax = 0, rbx = 0, rcx = 0, rdx = 0;
  std::uint64_t rsi = 0, rdi = 0, rbp = 0, rsp = 0;
  std::uint64_t r8 = 0, r9 = 0, r10 = 0, r11 = 0;
  std::uint64_t r12 = 0, r13 = 0, r14 = 0, r15 = 0;

  std::uint64_t rip = 0;
  std::uint64_t eflags = 0;

  // Runaway protection and tracing.
  std::uint64_t executed_ops = 0;
};

// Architectural EFLAGS bit positions. Facts from the Intel SDM Vol. 2, §3.1.2
// "Flags Register". Named, not magic numbers.
namespace eflags {
inline constexpr std::uint64_t kCf = 1u << 0;
inline constexpr std::uint64_t kPf = 1u << 2;
inline constexpr std::uint64_t kAf = 1u << 4;
inline constexpr std::uint64_t kZf = 1u << 6;
inline constexpr std::uint64_t kSf = 1u << 7;
inline constexpr std::uint64_t kOf = 1u << 11;

// Every bit the interpreter can ever set. Reserved and undefined bits are not
// produced, so a comparison over exactly these bits is a comparison over the
// architecturally defined EFLAGS state.
inline constexpr std::uint64_t kDefinedMask =
    kCf | kPf | kAf | kZf | kSf | kOf;

// A comparison over all 64 bits. Used where a fixture wants to observe the whole
// register, including bits that are architecturally reserved.
inline constexpr std::uint64_t kAllMask = ~std::uint64_t{0};
}  // namespace eflags

// How a block ended. Reported so the differential harness can distinguish
// "the same wrong value" from "the same value reached a different way".
enum class TerminationReason : std::uint8_t {
  kRunning = 0,
  kFallthrough,
  kReturn,
  kIndirectJump,
  kTrap,
  kStepLimit,
  kFault,
  kIrInvalid,
  // An external call (HLE import) could not be dispatched, or the host
  // implementation returned a failure. Distinct from a guest trap.
  kHleError,
};

const char* TerminationReasonName(TerminationReason reason) noexcept;

struct ExecutionResult {
  Status status = StatusCode::kOk;
  TerminationReason reason = TerminationReason::kRunning;
  std::uint64_t executed_ops = 0;
  memory::GuestAddr final_rip = 0;
};

class ReferenceInterpreter {
 public:
  // Steps budget. Reaching it terminates with kExecutionLimit. A finite default
  // is required: an emulator with an unbounded loop is a hang, and a hang is
  // not a diagnosable failure.
  static constexpr std::uint64_t kDefaultStepLimit = 1u << 20;

  explicit ReferenceInterpreter(std::uint64_t step_limit = kDefaultStepLimit,
                                ExternalCallHandler* external_calls = nullptr)
      : step_limit_(step_limit), external_calls_(external_calls) {}

  // Executes `program` starting from `entry`, mutating `state` and `memory`.
  //
  // `state` is an in/out parameter: the caller's initial state is the initial
  // state, and the final state is written back. `memory` is mutated by stores.
  //
  // The IR is validated BEFORE execution and again on every entry, so that the
  // interpreter and any future backend agree on what valid means. Validation
  // failure returns kInvalidIr and does not execute anything.
  ExecutionResult Execute(const ir::Program& program, CpuState* state,
                         memory::GuestMemoryPort& memory);

  [[nodiscard]] std::uint64_t step_limit() const noexcept { return step_limit_; }

 private:
  std::uint64_t step_limit_;
  ExternalCallHandler* external_calls_ = nullptr;
};

// ---------------------------------------------------------------------------
// State comparison, for differential testing
// ---------------------------------------------------------------------------
//
// Field-by-field, never memcmp. Returns true when equal. On mismatch, writes a
// human-readable description of the FIRST difference into `detail`.
//
// Signature chosen so the differential harness can compare two CpuState values
// without knowing anything about their layout.
//
// `eflags_mask` selects which EFLAGS bits are OBSERVABLE for this comparison.
// Bits outside the mask are ignored on both sides. This exists because some x86
// flags are architecturally UNDEFINED for certain instructions (for example OF
// after a shift by more than one): the interpreter necessarily leaves SOME bits
// there, but requiring two backends to agree on invented values would be
// requiring them to agree on behaviour the ISA does not specify. The default
// observes every bit, so nothing is masked unless a caller asks for it.
struct StateDifference {
  const char* field = nullptr;   // static string, e.g. "rax"
  std::uint64_t expected = 0;
  std::uint64_t actual = 0;
};

bool CompareCpuState(const CpuState& expected, const CpuState& actual,
                     StateDifference* difference,
                     std::uint64_t eflags_mask = eflags::kAllMask);

// Compares a declared memory window. `bytes` is the window length. Returns true
// when the windows are byte-identical.
bool CompareMemoryWindow(memory::GuestMemoryPort& expected,
                         memory::GuestMemoryPort& actual,
                         memory::GuestAddr addr, std::size_t bytes,
                         StateDifference* difference);

}  // namespace p5emu::cpu

#endif  // P5EMU_CPU_REFERENCE_INTERPRETER_H
