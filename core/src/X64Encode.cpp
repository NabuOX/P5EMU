// SPDX-License-Identifier: LicenseRef-P5EMU-Proprietary
//
// P5EMU source is independently authored and proprietary. See LICENSE.md and
// PROVENANCE.md before adding code here.
//
// See X64Encode.h for why the canonicalisation problem is handled by an explicit
// rule rather than by pretending byte equality always holds.

#include "p5emu/cpu/X64Encode.h"

namespace p5emu::x64 {
namespace {

// Emits a little-endian immediate of `n` bytes.
void PutUint(std::vector<std::uint8_t>* out, std::uint64_t value,
             std::size_t n) {
  for (std::size_t i = 0; i < n; ++i) {
    out->push_back(static_cast<std::uint8_t>((value >> (8u * i)) & 0xffu));
  }
}

void PutSint(std::vector<std::uint8_t>* out, std::int64_t value,
             std::size_t n) {
  PutUint(out, static_cast<std::uint64_t>(value) &
                    ((n >= 8) ? ~std::uint64_t{0}
                              : ((std::uint64_t{1} << (8u * n)) - 1u)),
              n);
}

// REX byte for an operand needing an extension.
std::uint8_t Rex(bool w, bool r, bool x, bool b) {
  std::uint8_t rex = 0x40u;
  if (w) rex |= 0x08u;
  if (r) rex |= 0x04u;
  if (x) rex |= 0x02u;
  if (b) rex |= 0x01u;
  return rex;
}

// ModRM byte: mod(2) reg(3) rm(3).
//
// The arithmetic is written in `unsigned` on purpose. A `std::uint8_t` operand
// promotes to `int`, and mixing that with the `unsigned` masks below makes the
// shift implicitly convert signed to unsigned -- legal C++, but Clang's
// -Wsign-conversion rejects it under -Werror. Doing the shifts in one unsigned
// domain removes the mixed-sign expression rather than suppressing the warning.
std::uint8_t ModRmByte(std::uint8_t mod, std::uint8_t reg, std::uint8_t rm) {
  const unsigned m = static_cast<unsigned>(mod) & 0x03u;
  const unsigned r = static_cast<unsigned>(reg) & 0x07u;
  const unsigned b = static_cast<unsigned>(rm) & 0x07u;
  return static_cast<std::uint8_t>((m << 6) | (r << 3) | b);
}

// SIB byte: scale(2) index(3) base(3).
std::uint8_t SibByte(std::uint8_t scale_code, std::uint8_t index,
                     std::uint8_t base) {
  const unsigned s = static_cast<unsigned>(scale_code) & 0x03u;
  const unsigned i = static_cast<unsigned>(index) & 0x07u;
  const unsigned b = static_cast<unsigned>(base) & 0x07u;
  return static_cast<std::uint8_t>((s << 6) | (i << 3) | b);
}

// True when any of base/index/reg/rm needs the REX extension bit.
bool NeedsRexBit(std::uint8_t reg) { return (reg & 0x08u) != 0; }

// Canonical ModRM + optional SIB + displacement for a memory operand.
//
// Canonical choice: always the disp32 form (mod == 2) when there is a
// displacement, and disp8 when the displacement fits in a signed byte. That is a
// choice, not a requirement, which is exactly why HasCanonicalEncoding() exists.
Status EncodeMem(const Operand& op, std::uint8_t modrm_reg, bool rex_r,
                 std::uint8_t reg_high, bool rex_x, bool rex_b,
                 std::vector<std::uint8_t>* out) {
  if (!op.has_base && !op.has_index && !op.rip_relative && !op.absolute) {
    // No addressing information at all: fall back to the absolute disp32 form,
    // which is representable and unambiguous.
    out->push_back(static_cast<std::uint8_t>(0x80u |
                                             ((modrm_reg & 0x07u) << 3) | 0x05u));
    PutSint(out, op.displacement, 4);
    return StatusCode::kOk;
  }

  if (op.rip_relative) {
    out->push_back(static_cast<std::uint8_t>(0x80u |
                                             ((modrm_reg & 0x07u) << 3) | 0x05u));
    PutSint(out, op.displacement, 4);
    return StatusCode::kOk;
  }

  const bool disp8_fits = (op.displacement >= -128 && op.displacement <= 127);
  const bool has_disp = (op.displacement != 0) || !op.has_base;

  std::uint8_t mod;
  if (!has_disp) {
    mod = 0;
  } else if (disp8_fits) {
    mod = 1;
  } else {
    mod = 2;
  }

  // SIB is required when there is an index, or when base is rsp/r12 (rm==100).
  const bool base_is_rsp = op.has_base &&
                           (op.base & 0x07u) == 0x04u;
  const bool needs_sib = op.has_index || base_is_rsp;

  std::uint8_t rm_field;
  if (needs_sib) {
    rm_field = 0x04u;
    out->push_back(ModRmByte(mod, modrm_reg, rm_field));

    // Scale from the logical scale. index 4 means "no index".
    std::uint8_t scale_code = 0;
    if (op.scale == 1) scale_code = 0;
    else if (op.scale == 2) scale_code = 1;
    else if (op.scale == 4) scale_code = 2;
    else if (op.scale == 8) scale_code = 3;
    else return StatusCode::kUnimplementedInstruction;

    std::uint8_t index_field = op.has_index ? (op.index & 0x07u) : 0x04u;
    std::uint8_t base_field =
        op.has_base ? (op.base & 0x07u) : 0x05u;

    out->push_back(SibByte(scale_code, index_field, base_field));

    if (!op.has_base) {
      // base == 5 with mod == 0 is the no-base disp32 form.
      if (mod != 0) {
        // The decoder would have produced has_base == false only with mod == 0.
        return StatusCode::kInvalidArgument;
      }
      PutSint(out, op.displacement, 4);
      return StatusCode::kOk;
    }
  } else {
    rm_field = op.base & 0x07u;
    out->push_back(ModRmByte(mod, modrm_reg, rm_field));
  }

  if (has_disp) {
    PutSint(out, op.displacement, disp8_fits ? 1 : 4);
  }

  // REX.X / REX.B reflect the index and base.
  if ((op.has_index && NeedsRexBit(op.index)) ||
      (op.has_base && NeedsRexBit(op.base))) {
    // Handled by the caller via rex_x/rex_b; nothing to emit here.
    (void)rex_x;
    (void)rex_b;
    (void)reg_high;
    (void)rex_r;
  }
  return StatusCode::kOk;
}

}  // namespace

bool HasCanonicalEncoding(const Instruction& insn) noexcept {
  // Where x86 permits multiple encodings for the same semantic form, P5EMU's
  // canonical form may differ from the input. These are exactly those cases.
  switch (insn.form) {
    case Form::kJmpRel:
    case Form::kJccRel:
      // rel8 (EB/7x) and rel32 (E9/0F 8x) both exist for the same target.
      return false;
    case Form::kCallRel:
      // Only rel32 is in the Phase-1 subset, so it round trips exactly.
      return true;
    case Form::kRR64RmDst:
      // `mov r/m64, imm32` (C7 /0 id, sign-extended) and `mov r64, imm64`
      // (B8+r) are both 64-bit MOV of an immediate, and P5EMU canonicalises to
      // the B8+r form. The value is identical; only the length differs.
      return insn.src.cls != OpClass::kNone;
    case Form::kR32RmDst:
    case Form::kMR64:
    case Form::kM32R32:
      // Same C7 /0 case for the other widths.
      return insn.src.cls != OpClass::kNone;
    case Form::kMovzxRM:
    case Form::kMovzxRM16:
      // REX.W is architecturally ignored by 0F B6/B7, so P5EMU omits it. A
      // decoded encoding that carried a redundant REX.W does not survive a
      // byte-exact round trip.
      return !(insn.has_rex && ((insn.rex & 0x08u) != 0));
    default:
      break;
  }
  return true;
}

Status EncodeCanonical(const Instruction& insn, std::vector<std::uint8_t>* out) {
  if (out == nullptr) {
    return StatusCode::kInvalidArgument;
  }
  out->clear();

  const bool w = insn.op_size == 8;
  const bool rex_w = w && insn.mnemonic != Mnemonic::kMovzx &&
                     insn.form != Form::kMovzxRM &&
                     insn.form != Form::kMovzxRM16;

  auto emit_rex = [&](std::uint8_t r_ext, std::uint8_t x_ext,
                      std::uint8_t b_ext) {
    const bool need = (r_ext != 0) || (x_ext != 0) || (b_ext != 0) || rex_w;
    if (need) {
      out->push_back(Rex(rex_w, r_ext, x_ext, b_ext));
    }
  };

  // Emits a REX byte with an EXPLICIT W bit, unconditionally. Used where the
  // width flag is semantically required (MOVSX r64) and cannot be left to the
  // `emit_rex` optimisation, which only pushes a byte when some extension is
  // actually needed. Calling emit_rex and then OR-ing out->back() with 0x08 was
  // a real bug: when no extension bit was set, emit_rex pushed nothing and the
  // OR corrupted the preceding opcode byte.
  auto emit_rex_forced_w = [&](std::uint8_t r_ext, std::uint8_t x_ext,
                               std::uint8_t b_ext) {
    out->push_back(Rex(true, r_ext, x_ext, b_ext));
  };

  // Emits REX for a ModRM form. `rm_register` is the ModRM.rm operand when the
  // rm slot holds a REGISTER; when `mem` is non-null the rm slot holds a memory
  // reference and REX.B belongs to the base register instead.
  //
  // r_ext is derived from `reg_operand` (ModRM.reg) by the caller, which knows
  // which operand is which for that form. It is NOT derived from REX.W: passing
  // the width flag in the REX.R slot produced a spurious REX.R on every 64-bit
  // shift.
  auto emit_rex_modrm = [&](std::uint8_t reg_operand,
                           const Operand* mem,
                           std::uint8_t rm_register) {
    const std::uint8_t x_ext =
        (mem != nullptr && mem->has_index) ? NeedsRexBit(mem->index) : 0u;
    const std::uint8_t b_ext =
        (mem != nullptr) ? (mem->has_base ? NeedsRexBit(mem->base) : 0u)
                         : NeedsRexBit(rm_register);
    emit_rex(NeedsRexBit(reg_operand), x_ext, b_ext);
  };

  // The memory operand for ModRM-style forms, or nullptr for register-register
  // and immediate forms.
  //
  // Membership in the Form's "r/m" slot is NOT sufficient: kMovsxRM and
  // friends name the rm position, which holds a REGISTER in the register form.
  // The operand CLASS is the authority on whether an access is a memory
  // operation.
  auto is_memory_operand = [](const Operand& o) {
    return o.cls == OpClass::kMem8 || o.cls == OpClass::kMem32 ||
           o.cls == OpClass::kMem64;
  };

  const Operand* mem_operand = nullptr;
  switch (insn.form) {
    case Form::kRM64:
    case Form::kR32RM32:
    case Form::kRM8:
    case Form::kMovzxRM:
    case Form::kMovzxRM16:
    case Form::kMovsxRM:
    case Form::kMovsxRM16:
    case Form::kMovsxRM32:
    case Form::kLea:
      if (is_memory_operand(insn.src)) {
        mem_operand = &insn.src;
      }
      break;
    case Form::kMR64:
    case Form::kM32R32:
    case Form::kMR8:
      if (is_memory_operand(insn.dst)) {
        mem_operand = &insn.dst;
      }
      break;
    case Form::kShiftCL:
    case Form::kShiftImm:
      if (is_memory_operand(insn.dst)) {
        mem_operand = &insn.dst;
      }
      break;
    default:
      break;
  }

  switch (insn.mnemonic) {
    case Mnemonic::kMov: {
      // IMMEDIATE forms. Whether the immediate was encoded as B8+r imm,
      // C7 /0 id, or C6 /0 ib is decided by the DESTINATION: a register
      // destination uses B8+r; a memory destination uses C6/C7. The source
      // operand class distinguishes "immediate" from "register", which is the
      // only reliable signal, because C7 /0 also appears with mod=3.
      const bool immediate_src = (insn.src.cls == OpClass::kNone);
      const bool dst_is_memory = is_memory_operand(insn.dst);

      if (immediate_src && !dst_is_memory && insn.op_size == 8) {
        // Canonical 64-bit: REX.W B8+r imm64, always 10 bytes.
        out->push_back(Rex(true, false, false, NeedsRexBit(insn.dst.reg)));
        out->push_back(
            static_cast<std::uint8_t>(0xb8u | (insn.dst.reg & 0x07u)));
        PutUint(out, insn.imm, 8);
        return StatusCode::kOk;
      }
      if (immediate_src && !dst_is_memory && insn.op_size == 4) {
        out->push_back(Rex(false, false, false, NeedsRexBit(insn.dst.reg)));
        out->push_back(
            static_cast<std::uint8_t>(0xb8u | (insn.dst.reg & 0x07u)));
        PutUint(out, insn.imm & 0xffffffffull, 4);
        return StatusCode::kOk;
      }
      if (immediate_src && dst_is_memory) {
        // C6 /0 ib (byte) or C7 /0 id (16/32/64-bit). For 64-bit operands the
        // imm32 is sign-extended, which is only possible when the value fits;
        // otherwise the form is not representable.
        if (insn.op_size == 1) {
          out->push_back(Rex(false, false, false, false));
          out->push_back(0xc6u);
          const Status st = EncodeMem(insn.dst, 0, false, 0, false, false, out);
          if (!st.ok()) return st;
          out->push_back(static_cast<std::uint8_t>(insn.imm & 0xffu));
          return StatusCode::kOk;
        }
        emit_rex_modrm(0, mem_operand, 0);
        out->push_back(0xc7u);
        const Status st = EncodeMem(insn.dst, 0, false, 0, false, false, out);
        if (!st.ok()) return st;
        PutSint(out, static_cast<std::int64_t>(insn.imm), 4);
        return StatusCode::kOk;
      }

      // REGISTER or MEMORY source: 88/89/8A/8B.
      {
        const bool byte_form = (insn.op_size == 1);
        // Which ModRM field is the DESTINATION: the RegDst forms take it from
        // ModRM.reg (8A/8B), the RmDst forms from ModRM.rm (88/89).
        const bool dst_is_reg =
            (insn.form == Form::kRR8RegDst) ||
            (insn.form == Form::kRR64RegDst) ||
            (insn.form == Form::kR32RegDst) ||
            (insn.form == Form::kRM8) ||
            (insn.form == Form::kRM64) ||
            (insn.form == Form::kR32RM32);
        const std::uint8_t reg_operand = dst_is_reg ? insn.dst.reg : insn.src.reg;
        const std::uint8_t rm_operand =
            dst_is_reg ? (mem_operand != nullptr ? 0 : insn.src.reg) : insn.dst.reg;

        emit_rex(static_cast<std::uint8_t>(NeedsRexBit(reg_operand)),
                 static_cast<std::uint8_t>(mem_operand != nullptr &&
                                           mem_operand->has_index &&
                                           NeedsRexBit(mem_operand->index)),
                 static_cast<std::uint8_t>(mem_operand != nullptr &&
                                           mem_operand->has_base &&
                                           NeedsRexBit(mem_operand->base) ? 1
                                                                            : (dst_is_reg &&
                                                                               mem_operand == nullptr
                                                                                   ? NeedsRexBit(rm_operand)
                                                                                   : 0)));
        if (byte_form) {
          out->push_back(dst_is_reg ? 0x8au : 0x88u);
        } else {
          out->push_back(dst_is_reg ? 0x8bu : 0x89u);
        }
        if (mem_operand != nullptr) {
          const Status st = EncodeMem(*mem_operand, reg_operand, false, 0,
                                      false, false, out);
          if (!st.ok()) return st;
        } else {
          out->push_back(static_cast<std::uint8_t>(
              0xc0u | ((reg_operand & 0x07u) << 3) | (rm_operand & 0x07u)));
        }
        return StatusCode::kOk;
      }
    }

    case Mnemonic::kMovzx:
    case Mnemonic::kMovsx: {
      const bool is_zx = (insn.mnemonic == Mnemonic::kMovzx);

      // REX must be emitted BEFORE the opcode bytes: a REX byte placed after
      // 0F is decoded as a prefix for a *different* instruction, silently
      // changing the meaning. Order is load-bearing here.
      //
      // MOVZX writes its destination 32-bit and always zero-extends, so REX.W is
      // architecturally IGNORED on 0F B6/B7 (Intel SDM Vol. 2 §2.2.1) and P5EMU
      // omits it.
      //
      // MOVSX is different: on 0F BE/BF, REX.W selects a 64-bit destination
      // instead of 32-bit, so dropping it would CHANGE the instruction. It is
      // therefore reproduced from the decoded prefix rather than recomputed.
      const bool src_is_mem = is_memory_operand(insn.src);
      const Operand* src_mem = src_is_mem ? &insn.src : nullptr;
      const bool decoded_rex_w = insn.has_rex && ((insn.rex & 0x08u) != 0);

      if (is_zx) {
        emit_rex_modrm(insn.dst.reg, src_mem, insn.src.reg);
      } else if (insn.form == Form::kMovsxRM32 || decoded_rex_w) {
        // 0F 63 (MOVSXD) requires REX.W unconditionally.
        // 0F BE/BF with REX.W selects a 64-bit destination.
        emit_rex_forced_w(
            NeedsRexBit(insn.dst.reg),
            (src_mem != nullptr && src_mem->has_index)
                ? NeedsRexBit(src_mem->index)
                : std::uint8_t{0},
            (src_mem != nullptr)
                ? (src_mem->has_base ? NeedsRexBit(src_mem->base)
                                     : std::uint8_t{0})
                : NeedsRexBit(insn.src.reg));
      } else {
        // MOVSX r32, r/m8 or r/m16: destination is 32-bit, no REX.W.
        emit_rex_modrm(insn.dst.reg, src_mem, insn.src.reg);
      }

      out->push_back(0x0fu);
      if (is_zx) {
        out->push_back(insn.op_size == 1 ? 0xb6u : 0xb7u);
      } else {
        if (insn.form == Form::kMovsxRM) out->push_back(0xbeu);
        else if (insn.form == Form::kMovsxRM16) out->push_back(0xbfu);
        else out->push_back(0x63u);
      }

      if (src_mem != nullptr) {
        const Status st =
            EncodeMem(*src_mem, insn.dst.reg, false, 0, false, false, out);
        if (!st.ok()) return st;
      } else {
        out->push_back(static_cast<std::uint8_t>(
            0xc0u | ((insn.dst.reg & 0x07u) << 3) | (insn.src.reg & 0x07u)));
      }
      return StatusCode::kOk;
    }

    case Mnemonic::kLea: {
      emit_rex_modrm(insn.dst.reg,
                     is_memory_operand(insn.src) ? &insn.src : nullptr, 0);
      out->push_back(0x8du);
      const Status st = EncodeMem(insn.src, insn.dst.reg, false, 0, false,
                                  false, out);
      if (!st.ok()) return st;
      return StatusCode::kOk;
    }

    case Mnemonic::kAdd:
    case Mnemonic::kSub:
    case Mnemonic::kAnd:
    case Mnemonic::kOr:
    case Mnemonic::kXor:
    case Mnemonic::kCmp: {
      // Intel SDM Vol. 2 §2.2.1, opcode column 2/3. Each ALU operation has
      // THREE opcodes, differing in operand size, and the size suffix is NOT a
      // simple +1 for every entry -- for the operations above OR/AND/XOR/ADD
      // the 8-bit forms are 00/08/20/28/30/38 and the 32/64-bit forms are
      // 01/09/21/29/31/39.
      struct Entry {
        std::uint8_t byte_op;    // opcode for 8-bit operands
        std::uint8_t word_op;    // opcode for 16/32/64-bit operands
        std::uint8_t digit;      // /digit for the immediate forms
      };
      Entry e{};
      switch (insn.mnemonic) {
        case Mnemonic::kAdd: e = {0x00, 0x01, 0}; break;
        case Mnemonic::kOr:  e = {0x08, 0x09, 1}; break;
        case Mnemonic::kAnd: e = {0x20, 0x21, 4}; break;
        case Mnemonic::kSub: e = {0x28, 0x29, 5}; break;
        case Mnemonic::kXor: e = {0x30, 0x31, 6}; break;
        case Mnemonic::kCmp: e = {0x38, 0x39, 7}; break;
        default: return StatusCode::kInvalidArgument;
      }
      const bool byte_form = (insn.op_size == 1);

      // Immediate forms: identified by src.cls == kNone. The decoder leaves
      // src unpopulated for an immediate, so this is a reliable discriminator.
      if (insn.src.cls == OpClass::kNone) {
        if (byte_form) {
          emit_rex_modrm(0, mem_operand, insn.dst.reg);
          out->push_back(0x80u);
          const std::uint8_t digit = e.digit;
          if (mem_operand != nullptr) {
            const Status st = EncodeMem(insn.dst, digit, false, 0, false, false,
                                        out);
            if (!st.ok()) return st;
          } else {
            out->push_back(ModRmByte(3, digit, insn.dst.reg));
          }
          out->push_back(static_cast<std::uint8_t>(insn.imm & 0xffu));
          return StatusCode::kOk;
        }
        // Canonical: prefer opcode 83 (imm8 sign-extended to the operand size)
        // when the value is representable that way, because 83 is both shorter
        // and the form a canonical assembler emits. Fall back to 81 (imm32)
        // otherwise. Intel SDM Vol. 2 §2.2.1.1.
        const std::int64_t value = static_cast<std::int64_t>(insn.imm);
        const bool fits_imm8 = (value >= -128) && (value <= 127);
        // 83 sign-extends, so it is only valid when the 64-bit value IS the
        // sign-extended imm8 -- not merely when it fits.
        const bool use_83 = fits_imm8;
        emit_rex_modrm(0, mem_operand, insn.dst.reg);
        out->push_back(use_83 ? 0x83u : 0x81u);
        const std::uint8_t digit = e.digit;
        if (mem_operand != nullptr) {
          const Status st = EncodeMem(insn.dst, digit, false, 0, false, false,
                                      out);
          if (!st.ok()) return st;
        } else {
          out->push_back(ModRmByte(3, digit, insn.dst.reg));
        }
        if (use_83) {
          out->push_back(static_cast<std::uint8_t>(insn.imm & 0xffu));
        } else {
          PutSint(out, value, 4);
        }
        return StatusCode::kOk;
      }

      // Register/memory forms. Which ModRM field is the destination comes from
      // the Form, not from a guess about operand classes.
      {
        const bool dst_is_reg =
            (insn.form != Form::kMR8) && (insn.form != Form::kMR64) &&
            (insn.form != Form::kM32R32) &&
            (insn.form != Form::kRR64RmDst) &&
            (insn.form != Form::kR32RmDst) &&
            (insn.form != Form::kRR8RmDst);
        const std::uint8_t reg_operand = dst_is_reg ? insn.dst.reg : insn.src.reg;
        const std::uint8_t rm_operand = dst_is_reg ? insn.src.reg : insn.dst.reg;
        emit_rex_modrm(reg_operand, mem_operand, rm_operand);
        // 8-bit operands use the 0x0n opcode; 32/64-bit use 0x0n+1.
        const std::uint8_t base_op = byte_form ? e.byte_op : e.word_op;
        out->push_back(dst_is_reg ? static_cast<std::uint8_t>(base_op + 0x02u)
                                  : base_op);
        if (mem_operand != nullptr) {
          const Status st = EncodeMem(*mem_operand, reg_operand, false, 0,
                                      false, false, out);
          if (!st.ok()) return st;
        } else {
          out->push_back(static_cast<std::uint8_t>(
              0xc0u | ((reg_operand & 0x07u) << 3) | (rm_operand & 0x07u)));
        }
        return StatusCode::kOk;
      }
    }

    case Mnemonic::kTest: {
      const bool byte_form = (insn.op_size == 1);
      // Which operand is the source register is decided by the Form: RegDst
      // forms take the destination from ModRM.reg, RmDst forms from ModRM.rm.
      {
        const bool dst_is_reg =
            (insn.form != Form::kMR8) && (insn.form != Form::kMR64) &&
            (insn.form != Form::kM32R32) &&
            (insn.form != Form::kRR64RmDst) &&
            (insn.form != Form::kR32RmDst) &&
            (insn.form != Form::kRR8RmDst);
        const std::uint8_t reg_operand = dst_is_reg ? insn.dst.reg : insn.src.reg;
        const std::uint8_t rm_operand = dst_is_reg ? insn.src.reg : insn.dst.reg;
        emit_rex_modrm(reg_operand, mem_operand, rm_operand);
        out->push_back(byte_form ? 0x84u : 0x85u);
        if (mem_operand != nullptr) {
          const Status st =
              EncodeMem(*mem_operand, reg_operand, false, 0, false, false, out);
          if (!st.ok()) return st;
        } else {
          out->push_back(static_cast<std::uint8_t>(
              0xc0u | ((reg_operand & 0x07u) << 3) | (rm_operand & 0x07u)));
        }
        return StatusCode::kOk;
      }
    }

    case Mnemonic::kShl:
    case Mnemonic::kShr:
    case Mnemonic::kSar: {
      std::uint8_t digit = 0;
      switch (insn.mnemonic) {
        case Mnemonic::kShl: digit = 4; break;
        case Mnemonic::kShr: digit = 5; break;
        case Mnemonic::kSar: digit = 7; break;
        default: return StatusCode::kInvalidArgument;
      }
      const bool byte_form = (insn.op_size == 1);

      // D2/D3 (count = CL) and C0/C1 (count = imm8) share the ModRM and SIB
      // encoding; only the immediate differs.
      emit_rex_modrm(0, mem_operand, insn.dst.reg);
      if (insn.form == Form::kShiftCL) {
        out->push_back(byte_form ? 0xd2u : 0xd3u);
      } else if (insn.shift_count == 1) {
        out->push_back(byte_form ? 0xd0u : 0xd1u);
      } else {
        out->push_back(byte_form ? 0xc0u : 0xc1u);
      }

      if (mem_operand != nullptr) {
        const Status st = EncodeMem(*mem_operand, digit, false, 0, false, false,
                                    out);
        if (!st.ok()) return st;
      } else {
        out->push_back(ModRmByte(3, digit, insn.dst.reg));
      }

      if (insn.form == Form::kShiftImm && insn.shift_count != 1) {
        out->push_back(insn.shift_count);
      }
      return StatusCode::kOk;
    }

    case Mnemonic::kJmp: {
      if (insn.imm >= static_cast<std::uint64_t>(-128) &&
          insn.imm <= static_cast<std::uint64_t>(127)) {
        out->push_back(0xebu);
        PutSint(out, static_cast<std::int64_t>(insn.imm), 1);
      } else {
        out->push_back(0xe9u);
        PutSint(out, static_cast<std::int64_t>(insn.imm), 4);
      }
      return StatusCode::kOk;
    }
    case Mnemonic::kJcc: {
      if (insn.imm >= static_cast<std::uint64_t>(-128) &&
          insn.imm <= static_cast<std::uint64_t>(127)) {
        out->push_back(static_cast<std::uint8_t>(
            0x70u | static_cast<std::uint8_t>(insn.cond)));
        PutSint(out, static_cast<std::int64_t>(insn.imm), 1);
      } else {
        out->push_back(0x0fu);
        out->push_back(static_cast<std::uint8_t>(
            0x80u | static_cast<std::uint8_t>(insn.cond)));
        PutSint(out, static_cast<std::int64_t>(insn.imm), 4);
      }
      return StatusCode::kOk;
    }
    case Mnemonic::kCall: {
      out->push_back(0xe8u);
      PutSint(out, static_cast<std::int64_t>(insn.imm), 4);
      return StatusCode::kOk;
    }
    case Mnemonic::kRet:
      out->push_back(0xc3u);
      return StatusCode::kOk;
    case Mnemonic::kNop:
      out->push_back(0x90u);
      return StatusCode::kOk;
    case Mnemonic::kInt3:
      out->push_back(0xccu);
      return StatusCode::kOk;

    case Mnemonic::kUnimplemented:
    default:
      return StatusCode::kUnimplementedInstruction;
  }
}

}  // namespace p5emu::x64
