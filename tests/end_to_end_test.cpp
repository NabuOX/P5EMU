// SPDX-License-Identifier: LicenseRef-P5EMU-Proprietary
//
// ============================================================================
// WHOLE-BLOCK END-TO-END TEST
// ============================================================================
//
// This is the Phase-1 execution milestone. It runs the COMPLETE pipeline:
//
//     raw guest bytes
//        -> X64Decoder
//        -> TranslateToIr (IR builder)
//        -> ValidateIr
//        -> ReferenceInterpreter
//        -> known architectural result
//
// There is NO manually constructed IR anywhere in this file. Every program
// starts as literal x86-64 machine code, decoded byte by byte. That is the
// point: a hand-written IR fixture would test the interpreter but not the
// decoder, and the decoder is where encoding bugs live.
//
// ============================================================================
// EXPECTED-VALUE PROVENANCE
// ============================================================================
//
// Every expected register value and memory value below is derived by hand from
// the Intel 64 and IA-32 Architectures Software Developer's Manual:
//
//   Vol. 2 §2.2.1    Instruction Reference (MOV, ADD, CMP, Jcc, LEA)
//   Vol. 2 §2.2.2.7  ModR/M Encoding (addressing modes)
//   Vol. 2 §3.1.2    Flags Register
//   Vol. 2 §3.1.2.1  Condition Codes, Table 3-1
//
// No disassembler, assembler, or other emulator was used to derive the byte
// sequences or the expected results. Each program is annotated with its manual
// derivation, instruction by instruction.
//
// ============================================================================

#include <cstdio>
#include <string>
#include <vector>

#include "FixtureProvenance.h"
#include "TestHarness.h"
#include "p5emu/cpu/LinearGuestMemory.h"
#include "p5emu/cpu/ReferenceInterpreter.h"
#include "p5emu/cpu/TranslateToIr.h"
#include "p5emu/cpu/X64Decoder.h"

namespace {

using namespace p5emu;         // NOLINT
using namespace p5emu::cpu;     // NOLINT
using namespace p5emu::ir;      // NOLINT
using namespace p5emu::memory;  // NOLINT
using namespace p5emu::test;   // NOLINT
using namespace p5emu::x64;    // NOLINT

constexpr GuestAddr kCodeBase = 0x0001'0000ull;
constexpr GuestAddr kDataBase = 0x0004'0000ull;
constexpr GuestSize kMappedSize = 0x0010'0000ull;

std::vector<std::uint8_t> B(std::initializer_list<int> v) {
  std::vector<std::uint8_t> out;
  out.reserve(v.size());
  for (int b : v) {
    out.push_back(static_cast<std::uint8_t>(b));
  }
  return out;
}

// The full pipeline, with each stage checked so a failure names the stage that
// broke rather than just "wrong result".
struct PipelineOutcome {
  bool decoded = false;
  bool translated = false;
  bool validated = false;
  bool executed = false;
  Status decode_status;
  Status translate_status;
  Status validate_status;
  Status execute_status;
  TerminationReason reason = TerminationReason::kRunning;
  ir::Program program;
  CpuState state;
  std::uint64_t op_count = 0;
  std::size_t instruction_count = 0;
  std::string ir_text;
};

PipelineOutcome RunPipeline(const std::vector<std::uint8_t>& code,
                            const CpuState& initial,
                            LinearGuestMemory& memory) {
  PipelineOutcome out;
  out.state = initial;

  // Stage 1: decode. Decode every instruction up front so that a decode
  // failure is reported as such rather than as a mysterious translate failure.
  const X64Decoder decoder;
  GuestAddr pc = kCodeBase;
  GuestAddr limit = kCodeBase + code.size();
  while (pc < limit) {
    Instruction insn;
    // The buffer is indexed by the OFFSET from the code base, not by the
    // absolute guest address: code.data() points at the first BYTE of the
    // program, not at guest address 0x1000.
    const std::size_t offset = static_cast<std::size_t>(pc - kCodeBase);
    const Status st = decoder.DecodeOne(pc, code.data() + offset,
                                        static_cast<std::size_t>(limit - pc),
                                        &insn);
    if (!st.ok()) {
      out.decode_status = st;
      return out;
    }
    ++out.instruction_count;
    pc += insn.length;
  }
  out.decoded = true;

  // Stage 2: translate (this internally re-decodes and lowers).
  std::vector<GuestAddr> addresses;
  out.translate_status = TranslateProgram(kCodeBase, code.data(), code.size(),
                                          "end-to-end", &out.program, &addresses);
  if (!out.translate_status.ok()) {
    return out;
  }
  out.translated = true;

  // Stage 3: validate. TranslateProgram already validates, but this stage is
  // checked explicitly so the pipeline's contract is visible in the test.
  out.validate_status = ValidateIr(out.program);
  if (!out.validate_status.ok()) {
    return out;
  }
  out.validated = true;
  out.ir_text = Dump(out.program);

  // Stage 4: execute.
  ReferenceInterpreter interp(10000);
  const ExecutionResult er = interp.Execute(out.program, &out.state, memory);
  out.execute_status = er.status;
  out.reason = er.reason;
  out.op_count = er.executed_ops;
  out.executed = true;
  return out;
}

}  // namespace

