// SPDX-License-Identifier: LicenseRef-P5EMU-Proprietary
//
// P5EMU source is independently authored and proprietary. See LICENSE.md and
// PROVENANCE.md before adding code here.
//
// ============================================================================
// P5EMU SSA IR
// ============================================================================
//
// DESIGN PROPERTIES (from Phase 0; each is load-bearing, not aspirational)
//
//   1. Typed values with explicit bit widths. 1/2/4/8 for scalars; a vector
//      value additionally carries an element count. x86 is a variable-width ISA,
//      so an IR with implicit width grows special cases forever.
//
//   2. SSA. Every value is defined once. Results are compact integer IDs, never
//      pointers, so a function is relocatable and serialisable.
//
//   3. Flags are first-class values, not hidden instruction side effects.
//      `Add` produces a flags value; `SetCc` consumes it. An IR that hides flags
//      behind instructions cannot optimise or verify them.
//
//   4. CPU-state operations are distinct from memory operations.
//      `GetReg`/`SetReg` address architectural state; `Load`/`Store` address
//      guest memory. They have different failure modes (the latter can fault)
//      and different aliasing rules, so conflating them is a correctness bug
//      waiting to happen.
//
//   5. Control flow is explicit. A program is a vector of basic blocks with
//      explicit terminators and explicit successors.
//
//   6. Relocatable. No raw pointers in the IR. Node payloads live in a
//      contiguous vector; cross-references are IDs.
//
//   7. Deterministic textual dump. Same IR in, byte-identical text out. This is
//      what makes golden-file testing and cross-backend comparison possible.
//
//   8. No host types. No host register, no host pointer, no host enum. The IR
//      must be printable on any host and interpretable on any host.
//
// Deliberately NOT in this IR:
//
//   * No optimizer. Phase 1 needs a correct IR, not a fast one.
//   * No vectors/SIMD. Out of scope until AVX-on-NEON is separately analysed.
//   * No x87. Out of scope.
//   * No host register allocation. `AllocateGpr` exists so the concept is
//     representable and validated, but Phase 1 does not run an allocator.
//
// ============================================================================

#ifndef P5EMU_CPU_IR_H
#define P5EMU_CPU_IR_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "p5emu/core/Status.h"
#include "p5emu/cpu/GuestMemory.h"

