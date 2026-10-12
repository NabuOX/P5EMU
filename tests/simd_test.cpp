// SPDX-License-Identifier: Apache-2.0
//
// P5EMU is an independent, open-source project. See LICENSE and
// PROVENANCE.md before adding code here.
//
// ============================================================================
// SIMD / SSE FOUNDATION TESTS (Phase 12)
// ============================================================================
//   SimdXmmState    XMM0..XMM15 are real architectural 128-bit state
//   SimdMoves       MOVD/MOVQ/MOVDQA/MOVDQU, including upper-bit semantics
//   SimdPacked      PXOR / PAND / POR / PADDQ
//   SimdMemory      128-bit guest memory transfers and faults
//   SimdProperty    fixed-seed packed-integer campaign (interpreter vs AArch64)
//   SimdNegative    deliberate proofs of representative SIMD defects
//
// Every test starts from independently authored x86 bytes and compares the
// reference interpreter against the AArch64 backend, field by field, including
// all 128 bits of every XMM register.
// ============================================================================

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "TestHarness.h"

#include "p5emu/cpu/Arm64Backend.h"
#include "p5emu/cpu/Arm64Runner.h"
#include "p5emu/cpu/Differential.h"
#include "p5emu/cpu/LinearGuestMemory.h"
#include "p5emu/cpu/ReferenceInterpreter.h"
#include "p5emu/cpu/TranslateToIr.h"
#include "p5emu/cpu/Vec128.h"

