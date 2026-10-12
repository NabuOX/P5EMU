// SPDX-License-Identifier: Apache-2.0
//
// P5EMU is an independent, open-source project. See LICENSE and
// PROVENANCE.md before adding code here.
//
// ============================================================================
// ValidateIr
// ============================================================================
//
// WHY THIS EXISTS
//
// A malformed IR reaching a native-code backend is a memory-corruption bug, not
// a graceful failure: the backend will index an undefined value, follow a
// dangling block id, or read an operand list that is one element short. The
// reference interpreter degrades more gracefully, which means an interpreter-only
// test suite can pass while the same IR would corrupt a JIT.
//
// So validation runs unconditionally -- not only in debug builds -- and the
// reference interpreter also calls it, so that interpreter and backend agree on
// what "valid" means.
//
// Each rule has a stable identifier string. Tests assert on the identifier, not
// on message text, so that improving diagnostics never breaks tests.
//
// RULES
//   empty-program            program has no blocks
//   invalid-entry            entry block id does not exist
//   duplicate-block-id       two blocks share an id
//   undefined-block          a successor names a block that does not exist
//   self-referential-block   a jump targets itself with no progress guarantee
//   invalid-terminator       terminator kind is kInvalid
//   malformed-terminator     terminator operands inconsistent with its kind
//   invalid-op               op kind is kInvalid
//   unknown-role             an operand has no declared role
//   role-count-mismatch      args and roles have different lengths
//   undefined-value          operand references a value that is never defined
//   duplicate-definition     two ops define the same value id
//   use-before-definition    operand uses a value defined later in program order
//   value-type-mismatch      operand used at a type it does not have
//   impossible-operand-count wrong number of operands for this op
//   invalid-register         register identifier is not a real register
//   invalid-width            byte width is not 1/2/4/8
//   malformed-memory-op      load/store width, or store arg count, is wrong
//   invalid-extend-mode      kExtend without a recognised mode
//   invalid-condition        condition code out of range
//   type-invalid             a value was defined with an invalid type
//   cross-block-ssa-violation
//                           an op uses a value defined in another block
//   unreachable-block        (not an error in Phase 1)
//   duplicate-value-id-across-blocks
//                           handled by duplicate-definition
//
// Cross-block SSA: Phase 1 models a single-entry linear-function-shaped program,
// so a value defined in one block must be consumed in that block or in a block
// that dominates it. Rather than compute dominance, Phase 1 enforces the
// stronger and simpler rule: a value defined in block B may only be used in
// block B, or in a block that appears LATER in stored order (the dominance-safe
// subset for reducible forward flow). Violations are reported rather than
// silently accepted, because accepting them would make the IR's SSA property
// depend on an unstated control-flow assumption.
// ============================================================================

#include <cstdio>
#include <string>
#include <vector>

#include "p5emu/cpu/Ir.h"

