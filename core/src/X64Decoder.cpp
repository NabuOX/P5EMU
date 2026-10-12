// SPDX-License-Identifier: Apache-2.0
//
// P5EMU is an independent, open-source project. See LICENSE and
// PROVENANCE.md before adding code here.
//
// See X64Decoder.h for the scope, separation-of-concerns and encoding-source
// notes that govern this file.

#include "p5emu/cpu/X64Decoder.h"

#include <cstring>

namespace p5emu::x64 {
namespace {

// ---------------------------------------------------------------------------
// Bit-level readers
//
// Every field extraction goes through these, so there is exactly one place where
// an out-of-bounds read could happen and one place where the bounds contract is
// enforced. A reader that has run past the end returns false and the caller
// aborts with kDecodeError: a truncated instruction must never be decoded from
// whatever bytes happened to follow in the buffer.
// ---------------------------------------------------------------------------
class Cursor {
 public:
  Cursor(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}

  [[nodiscard]] bool Ok() const noexcept { return ok_; }
  [[nodiscard]] std::size_t Offset() const noexcept { return pos_; }
  [[nodiscard]] std::size_t Remaining() const noexcept {
    return ok_ ? (size_ - pos_) : 0u;
  }

  // Pushes back `n` bytes just read. Used when a byte turned out not to be a
  // prefix and is therefore the opcode. Only ever called immediately after a
  // successful read, so it cannot move before the start of the buffer.
  void Rewind(std::size_t n) noexcept {
    if (n <= pos_) {
      pos_ -= n;
    } else {
      ok_ = false;
    }
  }

  std::uint8_t U8() {
    if (!Require(1)) {
      return 0;
    }
    return data_[pos_++];
  }

  std::uint16_t U16() {
    if (!Require(2)) {
      return 0;
    }
    const std::uint16_t v = static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(data_[pos_]) |
        (static_cast<std::uint16_t>(data_[pos_ + 1]) << 8));
    pos_ += 2;
    return v;
  }

  std::uint32_t U32() {
    if (!Require(4)) {
      return 0;
    }
    std::uint32_t v = 0;
    for (int i = 3; i >= 0; --i) {
      v = (v << 8) | data_[pos_ + static_cast<std::size_t>(i)];
    }
    pos_ += 4;
    return v;
  }

  std::uint64_t U64() {
    if (!Require(8)) {
      return 0;
    }
    std::uint64_t v = 0;
    for (int i = 7; i >= 0; --i) {
      v = (v << 8) | data_[pos_ + static_cast<std::size_t>(i)];
    }
    pos_ += 8;
    return v;
  }

  // Little-endian immediate of `n` bytes, zero-extended.
  std::uint64_t Uint(std::size_t n) {
    std::uint64_t v = 0;
    if (!Require(n)) {
      return 0;
    }
    for (std::size_t i = 0; i < n; ++i) {
      v |= static_cast<std::uint64_t>(data_[pos_ + i]) << (8u * i);
    }
    pos_ += n;
    return v;
  }

 private:
  bool Require(std::size_t n) {
    if (!ok_ || (size_ - pos_) < n) {
      ok_ = false;
      return false;
    }
    return true;
  }

  const std::uint8_t* data_;
  std::size_t size_;
  std::size_t pos_ = 0;
  bool ok_ = true;
};

std::int64_t SignExtend(std::uint64_t value, std::uint32_t bits) {
  if (bits == 0 || bits >= 64) {
    return static_cast<std::int64_t>(value);
  }
  const std::uint64_t mask = (std::uint64_t{1} << bits) - 1u;
  const std::uint64_t sign = std::uint64_t{1} << (bits - 1u);
  value &= mask;
  return static_cast<std::int64_t>((value ^ sign) - sign);
}

// ---------------------------------------------------------------------------
// Prefixes
// Intel SDM Vol. 2 §2.2.3.2 "REX Prefixes", §2.2.3.3 "Legacy Prefixes".
// ---------------------------------------------------------------------------
struct Prefixes {
  bool rex_w = false;    // REX.W: 64-bit operand size
  bool rex_r = false;    // extends ModRM.reg
  bool rex_x = false;    // extends SIB.index
  bool rex_b = false;    // extends ModRM.rm / SIB.base
  bool operand_override = false;   // 0x66
  bool address_override = false;   // 0x67
  bool repne = false;    // 0xF2
  bool rep = false;      // 0xF3
  bool lock = false;     // 0xF0 -- decoded and reported, not honoured
  std::uint8_t rex = 0;
  bool has_rex = false;

  [[nodiscard]] std::uint8_t OperandSizeBytes() const noexcept {
    if (rex_w) {
      return 8;
    }
    return operand_override ? 2 : 4;
  }
};

}  // namespace

const char* Reg8Name(std::uint8_t reg) noexcept {
  static const char* const kNames[16] = {
      "al", "cl", "dl", "bl", "spl", "bpl", "sil", "dil",
      "r8b", "r9b", "r10b", "r11b", "r12b", "r13b", "r14b", "r15b"};
  return (reg < 16) ? kNames[reg] : "<bad-reg8>";
}

const char* Reg64Name(std::uint8_t reg) noexcept {
  static const char* const kNames[16] = {
      "rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
      "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"};
  return (reg < 16) ? kNames[reg] : "<bad-reg64>";
}

const char* CondName(Cond c) noexcept {
  static const char* const kNames[16] = {
      "o", "no", "b", "ae", "e", "ne", "be", "a",
      "s", "ns", "p", "np", "l", "ge", "le", "g"};
  const std::uint8_t i = static_cast<std::uint8_t>(c);
  return (i < 16) ? kNames[i] : "<bad-cond>";
}

const char* OpClassName(OpClass cls) noexcept {
  switch (cls) {
    case OpClass::kNone:   return "none";
    case OpClass::kReg8:   return "reg8";
    case OpClass::kReg64:  return "reg64";
    case OpClass::kMem8:   return "mem8";
    case OpClass::kMem32:  return "mem32";
    case OpClass::kMem64:  return "mem64";
    case OpClass::kXmm:    return "xmm";
    case OpClass::kMem128: return "mem128";
  }
  return "<bad-opclass>";
}

const char* MnemonicName(Mnemonic m) noexcept {
  switch (m) {
    case Mnemonic::kUnimplemented: return "<unimplemented>";
    case Mnemonic::kMov:    return "mov";
    case Mnemonic::kMovzx:  return "movzx";
    case Mnemonic::kMovsx:  return "movsx";
    case Mnemonic::kLea:    return "lea";
    case Mnemonic::kAdd:    return "add";
    case Mnemonic::kSub:    return "sub";
    case Mnemonic::kAnd:    return "and";
    case Mnemonic::kOr:     return "or";
    case Mnemonic::kXor:    return "xor";
    case Mnemonic::kCmp:    return "cmp";
    case Mnemonic::kTest:   return "test";
    case Mnemonic::kShl:    return "shl";
    case Mnemonic::kShr:    return "shr";
    case Mnemonic::kSar:    return "sar";
    case Mnemonic::kJmp:    return "jmp";
    case Mnemonic::kJcc:    return "jcc";
    case Mnemonic::kCall:   return "call";
    case Mnemonic::kRet:    return "ret";
    case Mnemonic::kNop:    return "nop";
    case Mnemonic::kInt3:   return "int3";
    case Mnemonic::kPush:   return "push";
    case Mnemonic::kPop:    return "pop";
    case Mnemonic::kLeave:  return "leave";
    case Mnemonic::kAdc:    return "adc";
    case Mnemonic::kSbb:    return "sbb";
    case Mnemonic::kMul:    return "mul";
    case Mnemonic::kIMul:   return "imul";
    case Mnemonic::kDiv:    return "div";
    case Mnemonic::kIDiv:   return "idiv";
    case Mnemonic::kMovd:   return "movd";
    case Mnemonic::kMovq:   return "movq";
    case Mnemonic::kMovdqa: return "movdqa";
    case Mnemonic::kMovdqu: return "movdqu";
    case Mnemonic::kPxor:   return "pxor";
    case Mnemonic::kPand:   return "pand";
    case Mnemonic::kPor:    return "por";
    case Mnemonic::kPaddq:  return "paddq";
  }
  return "<bad-mnemonic>";
}

