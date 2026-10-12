// SPDX-License-Identifier: Apache-2.0
//
// P5EMU is an independent, open-source project. See LICENSE and
// PROVENANCE.md before adding code here.
//
// ============================================================================
// PHASE 3 SEMANTIC EXPANSION TESTS
// ============================================================================
//
//   Arm64Widths      8/16/32/64-bit ALU and flag production
//   Arm64PartialReg  AL/AX/EAX/RAX write rules
//   Arm64Shifts      SHL/SHR/SAR, immediate and CL counts, count masking, flags
//   Arm64MemWidth    1/2/4/8-byte loads and stores through GuestMemoryPort
//   Arm64Edge        carry/borrow/overflow boundaries per width
//   Arm64ControlFlow all condition codes, nesting, a bounded loop
//
// Every differential test runs the SAME IR through the ReferenceInterpreter and
// the AArch64 backend, the latter executed under the independent qemu-aarch64
// oracle. Architecturally UNDEFINED flag bits (e.g. OF after a shift by more
// than one) are excluded through a fixture-specific observable mask rather than
// by inventing a value for them.
// ============================================================================

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "TestHarness.h"

#include "p5emu/cpu/Arm64Backend.h"
#include "p5emu/cpu/Arm64CodeGen.h"
#include "p5emu/cpu/Arm64Runner.h"
#include "p5emu/cpu/Differential.h"
#include "p5emu/cpu/IrBuilder.h"
#include "p5emu/cpu/LinearGuestMemory.h"
#include "p5emu/cpu/ReferenceInterpreter.h"
#include "p5emu/cpu/TranslateToIr.h"

namespace {

using namespace p5emu;         // NOLINT(build/namespaces)
using namespace p5emu::cpu;    // NOLINT(build/namespaces)
namespace ir = p5emu::ir;
namespace memory = p5emu::memory;
using ir::Builder;

constexpr std::uint64_t kStepLimit = 1u << 20;
constexpr memory::GuestAddr kMemBase = 0x1'0000;
constexpr memory::GuestSize kMemSize = 0x1000;

std::string HarnessPath() {
  const char* env = std::getenv("P5EMU_A64_HARNESS");
  if (env != nullptr && *env != '\0') {
    return env;
  }
#ifdef P5EMU_A64_HARNESS_DEFAULT
  return P5EMU_A64_HARNESS_DEFAULT;
#else
  return "";
#endif
}

void ExpectAgree(const diff::ComparisonResult& r, const char* file, int line) {
  if (r.verdict != diff::Verdict::kAgree) {
    p5emu::test::ReportFailure(file, line,
                               std::string("expected agreement: ") + r.summary);
  }
}

diff::ComparisonResult CompareProgram(
    const ir::Program& program, const CpuState& initial,
    const std::vector<std::uint8_t>& memory_bytes = {},
    const std::vector<diff::MemoryWindow>& windows = {},
    std::uint64_t eflags_mask = eflags::kAllMask,
    const Arm64CodeGenOptions& options = {}) {
  QemuArm64Runner runner(HarnessPath());
  auto reference = MakeReferenceInterpreter(kStepLimit);
  auto arm = MakeArm64Backend(kStepLimit, &runner, options);

  diff::ComparisonRequest request;
  request.name = program.name;
  request.program = &program;
  request.initial_state = initial;
  request.memory_base = kMemBase;
  request.memory_size = kMemSize;
  request.initial_memory = memory_bytes;
  request.permissions = memory::kPermReadWriteExecute;
  request.windows = windows;
  request.step_limit = kStepLimit;
  request.eflags_observable_mask = eflags_mask;
  return diff::RunComparison(request, {reference.get(), arm.get()});
}

bool TranslateX86(const std::vector<std::uint8_t>& code, ir::Program* out) {
  std::vector<memory::GuestAddr> addresses;
  const Status ts =
      TranslateProgram(0x1000, code.data(), code.size(), "x86", out, &addresses);
  return ts.ok();
}

diff::ComparisonResult CompareX86(
    const std::vector<std::uint8_t>& code, const CpuState& initial,
    const std::vector<diff::MemoryWindow>& windows = {},
    std::uint64_t eflags_mask = eflags::kAllMask,
    const std::vector<std::uint8_t>& image = {}) {
  ir::Program program;
  if (!TranslateX86(code, &program)) {
    diff::ComparisonResult r;
    r.verdict = diff::Verdict::kError;
    r.summary = "TranslateX86 failed";
    return r;
  }
  return CompareProgram(program, initial, image, windows, eflags_mask);
}

// Runs raw x86 through decode -> IR -> reference interpreter only. Used for
// hand-derived expected values (never taken from ARM64). `image`, if non-empty,
// seeds guest memory from kMemBase.
bool RunInterp(const std::vector<std::uint8_t>& code, CpuState* state,
               ExecutionResult* out, const std::vector<std::uint8_t>& image = {}) {
  ir::Program program;
  if (!TranslateX86(code, &program)) {
    return false;
  }
  memory::LinearGuestMemory mem =
      memory::LinearGuestMemory::MakeCodeAndData(kMemBase, kMemSize);
  if (!image.empty()) {
    if (!mem.DebugFill(kMemBase, image.data(), image.size()).ok()) {
      return false;
    }
  }
  ReferenceInterpreter interp(kStepLimit);
  *out = interp.Execute(program, state, mem);
  return true;
}

std::uint64_t MaskForWidth(std::uint32_t width_bytes) {
  return (width_bytes >= 8) ? ~std::uint64_t{0}
                            : ((std::uint64_t{1} << (8u * width_bytes)) - 1u);
}

// The observable EOFAGS mask for a shift with a given effective count, derived
// from the ISA: OF is defined ONLY for count == 1; CF is defined while the count
// is below the operand size; AF is never defined for shifts; ZF/SF/PF are always
// defined (and for count == 0 every flag is simply unchanged).
std::uint64_t ShiftMask(std::uint32_t width_bytes, std::uint32_t raw_count) {
  const std::uint32_t bits = 8u * width_bytes;
  const std::uint32_t count =
      raw_count & ((width_bytes == 8) ? 0x3Fu : 0x1Fu);
  if (count == 0) {
    return eflags::kAllMask;  // no flag changes at all
  }
  std::uint64_t m = eflags::kPf | eflags::kZf | eflags::kSf;
  if (count < bits) {
    m |= eflags::kCf;
  }
  if (count == 1) {
    m |= eflags::kOf;
  }
  return m;
}

// Builds a single-block program: dst = dst <op> src, width-aware, then Return.
ir::Program BuildBinOp(ir::OpKind kind, std::uint8_t width,
                       ir::ArchRegister dst, ir::ArchRegister src) {
  Builder b("binop");
  b.CreateBlock(0x1000);
  b.SetEntryBlock(0);
  const ir::ValueId a = b.EmitGetReg(dst, width);
  const ir::ValueId c = b.EmitGetReg(src, width);
  if (kind == ir::OpKind::kCmp) {
    // CMP and TEST commit their flags to EFLAGS; the differential comparison of
    // ctx.eflags observes them, so no register result is needed.
    b.EmitCompare(a, c, width);
  } else if (kind == ir::OpKind::kTest) {
    b.EmitTest(a, c, width);
  } else {
    const ir::ValueId r = b.EmitBinaryFlags(kind, a, c, width);
    b.EmitSetReg(dst, r, width);
  }
  b.EmitReturn();
  return b.program();
}

std::vector<std::uint64_t> InterestingValues() {
  return {0ull, 1ull, 2ull, 0x7Full, 0x80ull, 0xFFull, 0x100ull,
          0x7FFFULL, 0x8000ULL, 0xFFFFULL, 0x10000ULL, 0x7FFFFFFFULL,
          0x80000000ULL, 0xFFFFFFFFULL, 0x100000000ULL, 0x7FFFFFFFFFFFFFFFULL,
          0x8000000000000000ULL, 0xFFFFFFFFFFFFFFFFULL};
}

}  // namespace