namespace p5emu::ir {

// ---------------------------------------------------------------------------
// Compact IDs
// ---------------------------------------------------------------------------
// 32-bit, dense. Chosen so a whole program fits comfortably in cache lines of
// IDs and so the whole IR is trivially serialisable. No pointer is ever stored
// in the IR, so moving the backing vector never invalidates a reference.
using ValueId = std::uint32_t;
using BlockId = std::uint32_t;
using OpId = std::uint16_t;

inline constexpr ValueId kInvalidValue = 0xffffffffu;
inline constexpr BlockId kInvalidBlock = 0xffffffffu;
inline constexpr OpId kInvalidOp = 0xffffu;

// ---------------------------------------------------------------------------
// Value types
// ---------------------------------------------------------------------------
enum class ValueType : std::uint8_t {
  kInvalid = 0,
  kI8,
  kI16,
  kI32,
  kI64,
  kFlags,
  // Placeholders, reserved so the enum is stable. Not producible in Phase 1.
  kV128,
  kF32,
  kF64,
};

// Returns the bit width of a scalar type, or 0 for kInvalid/kFlags.
std::uint32_t TypeWidthBits(ValueType type) noexcept;

// Returns the x86 architectural register number that a scalar type occupies, or
// 0xFFFFFFFF when the type has no register representation. kI8/kI16 are
// register *views* of a wider register, so they map to none.
std::uint32_t TypeArchRegister(ValueType type) noexcept;

// Canonical textual name, used by the deterministic dump.
const char* TypeName(ValueType type) noexcept;

// x86 condition codes. These are architectural facts, named so that IR text is
// readable and so that flag semantics are auditable.
enum class ConditionCode : std::uint8_t {
  kO = 0,   // overflow
  kNO,     // not overflow
  kB,      // below (CF==1)
  kAE,     // above or equal (CF==0)
  kE,      // equal (ZF==1)
  kNE,     // not equal
  kBE,     // below or equal (CF==1 || ZF==1)
  kA,      // above (CF==0 && ZF==0)
  kS,      // sign (SF==1)
  kNS,     // not sign
  kP,      // parity (PF==1)
  kNP,     // not parity
  kL,      // less (SF!=OF)
  kGE,     // greater or equal
  kLE,     // less or equal (ZF==1 || SF!=OF)
  kG,      // greater
};

const char* ConditionCodeName(ConditionCode cc) noexcept;

// CPU state accessors the IR can name. This is a *guest architectural* register
// set, deliberately not a host register set.
//
// `kRsp` and `kRip` are separated from the general set because control flow
// treats them differently.
enum class ArchRegister : std::uint8_t {
  kInvalid = 0,
  kRax, kRcx, kRdx, kRbx,
  kRsp, kRbp, kRsi, kRdi,
  kR8, kR9, kR10, kR11, kR12, kR13, kR14, kR15,
  kRip,
  kEflags,
  kInvalidSentinel,  // Validation must reject this. Kept for negative tests.
};

// Returns the architectural encoding 0..15 for a general-purpose register, or
// 0xFF if the register is not one of them.
std::uint32_t ArchRegisterNumber(ArchRegister reg) noexcept;
const char* ArchRegisterName(ArchRegister reg) noexcept;

// ---------------------------------------------------------------------------
// Terminators
// ---------------------------------------------------------------------------
enum class TerminatorKind : std::uint8_t {
  kInvalid = 0,
  kFallthrough,  // no successor; execution leaves the program
  kJump,         // unconditional to `target`
  kBranch,       // conditional: next if cond(value) else target
  // RET is architecturally a JUMP to a popped value, not a return from the
  // program. Modelling it as kReturn ends execution at the callee's `ret`, so a
  // caller's remaining code never runs.
  kIndirectJump, // jump to `cond_value` (which holds the target address)
  kReturn,       // leave the program entirely (end of guest code)
  // A call to an EXTERNAL host binding (an HLE import). The guest has already
  // pushed the return address; `external_target` is the guest binding address
  // and `fallthrough` is the block execution resumes in after the binding
  // returns. The IR does not know what an HLE function is -- only that control
  // transfers to a guest address that is not guest code.
  kExternalCall,
  kTrap,         // deliberate trap; Status becomes kUnimplementedInstruction
};

const char* TerminatorKindName(TerminatorKind kind) noexcept;

// ---------------------------------------------------------------------------
// Instructions
// ---------------------------------------------------------------------------
enum class OpKind : std::uint8_t {
  kInvalid = 0,

  // -- CPU state --
  kGetReg,   // reg -> value
  kSetReg,   // reg, value

  // -- Memory (guest) --
  // `addr` is a ValueId holding a guest virtual address. Faults are possible,
  // which is the whole reason these are distinct from GetReg/SetReg.
  kLoad,     // addr, width -> value zero-extended to 64
  kStore,    // addr, value, width

  // -- Constants --
  // Explicit, so that a constant is visible in the dump as an operation rather
  // than as a magic literal in an operand slot, and so a future folding pass has
  // an ordinary unary op to recognise.
  kConst,  // imm -> value (zero-extended into a kI64)

  // -- Integer ALU --
  // Each produces a plain value and leaves flags untouched.
  kAdd, kSub, kAnd, kOr, kXor,

  // -- Flag-producing ALU --
  // Each produces value AND flags. Keeping them as distinct opcodes (rather than
  // one op with an optional side effect) means the interpreter has no implicit
  // behaviour to get wrong, and ValidateIr can require the flags result.
  //
  // AND/OR/XOR are SEPARATE opcodes, not one kLogicFlags with a selector.
  // A single kLogicFlags op was a real Phase-1 defect: it did not carry which
  // logical operation it was, so the interpreter hardcoded `a & b` and silently
  // computed OR and XOR as AND. Splitting them makes the operation explicit in
  // the IR, the dump, and every backend, so it cannot be lost again.
  kAddFlags, kSubFlags, kAndFlags, kOrFlags, kXorFlags,

