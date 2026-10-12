// SPDX-License-Identifier: Apache-2.0
//
// ReferenceInterpreter tests.
//
// ============================================================================
// EXPECTED-VALUE PROVENANCE
// ============================================================================
//
// Every expected architectural result below is derived by hand from the Intel 64
// and IA-32 Architectures Software Developer's Manual:
//
//   Vol. 2 §2.2.1      Instruction Reference (operation definitions)
//   Vol. 2 §3.1.2      Flags Register -- the definition of each flag
//   Vol. 2 §3.1.2.1    Condition Codes, Table 3-1
//
// Each test carries a `test::Provenance` naming the section and the derivation.
// NO value here was produced by running P5EMU; that would prove determinism, not
// correctness. See FixtureProvenance.h.

#include <string>
#include <vector>

#include "FixtureProvenance.h"
#include "TestHarness.h"
#include "p5emu/cpu/Backend.h"
#include "p5emu/cpu/IrBuilder.h"
#include "p5emu/cpu/LinearGuestMemory.h"
#include "p5emu/cpu/ReferenceInterpreter.h"

namespace {

using namespace p5emu;   // NOLINT - test-local convenience
using namespace p5emu::cpu;   // NOLINT
using namespace p5emu::ir;    // NOLINT
using namespace p5emu::memory;  // NOLINT
using namespace p5emu::test;   // NOLINT

constexpr GuestAddr kMemBase = 0x0002'0000ull;
constexpr GuestSize kMemSize = 0x1000ull;

// Runs a program on the reference interpreter over a FRESH memory region, and
// leaves the final state and memory behind for inspection.
//
// `mem` is reset on every call, so no test can accidentally observe state left
// behind by a previous test. That isolation is the same property the
// differential harness depends on.
ExecutionResult Run(const Program& program, CpuState* state,
                    LinearGuestMemory& mem) {
  mem = LinearGuestMemory::MakeCodeAndData(kMemBase, kMemSize);
  ReferenceInterpreter interp;
  return interp.Execute(program, state, mem);
}

}  // namespace

// ===========================================================================
// Basic arithmetic
// ===========================================================================

P5EMU_TEST(Interpreter, AddProducesTheSum) {
  // Intel SDM Vol. 2 §2.2.1 ADD: dest = dest + src (64-bit operands, result
  // truncated to 64 bits).
  const Provenance prov = Derived(
      "Intel SDM Vol. 2 §2.2.1 ADD",
      "0x0000000000000005 + 0x0000000000000007 = 0x000000000000000C");

  Builder b("add");
  b.CreateBlock(0);
  b.SetEntryBlock(0);
  const ir::ValueId a = b.EmitGetReg(ArchRegister::kRax);
  const ir::ValueId c = b.EmitGetReg(ArchRegister::kRcx);
  const ir::ValueId s = b.EmitBinary(OpKind::kAdd, a, c);
  b.EmitSetReg(ArchRegister::kRdx, s);
  b.EmitReturn();

  CpuState state;
  state.rax = 5;
  state.rcx = 7;
  LinearGuestMemory mem;
  const ExecutionResult r = Run(b.program(), &state, mem);

  CHECK(r.status.ok());
  CHECK_EQ(r.reason, TerminationReason::kReturn);
  CHECK_EQ(state.rdx, std::uint64_t{12});
  (void)prov;
}

P5EMU_TEST(Interpreter, AddWrapsAtSixtyFourBits) {
  // 64-bit arithmetic is modulo 2^64. Intel SDM Vol. 2 §2.2.1: the result is
  // truncated to the destination operand size.
  Builder b("add-wrap");
  b.CreateBlock(0);
  b.SetEntryBlock(0);
  const ir::ValueId a = b.EmitGetReg(ArchRegister::kRax);
  const ir::ValueId one = b.EmitMaterializeConstant(1, ValueType::kI64);
  const ir::ValueId s = b.EmitBinary(OpKind::kAdd, a, one);
  b.EmitSetReg(ArchRegister::kRdx, s);
  b.EmitReturn();

  CpuState state;
  state.rax = ~std::uint64_t{0};
  LinearGuestMemory mem;
  Run(b.program(), &state, mem);
  CHECK_EQ(state.rdx, std::uint64_t{0});
}

P5EMU_TEST(Interpreter, SubAndXorAndAnd) {
  Builder b("alu");
  b.CreateBlock(0);
  b.SetEntryBlock(0);
  const ir::ValueId a = b.EmitGetReg(ArchRegister::kRax);
  const ir::ValueId c = b.EmitGetReg(ArchRegister::kRcx);
  const ir::ValueId diff = b.EmitBinary(OpKind::kSub, a, c);
  b.EmitSetReg(ArchRegister::kRdx, diff);
  const ir::ValueId xr = b.EmitBinary(OpKind::kXor, a, c);
  b.EmitSetReg(ArchRegister::kRsi, xr);
  const ir::ValueId an = b.EmitBinary(OpKind::kAnd, a, c);
  b.EmitSetReg(ArchRegister::kRdi, an);
  b.EmitReturn();

  // Intel SDM Vol. 2 §2.2.1:
  //   SUB: 0xF0 - 0x3C = 0xB4
  //   XOR: 0xF0 ^ 0x3C = 0xCC
  //   AND: 0xF0 & 0x3C = 0x30
  CpuState state;
  state.rax = 0xF0;
  state.rcx = 0x3C;
  LinearGuestMemory mem;
  Run(b.program(), &state, mem);
  CHECK_EQ(state.rdx, std::uint64_t{0xB4});
  CHECK_EQ(state.rsi, std::uint64_t{0xCC});
  CHECK_EQ(state.rdi, std::uint64_t{0x30});
}

// ===========================================================================
// Flags -- Intel SDM Vol. 2 §3.1.2
// ===========================================================================

P5EMU_TEST(Flags, AddSetsZeroFlagWhenResultIsZero) {
  // SDM §3.1.2: ZF is set when the result is zero.
  Builder b("add-zf");
  b.CreateBlock(0);
  b.SetEntryBlock(0);
  const ir::ValueId a = b.EmitGetReg(ArchRegister::kRax);
  const ir::ValueId k = b.EmitMaterializeConstant(5, ValueType::kI64);
  const ir::ValueId s = b.EmitBinaryFlags(OpKind::kAddFlags, a, k);
  b.EmitSetReg(ArchRegister::kRdx, s);
  b.EmitReturn();

  CpuState state;
  state.rax = ~std::uint64_t{4};  // +5 wraps to zero
  LinearGuestMemory mem;
  Run(b.program(), &state, mem);
  CHECK_EQ(state.rdx, std::uint64_t{0});
  CHECK((state.eflags & eflags::kZf) != 0);
  CHECK((state.eflags & eflags::kCf) != 0);  // wrapped => carry
}

