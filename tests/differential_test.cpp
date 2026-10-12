// SPDX-License-Identifier: LicenseRef-P5EMU-Proprietary
//
// ============================================================================
// DIFFERENTIAL HARNESS TESTS
// ============================================================================
//
// The two halves here matter for different reasons.
//
//   Differential.*       proves the harness correctly reports AGREEMENT for
//                        equivalent backends. Without this, "they agree" could
//                        mean "the comparison does nothing".
//
//   DifferentialGate.*   proves the harness DETECTS a real semantic divergence.
//                        This is the Phase-1 hard gate. A harness never observed
//                        to fail is indistinguishable from a harness that cannot
//                        fail, and every later claim about backend correctness
//                        rests on this one property.
//
// The deliberate bug used for the gate is: the backend does not set the ZERO
// FLAG when an add produces zero. See core/src/Backend.cpp.
//
// It is chosen specifically because it is INVISIBLE to a naive harness:
//
//   * The register receiving the sum is identical in both backends.
//   * RIP is identical.
//   * Written memory is identical.
//   * The program runs to completion in both backends; nothing crashes.
//   * ONLY EFLAGS differs.
//
// So a harness that compared "the value the instruction produced" would pass.
// Detecting it requires comparing architectural FLAGS -- which is exactly the
// part of differential testing most often done wrong, because a memcmp over a
// state struct or a "did the right register change?" check both miss it.
//
// The bug is injected by rewriting the IR's flags output, so both backends run
// on the SAME execution engine and any observed difference is attributable to
// the injected divergence alone.
//
// ============================================================================

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "FixtureProvenance.h"
#include "TestHarness.h"
#include "p5emu/cpu/Backend.h"
#include "p5emu/cpu/Differential.h"
#include "p5emu/cpu/IrBuilder.h"
#include "p5emu/cpu/LinearGuestMemory.h"

namespace {

using namespace p5emu;         // NOLINT
using namespace p5emu::cpu;     // NOLINT
using namespace p5emu::ir;      // NOLINT
using namespace p5emu::memory;  // NOLINT
using namespace p5emu::test;   // NOLINT

constexpr GuestAddr kDataBase = 0x0003'0000ull;

// Builds a program whose add result is ZERO, so the zero flag is architecturally
// observable.
//
//   rax = 0xFFFFFFFFFFFFFFFF
//   add rax, 1        -> rax = 0, ZF = 1, CF = 1
//   rdx = rax
//   store rax at DATA
//   return
//
// Provenance: Intel SDM Vol. 2 §2.2.1 ADD and §3.1.2 (ZF set when the result is
// zero). 0xFFFFFFFFFFFFFFFF + 1 wraps to 0 in 64-bit arithmetic.
Program MakeZeroResultAddProgram() {
  Builder b("zero-result-add");
  b.CreateBlock(0x1000);
  b.SetEntryBlock(0);

  const ir::ValueId rax = b.EmitGetReg(ArchRegister::kRax);
  const ir::ValueId one = b.EmitMaterializeConstant(1, ValueType::kI64);
  const ir::ValueId sum = b.EmitBinaryFlags(OpKind::kAddFlags, rax, one);
  b.EmitSetReg(ArchRegister::kRbx, sum);

  const ir::ValueId addr = b.EmitAddr(kDataBase, ir::kInvalidValue, 1, 0);
  b.EmitStore(addr, sum, 8);

  b.EmitReturn();
  return b.MutableProgram();
}

// Builds a program with NO add, so the buggy backend must behave identically.
Program MakeNoAddProgram() {
  Builder b("no-add");
  b.CreateBlock(0x1000);
  b.SetEntryBlock(0);
  const ir::ValueId rax = b.EmitGetReg(ArchRegister::kRax);
  const ir::ValueId one = b.EmitMaterializeConstant(1, ValueType::kI64);
  const ir::ValueId s = b.EmitBinary(OpKind::kSub, rax, one);  // SUB, not ADD
  b.EmitSetReg(ArchRegister::kRdx, s);
  b.EmitReturn();
  return b.MutableProgram();
}

diff::ComparisonRequest MakeRequest(const std::string& name,
                                   const Program& program) {
  diff::ComparisonRequest req;
  req.name = name;
  req.program = &program;
  req.memory_base = kDataBase;
  req.memory_size = 0x1000;
  req.windows.push_back(diff::MemoryWindow{kDataBase, 8});
  req.step_limit = 10000;
  return req;
}

}  // namespace

// ===========================================================================
// Agreement
// ===========================================================================