// ===========================================================================
// Test 1: the full example shape from the Phase-1 objective
//
//   mov  -> arithmetic -> memory store -> memory load
//        -> comparison -> conditional control flow -> known result
// ===========================================================================

P5EMU_TEST(EndToEnd, MovArithStoreLoadCompareBranch) {
  //
  // Guest program (Intel SDM Vol. 2 §2.2.1 for each instruction):
  //
  //   1000: 48 C7 C0 05 00 00 00     mov  rax, 5
  //                                 C7 /0 id, REX.W -> MOV r/m64, imm32
  //                                 Intel SDM Vol. 2 §2.2.1.1
  //   1007: 48 01 D8                add  rax, rbx
  //                                 01 /r, REX.W -> ADD r/m64, r64
  //   100a: 48 89 05 00 40 00 00     mov  [rip+0x4000], rax
  //                                 89 /r with RIP-relative ModRM (mod=00,
  //                                 rm=101 -> RIP-relative, SDM §2.2.2.7).
  //                                 The RIP base is the address of the NEXT
  //                                 instruction: 0x100a + 7 = 0x1011.
  //                                 0x1011 + 0x4000 = 0x5011.  Hmm, that is
  //                                 not the data base this test uses, so the
  //                                 program sets up its own pointer instead.
  //                                 See the NEXT program for the RIP-relative
  //                                 case; this one uses a register pointer.
  //
  // Rewritten with an explicit pointer so the address is exact and the
  // derivation is checkable:
  //
  //   1000: 48 C7 C0 05 00 00 00     mov  rax, 5
  //   1007: 48 C7 C1 0A 00 00 00     mov  rcx, 10
  //   100e: 48 01 C8                add  rax, rcx        ; rax = 15
  //   1011: 48 C7 C2 00 40 00 00     mov  rdx, 0x4000     ; the data address
  //   1018: 48 89 02                mov  [rdx], rax       ; store 15
  //   101b: 48 8B 32                mov  rsi, [rdx]       ; load 15
  //   101e: 48 39 C6                cmp  rsi, 15
  //   1021: 74 08                   je   +8 -> 0x102b
  //   1023: 48 C7 C7 FF FF FF FF     mov  rdi, -1         ; NOT taken
  //   102a: EB 0B                   jmp  +11 -> 0x1037
  //   102c: 90                       nop
  //   102d: 48 C7 C7 63 00 00 00     mov  rdi, 99         ; poison, skipped
  //   1034: EB 00                   jmp  +0
  //   1036: C3                       ret
  //
  // Hmm, hand-assembling that with correct branch targets is error-prone. The
  // program below is written with EXPLICIT, VERIFIED offsets instead.
  const auto code = B({
      // +0x00  mov rax, 5
      0x48, 0xC7, 0xC0, 0x05, 0x00, 0x00, 0x00,
      // +0x07  mov rcx, 10
      0x48, 0xC7, 0xC1, 0x0A, 0x00, 0x00, 0x00,
      // +0x0E  add rax, rcx
      0x48, 0x01, 0xC8,
      // +0x11  mov rdx, kDataBase
      0x48, 0xC7, 0xC2, 0x00, 0x00, 0x04, 0x00,
      // +0x18  mov [rdx], rax
      0x48, 0x89, 0x02,
      // +0x1B  mov rsi, [rdx]
      0x48, 0x8B, 0x32,
      // +0x1E  cmp rsi, 15
      0x48, 0x83, 0xFE, 0x0F,
      // +0x22  je +0x12 -> +0x36 (the ret)
      0x74, 0x12,
      // +0x24  mov rdi, -1  (not taken)
      0x48, 0xC7, 0xC7, 0xFF, 0xFF, 0xFF, 0xFF,
      // +0x2B  jmp +0x09 -> +0x36 (the ret)
      0xEB, 0x09,
      // +0x2D  nop  (skipped)
      0x90,
      // +0x2E  mov rdi, 99  (skipped)
      0x48, 0xC7, 0xC7, 0x63, 0x00, 0x00, 0x00,
      // +0x35  nop  (skipped)
      0x90,
      // +0x36  ret
      0xC3,
  });

  CpuState initial;
  initial.rbx = 0;
  initial.rdi = 0xDEAD;

  auto memory = LinearGuestMemory::MakeCodeAndData(0, kMappedSize);
  const PipelineOutcome out = RunPipeline(code, initial, memory);

  if (!out.decoded) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "decode failed: %s", out.decode_status.name());
    ReportFailure(__FILE__, __LINE__, buf);
    return;
  }
  if (!out.translated) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "translate failed: %s",
                  out.translate_status.name());
    ReportFailure(__FILE__, __LINE__, buf);
    return;
  }
  if (!out.validated) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "ValidateIr rejected the program: %s",
                  out.validate_status.name());
    ReportFailure(__FILE__, __LINE__, buf);
    return;
  }

  CHECK(out.executed);
  CHECK_EQ(out.reason, TerminationReason::kReturn);
  CHECK(out.execute_status.ok());

  // Expected architectural results, derived by hand:
  //   mov rax, 5          -> rax = 5
  //   mov rcx, 10         -> rcx = 10
  //   add rax, rcx        -> rax = 15          (5 + 10)
  //   mov [rdx], rax        -> [kDataBase] = 15
  //   mov rsi, [rdx]      -> rsi = 15
  //   cmp rsi, 15         -> ZF = 1, SF = 0, CF = 0, OF = 0, PF = 1
  //   je                  -> taken, because ZF = 1
  CHECK_EQ(out.state.rax, std::uint64_t{15});
  CHECK_EQ(out.state.rcx, std::uint64_t{10});
  CHECK_EQ(out.state.rsi, std::uint64_t{15});
  CHECK_EQ(out.state.rdx, kDataBase);

  // The branch WAS taken, so rdi kept its initial value; the two paths that
  // write rdi were both skipped.
  CHECK_EQ(out.state.rdi, std::uint64_t{0xDEAD});

  // Memory holds the stored value.
  std::uint64_t stored = 0;
  REQUIRE(memory.ReadData(kDataBase, 8, &stored).ok());
  CHECK_EQ(stored, std::uint64_t{15});

  // CMP 15, 15 sets ZF and PF (0 has an even number of set bits) and clears
  // SF, CF and OF. Provenance: Intel SDM Vol. 2 §3.1.2.
  CHECK((out.state.eflags & eflags::kZf) != 0);
  CHECK((out.state.eflags & eflags::kCf) == 0);
  CHECK((out.state.eflags & eflags::kSf) == 0);
  CHECK((out.state.eflags & eflags::kOf) == 0);
  CHECK((out.state.eflags & eflags::kPf) != 0);
}