namespace {

using namespace p5emu;       // NOLINT(build/namespaces)
using namespace p5emu::cpu;  // NOLINT(build/namespaces)
namespace ir = p5emu::ir;
namespace memory = p5emu::memory;

constexpr std::uint64_t kStepLimit = 1u << 20;
constexpr memory::GuestAddr kMemBase = 0x1'0000;
constexpr memory::GuestSize kMemSize = 0x1000;

std::string HarnessPath() {
  const char* env = std::getenv("P5EMU_A64_HARNESS");
  if (env != nullptr && *env != '\0') return env;
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

bool TranslateX86(const std::vector<std::uint8_t>& code, ir::Program* out) {
  std::vector<memory::GuestAddr> addresses;
  return TranslateProgram(0x1000, code.data(), code.size(), "x86", out,
                          &addresses)
      .ok();
}

diff::ComparisonResult CompareProgram(
    const ir::Program& program, const CpuState& initial,
    const std::vector<std::uint8_t>& image = {},
    const std::vector<diff::MemoryWindow>& windows = {}) {
  QemuArm64Runner runner(HarnessPath());
  auto reference = MakeReferenceInterpreter(kStepLimit);
  auto arm = MakeArm64Backend(kStepLimit, &runner, {});
  diff::ComparisonRequest request;
  request.name = program.name;
  request.program = &program;
  request.initial_state = initial;
  request.memory_base = kMemBase;
  request.memory_size = kMemSize;
  request.initial_memory = image;
  request.permissions = memory::kPermReadWriteExecute;
  request.windows = windows;
  request.step_limit = kStepLimit;
  return diff::RunComparison(request, {reference.get(), arm.get()});
}

diff::ComparisonResult CompareX86(const std::vector<std::uint8_t>& code,
                                  const CpuState& initial,
                                  const std::vector<diff::MemoryWindow>& windows = {},
                                  const std::vector<std::uint8_t>& image = {}) {
  ir::Program program;
  if (!TranslateX86(code, &program)) {
    diff::ComparisonResult r;
    r.verdict = diff::Verdict::kError;
    r.summary = "TranslateX86 failed";
    return r;
  }
  return CompareProgram(program, initial, image, windows);
}

bool RunInterp(const std::vector<std::uint8_t>& code, CpuState* state,
               ExecutionResult* out,
               const std::vector<std::uint8_t>& image = {}) {
  ir::Program program;
  if (!TranslateX86(code, &program)) return false;
  memory::LinearGuestMemory mem =
      memory::LinearGuestMemory::MakeCodeAndData(kMemBase, kMemSize);
  if (!image.empty() && !mem.DebugFill(kMemBase, image.data(), image.size()).ok()) {
    return false;
  }
  ReferenceInterpreter interp(kStepLimit);
  *out = interp.Execute(program, state, mem);
  return true;
}

Vec128 Make(uint64_t lo, uint64_t hi) { return Vec128{lo, hi}; }

}  // namespace

// ===========================================================================
// XMM architectural state
// ===========================================================================
P5EMU_TEST(SimdXmmState, RegistersAreIndependentAndRoundTrip) {
  // movdqa xmm0, xmm1  (66 0F 6F C1)
  const std::vector<std::uint8_t> code = {0x66, 0x0F, 0x6F, 0xC1};
  CpuState s;
  s.xmm[1] = Make(0x1122334455667788ull, 0x99AABBCCDDEEFF00ull);
  ExecutionResult r;
  REQUIRE(RunInterp(code, &s, &r));
  CHECK(s.xmm[0] == s.xmm[1]);
  CHECK(s.xmm[0] == Make(0x1122334455667788ull, 0x99AABBCCDDEEFF00ull));
  // A different XMM is untouched.
  CHECK(s.xmm[2] == Vec128{});
  CpuState d;
  d.xmm[1] = Make(0x1122334455667788ull, 0x99AABBCCDDEEFF00ull);
  ExpectAgree(CompareX86(code, d), __FILE__, __LINE__);
}

// ===========================================================================
// Moves and upper-bit semantics
// ===========================================================================
P5EMU_TEST(SimdMoves, MovdRegToXmmClearsUpperBits) {
  // movd xmm0, ecx  (66 0F 6E C1). Upper 96 bits become zero.
  const std::vector<std::uint8_t> code = {0x66, 0x0F, 0x6E, 0xC1};
  CpuState s;
  s.rcx = 0xDEADBEEFull;
  s.xmm[0] = Make(~0ull, ~0ull);  // stale upper state
  ExecutionResult r;
  REQUIRE(RunInterp(code, &s, &r));
  CHECK_EQ(s.xmm[0].lo, std::uint64_t{0xDEADBEEFull});
  CHECK_EQ(s.xmm[0].hi, std::uint64_t{0});
  CpuState d; d.rcx = 0xDEADBEEFull; d.xmm[0] = Make(~0ull, ~0ull);
  ExpectAgree(CompareX86(code, d), __FILE__, __LINE__);
}

P5EMU_TEST(SimdMoves, MovqRegToXmmClearsUpperBits) {
  // movq xmm0, rcx  (66 48 0F 6E C1). Upper 64 bits become zero.
  const std::vector<std::uint8_t> code = {0x66, 0x48, 0x0F, 0x6E, 0xC1};
  CpuState s;
  s.rcx = 0x1122334455667788ull;
  s.xmm[0] = Make(0xAAAAAAAAAAAAAAAAull, 0xBBBBBBBBBBBBBBBBull);
  ExecutionResult r;
  REQUIRE(RunInterp(code, &s, &r));
  CHECK_EQ(s.xmm[0].lo, std::uint64_t{0x1122334455667788ull});
  CHECK_EQ(s.xmm[0].hi, std::uint64_t{0});
  CpuState d; d.rcx = 0x1122334455667788ull;
  d.xmm[0] = Make(0xAAAAAAAAAAAAAAAAull, 0xBBBBBBBBBBBBBBBBull);
  ExpectAgree(CompareX86(code, d), __FILE__, __LINE__);
}

P5EMU_TEST(SimdMoves, MovdXmmToRegTakesLow32) {
  // movd ecx, xmm0  (66 0F 7E C1). ECX = low 32 bits, zero-extended.
  const std::vector<std::uint8_t> code = {0x66, 0x0F, 0x7E, 0xC1};
  CpuState s;
  s.xmm[0] = Make(0x11223344DEADBEEFull, 0x99AABBCCDDEEFF00ull);
  ExecutionResult r;
  REQUIRE(RunInterp(code, &s, &r));
  CHECK_EQ(s.rcx, std::uint64_t{0xDEADBEEFull});  // ECX, zero-extended to RCX
  CpuState d; d.xmm[0] = Make(0x11223344DEADBEEFull, 0x99AABBCCDDEEFF00ull);
  ExpectAgree(CompareX86(code, d), __FILE__, __LINE__);
}

P5EMU_TEST(SimdMoves, MovqXmmToRegTakesLow64) {
  // movq rcx, xmm0  (66 48 0F 7E C1)
  const std::vector<std::uint8_t> code = {0x66, 0x48, 0x0F, 0x7E, 0xC1};
  CpuState s;
  s.xmm[0] = Make(0x1122334455667788ull, 0x99AABBCCDDEEFF00ull);
  ExecutionResult r;
  REQUIRE(RunInterp(code, &s, &r));
  CHECK_EQ(s.rcx, std::uint64_t{0x1122334455667788ull});
  CpuState d; d.xmm[0] = Make(0x1122334455667788ull, 0x99AABBCCDDEEFF00ull);
  ExpectAgree(CompareX86(code, d), __FILE__, __LINE__);
}

// ===========================================================================
// Packed integer ops
// ===========================================================================
P5EMU_TEST(SimdPacked, XorAndOrAcrossBothHalves) {
  const CpuState base = [] {
    CpuState s;
    s.xmm[0] = Make(0xFFFFFFFF00000000ull, 0x123456789ABCDEF0ull);
    s.xmm[1] = Make(0x0F0F0F0F0F0F0F0Full, 0x00FF00FF00FF00FFull);
    return s;
  }();
  // pxor xmm0, xmm1 (66 0F EF C1)
  { ir::Program p; REQUIRE(TranslateX86({0x66,0x0F,0xEF,0xC1}, &p));
    CpuState s = base; ReferenceInterpreter i(kStepLimit);
    memory::LinearGuestMemory m = memory::LinearGuestMemory::MakeCodeAndData(kMemBase,kMemSize);
    i.Execute(p, &s, m);
    CHECK_EQ(s.xmm[0].lo, base.xmm[0].lo ^ base.xmm[1].lo);
    CHECK_EQ(s.xmm[0].hi, base.xmm[0].hi ^ base.xmm[1].hi); }
  // pand xmm0, xmm1 (66 0F DB C1)
  { ir::Program p; REQUIRE(TranslateX86({0x66,0x0F,0xDB,0xC1}, &p));
    CpuState s = base; ReferenceInterpreter i(kStepLimit);
    memory::LinearGuestMemory m = memory::LinearGuestMemory::MakeCodeAndData(kMemBase,kMemSize);
    i.Execute(p, &s, m);
    CHECK_EQ(s.xmm[0].lo, base.xmm[0].lo & base.xmm[1].lo);
    CHECK_EQ(s.xmm[0].hi, base.xmm[0].hi & base.xmm[1].hi); }
  // por xmm0, xmm1 (66 0F EB C1)
  { ir::Program p; REQUIRE(TranslateX86({0x66,0x0F,0xEB,0xC1}, &p));
    CpuState s = base; ReferenceInterpreter i(kStepLimit);
    memory::LinearGuestMemory m = memory::LinearGuestMemory::MakeCodeAndData(kMemBase,kMemSize);
    i.Execute(p, &s, m);
    CHECK_EQ(s.xmm[0].lo, base.xmm[0].lo | base.xmm[1].lo);
    CHECK_EQ(s.xmm[0].hi, base.xmm[0].hi | base.xmm[1].hi); }
  // differential for each
  ExpectAgree(CompareX86({0x66,0x0F,0xEF,0xC1}, base), __FILE__, __LINE__);
  ExpectAgree(CompareX86({0x66,0x0F,0xDB,0xC1}, base), __FILE__, __LINE__);
  ExpectAgree(CompareX86({0x66,0x0F,0xEB,0xC1}, base), __FILE__, __LINE__);
}

P5EMU_TEST(SimdPacked, PaddqLanesAreIndependent) {
  // paddq xmm0, xmm1 (66 0F D4 C1). Low lane overflows; high lane must not
  // receive the carry.
  const std::vector<std::uint8_t> code = {0x66, 0x0F, 0xD4, 0xC1};
  CpuState s;
  s.xmm[0] = Make(0xFFFFFFFFFFFFFFFFull, 0x0000000000000005ull);
  s.xmm[1] = Make(0x0000000000000001ull, 0x0000000000000003ull);
  ExecutionResult r;
  REQUIRE(RunInterp(code, &s, &r));
  CHECK_EQ(s.xmm[0].lo, std::uint64_t{0});       // wraps, no cross-lane carry
  CHECK_EQ(s.xmm[0].hi, std::uint64_t{8});       // 5 + 3, unaffected by the carry
  CpuState d;
  d.xmm[0] = Make(0xFFFFFFFFFFFFFFFFull, 0x0000000000000005ull);
  d.xmm[1] = Make(0x0000000000000001ull, 0x0000000000000003ull);
  ExpectAgree(CompareX86(code, d), __FILE__, __LINE__);
}

// ===========================================================================
// 128-bit memory
// ===========================================================================
P5EMU_TEST(SimdMemory, MovdquMemoryRoundTrip) {
  // movdqu xmm0, [rax] (F3 0F 6F 00) ; movdqu [rbx], xmm0 (F3 0F 7F 03)
  const std::vector<std::uint8_t> code = {0xF3, 0x0F, 0x6F, 0x00,
                                          0xF3, 0x0F, 0x7F, 0x03};
  const std::uint64_t src = kMemBase + 0x40;
  const std::uint64_t dst = kMemBase + 0x80;
  std::vector<std::uint8_t> image(kMemSize, 0);
  for (int i = 0; i < 8; ++i) {
    image[0x40 + static_cast<std::size_t>(i)] =
        static_cast<std::uint8_t>((0x1122334455667788ull >> (8 * i)) & 0xFF);
    image[0x48 + static_cast<std::size_t>(i)] =
        static_cast<std::uint8_t>((0x99AABBCCDDEEFF00ull >> (8 * i)) & 0xFF);
  }
  CpuState s;
  s.rax = src;
  s.rbx = dst;
  ExecutionResult r;
  REQUIRE(RunInterp(code, &s, &r, image));
  CHECK(s.xmm[0] == Make(0x1122334455667788ull, 0x99AABBCCDDEEFF00ull));
  CpuState d;
  d.rax = src;
  d.rbx = dst;
  ExpectAgree(CompareX86(code, d, {{dst, 16}}, image), __FILE__, __LINE__);
}

P5EMU_TEST(SimdMemory, MovdqaMemoryRoundTrip) {
  // movdqa xmm0, [rax] (66 0F 6F 00) ; movdqa [rbx], xmm0 (66 0F 7F 03)
  const std::vector<std::uint8_t> code = {0x66, 0x0F, 0x6F, 0x00,
                                          0x66, 0x0F, 0x7F, 0x03};
  const std::uint64_t src = kMemBase + 0x40;
  const std::uint64_t dst = kMemBase + 0x80;
  std::vector<std::uint8_t> image(kMemSize, 1);  // non-zero everywhere
  CpuState s;
  s.rax = src;
  s.rbx = dst;
  ExecutionResult r;
  REQUIRE(RunInterp(code, &s, &r, image));
  CpuState d;
  d.rax = src;
  d.rbx = dst;
  ExpectAgree(CompareX86(code, d, {{dst, 16}}, image), __FILE__, __LINE__);
}

P5EMU_TEST(SimdMemory, LoadFaultLeavesXmmUnchanged) {
  // movdqu xmm0, [rax] from an unmapped address faults; xmm0 is unchanged.
  const std::vector<std::uint8_t> code = {0xF3, 0x0F, 0x6F, 0x00};
  CpuState s;
  s.rax = 0xDEAD'0000ull;  // far outside the region
  s.xmm[0] = Make(0x1234567890ABCDEFull, 0xFEDCBA0987654321ull);
  ExecutionResult r;
  REQUIRE(RunInterp(code, &s, &r));
  CHECK(r.reason == TerminationReason::kFault);
  CHECK(s.xmm[0] == Make(0x1234567890ABCDEFull, 0xFEDCBA0987654321ull));
  CpuState d;
  d.rax = 0xDEAD'0000ull;
  d.xmm[0] = Make(0x1234567890ABCDEFull, 0xFEDCBA0987654321ull);
  ExpectAgree(CompareX86(code, d), __FILE__, __LINE__);
}

// ===========================================================================
// Negative proofs
// ===========================================================================
P5EMU_TEST(SimdNegative, RepresentativeDefectsAreDetected) {
  // 1. A high/low swap would be caught: build an expected that swaps halves and
  //    confirm it does NOT match the real result.
  const std::vector<std::uint8_t> mov = {0x66, 0x0F, 0x6F, 0xC1};  // movdqa xmm0,xmm1
  CpuState s;
  s.xmm[1] = Make(0x1122334455667788ull, 0x99AABBCCDDEEFF00ull);
  ExecutionResult r;
  REQUIRE(RunInterp(mov, &s, &r));
  const Vec128 swapped = Make(s.xmm[1].hi, s.xmm[1].lo);
  CHECK(s.xmm[0] != swapped);  // a swap is a real, detectable difference

  // 2. PADDQ must not carry between lanes (checked exactly by SimdPacked above;
  //    restated as a direct assertion on a carry-only-low case).
  const std::vector<std::uint8_t> add = {0x66, 0x0F, 0xD4, 0xC1};
  CpuState a;
  a.xmm[0] = Make(~0ull, 0);
  a.xmm[1] = Make(1, 0);
  ExecutionResult ra;
  REQUIRE(RunInterp(add, &a, &ra));
  CHECK_EQ(a.xmm[0].lo, std::uint64_t{0});
  CHECK_EQ(a.xmm[0].hi, std::uint64_t{0});  // no carry into the high lane

  // 3. A high-half-only difference is a real difference (guards against a
  //    comparison that only checks the low 64 bits).
  CpuState x;
  CpuState y;
  x.xmm[5] = Make(0, 0x1);
  y.xmm[5] = Make(0, 0x2);
  CHECK(!CompareCpuState(x, y, nullptr));
}

// ===========================================================================
// Property campaign
// ===========================================================================
P5EMU_TEST(SimdProperty, PackedIntegerCampaign) {
  std::uint64_t rng = 0x5A5A1234DEADBEEFull;
  const auto next = [&]() {
    rng ^= rng >> 12;
    rng ^= rng << 25;
    rng ^= rng >> 27;
    return rng * 0x2545F4914F6CDD1Dull;
  };
  const std::vector<std::vector<std::uint8_t>> ops = {
      {0x66, 0x0F, 0xEF, 0xC1},  // pxor
      {0x66, 0x0F, 0xDB, 0xC1},  // pand
      {0x66, 0x0F, 0xEB, 0xC1},  // por
      {0x66, 0x0F, 0xD4, 0xC1},  // paddq
  };
  int cases = 0;
  for (int i = 0; i < 80; ++i) {
    for (const auto& code : ops) {
      CpuState s;
      s.xmm[0] = Make(next(), next());
      s.xmm[1] = Make(next(), next());
      // Add the all-ones / all-zero / overflow patterns too.
      if ((i & 3) == 0) s.xmm[1] = Make(~0ull, 0);
      if ((i & 3) == 1) s.xmm[0] = Make(0, ~0ull);
      ExpectAgree(CompareX86(code, s), __FILE__, __LINE__);
      ++cases;
    }
  }
  CHECK(cases >= 300);
}