  // -- Flags --
  //
  // kLoadFlags / kStoreFlags move EFLAGS between the machine and the SSA world.
  // kCmp / kTest WRITE a flags value rather than mutating EFLAGS directly,
  // because x86 CMP and TEST both write EFLAGS and a backend must not be able to
  // "optimise away" the write. The builder emits a kStoreFlags immediately after,
  // so the IR shows the write explicitly.
  kLoadFlags,   // -> flags value
  kStoreFlags,  // flags value -> ()
  kTest,        // a, b, flags_out        (result discarded, flags written)
  kCmp,         // a, b, flags_out        (a-b discarded, flags written)

  // -- Shifts --
  kShl, kShr, kSar,  // value, count -> value, flags

  // -- Narrowing / extension --
  kExtend,  // value -> value. `mode` selects the extension mode.
  kTruncate,// value -> value, keep low N bits and zero-extend to 64

  // -- Conditions --
  kTestCc,   // flags -> i8 (0 or 1)

  // -- Address arithmetic (no memory access, no flags) --
  //
  // kAddrCalc: dest = imm + (sign_extend(imm_high)) + args[0]*width
  //
  // Dedicated op rather than a multiply plus adds, because on x86 address
  // *computation* (LEA) and address *access* (a memory operand) are different
  // operations: LEA touches no memory and sets no flags. Fusing them would hide
  // a real architectural distinction, and a backend optimising the fusion would
  // produce wrong behaviour for LEA.
  //
  // width carries the scale (1/2/4/8); args is empty for an absolute address.
  kAddrCalc,

  kNop,
};

const char* OpKindName(OpKind kind) noexcept;

// Extension modes for kExtend.
enum class ExtendMode : std::uint8_t {
  kNone = 0,
  kZeroExtend32,  // writing a 32-bit register zero-extends to 64
  kSignExtend8,
  kSignExtend16,
  kSignExtend32,
};

const char* ExtendModeName(ExtendMode mode) noexcept;

// Operand roles, so ValidateIr can check that arguments appear in the right
// positions without a switch per opcode at every call site.
enum class OperandRole : std::uint8_t {
  kDest = 0,   // value produced
  kSrc,        // general value operand
  kSrc2,       // second general value operand
  kFlags,      // flags operand
  kFlagsOut,   // flags result
  kReg,        // architectural register selector (immediate, not a ValueId)
  kWidth,      // byte width (immediate)
  kMode,       // ExtendMode (immediate)
  kCond,       // ConditionCode (immediate)
  kAddr,       // address operand (address-typed ValueId)
  kImm,        // unsigned immediate, read from Op::imm
  kSImm,       // signed immediate, sign-extended from Op::imm
  kScale,      // scale factor (immediate, read from Op::imm)
};

struct Op {
  OpKind kind = OpKind::kInvalid;
  ValueId dest = kInvalidValue;      // kDest
  std::vector<ValueId> args;         // ordered by position, see OperandRole
  std::vector<OperandRole> roles;    // parallel to `args`
  ArchRegister reg = ArchRegister::kInvalid;  // kReg
  // Operand width in bytes: 1, 2, 4 or 8.
  //
  // Carried by kLoad/kStore (access width), kTruncate and kAddrCalc (scale), and
  // as of Phase 3 also by kGetReg/kSetReg and the flag-producing ALU ops and
  // shifts. For those, it is the x86 OPERAND SIZE, and it drives real
  // width-specific behaviour -- flag computation at bit 8*width, partial-
  // register read/write masks, and count masking -- rather than a post-hoc mask.
  //
  // 0 means "unspecified"; consumers treat it as 8 for the ops where it is
  // meaningful. The builder always writes an explicit value.
  std::uint32_t width = 0;           // kWidth
  ExtendMode mode = ExtendMode::kNone;  // kMode
  ConditionCode cond = ConditionCode::kO;  // kCond
  std::uint64_t imm = 0;             // kImm / kSImm / kScale
  std::uint64_t imm_high = 0;        // kSImm high bits / signed displacement

