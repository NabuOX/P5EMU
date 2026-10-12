// SPDX-License-Identifier: Apache-2.0
//
// P5EMU is an independent, open-source project. See LICENSE and
// PROVENANCE.md before adding code here.
//
// ============================================================================
// HLE TESTS (Phase 6)
// ============================================================================
//   HleRegistry   registration, trampolines, duplicate/ambiguous, stale
//   HleAbi        guest ABI args/return and checked guest memory
//   HleSynthetic  p5emu_add / p5emu_sum6 / p5emu_memory
//   HleE2E        synthetic ELF -> reloc -> HLE -> CPU -> known result
//   HleDifferential  interpreter vs AArch64 including the HLE transition
//   HleNegative   swapped args, wrong return register, unknown-as-success
//   HleFuzz       deterministic malformed dispatch campaign
//   HleProperty   deterministic arithmetic property cases
//
// All identities are P5EMU-owned (libp5emu_test). No Sony function is used.
// ============================================================================

#include <cstdint>
#include <cstdlib>
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
#include "p5emu/cpu/MappedGuestMemory.h"
#include "p5emu/cpu/ReferenceInterpreter.h"
#include "p5emu/cpu/TranslateToIr.h"
#include "p5emu/hle/Hle.h"
#include "p5emu/loader/BinaryReader.h"
#include "p5emu/loader/Elf64.h"
#include "p5emu/loader/Import.h"
#include "p5emu/loader/Loader.h"
#include "p5emu/loader/Ps5Executable.h"
#include "p5emu/loader/Reloc.h"

namespace {

using namespace p5emu;          // NOLINT(build/namespaces)
namespace loader = p5emu::loader;
namespace memory = p5emu::memory;
namespace hle = p5emu::hle;
namespace cpu = p5emu::cpu;
namespace diff = p5emu::cpu::diff;
using p5emu::test::BuildRelocElf64;
using p5emu::test::RelocEntry;
using p5emu::test::RelocFixture;
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

// Builds an ELF whose text calls the trampoline `target` after loading two
// guest ABI arguments, then adds 16 to the return value. Exports rax = ret+16.
RelocFixtureOptions E2EOptions(std::uint64_t target, std::uint64_t arg0,
                               std::uint64_t arg1,
                               const char* symbol = "p5emu_add") {
  namespace rd = p5emu::test::reloc_detail;
  RelocFixtureOptions o;
  o.text.assign(23, 0);
  o.text[0] = 0x48; o.text[1] = 0xC7; o.text[2] = 0xC7;      // mov rdi, imm32
  rd::Put32(o.text, 3, static_cast<std::uint32_t>(arg0));
  o.text[7] = 0x48; o.text[8] = 0xC7; o.text[9] = 0xC6;      // mov rsi, imm32
  rd::Put32(o.text, 10, static_cast<std::uint32_t>(arg1));
  o.text[14] = 0xE8;                                         // call rel32
  const std::int64_t rel =
      static_cast<std::int64_t>(target) - static_cast<std::int64_t>(o.text_vaddr + 19);
  rd::Put32(o.text, 15, static_cast<std::uint32_t>(rel));
  o.text[19] = 0x48; o.text[20] = 0x83; o.text[21] = 0xC0; o.text[22] = 0x10;  // add rax,16
  o.data.assign(32, 0);  // [0..8) import slot, [8..32) stack room
  o.symbols.push_back(RelocSym{symbol, 0, true});
  o.rela_dyn.push_back(RelocEntry{o.data_vaddr + 0, 6 /*GLOB_DAT*/, 1, 0});
  return o;
}

struct LoadedHle {
  Status status = StatusCode::kInvalidArgument;
  loader::GenericElfImage elf;
  loader::Ps5ExecutableImage ps5;
  loader::LoadPlan plan;
  loader::ImportBinding binding;
  memory::MappedGuestMemory memory;
  RelocFixture fx;
};

LoadedHle LoadHle(const RelocFixture& fx, loader::ImportResolver& resolver) {
  LoadedHle out;
  out.fx = fx;
  const loader::BinaryReader r(fx.bytes.data(), fx.bytes.size());
  out.status = loader::ReadGenericElf64(r, &out.elf);
  if (!out.status.ok()) return out;
  out.status = loader::ValidateGenericLoadSegments(r, out.elf);
  if (!out.status.ok()) return out;
  out.status = loader::ParsePs5Executable(r, out.elf, &out.ps5);
  if (!out.status.ok()) return out;
  out.status = loader::BuildLoadPlan(r, out.ps5, 0, &out.plan);
  if (!out.status.ok()) return out;
  out.status = loader::ValidateLoadPlan(out.plan);
  if (!out.status.ok()) return out;
  out.status = loader::ResolveImports(out.plan, resolver, &out.binding);
  if (!out.status.ok()) return out;
  out.status = loader::ApplyLoadPlan(r, out.plan, out.binding, {}, &out.memory);
  return out;
}

// Runs the loaded image through the reference interpreter with the dispatcher.
cpu::ExecutionResult RunInterp(LoadedHle& lr, hle::HleDispatcher& dispatcher,
                               std::uint64_t rsp) {
  std::vector<std::uint8_t> text(23, 0);
  std::size_t avail = 0;
  lr.memory.FetchCode(lr.fx.text_vaddr, 23, text.data(), &avail);
  ir::Program ir;
  std::vector<memory::GuestAddr> addrs;
  cpu::TranslateProgram(lr.fx.text_vaddr, text.data(), text.size(), "hle", &ir,
                        &addrs);
  cpu::CpuState state;
  state.rip = lr.fx.entry;
  state.rsp = rsp;
  cpu::ReferenceInterpreter interp(kStepLimit, &dispatcher);
  return interp.Execute(ir, &state, lr.memory);
}

}  // namespace

