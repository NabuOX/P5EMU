// SPDX-License-Identifier: Apache-2.0
//
// P5EMU is an independent, open-source project. See LICENSE and
// PROVENANCE.md before adding code here.
//
// See ReferenceInterpreter.h for why this file is written for clarity rather
// than speed. The short version: every flag computation below states the
// architectural rule it implements, because that is what makes it auditable
// against the Intel SDM by a reviewer rather than by trusting the author.

#include "p5emu/cpu/ReferenceInterpreter.h"

#include <cstdio>
#include <cstring>

#include "p5emu/cpu/Ir.h"

namespace p5emu::cpu {
namespace {

using ir::ArchRegister;
using ir::Block;
using ir::BlockId;
using ir::ConditionCode;
using ir::ExtendMode;
using ir::Op;
using ir::OpKind;
using ir::Program;
using ir::TerminatorKind;
using ir::ValueId;
using ir::ValueType;

// ---------------------------------------------------------------------------
// Bit helpers, written longhand on purpose
// ---------------------------------------------------------------------------
inline std::uint64_t SignExtend64(std::uint64_t value, std::uint32_t bits) {
  if (bits == 0 || bits >= 64) {
    return value;
  }
  const std::uint64_t sign = std::uint64_t{1} << (bits - 1u);
  value &= (std::uint64_t{1} << bits) - 1u;
  return (value ^ sign) - sign;
}

// x86 parity flag: 1 when the low byte has an EVEN number of set bits.
// Intel SDM Vol. 2 §3.1.2: "parity flag ... set if the least-significant byte
// contains an even number of 1-bits".
inline bool ParityEven(std::uint64_t low_byte) {
  std::uint32_t v = static_cast<std::uint32_t>(low_byte & 0xffu);
  v ^= v >> 4;
  v ^= v >> 2;
  v ^= v >> 1;
  return (v & 1u) == 0u;
}

// Auxiliary carry: carry out of bit 3 into bit 4. Intel SDM Vol. 2 §3.1.2.
inline bool AuxCarryAdd(std::uint64_t a, std::uint64_t b, std::uint64_t carry_in) {
  return ((a & 0xfu) + (b & 0xfu) + carry_in) > 0xfu;
}

// Auxiliary carry for subtraction: carry (borrow) out of bit 3.
//
// The low nibbles must be compared as SIGNED values. Doing the comparison in
// unsigned arithmetic is a real bug: `(a & 0xf) - (b & 0xf)` in uint64 wraps, so
// the result can never be "less than zero" and AF would never be set. Intel SDM
// Vol. 2 §3.1.2: AF is set when there is a borrow from bit 4.
inline bool AuxCarrySub(std::uint64_t a, std::uint64_t b, std::uint64_t borrow_in) {
  const std::int64_t a_lo = static_cast<std::int64_t>(a & 0xfu);
  const std::int64_t b_lo = static_cast<std::int64_t>(b & 0xfu);
  return (a_lo - b_lo - static_cast<std::int64_t>(borrow_in)) < 0;
}

// x86 masks the shift count to 6 bits for 64-bit operands, so a count of 64
// yields a zero result rather than undefined behaviour.
// Intel SDM Vol. 2 §2.2.2 / SAL/SAR/SHR behaviour for 64-bit operands.
// Operand width model (Phase 3).
//
// Width is in BYTES: 1, 2, 4 or 8. 0 (unspecified) is treated as 8, so hand-built
// IR that predates widths keeps its 64-bit meaning.
inline std::uint32_t NormWidth(std::uint32_t width_bytes) noexcept {
  return (width_bytes == 1 || width_bytes == 2 || width_bytes == 4) ? width_bytes
                                                                    : 8u;
}
inline std::uint32_t WidthBits(std::uint32_t width_bytes) noexcept {
  return 8u * NormWidth(width_bytes);
}
inline std::uint64_t WidthMask(std::uint32_t width_bytes) noexcept {
  const std::uint32_t w = NormWidth(width_bytes);
  return (w >= 8) ? ~std::uint64_t{0} : ((std::uint64_t{1} << (8u * w)) - 1u);
}
// x86 masks a shift count to 5 bits, or 6 for a 64-bit operand. Intel SDM
// Vol. 2 §3.5 ("the count is masked to 5 bits, or 6 bits if REX.W").
inline std::uint32_t MaskShiftCount(std::uint64_t count,
                                    std::uint32_t width_bytes) noexcept {
  return static_cast<std::uint32_t>(count &
                                    ((NormWidth(width_bytes) == 8) ? 0x3fu
                                                                   : 0x1fu));
}

// 128-bit helpers for the wide arithmetic families (Phase 11). Host is GCC/Clang
// on x86-64, so 128-bit integers are available; the __extension__ typedefs keep
// -pedantic quiet. Every signed edge case is handled explicitly so no host UB is
// relied upon.
__extension__ typedef unsigned __int128 u128;
__extension__ typedef __int128 i128;

// Sign-extends the low `total_bits` bits of a 128-bit value. Used by the wide
// multiply/divide families. Host is GCC/Clang on x86-64, where i128 exists;
// every signed edge case is handled explicitly so no host UB is relied upon.
inline i128 SignExtend128(u128 v, std::uint32_t total_bits) {
  if (total_bits == 0) return 0;
  if (total_bits >= 128) return static_cast<i128>(v);
  const std::uint32_t sh = 128u - total_bits;
  return static_cast<i128>(v << sh) >> sh;
}

// Flags and result for a flag-producing arithmetic/logic op at a given operand
// width. The operands are masked to the width FIRST, so every flag is defined at
// bit (8*width - 1) rather than at bit 63. This is the whole reason the ops carry
// a width: computing a 32-bit ADD's carry from bit 63 is simply wrong.
//
// Intel SDM Vol. 2 §3.1.2 for each flag.
inline std::uint64_t ComputeArithFlags(OpKind kind, std::uint64_t a,
                                       std::uint64_t b,
                                       std::uint32_t width_bytes,
                                       std::uint64_t* result_out,
                                       std::uint64_t carry_in = 0) {
  const std::uint32_t bits = WidthBits(width_bytes);
  const std::uint64_t m = WidthMask(width_bytes);
  a &= m;
  b &= m;

  std::uint64_t f = 0;
  std::uint64_t r = 0;
  const auto sign_bit = [bits](std::uint64_t v) {
    return ((v >> (bits - 1u)) & 1u) != 0;
  };

  switch (kind) {
    // ADC: DEST + SRC + CF. The carry-in is a full third operand, so the flags
    // are computed from the true triple sum, never from a truncated a+b.
    case OpKind::kAdcFlags: {
      const u128 s =
          static_cast<u128>(a) + b + carry_in;
      r = static_cast<std::uint64_t>(s) & m;
      if (((s >> bits) & 1u) != 0) f |= eflags::kCf;
      const bool sa = sign_bit(a);
      const bool sb = sign_bit(b);
      const bool sr = sign_bit(r);
      if (sa == sb && sr != sa) f |= eflags::kOf;
      if (AuxCarryAdd(a, b, carry_in)) f |= eflags::kAf;
      break;
    }
    // SBB: DEST - SRC - CF. The borrow-in is a full third operand.
    case OpKind::kSbbFlags: {
      r = (a - b - carry_in) & m;
      if (a < b || (a == b && carry_in == 1)) f |= eflags::kCf;
      const bool sa = sign_bit(a);
      const bool sb = sign_bit(b);
      const bool sr = sign_bit(r);
      if (sa != sb && sr != sa) f |= eflags::kOf;
      if (AuxCarrySub(a, b, carry_in)) f |= eflags::kAf;
      break;
    }
    case OpKind::kAddFlags: {
      r = (a + b) & m;
      // CF: carry out of bit (bits-1). For bits == 64 the sum wraps, so it is
      // detected as r < a.
      if (bits < 64) {
        if ((a + b) > m) {
          f |= eflags::kCf;
        }
      } else if (r < a) {
        f |= eflags::kCf;
      }
      const bool sa = sign_bit(a);
      const bool sb = sign_bit(b);
      const bool sr = sign_bit(r);
      if (sa == sb && sr != sa) {
        f |= eflags::kOf;
      }
      if (AuxCarryAdd(a, b, 0)) {
        f |= eflags::kAf;
      }
      break;
    }
    case OpKind::kSubFlags:
    case OpKind::kCmp: {
      r = (a - b) & m;
      // CF: unsigned borrow. Once both operands are masked to the width this is
      // width-independent: a < b iff the subtraction borrows.
      if (a < b) {
        f |= eflags::kCf;
      }
      const bool sa = sign_bit(a);
      const bool sb = sign_bit(b);
      const bool sr = sign_bit(r);
      if (sa != sb && sr != sa) {
        f |= eflags::kOf;
      }
      if (AuxCarrySub(a, b, 0)) {
        f |= eflags::kAf;
      }
      break;
    }
    case OpKind::kAndFlags:
    case OpKind::kTest:
      r = (a & b) & m;
      break;
    case OpKind::kOrFlags:
      r = (a | b) & m;
      break;
    case OpKind::kXorFlags:
      r = (a ^ b) & m;
      break;
    default:
      r = 0;
      break;
  }

  if (r == 0) {
    f |= eflags::kZf;
  }
  if (sign_bit(r)) {
    f |= eflags::kSf;
  }
  if (ParityEven(r)) {
    f |= eflags::kPf;
  }
  *result_out = r;
  return f;
}

// Result and flags for SHL/SHR/SAR at a given operand width.
//
// x86 leaves FLAGS UNCHANGED when the (masked) count is zero, so `prior_flags`
// is returned in that case. OF is architecturally UNDEFINED for counts other
// than 1 (and for counts at or beyond the operand size); P5EMU makes a
// documented deterministic choice (0) and says so here, because "undefined" is
// not "whatever the code happened to do". The differential harness excludes those
// bits through a fixture-specific observable mask rather than requiring two
// backends to agree on an invented value.
inline void ComputeShift(OpKind kind, std::uint64_t value, std::uint64_t raw_count,
                         std::uint32_t width_bytes, std::uint64_t prior_flags,
                         std::uint64_t* result_out, std::uint64_t* flags_out) {
  const std::uint32_t bits = WidthBits(width_bytes);
  const std::uint64_t m = WidthMask(width_bytes);
  const std::uint64_t v = value & m;
  const std::uint32_t count = MaskShiftCount(raw_count, width_bytes);

  if (count == 0) {
    *result_out = v;
    *flags_out = prior_flags;  // Intel SDM: a zero count changes nothing.
    return;
  }

  std::uint64_t shifted = 0;
  std::uint64_t f = 0;
  switch (kind) {
    case OpKind::kShl:
      shifted = (count < bits) ? ((v << count) & m) : 0u;
      if (count <= bits && ((v >> (bits - count)) & 1u) != 0u) {
        f |= eflags::kCf;
      }
      if (count == 1) {
        const bool msb_before = ((v >> (bits - 1u)) & 1u) != 0;
        const bool msb_after = ((shifted >> (bits - 1u)) & 1u) != 0;
        if (msb_before != msb_after) {
          f |= eflags::kOf;
        }
      }
      break;
    case OpKind::kShr:
      shifted = (count < bits) ? (v >> count) : 0u;
      if (count <= bits && ((v >> (count - 1u)) & 1u) != 0u) {
        f |= eflags::kCf;
      }
      if (count == 1 && ((v >> (bits - 1u)) & 1u) != 0u) {
        f |= eflags::kOf;
      }
      break;
    case OpKind::kSar: {
      const std::int64_t sv = static_cast<std::int64_t>(SignExtend64(v, bits));
      shifted = static_cast<std::uint64_t>(sv >> count) & m;
      if (count <= bits && ((v >> (count - 1u)) & 1u) != 0u) {
        f |= eflags::kCf;
      }
      // OF is 0 for count == 1 and undefined for count > 1; deterministic 0.
      break;
    }
    default:
      break;
  }
  if (shifted == 0) {
    f |= eflags::kZf;
  }
  if (((shifted >> (bits - 1u)) & 1u) != 0u) {
    f |= eflags::kSf;
  }
  if (ParityEven(shifted)) {
    f |= eflags::kPf;
  }
  *result_out = shifted;
  *flags_out = f;
}

// AH/CH/DH/BH are byte views of bits 8..15 of RAX/RCX/RDX/RBX.
inline bool IsHighByteRegister(ArchRegister r) noexcept {
  return r == ArchRegister::kAh || r == ArchRegister::kCh ||
         r == ArchRegister::kDh || r == ArchRegister::kBh;
}

std::uint64_t* RegisterSlot(CpuState* s, ArchRegister reg) {
  switch (reg) {
    case ArchRegister::kRax: return &s->rax;
    case ArchRegister::kRcx: return &s->rcx;
    case ArchRegister::kRdx: return &s->rdx;
    case ArchRegister::kRbx: return &s->rbx;
    case ArchRegister::kRsp: return &s->rsp;
    case ArchRegister::kRbp: return &s->rbp;
    case ArchRegister::kRsi: return &s->rsi;
    case ArchRegister::kRdi: return &s->rdi;
    case ArchRegister::kR8:  return &s->r8;
    case ArchRegister::kR9:  return &s->r9;
    case ArchRegister::kR10: return &s->r10;
    case ArchRegister::kR11: return &s->r11;
    case ArchRegister::kR12: return &s->r12;
    case ArchRegister::kR13: return &s->r13;
    case ArchRegister::kR14: return &s->r14;
    case ArchRegister::kR15: return &s->r15;
    case ArchRegister::kRip: return &s->rip;
    case ArchRegister::kEflags: return &s->eflags;
    // High-byte views return the PARENT 64-bit slot; the get/set cases apply the
    // bits 8..15 subset.
    case ArchRegister::kAh: return &s->rax;
    case ArchRegister::kCh: return &s->rcx;
    case ArchRegister::kDh: return &s->rdx;
    case ArchRegister::kBh: return &s->rbx;
    case ArchRegister::kInvalid:
    case ArchRegister::kInvalidSentinel:
      return nullptr;
  }
  return nullptr;
}

// Evaluates an x86 condition code against EFLAGS.
// Intel SDM Vol. 2 §3.1.2.1 "Condition Codes" and Table 3-1 "Condition-Code
// Names and Their Meanings".
bool EvaluateCondition(ConditionCode cc, std::uint64_t eflags) {
  const bool cf = (eflags & eflags::kCf) != 0;
  const bool zf = (eflags & eflags::kZf) != 0;
  const bool sf = (eflags & eflags::kSf) != 0;
  const bool of = (eflags & eflags::kOf) != 0;
  const bool pf = (eflags & eflags::kPf) != 0;

  switch (cc) {
    case ConditionCode::kO:  return of;
    case ConditionCode::kNO: return !of;
    case ConditionCode::kB:  return cf;                 // below / carry
    case ConditionCode::kAE: return !cf;
    case ConditionCode::kE:  return zf;                 // equal / zero
    case ConditionCode::kNE: return !zf;
    case ConditionCode::kBE: return cf || zf;
    case ConditionCode::kA:  return !cf && !zf;
    case ConditionCode::kS:  return sf;                 // sign
    case ConditionCode::kNS: return !sf;
    case ConditionCode::kP:  return pf;                 // parity
    case ConditionCode::kNP: return !pf;
    case ConditionCode::kL:  return sf != of;           // less (signed)
    case ConditionCode::kGE: return sf == of;
    case ConditionCode::kLE: return zf || (sf != of);
    case ConditionCode::kG:  return !zf && (sf == of);
  }
  return false;
}

// The interpreter's value store. Values are 64-bit slots; a narrower typed
// value is stored with the same bits a real 64-bit register would hold, which
// is what x86 semantics require for a 32-bit destination (zero extension) and
// what kExtend makes explicit elsewhere.
class ValueStore {
 public:
  void Ensure(ValueId id, ValueType type) {
    if (id >= slots_.size()) {
      slots_.resize(id + 1u);
      types_.resize(id + 1u, ValueType::kInvalid);
    }
    slots_[id] = 0;
    types_[id] = type;
  }

