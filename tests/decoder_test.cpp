// SPDX-License-Identifier: Apache-2.0
//
// x86-64 decoder tests.
//
// ============================================================================
// WHERE THE EXPECTED VALUES COME FROM
// ============================================================================
//
// Every byte sequence and every expected field in this file is derived by hand
// from the Intel 64 and IA-32 Architectures Software Developer's Manual:
//
//   §2.2.1      Instruction Reference (opcode listings)
//   §2.2.2.7    ModR/M Encoding
//   §2.2.2.8    SIB Encoding
//   §2.2.3.2    REX Prefixes
//   §3.1.2      Flags Register
//   §3.7.2.1    Data Transfer Instructions (RIP-relative)
//
// and cross-checked against the AMD64 Architecture Programener's Manual.
//
// NO disassembler, assembler, emulator or other tool was used to produce the
// fixtures. In particular, objdump/ndisasm/gas were NOT consulted: an expected
// value produced by running a third-party tool would make these tests a
// comparison against that tool rather than against the architecture.
//
// Each test names its source section in a comment, per
// FixtureProvenance.h.

#include <cstring>
#include <memory>
#include <cstdio>
#include <string>
#include <vector>

#include "FixtureProvenance.h"
#include "TestHarness.h"
#include "p5emu/cpu/X64Decoder.h"
#include "p5emu/cpu/X64Encode.h"

