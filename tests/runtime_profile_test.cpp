// SPDX-License-Identifier: Apache-2.0
//
// P5EMU is an independent, open-source project. See LICENSE and
// PROVENANCE.md before adding code here.
//
// ============================================================================
// RUNTIME PROFILE / EVIDENCE BOUNDARY TESTS (Phase 8)
// ============================================================================
//   RuntimeProfileClassify     generic-vs-PS5 profile selection
//   RuntimeStartupFields       structured startup state, known/unknown
//   RuntimeStartupMaterialize  explicit refusal instead of fabrication
//   RuntimeTlsGap              TLS/thread-pointer gap fails explicitly
//   RuntimeLoadBiasEvidence    synthetic load-bias is not PS5 ASLR
//   RuntimeModuleMetadata      evidence-bounded module metadata
//   RuntimeInitOrder           synthetic vs PS5-unknown init ordering
//   RuntimeProfileE2E          loader -> startup -> CPU (interpreter + AArch64)
//   RuntimeProfileNegative     injected-boundary negative proofs
//   RuntimeProfileProperty     randomized evidence-boundary property campaign
// ============================================================================

#include <cstdint>
#include <cstdlib>
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
#include "p5emu/cpu/ReferenceInterpreter.h"
#include "p5emu/hle/Hle.h"
#include "p5emu/loader/Elf64.h"
#include "p5emu/loader/Ps5Executable.h"
#include "p5emu/runtime/Runtime.h"
#include "p5emu/runtime/RuntimeProfile.h"

namespace {

using namespace p5emu;  // NOLINT(build/namespaces)
namespace rt = p5emu::runtime;
namespace cpu = p5emu::cpu;
namespace loader = p5emu::loader;
namespace memory = p5emu::memory;

constexpr std::uint64_t kStepLimit = 1u << 20;

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

// A single-module fixture; text defaults to `mov eax, 42; ret`.
test::RelocFixture MakeExecutable(const std::string& export_name,
                                  std::uint8_t osabi = 0,
                                  std::uint16_t e_type = 3) {
  test::RelocFixtureOptions o;
  o.text = {0xB8, 0x2A, 0x00, 0x00, 0x00, 0xC3};
  o.data.assign(8, 0);
  o.symbols.push_back(test::RelocSym{export_name, o.text_vaddr, false});
  o.osabi = osabi;
  o.e_type = e_type;
  return test::BuildRelocElf64(o);
}

loader::Ps5ExecutableImage ParsePs5(const std::vector<std::uint8_t>& bytes) {
  loader::BinaryReader reader(bytes.data(), bytes.size());
  loader::GenericElfImage elf;
  loader::Ps5ExecutableImage ps5;
  (void)loader::ReadGenericElf64(reader, &elf);
  (void)loader::ParsePs5Executable(reader, elf, &ps5);
  return ps5;
}

// A startup state whose nine required fields are all set to `state`.
void SetAllRequired(rt::StartupState* s, rt::FieldState state) {
  auto f = [&]() -> rt::StartupField {
    switch (state) {
      case rt::FieldState::kKnown:
        return rt::StartupField::Known(0x1000, rt::EvidenceClass::kSynthetic);
      case rt::FieldState::kUnsupported:
        return rt::StartupField::Unsupported();
      case rt::FieldState::kUnknown:
      default:
        return rt::StartupField::Unknown();
    }
  };
  s->entry_rip = f();
  s->initial_rsp = f();
  s->initial_rbp = f();
  s->thread_pointer = f();
  s->argc = f();
  s->argv = f();
  s->envp = f();
  s->auxv = f();
  s->stack_alignment = f();
}

}  // namespace

