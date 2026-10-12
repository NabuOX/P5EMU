// SPDX-License-Identifier: Apache-2.0
//
// P5EMU is an independent, open-source project. See LICENSE and
// PROVENANCE.md before adding code here.
//
// ============================================================================
// AArch64 BACKEND TESTS (Phase 2)
// ============================================================================
//
// Three layers:
//
//   Arm64Emitter      -- instruction encodings, checked against known words
//   Arm64Codegen      -- explicit failure on unsupported IR; deterministic output
//   Arm64Differential -- interpreter vs AArch64, executed through an INDEPENDENT
//                        AArch64 environment, compared field by field
//
// The differential tests require the AArch64 harness (see
// scripts/build_aarch64_harness.sh). They fail loudly when it is missing rather
// than skipping, because the whole point of Phase 2 is that the emitted code
// really runs somewhere P5EMU did not write.
// ============================================================================

#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <vector>

#include "TestHarness.h"

#include "p5emu/cpu/Arm64Backend.h"
#include "p5emu/cpu/Arm64CodeGen.h"
#include "p5emu/cpu/Arm64Emitter.h"
#include "p5emu/cpu/Arm64Runner.h"
#include "p5emu/cpu/CodeBuffer.h"
#include "p5emu/cpu/Differential.h"
#include "p5emu/cpu/IrBuilder.h"
#include "p5emu/cpu/LinearGuestMemory.h"
#include "p5emu/cpu/ReferenceInterpreter.h"
#include "p5emu/cpu/TranslateToIr.h"

namespace {

using namespace p5emu;         // NOLINT(build/namespaces)
using namespace p5emu::cpu;    // NOLINT(build/namespaces)
namespace ir = p5emu::ir;
namespace arm64 = p5emu::arm64;
namespace memory = p5emu::memory;
using ir::Builder;

constexpr std::uint64_t kStepLimit = 1u << 20;
constexpr p5emu::memory::GuestAddr kMemBase = 0x1'0000;
constexpr p5emu::memory::GuestSize kMemSize = 0x1000;

std::string HarnessPath() {
  const char* env = std::getenv("P5EMU_A64_HARNESS");
  if (env != nullptr && *env != '\0') {
    return env;
  }
#ifdef P5EMU_A64_HARNESS_DEFAULT
  return P5EMU_A64_HARNESS_DEFAULT;
#else
  return "";
#endif
}

// Reports the comparison summary on divergence so a failure is actionable.
void ExpectAgree(const diff::ComparisonResult& r, const char* file, int line) {
  if (r.verdict != diff::Verdict::kAgree) {
    p5emu::test::ReportFailure(file, line,
                               std::string("expected agreement: ") + r.summary);
  }
}

diff::ComparisonResult CompareProgram(
    const ir::Program& program, const CpuState& initial,
    const std::vector<std::uint8_t>& memory = {},
    const std::vector<diff::MemoryWindow>& windows = {},
    const Arm64CodeGenOptions& options = {},
    std::uint64_t eflags_mask = p5emu::cpu::eflags::kAllMask) {
  QemuArm64Runner runner(HarnessPath());
  auto reference = MakeReferenceInterpreter(kStepLimit);
  auto arm = MakeArm64Backend(kStepLimit, &runner, options);

  diff::ComparisonRequest request;
  request.name = program.name;
  request.program = &program;
  request.initial_state = initial;
  request.memory_base = kMemBase;
  request.memory_size = kMemSize;
  request.initial_memory = memory;
  request.permissions = p5emu::memory::kPermReadWriteExecute;
  request.windows = windows;
  request.step_limit = kStepLimit;
  request.eflags_observable_mask = eflags_mask;

  return diff::RunComparison(request, {reference.get(), arm.get()});
}

}  // namespace