P5EMU_TEST(EndToEnd, BranchNotTakenPathIsExercised) {
  // Same shape, but the comparison fails so the OTHER branch is taken.
  // This proves both arms of the conditional are real and that the harness is
  // not simply always taking the taken-path.
  //
  //   mov rax, 5
  //   cmp rax, 99         ; ZF = 0
  //   je  poison           ; NOT taken
  //   mov rdi, 0x2A
  //   ret
  //   poison: mov rdi, 0xBAD
  //   ret
  const auto code = B({
      0x48, 0xC7, 0xC0, 0x05, 0x00, 0x00, 0x00,  // +00 mov rax, 5
      0x48, 0x83, 0xF8, 0x63,                    // +07 cmp rax, 99
      0x74, 0x09,                                // +0B je +9 -> +0x16 (the poison block)
      0x48, 0xC7, 0xC7, 0x2A, 0x00, 0x00, 0x00,  // +0D mov rdi, 42
      0xC3,                                       // +14 ret
      0x90,                                       // +15 nop (pad)
      0x48, 0xC7, 0xC7, 0xAD, 0x0B, 0x00, 0x00,  // +16 mov rdi, 0xBAD
      0xC3,                                       // +1D ret
  });

  CpuState initial;
  initial.rdi = 0xDEAD;
  auto memory = LinearGuestMemory::MakeCodeAndData(0, kMappedSize);
  const PipelineOutcome out = RunPipeline(code, initial, memory);

  REQUIRE(out.decoded);
  REQUIRE(out.translated);
  REQUIRE(out.validated);
  CHECK_EQ(out.reason, TerminationReason::kReturn);

  // 5 != 99, so ZF = 0 and the NOT-taken path runs.
  CHECK_EQ(out.state.rdi, std::uint64_t{42});
  CHECK((out.state.eflags & eflags::kZf) == 0);
}

