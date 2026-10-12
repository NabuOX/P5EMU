// SPDX-License-Identifier: Apache-2.0
//
// P5EMU is an independent, open-source project. See LICENSE and
// PROVENANCE.md before adding code here.
//
// ============================================================================
// AArch64 EMITTER
// ============================================================================
//
// A tiny, independently authored encoder for the AArch64 instructions Phase 2
// actually emits. It is NOT a general assembler and it is deliberately not
// backed by LLVM, VIXL or binutils: those would put a large third-party surface
// between P5EMU's IR and the bytes it claims to produce. See
// docs/architecture/arm64-backend.md for the emitter decision.
//
// Encodings are transcribed field-by-field from the Arm Architecture Reference
// Manual (ARM ARM, DD0487) A64 instruction set. Each function names its fields
// so a reviewer can check it against the ARM ARM without decoding magic numbers.
//
// This header produces bytes only. It never executes anything and has no host
// dependency, so it is fully testable on the x86-64 development host by
// disassembling its output with an independent tool.
//
// Register numbering: 0..30 are X0..X30. 31 means XZR or SP depending on the
// instruction; the helpers that need SP (load/store base, add/sub) accept 31 and
// the ARM ARM defines which it means there.
// ============================================================================

#ifndef P5EMU_CPU_ARM64_EMITTER_H
#define P5EMU_CPU_ARM64_EMITTER_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace p5emu::arm64 {

using Reg = std::uint8_t;

// AArch64 general-purpose register numbers used by the backend. Named so the
// codegen reads as intent rather than magic numbers.
namespace regs {
inline constexpr Reg kX0 = 0, kX1 = 1, kX2 = 2, kX3 = 3, kX4 = 4, kX5 = 5,
                     kX6 = 6, kX7 = 7;
inline constexpr Reg kX8 = 8, kX9 = 9, kX10 = 10, kX11 = 11, kX12 = 12,
                     kX13 = 13, kX14 = 14, kX15 = 15;
inline constexpr Reg kX16 = 16, kX17 = 17, kX18 = 18;
inline constexpr Reg kX19 = 19, kX20 = 20, kX21 = 21, kX22 = 22, kX23 = 23,
                     kX24 = 24, kX25 = 25, kX26 = 26, kX27 = 27, kX28 = 28;
inline constexpr Reg kX29 = 29, kX30 = 30;
inline constexpr Reg kSp = 31;
inline constexpr Reg kXzr = 31;
}  // namespace regs

// AArch64 condition codes, in architectural encoding order.
enum class Cond : std::uint8_t {
  kEq = 0b0000,  // Z == 1
  kNe = 0b0001,  // Z == 0
  kCs = 0b0010,  // C == 1  (carry set / unsigned >=)
  kCc = 0b0011,  // C == 0  (carry clear / unsigned <)
  kMi = 0b0100,  // N == 1
  kPl = 0b0101,  // N == 0
  kVs = 0b0110,  // V == 1
  kVc = 0b0111,  // V == 0
  kHi = 0b1000,  // C == 1 && Z == 0
  kLs = 0b1001,  // !(C == 1 && Z == 0)
  kGe = 0b1010,  // N == V
  kLt = 0b1011,  // N != V
  kGt = 0b1100,  // Z == 0 && N == V
  kLe = 0b1101,  // !(Z == 0 && N == V)
  kAl = 0b1110,  // always
  kNv = 0b1111,
};

const char* CondName(Cond c) noexcept;