// ===========================================================================
// Emitter: encodings must match the ARM ARM. Words below were independently
// disassembled with binutils (aarch64-linux-gnu-objdump) and cross-checked.
// ===========================================================================
P5EMU_TEST(Arm64Emitter, InstructionWordsMatchTheArchitecture) {
  using namespace p5emu::arm64::enc;
  CHECK_EQ(Movz(9, 0x1234, 0), 0xD2824689u);
  CHECK_EQ(Movz(9, 0xABCD, 16), 0xD2B579A9u);
  CHECK_EQ(Movk(9, 0x1234, 32), 0xF2C24689u);
  CHECK_EQ(AddSubImm(false, false, 11, 9, 5, false), 0x9100152Bu);
  CHECK_EQ(AddSubImm(true, false, 9, 9, 1, false), 0xD1000529u);
  CHECK_EQ(AddSubReg(false, true, 11, 9, 10, 0, 0), 0xAB0A012Bu);   // adds
  CHECK_EQ(AddSubReg(true, true, 11, 9, 10, 0, 0), 0xEB0A012Bu);    // subs
  CHECK_EQ(LogicalReg(0, 11, 9, 10, 0, 0), 0x8A0A012Bu);            // and
  CHECK_EQ(LogicalReg(1, 11, 9, 10, 0, 0), 0xAA0A012Bu);            // orr
  CHECK_EQ(LogicalReg(2, 11, 9, 10, 0, 0), 0xCA0A012Bu);            // eor
  CHECK_EQ(MovReg64(11, 9), 0xAA0903EBu);
  CHECK_EQ(MovReg32(11, 9), 0x2A0903EBu);
  CHECK_EQ(LsrImm(11, 9, 4), 0xD344FD2Bu);
  CHECK_EQ(AsrImm(11, 9, 1), 0x9341FD2Bu);
  CHECK_EQ(LslImm(11, 9, 3), 0xD37DF12Bu);
  CHECK_EQ(Sxtb(11, 9), 0x93401D2Bu);
  CHECK_EQ(Sxth(11, 9), 0x93403D2Bu);
  CHECK_EQ(Sxtw(11, 9), 0x93407D2Bu);
  CHECK_EQ(Csel(11, 9, 10, arm64::Cond::kEq), 0x9A8A012Bu);
  CHECK_EQ(Csinc(11, 9, 10, arm64::Cond::kNe), 0x9A8A152Bu);
  CHECK_EQ(Cset(11, arm64::Cond::kEq), 0x9A9F17EBu);
  CHECK_EQ(Cset(11, arm64::Cond::kHi), 0x9A9F97EBu);
  CHECK_EQ(Madd(11, 9, 10, 11), 0x9B0A2D2Bu);
  CHECK_EQ(LdrU64(9, 19, 0), 0xF9400269u);
  CHECK_EQ(StrU64(9, 19, 16), 0xF9000A69u);
  CHECK_EQ(StrU64Pre(9, 31, -16), 0xF81F0FE9u);
  CHECK_EQ(LdrU64Post(9, 31, 16), 0xF84107E9u);
  CHECK_EQ(StpU64Pre(29, 30, 31, -2), 0xA9BF7BFDu);
  CHECK_EQ(LdpU64Post(29, 30, 31, 2), 0xA8C17BFDu);
  CHECK_EQ(StpU64Off(9, 10, 19, 0), 0xA9002A69u);
  CHECK_EQ(Cbz(9, 2), 0xB4000049u);
  CHECK_EQ(Cbnz(9, 2), 0xB5000049u);
  CHECK_EQ(B(2), 0x14000002u);
  CHECK_EQ(Bl(2), 0x94000002u);
  CHECK_EQ(BCond(arm64::Cond::kEq, 2), 0x54000040u);
  CHECK_EQ(BCond(arm64::Cond::kGe, 2), 0x5400004Au);
  CHECK_EQ(Ret(30), 0xD65F03C0u);
  CHECK_EQ(Br(9), 0xD61F0120u);
  CHECK_EQ(Blr(9), 0xD63F0120u);
  CHECK_EQ(Ubfx(11, 9, 4, 4), 0xD3441D2Bu);
  // Phase 3 additions: variable shifts and signed bitfield extract. Words
  // independently verified against aarch64-linux-gnu-objdump.
  CHECK_EQ(Lslv(11, 9, 10), 0x9ACA212Bu);
  CHECK_EQ(Lsrv(11, 9, 10), 0x9ACA252Bu);
  CHECK_EQ(Asrv(11, 9, 10), 0x9ACA292Bu);
  CHECK_EQ(Sbfx(11, 9, 0, 8), 0x93401D2Bu);
  CHECK_EQ(Sbfx(11, 9, 0, 16), 0x93403D2Bu);
}