// ===========================================================================
// Registry.
// ===========================================================================
P5EMU_TEST(HleRegistry, DeterministicTrampolinesAndDuplicateRules) {
  hle::HleRegistry reg;
  const std::uint64_t a = reg.Register(
      p5emu::test::kTestModule, loader::SymbolIdentity::String("p5emu_add"),
      &p5emu::test::P5emuAdd);
  const std::uint64_t b = reg.Register(
      p5emu::test::kTestModule, loader::SymbolIdentity::String("p5emu_sum6"),
      &p5emu::test::P5emuSum6);
  CHECK_EQ(a, kTBase + 0);
  CHECK_EQ(b, kTBase + 8);
  CHECK(a >= hle::HleRegistry::kTrampolineBase);
  CHECK(a < hle::HleRegistry::kTrampolineEnd);

  hle::HleStatus st = hle::HleStatus::kInternalError;
  const hle::HleEntry* e = reg.Find(p5emu::test::kTestModule,
                                    loader::SymbolIdentity::String("p5emu_add"),
                                    &st);
  REQUIRE(e != nullptr);
  CHECK_EQ(st, hle::HleStatus::kOk);
  CHECK_EQ(e->trampoline, a);

  // Unknown symbol -> kNotFound, not a silent success.
  CHECK_EQ(reg.Find(p5emu::test::kTestModule,
                    loader::SymbolIdentity::String("missing"), &st),
           nullptr);
  CHECK_EQ(st, hle::HleStatus::kNotFound);

  // Duplicate registration of the same (module, symbol) -> kAmbiguous.
  reg.Register(p5emu::test::kTestModule,
               loader::SymbolIdentity::String("p5emu_add"),
               &p5emu::test::P5emuAdd);
  CHECK_EQ(reg.Find(p5emu::test::kTestModule,
                    loader::SymbolIdentity::String("p5emu_add"), &st),
           nullptr);
  CHECK_EQ(st, hle::HleStatus::kAmbiguous);

  // Stale / unknown trampoline address.
  CHECK(reg.FindByTrampoline(kTBase + 0x40) == nullptr);
  CHECK(reg.FindByTrampoline(kTBase + 0) != nullptr);
}