const char* FormName(Form f) noexcept {
  switch (f) {
    case Form::kUnimplemented: return "<unimplemented>";
    case Form::kRR64RegDst: return "rr64 reg<-rm";
    case Form::kRR64RmDst:  return "rr64 rm<-reg";
    case Form::kR32RegDst:  return "rr32 reg<-rm";
    case Form::kR32RmDst:   return "rr32 rm<-reg";
    case Form::kR8RegDst:   return "rr8 reg<-rm";
    case Form::kR8RmDst:    return "rr8 rm<-reg";
    case Form::kRM64:   return "rm64";
    case Form::kMR64:   return "mr64";
    case Form::kRI64:   return "ri64";
    case Form::kR32RM32:return "r32/rm32";
    case Form::kM32R32: return "m32/r32";
    case Form::kR32RI32:return "r32/imm32";
    case Form::kRR8RegDst: return "rr8 reg<-rm";
    case Form::kRR8RmDst:  return "rr8 rm<-reg";
    case Form::kRM8:    return "rm8";
    case Form::kMR8:    return "m8/r8";
    case Form::kRI8:    return "r8/imm8";
    case Form::kMoffs:  return "moffs";
    case Form::kLea:    return "lea";
    case Form::kMovzxRM:  return "movzx r32, r/m8";
    case Form::kMovzxRM16:return "movzx r32, r/m16";
    case Form::kMovsxRM:  return "movsx r64, r/m8";
    case Form::kMovsxRM16:return "movsx r64, r/m16";
    case Form::kMovsxRM32:return "movsx r64, r/m32";
    case Form::kShiftCL: return "shift cl";
    case Form::kShiftImm:return "shift imm8";
    case Form::kJmpRel:  return "jmp rel";
    case Form::kJccRel:  return "jcc rel";
    case Form::kCallRel: return "call rel";
    case Form::kRetn:    return "ret";
    case Form::kNop:     return "nop";
    case Form::kInt3:    return "int3";
    case Form::kPushR:   return "push r64";
    case Form::kPushImm: return "push imm";
    case Form::kPopR:    return "pop r64";
    case Form::kLeave:   return "leave";
    case Form::kMulRm:   return "mul r/m";
    case Form::kIMulRm:  return "imul r/m";
    case Form::kDivRm:   return "div r/m";
    case Form::kIDivRm:  return "idiv r/m";
    case Form::kIMulRR:  return "imul reg, r/m";
    case Form::kIMulRRI: return "imul reg, r/m, imm";
    case Form::kMovdToXmm:    return "movd/movq xmm, r/m";
    case Form::kMovdFromXmm:  return "movd/movq r/m, xmm";
    case Form::kMovdqaToXmm:  return "movdqa/movdqu xmm, xmm/m128";
    case Form::kMovdqaFromXmm:return "movdqa/movdqu xmm/m128, xmm";
    case Form::kPackedXmm:    return "packed xmm, xmm/m128";
  }
  return "<bad-form>";
}

bool X64Decoder::IsSupported(Mnemonic mnemonic) noexcept {
  switch (mnemonic) {
    case Mnemonic::kMov:
    case Mnemonic::kMovzx:
    case Mnemonic::kMovsx:
    case Mnemonic::kLea:
    case Mnemonic::kAdd:
    case Mnemonic::kSub:
    case Mnemonic::kAnd:
    case Mnemonic::kOr:
    case Mnemonic::kXor:
    case Mnemonic::kCmp:
    case Mnemonic::kTest:
    case Mnemonic::kShl:
    case Mnemonic::kShr:
    case Mnemonic::kSar:
    case Mnemonic::kJmp:
    case Mnemonic::kJcc:
    case Mnemonic::kCall:
    case Mnemonic::kRet:
    case Mnemonic::kNop:
    case Mnemonic::kInt3:
    case Mnemonic::kPush:
    case Mnemonic::kPop:
    case Mnemonic::kLeave:
    case Mnemonic::kAdc:
    case Mnemonic::kSbb:
    case Mnemonic::kMul:
    case Mnemonic::kIMul:
    case Mnemonic::kDiv:
    case Mnemonic::kIDiv:
    case Mnemonic::kMovd:
    case Mnemonic::kMovq:
    case Mnemonic::kMovdqa:
    case Mnemonic::kMovdqu:
    case Mnemonic::kPxor:
    case Mnemonic::kPand:
    case Mnemonic::kPor:
    case Mnemonic::kPaddq:
      return true;
    case Mnemonic::kUnimplemented:
      break;
  }
  return false;
}

std::size_t X64Decoder::SupportedCount() noexcept {
  // Number of distinct supported mnemonics. Kept as an explicit count rather
  // than derived, so the coverage number in reports is a checked constant and a
  // new instruction forces a deliberate decision to bump it.
  return 37;
}

namespace {

// Reads legacy prefixes and a REX prefix, if present.
// Intel SDM Vol. 2 §2.2.3: REX must be the LAST prefix before the opcode.
Status ReadPrefixes(Cursor* cur, Prefixes* out) {
  // Bounded: the architectural maximum prefix chain is short, and an unbounded
  // loop over 0x66/0xF0 bytes would decode a stream of prefixes as one
  // "instruction". Phase 1 caps at 15, matching the practical limit.
  int guard = 0;
  while (cur->Ok()) {
    const std::uint8_t b = cur->U8();
    if (!cur->Ok()) {
      return StatusCode::kDecodeError;
    }
    if (++guard > 15) {
      return StatusCode::kDecodeError;
    }

    switch (b) {
      case 0x66: out->operand_override = true; continue;
      case 0x67: out->address_override = true; continue;
      case 0xF0: out->lock = true; continue;
      case 0xF2: out->repne = true; continue;
      case 0xF3: out->rep = true; continue;
      default: break;
    }
    // REX: 0x40..0x4F. In 64-bit mode these are ALWAYS prefixes, never
    // INC/DEC, which is a 64-bit-mode change from 32-bit mode.
    if (b >= 0x40 && b <= 0x4f) {
      out->rex = b;
      out->has_rex = true;
      out->rex_w = (b & 0x08u) != 0;
      out->rex_r = (b & 0x04u) != 0;
      out->rex_x = (b & 0x02u) != 0;
      out->rex_b = (b & 0x01u) != 0;
      return StatusCode::kOk;
    }
    // Not a prefix: it is the opcode. Put it back logically by rewinding.
    cur->Rewind(1);
    return StatusCode::kOk;
  }
  return StatusCode::kDecodeError;
}

// ModRM / SIB decoding.
// Intel SDM Vol. 2 §2.2.2.7 "ModR/M Encoding", §2.2.2.8 "SIB Encoding".
struct ModRm {
  std::uint8_t mod = 0;
  std::uint8_t reg = 0;
  std::uint8_t rm = 0;

  bool is_memory = false;
  bool rip_relative = false;
  bool has_base = false;
  std::uint8_t base = 0;
  bool has_index = false;
  std::uint8_t index = 0;
  std::uint8_t scale = 1;
  std::int64_t displacement = 0;
};

Status ReadModRm(Cursor* cur, const Prefixes& p, ModRm* out) {
  const std::uint8_t byte = cur->U8();
  if (!cur->Ok()) {
    return StatusCode::kDecodeError;
  }
  out->mod = static_cast<std::uint8_t>((byte >> 6) & 0x03u);
  out->reg = static_cast<std::uint8_t>(((byte >> 3) & 0x07u) |
                                       (p.rex_r ? 0x08u : 0x00u));
  const std::uint8_t rm = static_cast<std::uint8_t>(
      (byte & 0x07u) | (p.rex_b ? 0x08u : 0x00u));

  // mod == 3: register form, rm IS the register.
  if (out->mod == 3) {
    out->rm = rm;
    out->is_memory = false;
    return StatusCode::kOk;
  }

  out->is_memory = true;

  // 64-bit mode, mod == 0, rm == 5: RIP-relative, disp32.
  // Intel SDM Vol. 2 §2.2.2.7. Note this OVERRIDES the 32-bit mod==0/rm==5
  // absolute form; the absolute form only exists with a 0x67 prefix.
  if (out->mod == 0 && (rm & 0x07u) == 0x05u && !p.address_override) {
    out->rip_relative = true;
    out->displacement = static_cast<std::int32_t>(cur->U32());
    if (!cur->Ok()) {
      return StatusCode::kDecodeError;
    }
    return StatusCode::kOk;
  }

  // SIB: rm == 4 selects a SIB byte. rm == 4 with mod == 3 is r12, not a SIB.
  if ((rm & 0x07u) == 0x04u) {
    const std::uint8_t sib = cur->U8();
    if (!cur->Ok()) {
      return StatusCode::kDecodeError;
    }
    const std::uint8_t scale_code = static_cast<std::uint8_t>((sib >> 6) & 0x03u);
    const std::uint8_t index_raw = static_cast<std::uint8_t>(
        ((sib >> 3) & 0x07u) | (p.rex_x ? 0x08u : 0x00u));
    const std::uint8_t base_raw =
        static_cast<std::uint8_t>((sib & 0x07u) | (p.rex_b ? 0x08u : 0x00u));

    out->scale = static_cast<std::uint8_t>(1u << scale_code);

    // index == 4 (rsp, with no REX.X) means "no index", not "rsp as index":
    // rsp cannot be an index register. Intel SDM Vol. 2 §2.2.2.8.
    if ((sib & 0x07u) != 0x04u || p.rex_x) {
      out->has_index = true;
      out->index = index_raw;
    }

    // base == 5 with mod == 0 means disp32 with NO base register.
    if (out->mod == 0 && (base_raw & 0x07u) == 0x05u && !p.address_override) {
      out->has_base = false;
      out->displacement = static_cast<std::int32_t>(cur->U32());
      if (!cur->Ok()) {
        return StatusCode::kDecodeError;
      }
      return StatusCode::kOk;
    }

    out->has_base = true;
    out->base = base_raw;

    // disp8 or disp32 by mod.
    if (out->mod == 1) {
      out->displacement = static_cast<std::int8_t>(cur->U8());
    } else if (out->mod == 2) {
      out->displacement = static_cast<std::int32_t>(cur->U32());
    }
    if (!cur->Ok()) {
      return StatusCode::kDecodeError;
    }
    return StatusCode::kOk;
  }

  // Plain base + disp.
  out->has_base = true;
  out->base = rm;
  if (out->mod == 1) {
    out->displacement = static_cast<std::int8_t>(cur->U8());
  } else if (out->mod == 2) {
    out->displacement = static_cast<std::int32_t>(cur->U32());
  } else if (out->mod == 0 && (rm & 0x07u) == 0x05u) {
    // Only reachable with a 0x67 address-size override, i.e. 32-bit addressing.
    // Phase 1 does not implement 32-bit addressing, so this is rejected rather
    // than silently decoded as a 64-bit absolute address.
    out->displacement = static_cast<std::int32_t>(cur->U32());
    if (!cur->Ok()) {
      return StatusCode::kDecodeError;
    }
    return StatusCode::kUnimplementedInstruction;
  }
  if (!cur->Ok()) {
    return StatusCode::kDecodeError;
  }
  return StatusCode::kOk;
}

void FillMemOperand(Operand* operand, const ModRm& modrm, OpClass cls) {
  operand->cls = cls;
  operand->has_base = modrm.has_base;
  operand->base = modrm.base;
  operand->has_index = modrm.has_index;
  operand->index = modrm.index;
  operand->scale = modrm.scale;
  operand->displacement = modrm.displacement;
  operand->rip_relative = modrm.rip_relative;
}

}  // namespace