// ---------------------------------------------------------------------------
// Single-instruction encoders
// ---------------------------------------------------------------------------
// Each returns the 32-bit instruction word. Kept free functions so they can be
// unit-tested directly against known-good words.
namespace enc {

// Move wide (immediate):  sf 10 100101 hw imm16 Rd
// `shift` is 0/16/32/48.
std::uint32_t Movz(Reg rd, std::uint16_t imm16, unsigned shift);
std::uint32_t Movk(Reg rd, std::uint16_t imm16, unsigned shift);

// Add/subtract (immediate):  sf op S 100010 sh imm12 Rn Rd
// `imm` is 0..4095 and `shift12` selects LSL #12.
std::uint32_t AddSubImm(bool sub, bool set_flags, Reg rd, Reg rn,
                        std::uint32_t imm, bool shift12);
// Shifted-register add/sub:  sf op S 01011 shift Rm imm6 Rn Rd
// `shift` is 0 LSL, 1 LSR, 2 ASR. `amount` is 0..63.
std::uint32_t AddSubReg(bool sub, bool set_flags, Reg rd, Reg rn, Reg rm,
                        unsigned shift, unsigned amount);

// Logical (shifted register):  sf opc 01010 shift N Rm imm6 Rn Rd
// `opc`: 0 AND, 1 ORR, 2 EOR.  `shift`: 0 LSL, 1 LSR, 2 ASR, 3 ROR.
std::uint32_t LogicalReg(unsigned opc, Reg rd, Reg rn, Reg rm, unsigned shift,
                         unsigned amount);

// Move register (alias of ORR Rd, XZR, Rm). 32-bit form zero-extends.
std::uint32_t MovReg64(Reg rd, Reg rm);
std::uint32_t MovReg32(Reg rd, Reg rm);

// Shifts by immediate (aliases of UBFM/SBFM).
std::uint32_t LslImm(Reg rd, Reg rn, unsigned amount);
std::uint32_t LsrImm(Reg rd, Reg rn, unsigned amount);
std::uint32_t AsrImm(Reg rd, Reg rn, unsigned amount);

// Shifts by register: Rd = Rn <</>> Rm. Shift amount is taken modulo 64.
std::uint32_t Lslv(Reg rd, Reg rn, Reg rm);
std::uint32_t Lsrv(Reg rd, Reg rn, Reg rm);
std::uint32_t Asrv(Reg rd, Reg rn, Reg rm);

// Sign-extend to 64 bits.
std::uint32_t Sxtb(Reg rd, Reg rn);
std::uint32_t Sxth(Reg rd, Reg rn);
std::uint32_t Sxtw(Reg rd, Reg rn);

// Conditional select / increment.
// CSEL  Xd, Xn, Xm, cond  : Rd = cond ? Rn : Rm
// CSINC Xd, Xn, Xm, cond  : Rd = cond ? Rn : Rm + 1
std::uint32_t Csel(Reg rd, Reg rn, Reg rm, Cond cond);
std::uint32_t Csinc(Reg rd, Reg rn, Reg rm, Cond cond);
// CSET Xd, cond : Rd = cond ? 1 : 0
std::uint32_t Cset(Reg rd, Cond cond);

// Multiply-add: Rd = Ra + Rn * Rm.
std::uint32_t Madd(Reg rd, Reg rn, Reg rm, Reg ra);

// Load/store (unsigned offset, 64-bit).
std::uint32_t LdrU64(Reg rt, Reg rn, unsigned byte_offset);
std::uint32_t StrU64(Reg rt, Reg rn, unsigned byte_offset);
// Load/store (pre/post-indexed, signed 9-bit byte offset).
std::uint32_t StrU64Pre(Reg rt, Reg rn, int simm9);
std::uint32_t LdrU64Post(Reg rt, Reg rn, int simm9);
// Store/load pair (64-bit), signed 7-bit scaled offset.
std::uint32_t StpU64Pre(Reg rt, Reg rt2, Reg rn, int simm7_scaled);
std::uint32_t LdpU64Post(Reg rt, Reg rt2, Reg rn, int simm7_scaled);
std::uint32_t StpU64Off(Reg rt, Reg rt2, Reg rn, int simm7_scaled);

// Compare-and-branch, zero / not-zero.
std::uint32_t Cbz(Reg rt, std::int32_t imm19_words);
std::uint32_t Cbnz(Reg rt, std::int32_t imm19_words);

// Unconditional branch / branch with link / return.
std::uint32_t B(std::int32_t imm26_words);
std::uint32_t Bl(std::int32_t imm26_words);
std::uint32_t BCond(Cond cond, std::int32_t imm19_words);
std::uint32_t Ret(Reg rn);
std::uint32_t Br(Reg rn);
std::uint32_t Blr(Reg rn);

// Bitfield extract: Rd = (Rn >> lsb) & ((1<<width)-1). width in 1..64.
std::uint32_t Ubfx(Reg rd, Reg rn, unsigned lsb, unsigned width);
// Signed bitfield extract: Rd = sign_extend((Rn >> lsb)[width-1:0]).
std::uint32_t Sbfx(Reg rd, Reg rn, unsigned lsb, unsigned width);

}  // namespace enc

// ---------------------------------------------------------------------------
// Assembler
// ---------------------------------------------------------------------------
// Accumulates instruction words and resolves symbolic labels. Labels are local
// to one emitted function.
class Assembler {
 public:
  // Returns a new, unbound label id.
  [[nodiscard]] int NewLabel();

  // Binds `label` to the current position.
  void Bind(int label);

  // Emits a raw instruction word.
  void Raw(std::uint32_t word);

  // Emits one of the instruction forms above, applying label fixups where the
  // form carries a branch displacement.
  void B(int label);
  void Bl(int label);
  void BCond(Cond cond, int label);
  void Cbz(Reg rt, int label);
  void Cbnz(Reg rt, int label);

  // Materialises a 64-bit constant into `rd` using MOVZ/MOVK.
  void MovImm64(Reg rd, std::uint64_t value);

  [[nodiscard]] std::size_t WordCount() const noexcept { return words_.size(); }

  // Resolves fixups and returns the little-endian instruction bytes.
  //
  // Returns false if any label was left unbound, which would otherwise produce a
  // silent branch to offset 0 -- a bug that is very hard to see in a diff.
  bool Finalize(std::vector<std::uint8_t>* out) const;

  // Disassembles nothing and executes nothing; for diagnostics only.
  [[nodiscard]] std::vector<std::uint32_t> Words() const { return words_; }

 private:
  std::vector<std::uint32_t> words_;
  std::vector<long> label_pos_;  // -1 == unbound
  struct Fixup {
    std::size_t word = 0;
    int label = -1;
    bool wide = false;  // true: B/BL (26-bit); false: B.cond/CBZ (19-bit)
  };
  std::vector<Fixup> fixups_;
};

}  // namespace p5emu::arm64

#endif  // P5EMU_CPU_ARM64_EMITTER_H
