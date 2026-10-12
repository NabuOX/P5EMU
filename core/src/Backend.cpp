// SPDX-License-Identifier: Apache-2.0
//
// P5EMU is an independent, open-source project. See LICENSE and
// PROVENANCE.md before adding code here.

#include "p5emu/cpu/Backend.h"

#include "p5emu/cpu/IrBuilder.h"

namespace p5emu::cpu {
namespace {

// ---------------------------------------------------------------------------
// ReferenceInterpreter adapter
// ---------------------------------------------------------------------------
class ReferenceInterpreterBackend final : public Backend {
 public:
  ReferenceInterpreterBackend(std::uint64_t step_limit,
                              ExternalCallHandler* external_calls)
      : step_limit_(step_limit),
        interpreter_(step_limit, external_calls) {}

  [[nodiscard]] BackendId id() const noexcept override {
    return BackendId::kReferenceInterpreter;
  }
  [[nodiscard]] const char* name() const noexcept override {
    return "reference-interpreter";
  }
  [[nodiscard]] std::uint64_t step_limit() const noexcept override {
    return step_limit_;
  }

  ExecutionResult Execute(const ir::Program& program, CpuState* state,
                          memory::GuestMemoryPort& memory) override {
    return interpreter_.Execute(program, state, memory);
  }

 private:
  std::uint64_t step_limit_;
  ReferenceInterpreter interpreter_;
};

// ---------------------------------------------------------------------------
// BuggyTestOnlyBackend -- THE DELIBERATE DIVERGENCE
// ---------------------------------------------------------------------------
//
// WHAT IT DOES, EXACTLY:
//
// It executes P5EMU IR using the same semantics as the reference, with ONE
// difference, applied at exactly one point:
//
//     The ZERO FLAG (ZF) produced by kAddFlags is cleared.
//
// That is: where the reference sets ZF when the 64-bit add result is zero, this
// backend does not. Everything else -- OF, SF, CF, PF, AF, the result value, the
// memory writes, the control flow -- is identical.
//
// WHY THIS SPECIFIC BUG
//
//   * It is a REAL semantic divergence, not a forced assertion. The backend runs
//     to completion and returns a plausible result. Nothing crashes.
//   * It is INVISIBLE to a value-only comparison. If the harness compared only
//     the register that received the sum, it would PASS. Only a comparison that
//     includes EFLAGS -- which the real x86 architecture requires, because ZF is
//     architecturally observable through JZ/JE -- catches it.
//   * It therefore tests the harness's ability to compare FLAGS, which is the
//     part of differential testing most likely to be got wrong. A memcmp-based
//     or value-only harness would miss it entirely.
//
// WHY IT IS GATED
//
// This backend must never appear in ordinary verification runs, because a
// knowingly-wrong backend in a normal test run trains everyone to ignore it.
// It is constructed only by MakeBuggyTestOnlyBackend, which is called only by
// the dedicated gate test. MakeReferenceInterpreter -- the function every normal
// path uses -- cannot return it.
//
// REMOVAL CONDITION: this backend is DELETED, not disabled, once the ARM64
// backend exists and the harness has been shown to catch a divergence in a
// backend that is itself non-trivial. At that point a deliberately-buggy
// throwaway backend is strictly worse than a real one.
class BuggyTestOnlyBackend final : public Backend {
 public:
  explicit BuggyTestOnlyBackend(std::uint64_t step_limit)
      : step_limit_(step_limit) {}

  [[nodiscard]] BackendId id() const noexcept override {
    return BackendId::kBuggyTestOnly;
  }
  [[nodiscard]] const char* name() const noexcept override {
    return "buggy-test-only(zf-cleared-on-add)";
  }
  [[nodiscard]] std::uint64_t step_limit() const noexcept override {
    return step_limit_;
  }

  ExecutionResult Execute(const ir::Program& program, CpuState* state,
                          memory::GuestMemoryPort& memory) override;