P5EMU_TEST(Arm64Emitter, BranchFixupsAreResolvedOrReportedUnbound) {
  arm64::Assembler asm_;
  const int target = asm_.NewLabel();
  asm_.B(target);           // index 0
  asm_.Raw(0xD503201Fu);    // nop (index 1)
  asm_.Bind(target);        // index 2
  asm_.Raw(0xD503201Fu);
  std::vector<std::uint8_t> bytes;
  CHECK(asm_.Finalize(&bytes));
  // The first word is B with imm26 = 2 (target - here). 0x14000002.
  const std::uint32_t first = static_cast<std::uint32_t>(bytes[0]) |
                              (static_cast<std::uint32_t>(bytes[1]) << 8) |
                              (static_cast<std::uint32_t>(bytes[2]) << 16) |
                              (static_cast<std::uint32_t>(bytes[3]) << 24);
  CHECK_EQ(first, 0x14000002u);

  // An unbound label must be reported, not silently patched to offset 0.
  arm64::Assembler bad;
  const int never = bad.NewLabel();
  bad.B(never);
  std::vector<std::uint8_t> out;
  CHECK(!bad.Finalize(&out));
}

// ===========================================================================
// Codegen: explicit failure, determinism.
// ===========================================================================
P5EMU_TEST(Arm64Codegen, SupportedProgramEmitsDeterministicCode) {
  Builder b("gen");
  b.CreateBlock(0x1000);
  b.SetEntryBlock(0);
  const ir::ValueId a = b.EmitMaterializeConstant(7, ir::ValueType::kI64);
  b.EmitSetReg(ir::ArchRegister::kRax, a);
  b.EmitReturn();

  Arm64CodeGenResult r1;
  Arm64CodeGenResult r2;
  REQUIRE(GenerateArm64(b.program(), &r1).ok());
  REQUIRE(GenerateArm64(b.program(), &r2).ok());
  CHECK(!r1.code.empty());
  CHECK_EQ(r1.code.size() % 4, std::size_t{0});  // whole instructions
  // Same IR in, byte-identical code out.
  CHECK(r1.code == r2.code);
}

P5EMU_TEST(Arm64Codegen, OversizedProgramIsRefusedExplicitlyAndEmitsNothing) {
  // The code generator refuses what it cannot address (the stack-slot frame
  // exceeds the unsigned-offset range) with kUnimplementedInstruction and emits
  // NO code. It must never emit a truncated or approximate function.
  Builder b("big");
  b.CreateBlock(0x1000);
  b.SetEntryBlock(0);
  for (int i = 0; i < 2100; ++i) {
    const ir::ValueId v = b.EmitMaterializeConstant(
        static_cast<std::uint64_t>(i), ir::ValueType::kI64);
    b.EmitSetReg(ir::ArchRegister::kRax, v);
  }
  b.EmitReturn();

  Arm64CodeGenResult r;
  const Status st = GenerateArm64(b.program(), &r);
  CHECK_EQ(st.code(), StatusCode::kUnimplementedInstruction);
  CHECK(r.code.empty());
  CHECK(!r.reason.empty());
}

P5EMU_TEST(Arm64Codegen, StructurallyUnusableProgramIsRejected) {
  // No entry block: the generator must report a hard error, not produce code.
  Builder b("noentry");
  b.CreateBlock(0x1000);
  b.EmitReturn();
  // Deliberately do not call SetEntryBlock.

  Arm64CodeGenResult r;
  const Status st = GenerateArm64(b.program(), &r);
  CHECK_EQ(st.code(), StatusCode::kInvalidArgument);
  CHECK(r.code.empty());
}

P5EMU_TEST(Arm64Codegen, CodeBufferMapsWritableThenExecutable) {
  // The code-buffer seam: bytes written into a generation buffer are visible
  // through the executable mapping, and the mapping is real. (Execution of this
  // machine code happens only on AArch64; here we verify the mechanism.)
  CodeBuffer buf;
  if (!CodeBuffer::Supported()) {
    CHECK(true);  // platform without a W^X implementation: nothing to check
    return;
  }
  const std::vector<std::uint8_t> code = {0xD2, 0x80, 0x00, 0x20,  // mov x0,#1
                                          0xD6, 0x5F, 0x03, 0xC0}; // ret
  REQUIRE(buf.Install(code));
  CHECK_EQ(buf.size(), code.size());
  REQUIRE(buf.data() != nullptr);
  for (std::size_t i = 0; i < code.size(); ++i) {
    CHECK_EQ(buf.data()[i], code[i]);
  }
  CHECK(buf.entry() != nullptr);
}