// ===========================================================================
// Widths: the SAME values and ops at 8/16/32/64 bits.
// ===========================================================================
P5EMU_TEST(Arm64Widths, ArithmeticFlagsAgreeAtEveryWidth) {
  const struct { ir::OpKind kind; const char* name; } kinds[] = {
      {ir::OpKind::kAddFlags, "add"}, {ir::OpKind::kSubFlags, "sub"},
      {ir::OpKind::kAndFlags, "and"}, {ir::OpKind::kOrFlags, "or"},
      {ir::OpKind::kXorFlags, "xor"}, {ir::OpKind::kCmp, "cmp"},
      {ir::OpKind::kTest, "test"}};
  const std::uint8_t widths[] = {1, 2, 4, 8};
  const auto values = InterestingValues();

  for (const std::uint8_t w : widths) {
    const std::uint64_t mask = MaskForWidth(w);
    for (const auto& k : kinds) {
      const ir::Program program =
          BuildBinOp(k.kind, w, ir::ArchRegister::kRax, ir::ArchRegister::kRcx);
      for (std::size_t i = 0; i + 1 < values.size(); ++i) {
        CpuState s;
        s.rax = values[i] & mask;
        s.rcx = values[i + 1] & mask;
        const auto r = CompareProgram(program, s);
        ExpectAgree(r, __FILE__, __LINE__);
      }
      // And the reversed operands, so borrow/carry direction is exercised.
      for (std::size_t i = 0; i + 1 < values.size(); ++i) {
        CpuState s;
        s.rax = values[i + 1] & mask;
        s.rcx = values[i] & mask;
        const auto r = CompareProgram(program, s);
        ExpectAgree(r, __FILE__, __LINE__);
      }
    }
  }
}

