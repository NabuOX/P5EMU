// SPDX-License-Identifier: Apache-2.0
//
// P5EMU is an independent, open-source project. See LICENSE and
// PROVENANCE.md before adding code here.
//
// ============================================================================
// X86-64 DECODER (Phase 1 subset)
// ============================================================================
//
// SCOPE, DELIBERATELY SMALL
//
// This decodes roughly twenty instructions chosen to exercise x86-64
// *architecture*, not to maximise coverage. Explicitly NOT here: SIMD, AVX, x87,
// privileged instructions, and any table large enough to look like an attempt at
// completeness. A large table would be worse than none: it would imply coverage
// P5EMU cannot test, and every unhandled case would become a silent
// miscompilation instead of an honest kUnimplementedInstruction.
//
// SEPARATION OF CONCERNS (hard architectural requirement)
//
//   * This file knows x86-64 ENCODING only: prefixes, opcode bytes, ModRM, SIB,
//     displacement, immediate. It answers "what bytes mean which fields?".
//   * src/TranslateToIr.cpp knows x86-64 SEMANTICS: what MOV means, which flags
//     ADD sets, when a 32-bit write zero-extends. It answers "what does this
//     instruction DO?".
//   * src/ReferenceInterpreter.cpp knows only P5EMU IR semantics.
//
// A decoder that also decided semantics would make the IR builder untestable in
// isolation, and would let an encoding quirk leak into IR semantics.
//
// The DecodedInstruction below is the boundary: a *description*, with no
// behaviour attached.
//
// ENCODING SOURCES
//
// Every encoding rule here is derived from the Intel 64 and IA-32 Architectures
// Software Developer's Manual (SDM), Vol. 2 and Vol. 3, and cross-checked
// against the AMD64 Architecture Programmer's Manual (APM). Section references
// appear at the relevant code. These are public vendor specifications; no
// third-party disassembler's tables or output were consulted or copied.
//
// FAILURE POLICY
//
// Unknown, unsupported, or malformed input returns kUnimplementedInstruction or
// kDecodeError. The decoder NEVER guesses, NEVER consumes a partial instruction,
// and NEVER returns a "best effort" result. A decoder that guesses produces two
// backends that disagree on what the guest program was, which is unrecoverable.
//
// ============================================================================

#ifndef P5EMU_CPU_X64_DECODER_H
#define P5EMU_CPU_X64_DECODER_H

#include <cstdint>

#include "p5emu/core/Status.h"
#include "p5emu/cpu/GuestMemory.h"

namespace p5emu::x64 {

// ---------------------------------------------------------------------------
// Architectural register numbering (as encoded in REX and in opcodes)
// ---------------------------------------------------------------------------
enum Reg8 : std::uint8_t {
  kAl = 0, kCl, kDl, kBl, kSpl, kBpl, kSil,  kDil,
  kR8b, kR9b, kR10b, kR11b, kR12b, kR13b, kR14b, kR15b,
};

enum Reg64 : std::uint8_t {
  kRax = 0, kRcx, kRdx, kRbx, kRsp, kRbp, kRsi, kRdi,
  kR8, kR9, kR10, kR11, kR12, kR13, kR14, kR15,
};

// ---------------------------------------------------------------------------
// Condition codes as encoded in Jcc (opcode 0x70..0x7F).
// Intel SDM Vol. 2 §3.1.2.1, Table 3-1 "Condition-Code Names".
// ---------------------------------------------------------------------------
enum class Cond : std::uint8_t {
  kO = 0, kNo, kB, kAE, kE, kNE, kBE, kA,
  kS, kNS, kP, kNP, kL, kGE, kLE, kG,
};

const char* Reg8Name(std::uint8_t reg) noexcept;
const char* Reg64Name(std::uint8_t reg) noexcept;
const char* CondName(Cond c) noexcept;

// ---------------------------------------------------------------------------
// Operand description
// ---------------------------------------------------------------------------
enum class OpClass : std::uint8_t {
  kNone = 0,
  kReg8,     // 8-bit general register
  kReg64,    // 64-bit general register
  // An x86 memory operand. Effective address is computed but NOT accessed:
  // address calculation and memory access are distinct operations, which is why
  // kMem64 has a width instead of implying a load.
  kMem8,
  kMem32,
  kMem64,
};

struct Operand {
  OpClass cls = OpClass::kNone;