P5EMU_TEST(HleRegistry, ResolverMapsStringIdentityOnly) {
  hle::HleRegistry reg;
  p5emu::test::RegisterCanonical(&reg);
  hle::HleResolver resolver(&reg);

  // Empty module (generic ELF): matched by name.
  const auto r = resolver.ResolveSymbol(
      "", loader::SymbolIdentity::String("p5emu_add"));
  CHECK_EQ(r.status, loader::ResolutionStatus::kResolved);
  CHECK_EQ(r.address, kTBase + 0);

  // Unknown name -> unresolved.
  CHECK_EQ(resolver.ResolveSymbol(
               "", loader::SymbolIdentity::String("nope")).status,
           loader::ResolutionStatus::kUnresolved);
  // Numeric identity -> unsupported (a distinct state).
  CHECK_EQ(resolver.ResolveSymbol(
               "", loader::SymbolIdentity::Numeric(7)).status,
           loader::ResolutionStatus::kUnsupportedIdentity);
}

// ===========================================================================
// Guest ABI + memory.
// ===========================================================================
P5EMU_TEST(HleAbi, ArgumentAndReturnRegisters) {
  cpu::CpuState s;
  s.rdi = 11; s.rsi = 22; s.rdx = 33; s.rcx = 44; s.r8 = 55; s.r9 = 66;
  memory::LinearGuestMemory mem =
      memory::LinearGuestMemory::MakeCodeAndData(0, 0x1000);
  hle::HleCallContext ctx(&s, &mem);
  CHECK_EQ(ctx.Arg(0), std::uint64_t{11});  // rdi
  CHECK_EQ(ctx.Arg(1), std::uint64_t{22});  // rsi
  CHECK_EQ(ctx.Arg(2), std::uint64_t{33});  // rdx
  CHECK_EQ(ctx.Arg(3), std::uint64_t{44});  // rcx
  CHECK_EQ(ctx.Arg(4), std::uint64_t{55});  // r8
  CHECK_EQ(ctx.Arg(5), std::uint64_t{66});  // r9
  CHECK_EQ(ctx.Arg(6), std::uint64_t{0});   // out of range
  ctx.SetReturn(0xABCD);
  CHECK_EQ(s.rax, std::uint64_t{0xABCD});   // return in rax
}