P5EMU_TEST(Arm64Widths, NarrowAddFlagsComeFromTheRightBit) {
  // The specific Phase-3 defect this guards: before widths, a 32-bit ADD's carry
  // was taken from bit 63. 0xFFFFFFFF + 1 is zero at 32 bits WITH carry; at 64
  // bits it is not zero and has no carry. Compare against the interpreter, which
  // is the specification.
  const std::uint8_t bytes32[] = {0x05, 0xFF, 0xFF, 0xFF, 0xFF};  // add eax, -1
  (void)bytes32;
  const ir::Program program =
      BuildBinOp(ir::OpKind::kAddFlags, 4, ir::ArchRegister::kRax,
                 ir::ArchRegister::kRcx);
  CpuState s;
  s.rax = 0xFFFFFFFFull;
  s.rcx = 1;
  const auto r = CompareProgram(program, s);
  ExpectAgree(r, __FILE__, __LINE__);

  // Independently derived (Intel SDM Vol. 2 §3.5): EAX = 0, CF = 1, ZF = 1,
  // PF = 1, SF = 0, OF = 0, AF = 1. Verified through the interpreter.
  CpuState check;
  check.rax = 0xFFFFFFFFull;
  check.rcx = 1;
  Builder b("narrow-add");
  b.CreateBlock(0x1000);
  b.SetEntryBlock(0);
  const ir::ValueId a = b.EmitGetReg(ir::ArchRegister::kRax, 4);
  const ir::ValueId c = b.EmitGetReg(ir::ArchRegister::kRcx, 4);
  b.EmitSetReg(ir::ArchRegister::kRax, b.EmitBinaryFlags(ir::OpKind::kAddFlags,
                                                         a, c, 4),
               4);
  b.EmitReturn();
  memory::LinearGuestMemory mem =
      memory::LinearGuestMemory::MakeCodeAndData(kMemBase, kMemSize);
  ReferenceInterpreter interp(kStepLimit);
  interp.Execute(b.program(), &check, mem);
  CHECK_EQ(check.rax, std::uint64_t{0});
  CHECK((check.eflags & eflags::kCf) != 0);
  CHECK((check.eflags & eflags::kZf) != 0);
  CHECK((check.eflags & eflags::kSf) == 0);
  CHECK((check.eflags & eflags::kOf) == 0);
}

// ===========================================================================
// Partial registers.
// ===========================================================================
P5EMU_TEST(Arm64PartialReg, WriteRulesForAlAxEaxRax) {
  struct Case {
    const char* name;
    std::vector<std::uint8_t> code;
    std::uint64_t initial_rax;
    std::uint64_t expected_rax;
    const char* provenance;
  };
  const std::vector<Case> cases = {
      {"mov al, 0xFF", {0xB0, 0xFF}, 0x1122334455667788ull,
       0x11223344556677FFull, "AL write preserves RAX[63:8]"},
      {"mov ax, 0x1234", {0x66, 0xB8, 0x34, 0x12}, 0x1122334455667788ull,
       0x1122334455661234ull, "AX write preserves RAX[63:16]"},
      {"mov eax, 0x12345678", {0xB8, 0x78, 0x56, 0x34, 0x12},
       0x1122334455667788ull, 0x12345678ull,
       "EAX write zero-extends into RAX (SDM Vol.1 3.4.1.1)"},
      {"mov rax, imm64",
       {0x48, 0xB8, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11},
       0ull, 0x1122334455667788ull, "RAX write replaces all 64 bits"},
  };

  for (const Case& c : cases) {
    CpuState s;
    s.rax = c.initial_rax;
    ExecutionResult r;
    REQUIRE(RunInterp(c.code, &s, &r));
    if (s.rax != c.expected_rax) {
      char buf[256];
      std::snprintf(buf, sizeof(buf),
                    "%s: expected rax=0x%llx got 0x%llx (%s)", c.name,
                    static_cast<unsigned long long>(c.expected_rax),
                    static_cast<unsigned long long>(s.rax), c.provenance);
      p5emu::test::ReportFailure(__FILE__, __LINE__, buf);
    }
    // And the ARM64 backend agrees.
    CpuState diff_state;
    diff_state.rax = c.initial_rax;
    const auto cmp = CompareX86(c.code, diff_state);
    ExpectAgree(cmp, __FILE__, __LINE__);
  }
}

P5EMU_TEST(Arm64PartialReg, AddToAlPreservesUpperBits) {
  // add al, cl: 0x88 + 0x80 = 0x08 with carry, low byte only.
  const std::vector<std::uint8_t> code = {0x00, 0xC8};
  CpuState s;
  s.rax = 0xAAAAAAAAAAAAAA88ull;
  s.rcx = 0x0000000000000080ull;
  ExecutionResult r;
  REQUIRE(RunInterp(code, &s, &r));
  CHECK_EQ(s.rax, std::uint64_t{0xAAAAAAAAAAAAAA08ull});
  CpuState d;
  d.rax = 0xAAAAAAAAAAAAAA88ull;
  d.rcx = 0x0000000000000080ull;
  const auto cmp = CompareX86(code, d);
  ExpectAgree(cmp, __FILE__, __LINE__);
}

