// SPDX-License-Identifier: LicenseRef-P5EMU-Proprietary
//
// P5EMU source is independently authored and proprietary. See LICENSE.md and
// PROVENANCE.md before adding code here.
//
// ============================================================================
// HLE FUZZ AND PROPERTY TESTS (Phase 6)
// ============================================================================
//   HleFuzz      deterministic malformed import/dispatch/identity campaign
//   HleProperty  deterministic arithmetic property cases (interpreter + AArch64)
// ============================================================================

#include <cstdint>
#include <string>
#include <vector>

#include "TestHarness.h"

#include "SyntheticHle.h"

#include "p5emu/core/Status.h"
#include "p5emu/cpu/Arm64CodeGen.h"
#include "p5emu/cpu/Arm64Runner.h"
#include "p5emu/cpu/IrBuilder.h"
#include "p5emu/cpu/Ir.h"
#include "p5emu/cpu/LinearGuestMemory.h"
#include "p5emu/cpu/ReferenceInterpreter.h"
#include "p5emu/hle/Hle.h"

namespace {

using namespace p5emu;          // NOLINT(build/namespaces)
namespace memory = p5emu::memory;
namespace cpu = p5emu::cpu;
namespace hle = p5emu::hle;
namespace ir = p5emu::ir;

constexpr std::uint64_t kStepLimit = 1u << 20;
constexpr std::uint64_t kTBase = hle::HleRegistry::kTrampolineBase;

std::uint64_t NextRandom(std::uint64_t* state) {
  std::uint64_t x = *state;
  x ^= x >> 12;
  x ^= x << 25;
  x ^= x >> 27;
  *state = x;
  return x * 0x2545F4914F6CDD1Dull;
}

std::string HarnessPath() {
  const char* env = std::getenv("P5EMU_A64_HARNESS");
  if (env != nullptr && *env != '\0') return env;
#ifdef P5EMU_A64_HARNESS_DEFAULT
  return P5EMU_A64_HARNESS_DEFAULT;
#else
  return "";
#endif
}

bool ValidDispatchStatus(StatusCode c) {
  switch (c) {
    case StatusCode::kOk:
    case StatusCode::kHleNotFound:
    case StatusCode::kHleAmbiguous:
    case StatusCode::kHleUnsupportedIdentity:
    case StatusCode::kHleInvalidArgument:
    case StatusCode::kHleMemoryFault:
    case StatusCode::kHleDispatchFault:
    case StatusCode::kHleInternalError:
      return true;
    default:
      return false;
  }
}

}  // namespace

// ===========================================================================
// Fuzz / malformed campaign.
// ===========================================================================
P5EMU_TEST(HleFuzz, DispatchAndIdentityCampaign) {
  hle::HleRegistry reg;
  p5emu::test::RegisterCanonical(&reg);
  hle::HleDispatcher dispatcher(&reg);
  memory::LinearGuestMemory mem =
      memory::LinearGuestMemory::MakeCodeAndData(0x1000, 0x2000);

  std::uint64_t rng = 0x1234'5678'9ABC'DEF1ull;
  std::uint64_t cases = 0;
  int bad = 0;

  // Deterministic dispatch over a broad mix of known, unknown, stale, and
  // boundary binding addresses.
  for (int i = 0; i < 6000; ++i) {
    cpu::CpuState st;
    st.rdi = NextRandom(&rng);
    st.rsi = NextRandom(&rng);
    st.rdx = NextRandom(&rng);
    st.rcx = NextRandom(&rng);
    st.r8 = NextRandom(&rng);
    st.r9 = NextRandom(&rng);

    std::uint64_t binding = 0;
    switch (i % 5) {
      case 0: binding = kTBase + (NextRandom(&rng) % 3) * 8; break;   // known
      case 1: binding = kTBase + 0x40 + NextRandom(&rng) % 0x1000; break;  // unknown
      case 2: binding = NextRandom(&rng); break;                      // random
      case 3: binding = kTBase; break;                               // boundary
      default: binding = ~std::uint64_t{0} - (NextRandom(&rng) % 16); break;
    }
    const Status s = dispatcher.HandleExternalCall(binding, &st, &mem);
    ++cases;
    if (!ValidDispatchStatus(s.code())) ++bad;
  }

  // Pointer/argument fault fuzzing through the memory HLE function.
  cpu::CpuState s;
  hle::HleCallContext ctx(&s, &mem);
  for (int i = 0; i < 2000; ++i) {
    s.rdi = NextRandom(&rng);            // ptr
    s.rsi = NextRandom(&rng) % 16;       // add
    s.rdx = NextRandom(&rng);            // out
    const hle::HleStatus st = p5emu::test::P5emuMemory(ctx);
    ++cases;
    if (st != hle::HleStatus::kOk && st != hle::HleStatus::kMemoryFault) ++bad;
  }

  std::printf("HLE malformed campaign: %llu deterministic cases\n",
              static_cast<unsigned long long>(cases));
  CHECK(cases >= 5000);
  CHECK_EQ(bad, 0);
}