// ---------------------------------------------------------------------------
// DecodeOne
// ---------------------------------------------------------------------------
Status X64Decoder::DecodeOne(memory::GuestAddr address, const std::uint8_t* code,
                             std::size_t available, Instruction* out) const {
  if (out == nullptr || (code == nullptr && available != 0)) {
    return StatusCode::kInvalidArgument;
  }
  if (code == nullptr || available == 0) {
    return StatusCode::kDecodeError;
  }

  Cursor cur(code, available);
  Prefixes prefixes;
  const Status prefix_status = ReadPrefixes(&cur, &prefixes);
  if (!prefix_status.ok()) {
    return prefix_status;
  }

  const std::uint8_t opcode = cur.U8();
  if (!cur.Ok()) {
    return StatusCode::kDecodeError;
  }

  Instruction insn;
  insn.address = address;
  insn.rex = prefixes.rex;
  insn.has_rex = prefixes.has_rex;
  insn.address_size = 8;

  // ---- one-byte opcode map ------------------------------------------------
  // ALU r/m, r   and   r, r/m    (Intel SDM Vol. 2 §2.2.1, Table 2-3)
  //   0x00 ADD   0x08 OR    0x10 ADC(unimpl)  0x18 SBB(unimpl)
  //   0x20 AND   0x28 SUB   0x30 XOR(unimpl)  0x38 CMP
  //   0x01 ADD   0x09 OR    0x11 ADC(unimpl)  0x19 SBB(unimpl)
  //   0x21 AND   0x29 SUB   0x31 XOR(unimpl)  0x39 CMP
  //   0x02 ADD   0x0A OR    0x12 ADC(unimpl)  0x1A SBB(unimpl)
  //   0x22 AND   0x2A SUB   0x32 XOR(unimpl)  0x3A CMP
  //
  // Phase 1 supports the non-flag-setting direction only: `opcode r/m, r` sets
  // flags, so those are 0x00/0x08/0x20/0x28/0x30/0x38 (+0x01/0x09/0x21/0x29/
  // 0x31/0x39). The "r, r/m" direction also sets flags, and is supported.
  // ADC/SBB are excluded: they consume CF as an input, which is a flag-
  // dependency feature Phase 1 does not model in the IR.
  struct AluEntry {
    std::uint8_t op_rm_reg;  // r/m, r
    std::uint8_t op_reg_rm;  // r, r/m
    std::uint8_t op_rm_imm;  // r/m, imm
    std::uint8_t op_acc_imm; // AL/eAX, imm
    Mnemonic mnemonic;
  };
  static const AluEntry kAlu[] = {
      {0x00, 0x01, 0x80, 0x04, Mnemonic::kAdd},
      {0x08, 0x09, 0x80, 0x0c, Mnemonic::kOr},
      {0x20, 0x21, 0x80, 0x24, Mnemonic::kAnd},
      {0x28, 0x29, 0x80, 0x2c, Mnemonic::kSub},
      {0x30, 0x31, 0x80, 0x34, Mnemonic::kXor},
      {0x38, 0x39, 0x80, 0x3c, Mnemonic::kCmp},
  };
  (void)kAlu;

  // Working operand-size decision, shared by the maps below.
  const std::uint8_t opsize = prefixes.OperandSizeBytes();
  (void)opsize;  // each opcode group computes its own width from REX.W/0x66

  // --- 0x88 / 0x89 / 0x8A / 0x8B : MOV ---------------------------------------
  // Intel SDM Vol. 2 §2.2.1, Table 2-3 "Mnemonic and Opcode Listing".
  if (opcode == 0x88 || opcode == 0x89 || opcode == 0x8a || opcode == 0x8b) {
    ModRm modrm;
    const Status st = ReadModRm(&cur, prefixes, &modrm);
    if (!st.ok()) {
      return st;
    }

    const bool byte_form = (opcode == 0x88 || opcode == 0x8a);
    // 8A / 8B are the "register is the DESTINATION" forms (r <- r/m).
    // 88 / 89 are the "r/m is the DESTINATION" forms (r/m <- r).
    // Getting this backwards swaps the operands of every register-to-register
    // MOV, so it is decided here from the opcode and nowhere else.
    const bool to_reg = (opcode == 0x8a || opcode == 0x8b);

    // Operand size: byte form is 8-bit. Otherwise REX.W selects 64; otherwise
    // the 0x66 prefix selects 16, which Phase 1 does not implement, and the
    // default is 32. A 32-bit destination write zero-extends to 64.
    if (byte_form) {
      insn.mnemonic = Mnemonic::kMov;
      insn.op_size = 1;
      if (to_reg) {
        // 8A /r: r8 <- r/m8
        insn.form = modrm.is_memory ? Form::kRM8 : Form::kRR8RegDst;
        insn.dst.cls = OpClass::kReg8;
        insn.dst.reg = modrm.reg;
      } else {
        // 88 /r: r/m8 <- r8
        insn.form = modrm.is_memory ? Form::kMR8 : Form::kRR8RmDst;
        if (modrm.is_memory) {
          FillMemOperand(&insn.dst, modrm, OpClass::kMem8);
        } else {
          insn.dst.cls = OpClass::kReg8;
          insn.dst.reg = modrm.rm;
        }
      }
      if (to_reg && modrm.is_memory) {
        FillMemOperand(&insn.src, modrm, OpClass::kMem8);
      }
      if (!to_reg && !modrm.is_memory) {
        // register-register, destination is r/m
        insn.src.cls = OpClass::kReg8;
        insn.src.reg = modrm.reg;
      } else if (!to_reg) {
        insn.src.cls = OpClass::kReg8;
        insn.src.reg = modrm.reg;
      }
      goto finalise;
    }

    if (prefixes.operand_override) {
      // 16-bit MOV. Uses the 64-bit register container; op_size carries the
      // real width and the translator uses it.
      insn.mnemonic = Mnemonic::kMov;
      insn.op_size = 2;
      if (to_reg) {
        insn.form = modrm.is_memory ? Form::kR32RM32 : Form::kR32RegDst;
        insn.dst.cls = OpClass::kReg64;
        insn.dst.reg = modrm.reg;
        if (modrm.is_memory) {
          FillMemOperand(&insn.src, modrm, OpClass::kMem32);
        } else {
          insn.src.cls = OpClass::kReg64;
          insn.src.reg = modrm.rm;
        }
      } else {
        insn.form = modrm.is_memory ? Form::kM32R32 : Form::kR32RmDst;
        if (modrm.is_memory) {
          FillMemOperand(&insn.dst, modrm, OpClass::kMem32);
        } else {
          insn.dst.cls = OpClass::kReg64;
          insn.dst.reg = modrm.rm;
        }
        insn.src.cls = OpClass::kReg64;
        insn.src.reg = modrm.reg;
      }
      goto finalise;
    }

    if (prefixes.rex_w) {
      insn.mnemonic = Mnemonic::kMov;
      insn.op_size = 8;
      if (to_reg) {
        insn.form = modrm.is_memory ? Form::kRM64 : Form::kRR64RegDst;
        insn.dst.cls = OpClass::kReg64;
        insn.dst.reg = modrm.reg;
        if (modrm.is_memory) {
          FillMemOperand(&insn.src, modrm, OpClass::kMem64);
        } else {
          insn.src.cls = OpClass::kReg64;
          insn.src.reg = modrm.rm;
        }
      } else {
        // 89 /r: r/m64 <- r64. Destination is ModRM.rm.
        insn.form = modrm.is_memory ? Form::kMR64 : Form::kRR64RmDst;
        if (modrm.is_memory) {
          FillMemOperand(&insn.dst, modrm, OpClass::kMem64);
        } else {
          insn.dst.cls = OpClass::kReg64;
          insn.dst.reg = modrm.rm;
        }
        insn.src.cls = OpClass::kReg64;
        insn.src.reg = modrm.reg;
      }
      goto finalise;
    }

    // 32-bit form. A 32-bit register destination zero-extends to 64.
    insn.mnemonic = Mnemonic::kMov;
    insn.op_size = 4;
    if (to_reg) {
      // 8B /r: r32 <- r/m32. Destination is ModRM.reg.
      insn.form = modrm.is_memory ? Form::kR32RM32 : Form::kR32RegDst;
      insn.dst.cls = OpClass::kReg64;  // the ENCODED register, width comes from op_size
      insn.dst.reg = modrm.reg;
      if (modrm.is_memory) {
        FillMemOperand(&insn.src, modrm, OpClass::kMem32);
      } else {
        insn.src.cls = OpClass::kReg64;
        insn.src.reg = modrm.rm;
      }
    } else {
      // 89 /r without REX.W: m32 <- r32. Destination is ModRM.rm.
      insn.form = modrm.is_memory ? Form::kM32R32 : Form::kR32RmDst;
      if (modrm.is_memory) {
        FillMemOperand(&insn.dst, modrm, OpClass::kMem32);
      } else {
        insn.dst.cls = OpClass::kReg64;
        insn.dst.reg = modrm.rm;
      }
      insn.src.cls = OpClass::kReg64;
      insn.src.reg = modrm.reg;
    }
    goto finalise;
  }

  // --- 0xB0..0xB7 : MOV r8, imm8 --------------------------------------------
  // Intel SDM Vol. 2 §2.2.1.1. The 8-bit immediate-to-register form.
  if (opcode >= 0xb0 && opcode <= 0xb7) {
    insn.mnemonic = Mnemonic::kMov;
    insn.form = Form::kRI8;
    insn.op_size = 1;
    insn.dst.cls = OpClass::kReg8;
    insn.dst.reg = static_cast<std::uint8_t>((opcode - 0xb0u) |
                                             (prefixes.rex_b ? 0x08u : 0x00u));
    insn.imm = cur.Uint(1);
    insn.imm_is_signed_extension = false;
    if (!cur.Ok()) {
      return StatusCode::kDecodeError;
    }
    goto finalise;
  }

  // --- 0xB8..0xBF : MOV r, imm ----------------------------------------------
  // Intel SDM Vol. 2 §2.2.1.1. With REX.W the immediate is 64 bits, and the
  // encoding is exactly 10 bytes, which makes it the natural Phase-1 test for
  // 64-bit immediates. A 0x66 prefix selects the 16-bit form.
  if (opcode >= 0xb8 && opcode <= 0xbf) {
    const std::uint8_t reg = static_cast<std::uint8_t>(
        (opcode - 0xb8u) | (prefixes.rex_b ? 0x08u : 0x00u));
    if (prefixes.operand_override) {
      insn.mnemonic = Mnemonic::kMov;
      insn.form = Form::kR32RI32;  // 32-bit container; op_size carries the truth
      insn.op_size = 2;
      insn.dst.cls = OpClass::kReg64;
      insn.dst.reg = reg;
      insn.imm = cur.Uint(2);
      insn.imm_is_signed_extension = false;
      if (!cur.Ok()) {
        return StatusCode::kDecodeError;
      }
      goto finalise;
    }
    if (prefixes.rex_w) {
      insn.mnemonic = Mnemonic::kMov;
      insn.form = Form::kRI64;
      insn.op_size = 8;
      insn.dst.cls = OpClass::kReg64;
      insn.dst.reg = reg;
      insn.imm = cur.U64();
      insn.imm_is_signed_extension = false;
      if (!cur.Ok()) {
        return StatusCode::kDecodeError;
      }
      goto finalise;
    }
    insn.mnemonic = Mnemonic::kMov;
    insn.form = Form::kR32RI32;
    insn.op_size = 4;
    insn.dst.cls = OpClass::kReg64;
    insn.dst.reg = reg;
    insn.imm = cur.U32();
    insn.imm_is_signed_extension = false;
    if (!cur.Ok()) {
      return StatusCode::kDecodeError;
    }
    goto finalise;
  }

  // --- 0xC6 / 0xC7 : MOV r/m, imm -------------------------------------------
  // Intel SDM Vol. 2 §2.2.1.1.
  if (opcode == 0xc6 || opcode == 0xc7) {
    ModRm modrm;
    const Status st = ReadModRm(&cur, prefixes, &modrm);
    if (!st.ok()) {
      return st;
    }
    if (opcode == 0xc6) {
      insn.mnemonic = Mnemonic::kMov;
      insn.op_size = 1;
      if (modrm.reg != 0) {
        // /0 is the only defined form for C6; a nonzero reg field is a
        // different, unimplemented instruction group.
        return StatusCode::kUnimplementedInstruction;
      }
      if (modrm.is_memory) {
        insn.form = Form::kMR8;
        FillMemOperand(&insn.dst, modrm, OpClass::kMem8);
      } else {
        insn.form = Form::kRR8RegDst;
        insn.dst.cls = OpClass::kReg8;
        insn.dst.reg = modrm.rm;
      }
      insn.imm = cur.Uint(1);
      if (!cur.Ok()) {
        return StatusCode::kDecodeError;
      }
      goto finalise;
    }
    if (prefixes.operand_override) {
      // 16-bit destination: imm16, container class only.
      insn.mnemonic = Mnemonic::kMov;
      insn.form = modrm.is_memory ? Form::kM32R32 : Form::kR32RmDst;
      insn.op_size = 2;
      if (modrm.is_memory) {
        FillMemOperand(&insn.dst, modrm, OpClass::kMem32);
      } else {
        insn.dst.cls = OpClass::kReg64;
        insn.dst.reg = modrm.rm;
      }
      insn.imm = cur.Uint(2);
      insn.imm_is_signed_extension = false;
      if (!cur.Ok()) {
        return StatusCode::kDecodeError;
      }
      goto finalise;
    }
    insn.mnemonic = Mnemonic::kMov;
    if (modrm.reg != 0) {
      // /0 is the only defined form for C7 (MOV r/m, imm).
      return StatusCode::kUnimplementedInstruction;
    }
    if (prefixes.rex_w) {
      insn.form = modrm.is_memory ? Form::kMR64 : Form::kRR64RmDst;
      insn.op_size = 8;
      if (modrm.is_memory) {
        FillMemOperand(&insn.dst, modrm, OpClass::kMem64);
      } else {
        insn.dst.cls = OpClass::kReg64;
        insn.dst.reg = modrm.rm;
      }
      insn.imm = static_cast<std::uint64_t>(
          SignExtend(cur.Uint(4), 32));  // imm32 sign-extended to 64
      insn.imm_is_signed_extension = true;
    } else {
      insn.form = modrm.is_memory ? Form::kM32R32 : Form::kR32RmDst;
      insn.op_size = 4;
      if (modrm.is_memory) {
        FillMemOperand(&insn.dst, modrm, OpClass::kMem32);
      } else {
        insn.dst.cls = OpClass::kReg64;
        insn.dst.reg = modrm.rm;
      }
      insn.imm = cur.Uint(4);
      insn.imm_is_signed_extension = false;
    }
    if (!cur.Ok()) {
      return StatusCode::kDecodeError;
    }
    goto finalise;
  }

  // --- ALU group: 0x00..0x3B, plus 0x80 / 0x81 / 0x83 -----------------------
  {
    struct AluForm {
      std::uint8_t opcode;
      Mnemonic mnemonic;
      std::uint8_t reg_is_destination;  // true: r <- r/m. false: r/m <- r
      std::uint8_t byte_form;           // true: 8-bit operand (op_size 1)
    };
    static const AluForm kAluMap[] = {
        // --- byte forms (op_size 1) ---
        //   00 ADD r/m8, r8   08 OR   20 AND   28 SUB   30 XOR   38 CMP
        {0x00, Mnemonic::kAdd, false, true}, {0x08, Mnemonic::kOr, false, true},
        {0x20, Mnemonic::kAnd, false, true}, {0x28, Mnemonic::kSub, false, true},
        {0x30, Mnemonic::kXor, false, true}, {0x38, Mnemonic::kCmp, false, true},
        //   02 ADD r8, r/m8   0A OR   22 AND   2A SUB   32 XOR   3A CMP
        {0x02, Mnemonic::kAdd, true, true},  {0x0a, Mnemonic::kOr, true, true},
        {0x22, Mnemonic::kAnd, true, true},  {0x2a, Mnemonic::kSub, true, true},
        {0x32, Mnemonic::kXor, true, true},  {0x3a, Mnemonic::kCmp, true, true},
        // --- word forms: r/m <- r (op_size from REX.W / 0x66) ---
        {0x01, Mnemonic::kAdd, false, false}, {0x09, Mnemonic::kOr, false, false},
        {0x21, Mnemonic::kAnd, false, false}, {0x29, Mnemonic::kSub, false, false},
        {0x31, Mnemonic::kXor, false, false}, {0x39, Mnemonic::kCmp, false, false},
        // --- word forms: r <- r/m ---
        {0x03, Mnemonic::kAdd, true, false},  {0x0b, Mnemonic::kOr, true, false},
        {0x23, Mnemonic::kAnd, true, false},  {0x2b, Mnemonic::kSub, true, false},
        {0x33, Mnemonic::kXor, true, false},  {0x3b, Mnemonic::kCmp, true, false},
        // --- ADC / SBB (Phase 11) ---
        {0x10, Mnemonic::kAdc, false, true}, {0x12, Mnemonic::kAdc, true, true},
        {0x18, Mnemonic::kSbb, false, true}, {0x1a, Mnemonic::kSbb, true, true},
        {0x11, Mnemonic::kAdc, false, false}, {0x13, Mnemonic::kAdc, true, false},
        {0x19, Mnemonic::kSbb, false, false}, {0x1b, Mnemonic::kSbb, true, false},
    };

    const AluForm* entry = nullptr;
    for (const AluForm& e : kAluMap) {
      if (e.opcode == opcode) {
        entry = &e;
        break;
      }
    }

    if (entry != nullptr) {
      ModRm modrm;
      const Status st = ReadModRm(&cur, prefixes, &modrm);
      if (!st.ok()) {
        return st;
      }

      insn.mnemonic = entry->mnemonic;

      // Real width selection. The byte forms are 8-bit regardless of prefixes
      // (an earlier version decoded them as 32-bit, so `add al, cl` silently
      // operated on EAX). The word forms are 16 with 0x66, 64 with REX.W, else
      // 32. A 16-bit operand uses the 64-bit register container; op_size carries
      // the true width and the translator uses it.
      std::uint8_t width = 0;
      OpClass reg_cls = OpClass::kReg64;
      OpClass mem_cls = OpClass::kMem32;
      Form reg_reg_dst = Form::kR32RegDst;
      Form rm_dst = Form::kR32RmDst;
      Form reg_mem = Form::kR32RM32;
      Form mem_reg = Form::kM32R32;

      if (entry->byte_form != 0) {
        width = 1;
        reg_cls = OpClass::kReg8;
        mem_cls = OpClass::kMem8;
        reg_reg_dst = Form::kRR8RegDst;
        rm_dst = Form::kRR8RmDst;
        reg_mem = Form::kRM8;
        mem_reg = Form::kMR8;
      } else if (prefixes.operand_override) {
        width = 2;
      } else {
        width = prefixes.rex_w ? 8u : 4u;
        mem_cls = (width == 8) ? OpClass::kMem64 : OpClass::kMem32;
        reg_reg_dst = (width == 8) ? Form::kRR64RegDst : Form::kR32RegDst;
        rm_dst = (width == 8) ? Form::kRR64RmDst : Form::kR32RmDst;
        reg_mem = (width == 8) ? Form::kRM64 : Form::kR32RM32;
        mem_reg = (width == 8) ? Form::kMR64 : Form::kM32R32;
      }
      insn.op_size = width;

      if (entry->reg_is_destination) {
        // r <- r/m
        insn.form = modrm.is_memory ? reg_mem : reg_reg_dst;
        insn.dst.cls = reg_cls;
        insn.dst.reg = modrm.reg;
        if (modrm.is_memory) {
          FillMemOperand(&insn.src, modrm, mem_cls);
        } else {
          insn.src.cls = reg_cls;
          insn.src.reg = modrm.rm;
        }
      } else {
        // r/m <- r
        insn.form = modrm.is_memory ? mem_reg : rm_dst;
        if (modrm.is_memory) {
          FillMemOperand(&insn.dst, modrm, mem_cls);
          insn.src.cls = reg_cls;
          insn.src.reg = modrm.reg;
        } else {
          insn.dst.cls = reg_cls;
          insn.dst.reg = modrm.rm;
          insn.src.cls = reg_cls;
          insn.src.reg = modrm.reg;
        }
      }
      goto finalise;
    }
  }

  // --- 0x80 / 0x81 / 0x83 : ALU r/m, imm ------------------------------------
  // Intel SDM Vol. 2 §2.2.1.1 "Immediate Encodings".
  //   0x80 imm8
  //   0x81 imm16/imm32 (imm16 needs a 0x66 prefix)
  //   0x83 imm8, SIGN-EXTENDED to the operand size
  if (opcode == 0x80 || opcode == 0x81 || opcode == 0x83) {
    ModRm modrm;
    const Status st = ReadModRm(&cur, prefixes, &modrm);
    if (!st.ok()) {
      return st;
    }

    static const Mnemonic kByReg[8] = {
        Mnemonic::kAdd, Mnemonic::kOr, Mnemonic::kAdc,
        Mnemonic::kSbb, Mnemonic::kAnd, Mnemonic::kSub,
        Mnemonic::kXor, Mnemonic::kCmp};
    static const bool kSupportedReg[8] = {true, true, true, true,
                                          true,  true,  true,  true};

    if (!kSupportedReg[modrm.reg & 0x07u]) {
      // ADC/SBB. Explicitly rejected: they consume CF, and Phase 1 does not
      // model flag inputs. Returning unsupported here is honest; decoding it and
      // ignoring CF would be silently wrong.
      return StatusCode::kUnimplementedInstruction;
    }
    insn.mnemonic = kByReg[modrm.reg & 0x07u];

    if (opcode == 0x80) {
      insn.op_size = 1;
      if (modrm.is_memory) {
        insn.form = Form::kMR8;
        FillMemOperand(&insn.dst, modrm, OpClass::kMem8);
      } else {
        insn.form = Form::kRR8RegDst;
        insn.dst.cls = OpClass::kReg8;
        insn.dst.reg = modrm.rm;
      }
      insn.imm = cur.Uint(1);
      insn.imm_is_signed_extension = false;
    } else {
      if (prefixes.operand_override) {
        return StatusCode::kUnimplementedInstruction;
      }
      insn.op_size = prefixes.rex_w ? 8u : 4u;
      const OpClass mem_cls =
          (insn.op_size == 8) ? OpClass::kMem64 : OpClass::kMem32;
      const Form mem_form =
          (insn.op_size == 8) ? Form::kMR64 : Form::kM32R32;
      const Form reg_form =
          (insn.op_size == 8) ? Form::kRR64RmDst : Form::kR32RmDst;
      insn.form = modrm.is_memory ? mem_form : reg_form;
      if (modrm.is_memory) {
        FillMemOperand(&insn.dst, modrm, mem_cls);
      } else {
        insn.dst.cls = OpClass::kReg64;
        insn.dst.reg = modrm.rm;
      }
      if (opcode == 0x81) {
        insn.imm = cur.Uint(4);
        insn.imm_is_signed_extension = false;
      } else {
        // 0x83: imm8 is sign-extended to the full operand size. This is an
        // architectural rule, not a convenience: Intel SDM Vol. 2 §2.2.1.1.
        insn.imm = static_cast<std::uint64_t>(SignExtend(cur.Uint(1), 8));
        insn.imm_is_signed_extension = true;
      }
    }
    if (!cur.Ok()) {
      return StatusCode::kDecodeError;
    }
    goto finalise;
  }

  // --- 0x84 / 0x85 : TEST r/m, r -------------------------------------------
  if (opcode == 0x84 || opcode == 0x85) {
    ModRm modrm;
    const Status st = ReadModRm(&cur, prefixes, &modrm);
    if (!st.ok()) {
      return st;
    }
    insn.mnemonic = Mnemonic::kTest;
    if (opcode == 0x84) {
      insn.op_size = 1;
      insn.form = modrm.is_memory ? Form::kMR8 : Form::kRR8RmDst;
      if (modrm.is_memory) {
        FillMemOperand(&insn.dst, modrm, OpClass::kMem8);
      } else {
        insn.dst.cls = OpClass::kReg8;
        insn.dst.reg = modrm.rm;
      }
      insn.src.cls = OpClass::kReg8;
      insn.src.reg = modrm.reg;
    } else {
      if (prefixes.operand_override) {
        return StatusCode::kUnimplementedInstruction;
      }
      insn.op_size = prefixes.rex_w ? 8u : 4u;
      const OpClass mem_cls =
          (insn.op_size == 8) ? OpClass::kMem64 : OpClass::kMem32;
      const Form mem_form =
          (insn.op_size == 8) ? Form::kMR64 : Form::kM32R32;
      const Form reg_form =
          (insn.op_size == 8) ? Form::kRR64RmDst : Form::kR32RmDst;
      insn.form = modrm.is_memory ? mem_form : reg_form;
      if (modrm.is_memory) {
        FillMemOperand(&insn.dst, modrm, mem_cls);
      } else {
        insn.dst.cls = OpClass::kReg64;
        insn.dst.reg = modrm.rm;
      }
      insn.src.cls = OpClass::kReg64;
      insn.src.reg = modrm.reg;
    }
    goto finalise;
  }

  // --- 0x8D : LEA ----------------------------------------------------------
  // Intel SDM Vol. 2 §2.2.1. LEA computes an address: no memory access, no
  // flags. That distinction is the reason it has its own instruction here.
  if (opcode == 0x8d) {
    ModRm modrm;
    const Status st = ReadModRm(&cur, prefixes, &modrm);
    if (!st.ok()) {
      return st;
    }
    if (!modrm.is_memory) {
      // mod == 3 is #UD for LEA.
      return StatusCode::kUnimplementedInstruction;
    }
    if (!prefixes.rex_w) {
      // LEA with a 32-bit destination is 32-bit addressing arithmetic; Phase 1
      // models 64-bit addresses only.
      return StatusCode::kUnimplementedInstruction;
    }
    insn.mnemonic = Mnemonic::kLea;
    insn.form = Form::kLea;
    insn.op_size = 8;
    insn.dst.cls = OpClass::kReg64;
    insn.dst.reg = modrm.reg;
    FillMemOperand(&insn.src, modrm, OpClass::kMem64);
    goto finalise;
  }

  // --- 0x0F 0xB6 / 0x0F 0xB7 : MOVZX ---------------------------------------
  // Intel SDM Vol. 2 §2.2.1, two-byte opcode map.
  if (opcode == 0x0f) {
    const std::uint8_t op2 = cur.U8();
    if (!cur.Ok()) {
      return StatusCode::kDecodeError;
    }
    if (op2 == 0xb6 || op2 == 0xb7) {
      ModRm modrm;
      const Status st = ReadModRm(&cur, prefixes, &modrm);
      if (!st.ok()) {
        return st;
      }
      insn.mnemonic = Mnemonic::kMovzx;
      const std::uint8_t src_width = (op2 == 0xb6) ? 1u : 2u;
      insn.form = (src_width == 1) ? Form::kMovzxRM : Form::kMovzxRM16;
      insn.op_size = src_width;
      insn.dst.cls = OpClass::kReg64;
      insn.dst.reg = modrm.reg;
      // The destination is always written 32-bit, so the result zero-extends to
      // 64. That is fixed by the encoding, independent of REX.W.
      if (modrm.is_memory) {
        FillMemOperand(&insn.src, modrm,
                       (src_width == 1) ? OpClass::kMem8 : OpClass::kMem32);
        insn.src.displacement = modrm.displacement;
        insn.src.scale = modrm.scale;
        insn.src.has_base = modrm.has_base;
        insn.src.base = modrm.base;
        insn.src.has_index = modrm.has_index;
        insn.src.index = modrm.index;
        insn.src.rip_relative = modrm.rip_relative;
        // Mark the 16-bit source width via op_size, and kMem32 as the natural
        // container class; the translator uses op_size, not the class, for the
        // access width. Documented in TranslateToIr.cpp.
        insn.src.cls = (src_width == 1) ? OpClass::kMem8 : OpClass::kMem32;
      } else {
        insn.src.cls = OpClass::kReg64;
        insn.src.reg = modrm.rm;
      }
      goto finalise;
    }
    if (op2 == 0xbe || op2 == 0xbf || op2 == 0x63) {
      // MOVSX r, r/m  (0x0F 0xBE /r 8-bit, 0x0F 0xBF /r 16-bit,
      //               0x0F 0x63 /r with REX.W 32-bit)
      ModRm modrm;
      const Status st = ReadModRm(&cur, prefixes, &modrm);
      if (!st.ok()) {
        return st;
      }
      insn.mnemonic = Mnemonic::kMovsx;
      std::uint8_t src_width = 0;
      if (op2 == 0xbe) {
        src_width = 1;
        insn.form = Form::kMovsxRM;
      } else if (op2 == 0xbf) {
        src_width = 2;
        insn.form = Form::kMovsxRM16;
      } else {
        // 0x63 without REX.W is MOVSXD r32, r/m32. Phase 1 requires REX.W
        // because it models 64-bit destinations only.
        if (!prefixes.rex_w) {
          return StatusCode::kUnimplementedInstruction;
        }
        src_width = 4;
        insn.form = Form::kMovsxRM32;
      }
      insn.op_size = src_width;
      insn.dst.cls = OpClass::kReg64;
      insn.dst.reg = modrm.reg;
      if (modrm.is_memory) {
        FillMemOperand(&insn.src, modrm,
                       (src_width == 4) ? OpClass::kMem32 : OpClass::kMem8);
      } else {
        insn.src.cls = OpClass::kReg64;
        insn.src.reg = modrm.rm;
      }
      goto finalise;
    }
    if (op2 == 0x1f) {
      // NOP r/m  (multi-byte NOP). Treated as a no-op.
      ModRm modrm;
      const Status st = ReadModRm(&cur, prefixes, &modrm);
      if (!st.ok()) {
        return st;
      }
      insn.mnemonic = Mnemonic::kNop;
      insn.form = Form::kNop;
      goto finalise;
    }
    if (op2 >= 0x80 && op2 <= 0x8f) {
      // Jcc rel32. Intel SDM Vol. 2 §2.2.1. Same conditions as the rel8 form
      // (0x70..0x7F); the condition code is the low nibble of the opcode.
      insn.mnemonic = Mnemonic::kJcc;
      insn.form = Form::kJccRel;
      insn.cond = static_cast<Cond>(op2 - 0x80u);
      insn.imm = static_cast<std::uint64_t>(SignExtend(cur.Uint(4), 32));
      if (!cur.Ok()) {
        return StatusCode::kDecodeError;
      }
      goto finalise;
    }
    if (op2 == 0xaf) {
      // IMUL r, r/m  (two-operand form). Intel SDM Vol. 2 §2.2.1.
      ModRm modrm;
      const Status st = ReadModRm(&cur, prefixes, &modrm);
      if (!st.ok()) {
        return st;
      }
      insn.mnemonic = Mnemonic::kIMul;
      insn.form = Form::kIMulRR;
      insn.op_size = prefixes.operand_override ? 2u : (prefixes.rex_w ? 8u : 4u);
      insn.dst.cls = OpClass::kReg64;
      insn.dst.reg = modrm.reg;
      if (modrm.is_memory) {
        FillMemOperand(&insn.src, modrm,
                       (insn.op_size == 8) ? OpClass::kMem64 : OpClass::kMem32);
      } else {
        insn.src.cls = OpClass::kReg64;
        insn.src.reg = modrm.rm;
      }
      goto finalise;
    }
    // ---- SSE / SSE2 foundation (Phase 12) ---------------------------------
    // 66 0F 6E / 7E : MOVD/MOVQ xmm <-> r/m (REX.W selects 64-bit MOVQ).
    if ((op2 == 0x6e || op2 == 0x7e) && prefixes.operand_override) {
      ModRm modrm;
      const Status st = ReadModRm(&cur, prefixes, &modrm);
      if (!st.ok()) return st;
      const bool to_xmm = (op2 == 0x6e);
      const std::uint8_t w = prefixes.rex_w ? 8u : 4u;
      insn.mnemonic = (w == 8) ? Mnemonic::kMovq : Mnemonic::kMovd;
      insn.op_size = w;
      insn.form = to_xmm ? Form::kMovdToXmm : Form::kMovdFromXmm;
      const OpClass mem_cls = (w == 8) ? OpClass::kMem64 : OpClass::kMem32;
      if (to_xmm) {
        insn.dst.cls = OpClass::kXmm;
        insn.dst.reg = modrm.reg;
        if (modrm.is_memory) {
          FillMemOperand(&insn.src, modrm, mem_cls);
        } else {
          insn.src.cls = OpClass::kReg64;
          insn.src.reg = modrm.rm;
        }
      } else {
        insn.src.cls = OpClass::kXmm;
        insn.src.reg = modrm.reg;
        if (modrm.is_memory) {
          FillMemOperand(&insn.dst, modrm, mem_cls);
        } else {
          insn.dst.cls = OpClass::kReg64;
          insn.dst.reg = modrm.rm;
        }
      }
      goto finalise;
    }
    // 66 0F 6F / 7F : MOVDQA ; F3 0F 6F / 7F : MOVDQU.
    if ((op2 == 0x6f || op2 == 0x7f) &&
        (prefixes.operand_override || prefixes.rep)) {
      ModRm modrm;
      const Status st = ReadModRm(&cur, prefixes, &modrm);
      if (!st.ok()) return st;
      insn.mnemonic = prefixes.rep ? Mnemonic::kMovdqu : Mnemonic::kMovdqa;
      const bool to_xmm = (op2 == 0x6f);
      insn.form = to_xmm ? Form::kMovdqaToXmm : Form::kMovdqaFromXmm;
      insn.op_size = 16;
      if (to_xmm) {
        insn.dst.cls = OpClass::kXmm;
        insn.dst.reg = modrm.reg;
        if (modrm.is_memory) {
          FillMemOperand(&insn.src, modrm, OpClass::kMem128);
        } else {
          insn.src.cls = OpClass::kXmm;
          insn.src.reg = modrm.rm;
        }
      } else {
        insn.src.cls = OpClass::kXmm;
        insn.src.reg = modrm.reg;
        if (modrm.is_memory) {
          FillMemOperand(&insn.dst, modrm, OpClass::kMem128);
        } else {
          insn.dst.cls = OpClass::kXmm;
          insn.dst.reg = modrm.rm;
        }
      }
      goto finalise;
    }
    // 66 0F EF/DB/EB/D4 : PXOR / PAND / POR / PADDQ.
    if (prefixes.operand_override &&
        (op2 == 0xef || op2 == 0xdb || op2 == 0xeb || op2 == 0xd4)) {
      ModRm modrm;
      const Status st = ReadModRm(&cur, prefixes, &modrm);
      if (!st.ok()) return st;
      insn.mnemonic = (op2 == 0xef)   ? Mnemonic::kPxor
                      : (op2 == 0xdb) ? Mnemonic::kPand
                      : (op2 == 0xeb) ? Mnemonic::kPor
                                      : Mnemonic::kPaddq;
      insn.form = Form::kPackedXmm;
      insn.op_size = 16;
      insn.dst.cls = OpClass::kXmm;
      insn.dst.reg = modrm.reg;
      if (modrm.is_memory) {
        FillMemOperand(&insn.src, modrm, OpClass::kMem128);
      } else {
        insn.src.cls = OpClass::kXmm;
        insn.src.reg = modrm.rm;
      }
      goto finalise;
    }
    return StatusCode::kUnimplementedInstruction;
  }

  // --- shifts: D0/D1 (by 1), D2/D3 (by CL), C0/C1 (by imm8) ----------------
  // Intel SDM Vol. 2 §2.2.1, Table 2-3, opcode column 4.
  if (opcode == 0xd0 || opcode == 0xd1 || opcode == 0xd2 || opcode == 0xd3 ||
      opcode == 0xc0 || opcode == 0xc1) {
    ModRm modrm;
    const Status st = ReadModRm(&cur, prefixes, &modrm);
    if (!st.ok()) {
      return st;
    }
    // Intel SDM Vol. 2 §2.2.1, opcode column 4:
    //   /0 ROL  /1 ROR  /2 RCL  /3 RCR  /4 SHL  /5 SHR  /6 SAL  /7 SAR
    // SAL is a synonym for SHL, so /4 and /6 are the same mnemonic.
    static const Mnemonic kShift[8] = {
        Mnemonic::kUnimplemented,  // /0 ROL
        Mnemonic::kUnimplemented,  // /1 ROR
        Mnemonic::kUnimplemented,  // /2 RCL
        Mnemonic::kUnimplemented,  // /3 RCR
        Mnemonic::kShl,            // /4 SHL
        Mnemonic::kShr,            // /5 SHR
        Mnemonic::kShl,            // /6 SAL
        Mnemonic::kSar,            // /7 SAR
    };
    static const bool kShiftSupported[8] = {true, true, false, false,
                                            true, true, true, true};
    const std::uint8_t regsel = modrm.reg & 0x07u;
    if (!kShiftSupported[regsel]) {
      return StatusCode::kUnimplementedInstruction;
    }
    insn.mnemonic = kShift[regsel];
    // Width: the byte opcodes (D0/D2/C0) are 8-bit. Otherwise 16 with 0x66, 64
    // with REX.W, else 32.
    insn.op_size = prefixes.operand_override
                       ? 2u
                       : (prefixes.rex_w ? 8u : 4u);

    if (opcode == 0xd0 || opcode == 0xd2 || opcode == 0xc0) {
      insn.op_size = 1;
      if (modrm.is_memory) {
        FillMemOperand(&insn.dst, modrm, OpClass::kMem8);
      } else {
        insn.dst.cls = OpClass::kReg8;
        insn.dst.reg = modrm.rm;
      }
    } else {
      if (modrm.is_memory) {
        FillMemOperand(&insn.dst, modrm,
                       (insn.op_size == 8) ? OpClass::kMem64 : OpClass::kMem32);
      } else {
        insn.dst.cls = OpClass::kReg64;
        insn.dst.reg = modrm.rm;
      }
    }

    if (opcode == 0xd0) {
      insn.form = Form::kShiftImm;
      insn.shift_count = 1;
    } else if (opcode == 0xd2) {
      insn.form = Form::kShiftCL;
    } else if (opcode == 0xc0) {
      insn.form = Form::kShiftImm;
      insn.shift_count = cur.U8();
    } else if (opcode == 0xd1) {
      insn.form = Form::kShiftImm;
      insn.shift_count = 1;
    } else if (opcode == 0xd3) {
      insn.form = Form::kShiftCL;
    } else {  // 0xc1
      insn.form = Form::kShiftImm;
      insn.shift_count = cur.U8();
    }
    if (!cur.Ok()) {
      return StatusCode::kDecodeError;
    }
    goto finalise;
  }

  // --- control flow ---------------------------------------------------------
  // Intel SDM Vol. 2 §2.2.1, Table 2-3, opcode column E and column F.
  if (opcode == 0xeb) {  // JMP rel8, sign-extended
    insn.mnemonic = Mnemonic::kJmp;
    insn.form = Form::kJmpRel;
    insn.imm = static_cast<std::uint64_t>(SignExtend(cur.Uint(1), 8));
    if (!cur.Ok()) {
      return StatusCode::kDecodeError;
    }
    goto finalise;
  }
  if (opcode == 0xe9) {  // JMP rel32
    insn.mnemonic = Mnemonic::kJmp;
    insn.form = Form::kJmpRel;
    insn.imm = static_cast<std::uint64_t>(SignExtend(cur.Uint(4), 32));
    if (!cur.Ok()) {
      return StatusCode::kDecodeError;
    }
    goto finalise;
  }
  if (opcode >= 0x70 && opcode <= 0x7f) {  // Jcc rel8
    insn.mnemonic = Mnemonic::kJcc;
    insn.form = Form::kJccRel;
    insn.cond = static_cast<Cond>(opcode - 0x70u);
    insn.imm = static_cast<std::uint64_t>(SignExtend(cur.Uint(1), 8));
    if (!cur.Ok()) {
      return StatusCode::kDecodeError;
    }
    goto finalise;
  }
  if (opcode == 0xe8) {  // CALL rel32
    insn.mnemonic = Mnemonic::kCall;
    insn.form = Form::kCallRel;
    insn.imm = static_cast<std::uint64_t>(SignExtend(cur.Uint(4), 32));
    if (!cur.Ok()) {
      return StatusCode::kDecodeError;
    }
    goto finalise;
  }
  if (opcode == 0xc3) {  // RET
    insn.mnemonic = Mnemonic::kRet;
    insn.form = Form::kRetn;
    goto finalise;
  }
  if (opcode == 0x90) {  // NOP
    insn.mnemonic = Mnemonic::kNop;
    insn.form = Form::kNop;
    goto finalise;
  }
  if (opcode == 0xcc) {  // INT3
    insn.mnemonic = Mnemonic::kInt3;
    insn.form = Form::kInt3;
    goto finalise;
  }

  // --- PUSH / POP r64: 0x50+rd / 0x58+rd ------------------------------------
  // Intel SDM Vol. 2 §2.2.1, Table 2-3. In 64-bit mode the default operand size
  // is 64 bits; a 0x66 prefix selects 16 bits, which is not in the supported set
  // and is reported rather than approximated.
  if (opcode >= 0x50 && opcode <= 0x57) {
    insn.mnemonic = Mnemonic::kPush;
    insn.form = Form::kPushR;
    insn.op_size = prefixes.operand_override ? 2u : 8u;  // 16- or 64-bit
    insn.dst.cls = OpClass::kReg64;
    insn.dst.reg = static_cast<std::uint8_t>((opcode - 0x50u) |
                                             (prefixes.rex_b ? 0x08u : 0x00u));
    goto finalise;
  }
  if (opcode >= 0x58 && opcode <= 0x5f) {
    insn.mnemonic = Mnemonic::kPop;
    insn.form = Form::kPopR;
    insn.op_size = prefixes.operand_override ? 2u : 8u;  // 16- or 64-bit
    insn.dst.cls = OpClass::kReg64;
    insn.dst.reg = static_cast<std::uint8_t>((opcode - 0x58u) |
                                             (prefixes.rex_b ? 0x08u : 0x00u));
    goto finalise;
  }

  // --- 0xFF /6 PUSH r/m ; 0x8F /0 POP r/m -----------------------------------
  if (opcode == 0xff || opcode == 0x8f) {
    ModRm modrm;
    const Status st = ReadModRm(&cur, prefixes, &modrm);
    if (!st.ok()) {
      return st;
    }
    const bool is_push = (opcode == 0xff);
    const std::uint8_t need_reg = is_push ? 6u : 0u;
    if ((modrm.reg & 0x07u) != need_reg) {
      return StatusCode::kUnimplementedInstruction;
    }
    insn.mnemonic = is_push ? Mnemonic::kPush : Mnemonic::kPop;
    insn.form = is_push ? Form::kPushR : Form::kPopR;
    insn.op_size = prefixes.operand_override ? 2u : 8u;
    if (modrm.is_memory) {
      FillMemOperand(&insn.dst, modrm,
                     (insn.op_size == 2) ? OpClass::kMem32 : OpClass::kMem64);
    } else {
      insn.dst.cls = OpClass::kReg64;
      insn.dst.reg = modrm.rm;
    }
    goto finalise;
  }

  // --- PUSH imm: 0x68 imm16/32, 0x6A imm8 (sign-extended) -------------------
  if (opcode == 0x68 || opcode == 0x6a) {
    insn.mnemonic = Mnemonic::kPush;
    insn.form = Form::kPushImm;
    insn.op_size = prefixes.operand_override ? 2u : 8u;
    if (opcode == 0x6a) {
      insn.imm = static_cast<std::uint64_t>(SignExtend(cur.Uint(1), 8));
    } else if (insn.op_size == 2) {
      insn.imm = static_cast<std::uint64_t>(SignExtend(cur.Uint(2), 16));
    } else {
      insn.imm = static_cast<std::uint64_t>(SignExtend(cur.Uint(4), 32));
    }
    insn.imm_is_signed_extension = true;
    if (!cur.Ok()) {
      return StatusCode::kDecodeError;
    }
    goto finalise;
  }

  // --- LEAVE: 0xC9 ----------------------------------------------------------
  if (opcode == 0xc9) {
    insn.mnemonic = Mnemonic::kLeave;
    insn.form = Form::kLeave;
    insn.op_size = 8;
    goto finalise;
  }

  // --- accumulator immediate: 0x14/0x15 ADC, 0x1C/0x1D SBB ------------------
  // Intel SDM Vol. 2 §2.2.1. AL/eAX is the implicit destination.
  if (opcode == 0x14 || opcode == 0x15 || opcode == 0x1c || opcode == 0x1d) {
    const bool adc = (opcode == 0x14 || opcode == 0x15);
    const bool byte_form = (opcode == 0x14 || opcode == 0x1c);
    insn.mnemonic = adc ? Mnemonic::kAdc : Mnemonic::kSbb;
    insn.src.cls = OpClass::kNone;  // immediate; the translator uses insn.imm
    insn.form = Form::kR32RI32;
    insn.dst.reg = 0;  // AL / eAX
    if (byte_form) {
      insn.op_size = 1;
      insn.dst.cls = OpClass::kReg8;
      insn.imm = cur.Uint(1);
    } else {
      insn.op_size =
          prefixes.operand_override ? 2u : (prefixes.rex_w ? 8u : 4u);
      insn.dst.cls = OpClass::kReg64;
      if (insn.op_size == 2) {
        insn.imm = cur.Uint(2);
      } else if (insn.op_size == 8) {
        insn.imm = static_cast<std::uint64_t>(SignExtend(cur.Uint(4), 32));
        insn.imm_is_signed_extension = true;
      } else {
        insn.imm = cur.Uint(4);
      }
    }
    if (!cur.Ok()) {
      return StatusCode::kDecodeError;
    }
    goto finalise;
  }

  // --- F6 / F7: MUL / IMUL / DIV / IDIV (implicit RAX/RDX, and AH for 8-bit) --
  // Intel SDM Vol. 2 §2.2.1. ModRM.reg selects: /4 MUL /5 IMUL /6 DIV /7 IDIV.
  if (opcode == 0xf6 || opcode == 0xf7) {
    ModRm modrm;
    const Status st = ReadModRm(&cur, prefixes, &modrm);
    if (!st.ok()) {
      return st;
    }
    Mnemonic m = Mnemonic::kUnimplemented;
    Form f = Form::kUnimplemented;
    switch (modrm.reg & 0x07u) {
      case 4: m = Mnemonic::kMul;  f = Form::kMulRm;  break;
      case 5: m = Mnemonic::kIMul; f = Form::kIMulRm; break;
      case 6: m = Mnemonic::kDiv;  f = Form::kDivRm;  break;
      case 7: m = Mnemonic::kIDiv; f = Form::kIDivRm; break;
      default: return StatusCode::kUnimplementedInstruction;
    }
    insn.mnemonic = m;
    insn.form = f;
    // 8-bit uses AH; 16/32/64 use the DX/EDX/RDX high register.
    insn.op_size = (opcode == 0xf6)
                       ? 1u
                       : (prefixes.operand_override ? 2u
                                                    : (prefixes.rex_w ? 8u : 4u));
    if (modrm.is_memory) {
      FillMemOperand(&insn.dst, modrm,
                     (insn.op_size == 1)
                         ? OpClass::kMem8
                         : ((insn.op_size == 8) ? OpClass::kMem64
                                                : OpClass::kMem32));
    } else {
      insn.dst.cls = (insn.op_size == 1) ? OpClass::kReg8 : OpClass::kReg64;
      insn.dst.reg = modrm.rm;
    }
    goto finalise;
  }

  // --- 0x69 / 0x6B: IMUL r, r/m, imm ----------------------------------------
  if (opcode == 0x69 || opcode == 0x6b) {
    ModRm modrm;
    const Status st = ReadModRm(&cur, prefixes, &modrm);
    if (!st.ok()) {
      return st;
    }
    insn.mnemonic = Mnemonic::kIMul;
    insn.form = Form::kIMulRRI;
    insn.op_size = prefixes.operand_override ? 2u : (prefixes.rex_w ? 8u : 4u);
    insn.dst.cls = OpClass::kReg64;
    insn.dst.reg = modrm.reg;
    if (modrm.is_memory) {
      FillMemOperand(&insn.src, modrm,
                     (insn.op_size == 8) ? OpClass::kMem64 : OpClass::kMem32);
    } else {
      insn.src.cls = OpClass::kReg64;
      insn.src.reg = modrm.rm;
    }
    if (opcode == 0x6b) {
      insn.imm = static_cast<std::uint64_t>(SignExtend(cur.Uint(1), 8));
    } else if (insn.op_size == 2) {
      insn.imm = static_cast<std::uint64_t>(SignExtend(cur.Uint(2), 16));
    } else {
      insn.imm = static_cast<std::uint64_t>(SignExtend(cur.Uint(4), 32));
    }
    insn.imm_is_signed_extension = true;
    if (!cur.Ok()) {
      return StatusCode::kDecodeError;
    }
    goto finalise;
  }

  // Not in the Phase-1 subset. Reported honestly rather than partially decoded.
  return StatusCode::kUnimplementedInstruction;

finalise:
  insn.length = static_cast<std::uint32_t>(cur.Offset());
  if (insn.length == 0 || insn.length > available) {
    return StatusCode::kDecodeError;
  }
  // A recognised ENCODING whose semantics are not in the Phase-1 subset is
  // reported as unimplemented. Returning kOk with an unimplemented mnemonic
  // would let a caller mistake "I understood the bytes" for "I can execute
  // them", which is precisely the confusion this decoder must never create.
  if (insn.mnemonic == Mnemonic::kUnimplemented) {
    return StatusCode::kUnimplementedInstruction;
  }
  // High-byte operands: an 8-bit register 4..7 with no REX prefix is AH/CH/DH/BH.
  const auto mark_high_byte = [&prefixes](Operand* o) {
    if (o->cls == OpClass::kReg8 && !prefixes.has_rex && o->reg >= 4 &&
        o->reg <= 7) {
      o->high_byte = true;
    }
  };
  mark_high_byte(&insn.dst);
  mark_high_byte(&insn.src);
  *out = insn;
  return StatusCode::kOk;
}

}  // namespace p5emu::x64