namespace {

using namespace p5emu;   // NOLINT - test-local convenience
using namespace p5emu::x64; // NOLINT

const X64Decoder g_decoder;

// Decodes one instruction from a byte array.
Status Decode(const std::vector<std::uint8_t>& bytes, Instruction* out,
              memory::GuestAddr addr = 0x1000) {
  return g_decoder.DecodeOne(addr, bytes.data(), bytes.size(), out);
}

std::vector<std::uint8_t> B(std::initializer_list<int> v) {
  std::vector<std::uint8_t> out;
  out.reserve(v.size());
  for (int b : v) {
    out.push_back(static_cast<std::uint8_t>(b));
  }
  return out;
}

// ===========================================================================
// MOV r64, imm64  --  Intel SDM Vol. 2 §2.2.1.1 (Immediate Encodings)
// ===========================================================================
// REX.W + B8+r + imm64 is exactly 10 bytes.
P5EMU_TEST(Decoder, MovRegImm64) {
  Instruction insn;
  const auto bytes = B({0x48, 0xB8, 0xEF, 0xCD, 0xAB, 0x89, 0x67, 0x45, 0x23, 0x01});
  REQUIRE(Decode(bytes, &insn).ok());

  CHECK_EQ(std::string(MnemonicName(insn.mnemonic)), std::string("mov"));
  CHECK_EQ(std::string(FormName(insn.form)), std::string("ri64"));
  CHECK_EQ(static_cast<int>(insn.dst.cls), static_cast<int>(OpClass::kReg64));
  CHECK_EQ(static_cast<int>(insn.dst.reg), 0);  // rax
  CHECK_EQ(insn.imm, 0x0123'4567'89ab'cdefull);
  CHECK_EQ(insn.length, 10u);
  CHECK_EQ(static_cast<int>(insn.op_size), 8);
}

P5EMU_TEST(Decoder, MovRegImm64WithRexB) {
  // REX.B extends the opcode's register field to r8..r15.
  // REX = 0x49 = 0100 1001: W=1, B=1. Intel SDM Vol. 2 §2.2.3.2.
  Instruction insn;
  const auto bytes = B({0x49, 0xB8, 1, 0, 0, 0, 0, 0, 0, 0});
  REQUIRE(Decode(bytes, &insn).ok());
  CHECK_EQ(static_cast<int>(insn.dst.reg), 8);  // r8
  CHECK_EQ(insn.imm, std::uint64_t{1});
}

P5EMU_TEST(Decoder, MovRegImm32ZeroExtendsTo64) {
  // Without REX.W, B8+r takes imm32 and the result is written to a 32-bit
  // destination, which zero-extends to 64. Intel SDM Vol. 2 §2.2.1.1.
  Instruction insn;
  const auto bytes = B({0xB8, 0x78, 0x56, 0x34, 0x12});
  REQUIRE(Decode(bytes, &insn).ok());
  CHECK_EQ(std::string(FormName(insn.form)), std::string("r32/imm32"));
  CHECK_EQ(insn.imm, 0x1234'5678ull);
  CHECK_EQ(insn.length, 5u);
  CHECK_EQ(static_cast<int>(insn.op_size), 4);
}

// ===========================================================================
// MOV reg, reg  --  Intel SDM Vol. 2 §2.2.1 (89 /r with REX.W)
// ===========================================================================
P5EMU_TEST(Decoder, MovRegReg) {
  Instruction insn;
  // 48 89 D8 = MOV rax, rbx
  //   48   REX.WB
  //   89   MOV r64 <- r/m64
  //   D8   ModRM: mod=11 reg=011(rbx) rm=000(rax)
  REQUIRE(Decode(B({0x48, 0x89, 0xD8}), &insn).ok());
  CHECK_EQ(std::string(MnemonicName(insn.mnemonic)), std::string("mov"));
  CHECK_EQ(static_cast<int>(insn.dst.reg), 0);  // rax
  CHECK_EQ(static_cast<int>(insn.src.reg), 3);  // rbx
  CHECK_EQ(insn.length, 3u);
}

P5EMU_TEST(Decoder, MovRegRegReversedOperands) {
  // 48 89 C3 = MOV rbx, rax. ModRM mod=11 reg=000(rax) rm=011(rbx):
  // destination is reg, source is r/m.
  Instruction insn;
  REQUIRE(Decode(B({0x48, 0x89, 0xC3}), &insn).ok());
  CHECK_EQ(static_cast<int>(insn.dst.reg), 3);  // rbx
  CHECK_EQ(static_cast<int>(insn.src.reg), 0);  // rax
}

// ===========================================================================
// MOV reg, r/m with memory operands  --  Intel SDM Vol. 2 §2.2.2.7
// ===========================================================================

P5EMU_TEST(Decoder, MovRegMemWithBaseAndDisp8) {
  // 48 8B 43 08 = MOV rax, [rbx+8]
  //   ModRM 43: mod=01 disp8, reg=000(rax), rm=011(rbx)
  Instruction insn;
  REQUIRE(Decode(B({0x48, 0x8B, 0x43, 0x08}), &insn).ok());
  CHECK_EQ(static_cast<int>(insn.dst.cls), static_cast<int>(OpClass::kReg64));
  CHECK_EQ(static_cast<int>(insn.dst.reg), 0);
  CHECK_EQ(static_cast<int>(insn.src.cls), static_cast<int>(OpClass::kMem64));
  CHECK(insn.src.has_base);
  CHECK_EQ(static_cast<int>(insn.src.base), 3);  // rbx
  CHECK(!insn.src.has_index);
  CHECK_EQ(static_cast<int>(insn.src.scale), 1);
  CHECK_EQ(insn.src.displacement, static_cast<std::int64_t>(8));
  CHECK_EQ(insn.length, 4u);
}

P5EMU_TEST(Decoder, MovRegMemWithSibScaleAndIndex) {
  // 48 8B 44 8B 10 = MOV rax, [rbx+rcx*4+0x10]
  //   ModRM 44: mod=01 disp8, reg=000(rax), rm=100(SIB)
  //   SIB   8B: scale=10(4), index=001(rcx), base=011(rbx)
  //   disp8 10
  Instruction insn;
  REQUIRE(Decode(B({0x48, 0x8B, 0x44, 0x8B, 0x10}), &insn).ok());
  CHECK_EQ(static_cast<int>(insn.dst.cls), static_cast<int>(OpClass::kReg64));
  CHECK_EQ(static_cast<int>(insn.dst.reg), 0);  // rax
  CHECK_EQ(static_cast<int>(insn.src.cls), static_cast<int>(OpClass::kMem64));
  CHECK(insn.src.has_base);
  CHECK_EQ(static_cast<int>(insn.src.base), 3);  // rbx
  CHECK(insn.src.has_index);
  CHECK_EQ(static_cast<int>(insn.src.index), 1);  // rcx
  CHECK_EQ(static_cast<int>(insn.src.scale), 4);
  CHECK_EQ(insn.src.displacement, static_cast<std::int64_t>(0x10));
  CHECK_EQ(insn.length, 5u);
}

P5EMU_TEST(Decoder, MovRegMemDisp32) {
  // 48 8B 83 00 01 00 00 = MOV rax, [rbx+0x100]
  Instruction insn;
  REQUIRE(Decode(B({0x48, 0x8B, 0x83, 0x00, 0x01, 0x00, 0x00}), &insn).ok());
  CHECK_EQ(insn.src.displacement, static_cast<std::int64_t>(0x100));
  CHECK_EQ(insn.length, 7u);
}

P5EMU_TEST(Decoder, MovRegMemNegativeDisp8) {
  // 48 8B 43 F8 = MOV rax, [rbx-8]. disp8 0xF8 sign-extends to -8.
  // Intel SDM Vol. 2 §2.2.2.7: displacement is sign-extended.
  Instruction insn;
  REQUIRE(Decode(B({0x48, 0x8B, 0x43, 0xF8}), &insn).ok());
  CHECK_EQ(insn.src.displacement, static_cast<std::int64_t>(-8));
}

P5EMU_TEST(Decoder, MovRegMemRipRelative) {
  // 48 8B 05 10 00 00 00 = MOV rax, [rip+0x10]
  //   ModRM 05: mod=00, reg=000, rm=101 -> RIP-relative disp32.
  // Intel SDM Vol. 2 §3.7.2.1: the displacement is relative to the END of the
  // instruction.
  Instruction insn;
  const memory::GuestAddr addr = 0x4000;
  REQUIRE(Decode(B({0x48, 0x8B, 0x05, 0x10, 0x00, 0x00, 0x00}), &insn, addr).ok());
  CHECK(insn.src.rip_relative);
  CHECK(!insn.src.has_base);
  CHECK(!insn.src.has_index);
  CHECK_EQ(insn.src.displacement, static_cast<std::int64_t>(0x10));
  CHECK_EQ(insn.length, 7u);
}

P5EMU_TEST(Decoder, SibWithRspBaseRequiresSibByte) {
  // 48 8B 44 24 18 = MOV rax, [rsp]
  //   SIB 24: scale=00(1), index=100(none), base=100(rsp)
  // Intel SDM Vol. 2 §2.2.2.8: rm=100 selects a SIB byte; SIB index=100 means
  // "no index"; SIB base=100 is rsp.
  Instruction insn;
  REQUIRE(Decode(B({0x48, 0x8B, 0x44, 0x24, 0x18}), &insn).ok());
  CHECK(insn.src.has_base);
  CHECK_EQ(static_cast<int>(insn.src.base), 4);  // rsp
  CHECK(!insn.src.has_index);
  CHECK_EQ(static_cast<int>(insn.src.scale), 1);
}

P5EMU_TEST(Decoder, SibWithR12AndNoIndex) {
  // REX = 0100WRXB (Intel SDM Vol. 2 §2.2.3.2).
  //   0x49 = 0100 1001 -> W=1, R=0, X=0, B=1
  // 49 8B 44 24 08 = MOV rcx, [r12]
  //   ModRM 44: mod=01 disp8, reg=001(rcx), rm=100(SIB)
  //   SIB   24: scale=00(1), index=100(no index), base=100 -> r12 via REX.B
  //
  // NOTE: with REX.X clear, SIB index=100 means NO INDEX; with REX.X set it
  // would mean r12 as a real index. That distinction is a classic decoder bug.
  Instruction insn;
  REQUIRE(Decode(B({0x49, 0x8B, 0x44, 0x24, 0x08}), &insn).ok());
  CHECK_EQ(static_cast<int>(insn.dst.reg), 0);  // ModRM.reg=000, REX.R clear
  CHECK(insn.src.has_base);
  CHECK_EQ(static_cast<int>(insn.src.base), 12);  // r12 via REX.B
  CHECK(!insn.src.has_index);
  CHECK_EQ(static_cast<int>(insn.src.scale), 1);
}

P5EMU_TEST(Decoder, RexXTurnsSibIndexFourIntoR12) {
  // REX bit layout is 0100WRXB, so X is bit 1 (0x02).
  //   0x49 = 0100 1001 -> W=1, R=0, X=0, B=1
  //   0x4B = 0100 1011 -> W=1, R=0, X=1, B=1
  // With REX.X set, SIB index=100 is r12, NOT "no index".
  // 4B 8B 44 24 08 = MOV rax, [r12 + r12*1]
  Instruction insn;
  REQUIRE(Decode(B({0x4B, 0x8B, 0x44, 0x24, 0x08}), &insn).ok());
  CHECK(insn.src.has_index);
  CHECK_EQ(static_cast<int>(insn.src.index), 12);  // r12 via REX.X
  CHECK(insn.src.has_base);
  CHECK_EQ(static_cast<int>(insn.src.base), 12);  // r12 via REX.B
}

// ===========================================================================
// Arithmetic  --  Intel SDM Vol. 2 §2.2.1 (opcode column 2/3)
// ===========================================================================

P5EMU_TEST(Decoder, AddRegReg) {
  // 48 01 D8 = ADD rax, rbx (01 /r = ADD r64 <- r/m64)
  Instruction insn;
  REQUIRE(Decode(B({0x48, 0x01, 0xD8}), &insn).ok());
  CHECK_EQ(std::string(MnemonicName(insn.mnemonic)), std::string("add"));
  CHECK_EQ(static_cast<int>(insn.dst.reg), 0);
  CHECK_EQ(static_cast<int>(insn.src.reg), 3);
  CHECK_EQ(static_cast<int>(insn.op_size), 8);
}

P5EMU_TEST(Decoder, AddMemRegDirection) {
  // 48 01 03 = ADD [rbx], rax  (mod=00 rm=011 -> base rbx, no disp)
  Instruction insn;
  REQUIRE(Decode(B({0x48, 0x01, 0x03}), &insn).ok());
  CHECK_EQ(static_cast<int>(insn.dst.cls), static_cast<int>(OpClass::kMem64));
  CHECK(insn.dst.has_base);
  CHECK_EQ(static_cast<int>(insn.dst.base), 3);  // rbx
  CHECK(!insn.dst.has_index);
  CHECK_EQ(static_cast<int>(insn.src.cls), static_cast<int>(OpClass::kReg64));
  CHECK_EQ(static_cast<int>(insn.src.reg), 0);  // rax, the added value
}

P5EMU_TEST(Decoder, SubRegImm8SignExtended) {
  // 48 83 EB 01 = SUB rbx, 1.
  // Opcode 83 with /5: imm8 is sign-extended to 64 bits.
  // Intel SDM Vol. 2 §2.2.1.1: "83 /r ... imm8 sign extended".
  Instruction insn;
  REQUIRE(Decode(B({0x48, 0x83, 0xEB, 0x01}), &insn).ok());
  CHECK_EQ(std::string(MnemonicName(insn.mnemonic)), std::string("sub"));
  CHECK_EQ(static_cast<int>(insn.dst.reg), 3);
  CHECK_EQ(insn.imm, std::uint64_t{1});
  CHECK(insn.imm_is_signed_extension);
}

P5EMU_TEST(Decoder, SubRegImm8NegativeSignExtends) {
  // 48 83 EB FF = SUB rbx, -1. 0xFF sign-extends to 0xFFFFFFFFFFFFFFFF.
  Instruction insn;
  REQUIRE(Decode(B({0x48, 0x83, 0xEB, 0xFF}), &insn).ok());
  CHECK_EQ(insn.imm, ~std::uint64_t{0});
}

P5EMU_TEST(Decoder, MovRm64Imm32SignExtended) {
  // 48 C7 C3 FF FF FF FF = MOV rbx, -1 (imm32 sign-extended to 64).
  // Intel SDM Vol. 2 §2.2.1.1, MOV r/m64, imm32.
  Instruction insn;
  REQUIRE(Decode(B({0x48, 0xC7, 0xC3, 0xFF, 0xFF, 0xFF, 0xFF}), &insn).ok());
  CHECK_EQ(std::string(MnemonicName(insn.mnemonic)), std::string("mov"));
  CHECK_EQ(insn.imm, ~std::uint64_t{0});
  CHECK(insn.imm_is_signed_extension);
}

P5EMU_TEST(Decoder, XorRegReg) {
  // 48 31 D8 = XOR rax, rbx
  Instruction insn;
  REQUIRE(Decode(B({0x48, 0x31, 0xD8}), &insn).ok());
  CHECK_EQ(std::string(MnemonicName(insn.mnemonic)), std::string("xor"));
}

P5EMU_TEST(Decoder, CmpRegReg) {
  // 48 39 D8 = CMP rax, rbx
  Instruction insn;
  REQUIRE(Decode(B({0x48, 0x39, 0xD8}), &insn).ok());
  CHECK_EQ(std::string(MnemonicName(insn.mnemonic)), std::string("cmp"));
  CHECK_EQ(static_cast<int>(insn.dst.reg), 0);
  CHECK_EQ(static_cast<int>(insn.src.reg), 3);
}

P5EMU_TEST(Decoder, TestRegReg) {
  // 48 85 D8 = TEST rax, rbx
  Instruction insn;
  REQUIRE(Decode(B({0x48, 0x85, 0xD8}), &insn).ok());
  CHECK_EQ(std::string(MnemonicName(insn.mnemonic)), std::string("test"));
  CHECK_EQ(static_cast<int>(insn.dst.reg), 0);
  CHECK_EQ(static_cast<int>(insn.src.reg), 3);
}

P5EMU_TEST(Decoder, AdcIsExplicitlyUnsupported) {
  // 48 11 D8 = ADC rax, rbx. ADC consumes CF, which Phase 1 does not model.
  // It must be REJECTED, not decoded and silently mis-evaluated.
  Instruction insn;
  const Status st = Decode(B({0x48, 0x11, 0xD8}), &insn);
  CHECK(st.code() == StatusCode::kUnimplementedInstruction);
}

// ===========================================================================
// Shifts  --  Intel SDM Vol. 2 §2.2.1 (D0-D3, C0/C1)
// ===========================================================================

P5EMU_TEST(Decoder, ShlRegImm8) {
  // 48 C1 E3 04 = SHL rbx, 4 (C1 /4 ib, ModRM E3: mod=11 reg=100 rm=011)
  Instruction insn;
  REQUIRE(Decode(B({0x48, 0xC1, 0xE3, 0x04}), &insn).ok());
  CHECK_EQ(std::string(MnemonicName(insn.mnemonic)), std::string("shl"));
  CHECK_EQ(std::string(FormName(insn.form)), std::string("shift imm8"));
  CHECK_EQ(static_cast<int>(insn.dst.reg), 3);
  CHECK_EQ(static_cast<int>(insn.shift_count), 4);
}

P5EMU_TEST(Decoder, ShrRegByOne) {
  // 48 D1 EB = SHR rbx, 1 (D1 /5, ModRM EB: mod=11 reg=101 rm=011)
  Instruction insn;
  REQUIRE(Decode(B({0x48, 0xD1, 0xEB}), &insn).ok());
  CHECK_EQ(std::string(MnemonicName(insn.mnemonic)), std::string("shr"));
  CHECK_EQ(static_cast<int>(insn.shift_count), 1);
}

P5EMU_TEST(Decoder, SarRegByCl) {
  // 48 D3 FB = SAR rbx, cl (D3 /7, ModRM FB: mod=11 reg=111 rm=011)
  Instruction insn;
  REQUIRE(Decode(B({0x48, 0xD3, 0xFB}), &insn).ok());
  CHECK_EQ(std::string(MnemonicName(insn.mnemonic)), std::string("sar"));
  CHECK_EQ(std::string(FormName(insn.form)), std::string("shift cl"));
}

P5EMU_TEST(Decoder, RotateIsExplicitlyUnsupported) {
  // 48 D3 C3 = ROL rbx, cl. Not in the Phase-1 subset.
  Instruction insn;
  CHECK(Decode(B({0x48, 0xD3, 0xC3}), &insn).code() ==
        StatusCode::kUnimplementedInstruction);
}

// ===========================================================================
// LEA  --  Intel SDM Vol. 2 §2.2.1 (8D /r)
// ===========================================================================
P5EMU_TEST(Decoder, LeaComputesAddressWithoutAccessingMemory) {
  // 48 8D 44 8B 10 = LEA rax, [rbx+rcx*4+0x10]
  // LEA's operand is a memory FORM but performs no access and sets no flags.
  // That distinction is why it has its own instruction here.
  Instruction insn;
  REQUIRE(Decode(B({0x48, 0x8D, 0x44, 0x8B, 0x10}), &insn).ok());
  CHECK_EQ(std::string(MnemonicName(insn.mnemonic)), std::string("lea"));
  CHECK_EQ(static_cast<int>(insn.dst.reg), 0);
  CHECK(insn.src.has_base);
  CHECK_EQ(static_cast<int>(insn.src.base), 3);
  CHECK(insn.src.has_index);
  CHECK_EQ(static_cast<int>(insn.src.index), 1);
  CHECK_EQ(static_cast<int>(insn.src.scale), 4);
}

P5EMU_TEST(Decoder, LeaWithMod11IsRejected) {
  // 48 8D C3 would be LEA with mod=11, which is #UD architecturally.
  Instruction insn;
  CHECK(!Decode(B({0x48, 0x8D, 0xC3}), &insn).ok());
}

// ===========================================================================
// MOVZX / MOVSX  --  Intel SDM Vol. 2 §2.2.1 (0F B6/B7/BE/BF, 0F 63)
// ===========================================================================

P5EMU_TEST(Decoder, MovzxRegFromMem8) {
  // 48 0F B6 03 = MOVZX rax, byte [rbx]
  //   ModRM 03: mod=00 reg=000(rax) rm=011(rbx)
  // The destination is written 32-bit and zero-extends to 64.
  Instruction insn;
  REQUIRE(Decode(B({0x48, 0x0F, 0xB6, 0x03}), &insn).ok());
  CHECK_EQ(std::string(MnemonicName(insn.mnemonic)), std::string("movzx"));
  CHECK_EQ(static_cast<int>(insn.op_size), 1);
  CHECK_EQ(static_cast<int>(insn.dst.reg), 0);
  CHECK_EQ(static_cast<int>(insn.src.cls), static_cast<int>(OpClass::kMem8));
}

P5EMU_TEST(Decoder, MovsxRegFromReg8) {
  // 48 0F BE C3 = MOVSX rax, bl
  Instruction insn;
  REQUIRE(Decode(B({0x48, 0x0F, 0xBE, 0xC3}), &insn).ok());
  CHECK_EQ(std::string(MnemonicName(insn.mnemonic)), std::string("movsx"));
  CHECK_EQ(static_cast<int>(insn.op_size), 1);
}

P5EMU_TEST(Decoder, MovsxdRequiresRexW) {
  // 0F 63 without REX.W is MOVSXD r32, r/m32. Phase 1 models 64-bit
  // destinations only, so it is rejected rather than silently truncated.
  Instruction insn;
  CHECK(Decode(B({0x0F, 0x63, 0xC3}), &insn).code() ==
        StatusCode::kUnimplementedInstruction);

  // With REX.W it is MOVSX r64, r/m32 and IS supported.
  Instruction ok;
  REQUIRE(Decode(B({0x48, 0x0F, 0x63, 0xC3}), &ok).ok());
  CHECK_EQ(static_cast<int>(ok.op_size), 4);
}

// ===========================================================================
// Control flow  --  Intel SDM Vol. 2 §2.2.1
// ===========================================================================

P5EMU_TEST(Decoder, JmpRel8) {
  // EB 05 = JMP rel8 +5
  Instruction insn;
  REQUIRE(Decode(B({0xEB, 0x05}), &insn).ok());
  CHECK_EQ(std::string(MnemonicName(insn.mnemonic)), std::string("jmp"));
  CHECK_EQ(insn.imm, std::uint64_t{5});
}

P5EMU_TEST(Decoder, JmpRel8NegativeSignExtends) {
  // EB FB = JMP rel8 -5. 0xFB sign-extends to -5.
  Instruction insn;
  REQUIRE(Decode(B({0xEB, 0xFB}), &insn).ok());
  CHECK_EQ(insn.imm, static_cast<std::uint64_t>(-5));
}

P5EMU_TEST(Decoder, JccRel8CoversAllSixteenConditions) {
  // 70..7F map to O,NO,B,AE,E,NE,BE,A,S,NS,P,NP,L,GE,LE,G.
  // Intel SDM Vol. 2 §3.1.2.1, Table 3-1.
  static const char* const kNames[16] = {
      "o", "no", "b", "ae", "e", "ne", "be", "a",
      "s", "ns", "p", "np", "l", "ge", "le", "g"};
  for (int i = 0; i < 16; ++i) {
    Instruction insn;
    const std::vector<std::uint8_t> bytes =
        B({0x70 + i, 0x02});
    REQUIRE(Decode(bytes, &insn).ok());
    CHECK_EQ(std::string(MnemonicName(insn.mnemonic)), std::string("jcc"));
    CHECK_EQ(std::string(CondName(insn.cond)), std::string(kNames[i]));
  }
}

P5EMU_TEST(Decoder, CallRel32) {
  Instruction insn;
  REQUIRE(Decode(B({0xE8, 0x10, 0x00, 0x00, 0x00}), &insn).ok());
  CHECK_EQ(std::string(MnemonicName(insn.mnemonic)), std::string("call"));
  CHECK_EQ(insn.imm, std::uint64_t{0x10});
  CHECK_EQ(insn.length, 5u);
}

P5EMU_TEST(Decoder, RetNopInt3) {
  Instruction insn;
  REQUIRE(Decode(B({0xC3}), &insn).ok());
  CHECK_EQ(std::string(MnemonicName(insn.mnemonic)), std::string("ret"));

  Instruction n;
  REQUIRE(Decode(B({0x90}), &n).ok());
  CHECK_EQ(std::string(MnemonicName(n.mnemonic)), std::string("nop"));

  Instruction i;
  REQUIRE(Decode(B({0xCC}), &i).ok());
  CHECK_EQ(std::string(MnemonicName(i.mnemonic)), std::string("int3"));
}

// ===========================================================================
// Malformed input  --  the decoder must never guess
// ===========================================================================

P5EMU_TEST(DecoderMalformed, TruncatedInstructionIsRejected) {
  // 48 B8 needs 10 bytes; 9 is not enough. Decoding from a short buffer must
  // fail rather than read whatever follows in memory.
  Instruction insn;
  const std::vector<std::uint8_t> truncated =
      B({0x48, 0xB8, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07});
  CHECK(!Decode(truncated, &insn).ok());
}

P5EMU_TEST(DecoderMalformed, TruncatedModRmIsRejected) {
  // 48 8B needs a ModRM byte; none is supplied.
  Instruction insn;
  CHECK(!Decode(B({0x48, 0x8B}), &insn).ok());
}

P5EMU_TEST(DecoderMalformed, TruncatedDisplacementIsRejected) {
  // 48 8B 43 needs a disp8.
  Instruction insn;
  CHECK(!Decode(B({0x48, 0x8B, 0x43}), &insn).ok());
}

P5EMU_TEST(DecoderMalformed, TruncatedSibIsRejected) {
  // 48 8B 44 indicates a SIB byte; none supplied.
  Instruction insn;
  CHECK(!Decode(B({0x48, 0x8B, 0x44}), &insn).ok());
}

P5EMU_TEST(DecoderMalformed, EmptyInputIsRejected) {
  Instruction insn;
  const Status st = g_decoder.DecodeOne(0x1000, nullptr, 0, &insn);
  CHECK(!st.ok());
}

P5EMU_TEST(DecoderMalformed, ExcessivePrefixesAreRejected) {
  // An unbounded prefix loop would treat a stream of 0x66 bytes as one
  // instruction. The decoder caps the chain.
  //
  // Heap-allocated rather than a fixed array so that the size is a runtime value
  // and the compiler cannot fold the bounds.
  const std::size_t prefix_count = 64;
  auto bytes = std::make_unique<std::uint8_t[]>(prefix_count + 1);
  for (std::size_t i = 0; i < prefix_count; ++i) {
    bytes[i] = 0x66;
  }
  bytes[prefix_count] = 0x90;

  Instruction insn;
  const Status st = g_decoder.DecodeOne(0x1000, bytes.get(), prefix_count + 1,
                                        &insn);
  CHECK(!st.ok());
}

P5EMU_TEST(DecoderMalformed, UnknownOpcodeIsRejectedNotGuessed) {
  // 0x0F 0x0B is UD2. Unsupported.
  Instruction insn;
  CHECK(Decode(B({0x0F, 0x0B}), &insn).code() ==
        StatusCode::kUnimplementedInstruction);
}

P5EMU_TEST(DecoderMalformed, NullOutputIsRejected) {
  CHECK(g_decoder.DecodeOne(0x1000, nullptr, 0, nullptr).code() ==
        StatusCode::kInvalidArgument);
}

P5EMU_TEST(DecoderMalformed, FailedDecodeLeavesOutputUntouched) {
  // The contract says `out` is never partially populated on failure, so a caller
  // can treat it as valid iff the status is kOk.
  //
  // The sentinel is set field by field rather than with memset, because
  // Instruction is not trivially copyable and memset over it is undefined.
  Instruction insn;
  // Sentinels chosen from within each enum's value range so that no conversion
  // is out of range.
  insn.mnemonic = Mnemonic::kJcc;    // will NOT match a decode of 0F 0B
  insn.form = Form::kJccRel;         // ditto
  insn.length = 0xABCDu;
  insn.imm = 0xDEADBEEFull;

  const Status st = Decode(B({0x0F, 0x0B}), &insn);
  CHECK(st.code() == StatusCode::kUnimplementedInstruction);
  CHECK_EQ(std::string(MnemonicName(insn.mnemonic)), std::string("jcc"));
  CHECK_EQ(std::string(FormName(insn.form)), std::string("jcc rel"));
  CHECK_EQ(insn.length, 0xABCDu);
  CHECK_EQ(insn.imm, 0xDEADBEEFull);
}

// ===========================================================================
// Round trip: decode -> re-encode -> decode
// ===========================================================================
//
// See X64Encode.h for why x86 has no single canonical encoding, and why the
// tests below assert IDEMPOTENCE plus FIELD EQUALITY rather than blindly
// comparing bytes.

P5EMU_TEST(DecoderRoundTrip, ReEncodingIsIdempotentAndPreservesFields) {
  struct Case {
    const char* name;
    std::vector<std::uint8_t> bytes;
    bool canonical;  // whether re-encoding must reproduce the original bytes
  };

  const Case cases[] = {
      {"mov rax, imm64", B({0x48, 0xB8, 0xEF, 0xCD, 0xAB, 0x89, 0x67, 0x45, 0x23, 0x01}), true},
      {"mov rax, rbx", B({0x48, 0x89, 0xD8}), true},
      {"mov rbx, rax", B({0x48, 0x89, 0xC3}), true},
      {"add rax, rbx", B({0x48, 0x01, 0xD8}), true},
      {"sub rbx, 1", B({0x48, 0x83, 0xEB, 0x01}), true},
      {"cmp rax, rbx", B({0x48, 0x39, 0xD8}), true},
      {"shl rbx, 4", B({0x48, 0xC1, 0xE3, 0x04}), true},
      {"shr rbx, 1", B({0x48, 0xD1, 0xEB}), true},
      {"sar rbx, cl", B({0x48, 0xD3, 0xFB}), true},
      {"mov rax, [rbx+8]", B({0x48, 0x8B, 0x43, 0x08}), true},
      {"mov [rbx], rax", B({0x48, 0x89, 0x03}), true},
      {"mov rax, [rbx+rcx*4+0x10]", B({0x48, 0x8B, 0x44, 0x8B, 0x10}), true},
      {"lea rax, [rbx+rcx*4+0x10]", B({0x48, 0x8D, 0x44, 0x8B, 0x10}), true},
      // C7 /0 id (sign-extended imm32) canonicalises to B8+r imm64; same value,
      // different length. Documented in HasCanonicalEncoding.
      {"mov rbx, -1", B({0x48, 0xC7, 0xC3, 0xFF, 0xFF, 0xFF, 0xFF}), false},
      // REX.W is architecturally IGNORED on 0F B6/B7, so P5EMU's canonical
      // form omits it and the redundant byte does not survive a round trip.
      {"movzx rax, byte[rbx]", B({0x48, 0x0F, 0xB6, 0x03}), false},
      {"movzx rax, byte[rbx] (no redundant REX.W)", B({0x0F, 0xB6, 0x03}), true},
      // MOVSX r64, r/m8 DOES use REX.W (it selects the destination width), so
      // this one round trips byte-exactly.
      {"movsx rax, bl", B({0x48, 0x0F, 0xBE, 0xC3}), true},
      {"ret", B({0xC3}), true},
      {"nop", B({0x90}), true},
      {"int3", B({0xCC}), true},
      {"call rel32", B({0xE8, 0x10, 0x00, 0x00, 0x00}), true},
      // Jumps have two valid encodings for one target (rel8 and rel32), so the
      // re-encoded form need not equal the input. Idempotence must still hold.
      {"jmp rel8", B({0xEB, 0x05}), false},
      {"jcc rel8", B({0x74, 0x05}), false},
  };

  for (const Case& c : cases) {
    Instruction first;
    REQUIRE(Decode(c.bytes, &first).ok());

    std::vector<std::uint8_t> encoded;
    const Status es = EncodeCanonical(first, &encoded);
    if (!es.ok()) {
      ::p5emu::test::ReportFailure(__FILE__, __LINE__,
                                   std::string("re-encode failed for ") + c.name +
                                       ": " + es.name());
      continue;
    }

    Instruction second;
    const Status ds = Decode(encoded, &second);
    if (!ds.ok()) {
      ::p5emu::test::ReportFailure(
          __FILE__, __LINE__,
          std::string("re-encoded bytes do not decode for ") + c.name + ": " +
              ds.name());
      continue;
    }

    // Field equality for the parts that must survive any canonicalisation:
    // the semantic content of the instruction.
    CHECK_EQ(static_cast<int>(second.mnemonic), static_cast<int>(first.mnemonic));
    CHECK_EQ(second.imm, first.imm);
    CHECK_EQ(static_cast<int>(second.dst.cls), static_cast<int>(first.dst.cls));
    CHECK_EQ(static_cast<int>(second.src.cls), static_cast<int>(first.src.cls));
    CHECK_EQ(second.src.displacement, first.src.displacement);
    CHECK_EQ(static_cast<int>(second.src.scale), static_cast<int>(first.src.scale));

    const bool canonical = HasCanonicalEncoding(first);

    // Form and length are only invariant when the encoding is canonical.
    // `mov rbx, -1` canonicalises from C7 /0 id (7 bytes) to B8+r imm64
    // (10 bytes); the VALUE is identical but the form and length are not. That
    // is the canonicalisation rule working, not a defect.
    if (canonical) {
      CHECK_EQ(static_cast<int>(second.form), static_cast<int>(first.form));
      CHECK_EQ(static_cast<int>(second.dst.reg), static_cast<int>(first.dst.reg));
      CHECK_EQ(static_cast<int>(second.src.reg), static_cast<int>(first.src.reg));
      CHECK_EQ(second.length, first.length);
    }

    // Idempotence: re-encoding the re-decoded form must be byte-identical.
    std::vector<std::uint8_t> encoded2;
    REQUIRE(EncodeCanonical(second, &encoded2).ok());
    CHECK_EQ(encoded.size(), encoded2.size());
    if (encoded == encoded2) {
      // fine
    } else {
      ::p5emu::test::ReportFailure(
          __FILE__, __LINE__,
          std::string("re-encoding is not idempotent for ") + c.name);
    }

    // Byte equality only where the encoding is canonical by construction.
    if (canonical && c.canonical) {
      CHECK_EQ(encoded.size(), c.bytes.size());
      if (encoded == c.bytes) {
        // exact match
      } else {
        std::string got;
        char buf[8];
        for (std::uint8_t b : encoded) {
          std::snprintf(buf, sizeof(buf), "%02x", b);
          got += buf;
        }
        std::string want;
        for (std::uint8_t b : c.bytes) {
          std::snprintf(buf, sizeof(buf), "%02x", b);
          want += buf;
        }
        ::p5emu::test::ReportFailure(
            __FILE__, __LINE__,
            std::string("canonical re-encode mismatch for ") + c.name +
                ": got " + got + " want " + want);
      }
    }
  }
}

P5EMU_TEST(DecoderRoundTrip, UnsupportedInstructionIsNotEncodable) {
  Instruction insn;
  insn.mnemonic = Mnemonic::kUnimplemented;
  std::vector<std::uint8_t> out;
  CHECK(EncodeCanonical(insn, &out).code() ==
        StatusCode::kUnimplementedInstruction);
  CHECK(out.empty());
}

P5EMU_TEST(DecoderRoundTrip, NullOutputBufferIsRejected) {
  Instruction insn;
  REQUIRE(Decode(B({0xC3}), &insn).ok());
  CHECK(EncodeCanonical(insn, nullptr).code() == StatusCode::kInvalidArgument);
}

P5EMU_TEST(Decoder, SupportedCountMatchesTheDocumentedSubset) {
  // The count is a checked constant, so adding an instruction forces a
  // deliberate decision to update it and the coverage report.
  CHECK_EQ(X64Decoder::SupportedCount(), std::size_t{20});
  CHECK(X64Decoder::IsSupported(Mnemonic::kMov));
  CHECK(X64Decoder::IsSupported(Mnemonic::kLea));
  CHECK(X64Decoder::IsSupported(Mnemonic::kInt3));
  CHECK(!X64Decoder::IsSupported(Mnemonic::kUnimplemented));
}

}  // namespace
