// SPDX-License-Identifier: Apache-2.0
//
// P5EMU is an independent, open-source project. See LICENSE and
// PROVENANCE.md before adding code here.
//
// ============================================================================
// EXECUTION BACKEND CONTRACT
// ============================================================================
//
// THE POINT OF THIS FILE
//
// P5EMU's correctness argument is that every backend, given the same validated
// IR and the same initial state, produces the same result. That argument is
// worthless unless the harness can actually CHECK it. This file defines the
// contract, the registry, and the comparison; Differential.cpp runs it.
//
// A backend:
//   * receives validated IR and an independent CpuState copy
//   * receives an independent GuestMemoryPort view
//   * MUST NOT see or modify the reference backend's state or memory
//   * MUST report honest Status, including kUnimplementedInstruction
//   * MUST NOT see any x86-64 encoding. It sees IR only.
//
// The isolation requirement is what makes the comparison meaningful: a backend
// that could observe the other's memory would "pass" by copying it.
// ============================================================================

#ifndef P5EMU_CPU_BACKEND_H
#define P5EMU_CPU_BACKEND_H

#include <memory>
#include <string>
#include <vector>

#include "p5emu/core/Status.h"
#include "p5emu/cpu/ExternalCall.h"
#include "p5emu/cpu/GuestMemory.h"
#include "p5emu/cpu/Ir.h"
#include "p5emu/cpu/ReferenceInterpreter.h"

namespace p5emu::cpu {

// Identifies a backend. Stable strings: they appear in differential failure
// output and in coverage reports.
enum class BackendId : std::uint8_t {
  kReferenceInterpreter = 0,
  kBuggyTestOnly,  // see Differential.cpp. Test-only. See the gate note there.
  kArm64,          // the Phase-2 AArch64 backend (real, not test-only)
  kUnimplementedTestOnly,  // always reports kUnimplementedInstruction
};

const char* BackendIdName(BackendId id) noexcept;

// A backend executes validated P5EMU IR.
class Backend {
 public:
  virtual ~Backend() = default;

  [[nodiscard]] virtual BackendId id() const noexcept = 0;
  [[nodiscard]] virtual const char* name() const noexcept = 0;

  // Executes `program`. `state` is in/out. `memory` is mutated by stores.
  //
  // Contract:
  //   * `program` MUST already pass ValidateIr. A backend that trusts the
  //     caller without checking is acceptable; a backend that crashes is not.
  //   * On kUnimplementedInstruction, the implementation must report it and must
  //     not pretend to have executed correctly.
  //   * Must not read or write any host memory outside `memory`.
  virtual ExecutionResult Execute(const ir::Program& program, CpuState* state,
                                  memory::GuestMemoryPort& memory) = 0;

  // Default step limit for this backend, so a runaway program terminates
  // deterministically on every backend and the comparison stays well-defined.
  [[nodiscard]] virtual std::uint64_t step_limit() const noexcept = 0;
};

// The reference backend. Always available.
//
// `external_calls` handles kExternalCall terminators (HLE imports). Passing
// nullptr makes an external call terminate with kHleError; it is never treated
// as success.
std::unique_ptr<Backend> MakeReferenceInterpreter(std::uint64_t step_limit,
                                                  ExternalCallHandler* external_calls = nullptr);

// A TEST-ONLY backend that is correct except for one deliberately injected
// semantic divergence.
//
// This exists to prove the differential harness can DETECT divergence. A harness
// never observed to fail is indistinguishable from a harness that cannot fail.
// See docs/architecture/differential-harness.md.
std::unique_ptr<Backend> MakeBuggyTestOnlyBackend(std::uint64_t step_limit);

// A TEST-ONLY backend that always reports kUnimplementedInstruction without
// executing. It exists so the differential harness's coverage-gap handling can
// be tested directly, without depending on some IR feature happening to be
// unsupported. It must never appear in ordinary verification runs.
std::unique_ptr<Backend> MakeUnimplementedTestOnlyBackend(std::uint64_t step_limit);

}  // namespace p5emu::cpu

#endif  // P5EMU_CPU_BACKEND_H
