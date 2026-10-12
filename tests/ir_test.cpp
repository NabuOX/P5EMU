// SPDX-License-Identifier: LicenseRef-P5EMU-Proprietary
//
// IR construction and ValidateIr tests.
//
// THE POINT OF THE NEGATIVE TESTS
//
// ValidateIr only has value if it is observed REJECTING malformed IR. The
// positive tests prove it accepts good IR; the negative tests prove the specific
// rule fires. Every rule listed in IrValidate.cpp's header has at least one
// negative test here that asserts the RULE IDENTIFIER, not the message text --
// so improving diagnostics never breaks the tests, but removing a check does.

#include <string>
#include <vector>

#include "TestHarness.h"
#include "p5emu/cpu/IrBuilder.h"

namespace {

using namespace p5emu;   // NOLINT - test-local convenience
using namespace p5emu::ir;

namespace {

// Runs validation and returns the rule identifier that fired, or "" when valid.
std::string ValidationRule(const Program& program) {
  ValidationReport report;
  const Status st = ValidateIrDetailed(program, &report);
  if (st.ok()) {
    return std::string();
  }
  return std::string(report.rule);
}

}  // namespace

// ===========================================================================
// Positive
// ===========================================================================

P5EMU_TEST(IrValidate, AcceptsAMinimalValidProgram) {
  Builder b("valid-minimal");
  b.CreateBlock(0x1000);
  b.SetEntryBlock(0);
  const ir::ValueId a = b.EmitGetReg(ArchRegister::kRax);
  const ir::ValueId sum = b.EmitBinary(OpKind::kAdd, a, a);
  b.EmitSetReg(ArchRegister::kRbx, sum);
  b.EmitReturn();

  const Status st = ValidateIr(b.program());
  if (!st.ok()) {
    // Report the reason; a bare failure here is undiagnosable.
    ValidationReport report;
    (void)ValidateIrDetailed(b.program(), &report);
    ::p5emu::test::ReportFailure(__FILE__, __LINE__,
                                 std::string("valid IR rejected: ") + report.rule +
                                     " " + report.detail);
  }
  CHECK(st.ok());
}

P5EMU_TEST(IrValidate, AcceptsControlFlow) {
  Builder b("valid-cfg");
  b.CreateBlock(0x1000);        // b0
  b.SetEntryBlock(0);
  const ir::ValueId a = b.EmitGetReg(ArchRegister::kRax);
  const ir::ValueId flags = b.EmitCompare(a, a);
  const ir::BlockId b1 = b.CreateBlock(0x1010);
  const ir::BlockId b2 = b.CreateBlock(0x1020);
  b.SetCurrentBlock(0);
  b.EmitBranch(ConditionCode::kE, flags, b1, b2);

  b.SetCurrentBlock(b1);
  b.EmitReturn();
  b.SetCurrentBlock(b2);
  b.EmitReturn();

  CHECK(ValidateIr(b.program()).ok());
}

P5EMU_TEST(IrValidate, AcceptsMemoryAndAddressCalculation) {
  Builder b("valid-mem");
  b.CreateBlock(0);
  b.SetEntryBlock(0);
  const ir::ValueId base = b.EmitGetReg(ArchRegister::kRbx);
  const ir::ValueId idx = b.EmitGetReg(ArchRegister::kRcx);
  const ir::ValueId addr = b.EmitAddr(0x2000, idx, 4, 8);
  const ir::ValueId loaded = b.EmitLoad(addr, 4);
  b.EmitStore(addr, loaded, 4);
  const ir::ValueId abs = b.EmitAddr(0x4000, ir::kInvalidValue, 1, 0);
  const ir::ValueId v = b.EmitBinary(OpKind::kAdd, loaded, abs);
  b.EmitSetReg(ArchRegister::kRdx, v);
  (void)base;
  b.EmitReturn();
  CHECK(ValidateIr(b.program()).ok());
}

P5EMU_TEST(IrBuild, DumpIsDeterministic) {
  // The dump format is a contract: golden files and cross-backend comparison
  // both depend on it being byte-stable for a given Program.
  Builder b("dump-determinism");
  b.CreateBlock(0x1000);
  b.SetEntryBlock(0);
  const ir::ValueId a = b.EmitGetReg(ArchRegister::kRax);
  const ir::ValueId k = b.EmitMaterializeConstant(0x1234, ValueType::kI64);
  const ir::ValueId s = b.EmitBinary(OpKind::kXor, a, k);
  b.EmitSetReg(ArchRegister::kRcx, s);
  b.EmitReturn();

  const std::string first = Dump(b.program());
  const std::string second = Dump(b.program());
  CHECK_EQ(first, second);

  // And it must not be empty, and must name the program.
  CHECK(!first.empty());
  CHECK(first.find("dump-determinism") != std::string::npos);
  CHECK(first.find("xor") != std::string::npos);
}

P5EMU_TEST(IrBuild, DumpIsIdenticalForTwoIdenticallyBuiltPrograms) {
  auto build = [](const char* name) {
    Builder b(name);
    b.CreateBlock(0x40);
    b.SetEntryBlock(0);
    const ir::ValueId a = b.EmitGetReg(ArchRegister::kRsi);
    const ir::ValueId one = b.EmitMaterializeConstant(1, ValueType::kI64);
    const ir::ValueId r = b.EmitBinary(OpKind::kSub, a, one);
    const ir::ValueId fl = b.EmitCompare(a, r);
    const ir::ValueId cc = b.EmitTestCc(ConditionCode::kNE, fl);
    b.EmitSetReg(ArchRegister::kRdi, cc);
    b.EmitReturn();
    return b.MutableProgram();
  };

  const Program p1 = build("same");
  const Program p2 = build("same");
  CHECK_EQ(Dump(p1), Dump(p2));
}

P5EMU_TEST(IrBuild, BuilderRefusesUntypedValues) {
  Builder b("no-untyped");
  b.CreateBlock(0);
  CHECK_EQ(b.DefineValue(ValueType::kInvalid), ir::kInvalidValue);
}

// ===========================================================================
// Negative -- one per rule
// ===========================================================================

P5EMU_TEST(IrValidate, RejectsEmptyProgram) {
  Program p;
  p.name = "empty";
  CHECK_EQ(ValidationRule(p), std::string("empty-program"));
}

P5EMU_TEST(IrValidate, RejectsInvalidEntry) {
  Builder b("bad-entry");
  b.CreateBlock(0);
  b.SetEntryBlock(99);  // no such block
  b.EmitReturn();
  CHECK_EQ(ValidationRule(b.program()), std::string("invalid-entry"));
}

P5EMU_TEST(IrValidate, RejectsDuplicateBlockIds) {
  Builder b("dup-block");
  b.CreateBlock(0);
  b.SetEntryBlock(0);
  b.EmitReturn();
  // Force a duplicate id.
  b.MutableProgram().blocks.push_back(b.MutableProgram().blocks[0]);
  CHECK_EQ(ValidationRule(b.program()), std::string("duplicate-block-id"));
}

P5EMU_TEST(IrValidate, RejectsUndefinedBlockTarget) {
  Builder b("bad-jump");
  b.CreateBlock(0);
  b.SetEntryBlock(0);
  b.EmitJump(77);  // no such block
  CHECK_EQ(ValidationRule(b.program()), std::string("undefined-block"));
}

P5EMU_TEST(IrValidate, RejectsInvalidTerminator) {
  Builder b("bad-terminator");
  b.CreateBlock(0);
  b.SetEntryBlock(0);
  // Leave the terminator as kInvalid.
  b.MutableProgram().blocks[0].terminator.kind = TerminatorKind::kInvalid;
  CHECK_EQ(ValidationRule(b.program()), std::string("invalid-terminator"));
}

P5EMU_TEST(IrValidate, RejectsTerminatorWithStrayTarget) {
  Builder b("stray-target");
  b.CreateBlock(0);
  b.SetEntryBlock(0);
  b.EmitReturn();
  b.MutableProgram().blocks[0].terminator.target = 0;  // return must not have one
  CHECK_EQ(ValidationRule(b.program()), std::string("malformed-terminator"));
}

P5EMU_TEST(IrValidate, RejectsTerminatorWithStrayCondition) {
  Builder b("stray-cond");
  b.CreateBlock(0);
  b.SetEntryBlock(0);
  b.EmitReturn();
  b.MutableProgram().blocks[0].terminator.cond_value = 0;
  CHECK_EQ(ValidationRule(b.program()), std::string("malformed-terminator"));
}

P5EMU_TEST(IrValidate, RejectsBranchWithMissingNotTakenSuccessor) {
  Builder b("branch-no-fallthrough");
  b.CreateBlock(0);
  b.SetEntryBlock(0);
  const ir::ValueId a = b.EmitGetReg(ArchRegister::kRax);
  const ir::ValueId fl = b.EmitCompare(a, a);
  const ir::BlockId b1 = b.CreateBlock(0x10);
  b.SetCurrentBlock(b1);
  b.EmitReturn();
  b.SetCurrentBlock(0);
  // taken = b1, not_taken left unset
  b.EmitBranch(ConditionCode::kE, fl, b1, ir::kInvalidBlock);
  CHECK_EQ(ValidationRule(b.program()), std::string("undefined-block"));
}

P5EMU_TEST(IrValidate, RejectsBranchWithIdenticalSuccessors) {
  Builder b("branch-same-target");
  b.CreateBlock(0);
  b.SetEntryBlock(0);
  const ir::ValueId a = b.EmitGetReg(ArchRegister::kRax);
  const ir::ValueId fl = b.EmitCompare(a, a);
  const ir::BlockId b1 = b.CreateBlock(0x10);
  b.SetCurrentBlock(b1);
  b.EmitReturn();
  b.SetCurrentBlock(0);
  b.EmitBranch(ConditionCode::kE, fl, b1, b1);
  CHECK_EQ(ValidationRule(b.program()), std::string("malformed-terminator"));
}

P5EMU_TEST(IrValidate, RejectsBranchConditionThatIsNotFlags) {
  Builder b("branch-bad-type");
  b.CreateBlock(0);
  b.SetEntryBlock(0);
  const ir::ValueId a = b.EmitGetReg(ArchRegister::kRax);  // i64, not flags
  const ir::BlockId b1 = b.CreateBlock(0x10);
  b.SetCurrentBlock(b1);
  b.EmitReturn();
  b.SetCurrentBlock(0);
  b.EmitBranch(ConditionCode::kE, a, b1, b1);
  // Fix the successor so the TYPE error is what fires.
  b.MutableProgram().blocks[0].terminator.fallthrough = b1;
  const ir::BlockId b2 = b.CreateBlock(0x20);
  b.SetCurrentBlock(b2);
  b.EmitReturn();
  b.MutableProgram().blocks[0].terminator.fallthrough = b2;
  CHECK_EQ(ValidationRule(b.program()), std::string("value-type-mismatch"));
}

P5EMU_TEST(IrValidate, RejectsInvalidOp) {
  Builder b("bad-op");
  b.CreateBlock(0);
  b.SetEntryBlock(0);
  Op op;
  op.kind = OpKind::kInvalid;
  b.Append(op);
  b.EmitReturn();
  CHECK_EQ(ValidationRule(b.program()), std::string("invalid-op"));
}

P5EMU_TEST(IrValidate, RejectsImpossibleOperandCount) {
  Builder b("wrong-arity");
  b.CreateBlock(0);
  b.SetEntryBlock(0);
  Op op;
  op.kind = OpKind::kAdd;
  op.dest = b.DefineValue(ValueType::kI64);
  op.args = {0};              // kAdd needs 2
  op.roles = {OperandRole::kSrc};
  b.Append(op);
  b.EmitReturn();
  CHECK_EQ(ValidationRule(b.program()), std::string("impossible-operand-count"));
}

P5EMU_TEST(IrValidate, RejectsRoleLengthMismatch) {
  Builder b("role-mismatch");
  b.CreateBlock(0);
  b.SetEntryBlock(0);
  Op op;
  op.kind = OpKind::kAdd;
  op.dest = b.DefineValue(ValueType::kI64);
  op.args = {0, 1};
  op.roles = {OperandRole::kSrc};  // only 1 role for 2 args
  b.Append(op);
  b.EmitReturn();
  CHECK_EQ(ValidationRule(b.program()), std::string("impossible-operand-count"));
}

P5EMU_TEST(IrValidate, RejectsWrongRoleKind) {
  // kAdd's second operand must be kSrc2. Declaring it kFlags is a structural
  // error, caught before any type question is asked.
  Builder b("wrong-role");
  b.CreateBlock(0);
  b.SetEntryBlock(0);
  const ir::ValueId a = b.EmitGetReg(ArchRegister::kRax);
  const ir::ValueId f = b.EmitCompare(a, a);
  Op op;
  op.kind = OpKind::kAdd;
  op.dest = b.DefineValue(ValueType::kI64);
  op.args = {a, f};
  op.roles = {OperandRole::kSrc, OperandRole::kFlags};
  b.Append(op);
  b.EmitReturn();
  CHECK_EQ(ValidationRule(b.program()), std::string("unknown-role"));
}

P5EMU_TEST(IrValidate, RejectsUndefinedValueReference) {
  Builder b("undefined-value");
  b.CreateBlock(0);
  b.SetEntryBlock(0);
  Op op;
  op.kind = OpKind::kAdd;
  op.dest = b.DefineValue(ValueType::kI64);
  op.args = {99, 100};  // never defined
  op.roles = {OperandRole::kSrc, OperandRole::kSrc2};
  b.Append(op);
  b.EmitReturn();
  CHECK_EQ(ValidationRule(b.program()), std::string("undefined-value"));
}

P5EMU_TEST(IrValidate, RejectsDuplicateDefinition) {
  Builder b("dup-def");
  b.CreateBlock(0);
  b.SetEntryBlock(0);
  const ir::ValueId a = b.EmitGetReg(ArchRegister::kRax);
  Op op;
  op.kind = OpKind::kAdd;
  op.dest = a;  // a is already defined by kGetReg
  op.args = {a, a};
  op.roles = {OperandRole::kSrc, OperandRole::kSrc2};
  b.Append(op);
  b.EmitReturn();
  CHECK_EQ(ValidationRule(b.program()), std::string("duplicate-definition"));
}

P5EMU_TEST(IrValidate, RejectsUseBeforeDefinition) {
  Builder b("use-before-def");
  b.CreateBlock(0);
  b.SetEntryBlock(0);
  Op first;
  first.kind = OpKind::kAdd;
  first.dest = 0;
  first.args = {1, 1};  // v1 used, but v1 is defined LATER
  first.roles = {OperandRole::kSrc, OperandRole::kSrc2};
  b.Append(first);

  Op second;
  second.kind = OpKind::kGetReg;
  second.dest = 1;
  second.reg = ArchRegister::kRax;
  b.Append(second);

  b.EmitReturn();
  CHECK_EQ(ValidationRule(b.program()), std::string("use-before-definition"));
}

P5EMU_TEST(IrValidate, RejectsTypeMismatchFlagsOperand) {
  Builder b("flags-type");
  b.CreateBlock(0);
  b.SetEntryBlock(0);
  const ir::ValueId a = b.EmitGetReg(ArchRegister::kRax);  // i64
  Op op;
  op.kind = OpKind::kLoadFlags;
  op.dest = b.DefineValue(ValueType::kFlags);
  b.Append(op);
  Op cc;
  cc.kind = OpKind::kTestCc;
  cc.dest = b.DefineValue(ValueType::kI8);
  cc.cond = ConditionCode::kE;
  cc.args = {a};  // i64 where flags are required
  cc.roles = {OperandRole::kFlags};
  b.Append(cc);
  b.EmitReturn();
  CHECK_EQ(ValidationRule(b.program()), std::string("value-type-mismatch"));
}

P5EMU_TEST(IrValidate, RejectsInvalidRegisterIdentifier) {
  Builder b("bad-reg");
  b.CreateBlock(0);
  b.SetEntryBlock(0);
  Op op;
  op.kind = OpKind::kGetReg;
  op.dest = b.DefineValue(ValueType::kI64);
  op.reg = ArchRegister::kInvalidSentinel;
  b.Append(op);
  b.EmitReturn();
  CHECK_EQ(ValidationRule(b.program()), std::string("invalid-register"));

  // And the kInvalid register.
  Builder b2("bad-reg2");
  b2.CreateBlock(0);
  b2.SetEntryBlock(0);
  Op op2;
  op2.kind = OpKind::kGetReg;
  op2.dest = 0;
  op2.reg = ArchRegister::kInvalid;
  b2.Append(op2);
  b2.EmitReturn();
  CHECK_EQ(ValidationRule(b2.program()), std::string("invalid-register"));
}

P5EMU_TEST(IrValidate, RejectsMalformedMemoryOperationWidth) {
  Builder b("bad-width");
  b.CreateBlock(0);
  b.SetEntryBlock(0);
  const ir::ValueId a = b.EmitMaterializeConstant(0x1000, ValueType::kI64);
  Op op;
  op.kind = OpKind::kLoad;
  op.dest = b.DefineValue(ValueType::kI64);
  op.width = 3;  // not 1/2/4/8
  op.args = {a};
  op.roles = {OperandRole::kAddr};
  b.Append(op);
  b.EmitReturn();
  CHECK_EQ(ValidationRule(b.program()), std::string("malformed-memory-op"));
}

P5EMU_TEST(IrValidate, RejectsAddressCalculationWithTooManyOperands) {
  Builder b("addr-arity");
  b.CreateBlock(0);
  b.SetEntryBlock(0);
  const ir::ValueId a = b.EmitGetReg(ArchRegister::kRax);
  Op op;
  op.kind = OpKind::kAddrCalc;
  op.dest = b.DefineValue(ValueType::kI64);
  op.imm = 0x1000;
  op.width = 1;
  op.args = {a, a};  // addr_calc takes at most 1 index
  op.roles = {OperandRole::kSrc, OperandRole::kSrc};
  b.Append(op);
  b.EmitReturn();
  CHECK_EQ(ValidationRule(b.program()), std::string("impossible-operand-count"));
}

P5EMU_TEST(IrValidate, RejectsAddressCalculationWithBadScale) {
  Builder b("addr-scale");
  b.CreateBlock(0);
  b.SetEntryBlock(0);
  const ir::ValueId a = b.EmitGetReg(ArchRegister::kRax);
  Op op;
  op.kind = OpKind::kAddrCalc;
  op.dest = b.DefineValue(ValueType::kI64);
  op.imm = 0x1000;
  op.width = 3;  // scale must be 1/2/4/8
  op.args = {a};
  op.roles = {OperandRole::kSrc};
  b.Append(op);
  b.EmitReturn();
  CHECK_EQ(ValidationRule(b.program()), std::string("malformed-memory-op"));
}

P5EMU_TEST(IrValidate, RejectsCrossBlockValueUse) {
  // Phase 1 forbids using a value defined in a LATER block, because that would
  // make the SSA property depend on an unstated control-flow assumption.
  //
  // The rule that fires is the SPECIFIC one (cross-block-ssa-violation) rather
  // than the general ordering rule, because the validator checks block ordering
  // before the general use-before-definition test. Asserting the specific rule
  // is deliberate: it pins the diagnosis a developer will actually see.
  Builder b("cross-block");
  b.CreateBlock(0);
  b.SetEntryBlock(0);
  b.EmitJump(1);                 // b0 jumps to b1
  const ir::BlockId b1 = b.CreateBlock(0x10);
  const ir::ValueId late = b.EmitGetReg(ArchRegister::kRax);
  b.EmitReturn();
  b.SetCurrentBlock(0);
  Op op;                          // b0 now uses a value defined in b1
  op.kind = OpKind::kSetReg;
  op.reg = ArchRegister::kRbx;
  op.args = {late};
  op.roles = {OperandRole::kSrc};
  b.Append(op);
  b.MutableProgram().blocks[0].terminator.kind = TerminatorKind::kJump;
  b.MutableProgram().blocks[0].terminator.target = b1;
  CHECK_EQ(ValidationRule(b.program()),
           std::string("cross-block-ssa-violation"));
}

P5EMU_TEST(IrValidate, DetailedReportCarriesTheOffendingLocation) {
  Builder b("report-detail");
  b.CreateBlock(7);
  b.SetEntryBlock(0);
  Op op;
  op.kind = OpKind::kAdd;
  op.dest = b.DefineValue(ValueType::kI64);
  op.args = {55, 66};
  op.roles = {OperandRole::kSrc, OperandRole::kSrc2};
  b.Append(op);
  b.EmitReturn();

  ValidationReport report;
  const Status st = ValidateIrDetailed(b.program(), &report);
  CHECK(!st.ok());
  CHECK(!report.valid);
  CHECK_EQ(std::string(report.rule), std::string("undefined-value"));
  CHECK_EQ(report.value, ir::ValueId{55});
  CHECK(!report.detail.empty());
}

P5EMU_TEST(IrValidate, NullReportIsRejected) {
  Builder b("null-report");
  b.CreateBlock(0);
  b.SetEntryBlock(0);
  b.EmitReturn();
  CHECK(!ValidateIrDetailed(b.program(), nullptr).ok());
}

}  // namespace