  // Register operand.
  std::uint8_t reg = 0;
  // Set for an 8-bit register number 4..7 with NO REX prefix: those encode the
  // high-byte registers AH/CH/DH/BH, not spl/bpl/sil/dil. Phase 12.
  bool high_byte = false;

  // Memory operand: base + index*scale + displacement.
  bool has_base = false;
  std::uint8_t base = 0;
  bool has_index = false;
  std::uint8_t index = 0;
  std::uint8_t scale = 1;
  std::int64_t displacement = 0;
  // RIP-relative. Kept distinct because it computes relative to the NEXT
  // instruction, a rule that is easy to get wrong and is a real x86 rule
  // (Intel SDM Vol. 2 §3.7.2.1), not a simplification.
  bool rip_relative = false;
  // Absolute address (mod == 0, rm == 5, no base, no SVD), i.e. moffs form.
  bool absolute = false;
  std::uint64_t absolute_address = 0;
};

const char* OpClassName(OpClass cls) noexcept;

// ---------------------------------------------------------------------------
// Instructions
// ---------------------------------------------------------------------------
// The Phase-1 subset. Small on purpose; see the scope note in the file header.
enum class Mnemonic : std::uint16_t {
  kUnimplemented = 0,

  kMov,       // reg/reg/mem/imm, in all the width combinations Phase 1 supports
  kMovzx,     // zero-extend
  kMovsx,     // sign-extend
  kLea,       // address computation only: no memory access, no flags

  kAdd,
  kSub,
  kAnd,
  kOr,
  kXor,
  kCmp,
  kTest,

  kShl,       // CL form and imm8 form
  kShr,
  kSar,

  kJmp,       // rel8 / rel32
  kJcc,       // rel8 / rel32
  kCall,      // rel32
  kRet,
  kNop,

  kInt3,      // decodes, and is translated to an IR trap

  // Phase 11: stack family (operand forms supported by the current IR only).
  kPush,      // PUSH r64 / PUSH imm8 / PUSH imm32
  kPop,       // POP r64
  kLeave,     // LEAVE (RSP := RBP; RBP := pop)

  // Phase 11: arithmetic families.
  kAdc,       // add with carry
  kSbb,       // subtract with borrow
  kMul,       // unsigned multiply (implicit RAX/RDX)
  kIMul,      // signed multiply (one-/two-operand/immediate)
  kDiv,       // unsigned divide (implicit RAX/RDX, #DE)
  kIDiv,      // signed divide (implicit RAX/RDX, #DE)
};

const char* MnemonicName(Mnemonic m) noexcept;

// Instruction form, which fixes operand meaning. Making this explicit rather
// than deriving it from "is dst a register, is src a register" is what makes the
// decode testable: every form has an unambiguous operand meaning.
enum class Form : std::uint8_t {
  kUnimplemented = 0,

  // ---- MOV register/register -------------------------------------------
  // Two forms, because "MOV rax, rbx" (89 /r: r <- r/m) and "MOV rbx, rax"
  // (89 /r: r/m <- r) share an opcode but differ in which ModRM field is the
  // DESTINATION. Collapsing them into one form would lose the operand order,
  // and both the translator and the re-encoder would silently swap the operands.
  //
  // kRR64RegDst: dst = ModRM.reg, src = ModRM.rm   (opcode 8B)
  // kRR64RmDst:  dst = ModRM.rm,  src = ModRM.reg  (opcode 89)
  kRR64RegDst,
  kRR64RmDst,
  kR32RegDst,
  kR32RmDst,
  kR8RegDst,
  kR8RmDst,