// ===========================================================================
// Shifts.
// ===========================================================================
P5EMU_TEST(Arm64Shifts, ImmediateAndClCountsAgreeAtEveryWidth) {
  struct Form {
    const char* name;
    std::vector<std::uint8_t> prefix;  // opcode bytes before the count/modrm
    bool uses_cl;
    const char* op;
  };
  // Encoding groups (Intel SDM Vol. 2 §2.2.2):
  //   D1 /4 SHL r/m,1 ; D3 /4 SHL r/m,CL ; C1 /4 SHL r/m,imm8  (32-bit default)
  //   ModRM /4 => E0 for eax.
  const std::vector<std::vector<std::uint8_t>> sizes = {
      {}, {0x66}, {0x48}};  // 32, 16, 64
  const std::uint8_t byte_form = 1;

  const struct { std::uint8_t ext; const char* op; } ops[] = {
      {4, "shl"}, {5, "shr"}, {7, "sar"}};

  for (const auto& size_prefix : sizes) {
    const std::uint32_t width = size_prefix.empty()
                                    ? 4u
                                    : (size_prefix[0] == 0x66 ? 2u : 8u);
    for (const auto& op : ops) {
      // imm8 form
      for (std::uint32_t count : {0u, 1u, 2u, 7u, 31u, 40u}) {
        std::vector<std::uint8_t> code = size_prefix;
        code.push_back(0xC1);
        code.push_back(static_cast<std::uint8_t>(0xE0 | (op.ext << 3)));
        code.push_back(static_cast<std::uint8_t>(count));
        CpuState s;
        s.rax = 0xDEADBEEFCAFEF00Dull & MaskForWidth(width);
        ExecutionResult r;
        REQUIRE(RunInterp(code, &s, &r));
        const auto cmp =
            CompareX86(code, s, {}, ShiftMask(width, count));
        ExpectAgree(cmp, __FILE__, __LINE__);
      }
      // CL form
      for (std::uint32_t count : {0u, 1u, 3u, 9u}) {
        std::vector<std::uint8_t> code = size_prefix;
        code.push_back(0xD3);
        code.push_back(static_cast<std::uint8_t>(0xE0 | (op.ext << 3)));
        CpuState s;
        s.rax = 0xDEADBEEFCAFEF00Dull & MaskForWidth(width);
        s.rcx = count;
        ExecutionResult r;
        REQUIRE(RunInterp(code, &s, &r));
        const auto cmp =
            CompareX86(code, s, {}, ShiftMask(width, count));
        ExpectAgree(cmp, __FILE__, __LINE__);
      }
      // By-one form (D1 /ext)
      {
        std::vector<std::uint8_t> code = size_prefix;
        code.push_back(0xD1);
        code.push_back(static_cast<std::uint8_t>(0xE0 | (op.ext << 3)));
        CpuState s;
        s.rax = 0x8000000000000001ull & MaskForWidth(width);
        ExecutionResult r;
        REQUIRE(RunInterp(code, &s, &r));
        const auto cmp = CompareX86(code, s, {}, ShiftMask(width, 1));
        ExpectAgree(cmp, __FILE__, __LINE__);
      }
    }
  }
  (void)byte_form;
}

P5EMU_TEST(Arm64Shifts, EightBitShiftByOne) {
  // D0 /4 SHL r/m8,1 : shl al,1
  const std::vector<std::uint8_t> code = {0xD0, 0xE0};
  CpuState s;
  s.rax = 0x81;
  ExecutionResult r;
  REQUIRE(RunInterp(code, &s, &r));
  CHECK_EQ(s.rax & 0xFFull, std::uint64_t{0x02});  // 0x81<<1 = 0x02, CF=1
  CHECK((s.eflags & eflags::kCf) != 0);
  CpuState d;
  d.rax = 0x81;
  const auto cmp = CompareX86(code, d, {}, ShiftMask(1, 1));
  ExpectAgree(cmp, __FILE__, __LINE__);
}

P5EMU_TEST(Arm64Shifts, CountMaskingIsModuloFiveOrSix) {
  // A 32-bit shift by 33 uses count 33 & 0x1F = 1. Derived from Intel SDM
  // Vol. 2 §3.5. shl eax, 33 == shl eax, 1.
  const std::vector<std::uint8_t> by33 = {0xC1, 0xE0, 0x21};  // shl eax, 33
  CpuState s;
  s.rax = 0x00000001;
  ExecutionResult r;
  REQUIRE(RunInterp(by33, &s, &r));
  CHECK_EQ(s.rax, std::uint64_t{2});  // 1 << (33 & 31) = 1 << 1

  CpuState d;
  d.rax = 0x00000001;
  const auto cmp = CompareX86(by33, d, {}, ShiftMask(4, 1));
  ExpectAgree(cmp, __FILE__, __LINE__);
}

// ===========================================================================
// Memory widths.
// ===========================================================================
P5EMU_TEST(Arm64MemWidth, LoadsAndStoresAtEveryWidthAgree) {
  const memory::GuestAddr addr = kMemBase + 0x40;
  const std::vector<diff::MemoryWindow> windows = {{addr, 8}};

  struct Case {
    const char* name;
    std::vector<std::uint8_t> code;
    std::uint64_t value;
    std::uint32_t store_width;   // 0 for a pure load case
    std::uint32_t load_width;    // 0 for a pure store case
  };
  const std::vector<Case> cases = {
      {"mov [rdx], al", {0x88, 0x02}, 0xAB, 1, 0},
      {"mov al, [rdx]", {0x8A, 0x02}, 0xAB, 0, 1},
      {"mov [rdx], ax", {0x66, 0x89, 0x02}, 0xBEEF, 2, 0},
      {"mov ax, [rdx]", {0x66, 0x8B, 0x02}, 0xBEEF, 0, 2},
      {"mov [rdx], eax", {0x89, 0x02}, 0xDEADBEEFull, 4, 0},
      {"mov eax, [rdx]", {0x8B, 0x02}, 0xDEADBEEFull, 0, 4},
      {"mov [rdx], rax", {0x48, 0x89, 0x02}, 0xDEADBEEFCAFEF00Dull, 8, 0},
      {"mov rax, [rdx]", {0x48, 0x8B, 0x02}, 0xDEADBEEFCAFEF00Dull, 0, 8},
  };

  for (const Case& c : cases) {
    // Seed the 8 bytes at `addr` with the value, little-endian.
    std::vector<std::uint8_t> image(kMemSize, 0);
    for (int i = 0; i < 8; ++i) {
      image[0x40 + static_cast<std::size_t>(i)] =
          static_cast<std::uint8_t>((c.value >> (8 * i)) & 0xFFu);
    }

    CpuState s;
    s.rdx = addr;
    s.rax = 0;
    ExecutionResult r;
    REQUIRE(RunInterp(c.code, &s, &r, image));

    if (c.load_width != 0) {
      // Low `load_width` bytes of rax equal the low bytes of the seeded value
      // (sign-extended for movsx).
      const std::uint64_t expected = c.value;  // for the fixtures used
      std::uint64_t got = s.rax;
      if (c.load_width == 4) {
        got &= 0xFFFFFFFFull;
      } else if (c.load_width == 2) {
        got &= 0xFFFFull;
      } else if (c.load_width == 1) {
        got &= 0xFFull;
      }
      if (got != (expected & (c.load_width >= 8 ? ~0ull
                                                : ((1ull << (8 * c.load_width)) - 1)))) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "%s: loaded 0x%llx", c.name,
                      static_cast<unsigned long long>(s.rax));
        p5emu::test::ReportFailure(__FILE__, __LINE__, buf);
      }
    }

    CpuState d;
    d.rdx = addr;
    d.rax = 0;
    const auto cmp = CompareX86(c.code, d, windows, eflags::kAllMask, image);
    ExpectAgree(cmp, __FILE__, __LINE__);
  }
}