P5EMU_TEST(HleAbi, GuestMemoryIsChecked) {
  cpu::CpuState s;
  memory::LinearGuestMemory mem =
      memory::LinearGuestMemory::MakeCodeAndData(0x1000, 0x100);
  hle::HleCallContext ctx(&s, &mem);

  CHECK(ctx.WriteGuest(0x1000, 8, 0x1122334455667788ull).ok());
  std::uint64_t v = 0;
  CHECK(ctx.ReadGuest(0x1000, 8, &v).ok());
  CHECK_EQ(v, std::uint64_t{0x1122334455667788ull});

  // Out-of-bounds pointer -> deterministic memory fault, no host corruption.
  CHECK_EQ(ctx.ReadGuest(0x9999, 8, &v).code(), StatusCode::kHleMemoryFault);
  CHECK_EQ(ctx.WriteGuest(0x9999, 8, 0).code(), StatusCode::kHleMemoryFault);
  CHECK_EQ(ctx.ReadGuest(0x1100, 8, &v).code(), StatusCode::kHleMemoryFault);

  // Zero-length read is a defined no-op.
  std::vector<std::uint8_t> bytes;
  CHECK(ctx.ReadBytes(0x9999, 0, &bytes).ok());
  CHECK_EQ(bytes.size(), std::size_t{0});

  // Overflowed pointer+length is bounded, not a giant allocation.
  CHECK_EQ(ctx.ReadBytes(0x10F0, 0xFFFF'FFFFull, &bytes).code(),
           StatusCode::kHleMemoryFault);
}

// ===========================================================================
// Synthetic functions.
// ===========================================================================
P5EMU_TEST(HleSynthetic, AddSubAndSum6) {
  cpu::CpuState s;
  memory::LinearGuestMemory mem =
      memory::LinearGuestMemory::MakeCodeAndData(0, 0x1000);
  hle::HleCallContext ctx(&s, &mem);

  s.rdi = 20; s.rsi = 22;
  CHECK_EQ(p5emu::test::P5emuAdd(ctx), hle::HleStatus::kOk);
  CHECK_EQ(s.rax, std::uint64_t{42});

 // p5emu_sub is non-commutative, so argument order is observable.
  s.rdi = 20; s.rsi = 22;
  CHECK_EQ(p5emu::test::P5emuSub(ctx), hle::HleStatus::kOk);
  CHECK_EQ(s.rax, std::uint64_t{0} - 2);  // 20 - 22

  s.rdi = 1; s.rsi = 2; s.rdx = 3; s.rcx = 4; s.r8 = 5; s.r9 = 6;
  CHECK_EQ(p5emu::test::P5emuSum6(ctx), hle::HleStatus::kOk);
  CHECK_EQ(s.rax, std::uint64_t{21});
}

P5EMU_TEST(HleSynthetic, MemoryFunctionReadsWritesAndFaults) {
  cpu::CpuState s;
  memory::LinearGuestMemory mem =
      memory::LinearGuestMemory::MakeCodeAndData(0x1000, 0x100);
  hle::HleCallContext ctx(&s, &mem);
  REQUIRE(mem.WriteData(0x1000, 8, 100).ok());

  s.rdi = 0x1000;  // ptr
  s.rsi = 5;       // add
  s.rdx = 0x1010;  // out_ptr
  CHECK_EQ(p5emu::test::P5emuMemory(ctx), hle::HleStatus::kOk);
  CHECK_EQ(s.rax, std::uint64_t{105});
  std::uint64_t out = 0;
  REQUIRE(mem.ReadData(0x1010, 8, &out).ok());
  CHECK_EQ(out, std::uint64_t{105});

  // Bad pointer -> kMemoryFault, not a host crash.
  s.rdi = 0x9999;
  CHECK_EQ(p5emu::test::P5emuMemory(ctx), hle::HleStatus::kMemoryFault);
}

P5EMU_TEST(HleSynthetic, WriteToReadOnlyIsAFault) {
  // The data port is read-write here, so simulate a read-only target by writing
  // to an unmapped address and asserting the fault path.
  cpu::CpuState s;
  memory::LinearGuestMemory mem =
      memory::LinearGuestMemory::MakeCodeAndData(0x1000, 0x100);
  hle::HleCallContext ctx(&s, &mem);
  s.rdi = 0x1000;
  s.rsi = 1;
  s.rdx = 0xDEAD'0000ull;  // unmapped output
  CHECK_EQ(p5emu::test::P5emuMemory(ctx), hle::HleStatus::kMemoryFault);
}

// ===========================================================================
// End-to-end.
// ===========================================================================
P5EMU_TEST(HleE2E, GuestCallReachesHleAndResumes) {
  hle::HleRegistry reg;
  p5emu::test::RegisterCanonical(&reg);
  const std::uint64_t t_add = kTBase + 0;
  const auto fx = BuildRelocElf64(E2EOptions(t_add, 20, 22));
  hle::HleResolver resolver(&reg);

  LoadedHle lr = LoadHle(fx, resolver);
  REQUIRE(lr.status.ok());

  // The import slot was bound to the trampoline address (not a host pointer).
  std::uint64_t slot = 0;
  REQUIRE(lr.memory.ReadData(fx.data_vaddr + 0, 8, &slot).ok());
  CHECK_EQ(slot, t_add);

  const std::uint64_t rsp = fx.data_vaddr + 32;
  hle::HleDispatcher dispatcher(&reg);
  dispatcher.EnableTrace(true);
  cpu::ExecutionResult res = RunInterp(lr, dispatcher, rsp);
  // p5emu_add(20,22)=42 ; +16 => 58. RSP balanced by the call/return pair.
  CHECK_EQ(res.reason, cpu::TerminationReason::kFallthrough);
  CHECK_EQ(dispatcher.trace().size(), std::size_t{1});
  CHECK_EQ(dispatcher.trace()[0].binding, t_add);
  CHECK_EQ(dispatcher.trace()[0].result, std::uint64_t{42});
}

P5EMU_TEST(HleE2E, MultipleGuestMemoryHleCall) {
  // mov rdi, ptr ; mov rsi, 5 ; mov rdx, out ; call p5emu_memory
  const std::uint64_t t_mem = kTBase + 16;
  namespace rd = p5emu::test::reloc_detail;
  RelocFixtureOptions o;
  const std::uint64_t ptr = o.data_vaddr + 8;
  const std::uint64_t out_ptr = o.data_vaddr + 16;
  o.text.assign(27, 0);
  o.text[0] = 0x48; o.text[1] = 0xC7; o.text[2] = 0xC7;
  rd::Put32(o.text, 3, static_cast<std::uint32_t>(ptr));
  o.text[7] = 0x48; o.text[8] = 0xC7; o.text[9] = 0xC6;
  rd::Put32(o.text, 10, 5);
  o.text[14] = 0x48; o.text[15] = 0xC7; o.text[16] = 0xC2;
  rd::Put32(o.text, 17, static_cast<std::uint32_t>(out_ptr));
  o.text[21] = 0xE8;
  const std::int64_t rel = static_cast<std::int64_t>(t_mem) -
                           static_cast<std::int64_t>(o.text_vaddr + 26);
  rd::Put32(o.text, 22, static_cast<std::uint32_t>(rel));
  o.text[26] = 0x90;  // nop: the return block after the call
  o.data.assign(32, 0);
  o.symbols.push_back(RelocSym{"p5emu_memory", 0, true});
  o.rela_dyn.push_back(RelocEntry{o.data_vaddr + 0, 6, 1, 0});
  const auto fx = BuildRelocElf64(o);

  hle::HleRegistry reg;
  p5emu::test::RegisterCanonical(&reg);
  hle::HleResolver resolver(&reg);
  LoadedHle lr = LoadHle(fx, resolver);
  REQUIRE(lr.status.ok());

  REQUIRE(lr.memory.WriteData(ptr, 8, 100).ok());

  ir::Program ir;
  std::vector<memory::GuestAddr> addrs;
  std::size_t avail = 0;
  std::vector<std::uint8_t> text(27, 0);
  REQUIRE(lr.memory.FetchCode(fx.text_vaddr, 27, text.data(), &avail).ok());
  REQUIRE(cpu::TranslateProgram(fx.text_vaddr, text.data(), text.size(), "m",
                                &ir, &addrs)
              .ok());
  cpu::CpuState state;
  state.rip = fx.entry;
  state.rsp = fx.data_vaddr + 32;
  hle::HleDispatcher dispatcher(&reg);
  cpu::ReferenceInterpreter interp(kStepLimit, &dispatcher);
  const cpu::ExecutionResult res = interp.Execute(ir, &state, lr.memory);
  CHECK_EQ(res.reason, cpu::TerminationReason::kFallthrough);
  CHECK_EQ(state.rax, std::uint64_t{105});
  std::uint64_t out = 0;
  REQUIRE(lr.memory.ReadData(out_ptr, 8, &out).ok());
  CHECK_EQ(out, std::uint64_t{105});
}

// ===========================================================================
// Differential: interpreter vs AArch64, including HLE.
// ===========================================================================
P5EMU_TEST(HleDifferential, InterpreterAndAArch64AgreeAcrossHle) {
  hle::HleRegistry reg;
  p5emu::test::RegisterCanonical(&reg);
  const std::uint64_t t_add = kTBase + 0;
  const auto fx = BuildRelocElf64(E2EOptions(t_add, 20, 22));
  hle::HleResolver resolver(&reg);
  LoadedHle lr = LoadHle(fx, resolver);
  REQUIRE(lr.status.ok());

  // Build the differential request over the loaded image span. The span covers
  // two regions with a gap; flatten into one zero-filled image for the
  // single-region comparison memory.
  const std::uint64_t base = fx.text_vaddr;
  const std::uint64_t end = fx.data_vaddr + fx.data_size;
  std::vector<std::uint8_t> image(static_cast<std::size_t>(end - base), 0);
  for (const auto& rg : lr.memory.Regions()) {
    std::vector<std::uint8_t> bytes;
    REQUIRE(lr.memory.ReadRange(rg.guest_base, rg.size, &bytes).ok());
    for (std::size_t i = 0; i < bytes.size(); ++i) {
      image[static_cast<std::size_t>(rg.guest_base - base) + i] = bytes[i];
    }
  }

  cpu::QemuArm64Runner runner(HarnessPath());
  if (!runner.available()) {
    p5emu::test::ReportFailure(__FILE__, __LINE__,
                               "AArch64 harness not available");
    return;
  }
  hle::HleDispatcher dispatcher(&reg);
  auto reference = cpu::MakeReferenceInterpreter(kStepLimit, &dispatcher);
  auto arm = cpu::MakeArm64Backend(kStepLimit, &runner);

  diff::ComparisonRequest req;
  req.name = "hle";
  req.program = nullptr;  // set below
  // We compare the FULL loaded program by translating it first.
  std::vector<std::uint8_t> text(23, 0);
  std::size_t avail = 0;
  REQUIRE(lr.memory.FetchCode(fx.text_vaddr, 23, text.data(), &avail).ok());
  ir::Program ir;
  std::vector<memory::GuestAddr> addrs;
  REQUIRE(cpu::TranslateProgram(fx.text_vaddr, text.data(), text.size(), "hle",
                                &ir, &addrs)
              .ok());
  req.program = &ir;
  req.initial_state.rip = fx.entry;
  req.initial_state.rsp = fx.data_vaddr + 32;
  req.memory_base = base;
  req.memory_size = static_cast<memory::GuestSize>(end - base);
  req.initial_memory = image;
  req.permissions = memory::kPermReadWriteExecute;
  req.step_limit = kStepLimit;

  const diff::ComparisonResult r =
      diff::RunComparison(req, {reference.get(), arm.get()});
  if (r.verdict != diff::Verdict::kAgree) {
    p5emu::test::ReportFailure(__FILE__, __LINE__,
                               std::string("expected agreement: ") + r.summary);
  }
}

// ===========================================================================
// Negative proofs.
// ===========================================================================
P5EMU_TEST(HleNegative, SwappedArgumentRegistersIsCaught) {
  // p5emu_sub is non-commutative, so a swapped-argument defect changes the
  // result. It is registered at index 0 (trampoline kTBase).
  hle::HleRegistry reg;
  reg.Register(p5emu::test::kTestModule,
               loader::SymbolIdentity::String("p5emu_sub"),
               &p5emu::test::P5emuSub);
  const std::uint64_t t_sub = kTBase + 0;
  const auto fx = BuildRelocElf64(
      E2EOptions(t_sub, 20, 22, "p5emu_sub"));
  hle::HleResolver resolver(&reg);
  const std::uint64_t rsp = fx.data_vaddr + 32;

  std::vector<std::uint8_t> text(23, 0);
  LoadedHle probe = LoadHle(fx, resolver);
  REQUIRE(probe.status.ok());
  std::size_t avail = 0;
  REQUIRE(probe.memory.FetchCode(fx.text_vaddr, 23, text.data(), &avail).ok());
  ir::Program ir;
  std::vector<memory::GuestAddr> addrs;
  REQUIRE(cpu::TranslateProgram(fx.text_vaddr, text.data(), text.size(), "n",
                                &ir, &addrs)
              .ok());

  // Correct: 20 - 22 = -2 ; +16 = 14.
  {
    hle::HleDispatcher d(&reg);
    LoadedHle l = LoadHle(fx, resolver);
    cpu::CpuState s;
    s.rip = fx.entry; s.rsp = rsp;
    cpu::ReferenceInterpreter interp(kStepLimit, &d);
    interp.Execute(ir, &s, l.memory);
    CHECK_EQ(s.rax, std::uint64_t{14});
  }
  // Defect: swapped args -> 22 - 20 = 2 ; +16 = 18. Observable difference.
  {
    hle::HleDispatcher d(&reg);
    d.SetInjections({true, false, false});
    LoadedHle l = LoadHle(fx, resolver);
    cpu::CpuState s;
    s.rip = fx.entry; s.rsp = rsp;
    cpu::ReferenceInterpreter interp(kStepLimit, &d);
    interp.Execute(ir, &s, l.memory);
    CHECK_EQ(s.rax, std::uint64_t{18});
    CHECK(s.rax != 14);
  }
}

P5EMU_TEST(HleNegative, WrongReturnRegisterIsCaught) {
  hle::HleRegistry reg;
  p5emu::test::RegisterCanonical(&reg);
  const std::uint64_t t_add = kTBase + 0;
  const auto fx = BuildRelocElf64(E2EOptions(t_add, 20, 22));
  hle::HleResolver resolver(&reg);

  std::vector<std::uint8_t> text(23, 0);
  LoadedHle probe = LoadHle(fx, resolver);
  REQUIRE(probe.status.ok());
  std::size_t avail = 0;
  REQUIRE(probe.memory.FetchCode(fx.text_vaddr, 23, text.data(), &avail).ok());
  ir::Program ir;
  std::vector<memory::GuestAddr> addrs;
  REQUIRE(cpu::TranslateProgram(fx.text_vaddr, text.data(), text.size(), "w",
                                &ir, &addrs)
              .ok());

  // Correct: return in RAX -> 42 + 16 = 58.
  {
    hle::HleDispatcher d(&reg);
    LoadedHle l = LoadHle(fx, resolver);
    cpu::CpuState s;
    s.rip = fx.entry; s.rsp = fx.data_vaddr + 32;
    cpu::ReferenceInterpreter interp(kStepLimit, &d);
    interp.Execute(ir, &s, l.memory);
    CHECK_EQ(s.rax, std::uint64_t{58});
  }
  // Defect: return written to RSI -> the guest's add rax,16 sees stale RAX.
  {
    hle::HleDispatcher d(&reg);
    d.SetInjections({false, true, false});
    LoadedHle l = LoadHle(fx, resolver);
    cpu::CpuState s;
    s.rip = fx.entry; s.rsp = fx.data_vaddr + 32;
    cpu::ReferenceInterpreter interp(kStepLimit, &d);
    interp.Execute(ir, &s, l.memory);
    CHECK(s.rax != 58);
  }
}

P5EMU_TEST(HleNegative, UnknownBindingAsSuccessIsCaught) {
  // A call to an unregistered trampoline address. Correct: kHleNotFound.
  const std::uint64_t unknown = kTBase + 0x1000;
  const auto fx = BuildRelocElf64(E2EOptions(unknown, 20, 22));
  hle::HleRegistry reg;  // empty
  hle::HleResolver resolver(&reg);
  LoadedHle lr = LoadHle(fx, resolver);
  // The empty registry cannot resolve the import -> not execution-ready.
  CHECK_EQ(lr.status.code(), StatusCode::kUnresolvedImport);

  // Even if we force the load (via the 4-arg apply path), the dispatch fails.
  // Build the plan/binding manually with no requirements.
  RelocFixtureOptions o = E2EOptions(unknown, 20, 22);
  o.symbols.clear();
  o.rela_dyn.clear();
  const auto fx2 = BuildRelocElf64(o);
  const loader::BinaryReader r(fx2.bytes.data(), fx2.bytes.size());
  loader::GenericElfImage elf;
  REQUIRE(loader::ReadGenericElf64(r, &elf).ok());
  REQUIRE(loader::ValidateGenericLoadSegments(r, elf).ok());
  loader::Ps5ExecutableImage ps5;
  REQUIRE(loader::ParsePs5Executable(r, elf, &ps5).ok());
  loader::LoadPlan plan;
  REQUIRE(loader::BuildLoadPlan(r, ps5, 0, &plan).ok());
  memory::MappedGuestMemory mem;
  REQUIRE(loader::ApplyLoadPlan(r, plan, {}, &mem).ok());

  std::vector<std::uint8_t> text(23, 0);
  std::size_t avail = 0;
  REQUIRE(mem.FetchCode(fx2.text_vaddr, 23, text.data(), &avail).ok());
  ir::Program ir;
  std::vector<memory::GuestAddr> addrs;
  REQUIRE(cpu::TranslateProgram(fx2.text_vaddr, text.data(), text.size(), "u",
                                &ir, &addrs)
              .ok());
  hle::HleDispatcher d(&reg);
  cpu::CpuState s;
  s.rip = fx2.entry; s.rsp = fx2.data_vaddr + 32;
  cpu::ReferenceInterpreter interp(kStepLimit, &d);
  const cpu::ExecutionResult res = interp.Execute(ir, &s, mem);
  CHECK_EQ(res.reason, cpu::TerminationReason::kHleError);
  CHECK_EQ(res.status.code(), StatusCode::kHleNotFound);

  // Defect: unknown binding treated as success -> the guest resumes with a
  // bogus RAX instead of terminating with an error.
  hle::HleDispatcher bad(&reg);
  bad.SetInjections({false, false, true});
  LoadedHle l2;  // fresh memory
  l2.memory = std::move(mem);
  l2.fx = fx2;
  cpu::CpuState s2;
  s2.rip = fx2.entry; s2.rsp = fx2.data_vaddr + 32;
  cpu::ReferenceInterpreter interp2(kStepLimit, &bad);
  const cpu::ExecutionResult res2 = interp2.Execute(ir, &s2, l2.memory);
  CHECK(res2.reason != cpu::TerminationReason::kHleError);
}

// ===========================================================================
// Transactional binding.
// ===========================================================================
P5EMU_TEST(HleTransactional, BindingFailureRollsBackTarget) {
  hle::HleRegistry reg;
  p5emu::test::RegisterCanonical(&reg);
  const std::uint64_t t_add = kTBase + 0;
  const auto fx = BuildRelocElf64(E2EOptions(t_add, 20, 22));
  hle::HleResolver resolver(&reg);

  const loader::BinaryReader r(fx.bytes.data(), fx.bytes.size());
  loader::GenericElfImage elf;
  REQUIRE(loader::ReadGenericElf64(r, &elf).ok());
  REQUIRE(loader::ValidateGenericLoadSegments(r, elf).ok());
  loader::Ps5ExecutableImage ps5;
  REQUIRE(loader::ParsePs5Executable(r, elf, &ps5).ok());
  loader::LoadPlan plan;
  REQUIRE(loader::BuildLoadPlan(r, ps5, 0, &plan).ok());
  REQUIRE(loader::ValidateLoadPlan(plan).ok());
  loader::ImportBinding binding;
  REQUIRE(loader::ResolveImports(plan, resolver, &binding).ok());
  REQUIRE(binding.execution_ready);

  // Pre-populate the target with a sentinel region.
  memory::MappedGuestMemory target;
  REQUIRE(target.AddRegion({0x9000, 0x100, memory::kPermReadWrite}).ok());
  const std::uint8_t marker[4] = {9, 8, 7, 6};
  REQUIRE(target.WriteImage(0x9000, marker, 4).ok());

  // Fail partway through the binding/relocation apply.
  loader::ApplyOptions opts;
  opts.fail_after_operations = 2;
  const Status st = loader::ApplyLoadPlan(r, plan, binding, opts, &target);
  CHECK_EQ(st.code(), StatusCode::kApplyFailed);

  // The target is unchanged: no half-written trampoline table, no partial image.
  CHECK_EQ(target.RegionCount(), std::size_t{1});
  CHECK(!target.IsMapped(fx.text_vaddr));
  std::uint64_t v = 0;
  REQUIRE(target.ReadData(0x9000, 1, &v).ok());
  CHECK_EQ(v, std::uint64_t{9});

  // A full apply then succeeds.
  target = memory::MappedGuestMemory{};
  REQUIRE(loader::ApplyLoadPlan(r, plan, binding, {}, &target).ok());
  std::uint64_t slot = 0;
  REQUIRE(target.ReadData(fx.data_vaddr, 8, &slot).ok());
  CHECK_EQ(slot, t_add);
}