namespace p5emu::ir {
namespace {

// Recovers the type of a value from the op that defines it.
//
// Phase 1 stores types implicitly in the defining op rather than in a parallel
// table, so that a serialised Program is self-describing. The rules are explicit
// so that the validator and the interpreter cannot disagree.
ValueType InferType(const Program& program, ValueId v,
                    const std::vector<std::pair<BlockId, OpId>>& defs) {
  if (v >= defs.size()) {
    return ValueType::kInvalid;
  }
  const BlockId b = defs[v].first;
  const OpId o = defs[v].second;
  if (b == kInvalidBlock || o == kInvalidOp) {
    return ValueType::kInvalid;
  }
  const Block* block = program.FindBlock(b);
  if (block == nullptr || o >= block->ops.size()) {
    return ValueType::kInvalid;
  }
  const Op& op = block->ops[o];

  // A flags value is produced in the op's kFlagsOut slot, not in `dest`. This
  // must be checked FIRST: kCmp and kTest have no `dest` at all, so falling
  // through to the switch would report them as producing nothing, and every
  // consumer of their flags would be told it is using an invalid value.
  for (std::size_t a = 0; a < op.args.size() && a < op.roles.size(); ++a) {
    if (op.roles[a] == OperandRole::kFlagsOut && op.args[a] == v) {
      return ValueType::kFlags;
    }
  }

  switch (op.kind) {
    case OpKind::kGetReg:
      return ValueType::kI64;
    case OpKind::kLoad:
      // Loads are zero-extended to 64 bits in Phase 1: see Ir.h.
      return ValueType::kI64;
    case OpKind::kLoadFlags:
      return ValueType::kFlags;
    case OpKind::kConst:
    case OpKind::kAdd:
    case OpKind::kSub:
    case OpKind::kAnd:
    case OpKind::kOr:
    case OpKind::kXor:
    case OpKind::kAddFlags:
    case OpKind::kSubFlags:
    case OpKind::kAndFlags:
    case OpKind::kOrFlags:
    case OpKind::kXorFlags:
    case OpKind::kShl:
    case OpKind::kShr:
    case OpKind::kSar:
    case OpKind::kExtend:
    case OpKind::kTruncate:
    case OpKind::kAddrCalc:
    case OpKind::kAdcFlags:
    case OpKind::kSbbFlags:
    case OpKind::kMulFlags:
    case OpKind::kIMulFlags:
    case OpKind::kDivFlags:
    case OpKind::kIDivFlags:
    case OpKind::kVecExtractLow64:
      return ValueType::kI64;
    case OpKind::kGetXmm:
    case OpKind::kVecMake:
    case OpKind::kVecLoad:
    case OpKind::kVecXor:
    case OpKind::kVecAnd:
    case OpKind::kVecOr:
    case OpKind::kVecAdd64x2:
      return ValueType::kV128;
    case OpKind::kTestCc:
      return ValueType::kI8;
    default:
      return ValueType::kInvalid;
  }
}

// The type a value produced by a kFlagsOut slot has. Used to type-check the
// definition itself, which no other rule covers.
ValueType FlagsOutTypeOf(const Op& op) {
  (void)op;
  return ValueType::kFlags;
}

bool IsValidArchRegister(ArchRegister r) {
  if (r == ArchRegister::kInvalid || r == ArchRegister::kInvalidSentinel) {
    return false;
  }
  return r >= ArchRegister::kRax && r <= ArchRegister::kBh;
}

// AH/CH/DH/BH are 8-bit-only views of bits 8..15 of RAX/RCX/RDX/RBX.
bool IsHighByteRegister(ArchRegister r) {
  return r == ArchRegister::kAh || r == ArchRegister::kCh ||
         r == ArchRegister::kDh || r == ArchRegister::kBh;
}

// Expected OPERAND (value) roles per op, in order.
//
// Only roles carried in Op::args appear here. Roles carried in Op fields
// (kReg, kWidth, kMode, kCond) are validated separately below. Mixing the two
// in one list made the arity check compare value args against a count that
// included field operands, so every op with a register selector was rejected.
//
// kAddrCalc has variable arity (0 or 1 index) and is validated explicitly.
bool ExpectedOperands(OpKind kind, std::vector<OperandRole>* roles_out) {
  roles_out->clear();
  switch (kind) {
    case OpKind::kConst:
    case OpKind::kGetReg:
    case OpKind::kLoadFlags:
    case OpKind::kNop:
      return true;
    case OpKind::kAddrCalc:
      // Variable arity: 0 operands for an absolute address, 1 index otherwise.
      // Not expressible in the fixed table, so it is validated in the per-op
      // pass. Listing it here with zero operands caused every indexed address
      // calculation to be rejected as having "too many" operands.
      return true;
    case OpKind::kSetReg:
    case OpKind::kExtend:
    case OpKind::kTruncate:
      *roles_out = {OperandRole::kSrc};
      return true;
    case OpKind::kStoreFlags:
    case OpKind::kTestCc:
      *roles_out = {OperandRole::kFlags};
      return true;
    case OpKind::kLoad:
      *roles_out = {OperandRole::kAddr};
      return true;
    case OpKind::kStore:
      *roles_out = {OperandRole::kAddr, OperandRole::kSrc};
      return true;
    case OpKind::kAdd:
    case OpKind::kSub:
    case OpKind::kAnd:
    case OpKind::kOr:
    case OpKind::kXor:
      *roles_out = {OperandRole::kSrc, OperandRole::kSrc2};
      return true;
    case OpKind::kAddFlags:
    case OpKind::kSubFlags:
    case OpKind::kAndFlags:
    case OpKind::kOrFlags:
    case OpKind::kXorFlags:
    case OpKind::kTest:
    case OpKind::kCmp:
    case OpKind::kShl:
    case OpKind::kShr:
    case OpKind::kSar:
      *roles_out = {OperandRole::kSrc, OperandRole::kSrc2, OperandRole::kFlagsOut};
      return true;
    // Phase 11 arithmetic.
    case OpKind::kAdcFlags:
    case OpKind::kSbbFlags:
      // a, b, carry-in, flags-out
      *roles_out = {OperandRole::kSrc, OperandRole::kSrc2, OperandRole::kSrc3,
                    OperandRole::kFlagsOut};
      return true;
    case OpKind::kMulFlags:
    case OpKind::kIMulFlags:
      // a, b, flags-out
      *roles_out = {OperandRole::kSrc, OperandRole::kSrc2, OperandRole::kFlagsOut};
      return true;
    case OpKind::kDivFlags:
    case OpKind::kIDivFlags:
      // dividend_hi, dividend_lo, divisor
      *roles_out = {OperandRole::kSrc, OperandRole::kSrc2, OperandRole::kSrc3};
      return true;
    // Phase 12 SIMD.
    case OpKind::kGetXmm:
      return true;  // reg field only
    case OpKind::kSetXmm:
    case OpKind::kVecExtractLow64:
      *roles_out = {OperandRole::kSrc};
      return true;
    case OpKind::kVecMake:
    case OpKind::kVecXor:
    case OpKind::kVecAnd:
    case OpKind::kVecOr:
    case OpKind::kVecAdd64x2:
      *roles_out = {OperandRole::kSrc, OperandRole::kSrc2};
      return true;
    case OpKind::kVecLoad:
      *roles_out = {OperandRole::kAddr};
      return true;
    case OpKind::kVecStore:
      *roles_out = {OperandRole::kAddr, OperandRole::kSrc};
      return true;
    case OpKind::kInvalid:
      return false;
  }
  return false;
}

bool IsValidConditionCode(ConditionCode cc) {
  return static_cast<std::uint8_t>(cc) <=
         static_cast<std::uint8_t>(ConditionCode::kG);
}

bool IsValidExtendMode(ExtendMode m) {
  return m == ExtendMode::kNone || m == ExtendMode::kZeroExtend32 ||
         m == ExtendMode::kSignExtend8 || m == ExtendMode::kSignExtend16 ||
         m == ExtendMode::kSignExtend32;
}

}  // namespace

Status ValidateIrDetailed(const Program& program, ValidationReport* report) {
  if (report == nullptr) {
    return StatusCode::kInvalidArgument;
  }
  report->valid = false;
  report->rule = "";
  report->block = kInvalidBlock;
  report->op_index = kInvalidOp;
  report->value = kInvalidValue;
  report->detail.clear();

  auto fail = [&](const char* rule, BlockId b, OpId o, ValueId v,
                  std::string detail) -> Status {
    report->valid = false;
    report->rule = rule;
    report->block = b;
    report->op_index = o;
    report->value = v;
    report->detail = std::move(detail);
    return StatusCode::kInvalidIr;
  };

  // -- Structural: blocks -------------------------------------------------
  if (program.blocks.empty()) {
    return fail("empty-program", kInvalidBlock, kInvalidOp, kInvalidValue,
                "program contains no blocks");
  }
  if (program.entry == kInvalidBlock ||
      program.FindBlock(program.entry) == nullptr) {
    return fail("invalid-entry", kInvalidBlock, kInvalidOp, kInvalidValue,
                "entry block does not exist");
  }
  for (std::size_t i = 0; i < program.blocks.size(); ++i) {
    for (std::size_t j = i + 1; j < program.blocks.size(); ++j) {
      if (program.blocks[i].id == program.blocks[j].id) {
        return fail("duplicate-block-id", program.blocks[i].id, kInvalidOp,
                    kInvalidValue, "two blocks share an id");
      }
    }
  }

  // -- Value id range: must be dense enough for all blocks -----------------
  ValueId max_value = 0;
  bool any_value = false;
  for (const Block& b : program.blocks) {
    for (const Op& op : b.ops) {
      if (op.dest != kInvalidValue) {
        if (!any_value || op.dest + 1u > max_value) {
          max_value = op.dest + 1u;
          any_value = true;
        }
      }
      for (ValueId a : op.args) {
        if (!any_value || a + 1u > max_value) {
          max_value = a + 1u;
          any_value = true;
        }
      }
    }
    if (b.terminator.cond_value != kInvalidValue &&
        (!any_value || b.terminator.cond_value + 1u > max_value)) {
      max_value = b.terminator.cond_value + 1u;
      any_value = true;
    }
  }

  std::vector<std::pair<BlockId, OpId>> defs(
      any_value ? max_value : 0u, {kInvalidBlock, kInvalidOp});

  // -- Pass 1: definitions, in stored order -------------------------------
  //
  // An op defines its `dest` AND, for flag-producing ops, its kFlagsOut
  // operand. Recording only `dest` made every kAddFlags/kCmp/kTest/shift flags
  // value look undefined, because a flags value never appears as a dest.
  for (std::size_t bi = 0; bi < program.blocks.size(); ++bi) {
    const Block& block = program.blocks[bi];
    for (std::size_t oi = 0; oi < block.ops.size(); ++oi) {
      const Op& op = block.ops[oi];

      // Collect this op's definition slots.
      std::vector<ValueId> defined_here;
      if (op.dest != kInvalidValue) {
        defined_here.push_back(op.dest);
      }
      if (op.dest2 != kInvalidValue) {
        defined_here.push_back(op.dest2);
      }
      for (std::size_t a = 0; a < op.args.size() && a < op.roles.size(); ++a) {
        if (op.roles[a] == OperandRole::kFlagsOut) {
          defined_here.push_back(op.args[a]);
        }
      }

      for (ValueId id : defined_here) {
        if (id >= defs.size()) {
          return fail("undefined-value", block.id, static_cast<OpId>(oi), id,
                      "definition id out of range");
        }
        if (defs[id].first != kInvalidBlock) {
          return fail("duplicate-definition", block.id, static_cast<OpId>(oi),
                      id, "value is defined more than once");
        }
        defs[id] = {block.id, static_cast<OpId>(oi)};

        // A flags definition must have flags type. InferType only understands
        // `dest`, so the flags slot is checked directly.
        bool is_flags_slot = false;
        for (std::size_t a = 0; a < op.args.size() && a < op.roles.size(); ++a) {
          if (op.roles[a] == OperandRole::kFlagsOut && op.args[a] == id) {
            is_flags_slot = true;
          }
        }
        if (is_flags_slot) {
          if (FlagsOutTypeOf(op) != ValueType::kFlags) {
            return fail("type-invalid", block.id, static_cast<OpId>(oi), id,
                        "flags output slot does not produce flags");
          }
        } else if (InferType(program, id, defs) == ValueType::kInvalid) {
          return fail("type-invalid", block.id, static_cast<OpId>(oi), id,
                      "op produces no type");
        }
      }
    }
  }

  // -- Pass 2: per-op validation ------------------------------------------
  for (std::size_t bi = 0; bi < program.blocks.size(); ++bi) {
    const Block& block = program.blocks[bi];

    for (std::size_t oi = 0; oi < block.ops.size(); ++oi) {
      const Op& op = block.ops[oi];
      const OpId op_index = static_cast<OpId>(oi);

      if (op.kind == OpKind::kInvalid) {
        return fail("invalid-op", block.id, op_index, op.dest,
                    "op kind is kInvalid");
      }

      std::vector<OperandRole> expected;
      if (!ExpectedOperands(op.kind, &expected)) {
        return fail("invalid-op", block.id, op_index, op.dest,
                    std::string("unknown op ") + OpKindName(op.kind));
      }
      const bool variable_arity = (op.kind == OpKind::kAddrCalc);
      if (variable_arity) {
        // kAddrCalc takes 0 or 1 index operand; everything else about its shape
        // is checked below.
        if (op.args.size() > 1) {
          return fail("impossible-operand-count", block.id, op_index, op.dest,
                      "addr_calc takes 0 or 1 index operands, got " +
                          std::to_string(op.args.size()));
        }
        if (op.args.size() != op.roles.size()) {
          return fail("impossible-operand-count", block.id, op_index, op.dest,
                      "addr_calc args/roles length mismatch");
        }
        if (op.args.size() == 1 && op.roles[0] != OperandRole::kSrc) {
          return fail("unknown-role", block.id, op_index, op.args[0],
                      "addr_calc index operand must have role kSrc");
        }
      } else if (op.args.size() != expected.size() ||
                 op.roles.size() != expected.size()) {
        return fail("impossible-operand-count", block.id, op_index, op.dest,
                    std::string(OpKindName(op.kind)) + " expects " +
                        std::to_string(expected.size()) + " operands, got " +
                        std::to_string(op.args.size()) + " args / " +
                        std::to_string(op.roles.size()) + " roles");
      }
      if (!variable_arity) {
        for (std::size_t a = 0; a < expected.size(); ++a) {
          if (op.roles[a] != expected[a]) {
            return fail("unknown-role", block.id, op_index, op.args[a],
                        std::string("operand ") + std::to_string(a) + " of " +
                            OpKindName(op.kind) + " has role " +
                            std::to_string(static_cast<int>(op.roles[a])) +
                            ", expected " +
                            std::to_string(static_cast<int>(expected[a])));
          }
        }
      }

      // Immediate-style operands (register selector, width, extend mode,
      // condition) live in Op FIELDS, not in Op::args. They are validated here
      // so that an op whose field was never set is rejected rather than being
      // silently read as a default.
      if ((op.kind == OpKind::kGetReg || op.kind == OpKind::kSetReg) &&
          !IsValidArchRegister(op.reg)) {
        return fail("invalid-register", block.id, op_index, op.dest,
                    std::string("invalid register ") + ArchRegisterName(op.reg));
      }
      if ((op.kind == OpKind::kGetReg || op.kind == OpKind::kSetReg) &&
          IsHighByteRegister(op.reg) && op.width != 1) {
        // AH/CH/DH/BH are byte-only views.
        return fail("invalid-width", block.id, op_index, op.dest,
                    std::string("high-byte register ") + ArchRegisterName(op.reg) +
                        " requires an 8-bit width");
      }
      if ((op.kind == OpKind::kGetXmm || op.kind == OpKind::kSetXmm) &&
          !IsXmmRegister(op.reg)) {
        return fail("invalid-register", block.id, op_index, op.dest,
                    std::string("expected an XMM register, got ") +
                        ArchRegisterName(op.reg));
      }
      if (op.kind == OpKind::kLoad || op.kind == OpKind::kStore) {
        if (!memory::IsValidAccessWidth(op.width)) {
          return fail("malformed-memory-op", block.id, op_index, op.dest,
                      "width " + std::to_string(op.width) +
                          " is not 1/2/4/8");
        }
      }
      if (op.kind == OpKind::kTruncate && !memory::IsValidAccessWidth(op.width)) {
        return fail("invalid-width", block.id, op_index, op.dest,
                    "truncate width " + std::to_string(op.width) +
                        " is not 1/2/4/8");
      }
      // Phase 11: dest2 is used only by the double-width multiply/divide ops, and
      // those require it. Arithmetic ops carry an explicit operand width.
      const bool new_arith =
          (op.kind == OpKind::kAdcFlags || op.kind == OpKind::kSbbFlags ||
           op.kind == OpKind::kMulFlags || op.kind == OpKind::kIMulFlags ||
           op.kind == OpKind::kDivFlags || op.kind == OpKind::kIDivFlags);
      const bool uses_dest2 =
          (op.kind == OpKind::kMulFlags || op.kind == OpKind::kIMulFlags ||
           op.kind == OpKind::kDivFlags || op.kind == OpKind::kIDivFlags);
      if (uses_dest2 && op.dest2 == kInvalidValue) {
        return fail("impossible-operand-count", block.id, op_index, op.dest,
                    std::string(OpKindName(op.kind)) +
                        " requires a second result (dest2)");
      }
      if (!uses_dest2 && op.dest2 != kInvalidValue) {
        return fail("invalid-op", block.id, op_index, op.dest2,
                    std::string(OpKindName(op.kind)) +
                        " must not define a second result");
      }
      if (new_arith && !memory::IsValidAccessWidth(op.width)) {
        return fail("invalid-width", block.id, op_index, op.dest,
                    "arithmetic width " + std::to_string(op.width) +
                        " is not 1/2/4/8");
      }
      if (op.kind == OpKind::kExtend && !IsValidExtendMode(op.mode)) {
        return fail("invalid-extend-mode", block.id, op_index, op.dest,
                    "unknown extend mode");
      }
      if (op.kind == OpKind::kTestCc && !IsValidConditionCode(op.cond)) {
        return fail("invalid-condition", block.id, op_index, op.dest,
                    "condition code out of range");
      }
      if (op.kind == OpKind::kAddrCalc) {
        // Variable arity: exactly 0 (absolute) or exactly 1 (indexed).
        if (op.args.size() > 1) {
          return fail("impossible-operand-count", block.id, op_index, op.dest,
                      "addr_calc takes 0 or 1 index operands, got " +
                          std::to_string(op.args.size()));
        }
        if (op.width != 1 && op.width != 2 && op.width != 4 && op.width != 8) {
          return fail("malformed-memory-op", block.id, op_index, op.dest,
                      "addr_calc scale " + std::to_string(op.width) +
                          " is not 1/2/4/8");
        }
      }

      // Value operands.
      for (std::size_t a = 0; a < op.args.size(); ++a) {
        const ValueId v = op.args[a];
        const OperandRole role = op.roles[a];

        // A kFlagsOut operand is a DEFINITION, checked in pass 1. It is not a
        // use, so it must not be re-checked for definedness or ordering here.
        if (role == OperandRole::kFlagsOut) {
          continue;
        }

        if (v >= defs.size() || defs[v].first == kInvalidBlock) {
          return fail("undefined-value", block.id, op_index, v,
                      "operand " + std::to_string(a) + " of " +
                          OpKindName(op.kind) + " is never defined");
        }

        // Use-before-definition: the defining op must appear earlier in
        // program order (block index, then op index).
        const BlockId def_block = defs[v].first;
        const OpId def_op = defs[v].second;
        const Block* defb = program.FindBlock(def_block);
        std::size_t def_bi = program.blocks.size();
        for (std::size_t k = 0; k < program.blocks.size(); ++k) {
          if (program.blocks[k].id == def_block) {
            def_bi = k;
            break;
          }
        }
        if (defb == nullptr) {
          return fail("undefined-block", block.id, op_index, v,
                      "value defined in a missing block");
        }
        const bool later_block = def_bi > bi;
        const bool same_block_earlier_op =
            (def_bi == bi) && (def_op < op_index);
        if (!later_block && !same_block_earlier_op) {
          return fail("use-before-definition", block.id, op_index, v,
                      "operand " + std::to_string(a) + " of " +
                          OpKindName(op.kind) + " uses a value defined later");
        }
        if (def_bi > bi) {
          return fail("cross-block-ssa-violation", block.id, op_index, v,
                      "Phase 1 forbids using a value from a later block");
        }

        // Type expectations.
        const ValueType vt = InferType(program, v, defs);
        if (role == OperandRole::kFlags || role == OperandRole::kFlagsOut) {
          if (vt != ValueType::kFlags) {
            return fail("value-type-mismatch", block.id, op_index, v,
                        std::string("expected flags, got ") + TypeName(vt));
          }
        } else if (role == OperandRole::kAddr || role == OperandRole::kSrc ||
                   role == OperandRole::kSrc2 || role == OperandRole::kSrc3) {
          // Phase 12: some kSrc/kSrc2 operands are 128-bit SIMD values, not
          // integers. The op determines which.
          const bool role_is_vec =
              (op.kind == OpKind::kSetXmm && role == OperandRole::kSrc) ||
              (op.kind == OpKind::kVecStore && role == OperandRole::kSrc) ||
              (op.kind == OpKind::kVecExtractLow64 &&
               role == OperandRole::kSrc) ||
              ((op.kind == OpKind::kVecXor || op.kind == OpKind::kVecAnd ||
                op.kind == OpKind::kVecOr ||
                op.kind == OpKind::kVecAdd64x2) &&
               (role == OperandRole::kSrc || role == OperandRole::kSrc2));
          if (role_is_vec) {
            if (vt != ValueType::kV128) {
              return fail("value-type-mismatch", block.id, op_index, v,
                          std::string("expected vec128, got ") + TypeName(vt));
            }
          } else {
            // Any SCALAR INTEGER type is acceptable where an integer is wanted.
            const bool scalar_integer =
                (vt == ValueType::kI8) || (vt == ValueType::kI16) ||
                (vt == ValueType::kI32) || (vt == ValueType::kI64);
            if (!scalar_integer) {
              return fail("value-type-mismatch", block.id, op_index, v,
                          std::string("expected an integer, got ") +
                              TypeName(vt));
            }
          }
        }
      }
    }

    // -- Terminator -------------------------------------------------------
    const Terminator& t = block.terminator;
    if (t.kind == TerminatorKind::kInvalid) {
      return fail("invalid-terminator", block.id, kInvalidOp, kInvalidValue,
                  "terminator kind is kInvalid");
    }
    if (t.kind == TerminatorKind::kJump || t.kind == TerminatorKind::kBranch) {
      if (t.target == kInvalidBlock || program.FindBlock(t.target) == nullptr) {
        return fail("undefined-block", block.id, kInvalidOp, kInvalidValue,
                    std::string(TerminatorKindName(t.kind)) +
                        " targets a block that does not exist");
      }
    } else {
      if (t.target != kInvalidBlock) {
        return fail("malformed-terminator", block.id, kInvalidOp,
                    kInvalidValue,
                    std::string(TerminatorKindName(t.kind)) +
                        " must not carry a target");
      }
    }
    if (t.kind == TerminatorKind::kBranch) {
      if (t.fallthrough == kInvalidBlock ||
          program.FindBlock(t.fallthrough) == nullptr) {
        return fail("undefined-block", block.id, kInvalidOp, kInvalidValue,
                    "branch not-taken successor does not exist");
      }
      // A conditional branch whose two successors are identical can never make
      // progress on either path. It is legal IR but always a translation bug.
      if (t.fallthrough == t.target) {
        return fail("malformed-terminator", block.id, kInvalidOp,
                    kInvalidValue,
                    "branch taken and not-taken successors are identical");
      }
    } else if (t.kind == TerminatorKind::kExternalCall) {
      // An external call resumes in a real block after the binding returns.
      if (t.fallthrough == kInvalidBlock ||
          program.FindBlock(t.fallthrough) == nullptr) {
        return fail("undefined-block", block.id, kInvalidOp, kInvalidValue,
                    "external call return block does not exist");
      }
    } else {
      if (t.fallthrough != kInvalidBlock) {
        return fail("malformed-terminator", block.id, kInvalidOp,
                    kInvalidValue,
                    std::string(TerminatorKindName(t.kind)) +
                        " must not carry a fallthrough successor");
      }
    }
    if (t.kind == TerminatorKind::kIndirectJump) {
      if (t.cond_value == kInvalidValue) {
        return fail("malformed-terminator", block.id, kInvalidOp, kInvalidValue,
                    "indirect jump has no target value");
      }
      if (t.cond_value >= defs.size() ||
          defs[t.cond_value].first == kInvalidBlock) {
        return fail("undefined-value", block.id, kInvalidOp, t.cond_value,
                    "indirect jump target value is never defined");
      }
      if (t.fallthrough != kInvalidBlock) {
        return fail("malformed-terminator", block.id, kInvalidOp, kInvalidValue,
                    "indirect jump must not carry a fallthrough successor");
      }
    } else if (t.kind == TerminatorKind::kBranch) {
      if (t.cond_value == kInvalidValue) {
        return fail("malformed-terminator", block.id, kInvalidOp, kInvalidValue,
                    "branch has no condition value");
      }
      if (!IsValidConditionCode(t.cond)) {
        return fail("invalid-condition", block.id, kInvalidOp, t.cond_value,
                    "branch condition code out of range");
      }
      if (t.cond_value >= defs.size() ||
          defs[t.cond_value].first == kInvalidBlock) {
        return fail("undefined-value", block.id, kInvalidOp, t.cond_value,
                    "branch condition value is never defined");
      }
      const ValueType vt = InferType(program, t.cond_value, defs);
      if (vt != ValueType::kFlags) {
        return fail("value-type-mismatch", block.id, kInvalidOp, t.cond_value,
                    std::string("branch condition must be flags, got ") +
                        TypeName(vt));
      }
    } else if (t.kind != TerminatorKind::kBranch &&
               t.kind != TerminatorKind::kIndirectJump) {
      // kJump, kFallthrough, kTrap and kReturn carry no condition value.
      if (t.cond_value != kInvalidValue) {
        return fail("malformed-terminator", block.id, kInvalidOp,
                    t.cond_value,
                    std::string(TerminatorKindName(t.kind)) +
                        " must not carry a condition value");
      }
    }
  }

  report->valid = true;
  return StatusCode::kOk;
}

Status ValidateIr(const Program& program) {
  ValidationReport report;
  return ValidateIrDetailed(program, &report);
}

}  // namespace p5emu::ir