P5EMU_TEST(Arm64MemWidth, SignAndZeroExtendingLoads) {
  const memory::GuestAddr addr = kMemBase + 0x40;
  auto seed = [&](std::uint8_t byte) {
    std::vector<std::uint8_t> image(kMemSize, 0);
    image[0x40] = byte;
    return image;
  };
  const std::vector<diff::MemoryWindow> windows = {{addr, 1}};

  // movsx eax, byte [rdx] : 0x80 -> 0xFFFFFF80 (sign-extended within EAX)
  {
    const std::vector<std::uint8_t> code = {0x0F, 0xBE, 0x02};
    CpuState s;
    s.rdx = addr;
    ExecutionResult r;
    REQUIRE(RunInterp(code, &s, &r, seed(0x80)));
    // Sign-extended to 32 bits, then EAX's zero-extension gives RAX = 0xFFFFFF80.
    CHECK_EQ(s.rax, std::uint64_t{0x00000000FFFFFF80ull});
    CpuState d;
    d.rdx = addr;
    const auto cmp = CompareX86(code, d, windows, eflags::kAllMask, seed(0x80));
    ExpectAgree(cmp, __FILE__, __LINE__);
  }
  // movzx eax, byte [rdx] : 0x80 -> 0x00000080 (zero-extended)
  {
    const std::vector<std::uint8_t> code = {0x0F, 0xB6, 0x02};
    CpuState s;
    s.rdx = addr;
    ExecutionResult r;
    REQUIRE(RunInterp(code, &s, &r, seed(0x80)));
    CHECK_EQ(s.rax, std::uint64_t{0x80});
    CpuState d;
    d.rdx = addr;
    const auto cmp = CompareX86(code, d, windows, eflags::kAllMask, seed(0x80));
    ExpectAgree(cmp, __FILE__, __LINE__);
  }
}