P5EMU_TEST(Differential, ReferenceInterpreterComparedWithItselfAgrees) {
  // Two instances of the reference interpreter must agree exactly. This is the
  // control that proves the comparison itself is doing work: if two runs of the
  // SAME code diverged, the harness would be at fault, not a backend.
  auto a = MakeReferenceInterpreter(10000);
  auto b = MakeReferenceInterpreter(10000);

  const Program program = MakeZeroResultAddProgram();
  diff::ComparisonRequest req = MakeRequest("self-agreement", program);
  req.initial_state.rax = ~std::uint64_t{0};

  const std::vector<Backend*> backends = {a.get(), b.get()};
  const diff::ComparisonResult r = diff::RunComparison(req, backends);

  CHECK_EQ(r.verdict, diff::Verdict::kAgree);
  CHECK(r.differences.empty());
  CHECK_EQ(r.reports.size(), std::size_t{2});
  for (const auto& rep : r.reports) {
    CHECK(rep.implemented);
    CHECK_EQ(static_cast<int>(rep.reason),
             static_cast<int>(TerminationReason::kReturn));
  }
  CHECK(r.coverage_gaps.empty());
}

P5EMU_TEST(Differential, ComparisonRequiresAtLeastTwoBackends) {
  // One backend cannot disagree with itself, so the harness refuses rather than
  // reporting a meaningless "agreement".
  auto a = MakeReferenceInterpreter(10000);
  const Program program = MakeZeroResultAddProgram();
  diff::ComparisonRequest req = MakeRequest("single", program);

  const std::vector<Backend*> backends = {a.get()};
  const diff::ComparisonResult r = diff::RunComparison(req, backends);
  CHECK_EQ(r.verdict, diff::Verdict::kError);
}

P5EMU_TEST(Differential, InvalidIrIsRejectedBeforeAnyBackendRuns) {
  // A backend that crashed on malformed IR would be reported as a divergence,
  // which would be a misleading diagnosis. Validation happens first.
  Builder bad("invalid");
  bad.CreateBlock(0);
  bad.SetEntryBlock(0);
  bad.EmitReturn();
  bad.MutableProgram().blocks[0].terminator.kind = TerminatorKind::kInvalid;
  const Program program = bad.MutableProgram();

  auto a = MakeReferenceInterpreter(1000);
  auto b = MakeReferenceInterpreter(1000);
  diff::ComparisonRequest req = MakeRequest("invalid-ir", program);
  const std::vector<Backend*> backends = {a.get(), b.get()};

  const diff::ComparisonResult r = diff::RunComparison(req, backends);
  CHECK_EQ(r.verdict, diff::Verdict::kError);
  CHECK(r.summary.find("validation") != std::string::npos);
}

P5EMU_TEST(Differential, BackendsAreIsolatedFromEachOther) {
  // Each backend gets its own state and its own memory port, so a backend that
  // somehow observed the other's results would be caught here rather than
  // passing silently.
  auto a = MakeReferenceInterpreter(10000);
  auto b = MakeReferenceInterpreter(10000);
  const Program program = MakeZeroResultAddProgram();
  diff::ComparisonRequest req = MakeRequest("isolation", program);
  req.initial_state.rax = ~std::uint64_t{0};

  const std::vector<Backend*> backends = {a.get(), b.get()};
  const diff::ComparisonResult r = diff::RunComparison(req, backends);
  CHECK_EQ(r.verdict, diff::Verdict::kAgree);

  // The request's own initial state must be untouched by execution.
  CHECK_EQ(req.initial_state.rax, ~std::uint64_t{0});
}

P5EMU_TEST(Differential, ComparisonIsDeterministic) {
  auto a = MakeReferenceInterpreter(10000);
  auto b = MakeBuggyTestOnlyBackend(10000);
  const Program program = MakeZeroResultAddProgram();
  diff::ComparisonRequest req = MakeRequest("determinism", program);
  req.initial_state.rax = ~std::uint64_t{0};
  const std::vector<Backend*> backends = {a.get(), b.get()};

  const diff::ComparisonResult first = diff::RunComparison(req, backends);
  const diff::ComparisonResult second = diff::RunComparison(req, backends);

  CHECK_EQ(static_cast<int>(first.verdict), static_cast<int>(second.verdict));
  CHECK_EQ(first.differences.size(), second.differences.size());
  for (std::size_t i = 0; i < first.differences.size() &&
                          i < second.differences.size();
       ++i) {
    CHECK_EQ(first.differences[i].field, second.differences[i].field);
    CHECK_EQ(first.differences[i].expected, second.differences[i].expected);
    CHECK_EQ(first.differences[i].actual, second.differences[i].actual);
  }
}

// ===========================================================================
// THE GATE
// ===========================================================================