// ===========================================================================
// Differential: interpreter vs AArch64 execution, same IR / state / memory.
// ===========================================================================
P5EMU_TEST(Arm64Differential, AddSubFlagsAgree) {
  Builder b("arith");
  b.CreateBlock(0x1000);
  b.SetEntryBlock(0);
  const ir::ValueId a = b.EmitGetReg(ir::ArchRegister::kRax);
  const ir::ValueId c = b.EmitGetReg(ir::ArchRegister::kRcx);
  const ir::ValueId sum = b.EmitBinaryFlags(ir::OpKind::kAddFlags, a, c);
  b.EmitSetReg(ir::ArchRegister::kRdx, sum);
  const ir::ValueId diff = b.EmitBinaryFlags(ir::OpKind::kSubFlags, a, c);
  b.EmitSetReg(ir::ArchRegister::kRsi, diff);
  b.EmitReturn();

  const struct { std::uint64_t a, c; } cases[] = {
      {5, 10}, {0, 0}, {0xFFFF, 1}, {0x8000'0000'0000'0000ull, 1},
      {0x7FFF'FFFF'FFFF'FFFFull, 1}, {10, 5}, {5, 10}};
  for (const auto& t : cases) {
    CpuState s;
    s.rax = t.a;
    s.rcx = t.c;
    const auto r = CompareProgram(b.program(), s);
    ExpectAgree(r, __FILE__, __LINE__);
  }
}

P5EMU_TEST(Arm64Differential, LogicFlagsAgreeForAndOrXor) {
  // Also the regression for the Phase-1 OR/XOR-as-AND defect: the AArch64
  // backend must compute the same operation the IR names.
  Builder b("logic");
  b.CreateBlock(0x1000);
  b.SetEntryBlock(0);
  const ir::ValueId a = b.EmitGetReg(ir::ArchRegister::kRax);
  const ir::ValueId c = b.EmitGetReg(ir::ArchRegister::kRcx);
  b.EmitSetReg(ir::ArchRegister::kR8,
               b.EmitBinaryFlags(ir::OpKind::kAndFlags, a, c));
  b.EmitSetReg(ir::ArchRegister::kR9,
               b.EmitBinaryFlags(ir::OpKind::kOrFlags, a, c));
  b.EmitSetReg(ir::ArchRegister::kR10,
               b.EmitBinaryFlags(ir::OpKind::kXorFlags, a, c));
  b.EmitReturn();

  const struct { std::uint64_t a, c; } cases[] = {
      {0xF0, 0x3C}, {0, 0}, {0xFFFF'FFFF'FFFF'FFFFull, 0x8000'0000'0000'0000ull},
      {0x1234'5678'9ABC'DEF0ull, 0x0F0F'0F0F'0F0F'0F0Full}};
  for (const auto& t : cases) {
    CpuState s;
    s.rax = t.a;
    s.rcx = t.c;
    const auto r = CompareProgram(b.program(), s);
    ExpectAgree(r, __FILE__, __LINE__);
  }
}

P5EMU_TEST(Arm64Differential, CompareAndConditionCodesAgree) {
  // CMP followed by TestCc for every condition code, stored to a register, so
  // every flag and every condition is exercised against the interpreter.
  for (int cc = 0; cc <= 15; ++cc) {
    Builder b("cc");
    b.CreateBlock(0x1000);
    b.SetEntryBlock(0);
    const ir::ValueId a = b.EmitGetReg(ir::ArchRegister::kRax);
    const ir::ValueId c = b.EmitGetReg(ir::ArchRegister::kRcx);
    const ir::ValueId flags = b.EmitCompare(a, c);
    b.EmitSetReg(ir::ArchRegister::kRdx,
                 b.EmitTestCc(static_cast<ir::ConditionCode>(cc), flags));
    b.EmitReturn();
    const struct { std::uint64_t a, c; } cases[] = {
        {5, 5}, {5, 10}, {10, 5}, {0, 0},
        {0xFFFF'FFFF'FFFF'FFFFull, 0}, {0x8000'0000'0000'0000ull, 1}};
    for (const auto& t : cases) {
      CpuState s;
      s.rax = t.a;
      s.rcx = t.c;
      const auto r = CompareProgram(b.program(), s);
      ExpectAgree(r, __FILE__, __LINE__);
    }
  }
}

P5EMU_TEST(Arm64Differential, ConditionalBranchAgrees) {
  // Taken and not-taken paths both update a register, so a wrong condition is a
  // state divergence.
  for (int cc = 0; cc <= 15; ++cc) {
    Builder b("branch");
    const ir::BlockId entry = b.CreateBlock(0x1000);
    b.SetEntryBlock(entry);
    const ir::ValueId a = b.EmitGetReg(ir::ArchRegister::kRax);
    const ir::ValueId c = b.EmitGetReg(ir::ArchRegister::kRcx);
    const ir::ValueId flags = b.EmitCompare(a, c);
    const ir::BlockId taken = b.CreateBlock(0x1000);
    const ir::BlockId nottaken = b.CreateBlock(0x1000);
    // CreateBlock makes the new block current, so return to the entry before
    // attaching the branch to it.
    b.SetCurrentBlock(entry);
    b.EmitBranch(static_cast<ir::ConditionCode>(cc), flags, taken, nottaken);
    b.SetCurrentBlock(taken);
    const ir::ValueId one = b.EmitMaterializeConstant(1, ir::ValueType::kI64);
    b.EmitSetReg(ir::ArchRegister::kRdx, one);
    b.EmitReturn();
    b.SetCurrentBlock(nottaken);
    const ir::ValueId two = b.EmitMaterializeConstant(2, ir::ValueType::kI64);
    b.EmitSetReg(ir::ArchRegister::kRdx, two);
    b.EmitReturn();

    const struct { std::uint64_t a, c; } cases[] = {{5, 5}, {3, 9}, {9, 3}};
    for (const auto& t : cases) {
      CpuState s;
      s.rax = t.a;
      s.rcx = t.c;
      const auto r = CompareProgram(b.program(), s);
      ExpectAgree(r, __FILE__, __LINE__);
    }
  }
}

P5EMU_TEST(Arm64Differential, MemoryLoadStoreAgrees) {
  Builder b("mem");
  b.CreateBlock(0x1000);
  b.SetEntryBlock(0);
  const ir::ValueId addr = b.EmitMaterializeConstant(kMemBase + 0x40,
                                                     ir::ValueType::kI64);
  const ir::ValueId val = b.EmitMaterializeConstant(0xCAFE'BABE'1234'5678ull,
                                                    ir::ValueType::kI64);
  b.EmitStore(addr, val, 8);
  const ir::ValueId loaded = b.EmitLoad(addr, 8);
  b.EmitSetReg(ir::ArchRegister::kRax, loaded);
  b.EmitReturn();

  CpuState s;
  std::vector<std::uint8_t> mem(kMemSize, 0);
  std::vector<diff::MemoryWindow> windows = {{kMemBase + 0x40, 8}};
  const auto r = CompareProgram(b.program(), s, mem, windows);
  ExpectAgree(r, __FILE__, __LINE__);
  // Also: memory outside the write is untouched (checked by the window plus the
  // fact that the whole image round-trips through the backend).
}

P5EMU_TEST(Arm64Differential, OutOfBoundsLoadFaultsInBothBackends) {
  Builder b("fault");
  b.CreateBlock(0x1000);
  b.SetEntryBlock(0);
  // Address well outside the single region.
  const ir::ValueId bad = b.EmitMaterializeConstant(0xDEAD'0000ull,
                                                    ir::ValueType::kI64);
  const ir::ValueId v = b.EmitLoad(bad, 8);
  b.EmitSetReg(ir::ArchRegister::kRax, v);
  b.EmitReturn();

  CpuState s;
  const auto r = CompareProgram(b.program(), s);
  // Both must fault. The harness maps its out-of-bounds code to the same
  // StatusCode the linear port returns, so the comparison is exact.
  ExpectAgree(r, __FILE__, __LINE__);
}

P5EMU_TEST(Arm64Differential, UnimplementedBackendIsACoverageGapNotADivergence) {
  // A backend that reports kUnimplementedInstruction must yield a PURE coverage
  // gap: no state-difference noise from a run that never happened, and a verdict
  // distinct from both agreement and divergence.
  Builder b("gap");
  b.CreateBlock(0x1000);
  b.SetEntryBlock(0);
  const ir::ValueId a = b.EmitMaterializeConstant(1, ir::ValueType::kI64);
  b.EmitSetReg(ir::ArchRegister::kRax, a);
  b.EmitReturn();

  auto reference = MakeReferenceInterpreter(kStepLimit);
  auto unimplemented = MakeUnimplementedTestOnlyBackend(kStepLimit);

  diff::ComparisonRequest request;
  request.program = &b.program();
  request.memory_base = kMemBase;
  request.memory_size = kMemSize;
  request.permissions = p5emu::memory::kPermReadWriteExecute;
  const auto r =
      diff::RunComparison(request, {reference.get(), unimplemented.get()});

  REQUIRE(r.reports.size() == 2);
  CHECK(r.reports[0].implemented == true);
  CHECK(r.reports[1].implemented == false);
  CHECK(!r.coverage_gaps.empty());
  // The key point: a gap is NOT reported as a divergence.
  CHECK_EQ(r.verdict, diff::Verdict::kCoverageGap);
  CHECK(r.differences.empty());
}

// ===========================================================================
// Negative proof: a real, injected code-generation defect must be CAUGHT by the
// differential harness, for the right reason (a state divergence), not by a
// crash or a forced assertion.
// ===========================================================================
P5EMU_TEST(Arm64Gate, InjectedAddSubtractSwapIsCaught) {
  Builder b("neg-addsub");
  b.CreateBlock(0x1000);
  b.SetEntryBlock(0);
  const ir::ValueId a = b.EmitGetReg(ir::ArchRegister::kRax);
  const ir::ValueId c = b.EmitGetReg(ir::ArchRegister::kRcx);
  const ir::ValueId sum = b.EmitBinaryFlags(ir::OpKind::kAddFlags, a, c);
  b.EmitSetReg(ir::ArchRegister::kRdx, sum);
  b.EmitReturn();

  CpuState s;
  s.rax = 100;
  s.rcx = 23;
  Arm64CodeGenOptions bad;
  bad.inject_add_sub_swap = true;
  const auto r = CompareProgram(b.program(), s, {}, {}, bad);
  CHECK_EQ(r.verdict, diff::Verdict::kDiverge);
  CHECK(!r.differences.empty());
}

P5EMU_TEST(Arm64Gate, InjectedClearedZeroFlagIsCaught) {
  // Invisible to a value-only comparison: the sum is still correct, but ZF is
  // wrong. Only a comparison that includes EFLAGS catches it.
  Builder b("neg-zf");
  b.CreateBlock(0x1000);
  b.SetEntryBlock(0);
  const ir::ValueId a = b.EmitGetReg(ir::ArchRegister::kRax);
  const ir::ValueId c = b.EmitGetReg(ir::ArchRegister::kRcx);
  const ir::ValueId sum = b.EmitBinaryFlags(ir::OpKind::kAddFlags, a, c);
  b.EmitSetReg(ir::ArchRegister::kRdx, sum);
  b.EmitReturn();

  CpuState s;
  s.rax = 0;
  s.rcx = 0;  // sum is 0 -> ZF should be set
  Arm64CodeGenOptions bad;
  bad.inject_clear_zf = true;
  const auto r = CompareProgram(b.program(), s, {}, {}, bad);
  CHECK_EQ(r.verdict, diff::Verdict::kDiverge);

  // And the correct backend agrees, proving the detector is not simply always
  // reporting divergence.
  const auto good = CompareProgram(b.program(), s);
  ExpectAgree(good, __FILE__, __LINE__);
}

// ===========================================================================
// Whole pipeline: x86-64 bytes -> decoder -> IR -> validate -> AArch64 -> run.
// ===========================================================================
P5EMU_TEST(Arm64Pipeline, X86BytesExecuteToTheSameResultOnBothBackends) {
  //   mov rax, 0x2A        (48 C7 C0 2A 00 00 00)
  //   mov rcx, 0x10        (48 C7 C1 10 00 00 00)
  //   add rax, rcx         (48 01 C8)
  //   cmp rax, 0x3A        (48 83 F8 3A)
  //   ret                  (C3)
  const std::vector<std::uint8_t> code = {
      0x48, 0xC7, 0xC0, 0x2A, 0x00, 0x00, 0x00, 0x48, 0xC7, 0xC1, 0x10,
      0x00, 0x00, 0x00, 0x48, 0x01, 0xC8, 0x48, 0x83, 0xF8, 0x3A, 0xC3};

  ir::Program program;
  std::vector<p5emu::memory::GuestAddr> addresses;
  const Status ts =
      TranslateProgram(0x1000, code.data(), code.size(), "pipeline",
                       &program, &addresses);
  REQUIRE(ts.ok());
  REQUIRE(ir::ValidateIr(program).ok());

  CpuState s;
  s.rdi = 0xABCD;              // must be untouched
  s.rsp = kMemBase + 0x800;    // RET pops [rsp]; give it a mapped stack
  const auto r = CompareProgram(program, s);
  ExpectAgree(r, __FILE__, __LINE__);

  // The expected value is derived independently by hand: 0x2A + 0x10 = 0x3A, and
  // it comes from the reference interpreter, never from ARM64 output.
  p5emu::memory::LinearGuestMemory mem =
      p5emu::memory::LinearGuestMemory::MakeCodeAndData(kMemBase, kMemSize);
  CpuState check = s;
  ReferenceInterpreter interp(kStepLimit);
  const ExecutionResult res = interp.Execute(program, &check, mem);
  CHECK_EQ(res.reason, TerminationReason::kReturn);
  CHECK_EQ(check.rax, std::uint64_t{0x3A});
  CHECK_EQ(check.rdi, std::uint64_t{0xABCD});
}

// ===========================================================================
// Property testing: seeded random arithmetic/logic programs.
// ===========================================================================
namespace {
std::uint64_t NextRandom(std::uint64_t* state) {
  // xorshift64*, deterministic and adequate for fixture generation.
  std::uint64_t x = *state;
  x ^= x >> 12;
  x ^= x << 25;
  x ^= x >> 27;
  *state = x;
  return x * 0x2545F4914F6CDD1Dull;
}
}  // namespace

P5EMU_TEST(Arm64Property, RandomArithmeticAndLogicAgree) {
  std::uint64_t rng = 0x1234'5678'9ABC'DEF0ull;  // FIXED SEED: reproducible
  const int kPrograms = 40;
  for (int i = 0; i < kPrograms; ++i) {
    Builder b("prop");
    b.CreateBlock(0x1000);
    b.SetEntryBlock(0);
    const ir::ValueId a = b.EmitMaterializeConstant(
        NextRandom(&rng), ir::ValueType::kI64);
    const ir::ValueId c = b.EmitMaterializeConstant(
        NextRandom(&rng), ir::ValueType::kI64);
    const ir::OpKind kind = static_cast<ir::OpKind>(
        static_cast<int>(ir::OpKind::kAddFlags) + (i % 5));
    b.EmitSetReg(ir::ArchRegister::kR8, b.EmitBinaryFlags(kind, a, c));
    b.EmitReturn();

    const auto r = CompareProgram(b.program(), CpuState{});
    ExpectAgree(r, __FILE__, __LINE__);
  }
}

// ===========================================================================
// Developer debugging path: IR -> emitted AArch64 bytes -> (external) disasm.
//
// Set P5EMU_A64_DUMP_DIR to have each generated function written out as a raw
// binary, then run scripts/arm64_disasm.sh on it. This is diagnostic
// infrastructure, not part of emulation.
// ===========================================================================
P5EMU_TEST(Arm64Debug, DumpEmittedCodeWhenRequested) {
  Builder b("debug");
  b.CreateBlock(0x1000);
  b.SetEntryBlock(0);
  const ir::ValueId a = b.EmitGetReg(ir::ArchRegister::kRax);
  const ir::ValueId c = b.EmitGetReg(ir::ArchRegister::kRcx);
  b.EmitSetReg(ir::ArchRegister::kRdx,
               b.EmitBinaryFlags(ir::OpKind::kAddFlags, a, c));
  b.EmitReturn();

  Arm64CodeGenResult r;
  REQUIRE(GenerateArm64(b.program(), &r).ok());
  CHECK(!r.code.empty());

  const char* dir = std::getenv("P5EMU_A64_DUMP_DIR");
  if (dir != nullptr && *dir != '\0') {
    const std::string path = std::string(dir) + "/p5emu_debug_a64.bin";
    std::FILE* f = std::fopen(path.c_str(), "wb");
    REQUIRE(f != nullptr);
    std::fwrite(r.code.data(), 1, r.code.size(), f);
    std::fclose(f);
    std::printf("wrote %zu AArch64 bytes to %s\n", r.code.size(), path.c_str());
  }
}