// ===========================================================================
// Profile classification.
// ===========================================================================
P5EMU_TEST(RuntimeProfileClassify, GenericAndPs5DialectAreDistinct) {
  const test::RelocFixture generic = MakeExecutable("stub");
  const loader::Ps5ExecutableImage gps5 = ParsePs5(generic.bytes);
  CHECK_EQ(gps5.is_ps5_dialect, false);
  CHECK_EQ(static_cast<int>(rt::ClassifyRuntimeProfile(gps5.is_ps5_dialect)),
           static_cast<int>(rt::RuntimeProfileKind::kGenericSynthetic));

  // PS5 dialect: FreeBSD OSABI + the PS5 ET_DYNEXEC type.
  const test::RelocFixture ps5exec =
      MakeExecutable("stub", loader::kElfOsabiFreeBsd, loader::kPs5EtDynExec);
  const loader::Ps5ExecutableImage pimg = ParsePs5(ps5exec.bytes);
  CHECK_EQ(pimg.is_ps5_dialect, true);
  CHECK_EQ(static_cast<int>(rt::ClassifyRuntimeProfile(pimg.is_ps5_dialect)),
           static_cast<int>(rt::RuntimeProfileKind::kPs5));

  CHECK_EQ(std::string(rt::RuntimeProfileName(rt::RuntimeProfileKind::kPs5)),
           std::string("ps5"));
  CHECK_EQ(std::string(rt::RuntimeProfileName(
               rt::RuntimeProfileKind::kGenericSynthetic)),
           std::string("generic-synthetic"));
}

// ===========================================================================
// Structured startup state.
// ===========================================================================
P5EMU_TEST(RuntimeStartupFields, GenericAllKnownSyntheticPs5MostlyUnknown) {
  rt::ProcessStartup ps;
  ps.entry_rip = 0x400000;
  ps.initial_rsp = 0x7FFFFFF0;
  ps.argc = 0;
  const rt::StartupState g = rt::MakeGenericStartupState(ps);
  CHECK(g.is_synthetic_model());
  CHECK_EQ(static_cast<int>(g.profile),
           static_cast<int>(rt::RuntimeProfileKind::kGenericSynthetic));
  CHECK(g.entry_rip.known() && g.entry_rip.value == 0x400000);
  CHECK(g.initial_rsp.known());
  CHECK_EQ(static_cast<int>(g.entry_rip.evidence),
           static_cast<int>(rt::EvidenceClass::kSynthetic));

  const rt::StartupState p = rt::MakePs5StartupState(0x410000);
  CHECK(!p.is_synthetic_model());
  // Entry value is documented (from the ELF header)...
  CHECK(p.entry_rip.known());
  CHECK_EQ(p.entry_rip.value, std::uint64_t{0x410000});
  CHECK_EQ(static_cast<int>(p.entry_rip.evidence),
           static_cast<int>(rt::EvidenceClass::kDocumented));
  // ...but the rest of the PS5 startup state is UNKNOWN.
  CHECK_EQ(static_cast<int>(p.initial_rsp.state),
           static_cast<int>(rt::FieldState::kUnknown));
  CHECK_EQ(static_cast<int>(p.argc.state),
           static_cast<int>(rt::FieldState::kUnknown));
  CHECK_EQ(static_cast<int>(p.thread_pointer.state),
           static_cast<int>(rt::FieldState::kUnknown));
}

// ===========================================================================
// Materialisation refusals.
// ===========================================================================
P5EMU_TEST(RuntimeStartupMaterialize, GenericSucceedsPs5Refused) {
  rt::ProcessStartup ps;
  ps.entry_rip = 0x400000;
  ps.initial_rsp = 0x7FFFFFF0;
  const rt::StartupState g = rt::MakeGenericStartupState(ps);
  rt::ProcessStartup out;
  std::vector<std::string> blockers;
  REQUIRE(rt::MaterializeStartup(g, &out, &blockers).ok());
  CHECK_EQ(out.entry_rip, std::uint64_t{0x400000});
  CHECK_EQ(out.initial_rsp, std::uint64_t{0x7FFFFFF0});
  CHECK(blockers.empty());

  const rt::StartupState p = rt::MakePs5StartupState(0x410000);
  rt::ProcessStartup out2;
  out2.entry_rip = 0xDEAD;  // must be left untouched on failure
  std::vector<std::string> b2;
  const Status st = rt::MaterializeStartup(p, &out2, &b2);
  CHECK_EQ(static_cast<int>(st.code()),
           static_cast<int>(StatusCode::kUnknownRuntimeField));
  CHECK(!b2.empty());
  CHECK_EQ(out2.entry_rip, std::uint64_t{0xDEAD});  // no fabrication
  // The entry value must NOT appear as a fabricated process startup.
  CHECK(out2.initial_rsp == 0);
}