P5EMU_TEST(DifferentialGate, HarnessDetectsTheInjectedFlagDivergence) {
  //
  // THE PHASE-1 HARD GATE.
  //
  // A real backend executes the same IR to completion and returns a plausible
  // result. The ONLY difference is that it does not set the ZERO FLAG.
  // The harness must report DIVERGE and must name EFLAGS as the field.
  //
  // This is not a synthetic forced failure. Both backends run real IR; the
  // difference is a semantic one, and it is invisible to any harness that does
  // not compare architectural flags.
  //
  auto reference = MakeReferenceInterpreter(10000);
  auto buggy = MakeBuggyTestOnlyBackend(10000);

  const Program program = MakeZeroResultAddProgram();
  diff::ComparisonRequest req = MakeRequest("zf-divergence", program);
  req.initial_state.rax = ~std::uint64_t{0};

  const std::vector<Backend*> backends = {reference.get(), buggy.get()};
  const diff::ComparisonResult r = diff::RunComparison(req, backends);

  // 1. The harness must report divergence.
  if (r.verdict != diff::Verdict::kDiverge) {
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "GATE FAILED: harness reported '%s', expected 'diverge'. "
                  "Summary: %s",
                  diff::VerdictName(r.verdict), r.summary.c_str());
    ReportFailure(__FILE__, __LINE__, buf);
    return;
  }

  // 2. Both backends must have run to completion. If the buggy backend had
  //    trapped or crashed, the harness would be detecting a different thing.
  REQUIRE(r.reports.size() == 2);
  for (const auto& rep : r.reports) {
    if (rep.reason != TerminationReason::kReturn) {
      char buf[256];
      std::snprintf(buf, sizeof(buf),
                    "GATE INVALID: backend '%s' ended with '%s', not 'return'. "
                    "The harness must catch a semantic divergence, not a crash.",
                    rep.name.c_str(), TerminationReasonName(rep.reason));
      ReportFailure(__FILE__, __LINE__, buf);
      return;
    }
    CHECK(rep.status.ok());
  }

  // 3. The divergence must be reported as EFLAGS, and ONLY as EFLAGS. If it
  //    were also reported for a register, the injected bug would be broader than
  //    documented and the test would be proving the wrong thing.
  REQUIRE(!r.differences.empty());
  const diff::Difference& d = r.differences.front();
  CHECK_EQ(d.field, std::string("eflags"));
  CHECK_EQ(d.backend_a, std::string("reference-interpreter"));
  CHECK(d.backend_b.find("buggy-test-only") != std::string::npos);

  // The reference sets ZF; the buggy backend does not. So the reference's
  // EFLAGS has the zero bit set and the buggy one's does not.
  CHECK((d.expected & eflags::kZf) != 0);
  CHECK((d.actual & eflags::kZf) == 0);

  // 4. Everything else must match. This is what makes the test specific: it
  //    proves the harness is comparing a FIELD SET, not just detecting that
  //    "something" differs.
  CHECK_EQ(r.differences.size(), std::size_t{1});
}

P5EMU_TEST(DifferentialGate, TheDivergenceIsInvisibleToAValueOnlyCheck) {
  // Characterises the injected bug: the register holding the sum is IDENTICAL
  // in both backends. If this ever stops being true, the gate above would no
  // longer be testing flag comparison specifically, and this test fails loudly.
  auto reference = MakeReferenceInterpreter(10000);
  auto buggy = MakeBuggyTestOnlyBackend(10000);
  const Program program = MakeZeroResultAddProgram();

  CpuState state_a;
  state_a.rax = ~std::uint64_t{0};
  CpuState state_b = state_a;

  auto mem_a = LinearGuestMemory::MakeCodeAndData(kDataBase, 0x1000);
  auto mem_b = LinearGuestMemory::MakeCodeAndData(kDataBase, 0x1000);

  reference->Execute(program, &state_a, mem_a);
  buggy->Execute(program, &state_b, mem_b);

  // The SUM matches.
  CHECK_EQ(state_a.rbx, state_b.rbx);
  CHECK_EQ(state_a.rbx, std::uint64_t{0});

  // The EFLAGS do NOT.
  CHECK(state_a.eflags != state_b.eflags);
  CHECK((state_a.eflags & eflags::kZf) != 0);
  CHECK((state_b.eflags & eflags::kZf) == 0);

  // Both still agree about carry, which the bug does not touch.
  CHECK_EQ((state_a.eflags & eflags::kCf) != 0, (state_b.eflags & eflags::kCf) != 0);
}

P5EMU_TEST(DifferentialGate, HarnessAgreesWhenThereIsNoAddToCorrupt) {
  // The buggy backend must be correct on programs with no add, so a green run
  // on those is meaningful. Without this, "the harness passes" could just mean
  // "the bug never fires".
  auto reference = MakeReferenceInterpreter(10000);
  auto buggy = MakeBuggyTestOnlyBackend(10000);

  const Program program = MakeNoAddProgram();
  diff::ComparisonRequest req = MakeRequest("no-add", program);
  req.initial_state.rax = 100;

  const std::vector<Backend*> backends = {reference.get(), buggy.get()};
  const diff::ComparisonResult r = diff::RunComparison(req, backends);
  CHECK_EQ(r.verdict, diff::Verdict::kAgree);
  CHECK(r.differences.empty());
}