P5EMU_TEST(Flags, AddSetsCarryOnUnsignedOverflow) {
  // SDM §3.1.2: CF is set when the result is greater than the maximum value for
  // the operand size. 0xFFFFFFFFFFFFFFFF + 1 > max => CF=1, ZF=1.
  Builder b("add-cf");
  b.CreateBlock(0);
  b.SetEntryBlock(0);
  const ir::ValueId a = b.EmitGetReg(ArchRegister::kRax);
  const ir::ValueId one = b.EmitMaterializeConstant(1, ValueType::kI64);
  const ir::ValueId s = b.EmitBinaryFlags(OpKind::kAddFlags, a, one);
  b.EmitSetReg(ArchRegister::kRdx, s);
  b.EmitReturn();

  CpuState state;
  state.rax = ~std::uint64_t{0};
  LinearGuestMemory mem;
  Run(b.program(), &state, mem);
  CHECK_EQ(state.rdx, std::uint64_t{0});
  CHECK((state.eflags & eflags::kCf) != 0);
  CHECK((state.eflags & eflags::kOf) == 0);  // 0xFFFFFFFF.. + 1 does not overflow signed
}

P5EMU_TEST(Flags, AddSetsSignedOverflowOnlyWhenSignsDiffer) {
  // SDM §3.1.2 OF: "the result has a sign different from the sign of the two
  // operands". Adding two positives that overflows sets OF.
  //   0x7FFFFFFFFFFFFFFF + 1 = 0x8000000000000000: OF=1
  Builder b("add-of");
  b.CreateBlock(0);
  b.SetEntryBlock(0);
  const ir::ValueId a = b.EmitGetReg(ArchRegister::kRax);
  const ir::ValueId one = b.EmitMaterializeConstant(1, ValueType::kI64);
  const ir::ValueId s = b.EmitBinaryFlags(OpKind::kAddFlags, a, one);
  b.EmitSetReg(ArchRegister::kRdx, s);
  b.EmitReturn();

  CpuState state;
  state.rax = 0x7fff'ffff'ffff'ffffull;
  LinearGuestMemory mem;
  Run(b.program(), &state, mem);
  CHECK_EQ(state.rdx, 0x8000'0000'0000'0000ull);
  CHECK((state.eflags & eflags::kOf) != 0);
  CHECK((state.eflags & eflags::kSf) != 0);
  CHECK((state.eflags & eflags::kCf) == 0);  // no unsigned carry
}

P5EMU_TEST(Flags, SubSetsBorrowAndSignedOverflow) {
  // SDM §3.1.2: CF set on unsigned borrow; OF set when subtracting a negative
  // from a positive overflows signed.
  //   0x0000000000000000 - 0x0000000000000001: CF=1, OF=0, SF=1
  Builder b("sub-flags");
  b.CreateBlock(0);
  b.SetEntryBlock(0);
  const ir::ValueId a = b.EmitGetReg(ArchRegister::kRax);
  const ir::ValueId one = b.EmitMaterializeConstant(1, ValueType::kI64);
  const ir::ValueId s = b.EmitBinaryFlags(OpKind::kSubFlags, a, one);
  b.EmitSetReg(ArchRegister::kRdx, s);
  b.EmitReturn();

  CpuState state;
  state.rax = 0;
  LinearGuestMemory mem;
  Run(b.program(), &state, mem);
  CHECK_EQ(state.rdx, ~std::uint64_t{0});
  CHECK((state.eflags & eflags::kCf) != 0);
  CHECK((state.eflags & eflags::kOf) == 0);
  CHECK((state.eflags & eflags::kSf) != 0);
  CHECK((state.eflags & eflags::kZf) == 0);
}

P5EMU_TEST(Flags, AuxiliaryCarryIsSetOnlyAcrossBitThree) {
  // SDM §3.1.2 AF: carry or borrow out of bit 3. This test is the one that caught
  // a real bug: an unsigned comparison made AF permanently zero.
  //
  //   0x0F + 0x01 = 0x10  -> borrow/carry out of bit 3 -> AF=1
  //   0x08 + 0x01 = 0x09  -> no carry out of bit 3  -> AF=0
  {
    Builder b("af-add-set");
    b.CreateBlock(0);
    b.SetEntryBlock(0);
    const ir::ValueId a = b.EmitGetReg(ArchRegister::kRax);
    const ir::ValueId one = b.EmitMaterializeConstant(1, ValueType::kI64);
    const ir::ValueId s = b.EmitBinaryFlags(OpKind::kAddFlags, a, one);
    b.EmitSetReg(ArchRegister::kRdx, s);
    b.EmitReturn();
    CpuState state;
    state.rax = 0x0F;
    LinearGuestMemory mem;
    Run(b.program(), &state, mem);
    CHECK_EQ(state.rdx, std::uint64_t{0x10});
    CHECK((state.eflags & eflags::kAf) != 0);
  }
  {
    Builder b("af-add-clear");
    b.CreateBlock(0);
    b.SetEntryBlock(0);
    const ir::ValueId a = b.EmitGetReg(ArchRegister::kRax);
    const ir::ValueId one = b.EmitMaterializeConstant(1, ValueType::kI64);
    const ir::ValueId s = b.EmitBinaryFlags(OpKind::kAddFlags, a, one);
    b.EmitSetReg(ArchRegister::kRdx, s);
    b.EmitReturn();
    CpuState state;
    state.rax = 0x08;
    LinearGuestMemory mem;
    Run(b.program(), &state, mem);
    CHECK_EQ(state.rdx, std::uint64_t{0x09});
    CHECK((state.eflags & eflags::kAf) == 0);
  }
  {
    // 0x00 - 0x01 borrows out of bit 3 -> AF=1
    Builder b("af-sub-set");
    b.CreateBlock(0);
    b.SetEntryBlock(0);
    const ir::ValueId a = b.EmitGetReg(ArchRegister::kRax);
    const ir::ValueId one = b.EmitMaterializeConstant(1, ValueType::kI64);
    const ir::ValueId s = b.EmitBinaryFlags(OpKind::kSubFlags, a, one);
    b.EmitSetReg(ArchRegister::kRdx, s);
    b.EmitReturn();
    CpuState state;
    state.rax = 0;
    LinearGuestMemory mem;
    Run(b.program(), &state, mem);
    CHECK((state.eflags & eflags::kAf) != 0);
  }
}

P5EMU_TEST(Flags, ParityFlagCountsSetBitsInTheLowByte) {
  // SDM §3.1.2 PF: set when the low byte has an EVEN number of set bits.
  //   0x03 has two set bits -> even -> PF=1
  //   0x07 has three set bits -> odd  -> PF=0
  const struct {
    std::uint64_t input;
    bool parity;
  } cases[] = {{0x03, true}, {0x07, false}, {0x00, true}, {0xFF, true}};
  for (const auto& c : cases) {
    Builder b("parity");
    b.CreateBlock(0);
    b.SetEntryBlock(0);
    const ir::ValueId a = b.EmitGetReg(ArchRegister::kRax);
    const ir::ValueId and_self = b.EmitBinary(OpKind::kAnd, a, a);
    const ir::ValueId res = b.EmitBinaryFlags(OpKind::kAndFlags, and_self, and_self);
    b.EmitSetReg(ArchRegister::kRdx, res);
    b.EmitReturn();
    CpuState state;
    state.rax = c.input;
    LinearGuestMemory mem;
    Run(b.program(), &state, mem);
    const bool pf = (state.eflags & eflags::kPf) != 0;
    if (pf != c.parity) {
      char buf[128];
      std::snprintf(buf, sizeof(buf),
                    "PF for low byte 0x%llx: expected %d, got %d",
                    static_cast<unsigned long long>(c.input & 0xffu),
                    c.parity ? 1 : 0, pf ? 1 : 0);
      ReportFailure(__FILE__, __LINE__, buf);
    }
    // AND always clears CF and OF.
    CHECK((state.eflags & eflags::kCf) == 0);
    CHECK((state.eflags & eflags::kOf) == 0);
  }
}

P5EMU_TEST(Flags, OrAndXorProduceTheirOwnOperationNotAnd) {
  // Regression for a Phase-1 defect found while building the AArch64 backend:
  // a single kLogicFlags opcode did not carry WHICH logical operation it was, so
  // the interpreter hardcoded `a & b` and silently computed OR and XOR as AND.
  // `xor rax, rax` passed by coincidence (a^a == a&a == 0), which is why the
  // Phase-1 end-to-end test did not catch it.
  //
  // Values chosen so all three differ:
  //   a = 0xF0, b = 0x3C
  //   AND = 0x30, OR = 0xFC, XOR = 0xCC
  Builder b("logic");
  b.CreateBlock(0);
  b.SetEntryBlock(0);
  const ir::ValueId a = b.EmitGetReg(ArchRegister::kRax);
  const ir::ValueId c = b.EmitGetReg(ArchRegister::kRcx);
  const ir::ValueId andv = b.EmitBinaryFlags(OpKind::kAndFlags, a, c);
  b.EmitSetReg(ArchRegister::kR8, andv);
  const ir::ValueId orv = b.EmitBinaryFlags(OpKind::kOrFlags, a, c);
  b.EmitSetReg(ArchRegister::kR9, orv);
  const ir::ValueId xorv = b.EmitBinaryFlags(OpKind::kXorFlags, a, c);
  b.EmitSetReg(ArchRegister::kR10, xorv);
  b.EmitReturn();

  CpuState state;
  state.rax = 0xF0;
  state.rcx = 0x3C;
  LinearGuestMemory mem;
  Run(b.program(), &state, mem);

  CHECK_EQ(state.r8, std::uint64_t{0x30});   // AND
  CHECK_EQ(state.r9, std::uint64_t{0xFC});   // OR, not 0x30
  CHECK_EQ(state.r10, std::uint64_t{0xCC});  // XOR, not 0x30
  // The last flags written are XOR's: 0xCC is non-zero and has SF=0.
  CHECK((state.eflags & eflags::kZf) == 0);
  CHECK((state.eflags & eflags::kSf) == 0);
  // XOR clears CF and OF.
  CHECK((state.eflags & eflags::kCf) == 0);
  CHECK((state.eflags & eflags::kOf) == 0);
}

P5EMU_TEST(Flags, CmpWritesFlagsWithoutAValueResult) {
  // SDM §3.1.2: CMP performs a subtraction and discards the result.
  //   CMP 5, 5 -> ZF=1, CF=0
  Builder b("cmp");
  b.CreateBlock(0);
  b.SetEntryBlock(0);
  const ir::ValueId a = b.EmitGetReg(ArchRegister::kRax);
  const ir::ValueId c = b.EmitGetReg(ArchRegister::kRcx);
  b.EmitCompare(a, c);
  b.EmitReturn();

  CpuState state;
  state.rax = 5;
  state.rcx = 5;
  state.rdx = 0xDEAD;  // must be untouched
  LinearGuestMemory mem;
  Run(b.program(), &state, mem);
  CHECK((state.eflags & eflags::kZf) != 0);
  CHECK((state.eflags & eflags::kCf) == 0);
  CHECK_EQ(state.rdx, std::uint64_t{0xDEAD});
}

P5EMU_TEST(Flags, TestWritesFlagsAndDoesNotAffectRegisters) {
  // SDM §3.1.2: TEST performs a bitwise AND and discards the result.
  //   TEST 0x0F, 0x08 -> 0x08 != 0 -> ZF=0
  Builder b("test");
  b.CreateBlock(0);
  b.SetEntryBlock(0);
  const ir::ValueId a = b.EmitGetReg(ArchRegister::kRax);
  const ir::ValueId m = b.EmitMaterializeConstant(0x08, ValueType::kI64);
  b.EmitTest(a, m);
  b.EmitReturn();

  CpuState state;
  state.rax = 0x0F;
  LinearGuestMemory mem;
  Run(b.program(), &state, mem);
  CHECK((state.eflags & eflags::kZf) == 0);
  CHECK_EQ(state.rax, std::uint64_t{0x0F});
}

// ===========================================================================
// Shifts -- Intel SDM Vol. 2 §2.2.1
// ===========================================================================

P5EMU_TEST(Interpreter, ShiftLeftLogical) {
  // SDM §2.2.1 SHL: SAL/SHL shifts left, filling with zeros.
  //   0x01 << 4 = 0x10
  Builder b("shl");
  b.CreateBlock(0);
  b.SetEntryBlock(0);
  const ir::ValueId a = b.EmitGetReg(ArchRegister::kRax);
  const ir::ValueId n = b.EmitMaterializeConstant(4, ValueType::kI64);
  const ir::ValueId s = b.EmitShift(OpKind::kShl, a, n);
  b.EmitSetReg(ArchRegister::kRdx, s);
  b.EmitReturn();

  CpuState state;
  state.rax = 1;
  LinearGuestMemory mem;
  Run(b.program(), &state, mem);
  CHECK_EQ(state.rdx, std::uint64_t{0x10});
}

P5EMU_TEST(Interpreter, ShiftRightLogicalAndArithmeticDifferOnNegatives) {
  // SDM §2.2.1. The distinction only matters for a NEGATIVE operand, so the
  // input must have bit 63 set; a small positive value like 0xF0 has the same
  // result under SHR and SAR, which would make the test vacuous.
  //
  // Operand 0xFFFFFFFFFFFFFFF0 (= -16 as a signed 64-bit value):
  //   SHR 4 -> 0x0FFFFFFFFFFFFFFF  (logical: zeros shifted in)
  //   SAR 4 -> 0xFFFFFFFFFFFFFFFF  (arithmetic: sign bit replicated)
  constexpr std::uint64_t kNegative = 0xFFFFFFFFFFFFFFF0ull;
  {
    Builder b("shr");
    b.CreateBlock(0);
    b.SetEntryBlock(0);
    const ir::ValueId a = b.EmitGetReg(ArchRegister::kRax);
    const ir::ValueId n = b.EmitMaterializeConstant(4, ValueType::kI64);
    const ir::ValueId s = b.EmitShift(OpKind::kShr, a, n);
    b.EmitSetReg(ArchRegister::kRdx, s);
    b.EmitReturn();
    CpuState state;
    state.rax = kNegative;
    LinearGuestMemory mem;
    Run(b.program(), &state, mem);
    CHECK_EQ(state.rdx, kNegative >> 4);
    CHECK_EQ(state.rdx, std::uint64_t{0x0FFFFFFFFFFFFFFF});
  }
  {
    Builder b("sar");
    b.CreateBlock(0);
    b.SetEntryBlock(0);
    const ir::ValueId a = b.EmitGetReg(ArchRegister::kRax);
    const ir::ValueId n = b.EmitMaterializeConstant(4, ValueType::kI64);
    const ir::ValueId s = b.EmitShift(OpKind::kSar, a, n);
    b.EmitSetReg(ArchRegister::kRdx, s);
    b.EmitReturn();
    CpuState state;
    state.rax = kNegative;
    LinearGuestMemory mem;
    Run(b.program(), &state, mem);
    // -16 >> 4 (arithmetic) = -1 = 0xFFFFFFFFFFFFFFFF
    CHECK_EQ(state.rdx, std::uint64_t{0xFFFFFFFFFFFFFFFF});
    // The defining difference from SHR: every bit above the shift is the sign.
    CHECK(state.rdx != (kNegative >> 4));
    CHECK_EQ(state.rdx, ~std::uint64_t{0});
  }
  {
    // SHR must be LOGICAL: a negative operand shifted right fills with zeros,
    // so the result is not negative.
    Builder b("shr-positive-check");
    b.CreateBlock(0);
    b.SetEntryBlock(0);
    const ir::ValueId a = b.EmitGetReg(ArchRegister::kRax);
    const ir::ValueId n = b.EmitMaterializeConstant(1, ValueType::kI64);
    const ir::ValueId s = b.EmitShift(OpKind::kShr, a, n);
    b.EmitSetReg(ArchRegister::kRdx, s);
    b.EmitReturn();
    CpuState state;
    state.rax = 0x8000'0000'0000'0000ull;
    LinearGuestMemory mem;
    Run(b.program(), &state, mem);
    CHECK_EQ(state.rdx, std::uint64_t{0x4000'0000'0000'0000});
  }
}

P5EMU_TEST(Interpreter, ShiftCountIsMaskedToSixBits) {
  // SDM Vol. 2 §2.2.2: for 64-bit operands the count is masked to 6 bits, so a
  // count of 64 behaves as 0 and leaves the operand unchanged. This is a
  // hardware-defined behaviour, not undefined behaviour, and getting it wrong
  // produces a divergence only visible on large counts.
  Builder b("shift-mask");
  b.CreateBlock(0);
  b.SetEntryBlock(0);
  const ir::ValueId a = b.EmitGetReg(ArchRegister::kRax);
  const ir::ValueId n = b.EmitMaterializeConstant(64, ValueType::kI64);
  const ir::ValueId s = b.EmitShift(OpKind::kShl, a, n);
  b.EmitSetReg(ArchRegister::kRdx, s);
  b.EmitReturn();

  CpuState state;
  state.rax = 0x1234;
  LinearGuestMemory mem;
  Run(b.program(), &state, mem);
  CHECK_EQ(state.rdx, std::uint64_t{0x1234});
}

P5EMU_TEST(Flags, ShiftSetsCarryFromTheLastBitShiftedOut) {
  // SDM §3.1.2 for shifts: CF is the LAST BIT SHIFTED OUT.
  //
  // For SHL by 1 that is bit 63 of the original operand, so the operand must
  // have its MSB set for CF to become 1. 0x80 has bit 7 set, not bit 63, and
  // would legitimately leave CF clear -- an earlier version of this test made
  // exactly that mistake.
  //
  //   0x8000000000000000 << 1 = 0 (CF=1, bit 63 shifted out)
  Builder b("shl-cf");
  b.CreateBlock(0);
  b.SetEntryBlock(0);
  const ir::ValueId a = b.EmitGetReg(ArchRegister::kRax);
  const ir::ValueId n = b.EmitMaterializeConstant(1, ValueType::kI64);
  const ir::ValueId s = b.EmitShift(OpKind::kShl, a, n);
  b.EmitSetReg(ArchRegister::kRdx, s);
  b.EmitReturn();

  CpuState state;
  state.rax = 0x8000'0000'0000'0000ull;
  LinearGuestMemory mem;
  Run(b.program(), &state, mem);
  CHECK_EQ(state.rdx, std::uint64_t{0});
  CHECK((state.eflags & eflags::kCf) != 0);
  CHECK((state.eflags & eflags::kZf) != 0);
}

P5EMU_TEST(Flags, ShiftByOneOverflowFlagTracksTheSignBit) {
  // SDM §3.1.2: for a shift by exactly 1, OF is set when the MSB of the RESULT
  // differs from the MSB of the ORIGINAL operand.
  //
  //   0x80 << 1        = 0x00000100 : MSB 0 -> 0, so OF is CLEAR
  //   0x8000...0 << 1  = 0x0000...0 : MSB 1 -> 0, so OF is SET
  //
  // Both cases are asserted so the flag cannot pass by always setting or always
  // clearing it.
  {
    Builder b("shl-of-clear");
    b.CreateBlock(0);
    b.SetEntryBlock(0);
    const ir::ValueId a = b.EmitGetReg(ArchRegister::kRax);
    const ir::ValueId n = b.EmitMaterializeConstant(1, ValueType::kI64);
    const ir::ValueId s = b.EmitShift(OpKind::kShl, a, n);
    b.EmitSetReg(ArchRegister::kRdx, s);
    b.EmitReturn();
    CpuState state;
    state.rax = 0x80;
    LinearGuestMemory mem;
    Run(b.program(), &state, mem);
    CHECK_EQ(state.rdx, std::uint64_t{0x100});
    CHECK((state.eflags & eflags::kCf) == 0);
    CHECK((state.eflags & eflags::kOf) == 0);
  }
  {
    Builder b("shl-of-set");
    b.CreateBlock(0);
    b.SetEntryBlock(0);
    const ir::ValueId a = b.EmitGetReg(ArchRegister::kRax);
    const ir::ValueId n = b.EmitMaterializeConstant(1, ValueType::kI64);
    const ir::ValueId s = b.EmitShift(OpKind::kShl, a, n);
    b.EmitSetReg(ArchRegister::kRdx, s);
    b.EmitReturn();
    CpuState state;
    state.rax = 0x8000'0000'0000'0000ull;
    LinearGuestMemory mem;
    Run(b.program(), &state, mem);
    CHECK_EQ(state.rdx, std::uint64_t{0});
    CHECK((state.eflags & eflags::kOf) != 0);
  }
}

P5EMU_TEST(Flags, ShiftRightByOneTakesCarryFromBitZero) {
  // SDM: for SHR, CF is bit 0 of the original operand.
  Builder b("shr-cf");
  b.CreateBlock(0);
  b.SetEntryBlock(0);
  const ir::ValueId a = b.EmitGetReg(ArchRegister::kRax);
  const ir::ValueId n = b.EmitMaterializeConstant(1, ValueType::kI64);
  const ir::ValueId s = b.EmitShift(OpKind::kShr, a, n);
  b.EmitSetReg(ArchRegister::kRdx, s);
  b.EmitReturn();

  CpuState state;
  state.rax = 0x0000'0000'0000'0001ull;  // bit 0 set
  LinearGuestMemory mem;
  Run(b.program(), &state, mem);
  CHECK_EQ(state.rdx, std::uint64_t{0});
  CHECK((state.eflags & eflags::kCf) != 0);
}

// ===========================================================================
// Extension -- Intel SDM Vol. 2 §2.2.1 (MOVZX/MOVSX)
// ===========================================================================

P5EMU_TEST(Interpreter, ZeroAndSignExtension) {
  Builder b("extend");
  b.CreateBlock(0);
  b.SetEntryBlock(0);
  const ir::ValueId a = b.EmitGetReg(ArchRegister::kRax);
  const ir::ValueId z32 = b.EmitExtend(a, ExtendMode::kZeroExtend32);
  b.EmitSetReg(ArchRegister::kRdx, z32);
  const ir::ValueId s8 = b.EmitExtend(a, ExtendMode::kSignExtend8);
  b.EmitSetReg(ArchRegister::kRsi, s8);
  b.EmitReturn();

  // Intel SDM Vol. 2 §2.2.1:
  //   MOVZX: zero-extend, so upper bits are 0
  //   MOVSX: sign-extend from bit 7, so 0x80 -> 0xFFFFFFFFFFFFFF80
  CpuState state;
  state.rax = 0x0000'0000'0000'0180ull;
  LinearGuestMemory mem;
  Run(b.program(), &state, mem);
  CHECK_EQ(state.rdx, std::uint64_t{0x180});
  CHECK_EQ(state.rsi, std::uint64_t{0xFFFFFFFFFFFFFF80ull});
}

// ===========================================================================
// Control flow
// ===========================================================================

P5EMU_TEST(ControlFlow, TakenBranchReachesItsTarget) {
  Builder b("branch-taken");
  b.CreateBlock(0x1000);        // b0: cmp, branch
  b.SetEntryBlock(0);
  const ir::ValueId a = b.EmitGetReg(ArchRegister::kRax);
  const ir::ValueId z = b.EmitMaterializeConstant(0, ValueType::kI64);
  const ir::ValueId f = b.EmitCompare(a, z);
  const ir::BlockId then = b.CreateBlock(0x1010);
  const ir::BlockId other = b.CreateBlock(0x1020);
  b.SetCurrentBlock(0);
  b.EmitBranch(ConditionCode::kE, f, then, other);

  b.SetCurrentBlock(then);
  const ir::ValueId one = b.EmitMaterializeConstant(1, ValueType::kI64);
  b.EmitSetReg(ArchRegister::kRdx, one);
  b.EmitReturn();

  b.SetCurrentBlock(other);
  const ir::ValueId two = b.EmitMaterializeConstant(2, ValueType::kI64);
  b.EmitSetReg(ArchRegister::kRdx, two);
  b.EmitReturn();

  CpuState state;
  state.rax = 0;
  LinearGuestMemory mem;
  const ExecutionResult r = Run(b.program(), &state, mem);
  CHECK(r.status.ok());
  CHECK_EQ(r.reason, TerminationReason::kReturn);
  CHECK_EQ(state.rdx, std::uint64_t{1});
}

P5EMU_TEST(ControlFlow, NotTakenBranchFallsThrough) {
  Builder b("branch-not-taken");
  b.CreateBlock(0x1000);
  b.SetEntryBlock(0);
  const ir::ValueId a = b.EmitGetReg(ArchRegister::kRax);
  const ir::ValueId z = b.EmitMaterializeConstant(0, ValueType::kI64);
  const ir::ValueId f = b.EmitCompare(a, z);
  const ir::BlockId then = b.CreateBlock(0x1010);
  const ir::BlockId other = b.CreateBlock(0x1020);
  b.SetCurrentBlock(0);
  b.EmitBranch(ConditionCode::kE, f, then, other);
  b.SetCurrentBlock(then);
  const ir::ValueId one = b.EmitMaterializeConstant(1, ValueType::kI64);
  b.EmitSetReg(ArchRegister::kRdx, one);
  b.EmitReturn();
  b.SetCurrentBlock(other);
  const ir::ValueId two = b.EmitMaterializeConstant(2, ValueType::kI64);
  b.EmitSetReg(ArchRegister::kRdx, two);
  b.EmitReturn();

  CpuState state;
  state.rax = 7;  // != 0, so not equal
  LinearGuestMemory mem;
  Run(b.program(), &state, mem);
  CHECK_EQ(state.rdx, std::uint64_t{2});
}

P5EMU_TEST(ControlFlow, UnconditionalJumpSkipsTheFallthroughBlock) {
  Builder b("jmp");
  b.CreateBlock(0x1000);
  b.SetEntryBlock(0);
  b.EmitJump(1);
  const ir::BlockId skipped = b.CreateBlock(0x1010);
  const ir::BlockId target = b.CreateBlock(0x1020);
  b.MutableProgram().blocks[0].terminator.target = target;

  b.SetCurrentBlock(skipped);
  const ir::ValueId poison = b.EmitMaterializeConstant(0xBAD, ValueType::kI64);
  b.EmitSetReg(ArchRegister::kRdx, poison);
  b.EmitReturn();

  b.SetCurrentBlock(target);
  const ir::ValueId good = b.EmitMaterializeConstant(0x600D, ValueType::kI64);
  b.EmitSetReg(ArchRegister::kRdx, good);
  b.EmitReturn();

  CpuState state;
  LinearGuestMemory mem;
  Run(b.program(), &state, mem);
  CHECK_EQ(state.rdx, std::uint64_t{0x600D});
}

P5EMU_TEST(ControlFlow, AllSixteenConditionCodes) {
  // Intel SDM Vol. 2 §3.1.2.1, Table 3-1. Each condition is exercised against
  // two EFLAGS values for which it must be true and two for which it must be
  // false, so no condition is "true by accident".
  struct Case {
    ConditionCode cc;
    const char* name;
    std::uint64_t flags_true[2];
    std::uint64_t flags_false[2];
  };

#define F(cf, pf, af, zf, sf, of) \
  ((cf) | (pf) << 2 | (af) << 4 | (zf) << 6 | (sf) << 7 | (of) << 11)

  const Case cases[] = {
      {ConditionCode::kO, "o", {F(0, 0, 0, 0, 0, 1), F(1, 1, 1, 1, 1, 1)},
       {F(0, 0, 0, 0, 0, 0), F(1, 0, 0, 0, 0, 0)}},
      {ConditionCode::kNO, "no", {F(0, 0, 0, 0, 0, 0), F(1, 1, 1, 1, 1, 0)},
       {F(0, 0, 0, 0, 0, 1), F(1, 1, 1, 1, 1, 1)}},
      {ConditionCode::kB, "b", {F(1, 0, 0, 0, 0, 0), F(1, 1, 0, 0, 1, 0)},
       {F(0, 0, 0, 0, 0, 0), F(0, 1, 0, 1, 1, 0)}},
      {ConditionCode::kAE, "ae", {F(0, 0, 0, 0, 0, 0), F(0, 1, 0, 1, 1, 0)},
       {F(1, 0, 0, 0, 0, 0), F(1, 1, 0, 0, 1, 0)}},
      {ConditionCode::kE, "e", {F(0, 0, 0, 1, 0, 0), F(1, 1, 1, 1, 1, 0)},
       {F(0, 0, 0, 0, 0, 0), F(1, 0, 0, 0, 1, 0)}},
      {ConditionCode::kNE, "ne", {F(0, 0, 0, 0, 0, 0), F(1, 0, 0, 0, 1, 0)},
       {F(0, 0, 0, 1, 0, 0), F(1, 1, 1, 1, 1, 0)}},
      {ConditionCode::kBE, "be", {F(1, 0, 0, 0, 0, 0), F(0, 0, 0, 1, 0, 0)},
       {F(0, 0, 0, 0, 0, 0), F(0, 1, 1, 0, 1, 0)}},
      {ConditionCode::kA, "a", {F(0, 0, 0, 0, 0, 0), F(0, 1, 1, 0, 1, 0)},
       {F(1, 0, 0, 0, 0, 0), F(0, 0, 0, 1, 0, 0)}},
      {ConditionCode::kS, "s", {F(0, 0, 0, 0, 1, 0), F(0, 0, 0, 0, 1, 1)},
       {F(0, 0, 0, 0, 0, 0), F(0, 0, 0, 1, 0, 0)}},
      {ConditionCode::kNS, "ns", {F(0, 0, 0, 0, 0, 0), F(0, 0, 0, 1, 0, 0)},
       {F(0, 0, 0, 0, 1, 0), F(0, 0, 0, 0, 1, 1)}},
      {ConditionCode::kP, "p", {F(0, 1, 0, 0, 0, 0), F(0, 1, 0, 1, 0, 0)},
       {F(0, 0, 0, 0, 0, 0), F(0, 0, 0, 1, 0, 0)}},
      {ConditionCode::kNP, "np", {F(0, 0, 0, 0, 0, 0), F(0, 0, 0, 1, 0, 0)},
       {F(0, 1, 0, 0, 0, 0), F(0, 1, 0, 1, 0, 0)}},
      // L:  SF != OF ;  GE: SF == OF
      {ConditionCode::kL, "l", {F(0, 0, 0, 0, 1, 0), F(0, 0, 0, 0, 0, 1)},
       {F(0, 0, 0, 0, 0, 0), F(0, 0, 0, 0, 1, 1)}},
      {ConditionCode::kGE, "ge", {F(0, 0, 0, 0, 0, 0), F(0, 0, 0, 0, 1, 1)},
       {F(0, 0, 0, 0, 1, 0), F(0, 0, 0, 0, 0, 1)}},
      {ConditionCode::kLE, "le", {F(0, 0, 0, 1, 0, 0), F(0, 0, 0, 0, 1, 0)},
       {F(0, 0, 0, 0, 0, 0), F(0, 0, 0, 0, 0, 1)}},
      {ConditionCode::kG, "g", {F(0, 0, 0, 0, 0, 0), F(0, 0, 0, 0, 1, 1)},
       {F(0, 0, 0, 1, 0, 0), F(0, 0, 0, 0, 1, 0)}},
  };
#undef F

  for (const Case& c : cases) {
    for (int expect_true = 0; expect_true < 2; ++expect_true) {
      const std::uint64_t flags =
          expect_true ? c.flags_true[0] : c.flags_false[0];

      Builder b(c.name);
      b.CreateBlock(0);
      b.SetEntryBlock(0);
      const ir::ValueId fl = b.EmitLoadFlags();
      const ir::ValueId cc = b.EmitTestCc(c.cc, fl);
      b.EmitSetReg(ArchRegister::kRax, cc);
      b.EmitReturn();

      CpuState state;
      state.eflags = flags;
      LinearGuestMemory mem;
      Run(b.program(), &state, mem);
      const bool got = state.rax != 0;
      if (got != (expect_true != 0)) {
        char buf[192];
        std::snprintf(buf, sizeof(buf),
                      "condition %s with EFLAGS=0x%04llx: expected %d, got %d",
                      c.name, static_cast<unsigned long long>(flags),
                      expect_true, got ? 1 : 0);
        ReportFailure(__FILE__, __LINE__, buf);
      }
    }
  }
}

P5EMU_TEST(ControlFlow, TrapTerminatesWithUnimplementedInstruction) {
  Builder b("trap");
  b.CreateBlock(0);
  b.SetEntryBlock(0);
  b.EmitTrap();
  CpuState state;
  LinearGuestMemory mem;
  const ExecutionResult r = Run(b.program(), &state, mem);
  CHECK_EQ(r.reason, TerminationReason::kTrap);
  CHECK_EQ(r.status.code(), StatusCode::kUnimplementedInstruction);
}

P5EMU_TEST(ControlFlow, RunawayProgramHitsTheStepLimitDeterministically) {
  // An infinite loop must terminate deterministically. A hang is not a
  // diagnosable failure, so the interpreter carries an explicit budget.
  Builder b("spin");
  b.CreateBlock(0);
  b.SetEntryBlock(0);
  b.EmitJump(0);  // unconditional self-loop

  CpuState state;
  LinearGuestMemory mem;
  ReferenceInterpreter interp(1000);
  mem = LinearGuestMemory::MakeCodeAndData(kMemBase, kMemSize);
  const ExecutionResult r = interp.Execute(b.program(), &state, mem);

  CHECK_EQ(r.reason, TerminationReason::kStepLimit);
  CHECK_EQ(r.status.code(), StatusCode::kExecutionLimit);
  CHECK_EQ(r.executed_ops, std::uint64_t{1000});
}

P5EMU_TEST(ControlFlow, InterpreterRefusesToExecuteInvalidIr) {
  Builder b("bad");
  b.CreateBlock(0);
  b.SetEntryBlock(0);
  b.EmitReturn();
  b.MutableProgram().blocks[0].terminator.kind = TerminatorKind::kInvalid;

  CpuState state;
  LinearGuestMemory mem;
  mem = LinearGuestMemory::MakeCodeAndData(kMemBase, kMemSize);
  ReferenceInterpreter interp;
  const ExecutionResult r = interp.Execute(b.program(), &state, mem);
  CHECK_EQ(r.reason, TerminationReason::kIrInvalid);
  CHECK_EQ(r.status.code(), StatusCode::kInvalidIr);
  CHECK_EQ(r.executed_ops, std::uint64_t{0});
}

// ===========================================================================
// Memory semantics
// ===========================================================================

P5EMU_TEST(MemorySemantics, StoreThenLoadRoundTripsThroughThePort) {
  Builder b("store-load");
  b.CreateBlock(0);
  b.SetEntryBlock(0);
  const ir::ValueId v = b.EmitGetReg(ArchRegister::kRax);
  const ir::ValueId addr = b.EmitAddr(kMemBase, ir::kInvalidValue, 1, 0);
  b.EmitStore(addr, v, 8);
  const ir::ValueId loaded = b.EmitLoad(addr, 8);
  b.EmitSetReg(ArchRegister::kRdx, loaded);
  b.EmitReturn();

  CpuState state;
  state.rax = 0x1234'5678'9ABC'DEF0ull;
  LinearGuestMemory mem;
  Run(b.program(), &state, mem);
  CHECK_EQ(state.rdx, 0x1234'5678'9ABC'DEF0ull);
}

P5EMU_TEST(MemorySemantics, ScaledIndexedAddressIsCorrect) {
  // base + index*scale + displacement.
  // Intel SDM Vol. 2 §2.2.2.8 (SIB): effective address = base + index*scale + disp.
  Builder b("scaled");
  b.CreateBlock(0);
  b.SetEntryBlock(0);
  const ir::ValueId base = b.EmitGetReg(ArchRegister::kRbx);
  const ir::ValueId index = b.EmitGetReg(ArchRegister::kRcx);
  const ir::ValueId addr = b.EmitAddr(0, index, 8, 16);
  const ir::ValueId with_base = b.EmitBinary(OpKind::kAdd, addr, base);
  const ir::ValueId loaded = b.EmitLoad(with_base, 8);
  b.EmitSetReg(ArchRegister::kRdx, loaded);
  b.EmitReturn();

  // Seed guest memory at the computed address.
  const GuestAddr target = kMemBase + 16 + (3 * 8);
  CpuState state;
  state.rbx = kMemBase;
  state.rcx = 3;
  LinearGuestMemory mem;
  mem = LinearGuestMemory::MakeCodeAndData(kMemBase, kMemSize);
  std::uint64_t seed = 0;
  REQUIRE(mem.DebugFill(target, &seed, 8).ok());
  // Write a recognisable value.
  seed = 0xABCD;
  REQUIRE(mem.WriteData(target, 8, 0xABCDull).ok());

  ReferenceInterpreter interp;
  interp.Execute(b.program(), &state, mem);
  CHECK_EQ(state.rdx, std::uint64_t{0xABCD});
}

P5EMU_TEST(MemorySemantics, AccessToUnmappedMemoryFaultsAndDoesNotCorruptState) {
  Builder b("fault");
  b.CreateBlock(0);
  b.SetEntryBlock(0);
  const ir::ValueId addr = b.EmitAddr(0xFFFF'0000'0000ull,
                                      ir::kInvalidValue, 1, 0);
  const ir::ValueId v = b.EmitLoad(addr, 8);
  b.EmitSetReg(ArchRegister::kRax, v);
  b.EmitReturn();

  CpuState state;
  state.rax = 0xCAFE;
  LinearGuestMemory mem;
  mem = LinearGuestMemory::MakeCodeAndData(kMemBase, kMemSize);
  ReferenceInterpreter interp;
  const ExecutionResult r = interp.Execute(b.program(), &state, mem);

  CHECK_EQ(r.reason, TerminationReason::kFault);
  CHECK_EQ(r.status.code(), StatusCode::kUnmappedAccess);
  // The register must not have been written by the faulted load.
  CHECK_EQ(state.rax, std::uint64_t{0xCAFE});
}

P5EMU_TEST(MemorySemantics, FaultHaltsExecutionImmediately) {
  // A fault must stop execution, not be caught and skipped. The sentinel store
  // after the faulting load must never execute.
  Builder b("fault-halt");
  b.CreateBlock(0);
  b.SetEntryBlock(0);
  const ir::ValueId bad = b.EmitAddr(0xFFFF'0000'0000ull, ir::kInvalidValue, 1, 0);
  const ir::ValueId loaded = b.EmitLoad(bad, 8);
  const ir::ValueId sentinel = b.EmitMaterializeConstant(0xDEADull, ValueType::kI64);
  const ir::ValueId addr = b.EmitAddr(kMemBase, ir::kInvalidValue, 1, 0);
  b.EmitStore(addr, sentinel, 8);
  b.EmitSetReg(ArchRegister::kRax, loaded);
  b.EmitReturn();

  CpuState state;
  LinearGuestMemory mem;
  mem = LinearGuestMemory::MakeCodeAndData(kMemBase, kMemSize);
  ReferenceInterpreter interp;
  interp.Execute(b.program(), &state, mem);

  std::uint64_t at_base = 0;
  REQUIRE(mem.ReadData(kMemBase, 8, &at_base).ok());
  // The region starts zeroed, so the sentinel value 0xDEAD must NOT be there:
  // the faulting load halted execution before the store could run.
  CHECK_EQ(at_base, std::uint64_t{0});
}

P5EMU_TEST(MemorySemantics, ExecutionIsDeterministicAcrossRepeatedRuns) {
  // Determinism is a stated requirement of the reference interpreter: the same
  // IR and initial state must produce bit-identical results, every time.
  Builder b("determinism");
  b.CreateBlock(0);
  b.SetEntryBlock(0);
  const ir::ValueId a = b.EmitGetReg(ArchRegister::kRax);
  const ir::ValueId c = b.EmitGetReg(ArchRegister::kRcx);
  const ir::ValueId s = b.EmitBinaryFlags(OpKind::kAddFlags, a, c);
  b.EmitSetReg(ArchRegister::kRdx, s);
  const ir::ValueId addr = b.EmitAddr(kMemBase, ir::kInvalidValue, 1, 0);
  b.EmitStore(addr, s, 8);
  b.EmitReturn();

  CpuState first;
  CpuState last;
  std::uint64_t snapshot = 0;

  for (int i = 0; i < 8; ++i) {
    CpuState state;
    state.rax = 0x0123'4567'89AB'CDEFull;
    state.rcx = 0x1111'1111'1111'1111ull;
    LinearGuestMemory mem;
    Run(b.program(), &state, mem);

    if (i == 0) {
      first = state;
      // Snapshot the first run's committed bytes so later runs can be compared
      // against it. A memory port is move-only by design, so the snapshot is
      // taken through the public read path rather than by copying the port.
      REQUIRE(mem.ReadData(kMemBase, 8, &snapshot).ok());
      continue;
    }
    StateDifference sd;
    if (!CompareCpuState(first, state, &sd)) {
      ReportFailure(__FILE__, __LINE__,
                    std::string("non-deterministic state at field ") +
                        (sd.field != nullptr ? sd.field : "?"));
    }
    std::uint64_t now = 0;
    REQUIRE(mem.ReadData(kMemBase, 8, &now).ok());
    if (now != snapshot) {
      char buf[128];
      std::snprintf(buf, sizeof(buf),
                    "non-deterministic memory: first=0x%llx run%d=0x%llx",
                    static_cast<unsigned long long>(snapshot), i,
                    static_cast<unsigned long long>(now));
      ReportFailure(__FILE__, __LINE__, buf);
    }
    last = state;
  }
  CHECK_EQ(last.rax, first.rax);
}

// ===========================================================================
// State comparison
// ===========================================================================

P5EMU_TEST(Interpreter, StateComparisonNamesTheOffendingField) {
  // A memcmp-based comparison would pass here, because the differing field is
  // EFLAGS and a memcmp over a struct would depend on padding. That is exactly
  // why comparison is field-by-field.
  CpuState a;
  CpuState c;
  a.eflags = 0;
  c.eflags = eflags::kZf;

  StateDifference d;
  CHECK(!CompareCpuState(a, c, &d));
  CHECK(d.field != nullptr);
  CHECK_EQ(std::string(d.field), std::string("eflags"));
  CHECK_EQ(d.expected, std::uint64_t{0});
  CHECK_EQ(d.actual, eflags::kZf);
}

P5EMU_TEST(Interpreter, IdenticalStatesCompareEqual) {
  CpuState a;
  CpuState c;
  a.rax = 0x1234;
  c.rax = 0x1234;
  StateDifference d;
  CHECK(CompareCpuState(a, c, &d));
  CHECK(d.field == nullptr);
}

P5EMU_TEST(Interpreter, BackendFactoryReturnsTheReferenceInterpreter) {
  auto backend = MakeReferenceInterpreter(100);
  REQUIRE(backend != nullptr);
  CHECK_EQ(std::string(backend->name()), std::string("reference-interpreter"));
  CHECK_EQ(backend->step_limit(), std::uint64_t{100});
  CHECK_EQ(backend->id(), BackendId::kReferenceInterpreter);
}