 private:
  std::uint64_t step_limit_;
};

ExecutionResult BuggyTestOnlyBackend::Execute(const ir::Program& program,
                                              CpuState* state,
                                              memory::GuestMemoryPort& memory) {
  // Strategy: rewrite the IR so that the kAddFlags op produces a flags value
  // with ZF masked off, then execute it with a plain interpreter.
  //
  // Rewriting the IR (rather than reimplementing the interpreter) keeps the two
  // backends on the SAME execution engine. That is deliberate: it means any
  // divergence observed is attributable to the injected difference alone, and
  // not to two independent reimplementations drifting apart.
  //
  // The rewrite is: for every kAddFlags, replace it with a sequence that
  // computes the same sum but clears ZF in the flags value. Because P5EMU IR has
  // no "add then fix flags" op, the rewrite is done by post-processing the
  // interpreter's flags write: the buggy backend executes the program normally,
  // then clears ZF in EFLAGS if the program contained an add whose result was
  // zero. That is a faithful simulation of "backend forgets to set ZF".
  ReferenceInterpreter interpreter(step_limit_);
  ExecutionResult result = interpreter.Execute(program, state, memory);

  // Inject the bug only if the program actually performed an add. A program
  // with no add must behave identically, so the harness comparison stays
  // meaningful for non-add fixtures.
  bool has_add = false;
  for (const ir::Block& block : program.blocks) {
    for (const ir::Op& op : block.ops) {
      if (op.kind == ir::OpKind::kAddFlags) {
        has_add = true;
        break;
      }
    }
    if (has_add) {
      break;
    }
  }

  if (has_add && result.reason != TerminationReason::kIrInvalid) {
    state->eflags &= ~eflags::kZf;
  }

  return result;
}

}  // namespace

const char* BackendIdName(BackendId id) noexcept {
  switch (id) {
    case BackendId::kReferenceInterpreter:
      return "reference-interpreter";
    case BackendId::kBuggyTestOnly:
      return "buggy-test-only";
    case BackendId::kArm64:
      return "arm64";
    case BackendId::kUnimplementedTestOnly:
      return "unimplemented-test-only";
  }
  return "unknown";
}

std::unique_ptr<Backend> MakeReferenceInterpreter(
    std::uint64_t step_limit, ExternalCallHandler* external_calls) {
  return std::make_unique<ReferenceInterpreterBackend>(step_limit,
                                                       external_calls);
}

std::unique_ptr<Backend> MakeBuggyTestOnlyBackend(std::uint64_t step_limit) {
  return std::make_unique<BuggyTestOnlyBackend>(step_limit);
}

namespace {
// The always-unimplemented test-only backend. See Backend.h.
class UnimplementedTestOnlyBackend final : public Backend {
 public:
  explicit UnimplementedTestOnlyBackend(std::uint64_t step_limit)
      : step_limit_(step_limit) {}

  [[nodiscard]] BackendId id() const noexcept override {
    return BackendId::kUnimplementedTestOnly;
  }
  [[nodiscard]] const char* name() const noexcept override {
    return "unimplemented-test-only";
  }
  [[nodiscard]] std::uint64_t step_limit() const noexcept override {
    return step_limit_;
  }

  ExecutionResult Execute(const ir::Program& program, CpuState* state,
                          memory::GuestMemoryPort& memory) override {
    (void)program;
    (void)memory;
    ExecutionResult result;
    result.status = StatusCode::kUnimplementedInstruction;
    result.reason = TerminationReason::kRunning;
    result.final_rip = (state != nullptr) ? state->rip : 0;
    return result;
  }

 private:
  std::uint64_t step_limit_;
};
}  // namespace

std::unique_ptr<Backend> MakeUnimplementedTestOnlyBackend(
    std::uint64_t step_limit) {
  return std::make_unique<UnimplementedTestOnlyBackend>(step_limit);
}

}  // namespace p5emu::cpu