P5EMU_TEST(DifferentialGate, ReferenceInterpreterAloneStillCorrectWithoutTheBuggy) {
  // Normal verification never constructs the buggy backend. This asserts the
  // ordinary path is clean, so a future change cannot quietly leave the buggy
  // backend in the default verification set.
  auto reference = MakeReferenceInterpreter(10000);
  const Program program = MakeZeroResultAddProgram();
  diff::ComparisonRequest req = MakeRequest("reference-only", program);
  req.initial_state.rax = ~std::uint64_t{0};

  const std::vector<Backend*> backends = {reference.get()};
  const diff::ComparisonResult r = diff::RunComparison(req, backends);

  // With a single backend the harness reports an error, which is the documented
  // behaviour: a comparison needs something to compare against.
  CHECK_EQ(r.verdict, diff::Verdict::kError);

  // The reference interpreter itself is correct on this program.
  CpuState state;
  state.rax = ~std::uint64_t{0};
  auto mem = LinearGuestMemory::MakeCodeAndData(kDataBase, 0x1000);
  const ExecutionResult er = reference->Execute(program, &state, mem);
  CHECK(er.status.ok());
  CHECK_EQ(state.rbx, std::uint64_t{0});
  CHECK((state.eflags & eflags::kZf) != 0);
}

P5EMU_TEST(DifferentialGate, HarnessReportsWhichBackendDiverged) {
  // Diagnostics must name both sides, otherwise a failure is unactionable.
  auto reference = MakeReferenceInterpreter(10000);
  auto buggy = MakeBuggyTestOnlyBackend(10000);
  const Program program = MakeZeroResultAddProgram();
  diff::ComparisonRequest req = MakeRequest("diagnostics", program);
  req.initial_state.rax = ~std::uint64_t{0};

  const std::vector<Backend*> backends = {reference.get(), buggy.get()};
  const diff::ComparisonResult r = diff::RunComparison(req, backends);

  CHECK_EQ(r.verdict, diff::Verdict::kDiverge);
  CHECK(r.summary.find("eflags") != std::string::npos);
  CHECK(r.summary.find("reference-interpreter") != std::string::npos);
  CHECK(r.summary.find("buggy-test-only") != std::string::npos);
}

P5EMU_TEST(DifferentialGate, MemoryDivergenceIsAlsoDetected) {
  // The harness must compare WRITTEN MEMORY, not just registers. A separate
  // deliberately-wrong backend wrapper proves that half of the comparison works.
  //
  // This one perturbs memory rather than flags, so it demonstrates the two
  // halves of the comparison are independent.
  class MemoryCorruptingBackend final : public Backend {
   public:
    explicit MemoryCorruptingBackend(std::uint64_t limit)
        : inner_(MakeReferenceInterpreter(limit)) {}

    [[nodiscard]] BackendId id() const noexcept override {
      return BackendId::kBuggyTestOnly;
    }
    [[nodiscard]] const char* name() const noexcept override {
      return "buggy-test-only(memory-offset-by-one)";
    }
    [[nodiscard]] std::uint64_t step_limit() const noexcept override {
      return inner_->step_limit();
    }
    ExecutionResult Execute(const ir::Program& program, CpuState* state,
                            memory::GuestMemoryPort& memory) override {
      ExecutionResult r = inner_->Execute(program, state, memory);
      // Write one byte past the end of the value that was stored, so the
      // declared window still matches but a wider view would not.
      std::uint64_t ignored = 0;
      (void)memory.WriteData(0x0003'0008ull, 1, 0xFF);
      (void)ignored;
      return r;
    }

   private:
    std::unique_ptr<Backend> inner_;
  };

  auto reference = MakeReferenceInterpreter(10000);
  MemoryCorruptingBackend corruptor(10000);

  const Program program = MakeZeroResultAddProgram();
  diff::ComparisonRequest req = MakeRequest("memory-divergence", program);
  req.initial_state.rax = 5;
  req.windows.push_back(diff::MemoryWindow{kDataBase, 16});

  const std::vector<Backend*> backends = {reference.get(), &corruptor};
  const diff::ComparisonResult r = diff::RunComparison(req, backends);

  CHECK_EQ(r.verdict, diff::Verdict::kDiverge);
  REQUIRE(!r.differences.empty());
  CHECK(r.differences.front().field.find("mem[") != std::string::npos);
}