P5EMU_TEST(EndToEnd, RipRelativeAddressingUsesTheNextInstructionAddress) {
  // Intel SDM Vol. 2 §3.7.2.1: a RIP-relative displacement is added to the
  // address of the NEXT instruction, not the current one.
  //
  //   +00: 48 8B 05 FA 0F 00 00   mov rax, [rip+0x0FFA]
  //   length 7, so the next instruction is at +0x07.
  //   0x1007 + 0x0FFA = 0x2001
  //   +0D: C3                      ret
  //
  // If the decoder used the CURRENT instruction's address the result would be
  // 0x1FFA, seven bytes lower -- a bug this test is built to catch.
  const auto code = B({
      0x48, 0x8B, 0x05, 0xFA, 0x0F, 0x00, 0x00,  // +00 mov rax, [rip+0x0FFA]
      0xC3,                                       // +07 ret
  });

  CpuState initial;
  auto memory = LinearGuestMemory::MakeCodeAndData(0, kMappedSize);
  const GuestAddr expected_addr = kCodeBase + 0x07 + 0x0FFA;  // 0x2001

  std::uint64_t marker = 0xFEED'FACE'CAFE'BEEFull;
  // Write a decoy FIRST, at the WRONG address an implementation that uses the
  // instruction's start instead of its end would compute (seven bytes earlier).
  // The marker is written afterwards because the decoy's eight bytes would
  // otherwise overlap the marker's first byte and corrupt it.
  REQUIRE(memory.WriteData(expected_addr - 7, 8, 0x1111'1111'1111'1111ull).ok());
  REQUIRE(memory.WriteData(expected_addr, 8, marker).ok());

  const PipelineOutcome out = RunPipeline(code, initial, memory);

  REQUIRE(out.decoded);
  REQUIRE(out.translated);
  REQUIRE(out.validated);
  CHECK_EQ(out.reason, TerminationReason::kReturn);
  CHECK_EQ(out.state.rax, marker);
}

P5EMU_TEST(EndToEnd, LeaComputesAnAddressWithoutTouchingMemory) {
  // Intel SDM Vol. 2 §2.2.1 LEA: computes an effective address and stores it;
  // it performs NO memory access and sets NO flags. That distinction is the
  // reason LEA is separate from MOV in the IR.
  //
  //   +00: 48 8D 44 8B 10   lea rax, [rbx+rcx*4+0x10]
  //   +05: C3               ret
  const auto code = B({
      0x48, 0x8D, 0x44, 0x8B, 0x10,  // +00 lea rax, [rbx+rcx*4+0x10]
      0xC3,                            // +05 ret
  });

  CpuState initial;
  initial.rbx = 0x1000;
  initial.rcx = 7;
  initial.eflags = 0xFFFF;  // must be UNCHANGED by LEA

  auto memory = LinearGuestMemory::MakeCodeAndData(0, kMappedSize);
  const PipelineOutcome out = RunPipeline(code, initial, memory);

  REQUIRE(out.decoded);
  REQUIRE(out.translated);
  REQUIRE(out.validated);
  CHECK_EQ(out.reason, TerminationReason::kReturn);

  // base + index*scale + displacement = 0x1000 + 7*4 + 0x10 = 0x102C
  CHECK_EQ(out.state.rax, std::uint64_t{0x1000 + 7 * 4 + 0x10});
  // LEA sets no flags.
  CHECK_EQ(out.state.eflags, std::uint64_t{0xFFFF});
}

P5EMU_TEST(EndToEnd, ThirtyTwoBitRegisterWriteZeroExtends) {
  // Intel SDM Vol. 2 §2.2.1: a write to a 32-bit register ZERO-EXTENDS to 64.
  // This is a genuine 64-bit-mode rule, not a 32-bit-mode one, and it is a
  // classic source of divergence between backends.
  //
  //   +00: B8 FF FF FF FF   mov eax, 0xFFFFFFFF   (32-bit destination)
  //   +05: 48 89 C3         mov rbx, rax          (capture all 64 bits)
  //   +08: C3               ret
  const auto code = B({
      0xB8, 0xFF, 0xFF, 0xFF, 0xFF,  // +00 mov eax, -1
      0x48, 0x89, 0xC3,                // +05 mov rbx, rax
      0xC3,                            // +08 ret
  });

  CpuState initial;
  auto memory = LinearGuestMemory::MakeCodeAndData(0, kMappedSize);
  const PipelineOutcome out = RunPipeline(code, initial, memory);

  REQUIRE(out.decoded);
  REQUIRE(out.translated);
  REQUIRE(out.validated);
  CHECK_EQ(out.reason, TerminationReason::kReturn);

  // The upper 32 bits must be ZERO, not sign-extended. If a backend used
  // MOVSXD semantics here, rbx would be 0xFFFFFFFFFFFFFFFF.
  CHECK_EQ(out.state.rax, std::uint64_t{0x0000'0000'FFFF'FFFFull});
  CHECK_EQ(out.state.rbx, std::uint64_t{0x0000'0000'FFFF'FFFFull});
  CHECK(out.state.rax != ~std::uint64_t{0});
}

P5EMU_TEST(EndToEnd, CallAndReturnThroughTheStack) {
  // CALL pushes the return address and transfers; RET pops it.
  // Intel SDM Vol. 2 §2.2.1 (CALL/RET).
  //
  //   +00: 48 C7 C0 05 00 00 00   mov rax, 5
  //   +07: E8 0E 00 00 00         call +0x0E -> +0x14
  //   +0C: 48 83 C4 08           add  rsp, 8      (balance the stack)
  //   +10: 90                     nop
  //   +11: C3                     ret
  //   ...
  //   +14: 48 01 D8               add rax, rbx     ; the callee body
  //   +17: C3                     ret
  const auto code = B({
      0x48, 0xC7, 0xC0, 0x05, 0x00, 0x00, 0x00,  // +00 mov rax, 5
      0xE8, 0x08, 0x00, 0x00, 0x00,                // +07 call +8 -> +0x14
      0x48, 0x83, 0xC4, 0x08,                      // +0C add rsp, 8
      0x90,                                        // +10 nop
      0xC3,                                        // +11 ret
      0x90,                                        // +12 nop
      0x90,                                        // +13 nop
      0x48, 0x01, 0xD8,                             // +14 add rax, rbx
      0xC3,                                        // +17 ret
  });

  CpuState initial;
  initial.rsp = 0x0008'0000ull;
  initial.rbx = 100;
  // Seed the stack so RET's target is inside the mapped region.
  auto memory = LinearGuestMemory::MakeCodeAndData(0, kMappedSize);
  REQUIRE(memory.WriteData(initial.rsp, 8, 0x0000'9000ull).ok());

  const PipelineOutcome out = RunPipeline(code, initial, memory);

  REQUIRE(out.decoded);
  REQUIRE(out.translated);
  REQUIRE(out.validated);

  // The callee ran: rax = 5 + 100 = 105.
  CHECK_EQ(out.state.rax, std::uint64_t{105});
  // CALL pushed 8 bytes and the caller's `add rsp, 8` popped it, so RSP is back
  // where it started.
  CHECK_EQ(out.state.rsp, initial.rsp);
}

P5EMU_TEST(EndToEnd, LoopAccumulatorReachesAKnownValue) {
  // A short counted loop, which exercises a BACKWARD branch -- the case a
  // linear one-pass lowering cannot handle and where a hang would show up.
  //
  //   rax = 0            (accumulator)
  //   rcx = 10           (counter)
  //   rbx = 0x5000       (data pointer)
  // loop:
  //   add rax, rcx       ; rax += rcx
  //   sub rcx, 1
  //   mov [rbx], rax
  //   add rbx, 8
  //   cmp rcx, 0
  //   jne loop
  //   ret
  //
  // `sub rcx, 1` is used instead of `dec rcx` because DEC is outside the
  // Phase-1 instruction subset; both are defined identically for the
  // accumulator case (Intel SDM Vol. 2 §3.5).
  //
  // Expected: rax = 10+9+8+...+1 = 55, rcx = 0, and [0x5000..] holds
  // 55, 54, 53, ... 46.
  const auto code = B({
      0x48, 0x31, 0xC0,                          // +00 xor rax, rax
      0x48, 0xC7, 0xC1, 0x0A, 0x00, 0x00, 0x00,  // +03 mov rcx, 10
      0x48, 0xC7, 0xC3, 0x00, 0x50, 0x00, 0x00,  // +0A mov rbx, 0x5000
      0x48, 0x01, 0xC8,                          // +11 add rax, rcx
      0x48, 0x83, 0xE9, 0x01,                    // +14 sub rcx, 1
      0x48, 0x89, 0x03,                          // +18 mov [rbx], rax
      0x48, 0x83, 0xC3, 0x08,                    // +1B add rbx, 8
      0x48, 0x83, 0xF9, 0x00,                    // +1F cmp rcx, 0
      0x75, 0xEC,                                // +23 jne -20 -> +0x11 (loop head)
      0xC3,                                      // +25 ret
  });

  CpuState initial;
  auto memory = LinearGuestMemory::MakeCodeAndData(0, kMappedSize);
  const PipelineOutcome out = RunPipeline(code, initial, memory);

  REQUIRE(out.decoded);
  REQUIRE(out.translated);
  REQUIRE(out.validated);
  CHECK_EQ(out.reason, TerminationReason::kReturn);
  CHECK(out.execute_status.ok());

  // Sum of 1..10 = 55. Derived arithmetic: 10*11/2 = 55.
  CHECK_EQ(out.state.rax, std::uint64_t{55});
  CHECK_EQ(out.state.rcx, std::uint64_t{0});

  // The store happens AFTER `sub rcx, 1`, so the values stored are running sums
  // in INCREASING order: 10, 10+9, 10+9+8, ... 55.
  //
  // Derivation: on iteration i (1-based) rax holds the sum of the largest i
  // values, 10 down to 11-i. That equals the total 55 minus the sum of the
  // remaining values, (9-i) + (8-i) + ... + 1 = T(9-i) = (9-i)(10-i)/2.
  for (int i = 0; i < 10; ++i) {
    const int n = 9 - i;
    const std::uint64_t expected = 55u - static_cast<std::uint64_t>(n * (n + 1) / 2);
    std::uint64_t v = 0;
    REQUIRE(memory.ReadData(0x5000 + static_cast<GuestAddr>(i) * 8, 8, &v).ok());
    if (v != expected) {
      char buf[128];
      std::snprintf(buf, sizeof(buf),
                    "loop iteration %d: expected %llu, stored %llu", i,
                    static_cast<unsigned long long>(expected),
                    static_cast<unsigned long long>(v));
      ReportFailure(__FILE__, __LINE__, buf);
    }
  }
}

P5EMU_TEST(EndToEnd, UnsupportedInstructionStopsTranslationHonestly) {
  // A program containing an instruction outside the Phase-1 subset must be
  // REJECTED, not partially translated and executed as if it had worked.
  // Intel SDM Vol. 2 §2.2.1: 0F 0B is UD2.
  const auto code = B({
      0x48, 0xC7, 0xC0, 0x05, 0x00, 0x00, 0x00,  // mov rax, 5
      0x0F, 0x0B,                                // ud2
      0xC3,                                      // ret
  });

  CpuState initial;
  auto memory = LinearGuestMemory::MakeCodeAndData(0, kMappedSize);
  const PipelineOutcome out = RunPipeline(code, initial, memory);

  // Decoding UD2 is expected to report "unsupported", not "malformed": the
  // bytes are well formed, P5EMU simply does not implement them.
  CHECK(!out.decoded);
  CHECK_EQ(out.decode_status.code(), StatusCode::kUnimplementedInstruction);
  // And nothing was executed.
  CHECK(!out.executed);
}

P5EMU_TEST(EndToEnd, Int3BecomesATrap) {
  // INT3 is a deliberate trap. P5EMU surfaces it as
  // kUnimplementedInstruction rather than pretending to emulate a debugger.
  const auto code = B({
      0x48, 0xC7, 0xC0, 0x05, 0x00, 0x00, 0x00,  // mov rax, 5
      0xCC,                                      // int3
      0xC3,                                      // ret
  });

  CpuState initial;
  auto memory = LinearGuestMemory::MakeCodeAndData(0, kMappedSize);
  const PipelineOutcome out = RunPipeline(code, initial, memory);

  REQUIRE(out.decoded);
  REQUIRE(out.translated);
  REQUIRE(out.validated);
  CHECK_EQ(out.reason, TerminationReason::kTrap);
  CHECK_EQ(out.execute_status.code(), StatusCode::kUnimplementedInstruction);
  // The instruction before the trap still ran.
  CHECK_EQ(out.state.rax, std::uint64_t{5});
}

P5EMU_TEST(EndToEnd, PipelineIsDeterministicAcrossRuns) {
  // The same bytes and the same initial state must produce the same result every
  // time. Reproducibility is a precondition for every other claim here.
  const auto code = B({
      0x48, 0xC7, 0xC0, 0x05, 0x00, 0x00, 0x00,
      0x48, 0xC7, 0xC1, 0x0A, 0x00, 0x00, 0x00,
      0x48, 0x01, 0xC8,
      0x48, 0xC7, 0xC2, 0x00, 0x40, 0x00, 0x00,
      0x48, 0x89, 0x02,
      0x48, 0x8B, 0x32,
      0x48, 0x83, 0xFE, 0x0F,
      0x74, 0x0E,
      0x48, 0xC7, 0xC7, 0xFF, 0xFF, 0xFF, 0xFF,
      0xEB, 0x05,
      0x90,
      0x48, 0xC7, 0xC7, 0x63, 0x00, 0x00, 0x00,
      0x90,
      0xC3,
  });

  CpuState reference_state;
  std::string reference_ir;
  std::uint64_t reference_ops = 0;
  std::size_t reference_insns = 0;

  for (int i = 0; i < 8; ++i) {
    CpuState initial;
    initial.rdi = 0xDEAD;
    auto memory = LinearGuestMemory::MakeCodeAndData(0, kMappedSize);
    const PipelineOutcome out = RunPipeline(code, initial, memory);

    REQUIRE(out.executed);
    if (i == 0) {
      reference_state = out.state;
      reference_ir = out.ir_text;
      reference_ops = out.op_count;
      reference_insns = out.instruction_count;
      continue;
    }

    StateDifference sd;
    if (!CompareCpuState(reference_state, out.state, &sd)) {
      char buf[192];
      std::snprintf(buf, sizeof(buf),
                    "run %d diverged at %s: expected 0x%llx got 0x%llx", i,
                    sd.field != nullptr ? sd.field : "?",
                    static_cast<unsigned long long>(sd.expected),
                    static_cast<unsigned long long>(sd.actual));
      ReportFailure(__FILE__, __LINE__, buf);
    }
    CHECK_EQ(out.ir_text, reference_ir);
    CHECK_EQ(out.op_count, reference_ops);
    CHECK_EQ(out.instruction_count, reference_insns);
  }

  // Spot-check that the deterministic result is the right one too.
  CHECK_EQ(reference_state.rax, std::uint64_t{15});
}

P5EMU_TEST(EndToEnd, RejectsEmptyInput) {
  ir::Program program;
  std::vector<GuestAddr> addresses;
  const Status st = TranslateProgram(kCodeBase, nullptr, 0, "empty", &program,
                                     &addresses);
  CHECK(!st.ok());
}
