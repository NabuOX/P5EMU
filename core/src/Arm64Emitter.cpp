// SPDX-License-Identifier: LicenseRef-P5EMU-Proprietary
//
// P5EMU source is independently authored and proprietary. See LICENSE.md and
// PROVENANCE.md before adding code here.
//
// Field-level encoders for the AArch64 subset Phase 2 emits. Every function
// names the ARM ARM fields it packs. See Arm64Emitter.h.

#include "p5emu/cpu/Arm64Emitter.h"

namespace p5emu::arm64 {

const char* CondName(Cond c) noexcept {
  switch (c) {
    case Cond::kEq: return "eq";
    case Cond::kNe: return "ne";
    case Cond::kCs: return "cs";
    case Cond::kCc: return "cc";
    case Cond::kMi: return "mi";
    case Cond::kPl: return "pl";
    case Cond::kVs: return "vs";
    case Cond::kVc: return "vc";
    case Cond::kHi: return "hi";
    case Cond::kLs: return "ls";
    case Cond::kGe: return "ge";
    case Cond::kLt: return "lt";
    case Cond::kGt: return "gt";
    case Cond::kLe: return "le";
    case Cond::kAl: return "al";
    case Cond::kNv: return "nv";
  }
  return "??";
}

namespace {

inline std::uint32_t Bits(std::uint32_t value, unsigned lo, unsigned hi) {
  const unsigned width = hi - lo + 1;
  const std::uint32_t mask = (width >= 32) ? 0xFFFFFFFFu : ((1u << width) - 1u);
  return (value & mask) << lo;
}

// 5-bit register fields never exceed 31.
inline std::uint32_t Rd(std::uint32_t r) { return (r & 0x1Fu); }
inline std::uint32_t Rn(std::uint32_t r) { return (r & 0x1Fu) << 5; }
inline std::uint32_t Rm(std::uint32_t r) { return (r & 0x1Fu) << 16; }
inline std::uint32_t Ra(std::uint32_t r) { return (r & 0x1Fu) << 10; }

inline std::uint32_t Truncate9(std::int32_t v) {
  return static_cast<std::uint32_t>(v) & 0x1FFu;
}
inline std::uint32_t Truncate7(std::int32_t v) {
  return static_cast<std::uint32_t>(v) & 0x7Fu;
}

}  // namespace

namespace enc {

std::uint32_t Movz(Reg rd, std::uint16_t imm16, unsigned shift) {
  const unsigned hw = shift / 16u;  // 0..3
  return 0xD2800000u | (hw << 21) | (static_cast<std::uint32_t>(imm16) << 5) |
         Rd(rd);
}

std::uint32_t Movk(Reg rd, std::uint16_t imm16, unsigned shift) {
  const unsigned hw = shift / 16u;
  return 0xF2800000u | (hw << 21) | (static_cast<std::uint32_t>(imm16) << 5) |
         Rd(rd);
}

std::uint32_t AddSubImm(bool sub, bool set_flags, Reg rd, Reg rn,
                        std::uint32_t imm, bool shift12) {
  return 0x91000000u | (sub ? 0x40000000u : 0u) |
         (set_flags ? 0x20000000u : 0u) | (shift12 ? 0x00400000u : 0u) |
         Bits(imm, 10, 21) | Rn(rn) | Rd(rd);
}

std::uint32_t AddSubReg(bool sub, bool set_flags, Reg rd, Reg rn, Reg rm,
                        unsigned shift, unsigned amount) {
  return 0x8B000000u | (sub ? 0x40000000u : 0u) |
         (set_flags ? 0x20000000u : 0u) | Bits(shift, 22, 23) | Rm(rm) |
         Bits(amount, 10, 15) | Rn(rn) | Rd(rd);
}

std::uint32_t LogicalReg(unsigned opc, Reg rd, Reg rn, Reg rm, unsigned shift,
                         unsigned amount) {
  return 0x8A000000u | Bits(opc, 29, 30) | Bits(shift, 22, 23) | Rm(rm) |
         Bits(amount, 10, 15) | Rn(rn) | Rd(rd);
}

std::uint32_t MovReg64(Reg rd, Reg rm) {
  return LogicalReg(1u, rd, regs::kXzr, rm, 0u, 0u);
}

std::uint32_t MovReg32(Reg rd, Reg rm) {
  // 32-bit ORR Wd, WZR, Wm : sf = 0.
  return 0x2A000000u | (1u << 29) | Rm(rm) | (31u << 5) | Rd(rd);
}

std::uint32_t LsrImm(Reg rd, Reg rn, unsigned amount) {
  // UBFM Xd, Xn, #amount, #63
  return 0xD3400000u | Bits(amount, 16, 21) | Bits(63u, 10, 15) | Rn(rn) | Rd(rd);
}

std::uint32_t AsrImm(Reg rd, Reg rn, unsigned amount) {
  // SBFM Xd, Xn, #amount, #63
  return 0x93400000u | Bits(amount, 16, 21) | Bits(63u, 10, 15) | Rn(rn) | Rd(rd);
}

std::uint32_t LslImm(Reg rd, Reg rn, unsigned amount) {
  // UBFM Xd, Xn, #((-amount) mod 64), #(63 - amount)
  const unsigned immr = (64u - amount) & 63u;
  const unsigned imms = (63u - amount) & 63u;
  return 0xD3400000u | Bits(immr, 16, 21) | Bits(imms, 10, 15) | Rn(rn) | Rd(rd);
}

std::uint32_t Lslv(Reg rd, Reg rn, Reg rm) {
  // sf 0 0 11010110 Rm 001000 Rn Rd
  return 0x9AC02000u | Rm(rm) | Rn(rn) | Rd(rd);
}
std::uint32_t Lsrv(Reg rd, Reg rn, Reg rm) {
  return 0x9AC02400u | Rm(rm) | Rn(rn) | Rd(rd);
}
std::uint32_t Asrv(Reg rd, Reg rn, Reg rm) {
  return 0x9AC02800u | Rm(rm) | Rn(rn) | Rd(rd);
}

std::uint32_t Sxtb(Reg rd, Reg rn) {
  return 0x93400000u | Bits(0u, 16, 21) | Bits(7u, 10, 15) | Rn(rn) | Rd(rd);
}
std::uint32_t Sxth(Reg rd, Reg rn) {
  return 0x93400000u | Bits(0u, 16, 21) | Bits(15u, 10, 15) | Rn(rn) | Rd(rd);
}
std::uint32_t Sxtw(Reg rd, Reg rn) {
  return 0x93400000u | Bits(0u, 16, 21) | Bits(31u, 10, 15) | Rn(rn) | Rd(rd);
}

std::uint32_t Csel(Reg rd, Reg rn, Reg rm, Cond cond) {
  return 0x9A800000u | Rm(rm) | Bits(static_cast<unsigned>(cond), 12, 15) |
         Rn(rn) | Rd(rd);
}

std::uint32_t Csinc(Reg rd, Reg rn, Reg rm, Cond cond) {
  return 0x9A800400u | Rm(rm) | Bits(static_cast<unsigned>(cond), 12, 15) |
         Rn(rn) | Rd(rd);
}

std::uint32_t Cset(Reg rd, Cond cond) {
  const unsigned inverted = (static_cast<unsigned>(cond) ^ 1u) & 0xFu;
  return Csinc(rd, regs::kXzr, regs::kXzr, static_cast<Cond>(inverted));
}

std::uint32_t Madd(Reg rd, Reg rn, Reg rm, Reg ra) {
  return 0x9B000000u | Rm(rm) | Ra(ra) | Rn(rn) | Rd(rd);
}

std::uint32_t LdrU64(Reg rt, Reg rn, unsigned byte_offset) {
  const unsigned imm12 = byte_offset / 8u;
  return 0xF9400000u | Bits(imm12, 10, 21) | Rn(rn) | Rd(rt);
}

std::uint32_t StrU64(Reg rt, Reg rn, unsigned byte_offset) {
  const unsigned imm12 = byte_offset / 8u;
  return 0xF9000000u | Bits(imm12, 10, 21) | Rn(rn) | Rd(rt);
}

std::uint32_t StrU64Pre(Reg rt, Reg rn, int simm9) {
  return 0xF8000C00u | Bits(Truncate9(simm9), 12, 20) | Rn(rn) | Rd(rt);
}

std::uint32_t LdrU64Post(Reg rt, Reg rn, int simm9) {
  return 0xF8400400u | Bits(Truncate9(simm9), 12, 20) | Rn(rn) | Rd(rt);
}

std::uint32_t StpU64Pre(Reg rt, Reg rt2, Reg rn, int simm7_scaled) {
  // opc 101 0 010 0 imm7 Rt2 Rn Rt : Rt2 is bits 14-10, NOT 20-16.
  return 0xA9800000u | Bits(Truncate7(simm7_scaled), 15, 21) | Ra(rt2) | Rn(rn) |
         Rd(rt);
}

std::uint32_t LdpU64Post(Reg rt, Reg rt2, Reg rn, int simm7_scaled) {
  return 0xA8C00000u | Bits(Truncate7(simm7_scaled), 15, 21) | Ra(rt2) | Rn(rn) |
         Rd(rt);
}

std::uint32_t StpU64Off(Reg rt, Reg rt2, Reg rn, int simm7_scaled) {
  return 0xA9000000u | Bits(Truncate7(simm7_scaled), 15, 21) | Ra(rt2) | Rn(rn) |
         Rd(rt);
}

std::uint32_t Cbz(Reg rt, std::int32_t imm19_words) {
  return 0xB4000000u |
         Bits(static_cast<std::uint32_t>(imm19_words), 5, 23) | Rd(rt);
}

std::uint32_t Cbnz(Reg rt, std::int32_t imm19_words) {
  return 0xB5000000u |
         Bits(static_cast<std::uint32_t>(imm19_words), 5, 23) | Rd(rt);
}

std::uint32_t B(std::int32_t imm26_words) {
  return 0x14000000u |
         (static_cast<std::uint32_t>(imm26_words) & 0x03FFFFFFu);
}

std::uint32_t Bl(std::int32_t imm26_words) {
  return 0x94000000u |
         (static_cast<std::uint32_t>(imm26_words) & 0x03FFFFFFu);
}

std::uint32_t BCond(Cond cond, std::int32_t imm19_words) {
  return 0x54000000u |
         Bits(static_cast<std::uint32_t>(imm19_words), 5, 23) |
         Bits(static_cast<unsigned>(cond), 0, 3);
}

std::uint32_t Ret(Reg rn) { return 0xD65F0000u | Rn(rn); }
std::uint32_t Br(Reg rn) { return 0xD61F0000u | Rn(rn); }
std::uint32_t Blr(Reg rn) { return 0xD63F0000u | Rn(rn); }

std::uint32_t Ubfx(Reg rd, Reg rn, unsigned lsb, unsigned width) {
  const unsigned imms = (lsb + width - 1u) & 63u;
  return 0xD3400000u | Bits(lsb, 16, 21) | Bits(imms, 10, 15) | Rn(rn) | Rd(rd);
}

std::uint32_t Sbfx(Reg rd, Reg rn, unsigned lsb, unsigned width) {
  // SBFM Xd, Xn, #lsb, #(lsb + width - 1)
  const unsigned imms = (lsb + width - 1u) & 63u;
  return 0x93400000u | Bits(lsb, 16, 21) | Bits(imms, 10, 15) | Rn(rn) | Rd(rd);
}

}  // namespace enc

int Assembler::NewLabel() {
  label_pos_.push_back(-1);
  return static_cast<int>(label_pos_.size()) - 1;
}

void Assembler::Bind(int label) {
  if (label < 0 || static_cast<std::size_t>(label) >= label_pos_.size()) {
    return;
  }
  label_pos_[static_cast<std::size_t>(label)] =
      static_cast<long>(words_.size());
}

void Assembler::Raw(std::uint32_t word) { words_.push_back(word); }

void Assembler::B(int label) {
  Raw(enc::B(0));
  fixups_.push_back(Fixup{words_.size() - 1, label, true});
}

void Assembler::Bl(int label) {
  Raw(enc::Bl(0));
  fixups_.push_back(Fixup{words_.size() - 1, label, true});
}

void Assembler::BCond(Cond cond, int label) {
  Raw(enc::BCond(cond, 0));
  fixups_.push_back(Fixup{words_.size() - 1, label, false});
}

void Assembler::Cbz(Reg rt, int label) {
  Raw(enc::Cbz(rt, 0));
  fixups_.push_back(Fixup{words_.size() - 1, label, false});
}

void Assembler::Cbnz(Reg rt, int label) {
  Raw(enc::Cbnz(rt, 0));
  fixups_.push_back(Fixup{words_.size() - 1, label, false});
}

void Assembler::MovImm64(Reg rd, std::uint64_t value) {
  bool placed_any = false;
  // MOVZ the lowest non-zero halfword, MOVK the rest. If every halfword is
  // zero, a single MOVZ #0 still materialises the register.
  for (unsigned shift = 0; shift < 64; shift += 16) {
    const std::uint16_t chunk =
        static_cast<std::uint16_t>((value >> shift) & 0xFFFFu);
    if (!placed_any) {
      if (chunk == 0 && shift != 48) {
        continue;
      }
      Raw(enc::Movz(rd, chunk, shift));
      placed_any = true;
    } else if (chunk != 0) {
      Raw(enc::Movk(rd, chunk, shift));
    }
  }
  if (!placed_any) {
    Raw(enc::Movz(rd, 0, 0));
  }
}

bool Assembler::Finalize(std::vector<std::uint8_t>* out) const {
  if (out == nullptr) {
    return false;
  }
  std::vector<std::uint32_t> patched = words_;
  for (const Fixup& f : fixups_) {
    if (f.label < 0 || static_cast<std::size_t>(f.label) >= label_pos_.size()) {
      return false;
    }
    const long dest = label_pos_[static_cast<std::size_t>(f.label)];
    if (dest < 0) {
      return false;  // unbound label
    }
    const long offset = dest - static_cast<long>(f.word);
    if (f.wide) {
      patched[f.word] =
          (patched[f.word] & 0xFC000000u) |
          (static_cast<std::uint32_t>(offset) & 0x03FFFFFFu);
    } else {
      patched[f.word] =
          (patched[f.word] & ~(0x7FFFFu << 5)) |
          ((static_cast<std::uint32_t>(offset) & 0x7FFFFu) << 5);
    }
  }
  out->clear();
  out->reserve(patched.size() * 4);
  for (const std::uint32_t w : patched) {
    out->push_back(static_cast<std::uint8_t>(w & 0xFFu));
    out->push_back(static_cast<std::uint8_t>((w >> 8) & 0xFFu));
    out->push_back(static_cast<std::uint8_t>((w >> 16) & 0xFFu));
    out->push_back(static_cast<std::uint8_t>((w >> 24) & 0xFFu));
  }
  return true;
}

}  // namespace p5emu::arm64