// ===========================================================================
// Property testing: arithmetic HLE, interpreter and AArch64 agree.
// ===========================================================================
P5EMU_TEST(HleProperty, ArithmeticMatchesOracleAcrossBackends) {
  hle::HleRegistry reg;
  p5emu::test::RegisterCanonical(&reg);
  hle::HleDispatcher dispatcher(&reg);
  const std::uint64_t t_add = kTBase + 0;

  constexpr int kCases = 2000;
  std::uint64_t rng = 0xF00D'BEEF'CAFE'1234ull;

  cpu::QemuArm64Runner runner(HarnessPath());
  const bool have_a64 = runner.available();
  std::vector<cpu::Arm64BatchCase> batch;
  std::vector<std::uint64_t> expected;
  std::vector<std::uint64_t> interp_rax;

  int failures = 0;
  int cases = 0;

  const std::uint64_t ret_addr = 0x1040;
  for (int i = 0; i < kCases; ++i) {
    const std::uint64_t a = NextRandom(&rng) & 0xFFFF;
    const std::uint64_t b = NextRandom(&rng) & 0xFFFF;
    const std::uint64_t c = NextRandom(&rng) & 0xFF;

    ir::Builder bld("prop");
    const ir::BlockId entry = bld.CreateBlock(0x1000);
    bld.SetEntryBlock(entry);
    bld.EmitSetReg(ir::ArchRegister::kRdi,
                   bld.EmitMaterializeConstant(a, ir::ValueType::kI64));
    bld.EmitSetReg(ir::ArchRegister::kRsi,
                   bld.EmitMaterializeConstant(b, ir::ValueType::kI64));
    // push the return address (the CALL lowering does this for real calls)
    const ir::ValueId sp = bld.EmitGetReg(ir::ArchRegister::kRsp);
    const ir::ValueId nsp = bld.EmitBinary(
        ir::OpKind::kSub, sp,
        bld.EmitMaterializeConstant(8, ir::ValueType::kI64));
    bld.EmitStore(nsp, bld.EmitMaterializeConstant(ret_addr, ir::ValueType::kI64), 8);
    bld.EmitSetReg(ir::ArchRegister::kRsp, nsp);
    const ir::BlockId retblk = bld.CreateBlock(ret_addr);
    bld.SetCurrentBlock(entry);
    bld.EmitExternalCall(t_add, retblk);
    bld.SetCurrentBlock(retblk);
    const ir::ValueId rax = bld.EmitGetReg(ir::ArchRegister::kRax, 8);
    const ir::ValueId sum = bld.EmitBinary(
        ir::OpKind::kAdd, rax,
        bld.EmitMaterializeConstant(c, ir::ValueType::kI64));
    bld.EmitSetReg(ir::ArchRegister::kRax, sum, 8);
    bld.EmitReturn();
    const ir::Program program = bld.program();

    // Interpreter.
    memory::LinearGuestMemory mem =
        memory::LinearGuestMemory::MakeCodeAndData(0x1000, 0x2000);
    cpu::CpuState st;
    st.rip = 0x1000;
    st.rsp = 0x1800;
    cpu::ReferenceInterpreter interp(kStepLimit, &dispatcher);
    interp.Execute(program, &st, mem);
    ++cases;
    const std::uint64_t want = a + b + c;
    if (st.rax != want) ++failures;
    interp_rax.push_back(st.rax);
    expected.push_back(want);

    // AArch64 batch case for the same program.
    if (have_a64) {
      cpu::Arm64CodeGenResult cg;
      if (cpu::GenerateArm64(program, &cg).ok()) {
        cpu::Arm64BatchCase bc;
        bc.code = cg.code;
        std::memset(&bc.ctx, 0, sizeof(bc.ctx));
        bc.ctx.gpr[7] = 0x1800;      // rsp
        bc.ctx.gpr[0] = 0;           // rax
        bc.ctx.rip = 0x1000;
        bc.ctx.steps = kStepLimit;
        bc.mem_base = 0x1000;
        bc.mem_in.assign(0x2000, 0);
        batch.push_back(std::move(bc));
      }
    }
  }

  if (have_a64 && !batch.empty()) {
    std::vector<cpu::Arm64BatchResult> results;
    std::string detail;
    const Status st = runner.RunBatch(batch, &results, &detail);
    REQUIRE(st.ok());
    REQUIRE(results.size() == interp_rax.size());
    for (std::size_t i = 0; i < results.size(); ++i) {
      const std::uint64_t a64 = results[i].ctx.gpr[0];
      if (a64 != interp_rax[i]) ++failures;
    }
  }

  std::printf("HLE property cases: %d (interpreter%s), %d failures\n", cases,
              have_a64 ? " + AArch64" : "", failures);
  CHECK(cases >= 2000);
  CHECK_EQ(failures, 0);
}