  kRM64,      // dst = ModRM.reg (64-bit), src = memory
  kMR64,      // dst = memory, src = ModRM.reg (64-bit)
  kRI64,      // dst = ModRM.reg, src = imm64
  kR32RM32,   // dst = reg32, src = r/m32
  kM32R32,    // dst = m32, src = reg32
  kR32RI32,   // dst = reg32, src = imm32
  kRR8RegDst,
  kRR8RmDst,
  kRM8,       // dst = reg8, src = r/m8
  kMR8,       // dst = m8, src = reg8
  kRI8,       // dst = reg8, src = imm8
  kMoffs,     // reserved: absolute moffs form is not in the Phase-1 subset
  kLea,       // dst = reg64, src = memory address (no access)
  kMovzxRM,   // dst = reg32, src = r/m8
  kMovzxRM16, // dst = reg32, src = r/m16
  kMovsxRM,   // dst = reg64, src = r/m8
  kMovsxRM16, // dst = reg64, src = r/m16
  kMovsxRM32, // dst = reg64, src = r/m32
  kShiftCL,   // dst = r/m, count = CL
  kShiftImm,  // dst = r/m, count = imm8
  kJmpRel,
  kJccRel,
  kCallRel,
  kRetn,
  kNop,
  kInt3,

  // Phase 11 stack forms. Only the 64-bit register/immediate forms are in the
  // supported set; 16-bit operand-size and memory-source forms are reported
  // unimplemented rather than approximated.
  kPushR,      // PUSH r64
  kPushImm,    // PUSH imm8/imm32 (sign-extended to 64)
  kPopR,       // POP r64
  kLeave,      // LEAVE

  // Phase 11 arithmetic forms.
  kMulRm,      // MUL r/m (implicit RAX/RDX)
  kIMulRm,     // IMUL r/m (one-operand implicit form)
  kDivRm,      // DIV r/m (implicit RAX/RDX)
  kIDivRm,     // IDIV r/m (implicit RAX/RDX)
  kIMulRR,     // IMUL reg, r/m   (0F AF)
  kIMulRRI,    // IMUL reg, r/m, imm  (69 / 6B)
};

const char* FormName(Form f) noexcept;

struct Instruction {
  Mnemonic mnemonic = Mnemonic::kUnimplemented;
  Form form = Form::kUnimplemented;

  Operand dst;
  Operand src;

  // Extra immediates, already sign- or zero-extended to 64 bits per the
  // architectural rule for the operand size. Kept separate from `src` because
  // an immediate is not an Operand class.
  std::uint64_t imm = 0;
  bool imm_is_signed_extension = false;

  Cond cond = Cond::kO;          // kJccRel
  std::uint8_t shift_count = 0;  // kShiftImm
  std::uint8_t op_size = 8;      // operand size in bytes, after prefixes
  std::uint8_t address_size = 8;
  std::uint8_t rex = 0;          // 0 when absent
  bool has_rex = false;

  memory::GuestAddr address = 0;     // guest VA this was fetched from
  std::uint32_t length = 0;          // bytes consumed
};

// ---------------------------------------------------------------------------
// Decoder
// ---------------------------------------------------------------------------
class Decoder {
 public:
  virtual ~Decoder() = default;

  // Decodes exactly one instruction from `code` (at most `available` bytes,
  // starting at guest address `address`).
  //
  // On success: kOk, `out` filled, `out->length` set.
  // On unsupported encoding: kUnimplementedInstruction, `out` untouched.
  // On malformed/truncated encoding: kDecodeError, `out` untouched.
  //
  // `out` is never partially populated on failure. A caller can therefore treat
  // it as valid iff the status is kOk, with no cleanup step.
  virtual Status DecodeOne(memory::GuestAddr address, const std::uint8_t* code,
                           std::size_t available, Instruction* out) const = 0;
};

// The concrete Phase-1 decoder.
class X64Decoder final : public Decoder {
 public:
  Status DecodeOne(memory::GuestAddr address, const std::uint8_t* code,
                   std::size_t available, Instruction* out) const override;

  // True if `mnemonic` is in the Phase-1 supported set. Used by tests and by the
  // coverage report, so "unsupported" is an inspectable fact rather than an
  // absence.
  static bool IsSupported(Mnemonic mnemonic) noexcept;

  // The supported set, for documentation and coverage reporting.
  static std::size_t SupportedCount() noexcept;
};

}  // namespace p5emu::x64

#endif  // P5EMU_CPU_X64_DECODER_H