  // Grows the backing store if `id` is not yet present.
  //
  // This must be defensive. Flagging ops write their FLAGS DESTINATION
  // (`args[2]`), which is a value slot like any other but was never passed to
  // Ensure by the old code. An id one past the end wrote through the end of
  // `slots_`, corrupting the adjacent `types_` vector's control block -- a heap
  // overflow that happened to be silent until AddressSanitizer reported a
  // bogus free in ~ValueStore.
  void Set(ValueId id, std::uint64_t v) {
    if (id >= slots_.size()) {
      slots_.resize(id + 1u, 0u);
      types_.resize(id + 1u, ValueType::kInvalid);
    }
    slots_[id] = v;
  }

  // Stores the flags produced by an ALU op into its flags destination.
  // Separate from Set so that the destination also gets its type recorded.
  void SetFlags(ValueId id, std::uint64_t value) {
    Ensure(id, ValueType::kFlags);
    slots_[id] = value;
  }

  [[nodiscard]] std::uint64_t Get(ValueId id) const {
    // ValidateIr rejects use-before-definition, so a valid program never
    // reaches this with an out-of-range id. The guard keeps a malformed program
    // a wrong answer instead of an out-of-bounds read.
    return (id < slots_.size()) ? slots_[id] : 0u;
  }

 private:
  std::vector<std::uint64_t> slots_;
  std::vector<ValueType> types_;
};

}  // namespace

const char* TerminationReasonName(TerminationReason reason) noexcept {
  switch (reason) {
    case TerminationReason::kRunning:     return "running";
    case TerminationReason::kFallthrough: return "fallthrough";
    case TerminationReason::kReturn:      return "return";
    case TerminationReason::kIndirectJump: return "indirect-jump";
    case TerminationReason::kTrap:        return "trap";
    case TerminationReason::kStepLimit:   return "step-limit";
    case TerminationReason::kFault:       return "fault";
    case TerminationReason::kIrInvalid:   return "ir-invalid";
    case TerminationReason::kHleError:    return "hle-error";
    case TerminationReason::kCpuFault:    return "cpu-fault";
  }
  return "unknown";
}

// ---------------------------------------------------------------------------
// Execute
// ---------------------------------------------------------------------------
ExecutionResult ReferenceInterpreter::Execute(const Program& program,
                                              CpuState* state,
                                              memory::GuestMemoryPort& memory) {
  ExecutionResult result;

  if (state == nullptr) {
    result.status = StatusCode::kInvalidArgument;
    result.reason = TerminationReason::kFault;
    return result;
  }

  // Validate before executing. A malformed IR must not reach evaluation: the
  // interpreter tolerating malformed IR while a future backend does not would
  // make the differential comparison meaningless.
  const Status valid = ir::ValidateIr(program);
  if (!valid.ok()) {
    result.status = valid;
    result.reason = TerminationReason::kIrInvalid;
    result.executed_ops = 0;
    result.final_rip = state->rip;
    return result;
  }

  ValueStore values;
  std::uint64_t flags_value = state->eflags;

  BlockId current = program.entry;
  std::uint64_t steps = 0;
  bool halted = false;

  while (current != ir::kInvalidBlock && !halted) {
    const Block* block = program.FindBlock(current);
    if (block == nullptr) {
      result.status = StatusCode::kInvalidIr;
      result.reason = TerminationReason::kIrInvalid;
      break;
    }

    // The step budget covers BLOCK TRANSFERS as well as ops.
    //
    // Checking only inside the op loop let a block with no ops that jumps to
    // itself loop forever: `jmp .` emits a terminator and no ops, so the op loop
    // never ran and the limit was never consulted. A self-loop is the most
    // common form of runaway guest code, so this is a hang, not a corner case.
    if (steps >= step_limit_) {
      result.status = StatusCode::kExecutionLimit;
      result.reason = TerminationReason::kStepLimit;
      break;
    }
    ++steps;

    for (const Op& op : block->ops) {
      if (steps >= step_limit_) {
        result.status = StatusCode::kExecutionLimit;
        result.reason = TerminationReason::kStepLimit;
        halted = true;
        break;
      }
      ++steps;

      switch (op.kind) {
        // ---- constants -----------------------------------------------
        case OpKind::kConst: {
          values.Ensure(op.dest, ValueType::kI64);
          values.Set(op.dest, op.imm);
          break;
        }

        // ---- CPU state ------------------------------------------------
        case OpKind::kGetReg: {
          std::uint64_t* slot = RegisterSlot(state, op.reg);
          values.Ensure(op.dest, ValueType::kI64);
          // A narrow read observes only the low bits (AL is RAX[7:0]); a
          // high-byte view observes bits 8..15.
          const std::uint64_t v = (slot != nullptr) ? *slot : 0u;
          if (IsHighByteRegister(op.reg)) {
            values.Set(op.dest, (v >> 8) & 0xFFu);
          } else {
            values.Set(op.dest, v & WidthMask(op.width));
          }
          break;
        }
        case OpKind::kSetReg: {
          std::uint64_t* slot = RegisterSlot(state, op.reg);
          if (slot != nullptr) {
            const std::uint64_t v = values.Get(op.args[0]);
            if (IsHighByteRegister(op.reg)) {
              // AH/CH/DH/BH: replace bits 8..15, preserving all others.
              *slot = (*slot & ~std::uint64_t{0xFF00u}) | ((v & 0xFFu) << 8);
            } else {
              const std::uint32_t w = NormWidth(op.width);
              const std::uint64_t m = WidthMask(op.width);
              if (w == 8) {
                *slot = v;                       // RAX: replace all 64 bits
              } else if (w == 4) {
                *slot = v & 0xFFFFFFFFull;       // EAX: zero-extend into RAX
              } else {
                *slot = (*slot & ~m) | (v & m);  // AL/AX: preserve upper bits
              }
            }
          }
          break;
        }

        // ---- guest memory ---------------------------------------------
        case OpKind::kLoad: {
          const std::uint64_t addr = values.Get(op.args[0]);
          std::uint64_t out = 0;
          // Loads are zero-extended to 64 bits: Intel SDM Vol. 2 §2.2.1,
          // "32-bit operands generate a 32-bit result, zero-extended to a
          // 64-bit result in the destination general-purpose register".
          const Status st = memory.ReadData(addr, op.width, &out);
          if (!st.ok()) {
            result.status = st;
            result.reason = TerminationReason::kFault;
            halted = true;
            break;
          }
          values.Ensure(op.dest, ValueType::kI64);
          values.Set(op.dest, out);
          break;
        }
        case OpKind::kStore: {
          const std::uint64_t addr = values.Get(op.args[0]);
          const std::uint64_t value = values.Get(op.args[1]);
          const Status st = memory.WriteData(addr, op.width, value);
          if (!st.ok()) {
            result.status = st;
            result.reason = TerminationReason::kFault;
            halted = true;
            break;
          }
          break;
        }

        // ---- integer ALU, no flags ------------------------------------
        // Written as 64-bit unsigned wraparound. x86 arithmetic is modulo 2^64
        // for 64-bit operands; no C++ signed overflow is relied upon.
        case OpKind::kAdd:
          values.Ensure(op.dest, ValueType::kI64);
          values.Set(op.dest, values.Get(op.args[0]) + values.Get(op.args[1]));
          break;
        case OpKind::kSub:
          values.Ensure(op.dest, ValueType::kI64);
          values.Set(op.dest, values.Get(op.args[0]) - values.Get(op.args[1]));
          break;
        case OpKind::kAnd:
          values.Ensure(op.dest, ValueType::kI64);
          values.Set(op.dest, values.Get(op.args[0]) & values.Get(op.args[1]));
          break;
        case OpKind::kOr:
          values.Ensure(op.dest, ValueType::kI64);
          values.Set(op.dest, values.Get(op.args[0]) | values.Get(op.args[1]));
          break;
        case OpKind::kXor:
          values.Ensure(op.dest, ValueType::kI64);
          values.Set(op.dest, values.Get(op.args[0]) ^ values.Get(op.args[1]));
          break;

        // ---- flag-producing ALU ----------------------------------------
        // Intel SDM Vol. 2 §3.1.2 for each flag's definition.
        case OpKind::kAddFlags:
        case OpKind::kSubFlags:
        case OpKind::kAndFlags:
        case OpKind::kOrFlags:
        case OpKind::kXorFlags: {
          std::uint64_t r = 0;
          const std::uint64_t f =
              ComputeArithFlags(op.kind, values.Get(op.args[0]),
                                values.Get(op.args[1]), op.width, &r);
          values.Ensure(op.dest, ValueType::kI64);
          values.Set(op.dest, r);
          values.SetFlags(op.args[2], f);
          break;
        }

        // ---- ADC / SBB (carry-in) ---------------------------------------
        case OpKind::kAdcFlags:
        case OpKind::kSbbFlags: {
          std::uint64_t r = 0;
          const std::uint64_t f =
              ComputeArithFlags(op.kind, values.Get(op.args[0]),
                                values.Get(op.args[1]), op.width, &r,
                                values.Get(op.args[2]));
          values.Ensure(op.dest, ValueType::kI64);
          values.Set(op.dest, r);
          values.SetFlags(op.args[3], f);
          break;
        }

        // ---- MUL / IMUL (double-width product) --------------------------
        case OpKind::kMulFlags:
        case OpKind::kIMulFlags: {
          const std::uint32_t bits = WidthBits(op.width);
          const std::uint64_t m = WidthMask(op.width);
          const std::uint64_t a = values.Get(op.args[0]) & m;
          const std::uint64_t b = values.Get(op.args[1]) & m;
          std::uint64_t lo = 0;
          std::uint64_t hi = 0;
          std::uint64_t f = 0;
          if (op.kind == OpKind::kMulFlags) {
            const u128 p = static_cast<u128>(a) * b;
            lo = static_cast<std::uint64_t>(p) & m;
            hi = static_cast<std::uint64_t>(p >> bits) & m;
            if (hi != 0) f |= eflags::kCf | eflags::kOf;
          } else {
            const i128 p = SignExtend128(a, bits) * SignExtend128(b, bits);
            lo = static_cast<std::uint64_t>(p) & m;
            hi = static_cast<std::uint64_t>(p >> bits) & m;
            if (p != SignExtend128(lo, bits)) f |= eflags::kCf | eflags::kOf;
          }
          values.Ensure(op.dest, ValueType::kI64);
          values.Set(op.dest, lo);
          if (op.dest2 != ir::kInvalidValue) {
            values.Ensure(op.dest2, ValueType::kI64);
            values.Set(op.dest2, hi);
          }
          values.SetFlags(op.args[2], f);
          break;
        }

        // ---- DIV / IDIV (double-width dividend, divide fault) -----------
        case OpKind::kDivFlags:
        case OpKind::kIDivFlags: {
          const std::uint32_t bits = WidthBits(op.width);
          const std::uint64_t m = WidthMask(op.width);
          const std::uint64_t hi = values.Get(op.args[0]) & m;
          const std::uint64_t lo = values.Get(op.args[1]) & m;
          const std::uint64_t dv = values.Get(op.args[2]) & m;
          const u128 udiv =
              (static_cast<u128>(hi) << bits) | lo;
          std::uint64_t quot = 0;
          std::uint64_t rem = 0;
          bool fault = false;
          if (op.kind == OpKind::kDivFlags) {
            if (dv == 0) {
              fault = true;
            } else {
              const u128 q = udiv / dv;
              const u128 r = udiv % dv;
              if (q > m) {
                fault = true;  // quotient overflow (#DE)
              } else {
                quot = static_cast<std::uint64_t>(q) & m;
                rem = static_cast<std::uint64_t>(r) & m;
              }
            }
          } else {
            const i128 dividend = SignExtend128(udiv, 2u * bits);
            const i128 d = SignExtend128(dv, bits);
            if (d == 0) {
              fault = true;
            } else {
              i128 q = 0;
              i128 r = 0;
              if (d == -1) {
                // -dividend is UB for the most-negative value; special-case it.
                if (dividend == (static_cast<i128>(1) << 127)) {
                  fault = true;
                } else {
                  q = -dividend;
                  r = 0;
                }
              } else {
                q = dividend / d;
                r = dividend % d;
              }
              if (!fault) {
                const i128 qmin = -(static_cast<i128>(1) << (bits - 1u));
                const i128 qmax =
                    (static_cast<i128>(1) << (bits - 1u)) - 1;
                if (q < qmin || q > qmax) {
                  fault = true;  // quotient overflow (#DE)
                } else {
                  quot = static_cast<std::uint64_t>(q) & m;
                  rem = static_cast<std::uint64_t>(r) & m;
                }
              }
            }
          }
          if (fault) {
            // A divide error commits NOTHING: no quotient, no remainder.
            result.status = StatusCode::kDivideError;
            result.reason = TerminationReason::kCpuFault;
            halted = true;
            break;
          }
          values.Ensure(op.dest, ValueType::kI64);
          values.Set(op.dest, quot);
          if (op.dest2 != ir::kInvalidValue) {
            values.Ensure(op.dest2, ValueType::kI64);
            values.Set(op.dest2, rem);
          }
          break;
        }

        // ---- compare / test --------------------------------------------
        // Both write a flags VALUE; committing it to EFLAGS is a separate,
        // explicit kStoreFlags op, so a backend cannot treat the write as
        // optional and so it appears in the IR dump.
        case OpKind::kCmp:
        case OpKind::kTest: {
          std::uint64_t r = 0;
          const std::uint64_t f =
              ComputeArithFlags(op.kind, values.Get(op.args[0]),
                                values.Get(op.args[1]), op.width, &r);
          values.SetFlags(op.args[2], f);
          break;
        }

        // ---- shifts -----------------------------------------------------
        // Intel SDM Vol. 2 §2.2.2 / §3.5.
        case OpKind::kShl:
        case OpKind::kShr:
        case OpKind::kSar: {
          std::uint64_t r = 0;
          std::uint64_t f = 0;
          ComputeShift(op.kind, values.Get(op.args[0]), values.Get(op.args[1]),
                       op.width, flags_value, &r, &f);
          values.Ensure(op.dest, ValueType::kI64);
          values.Set(op.dest, r);
          values.SetFlags(op.args[2], f);
          break;
        }

        // ---- extension --------------------------------------------------
        case OpKind::kExtend: {
          const std::uint64_t v = values.Get(op.args[0]);
          std::uint64_t r = v;
          switch (op.mode) {
            case ExtendMode::kNone:
              r = v;
              break;
            case ExtendMode::kZeroExtend32:
              // Writing a 32-bit register zero-extends to 64.
              // Intel SDM Vol. 2 §2.2.1.
              r = v & 0xffffffffull;
              break;
            case ExtendMode::kSignExtend8:
              r = SignExtend64(v, 8);
              break;
            case ExtendMode::kSignExtend16:
              r = SignExtend64(v, 16);
              break;
            case ExtendMode::kSignExtend32:
              r = SignExtend64(v, 32);
              break;
          }
          values.Ensure(op.dest, ValueType::kI64);
          values.Set(op.dest, r);
          break;
        }
        case OpKind::kTruncate: {
          const std::uint64_t v = values.Get(op.args[0]);
          values.Ensure(op.dest, ValueType::kI64);
          values.Set(op.dest, v & ((std::uint64_t{1} << op.width) - 1u));
          break;
        }

        // ---- address calculation ----------------------------------------
        // Deliberately separate from Load/Store: address computation touches no
        // memory and sets no flags.
        case OpKind::kAddrCalc: {
          std::uint64_t addr = op.imm;
          addr += static_cast<std::uint64_t>(static_cast<std::int64_t>(op.imm_high));
          if (!op.args.empty()) {
            addr += values.Get(op.args[0]) * op.width;
          }
          values.Ensure(op.dest, ValueType::kI64);
          values.Set(op.dest, addr);
          break;
        }

        // ---- flags ------------------------------------------------------
        case OpKind::kLoadFlags: {
          values.Ensure(op.dest, ValueType::kFlags);
          values.Set(op.dest, flags_value);
          break;
        }
        case OpKind::kStoreFlags: {
          flags_value = values.Get(op.args[0]);
          break;
        }
        case OpKind::kTestCc: {
          values.Ensure(op.dest, ValueType::kI8);
          values.Set(op.dest, EvaluateCondition(op.cond, flags_value) ? 1u : 0u);
          break;
        }

        case OpKind::kNop:
          break;

        case OpKind::kInvalid:
          result.status = StatusCode::kInvalidIr;
          result.reason = TerminationReason::kIrInvalid;
          halted = true;
          break;
      }

      if (result.reason != TerminationReason::kRunning) {
        break;
      }
    }

    if (result.reason != TerminationReason::kRunning) {
      break;
    }

    // ---- terminator ----------------------------------------------------
    const ir::Terminator& t = block->terminator;
    switch (t.kind) {
      case TerminatorKind::kFallthrough:
        result.reason = TerminationReason::kFallthrough;
        current = ir::kInvalidBlock;
        break;
      case TerminatorKind::kJump:
        current = t.target;
        break;
      case TerminatorKind::kBranch: {
        const std::uint64_t f = values.Get(t.cond_value);
        // The branch reads the FLAGS *value* carried in the IR, not machine
        // EFLAGS. A divergence between "flags as a value" and "flags in a
        // register" therefore shows up as a differential mismatch instead of
        // being silently reconciled.
        current = EvaluateCondition(t.cond, f) ? t.target : t.fallthrough;
        break;
      }
      case TerminatorKind::kIndirectJump: {
        // A RET (or other indirect transfer) to a guest address. If the address
        // is a lowered block, execution resumes there -- this is how a guest
        // function returns to its caller. A target that is not a known block is
        // a return out of the program.
        const std::uint64_t target = values.Get(t.cond_value);
        const ir::Block* next = program.FindBlockByAddress(target);
        if (next == nullptr) {
          result.reason = TerminationReason::kReturn;
          current = ir::kInvalidBlock;
        } else {
          current = next->id;
        }
        break;
      }
      case TerminatorKind::kReturn:
        result.reason = TerminationReason::kReturn;
        current = ir::kInvalidBlock;
        break;
      case TerminatorKind::kExternalCall: {
        // An HLE import. The CALL lowering pushed the return address; dispatch
        // the binding, then pop it and resume in the return block.
        if (external_calls_ == nullptr) {
          result.status = StatusCode::kUnimplementedInstruction;
          result.reason = TerminationReason::kHleError;
          current = ir::kInvalidBlock;
          break;
        }
        const Status hs =
            external_calls_->HandleExternalCall(t.external_target, state,
                                                &memory);
        if (!hs.ok()) {
          result.status = hs;
          result.reason = TerminationReason::kHleError;
          current = ir::kInvalidBlock;
          break;
        }
        // HLE functions do not adjust the guest stack; the CALL pushed the
        // return address and this pop balances it.
        state->rsp += 8;
        current = t.fallthrough;
        break;
      }
      case TerminatorKind::kTrap:
        result.status = StatusCode::kUnimplementedInstruction;
        result.reason = TerminationReason::kTrap;
        current = ir::kInvalidBlock;
        break;
      case TerminatorKind::kInvalid:
        result.status = StatusCode::kInvalidIr;
        result.reason = TerminationReason::kIrInvalid;
        current = ir::kInvalidBlock;
        break;
    }
  }

  state->eflags = flags_value;
  state->executed_ops = steps;

  result.executed_ops = steps;
  result.final_rip = state->rip;
  if (result.reason == TerminationReason::kRunning) {
    result.reason = TerminationReason::kFallthrough;
  }
  if (result.status.ok() && result.reason == TerminationReason::kStepLimit) {
    result.status = StatusCode::kExecutionLimit;
  }
  return result;
}

// ---------------------------------------------------------------------------
// State comparison
// ---------------------------------------------------------------------------
namespace {

bool Cmp(const char* name, std::uint64_t a, std::uint64_t b,
         StateDifference* diff) {
  if (a == b) {
    return true;
  }
  if (diff != nullptr) {
    diff->field = name;
    diff->expected = a;
    diff->actual = b;
  }
  return false;
}

}  // namespace

bool CompareCpuState(const CpuState& expected, const CpuState& actual,
                     StateDifference* difference, std::uint64_t eflags_mask) {
  if (difference != nullptr) {
    difference->field = nullptr;
    difference->expected = 0;
    difference->actual = 0;
  }

  // Field by field, never memcmp. A memcmp would pass with a wrong EFLAGS,
  // because padding and unused bits are unspecified.
  static const char* const kGprNames[16] = {
      "rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
      "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15"};

  const std::uint64_t e[16] = {expected.rax, expected.rcx, expected.rdx,
                               expected.rbx, expected.rsp, expected.rbp,
                               expected.rsi, expected.rdi, expected.r8,
                               expected.r9,  expected.r10, expected.r11,
                               expected.r12, expected.r13, expected.r14,
                               expected.r15};
  const std::uint64_t a[16] = {actual.rax, actual.rcx, actual.rdx, actual.rbx,
                               actual.rsp, actual.rbp, actual.rsi, actual.rdi,
                               actual.r8,  actual.r9,  actual.r10, actual.r11,
                               actual.r12, actual.r13, actual.r14, actual.r15};

  for (int i = 0; i < 16; ++i) {
    if (!Cmp(kGprNames[i], e[i], a[i], difference)) {
      return false;
    }
  }
  if (!Cmp("rip", expected.rip, actual.rip, difference)) {
    return false;
  }
  if (!Cmp("eflags", expected.eflags & eflags_mask,
           actual.eflags & eflags_mask, difference)) {
    return false;
  }
  return true;
}

bool CompareMemoryWindow(memory::GuestMemoryPort& expected,
                         memory::GuestMemoryPort& actual,
                         memory::GuestAddr addr, std::size_t bytes,
                         StateDifference* difference) {
  if (difference != nullptr) {
    difference->field = nullptr;
  }
  for (std::size_t i = 0; i < bytes; ++i) {
    std::uint64_t e = 0;
    std::uint64_t a = 0;
    const Status se = expected.ReadData(addr + i, 1, &e);
    const Status sa = actual.ReadData(addr + i, 1, &a);
    if (!se.ok() || !sa.ok()) {
      if (difference != nullptr) {
        difference->field = "memory-read-failed";
        difference->expected = static_cast<std::uint64_t>(se.code());
        difference->actual = static_cast<std::uint64_t>(sa.code());
      }
      return false;
    }
    if (e != a) {
      if (difference != nullptr) {
        char name[64];
        std::snprintf(name, sizeof(name), "mem[0x%llx]",
                      static_cast<unsigned long long>(addr + i));
        difference->field = name;
        difference->expected = e;
        difference->actual = a;
      }
      return false;
    }
  }
  return true;
}

}  // namespace p5emu::cpu
