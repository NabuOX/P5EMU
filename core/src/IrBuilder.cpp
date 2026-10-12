// SPDX-License-Identifier: Apache-2.0
//
// P5EMU is an independent, open-source project. See LICENSE and
// PROVENANCE.md before adding code here.

#include "p5emu/cpu/IrBuilder.h"

#include <cstdio>

namespace p5emu::ir {
namespace {

bool IsValidExtendMode(ExtendMode m) {
  return m == ExtendMode::kNone || m == ExtendMode::kZeroExtend32 ||
         m == ExtendMode::kSignExtend8 || m == ExtendMode::kSignExtend16 ||
         m == ExtendMode::kSignExtend32;
}

Op MakeOp(OpKind kind) {
  Op op;
  op.kind = kind;
  return op;
}

}  // namespace

Builder::Builder(std::string program_name) {
  program_.name = std::move(program_name);
}

// ---------------------------------------------------------------------------
// Blocks
// ---------------------------------------------------------------------------
BlockId Builder::CreateBlock(memory::GuestAddr entry_address) {
  if (program_.blocks.size() >= kInvalidBlock) {
    return kInvalidBlock;
  }
  Block block;
  block.id = static_cast<BlockId>(program_.blocks.size());
  block.entry_address = entry_address;
  program_.blocks.push_back(std::move(block));
  current_ = program_.blocks.back().id;
  return current_;
}

void Builder::SetCurrentBlock(BlockId block) {
  if (program_.FindBlock(block) != nullptr) {
    current_ = block;
  }
}

void Builder::SetEntryBlock(BlockId block) { program_.entry = block; }

// ---------------------------------------------------------------------------
// Values
// ---------------------------------------------------------------------------
ValueId Builder::DefineValue(ValueType type) {
  if (current_ == kInvalidBlock) {
    return kInvalidValue;
  }
  if (type == ValueType::kInvalid) {
    // Refuse to mint an untyped value: the mistake becomes impossible rather
    // than merely detectable.
    return kInvalidValue;
  }
  if (value_types_.size() >= kInvalidValue) {
    return kInvalidValue;
  }
  value_types_.push_back(type);
  return static_cast<ValueId>(value_types_.size() - 1u);
}

ValueType Builder::TypeOf(ValueId value) const noexcept {
  if (value >= value_types_.size()) {
    return ValueType::kInvalid;
  }
  return value_types_[value];
}

OpId Builder::Append(Op op) {
  if (current_ == kInvalidBlock) {
    return kInvalidOp;
  }
  Block* block = program_.FindBlock(current_);
  if (block == nullptr) {
    return kInvalidOp;
  }
  block->ops.push_back(std::move(op));
  return static_cast<OpId>(block->ops.size() - 1u);
}

// ---------------------------------------------------------------------------
// CPU state
// ---------------------------------------------------------------------------
ValueId Builder::EmitGetReg(ArchRegister reg, std::uint8_t width_bytes) {
  const ValueId dest = DefineValue(ValueType::kI64);
  if (dest == kInvalidValue) {
    return dest;
  }
  Op op = MakeOp(OpKind::kGetReg);
  op.dest = dest;
  op.reg = reg;
  op.width = width_bytes;
  Append(std::move(op));
  return dest;
}

void Builder::EmitSetReg(ArchRegister reg, ValueId value,
                         std::uint8_t width_bytes) {
  Op op = MakeOp(OpKind::kSetReg);
  op.reg = reg;
  op.args = {value};
  op.roles = {OperandRole::kSrc};
  op.width = width_bytes;
  Append(std::move(op));
}

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
ValueId Builder::EmitMaterializeConstant(std::uint64_t value, ValueType type) {
  // Narrower constants are zero-extended on the way in, so the stored i64 always
  // holds the architectural value. Sign-extension is an explicit kExtend.
  if (TypeWidthBits(type) != 0 && TypeWidthBits(type) < 64) {
    const std::uint32_t bits = TypeWidthBits(type);
    value &= (std::uint64_t{1} << bits) - 1u;
  }

  const ValueId dest = DefineValue(ValueType::kI64);
  if (dest == kInvalidValue) {
    return dest;
  }
  Op op = MakeOp(OpKind::kConst);
  op.dest = dest;
  op.imm = value;
  Append(std::move(op));
  return dest;
}

// ---------------------------------------------------------------------------
// Memory
// ---------------------------------------------------------------------------
ValueId Builder::EmitAddr(memory::GuestAddr base, ValueId index,
                          std::uint32_t scale, std::int64_t displacement) {
  if (scale != 1 && scale != 2 && scale != 4 && scale != 8) {
    return kInvalidValue;
  }
  if (index == kInvalidValue && scale != 1) {
    return kInvalidValue;
  }

  // Absolute base (no base REGISTER) with an optional index.
  const ValueId dest = DefineValue(ValueType::kI64);
  if (dest == kInvalidValue) {
    return dest;
  }
  Op op = MakeOp(OpKind::kAddrCalc);
  op.dest = dest;
  op.imm = base;
  op.width = scale;
  op.imm_high = static_cast<std::uint64_t>(displacement);
  if (index != kInvalidValue) {
    op.args = {index};
    op.roles = {OperandRole::kSrc};
  }
  Append(std::move(op));
  return dest;
}

// Computes a guest address from an optional BASE REGISTER value, an optional
// index with a scale, and a displacement.
//
// The base is a VALUE, not an immediate. An earlier version took the base as a
// raw address and never read the base REGISTER, so `mov [rdx], rax` computed
// address 0 and wrote to the wrong place. Address computation must use the
// whole addressing mode.
//
// The absolute base + index*scale + displacement part maps onto the dedicated
// kAddrCalc op; the base register, being a runtime value, is then added. The two
// are kept separate so the IR can still distinguish a register-relative
// address from an absolute one.
ValueId Builder::EmitAddress(ValueId base, ValueId index, std::uint32_t scale,
                             std::int64_t displacement) {
  if (scale != 1 && scale != 2 && scale != 4 && scale != 8) {
    return kInvalidValue;
  }
  if (index == kInvalidValue && scale != 1) {
    return kInvalidValue;
  }

  // Purely absolute: one op, no register reads.
  if (base == kInvalidValue && index == kInvalidValue) {
    return EmitAddr(static_cast<memory::GuestAddr>(displacement),
                    kInvalidValue, 1, 0);
  }

  // index*scale, when present.
  ValueId scaled_index = kInvalidValue;
  if (index != kInvalidValue) {
    scaled_index = EmitAddr(0, index, scale, 0);
    if (scaled_index == kInvalidValue) {
      return scaled_index;
    }
  }

  // base + displacement, when there is a base.
  ValueId base_part = kInvalidValue;
  if (base != kInvalidValue) {
    base_part = base;
    if (displacement != 0) {
      const ValueId disp = EmitMaterializeConstant(
          static_cast<std::uint64_t>(displacement), ValueType::kI64);
      base_part = EmitBinary(OpKind::kAdd, base_part, disp);
    }
  }

  if (base_part != kInvalidValue && scaled_index != kInvalidValue) {
    return EmitBinary(OpKind::kAdd, base_part, scaled_index);
  }
  if (base_part != kInvalidValue) {
    return base_part;
  }
  return scaled_index;
}

ValueId Builder::EmitLoad(ValueId addr, std::uint32_t width_bytes) {
  const ValueId dest = DefineValue(ValueType::kI64);
  if (dest == kInvalidValue) {
    return dest;
  }
  Op op = MakeOp(OpKind::kLoad);
  op.dest = dest;
  op.width = width_bytes;
  op.args = {addr};
  op.roles = {OperandRole::kAddr};
  Append(std::move(op));
  return dest;
}

void Builder::EmitStore(ValueId addr, ValueId value, std::uint32_t width_bytes) {
  Op op = MakeOp(OpKind::kStore);
  op.width = width_bytes;
  op.args = {addr, value};
  op.roles = {OperandRole::kAddr, OperandRole::kSrc};
  Append(std::move(op));
}

// ---------------------------------------------------------------------------
// Integer ALU
// ---------------------------------------------------------------------------
ValueId Builder::EmitBinary(OpKind kind, ValueId lhs, ValueId rhs) {
  const ValueId dest = DefineValue(ValueType::kI64);
  if (dest == kInvalidValue) {
    return dest;
  }
  Op op = MakeOp(kind);
  op.dest = dest;
  op.args = {lhs, rhs};
  op.roles = {OperandRole::kSrc, OperandRole::kSrc2};
  Append(std::move(op));
  return dest;
}

ValueId Builder::EmitBinaryFlags(OpKind kind, ValueId lhs, ValueId rhs,
                                std::uint8_t width_bytes) {
  if (kind != OpKind::kAddFlags && kind != OpKind::kSubFlags &&
      kind != OpKind::kAndFlags && kind != OpKind::kOrFlags &&
      kind != OpKind::kXorFlags) {
    return kInvalidValue;
  }
  const ValueId dest = DefineValue(ValueType::kI64);
  if (dest == kInvalidValue) {
    return dest;
  }
  const ValueId flags = DefineValue(ValueType::kFlags);
  if (flags == kInvalidValue) {
    return dest;
  }
  Op op = MakeOp(kind);
  op.dest = dest;
  op.args = {lhs, rhs, flags};
  op.roles = {OperandRole::kSrc, OperandRole::kSrc2, OperandRole::kFlagsOut};
  op.width = width_bytes;
  Append(std::move(op));
  // Commit the flags to EFLAGS.
  //
  // This step is REQUIRED, not an optimisation: x86 ADD/SUB/AND/OR/XOR all
  // write EFLAGS architecturally. Omitting it produced a builder that
  // computed correct flag VALUES and then threw them away, so every
  // flag-observing instruction (including all sixteen condition codes) saw
  // stale flags.
  EmitStoreFlags(flags);
  return dest;
}

// CMP and TEST write EFLAGS on x86. Rather than letting the op mutate machine
// state directly, they produce a flags VALUE and the builder immediately commits
// it with kStoreFlags. That keeps every flag write visible in the IR dump and
// prevents a backend from treating the write as optional.
ValueId Builder::EmitCompare(ValueId lhs, ValueId rhs,
                            std::uint8_t width_bytes) {
  const ValueId flags = DefineValue(ValueType::kFlags);
  if (flags == kInvalidValue) {
    return flags;
  }
  Op op = MakeOp(OpKind::kCmp);
  op.args = {lhs, rhs, flags};
  op.roles = {OperandRole::kSrc, OperandRole::kSrc2, OperandRole::kFlagsOut};
  op.width = width_bytes;
  Append(std::move(op));
  EmitStoreFlags(flags);
  return flags;
}

ValueId Builder::EmitTest(ValueId lhs, ValueId rhs,
                         std::uint8_t width_bytes) {
  const ValueId flags = DefineValue(ValueType::kFlags);
  if (flags == kInvalidValue) {
    return flags;
  }
  Op op = MakeOp(OpKind::kTest);
  op.args = {lhs, rhs, flags};
  op.roles = {OperandRole::kSrc, OperandRole::kSrc2, OperandRole::kFlagsOut};
  op.width = width_bytes;
  Append(std::move(op));
  EmitStoreFlags(flags);
  return flags;
}

ValueId Builder::EmitShift(OpKind kind, ValueId value, ValueId count,
                           std::uint8_t width_bytes) {
  if (kind != OpKind::kShl && kind != OpKind::kShr && kind != OpKind::kSar) {
    return kInvalidValue;
  }
  const ValueId dest = DefineValue(ValueType::kI64);
  if (dest == kInvalidValue) {
    return dest;
  }
  const ValueId flags = DefineValue(ValueType::kFlags);
  if (flags == kInvalidValue) {
    return dest;
  }
  Op op = MakeOp(kind);
  op.dest = dest;
  op.args = {value, count, flags};
  op.roles = {OperandRole::kSrc, OperandRole::kSrc2, OperandRole::kFlagsOut};
  op.width = width_bytes;
  Append(std::move(op));
  // Shifts write EFLAGS too; commit the flags value. See EmitBinaryFlags.
  EmitStoreFlags(flags);
  return dest;
}

ValueId Builder::EmitExtend(ValueId value, ExtendMode mode) {
  if (!IsValidExtendMode(mode)) {
    return kInvalidValue;
  }
  const ValueId dest = DefineValue(ValueType::kI64);
  if (dest == kInvalidValue) {
    return dest;
  }
  Op op = MakeOp(OpKind::kExtend);
  op.dest = dest;
  op.mode = mode;
  op.args = {value};
  op.roles = {OperandRole::kSrc};
  Append(std::move(op));
  return dest;
}

// --- Phase 11 arithmetic ---------------------------------------------------
namespace {
ValueId EmitCarryFlagsImpl(Builder* b, OpKind kind, ValueId a, ValueId bval,
                           ValueId carry, std::uint8_t width_bytes) {
  const ValueId dest = b->DefineValue(ValueType::kI64);
  if (dest == kInvalidValue) return dest;
  const ValueId flags = b->DefineValue(ValueType::kFlags);
  if (flags == kInvalidValue) return dest;
  Op op;
  op.kind = kind;
  op.dest = dest;
  op.args = {a, bval, carry, flags};
  op.roles = {OperandRole::kSrc, OperandRole::kSrc2, OperandRole::kSrc3,
              OperandRole::kFlagsOut};
  op.width = width_bytes;
  b->Append(std::move(op));
  b->EmitStoreFlags(flags);
  return dest;
}

ValueId EmitWideMulImpl(Builder* b, OpKind kind, ValueId a, ValueId bval,
                        ValueId* high_out, std::uint8_t width_bytes) {
  const ValueId lo = b->DefineValue(ValueType::kI64);
  if (lo == kInvalidValue) return lo;
  const ValueId hi = b->DefineValue(ValueType::kI64);
  if (hi == kInvalidValue) return lo;
  const ValueId flags = b->DefineValue(ValueType::kFlags);
  if (flags == kInvalidValue) return lo;
  Op op;
  op.kind = kind;
  op.dest = lo;
  op.dest2 = hi;
  op.args = {a, bval, flags};
  op.roles = {OperandRole::kSrc, OperandRole::kSrc2, OperandRole::kFlagsOut};
  op.width = width_bytes;
  b->Append(std::move(op));
  b->EmitStoreFlags(flags);
  if (high_out != nullptr) *high_out = hi;
  return lo;
}

ValueId EmitDivImpl(Builder* b, OpKind kind, ValueId hi, ValueId lo,
                    ValueId divisor, ValueId* rem_out,
                    std::uint8_t width_bytes) {
  const ValueId quot = b->DefineValue(ValueType::kI64);
  if (quot == kInvalidValue) return quot;
  const ValueId rem = b->DefineValue(ValueType::kI64);
  if (rem == kInvalidValue) return quot;
  Op op;
  op.kind = kind;
  op.dest = quot;
  op.dest2 = rem;
  op.args = {hi, lo, divisor};
  op.roles = {OperandRole::kSrc, OperandRole::kSrc2, OperandRole::kSrc3};
  op.width = width_bytes;
  b->Append(std::move(op));
  if (rem_out != nullptr) *rem_out = rem;
  return quot;
}
}  // namespace

ValueId Builder::EmitAdcFlags(ValueId a, ValueId b, ValueId carry_in,
                              std::uint8_t width_bytes) {
  return EmitCarryFlagsImpl(this, OpKind::kAdcFlags, a, b, carry_in, width_bytes);
}
ValueId Builder::EmitSbbFlags(ValueId a, ValueId b, ValueId borrow_in,
                              std::uint8_t width_bytes) {
  return EmitCarryFlagsImpl(this, OpKind::kSbbFlags, a, b, borrow_in, width_bytes);
}
ValueId Builder::EmitMulFlags(ValueId a, ValueId b, ValueId* high_out,
                              std::uint8_t width_bytes) {
  return EmitWideMulImpl(this, OpKind::kMulFlags, a, b, high_out, width_bytes);
}
ValueId Builder::EmitIMulFlags(ValueId a, ValueId b, ValueId* high_out,
                               std::uint8_t width_bytes) {
  return EmitWideMulImpl(this, OpKind::kIMulFlags, a, b, high_out, width_bytes);
}
ValueId Builder::EmitDiv(ValueId dividend_hi, ValueId dividend_lo, ValueId divisor,
                         ValueId* rem_out, std::uint8_t width_bytes) {
  return EmitDivImpl(this, OpKind::kDivFlags, dividend_hi, dividend_lo, divisor,
                     rem_out, width_bytes);
}
ValueId Builder::EmitIDiv(ValueId dividend_hi, ValueId dividend_lo,
                          ValueId divisor, ValueId* rem_out,
                          std::uint8_t width_bytes) {
  return EmitDivImpl(this, OpKind::kIDivFlags, dividend_hi, dividend_lo, divisor,
                     rem_out, width_bytes);
}

// ---------------------------------------------------------------------------
// Flags
// ---------------------------------------------------------------------------
ValueId Builder::EmitLoadFlags() {
  const ValueId dest = DefineValue(ValueType::kFlags);
  if (dest == kInvalidValue) {
    return dest;
  }
  Op op = MakeOp(OpKind::kLoadFlags);
  op.dest = dest;
  Append(std::move(op));
  return dest;
}

void Builder::EmitStoreFlags(ValueId flags) {
  Op op = MakeOp(OpKind::kStoreFlags);
  op.args = {flags};
  op.roles = {OperandRole::kFlags};
  Append(std::move(op));
}

ValueId Builder::EmitTestCc(ConditionCode cc, ValueId flags) {
  const ValueId dest = DefineValue(ValueType::kI8);
  if (dest == kInvalidValue) {
    return dest;
  }
  Op op = MakeOp(OpKind::kTestCc);
  op.dest = dest;
  op.cond = cc;
  op.args = {flags};
  op.roles = {OperandRole::kFlags};
  Append(std::move(op));
  return dest;
}

// ---------------------------------------------------------------------------
// Terminators
// ---------------------------------------------------------------------------
void Builder::EmitFallthrough() {
  Block* b = (current_ == kInvalidBlock) ? nullptr : program_.FindBlock(current_);
  if (b == nullptr) {
    return;
  }
  b->terminator = Terminator{};
  b->terminator.kind = TerminatorKind::kFallthrough;
}

void Builder::EmitJump(BlockId target) {
  Block* b = (current_ == kInvalidBlock) ? nullptr : program_.FindBlock(current_);
  if (b == nullptr) {
    return;
  }
  b->terminator = Terminator{};
  b->terminator.kind = TerminatorKind::kJump;
  b->terminator.target = target;
}

void Builder::EmitBranch(ConditionCode cc, ValueId flags, BlockId taken,
                         BlockId not_taken) {
  Block* b = (current_ == kInvalidBlock) ? nullptr : program_.FindBlock(current_);
  if (b == nullptr) {
    return;
  }
  b->terminator = Terminator{};
  b->terminator.kind = TerminatorKind::kBranch;
  b->terminator.cond = cc;
  b->terminator.cond_value = flags;
  b->terminator.target = taken;
  b->terminator.fallthrough = not_taken;
}

void Builder::EmitReturn() {
  Block* b = (current_ == kInvalidBlock) ? nullptr : program_.FindBlock(current_);
  if (b == nullptr) {
    return;
  }
  b->terminator = Terminator{};
  b->terminator.kind = TerminatorKind::kReturn;
}

void Builder::EmitExternalCall(memory::GuestAddr target, BlockId return_block) {
  Block* b = (current_ == kInvalidBlock) ? nullptr : program_.FindBlock(current_);
  if (b == nullptr) {
    return;
  }
  b->terminator = Terminator{};
  b->terminator.kind = TerminatorKind::kExternalCall;
  b->terminator.external_target = target;
  b->terminator.fallthrough = return_block;
}

void Builder::EmitIndirectJump(ValueId target) {
  Block* b = (current_ == kInvalidBlock) ? nullptr : program_.FindBlock(current_);
  if (b == nullptr) {
    return;
  }
  b->terminator = Terminator{};
  b->terminator.kind = TerminatorKind::kIndirectJump;
  b->terminator.cond_value = target;
}

void Builder::EmitTrap() {
  Block* b = (current_ == kInvalidBlock) ? nullptr : program_.FindBlock(current_);
  if (b == nullptr) {
    return;
  }
  b->terminator = Terminator{};
  b->terminator.kind = TerminatorKind::kTrap;
}

}  // namespace p5emu::ir
