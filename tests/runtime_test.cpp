// SPDX-License-Identifier: Apache-2.0
//
// P5EMU is an independent, open-source project. See LICENSE and
// PROVENANCE.md before adding code here.
//
// ============================================================================
// RUNTIME TESTS (Phase 7)
// ============================================================================
//   RuntimeAddressSpace, RuntimeStack, RuntimeStartup, RuntimeGraph,
//   RuntimeBias, RuntimeLifecycle, RuntimeResolve, RuntimeMerge,
//   RuntimeMultiModule, RuntimeHleStackArgs, RuntimeNegative, RuntimeReadiness
// ============================================================================

#include <cstdint>
#include <cstdlib>
#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "TestHarness.h"

#include "RelocFixtureBuilder.h"
#include "SyntheticHle.h"

#include "p5emu/core/Status.h"
#include "p5emu/cpu/Arm64Backend.h"
#include "p5emu/cpu/Arm64Runner.h"
#include "p5emu/cpu/Differential.h"
#include "p5emu/cpu/LinearGuestMemory.h"
#include "p5emu/cpu/ReferenceInterpreter.h"
#include "p5emu/cpu/TranslateToIr.h"
#include "p5emu/hle/Hle.h"
#include "p5emu/runtime/Runtime.h"

namespace {

using namespace p5emu;          // NOLINT(build/namespaces)
namespace loader = p5emu::loader;
namespace memory = p5emu::memory;
namespace hle = p5emu::hle;
namespace cpu = p5emu::cpu;
namespace rt = p5emu::runtime;
using p5emu::test::BuildRelocElf64;
using p5emu::test::RelocEntry;
using p5emu::test::RelocFixtureOptions;
using p5emu::test::RelocSym;

constexpr std::uint64_t kStepLimit = 1u << 20;
constexpr std::uint64_t kTBase = hle::HleRegistry::kTrampolineBase;

std::string HarnessPath() {
  const char* env = std::getenv("P5EMU_A64_HARNESS");
  if (env != nullptr && *env != '\0') return env;
#ifdef P5EMU_A64_HARNESS_DEFAULT
  return P5EMU_A64_HARNESS_DEFAULT;
#else
  return "";
#endif
}

// ---------------------------------------------------------------------------
// Address space
// ---------------------------------------------------------------------------
P5EMU_TEST(RuntimeAddressSpace, OverlapIsRejected) {
  rt::AddressSpace space;
  CHECK(space.Add({0x1000, 0x1000, memory::kPermRead, rt::kNoOwner, "m"}).ok());
  // Overlapping.
  CHECK_EQ(space.Add({0x1800, 0x1000, memory::kPermRead, rt::kNoOwner, "m"}).code(),
           StatusCode::kOverlappingMapping);
  // Adjacent is fine.
  CHECK(space.Add({0x2000, 0x1000, memory::kPermRead, rt::kNoOwner, "m"}).ok());
  CHECK(space.Find(0x1000) != nullptr);
  CHECK(space.Find(0x2FFF) != nullptr);
  CHECK(space.Find(0x3000) == nullptr);
  CHECK(space.Overlaps(0x1500, 0x100));
  CHECK(!space.Overlaps(0x3000, 0x100));
}

// ---------------------------------------------------------------------------
// Stack
// ---------------------------------------------------------------------------
P5EMU_TEST(RuntimeStack, PushPopAndBounds) {
  rt::GuestStack stack;
  REQUIRE(stack.Create(0x10000, 0x1000).ok());
  CHECK_EQ(stack.top(), 0x11000);
  memory::MappedGuestMemory mem;
  REQUIRE(mem.AddRegion({0x10000, 0x1000, memory::kPermReadWrite}).ok());

  memory::GuestAddr rsp = stack.top();
  REQUIRE(stack.Push64(&mem, &rsp, 0xAAAAAAAAAAAAAAAAull).ok());
  REQUIRE(stack.Push64(&mem, &rsp, 0xBBBBBBBBBBBBBBBBull).ok());
  CHECK_EQ(rsp, stack.top() - 16);
  std::uint64_t v = 0;
  REQUIRE(stack.Pop64(&mem, &rsp, &v).ok());
  CHECK_EQ(v, 0xBBBBBBBBBBBBBBBBull);
  REQUIRE(stack.Pop64(&mem, &rsp, &v).ok());
  CHECK_EQ(v, 0xAAAAAAAAAAAAAAAAull);
  CHECK_EQ(rsp, stack.top());

  // Underflow: rsp at the base cannot push.
  memory::GuestAddr low = stack.base();
  CHECK_EQ(stack.Push64(&mem, &low, 1).code(), StatusCode::kUnmappedAccess);
  // Overrun: rsp at the top cannot pop.
  memory::GuestAddr high = stack.top();
  CHECK_EQ(stack.Pop64(&mem, &high, &v).code(), StatusCode::kUnmappedAccess);
}

// ---------------------------------------------------------------------------
// Startup / initial stack builder
// ---------------------------------------------------------------------------
namespace {
memory::MappedGuestMemory MakeStackMem(const rt::GuestStack& stack) {
  memory::MappedGuestMemory mem;
  mem.AddRegion({stack.base(), stack.size(), memory::kPermReadWrite});
  return mem;
}
}  // namespace

P5EMU_TEST(RuntimeStartup, ZeroAndMultipleArgs) {
  rt::GuestStack stack;
  REQUIRE(stack.Create().ok());
  memory::MappedGuestMemory mem = MakeStackMem(stack);
  const std::vector<std::string> no_args;
  const std::vector<std::string> no_env;
  const std::vector<std::pair<std::uint64_t, std::uint64_t>> no_aux;
  const rt::StartupOptions opts;

  {
    rt::ProcessStartup up;
    REQUIRE(rt::BuildInitialStack(&mem, stack, no_args, no_env, no_aux, opts, &up)
                .ok());
    CHECK_EQ(up.argc, std::uint64_t{0});
    CHECK_EQ(up.initial_rsp % 16, memory::GuestAddr{0});
    // argc at [rsp].
    std::uint64_t argc = 0;
    REQUIRE(mem.ReadData(up.initial_rsp, 8, &argc).ok());
    CHECK_EQ(argc, std::uint64_t{0});
  }
  {
    rt::ProcessStartup up;
    const std::vector<std::string> args = {"hello", "world", ""};
    const std::vector<std::string> env = {"A=1", "B=2"};
    REQUIRE(rt::BuildInitialStack(&mem, stack, args, env, no_aux, opts, &up)
                .ok());
    CHECK_EQ(up.argc, std::uint64_t{3});
    CHECK_EQ(up.initial_rsp % 16, memory::GuestAddr{0});
    REQUIRE(up.argv.size() == 3);
    // argv[0] points at "hello".
    std::uint64_t p0 = 0;
    REQUIRE(mem.ReadData(up.initial_rsp + 8, 8, &p0).ok());
    CHECK_EQ(p0, up.argv[0]);
    std::uint64_t c = 0;
    REQUIRE(mem.ReadData(p0, 1, &c).ok());
    CHECK_EQ(c, std::uint64_t{'h'});
    // Empty string is a valid pointer to a NUL.
    REQUIRE(mem.ReadData(up.argv[2], 1, &c).ok());
    CHECK_EQ(c, std::uint64_t{0});
  }
}

P5EMU_TEST(RuntimeStartup, InsufficientStackAndHugeArgs) {
  const std::vector<std::string> no_env;
  const std::vector<std::pair<std::uint64_t, std::uint64_t>> no_aux;
  const rt::StartupOptions opts;

  rt::GuestStack small;
  REQUIRE(small.Create(0x10000, 16).ok());  // tiny
  memory::MappedGuestMemory mem = MakeStackMem(small);
  rt::ProcessStartup up;
  const std::vector<std::string> big_arg = {"a-fairly-long-argument"};
  CHECK_EQ(rt::BuildInitialStack(&mem, small, big_arg, no_env, no_aux, opts, &up)
               .code(),
           StatusCode::kImageTooLarge);

  // A very large argument count overflows the stack deterministically.
  rt::GuestStack stack;
  REQUIRE(stack.Create().ok());
  memory::MappedGuestMemory mem2 = MakeStackMem(stack);
  std::vector<std::string> many(300000, "x");
  rt::ProcessStartup up2;
  const Status st =
      rt::BuildInitialStack(&mem2, stack, many, no_env, no_aux, opts, &up2);
  CHECK_EQ(st.code(), StatusCode::kImageTooLarge);
}

// ---------------------------------------------------------------------------
// Dependency graph
// ---------------------------------------------------------------------------
P5EMU_TEST(RuntimeGraph, OrderDuplicateMissingCycle) {
  // main -> libA -> common ; main -> libB -> common  (diamond)
  std::map<std::string, std::vector<std::string>> deps = {
      {"main", {"libA", "libB", "libA"}},  // duplicate libA
      {"libA", {"common"}},
      {"libB", {"common"}},
      {"common", {}},
  };
  rt::DependencyGraph g;
  REQUIRE(rt::BuildDependencyGraph("main", deps, {}, &g).ok());
  // common before libA/libB before main.
  auto pos = [&](const std::string& n) {
    return std::find(g.order.begin(), g.order.end(), n) - g.order.begin();
  };
  CHECK(pos("common") < pos("libA"));
  CHECK(pos("common") < pos("libB"));
  CHECK(pos("libA") < pos("main"));
  CHECK(pos("libB") < pos("main"));
  // common appears once.
  CHECK_EQ(std::count(g.order.begin(), g.order.end(), "common"),
           std::ptrdiff_t{1});

  // Missing dependency.
  {
    std::map<std::string, std::vector<std::string>> d2 = {{"main", {"ghost"}}};
    rt::DependencyGraph g2;
    CHECK(!rt::BuildDependencyGraph("main", d2, {}, &g2).ok());
    REQUIRE(g2.missing.size() == 1);
    CHECK_EQ(g2.missing[0], std::string("ghost"));
  }
  // Cycle.
  {
    std::map<std::string, std::vector<std::string>> d3 = {
        {"a", {"b"}}, {"b", {"c"}}, {"c", {"a"}}};
    rt::DependencyGraph g3;
    CHECK(!rt::BuildDependencyGraph("a", d3, {}, &g3).ok());
    CHECK(!g3.cycle.empty());
  }
}

// ---------------------------------------------------------------------------
// Load-bias allocation
// ---------------------------------------------------------------------------
P5EMU_TEST(RuntimeBias, DeterministicAlignedNonOverlapping) {
  rt::LoadBiasAllocator a;
  rt::LoadBiasAllocator b;
  std::vector<std::uint64_t> va, vb;
  for (int i = 0; i < 4; ++i) {
    std::uint64_t x = 0;
    REQUIRE(a.Allocate(0x1000, &x).ok());
    va.push_back(x);
    std::uint64_t y = 0;
    REQUIRE(b.Allocate(0x1000, &y).ok());
    vb.push_back(y);
  }
  CHECK(va == vb);  // deterministic
  for (std::size_t i = 0; i < va.size(); ++i) {
    CHECK_EQ(va[i] % rt::LoadBiasAllocator::kStride, memory::GuestAddr{0});
    if (i > 0) CHECK(va[i] - va[i - 1] >= rt::LoadBiasAllocator::kStride);
  }
  // A module larger than a slot is refused.
  std::uint64_t x = 0;
  CHECK_EQ(a.Allocate(rt::LoadBiasAllocator::kStride + 1, &x).code(),
           StatusCode::kImageTooLarge);

  // TEST-ONLY overlap defect hands out the same slot twice.
  rt::LoadBiasAllocator bad;
  bad.SetInjectOverlap(true);
  std::uint64_t p = 0, q = 0;
  REQUIRE(bad.Allocate(0x1000, &p).ok());
  REQUIRE(bad.Allocate(0x1000, &q).ok());
  CHECK_EQ(p, q);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
P5EMU_TEST(RuntimeLifecycle, TransitionsAreForwardOnly) {
  CHECK(rt::IsValidModuleTransition(rt::ModuleState::kParsed,
                                    rt::ModuleState::kValidated));
  CHECK(rt::IsValidModuleTransition(rt::ModuleState::kMapped,
                                    rt::ModuleState::kReady));
  // Invalid: skip a step.
  CHECK(!rt::IsValidModuleTransition(rt::ModuleState::kParsed,
                                     rt::ModuleState::kMapped));
  // Invalid: backwards.
  CHECK(!rt::IsValidModuleTransition(rt::ModuleState::kReady,
                                     rt::ModuleState::kPlanned));
}

// ---------------------------------------------------------------------------
// Cross-module resolution
// ---------------------------------------------------------------------------
P5EMU_TEST(RuntimeResolve, GuestExportThenHleWithExplicitPriority) {
  std::vector<rt::LoadedModule> mods(1);
  rt::LoadedModule& m = mods[0];
  m.id = 1;
  rt::ExportedSymbol ex;
  ex.module_id = 1;
  ex.symbol = loader::SymbolIdentity::String("libmath_add");
  ex.guest_va = 0x50400000;
  m.exports.push_back(ex);

  hle::HleRegistry reg;
  p5emu::test::RegisterCanonical(&reg);

  // Guest export resolves.
  rt::ResolverOptions opt;
  rt::ProcessResolution r =
      rt::ResolveIdentity(mods, &reg, ex.symbol, opt);
  CHECK_EQ(r.status.code(), StatusCode::kOk);
  CHECK_EQ(r.source, rt::ResolutionSource::kGuestExport);
  CHECK_EQ(r.address, std::uint64_t{0x50400000});

  // HLE resolves when there is no guest export.
  r = rt::ResolveIdentity(mods, &reg,
                          loader::SymbolIdentity::String("p5emu_add"), opt);
  CHECK_EQ(r.source, rt::ResolutionSource::kHle);
  CHECK_EQ(r.address, kTBase);

  // Unknown -> unresolved.
  r = rt::ResolveIdentity(mods, &reg,
                          loader::SymbolIdentity::String("nope"), opt);
  CHECK_EQ(r.status.code(), StatusCode::kUnresolvedImport);

  // A guest export exists AND an HLE binding of the same name: guest wins by
  // default, HLE wins under kHleFirst / the injected defect.
  reg.Register("", loader::SymbolIdentity::String("libmath_add"),
               &p5emu::test::P5emuAdd);
  r = rt::ResolveIdentity(mods, &reg, ex.symbol, opt);
  CHECK_EQ(r.source, rt::ResolutionSource::kGuestExport);
  opt.policy = rt::ResolutionPolicy::kHleFirst;
  r = rt::ResolveIdentity(mods, &reg, ex.symbol, opt);
  CHECK_EQ(r.source, rt::ResolutionSource::kHle);
  opt.policy = rt::ResolutionPolicy::kGuestFirst;
  opt.inject_hle_override = true;  // TEST-ONLY defect
  r = rt::ResolveIdentity(mods, &reg, ex.symbol, opt);
  CHECK_EQ(r.source, rt::ResolutionSource::kHle);  // wrong priority, caught
}

// ---------------------------------------------------------------------------
// Merge / link
// ---------------------------------------------------------------------------
P5EMU_TEST(RuntimeMerge, MergeRemapsIdsAndLinksGuestCalls) {
  // Two tiny programs; program 0 has an external call to program 1's entry.
  ir::Builder b0("m0");
  b0.CreateBlock(0x1000);
  b0.SetEntryBlock(0);
  const ir::BlockId ret0 = b0.CreateBlock(0x1002);
  b0.SetCurrentBlock(0);
  b0.EmitExternalCall(0x2000, ret0);  // target is program 1's entry
  b0.SetCurrentBlock(ret0);
  b0.EmitReturn();
  const ir::Program p0 = b0.program();

  ir::Builder b1("m1");
  b1.CreateBlock(0x2000);
  b1.SetEntryBlock(0);
  b1.EmitReturn();
  const ir::Program p1 = b1.program();

  ir::Program merged;
  REQUIRE(rt::MergePrograms({&p0, &p1}, 0, &merged).ok());
  CHECK_EQ(merged.blocks.size(), std::size_t{3});
  REQUIRE(ir::ValidateIr(merged).ok());
  // The external call to 0x2000 becomes an internal jump to the merged block.
  rt::LinkGuestCalls(&merged);
  REQUIRE(ir::ValidateIr(merged).ok());
  bool found_jump = false;
  for (const ir::Block& b : merged.blocks) {
    if (b.terminator.kind == ir::TerminatorKind::kJump) found_jump = true;
  }
  CHECK(found_jump);
}

// ===========================================================================
// Multi-module end-to-end: guest->guest call then HLE call.
// ===========================================================================
namespace {

// Builds main's text:
//   +00 mov rdi,20      (48 C7 C7 14 00 00 00)
//   +07 mov rsi,22      (48 C7 C6 16 00 00 00)
//   +0E call libmath    (E8 rel32)
//   +13 mov rdi,rax     (48 89 C7)
//   +16 mov rsi,16      (48 C7 C6 10 00 00 00)
//   +1D call hle        (E8 rel32)
//   +22 nop
std::vector<std::uint8_t> MainText(std::uint64_t libmath_va,
                                   std::uint64_t hle_va,
                                   std::uint64_t main_text_va) {
  namespace rd = p5emu::test::reloc_detail;
  std::vector<std::uint8_t> t(35, 0);
  t[0] = 0x48; t[1] = 0xC7; t[2] = 0xC7; rd::Put32(t, 3, 20);
  t[7] = 0x48; t[8] = 0xC7; t[9] = 0xC6; rd::Put32(t, 10, 22);
  t[14] = 0xE8;
  rd::Put32(t, 15,
            static_cast<std::uint32_t>(static_cast<std::int64_t>(libmath_va) -
                                       static_cast<std::int64_t>(main_text_va + 0x13)));
  t[19] = 0x48; t[20] = 0x89; t[21] = 0xC7;  // mov rdi, rax
  t[22] = 0x48; t[23] = 0xC7; t[24] = 0xC6; rd::Put32(t, 25, 16);
  t[29] = 0xE8;
  rd::Put32(t, 30,
            static_cast<std::uint32_t>(static_cast<std::int64_t>(hle_va) -
                                       static_cast<std::int64_t>(main_text_va + 0x22)));
  t[34] = 0x90;  // nop: return block after the second call
  return t;
}

}  // namespace

P5EMU_TEST(RuntimeMultiModule, GuestToGuestThenHleReachesKnownResult) {
  // Load order is dependencies first, so libmath gets slot 0 and main slot 1.
  const std::uint64_t lib_bias = rt::LoadBiasAllocator::kBase;
  const std::uint64_t main_bias =
      rt::LoadBiasAllocator::kBase + rt::LoadBiasAllocator::kStride;
  const std::uint64_t main_text_va = main_bias + 0x400000;
  const std::uint64_t lib_text_va = lib_bias + 0x400000;

  // libmath: mov rax,rdi ; add rax,rsi ; ret  -> returns a+b.
  RelocFixtureOptions lib;
  lib.text = {0x48, 0x89, 0xF8, 0x48, 0x01, 0xF0, 0xC3};
  lib.data = {0};
  lib.symbols.push_back(RelocSym{"libmath_add", 0x400000, false});  // defined export
  const auto libfx = BuildRelocElf64(lib);

  // main: direct call to libmath export and to the HLE trampoline.
  RelocFixtureOptions main;
  main.text = MainText(lib_text_va, kTBase, main_text_va);
  main.data.assign(16, 0);
  main.symbols.push_back(RelocSym{"p5emu_add", 0, true});
  main.rela_dyn.push_back(RelocEntry{main.data_vaddr, 6 /*GLOB_DAT*/, 1, 0});
  const auto mainfx = BuildRelocElf64(main);

  hle::HleRegistry reg;
  p5emu::test::RegisterCanonical(&reg);

  rt::InMemoryModuleRepository repo;
  repo.Add("main", mainfx.bytes);
  repo.Add("libmath_test", libfx.bytes);

  std::map<std::string, std::vector<std::string>> deps = {
      {"main", {"libmath_test"}}, {"libmath_test", {}}};

  rt::GuestStack stack;
  REQUIRE(stack.Create().ok());

  rt::LoadedProcess proc;
  const Status st = rt::LoadProcess(repo, "main", deps, stack, &reg, {}, &proc);
  if (!st.ok()) {
    std::string msg = std::string("LoadProcess failed: ") + st.name();
    for (const std::string& b : proc.blockers) msg += " | " + b;
    p5emu::test::ReportFailure(__FILE__, __LINE__, msg);
    return;
  }
  REQUIRE(proc.execution_ready);
  REQUIRE(proc.image.modules.size() == 2);

  // The import slot was bound to the HLE trampoline address.
  std::uint64_t slot = 0;
  REQUIRE(proc.memory.ReadData(main_bias + 0x401000, 8, &slot).ok());
  CHECK_EQ(slot, kTBase);

  // Execute the merged program.
  hle::HleDispatcher dispatcher(&reg);
  dispatcher.EnableTrace(true);
  cpu::CpuState state;
  state.rip = proc.image.entry;
  state.rsp = stack.top() - 16;
  cpu::ReferenceInterpreter interp(kStepLimit, &dispatcher);
  const cpu::ExecutionResult res = interp.Execute(proc.program, &state, proc.memory);

  // guest: libmath_add(20,22)=42 ; hle: p5emu_add(42,16)=58.
  CHECK_EQ(state.rax, std::uint64_t{58});
  // exactly one HLE dispatch occurred.
  CHECK_EQ(dispatcher.trace().size(), std::size_t{1});
  (void)res;

  // The AArch64 backend must reach the same result through MULTI-REGION
  // transport: proc.memory exposes the two module segments, the stack, and the
  // HLE trampoline region as separate regions with distinct permissions.
  cpu::QemuArm64Runner runner(HarnessPath());
  if (runner.available()) {
    REQUIRE(proc.memory.RegionCount() >= 2);
    cpu::CpuState st2;
    st2.rip = proc.image.entry;
    st2.rsp = stack.top() - 16;
    std::unique_ptr<cpu::Backend> backend =
        cpu::MakeArm64Backend(kStepLimit, &runner, {});
    const cpu::ExecutionResult r2 =
        backend->Execute(proc.program, &st2, proc.memory);
    CHECK_EQ(st2.rax, std::uint64_t{58});
    CHECK_EQ(static_cast<int>(r2.reason), static_cast<int>(res.reason));
  }
}

// ===========================================================================
// Stack-argument HLE.
// ===========================================================================
P5EMU_TEST(RuntimeHleStackArgs, Sum10UsesRegisterAndStackArguments) {
  hle::HleRegistry reg;
  p5emu::test::RegisterCanonical(&reg);
  const std::uint64_t t_sum10 = kTBase + 24;

  // Build main: mov rdi..r9 with 1..6, push args 7..10 (values 7,8,9,10) so the
  // stack holds [rsp]=7,[rsp+8]=8,[rsp+16]=9,[rsp+24]=10 before the call, then
  // call p5emu_sum10 ; add rax,0 ; nop.
  // Simpler: use hand-built IR so the stack layout is explicit.
  ir::Builder b("sum10");
  const ir::BlockId entry = b.CreateBlock(0x40000000);
  b.SetEntryBlock(entry);
  b.EmitSetReg(ir::ArchRegister::kRdi, b.EmitMaterializeConstant(1, ir::ValueType::kI64));
  b.EmitSetReg(ir::ArchRegister::kRsi, b.EmitMaterializeConstant(2, ir::ValueType::kI64));
  b.EmitSetReg(ir::ArchRegister::kRdx, b.EmitMaterializeConstant(3, ir::ValueType::kI64));
  b.EmitSetReg(ir::ArchRegister::kRcx, b.EmitMaterializeConstant(4, ir::ValueType::kI64));
  b.EmitSetReg(ir::ArchRegister::kR8, b.EmitMaterializeConstant(5, ir::ValueType::kI64));
  b.EmitSetReg(ir::ArchRegister::kR9, b.EmitMaterializeConstant(6, ir::ValueType::kI64));
  // Push stack args 10,9,8,7 (highest first) so [rsp]=7.
  for (std::uint64_t v : {10ull, 9ull, 8ull, 7ull}) {
    const ir::ValueId sp = b.EmitGetReg(ir::ArchRegister::kRsp);
    const ir::ValueId nsp = b.EmitBinary(ir::OpKind::kSub, sp,
                                         b.EmitMaterializeConstant(8, ir::ValueType::kI64));
    b.EmitStore(nsp, b.EmitMaterializeConstant(v, ir::ValueType::kI64), 8);
    b.EmitSetReg(ir::ArchRegister::kRsp, nsp);
  }
  // The CALL itself pushes the return address (the external-call terminator
  // assumes the guest CALL did this).
  {
    const ir::ValueId sp = b.EmitGetReg(ir::ArchRegister::kRsp);
    const ir::ValueId nsp = b.EmitBinary(ir::OpKind::kSub, sp,
                                         b.EmitMaterializeConstant(8, ir::ValueType::kI64));
    b.EmitStore(nsp, b.EmitMaterializeConstant(0x40010000, ir::ValueType::kI64), 8);
    b.EmitSetReg(ir::ArchRegister::kRsp, nsp);
  }
  const ir::BlockId retblk = b.CreateBlock(0x40010000);
  b.SetCurrentBlock(entry);
  b.EmitExternalCall(t_sum10, retblk);
  b.SetCurrentBlock(retblk);
  b.EmitReturn();
  const ir::Program program = b.program();

  memory::LinearGuestMemory mem =
      memory::LinearGuestMemory::MakeCodeAndData(0x40000000, 0x20000);
  cpu::CpuState state;
  state.rip = 0x40000000;
  state.rsp = 0x4001F000;
  hle::HleDispatcher dispatcher(&reg);
  cpu::ReferenceInterpreter interp(kStepLimit, &dispatcher);
  interp.Execute(program, &state, mem);

  // 1+2+3+4+5+6 (registers) + 7+8+9+10 (stack) = 55.
  CHECK_EQ(state.rax, std::uint64_t{55});

  // AArch64 must agree (the harness reads the same stack arguments).
  cpu::QemuArm64Runner runner(HarnessPath());
  if (runner.available()) {
    cpu::Arm64CodeGenResult cg;
    REQUIRE(cpu::GenerateArm64(program, &cg).ok());
    cpu::Arm64BatchCase bc;
    bc.code = cg.code;
    bc.ctx.gpr[7] = 0x4001F000;  // rsp
    bc.ctx.rip = 0x40000000;
    bc.ctx.steps = kStepLimit;
    bc.mem_base = 0x40000000;
    bc.mem_in.assign(0x20000, 0);
    std::vector<cpu::Arm64BatchResult> results;
    std::string detail;
    REQUIRE(runner.RunBatch({bc}, &results, &detail).ok());
    CHECK_EQ(results[0].ctx.gpr[0], std::uint64_t{55});
  }
}

// ===========================================================================
// Negative proofs.
// ===========================================================================
P5EMU_TEST(RuntimeNegative, ReversedDependencyOrderIsCaught) {
  std::map<std::string, std::vector<std::string>> deps = {
      {"main", {"lib"}}, {"lib", {}}};
  rt::DependencyGraph good;
  REQUIRE(rt::BuildDependencyGraph("main", deps, {}, &good).ok());
  CHECK_EQ(good.order.front(), std::string("lib"));
  CHECK_EQ(good.order.back(), std::string("main"));

  rt::GraphOptions bad;
  bad.inject_reverse_order = true;
  rt::DependencyGraph rev;
  REQUIRE(rt::BuildDependencyGraph("main", deps, bad, &rev).ok());
  // Dependents before dependencies: the defect is visible.
  CHECK(rev.order != good.order);
}

P5EMU_TEST(RuntimeNegative, OverlapAllocatorIsCaught) {
  rt::LoadBiasAllocator good;
  std::uint64_t a = 0, b = 0;
  REQUIRE(good.Allocate(0x1000, &a).ok());
  REQUIRE(good.Allocate(0x1000, &b).ok());
  CHECK(a != b);

  rt::LoadBiasAllocator bad;
  bad.SetInjectOverlap(true);
  std::uint64_t c = 0, d = 0;
  REQUIRE(bad.Allocate(0x1000, &c).ok());
  REQUIRE(bad.Allocate(0x1000, &d).ok());
  CHECK_EQ(c, d);  // the defect
}

P5EMU_TEST(RuntimeNegative, WrongSeventhArgumentOffsetIsCaught) {
  // A deliberately wrong stack-offset read of argument index 6 must differ.
  cpu::CpuState s;
  s.rsp = 0x1000;
  memory::LinearGuestMemory mem =
      memory::LinearGuestMemory::MakeCodeAndData(0x1000, 0x1000);
  REQUIRE(mem.WriteData(0x1008, 8, 100).ok());  // correct location of arg 6
  REQUIRE(mem.WriteData(0x1010, 8, 999).ok());  // the wrong location
  hle::HleCallContext ctx(&s, &mem);
  CHECK_EQ(ctx.Arg(6), std::uint64_t{100});  // [rsp+8]
  CHECK(ctx.Arg(6) != 999);                  // the wrong offset would read 999
}

P5EMU_TEST(RuntimeReadiness, UnresolvedImportBlocksReadiness) {
  RelocFixtureOptions main;
  main.text = {0x90};
  main.data.assign(8, 0);
  main.symbols.push_back(RelocSym{"missing_symbol", 0, true});
  main.rela_dyn.push_back(RelocEntry{main.data_vaddr, 6, 1, 0});
  const auto fx = BuildRelocElf64(main);

  hle::HleRegistry reg;  // does not know missing_symbol
  rt::InMemoryModuleRepository repo;
  repo.Add("main", fx.bytes);
  std::map<std::string, std::vector<std::string>> deps = {{"main", {}}};
  rt::GuestStack stack;
  REQUIRE(stack.Create().ok());

  rt::LoadedProcess proc;
  const Status st = rt::LoadProcess(repo, "main", deps, stack, &reg, {}, &proc);
  CHECK(!st.ok());
  CHECK(!proc.execution_ready);
  CHECK(!proc.blockers.empty());
}

}  // namespace