// ===========================================================================
// TLS / thread-pointer gap.
// ===========================================================================
P5EMU_TEST(RuntimeTlsGap, UnsupportedThreadPointerFailsExplicitly) {
  rt::ProcessStartup ps;
  ps.entry_rip = 1;
  ps.initial_rsp = 2;
  const rt::StartupState g = rt::MakeGenericStartupState(ps);
  CHECK(rt::CheckThreadPointerSupport(g).ok());

  const rt::StartupState p = rt::MakePs5StartupState(0x410000);
  CHECK_EQ(static_cast<int>(rt::CheckThreadPointerSupport(p).code()),
           static_cast<int>(StatusCode::kRuntimeProfileUnsupported));
}

// ===========================================================================
// Load-bias evidence.
// ===========================================================================
P5EMU_TEST(RuntimeLoadBiasEvidence, SyntheticAllocatorIsNotPs5Aslr) {
  // The Phase-7 allocator is a SYNTHETIC test layout...
  CHECK_EQ(rt::LoadBiasAllocator::kBase, std::uint64_t{0x2000'0000ull});
  CHECK_EQ(rt::LoadBiasAllocator::kStride, std::uint64_t{0x1000'0000ull});
  // ...and PS5 load-address policy remains UNKNOWN.
  const rt::StartupField bias = rt::Ps5LoadBiasEvidence();
  CHECK_EQ(static_cast<int>(bias.state),
           static_cast<int>(rt::FieldState::kUnknown));
}

// ===========================================================================
// Module metadata.
// ===========================================================================
P5EMU_TEST(RuntimeModuleMetadata, RoleAndRawMetadataAreEvidenceBounded) {
  // A generic ELF64 has no evidenced PS5 role.
  const loader::Ps5ExecutableImage g =
      ParsePs5(MakeExecutable("stub").bytes);
  const rt::ModuleMetadataInfo gm = rt::DescribeModuleMetadata(g);
  CHECK_EQ(static_cast<int>(gm.role),
           static_cast<int>(rt::ModuleRole::kUnknown));
  CHECK(gm.dependencies.empty());
  CHECK_EQ(static_cast<int>(gm.initialization_metadata),
           static_cast<int>(rt::FieldState::kUnknown));
  CHECK_EQ(static_cast<int>(gm.finalization_metadata),
           static_cast<int>(rt::FieldState::kUnknown));

  // A PS5 ET_DYNEXEC is classified as an executable; ET_DYNAMIC as a library.
  const loader::Ps5ExecutableImage ex =
      ParsePs5(MakeExecutable("stub", loader::kElfOsabiFreeBsd,
                              loader::kPs5EtDynExec).bytes);
  CHECK_EQ(static_cast<int>(rt::DescribeModuleMetadata(ex).role),
           static_cast<int>(rt::ModuleRole::kExecutable));
  const loader::Ps5ExecutableImage sh =
      ParsePs5(MakeExecutable("stub", loader::kElfOsabiFreeBsd,
                              loader::kPs5EtDynamic).bytes);
  CHECK_EQ(static_cast<int>(rt::DescribeModuleMetadata(sh).role),
           static_cast<int>(rt::ModuleRole::kSharedLibrary));
}

// ===========================================================================
// Initialization ordering model.
// ===========================================================================
P5EMU_TEST(RuntimeInitOrder, SyntheticOrderIsNotPs5StartupOrder) {
  CHECK_EQ(std::string(rt::InitOrderModelName(
               rt::InitOrderModel::kSyntheticDependenciesFirst)),
           std::string("synthetic-dependencies-first"));
  CHECK_EQ(std::string(rt::InitOrderModelName(rt::InitOrderModel::kPs5Unknown)),
           std::string("ps5-unknown"));
}

// ===========================================================================
// End-to-end: loader -> startup state -> CPU (interpreter + AArch64).
// ===========================================================================
P5EMU_TEST(RuntimeProfileE2E, GenericStartupRunsToKnownResult) {
  const test::RelocFixture fx = MakeExecutable("main");
  const loader::Ps5ExecutableImage img = ParsePs5(fx.bytes);
  CHECK(!img.is_ps5_dialect);
  CHECK_EQ(static_cast<int>(rt::ClassifyRuntimeProfile(img.is_ps5_dialect)),
           static_cast<int>(rt::RuntimeProfileKind::kGenericSynthetic));

  hle::HleRegistry reg;
  p5emu::test::RegisterCanonical(&reg);
  rt::InMemoryModuleRepository repo;
  repo.Add("main", fx.bytes);
  std::map<std::string, std::vector<std::string>> deps = {{"main", {}}};
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

  // Build the generic startup image and materialise it.
  rt::ProcessStartup ps;
  REQUIRE(rt::BuildInitialStack(&proc.memory, stack, {}, {}, {}, {}, &ps).ok());
  ps.entry_rip = proc.image.entry;  // the loader decided the entry
  const rt::StartupState ss = rt::MakeGenericStartupState(ps);
  rt::ProcessStartup mat;
  std::vector<std::string> blockers;
  REQUIRE(rt::MaterializeStartup(ss, &mat, &blockers).ok());

  // Reference interpreter.
  cpu::CpuState state;
  state.rip = mat.entry_rip;
  state.rsp = mat.initial_rsp;
  hle::HleDispatcher dispatcher(&reg);
  cpu::ReferenceInterpreter interp(kStepLimit, &dispatcher);
  const cpu::ExecutionResult res = interp.Execute(proc.program, &state, proc.memory);
  CHECK_EQ(state.rax, std::uint64_t{42});

  // AArch64 backend must agree, through multi-region transport.
  cpu::QemuArm64Runner runner(HarnessPath());
  if (runner.available()) {
    cpu::CpuState st2;
    st2.rip = mat.entry_rip;
    st2.rsp = mat.initial_rsp;
    std::unique_ptr<cpu::Backend> backend =
        cpu::MakeArm64Backend(kStepLimit, &runner, {});
    const cpu::ExecutionResult r2 =
        backend->Execute(proc.program, &st2, proc.memory);
    CHECK_EQ(st2.rax, std::uint64_t{42});
    CHECK_EQ(static_cast<int>(r2.reason), static_cast<int>(res.reason));
  }
}

// ===========================================================================
// Negative proofs.
// ===========================================================================
P5EMU_TEST(RuntimeProfileNegative, UnknownBoundariesAreProtected) {
  // 1. A PS5 profile never yields fabricated startup values.
  const rt::StartupState ps5 = rt::MakePs5StartupState(0x410000);
  rt::ProcessStartup out;
  std::vector<std::string> blockers;
  CHECK(!rt::MaterializeStartup(ps5, &out, &blockers).ok());
  CHECK(blockers.size() >= 1);

  // 2. An UNSUPPORTED required field fails more strongly than UNKNOWN.
  rt::StartupState uns = rt::MakeGenericStartupState(rt::ProcessStartup{});
  uns.thread_pointer = rt::StartupField::Unsupported();
  CHECK_EQ(static_cast<int>(rt::MaterializeStartup(uns, &out, &blockers).code()),
           static_cast<int>(StatusCode::kRuntimeProfileUnsupported));

  // 3. Ambiguous import identities remain ambiguous (never silently resolved).
  std::vector<rt::LoadedModule> mods(2);
  mods[0].id = 0;
  mods[1].id = 1;
  mods[0].exports.push_back(
      rt::ExportedSymbol{0, loader::SymbolIdentity::String("dup"), 0x1000});
  mods[1].exports.push_back(
      rt::ExportedSymbol{1, loader::SymbolIdentity::String("dup"), 0x2000});
  const rt::ProcessResolution r = rt::ResolveIdentity(
      mods, nullptr, loader::SymbolIdentity::String("dup"), rt::ResolverOptions{});
  CHECK_EQ(static_cast<int>(r.status.code()),
           static_cast<int>(StatusCode::kAmbiguousImport));

  // 4. An unknown identity is not resolved by guesswork.
  const rt::ProcessResolution u = rt::ResolveIdentity(
      mods, nullptr, loader::SymbolIdentity::Unknown(), rt::ResolverOptions{});
  CHECK(!u.status.ok());
}

// ===========================================================================
// Property campaign: the evidence boundary holds for arbitrary field states.
// ===========================================================================
P5EMU_TEST(RuntimeProfileProperty, MaterialisationRespectsKnownBoundary) {
  constexpr int kCases = 2000;
  std::uint64_t rng = 0x51ED270B2C3A4F61ull;
  for (int i = 0; i < kCases; ++i) {
    rt::StartupState s;
    const int mode = static_cast<int>(NextRandom(&rng) % 3);
    if (mode == 0) {
      SetAllRequired(&s, rt::FieldState::kKnown);
    } else if (mode == 1) {
      SetAllRequired(&s, rt::FieldState::kUnknown);
    } else {
      SetAllRequired(&s, rt::FieldState::kKnown);
      // Randomly demote individual fields.
      rt::StartupField* fields[] = {
          &s.entry_rip,       &s.initial_rsp, &s.initial_rbp,
          &s.thread_pointer,  &s.argc,        &s.argv,
          &s.envp,            &s.auxv,        &s.stack_alignment};
      for (rt::StartupField* f : fields) {
        const std::uint64_t r = NextRandom(&rng) % 3;
        if (r == 1) *f = rt::StartupField::Unknown();
        else if (r == 2) *f = rt::StartupField::Unsupported();
      }
    }

    bool all_known = true, any_unsupported = false;
    const rt::StartupField* fields[] = {
        &s.entry_rip,      &s.initial_rsp, &s.initial_rbp,
        &s.thread_pointer, &s.argc,        &s.argv,
        &s.envp,           &s.auxv,        &s.stack_alignment};
    for (const rt::StartupField* f : fields) {
      if (!f->known()) all_known = false;
      if (f->state == rt::FieldState::kUnsupported) any_unsupported = true;
    }

    rt::ProcessStartup out;
    std::vector<std::string> b1, b2;
    const Status a = rt::MaterializeStartup(s, &out, &b1);
    const Status c = rt::MaterializeStartup(s, &out, &b2);
    // Determinism.
    CHECK_EQ(static_cast<int>(a.code()), static_cast<int>(c.code()));
    CHECK_EQ(b1.size(), b2.size());
    if (all_known) {
      CHECK(a.ok());
      CHECK(b1.empty());
    } else if (any_unsupported) {
      CHECK_EQ(static_cast<int>(a.code()),
               static_cast<int>(StatusCode::kRuntimeProfileUnsupported));
      CHECK(!b1.empty());
    } else {
      CHECK_EQ(static_cast<int>(a.code()),
               static_cast<int>(StatusCode::kUnknownRuntimeField));
      CHECK(!b1.empty());
    }
  }
}