  // Guest address this instruction was translated from. Present for
  // diagnostics and for differential reporting; it has NO effect on semantics,
  // which is why two blocks differing only in these values must still be
  // behaviourally identical.
  memory::GuestAddr source_address = 0;
};

// ---------------------------------------------------------------------------
// Terminator
// ---------------------------------------------------------------------------
struct Terminator {
  TerminatorKind kind = TerminatorKind::kInvalid;
  // kBranch: the FLAGS operand. kIndirectJump: the target address.
  ValueId cond_value = kInvalidValue;
  ConditionCode cond = ConditionCode::kO;
  BlockId target = kInvalidBlock;       // kJump: sole successor.
                                         // kBranch: the TAKEN successor.
  // kBranch: the NOT-TAKEN successor.
  //
  // Explicit rather than inferred as "target + 1". Inferring it would couple
  // block identity to block *order* in the container, which is an unstated
  // invariant a future block reordering pass would silently break. An implicit
  // fallthrough is exactly the kind of hidden coupling that makes differential
  // results unreproducible.
  BlockId fallthrough = kInvalidBlock;

  // kExternalCall: the guest binding address control transfers to. Guest
  // addresses and host pointers are different domains; this is an address the
  // dispatcher maps to an HLE binding, never a raw host pointer.
  memory::GuestAddr external_target = 0;

  memory::GuestAddr source_address = 0;
};

// ---------------------------------------------------------------------------
// Block / Program
// ---------------------------------------------------------------------------
struct Block {
  BlockId id = kInvalidBlock;
  std::vector<Op> ops;
  Terminator terminator;
  // Guest address this block was translated from. For kFallthrough/kReturn this
  // is the address execution leaves at.
  memory::GuestAddr entry_address = 0;
};

struct Program {
  std::string name;
  std::vector<Block> blocks;
  BlockId entry = kInvalidBlock;

  [[nodiscard]] const Block* FindBlock(BlockId id) const noexcept;
  [[nodiscard]] Block* FindBlock(BlockId id) noexcept;

  // Resolves a GUEST ADDRESS to the block that starts there.
  //
  // Needed because a RET is an indirect control transfer: the target is a value
  // computed at run time, so it cannot be resolved statically, only looked up
  // when it is known to be a block entry.
  [[nodiscard]] const Block* FindBlockByAddress(memory::GuestAddr address) const noexcept;
  [[nodiscard]] Block* FindBlockByAddress(memory::GuestAddr address) noexcept;

  [[nodiscard]] std::size_t BlockCount() const noexcept { return blocks.size(); }
};

// ---------------------------------------------------------------------------
// Deterministic textual dump
// ---------------------------------------------------------------------------
// Byte-identical for a given Program. Used by golden tests and by cross-backend
// differential comparison. Stability is a contract: if this format changes, the
// change is deliberate and all goldens are regenerated in the same commit.
std::string Dump(const Program& program);

// ---------------------------------------------------------------------------
// ValidateIr
// ---------------------------------------------------------------------------
// Returns kOk, or a Status whose code identifies the failure class. The
// detailed reason (which op, which value, which block) is available from
// ValidationReport so failures are diagnosable rather than opaque.
Status ValidateIr(const Program& program);

// Structured detail for a failed validation. Deliberately simple: enough to
// print one actionable line.
struct ValidationReport {
  bool valid = false;
  const char* rule = "";       // stable rule identifier, e.g. "undefined-value"
  BlockId block = kInvalidBlock;
  ValueId value = kInvalidValue;
  OpId op_index = kInvalidOp;
  std::string detail;
};

// Same checks, but returns the report instead of only a Status.
Status ValidateIrDetailed(const Program& program, ValidationReport* report);

}  // namespace p5emu::ir

#endif  // P5EMU_CPU_IR_H