P5EMU_TEST(Arm64MemWidth, StorePreservesAdjacentBytes) {
  // mov [rdx], al must not disturb the seven bytes after it.
  const memory::GuestAddr addr = kMemBase + 0x40;
  const std::vector<std::uint8_t> code = {0x88, 0x02, 0x48, 0x8B, 0x02,
                                          0xC3};  // store al; load rax; ret
  CpuState s;
  s.rdx = addr;
  s.rax = 0xBB;
  s.rsp = kMemBase + 0x800;
  const std::vector<diff::MemoryWindow> windows = {{addr, 8}};

  // Seed the region with a known pattern in both backends via the initial image.
  std::vector<std::uint8_t> image(kMemSize, 0);
  for (int i = 0; i < 8; ++i) {
    image[0x40 + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(0x10 + i);
  }
  const auto cmp = CompareX86(code, s, windows);
  ExpectAgree(cmp, __FILE__, __LINE__);
  // The interpreter-only check: low byte becomes 0xBB, bytes 1..7 unchanged.
  CpuState check;
  check.rdx = addr;
  check.rax = 0xBB;
  check.rsp = kMemBase + 0x800;
  ir::Program program;
  REQUIRE(TranslateX86(code, &program));
  memory::LinearGuestMemory mem =
      memory::LinearGuestMemory::MakeCodeAndData(kMemBase, kMemSize);
  REQUIRE(mem.DebugFill(kMemBase + 0x40, image.data() + 0x40, 8).ok());
  ReferenceInterpreter interp(kStepLimit);
  interp.Execute(program, &check, mem);
  std::uint64_t low = 0, high = 0;
  REQUIRE(mem.ReadData(addr, 1, &low).ok());
  REQUIRE(mem.ReadData(addr + 1, 1, &high).ok());
  CHECK_EQ(low, std::uint64_t{0xBB});
  CHECK_EQ(high, std::uint64_t{0x11});
}

P5EMU_TEST(Arm64MemWidth, OutOfBoundsAccessFaultsAtEveryWidth) {
  // A load from an unmapped address faults in both backends, at each width.
  const struct { std::vector<std::uint8_t> code; } cases[] = {
      {{0x8A, 0x02}},              // mov al, [rdx]
      {{0x66, 0x8B, 0x02}},        // mov ax, [rdx]
      {{0x8B, 0x02}},              // mov eax, [rdx]
      {{0x48, 0x8B, 0x02}},        // mov rax, [rdx]
  };
  for (const auto& c : cases) {
    CpuState s;
    s.rdx = 0xDEAD'0000ull;  // far outside the single region
    const auto cmp = CompareX86(c.code, s);
    ExpectAgree(cmp, __FILE__, __LINE__);
  }
}

// ===========================================================================
// Edge cases.
// ===========================================================================
P5EMU_TEST(Arm64Edge, CarryBorrowOverflowBoundariesPerWidth) {
  // Boundary pairs chosen to force carry/borrow/overflow at each width:
  //   0 and max, max and 1, INT_MIN and -1, INT_MAX and 1.
  const std::uint8_t widths[] = {1, 2, 4, 8};
  for (const std::uint8_t w : widths) {
    const std::uint64_t mask = MaskForWidth(w);
    const std::uint64_t sign = (w >= 8) ? 0x8000000000000000ull
                                        : (std::uint64_t{1} << (8u * w - 1u));
    const std::uint64_t pairs[][2] = {
        {0, mask}, {mask, 1}, {sign, mask}, {sign - 1, 1}, {1, mask},
        {mask, mask}, {sign, sign}};
    for (const auto& p : pairs) {
      for (const ir::OpKind kind :
           {ir::OpKind::kAddFlags, ir::OpKind::kSubFlags,
            ir::OpKind::kCmp}) {
        const ir::Program program = BuildBinOp(
            kind, w, ir::ArchRegister::kRax, ir::ArchRegister::kRcx);
        CpuState s;
        s.rax = p[0] & mask;
        s.rcx = p[1] & mask;
        const auto r = CompareProgram(program, s);
        ExpectAgree(r, __FILE__, __LINE__);
      }
    }
  }
}

// ===========================================================================
// Control flow.
// ===========================================================================
P5EMU_TEST(Arm64ControlFlow, EveryConditionCodeAndBothPaths) {
  // Multi-block: cmp rax, rcx; jcc taken(+1) else nottaken(+2).
  for (int cc = 0; cc <= 15; ++cc) {
    Builder b("cf");
    const ir::BlockId entry = b.CreateBlock(0x1000);
    b.SetEntryBlock(entry);
    const ir::ValueId a = b.EmitGetReg(ir::ArchRegister::kRax);
    const ir::ValueId c = b.EmitGetReg(ir::ArchRegister::kRcx);
    const ir::ValueId flags = b.EmitCompare(a, c);
    const ir::BlockId taken = b.CreateBlock(0x1000);
    const ir::BlockId nottaken = b.CreateBlock(0x1000);
    b.SetCurrentBlock(entry);
    b.EmitBranch(static_cast<ir::ConditionCode>(cc), flags, taken, nottaken);
    b.SetCurrentBlock(taken);
    b.EmitSetReg(ir::ArchRegister::kRdx,
                 b.EmitMaterializeConstant(1, ir::ValueType::kI64));
    b.EmitReturn();
    b.SetCurrentBlock(nottaken);
    b.EmitSetReg(ir::ArchRegister::kRdx,
                 b.EmitMaterializeConstant(2, ir::ValueType::kI64));
    b.EmitReturn();

    const std::uint64_t pairs[][2] = {{5, 5}, {5, 10}, {10, 5}, {0, 0},
                                      {0xFFFFFFFFFFFFFFFFull, 0},
                                      {0x8000000000000000ull, 1}};
    for (const auto& p : pairs) {
      CpuState s;
      s.rax = p[0];
      s.rcx = p[1];
      const auto r = CompareProgram(b.program(), s);
      ExpectAgree(r, __FILE__, __LINE__);
    }
  }
}

P5EMU_TEST(Arm64ControlFlow, BoundedLoopAccumulatesIdentically) {
  // sum = 0; i = 10; while (i != 0) { sum += i; i -= 1; } ret
  //   xor eax, eax              (31 C0)
  //   mov ecx, 10               (B9 0A 00 00 00)
  //   loop: add eax, ecx        (01 C8)
  //         sub ecx, 1            (83 E9 01)
  //         cmp ecx, 0            (83 F9 00)
  //         jne loop              (75 F6)
  //         ret                   (C3)
  const std::vector<std::uint8_t> code = {
      0x31, 0xC0,
      0xB9, 0x0A, 0x00, 0x00, 0x00,
      0x01, 0xC8,
      0x83, 0xE9, 0x01,
      0x83, 0xF9, 0x00,
      0x75, 0xF6,
      0xC3};
  CpuState s;
  s.rsp = kMemBase + 0x800;  // ret pops [rsp]
  ExecutionResult r;
  REQUIRE(RunInterp(code, &s, &r));
  CHECK_EQ(s.rax, std::uint64_t{55});  // 10+9+...+1 = 55
  CHECK_EQ(s.rcx, std::uint64_t{0});
  CpuState d;
  d.rsp = kMemBase + 0x800;
  const auto cmp = CompareX86(code, d);
  ExpectAgree(cmp, __FILE__, __LINE__);
}

// ===========================================================================
// Negative proofs: a controlled TEST-ONLY defect must be caught by the
// differential system, for the right reason (a real state divergence).
// ===========================================================================
P5EMU_TEST(Arm64NegGate, WidthZeroExtensionDefectIsCaught) {
  // A 32-bit register write must zero-extend the upper 32 bits. The IR here
  // feeds a FULL 64-bit value into a 32-bit write, so a backend that forgets the
  // zero-extension diverges. Correct result: rax = rcx & 0xFFFFFFFF.
  Builder b("zext");
  b.CreateBlock(0x1000);
  b.SetEntryBlock(0);
  const ir::ValueId full = b.EmitGetReg(ir::ArchRegister::kRcx, 8);
  b.EmitSetReg(ir::ArchRegister::kRax, full, 4);
  b.EmitReturn();

  CpuState s;
  s.rcx = 0xDEADBEEF12345678ull;

  // The correct backend agrees with the interpreter.
  const auto good = CompareProgram(b.program(), s);
  ExpectAgree(good, __FILE__, __LINE__);

  // The injected defect is caught as a divergence.
  Arm64CodeGenOptions bad;
  bad.inject_no_zero_extend = true;
  const auto r = CompareProgram(b.program(), s, {}, {}, eflags::kAllMask, bad);
  CHECK_EQ(r.verdict, diff::Verdict::kDiverge);
  CHECK(!r.differences.empty());
}

P5EMU_TEST(Arm64NegGate, ShiftCountMaskDefectIsCaught) {
  // shl eax, 33 : x86 masks the count to 5 bits, so this is shl eax, 1 -> 2.
  // A backend that masks to 6 bits (or not at all) computes a different result.
  const std::vector<std::uint8_t> code = {0xC1, 0xE0, 0x21};
  CpuState s;
  s.rax = 1;

  const auto good = CompareX86(code, s, {}, ShiftMask(4, 1));
  ExpectAgree(good, __FILE__, __LINE__);

  Arm64CodeGenOptions bad;
  bad.inject_wide_shift_count = true;
  ir::Program program;
  REQUIRE(TranslateX86(code, &program));
  const auto r = CompareProgram(program, s, {}, {}, ShiftMask(4, 1), bad);
  CHECK_EQ(r.verdict, diff::Verdict::kDiverge);
  CHECK(!r.differences.empty());
}

// ===========================================================================
// Randomized differential testing, batched through ONE qemu process.
//
// Thousands of hand-built IR programs (mixed widths, operations, shift counts,
// and memory widths) are generated from a fixed seed, executed by the reference
// interpreter on the host and by the AArch64 backend through a single batched
// harness run, then compared with the SAME primitives the differential harness
// uses (CompareCpuState / reason / memory bytes). Determinism: a failure names
// the seed and case index, and the whole run reproduces exactly.
// ===========================================================================
namespace {

std::uint64_t NextRandom(std::uint64_t* state) {
  std::uint64_t x = *state;
  x ^= x >> 12;
  x ^= x << 25;
  x ^= x >> 27;
  *state = x;
  return x * 0x2545F4914F6CDD1Dull;
}

void StateToCtx(const CpuState& s, P5EmuArm64Ctx* c) {
  c->gpr[0] = s.rax;  c->gpr[1] = s.rbx;  c->gpr[2] = s.rcx;
  c->gpr[3] = s.rdx;  c->gpr[4] = s.rsi;  c->gpr[5] = s.rdi;
  c->gpr[6] = s.rbp;  c->gpr[7] = s.rsp;  c->gpr[8] = s.r8;
  c->gpr[9] = s.r9;   c->gpr[10] = s.r10; c->gpr[11] = s.r11;
  c->gpr[12] = s.r12; c->gpr[13] = s.r13; c->gpr[14] = s.r14;
  c->gpr[15] = s.r15;
  c->rip = s.rip;
  c->eflags = s.eflags;
}

CpuState CtxToState(const P5EmuArm64Ctx& c) {
  CpuState s;
  s.rax = c.gpr[0];  s.rbx = c.gpr[1];  s.rcx = c.gpr[2];
  s.rdx = c.gpr[3];  s.rsi = c.gpr[4];  s.rdi = c.gpr[5];
  s.rbp = c.gpr[6];  s.rsp = c.gpr[7];  s.r8 = c.gpr[8];
  s.r9 = c.gpr[9];   s.r10 = c.gpr[10]; s.r11 = c.gpr[11];
  s.r12 = c.gpr[12]; s.r13 = c.gpr[13]; s.r14 = c.gpr[14];
  s.r15 = c.gpr[15];
  s.rip = c.rip;
  s.eflags = c.eflags;
  return s;
}

constexpr memory::GuestSize kRandMem = 64;

}  // namespace

P5EMU_TEST(Arm64Random, BatchedRandomizedDifferential) {
  std::uint64_t rng = 0x0BADF00DDEADBEEFull;  // FIXED SEED
  const int kCases = 2500;

  struct Case {
    CpuState initial;
    CpuState expected;
    std::uint64_t mask;
    TerminationReason reason;
    std::vector<std::uint8_t> mem_expected;
  };

  std::vector<Arm64BatchCase> batch;
  std::vector<Case> cases;
  batch.reserve(static_cast<std::size_t>(kCases));
  cases.reserve(static_cast<std::size_t>(kCases));

  for (int i = 0; i < kCases; ++i) {
    const std::uint8_t widths[4] = {1, 2, 4, 8};
    const std::uint8_t w = widths[NextRandom(&rng) % 4];
    const std::uint64_t mask = MaskForWidth(w);

    Builder b("rand");
    b.CreateBlock(0x1000);
    b.SetEntryBlock(0);
    CpuState initial;
    initial.rax = NextRandom(&rng) & mask;
    initial.rcx = NextRandom(&rng) & mask;
    initial.rdx = kMemBase + (NextRandom(&rng) % (kRandMem - 8));
    initial.rsp = kMemBase + kRandMem - 8;
    std::uint64_t obs_mask = eflags::kAllMask;

    const int kind = static_cast<int>(NextRandom(&rng) % 6);
    if (kind <= 3) {
      // ALU with flags: rax = rax <op> rcx at width w.
      const ir::OpKind kinds[5] = {ir::OpKind::kAddFlags, ir::OpKind::kSubFlags,
                                   ir::OpKind::kAndFlags, ir::OpKind::kOrFlags,
                                   ir::OpKind::kXorFlags};
      const ir::OpKind k = kinds[NextRandom(&rng) % 5];
      const ir::ValueId a = b.EmitGetReg(ir::ArchRegister::kRax, w);
      const ir::ValueId c = b.EmitGetReg(ir::ArchRegister::kRcx, w);
      b.EmitSetReg(ir::ArchRegister::kRax, b.EmitBinaryFlags(k, a, c, w), w);
    } else if (kind == 4) {
      // Shift: rax = rax <shift> count at width w.
      const ir::OpKind kinds[3] = {ir::OpKind::kShl, ir::OpKind::kShr,
                                   ir::OpKind::kSar};
      const ir::OpKind k = kinds[NextRandom(&rng) % 3];
      const std::uint32_t count = static_cast<std::uint32_t>(NextRandom(&rng) % 41);
      const ir::ValueId v = b.EmitGetReg(ir::ArchRegister::kRax, w);
      const ir::ValueId c =
          b.EmitMaterializeConstant(count, ir::ValueType::kI64);
      b.EmitSetReg(ir::ArchRegister::kRax, b.EmitShift(k, v, c, w), w);
      obs_mask = ShiftMask(w, count);
    } else {
      // Memory: store rax to [rdx] then load it back into rcx, at width w.
      const ir::ValueId addr =
          b.EmitGetReg(ir::ArchRegister::kRdx, 8);
      const ir::ValueId val = b.EmitGetReg(ir::ArchRegister::kRax, w);
      b.EmitStore(addr, val, w);
      const ir::ValueId loaded = b.EmitLoad(addr, w);
      b.EmitSetReg(ir::ArchRegister::kRcx, loaded, w);
    }
    b.EmitReturn();

    ir::Program program = b.program();

    std::vector<std::uint8_t> mem_in(kRandMem, 0);
    for (std::size_t j = 0; j < kRandMem; ++j) {
      mem_in[j] = static_cast<std::uint8_t>(NextRandom(&rng) & 0xFFu);
    }

    // Reference interpreter.
    memory::LinearGuestMemory mem =
        memory::LinearGuestMemory::MakeCodeAndData(kMemBase, kRandMem);
    if (!mem.DebugFill(kMemBase, mem_in.data(), mem_in.size()).ok()) {
      p5emu::test::ReportFailure(__FILE__, __LINE__, "seed fill failed");
      return;
    }
    CpuState ref_state = initial;
    ReferenceInterpreter interp(kStepLimit);
    const ExecutionResult er = interp.Execute(program, &ref_state, mem);

    // AArch64 codegen.
    Arm64CodeGenResult cg;
    const Status gs = GenerateArm64(program, &cg);
    if (!gs.ok()) {
      char buf[128];
      std::snprintf(buf, sizeof(buf), "case %d: codegen failed: %s", i,
                    cg.reason.c_str());
      p5emu::test::ReportFailure(__FILE__, __LINE__, buf);
      return;
    }

    Arm64BatchCase bc;
    bc.code = cg.code;
    StateToCtx(initial, &bc.ctx);
    bc.ctx.steps = kStepLimit;
    bc.mem_base = kMemBase;
    bc.mem_in = mem_in;
    batch.push_back(std::move(bc));

    Case c;
    c.initial = initial;
    c.expected = ref_state;
    c.mask = obs_mask;
    c.reason = er.reason;
    c.mem_expected.resize(kRandMem);
    (void)mem.ReadRange(kMemBase, kRandMem, &c.mem_expected);
    cases.push_back(std::move(c));
  }

  // One qemu process for all cases.
  QemuArm64Runner runner(HarnessPath());
  std::vector<Arm64BatchResult> results;
  std::string detail;
  const Status st = runner.RunBatch(batch, &results, &detail);
  if (!st.ok()) {
    p5emu::test::ReportFailure(
        __FILE__, __LINE__, std::string("RunBatch failed: ") + detail);
    return;
  }
  REQUIRE(results.size() == cases.size());

  int divergences = 0;
  for (std::size_t i = 0; i < cases.size(); ++i) {
    const Case& c = cases[i];
    const CpuState actual = CtxToState(results[i].ctx);
    StateDifference sd;
    bool ok = CompareCpuState(c.expected, actual, &sd, c.mask);
    if (ok &&
        static_cast<TerminationReason>(results[i].ctx.status) != c.reason) {
      ok = false;
      sd.field = "termination-reason";
    }
    if (ok && results[i].mem_out != c.mem_expected) {
      ok = false;
      sd.field = "memory";
    }
    if (!ok) {
      if (divergences < 5) {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "random divergence seed=0x%llx case=%zu field=%s "
                      "expected=0x%llx actual=0x%llx",
                      static_cast<unsigned long long>(0x0BADF00DDEADBEEFull), i,
                      sd.field != nullptr ? sd.field : "<unknown>",
                      static_cast<unsigned long long>(sd.expected),
                      static_cast<unsigned long long>(sd.actual));
        p5emu::test::ReportFailure(__FILE__, __LINE__, buf);
      }
      ++divergences;
    }
  }
  CHECK_EQ(divergences, 0);
}
