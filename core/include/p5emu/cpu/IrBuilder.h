// SPDX-License-Identifier: Apache-2.0
//
// P5EMU is an independent, open-source project. See LICENSE and
// PROVENANCE.md before adding code here.

#ifndef P5EMU_CPU_IR_BUILDER_H
#define P5EMU_CPU_IR_BUILDER_H

#include <cstdint>
#include <vector>

#include "p5emu/cpu/Ir.h"

namespace p5emu::ir {

// ---------------------------------------------------------------------------
// Builder
// ---------------------------------------------------------------------------
// Construction API for Program/Block/Op. Enforces the two structural invariants
// that make the IR relocatable and SSA:
//
//   * every ValueId is allocated densely and monotonically
//   * a value is defined in exactly one op, in one block, before any use
//
// The builder tracks the current block, so a caller cannot accidentally append
// an op to a block it did not mean to.
//
// The builder does NOT validate. Call ValidateIr() on the finished Program. The
// builder's job is construction, not checking -- except that it refuses
// obviously impossible requests (e.g. creating a value with no type) rather than
// emitting nonsense.
//
// Values are NOT auto-materialised. A caller that needs a constant materialises
// it explicitly via MaterializeConstant, so that "what constants exist" is
// always a deliberate, inspectable decision.
// ---------------------------------------------------------------------------
class Builder {
 public:
  explicit Builder(std::string program_name = "p5emu-program");

  // -- Blocks --------------------------------------------------------------
  // Creates a block and makes it current. Returns kInvalidBlock if the program
  // has exceeded BlockId capacity.
  BlockId CreateBlock(memory::GuestAddr entry_address = 0);
  void SetCurrentBlock(BlockId block);
  [[nodiscard]] BlockId CurrentBlock() const noexcept { return current_; }
  void SetEntryBlock(BlockId block);

  // -- Values --------------------------------------------------------------
  // Allocates a ValueId and records its type. Returns kInvalidValue if the
  // builder has no current block.
  ValueId DefineValue(ValueType type);

  // -- Ops -----------------------------------------------------------------
  // Appends an op to the current block. `dest` may be kInvalidValue for ops
  // with no value result (kStore, kTest, kCmp, kStoreFlags, kNop).
  //
  // Returns the op's index within the current block, or kInvalidOp on failure
  // (no current block).
  OpId Append(Op op);

  // -- Convenience emitters ------------------------------------------------
  // These exist so that the IR builder for decoded instructions reads like the
  // semantics it implements, rather than like struct initialisation.

  ValueId EmitGetReg(ArchRegister reg, std::uint8_t width_bytes = 8);
  void EmitSetReg(ArchRegister reg, ValueId value, std::uint8_t width_bytes = 8);

  // Absolute base + index*scale + displacement. `index` may be kInvalidValue,
  // in which case the address is `base + displacement`.
  ValueId EmitAddr(memory::GuestAddr base, ValueId index, std::uint32_t scale,
                   std::int64_t displacement);

  // base REGISTER (a runtime value) + index*scale + displacement. Either
  // `base` or `index` may be kInvalidValue, but not both.
  //
  // Deliberately separate from EmitAddr: a base register is a value while an
  // absolute base is a constant, and conflating them is how an addressing mode
  // gets silently dropped from a lowering.
  ValueId EmitAddress(ValueId base, ValueId index, std::uint32_t scale,
                      std::int64_t displacement);

  ValueId EmitLoad(ValueId addr, std::uint32_t width_bytes);
  void EmitStore(ValueId addr, ValueId value, std::uint32_t width_bytes);

  ValueId EmitBinary(OpKind kind, ValueId lhs, ValueId rhs);
  ValueId EmitBinaryFlags(OpKind kind, ValueId lhs, ValueId rhs,
                          std::uint8_t width_bytes = 8);
  // CMP and TEST return the flags value they wrote, and have already committed
  // it to EFLAGS via kStoreFlags. The returned id is for inspection/testing.
  ValueId EmitCompare(ValueId lhs, ValueId rhs, std::uint8_t width_bytes = 8);
  ValueId EmitTest(ValueId lhs, ValueId rhs, std::uint8_t width_bytes = 8);

  ValueId EmitShift(OpKind kind, ValueId value, ValueId count,
                    std::uint8_t width_bytes = 8);
  ValueId EmitExtend(ValueId value, ExtendMode mode);

  // -- Phase 11 arithmetic -------------------------------------------------
  // ADC/SBB consume an explicit carry-in value (0 or 1) and write flags to
  // EFLAGS. The result is width-masked.
  ValueId EmitAdcFlags(ValueId a, ValueId b, ValueId carry_in,
                       std::uint8_t width_bytes = 8);
  ValueId EmitSbbFlags(ValueId a, ValueId b, ValueId borrow_in,
                       std::uint8_t width_bytes = 8);
  // MUL/IMUL return the LOW half; the HIGH half is written to *high_out (which
  // may be null). CF/OF are written to EFLAGS.
  ValueId EmitMulFlags(ValueId a, ValueId b, ValueId* high_out,
                       std::uint8_t width_bytes = 8);
  ValueId EmitIMulFlags(ValueId a, ValueId b, ValueId* high_out,
                        std::uint8_t width_bytes = 8);
  // DIV/IDIV return the quotient; the remainder is written to *rem_out (which
  // may be null). They write no flags. They fault on a divide error.
  ValueId EmitDiv(ValueId dividend_hi, ValueId dividend_lo, ValueId divisor,
                  ValueId* rem_out, std::uint8_t width_bytes = 8);
  ValueId EmitIDiv(ValueId dividend_hi, ValueId dividend_lo, ValueId divisor,
                   ValueId* rem_out, std::uint8_t width_bytes = 8);

  ValueId EmitMaterializeConstant(std::uint64_t value, ValueType type);
  ValueId EmitLoadFlags();
  void EmitStoreFlags(ValueId flags);
  ValueId EmitTestCc(ConditionCode cc, ValueId flags);

  // -- Terminators ---------------------------------------------------------
  void EmitFallthrough();
  void EmitJump(BlockId target);
  // `taken` executes when the condition holds; `not_taken` otherwise. Both are
  // explicit: neither is inferred from block ordering.
  void EmitBranch(ConditionCode cc, ValueId flags, BlockId taken,
                  BlockId not_taken);
  // A call to an external host binding (HLE import) at `target`; execution
  // resumes in `return_block` after the binding returns.
  void EmitExternalCall(memory::GuestAddr target, BlockId return_block);
  // Jump to the guest address held in `target` (an indirect control transfer,
  // e.g. RET). Resolved to a block by address at run time.
  void EmitIndirectJump(ValueId target);
  void EmitReturn();
  void EmitTrap();

  // -- Access --------------------------------------------------------------
  [[nodiscard]] Program& MutableProgram() noexcept { return program_; }
  [[nodiscard]] const Program& program() const noexcept { return program_; }
  [[nodiscard]] ValueType TypeOf(ValueId value) const noexcept;

  // Value ids in definition order. Exposed for tests and for the dump.
  [[nodiscard]] const std::vector<ValueType>& value_types() const noexcept {
    return value_types_;
  }

 private:
  Program program_;
  std::vector<ValueType> value_types_;
  BlockId current_ = kInvalidBlock;
};

}  // namespace p5emu::ir

#endif  // P5EMU_CPU_IR_BUILDER_H
