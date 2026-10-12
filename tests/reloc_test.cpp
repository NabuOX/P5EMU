// SPDX-License-Identifier: Apache-2.0
//
// P5EMU is an independent, open-source project. See LICENSE and
// PROVENANCE.md before adding code here.
//
// ============================================================================
// RELOCATION AND IMPORT RESOLUTION TESTS (Phase 5)
// ============================================================================
//   RelocParse    RELA parsing, REL rejection, malformed tables
//   RelocApply    formulas and the load-bias model at multiple bases
//   RelocProperty deterministic property cases vs an independent formula oracle
//   ImportModel   resolver statuses: resolved/unresolved/unsupported/ambiguous
//   RelocE2E      ELF -> reloc -> import -> resolver -> memory -> CPU -> result
//   RelocNegative injected wrong-load-bias and unresolved-as-zero defects caught
//
// Fixtures are synthetic (RelocFixtureBuilder.h). No real binary is used.
// ============================================================================

#include <cstdint>
#include <string>
#include <vector>

#include "TestHarness.h"

#include "RelocFixtureBuilder.h"

#include "p5emu/core/Status.h"
#include "p5emu/cpu/Arm64Backend.h"
#include "p5emu/cpu/Arm64Runner.h"
#include "p5emu/cpu/LinearGuestMemory.h"
#include "p5emu/cpu/MappedGuestMemory.h"
#include "p5emu/cpu/ReferenceInterpreter.h"
#include "p5emu/cpu/TranslateToIr.h"
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
using p5emu::test::BuildRelocElf64;
using p5emu::test::RelocEntry;
using p5emu::test::RelocFixtureOptions;
using p5emu::test::RelocSym;

constexpr std::uint64_t kStepLimit = 1u << 20;
constexpr std::uint64_t kR64 = 1, kRRelative = 8;

std::string HarnessPath() {
  const char* env = std::getenv("P5EMU_A64_HARNESS");
  if (env != nullptr && *env != '\0') return env;
#ifdef P5EMU_A64_HARNESS_DEFAULT
  return P5EMU_A64_HARNESS_DEFAULT;
#else
  return "";
#endif
}

struct LR {
  Status status = StatusCode::kInvalidArgument;
  loader::GenericElfImage elf;
  loader::Ps5ExecutableImage ps5;
  loader::LoadPlan plan;
  loader::ImportBinding binding;
  memory::MappedGuestMemory memory;
};

LR LoadReloc(const std::vector<std::uint8_t>& bytes, std::uint64_t bias,
             loader::ImportResolver& resolver,
             const loader::ApplyOptions& opts = {}) {
  LR out;
  const loader::BinaryReader r(bytes.data(), bytes.size());
  out.status = loader::ReadGenericElf64(r, &out.elf);
  if (!out.status.ok()) return out;
  out.status = loader::ValidateGenericLoadSegments(r, out.elf);
  if (!out.status.ok()) return out;
  out.status = loader::ParsePs5Executable(r, out.elf, &out.ps5);
  if (!out.status.ok()) return out;
  out.status = loader::BuildLoadPlan(r, out.ps5, bias, &out.plan);
  if (!out.status.ok()) return out;
  out.status = loader::ValidateLoadPlan(out.plan);
  if (!out.status.ok()) return out;
  out.status = loader::ResolveImports(out.plan, resolver, &out.binding);
  if (!out.status.ok()) return out;
  out.status = loader::ApplyLoadPlan(r, out.plan, out.binding, opts, &out.memory);
  return out;
}

// A program that reads a relocated data slot and uses a relocated code
// immediate. See the test comments for the expected values.
RelocFixtureOptions E2EOptions(std::uint64_t a1, std::uint64_t a2) {
  RelocFixtureOptions o;
  o.text.assign(23, 0);
  o.text[0] = 0x48; o.text[1] = 0xB8;              // mov rax, imm64 (imm@2)
  o.text[10] = 0x48; o.text[11] = 0xB9;            // mov rcx, imm64 (imm@12)
  o.text[20] = 0x48; o.text[21] = 0x8B; o.text[22] = 0x09;  // mov rcx,[rcx]
  p5emu::test::reloc_detail::Put64(o.text, 12, o.data_vaddr);
  o.data.assign(8, 0);  // the RELATIVE target slot at data_vaddr+0
  o.symbols.push_back(RelocSym{"p5emu_value", 0, true});  // dynsym index 1
  o.rela_dyn.push_back(RelocEntry{o.text_vaddr + 2, static_cast<std::uint32_t>(kR64),
                                  1, static_cast<std::int64_t>(a2)});
  o.rela_dyn.push_back(RelocEntry{o.data_vaddr + 0,
                                  static_cast<std::uint32_t>(kRRelative), 0,
                                  static_cast<std::int64_t>(a1)});
  return o;
}

}  // namespace

// ===========================================================================
// RELA parsing.
// ===========================================================================
P5EMU_TEST(RelocParse, RelaTablesParseAndExposeTypedRelocations) {
  const auto fx = BuildRelocElf64(E2EOptions(0x11, 0x22));
  const loader::BinaryReader r(fx.bytes.data(), fx.bytes.size());
  loader::GenericElfImage elf;
  REQUIRE(loader::ReadGenericElf64(r, &elf).ok());
  loader::Ps5ExecutableImage ps5;
  REQUIRE(loader::ParsePs5Executable(r, elf, &ps5).ok());

  loader::RelocationSet rs;
  REQUIRE(loader::ParseDynamicRelocations(r, ps5, 0, &rs).ok());
  REQUIRE(rs.relocations.size() == 2);
  REQUIRE(rs.symbols.size() == 2);  // null + p5emu_value
  CHECK_EQ(rs.symbols[1].name, std::string("p5emu_value"));
  CHECK(rs.symbols[1].IsImport());
  // The first relocation is on the code immediate; the second is relative.
  bool saw_k64 = false, saw_relative = false;
  for (const auto& rel : rs.relocations) {
    if (rel.type == loader::RelocType::k64) {
      saw_k64 = true;
      CHECK_EQ(rel.symbol_index, std::uint32_t{1});
      CHECK_EQ(rel.addend, std::uint64_t{0x22});
    }
    if (rel.type == loader::RelocType::kRelative) saw_relative = true;
  }
  CHECK(saw_k64);
  CHECK(saw_relative);
}

P5EMU_TEST(RelocParse, RelTableIsRejectedNotMisread) {
  // DT_PLTREL naming REL (17) must be rejected: REL uses implicit addends and
  // must never be interpreted as RELA.
  RelocFixtureOptions o = E2EOptions(0, 0);
  auto fx = BuildRelocElf64(o);
  // The dynamic table is the last 11*16 bytes of the data segment; the DT_PLTREL
  // entry is the 10th (value 7). Overwrite its value with 17.
  // Find it by scanning for the DT_PLTREL tag (20) in the data region.
  // Simpler: rebuild is hard to patch precisely, so patch via search.
  bool patched = false;
  for (std::size_t i = 0; i + 16 <= fx.bytes.size(); ++i) {
    const std::uint8_t* p = fx.bytes.data() + i;
    if (p[0] == 20 && p[1] == 0 && p[2] == 0 && p[3] == 0) {
      fx.bytes[i + 8] = 17;  // DT_REL
      patched = true;
      break;
    }
  }
  REQUIRE(patched);
  const loader::BinaryReader r(fx.bytes.data(), fx.bytes.size());
  loader::GenericElfImage elf;
  REQUIRE(loader::ReadGenericElf64(r, &elf).ok());
  loader::Ps5ExecutableImage ps5;
  REQUIRE(loader::ParsePs5Executable(r, elf, &ps5).ok());
  loader::RelocationSet rs;
  CHECK_EQ(loader::ParseDynamicRelocations(r, ps5, 0, &rs).code(),
           StatusCode::kUnsupportedRelocation);
}

// ===========================================================================
// Apply + load bias.
// ===========================================================================
P5EMU_TEST(RelocApply, LoadBiasChangesRelativeResultsAtMultipleBases) {
  // A fixture with only a RELATIVE relocation, so no import resolution is
  // needed and the load-bias model is what is exercised.
  RelocFixtureOptions o = E2EOptions(0, 0);
  o.symbols.clear();
  o.rela_dyn.clear();
  o.rela_dyn.push_back(RelocEntry{o.data_vaddr + 0,
                                  static_cast<std::uint32_t>(kRRelative), 0,
                                  0x100});
  const auto fx = BuildRelocElf64(o);
  loader::RegistryResolver resolver;

  const std::uint64_t bases[] = {0x0, 0x1000'0000ull, 0x2000'0000ull};
  for (std::uint64_t bias : bases) {
    LR lr = LoadReloc(fx.bytes, bias, resolver);
    REQUIRE(lr.status.ok());
    std::uint64_t v = 0;
    REQUIRE(lr.memory.ReadData(fx.data_vaddr + bias, 8, &v).ok());
    CHECK_EQ(v, bias + 0x100);  // B + A at every base
  }
}

P5EMU_TEST(RelocApply, RelativeFormulaUsesLoadBias) {
  // A fixture with only a RELATIVE relocation at data_vaddr.
  RelocFixtureOptions o = E2EOptions(0, 0);
  o.symbols.clear();
  o.rela_dyn.clear();
  o.rela_dyn.push_back(RelocEntry{o.data_vaddr + 0,
                                  static_cast<std::uint32_t>(kRRelative), 0,
                                  0x1234});
  const auto fx = BuildRelocElf64(o);
  loader::RegistryResolver resolver;

  const std::uint64_t bias = 0x2000'0000ull;
  LR lr = LoadReloc(fx.bytes, bias, resolver);
  REQUIRE(lr.status.ok());
  std::uint64_t v = 0;
  REQUIRE(lr.memory.ReadData(fx.data_vaddr + bias, 8, &v).ok());
  CHECK_EQ(v, bias + 0x1234);  // B + A
}

// ===========================================================================
// Property testing against an independent oracle.
// ===========================================================================
P5EMU_TEST(RelocProperty, RelativeFormulaMatchesOracle) {
  loader::RegistryResolver resolver;
  int cases = 0;
  int failures = 0;
  for (std::uint64_t ai = 0; ai < 40; ++ai) {
    const std::uint64_t addend = (ai * 0x9E37'79B9ull) & 0xFFFFull;
    RelocFixtureOptions o = E2EOptions(0, 0);
    o.symbols.clear();
    o.rela_dyn.clear();
    o.rela_dyn.push_back(RelocEntry{o.data_vaddr + 0,
                                    static_cast<std::uint32_t>(kRRelative), 0,
                                    static_cast<std::int64_t>(addend)});
    const auto fx = BuildRelocElf64(o);
    for (std::uint64_t bi = 0; bi < 30; ++bi) {
      const std::uint64_t bias = bi * 0x1000'0000ull + 0x1000;
      LR lr = LoadReloc(fx.bytes, bias, resolver);
      ++cases;
      if (!lr.status.ok()) { ++failures; continue; }
      std::uint64_t v = 0;
      if (!lr.memory.ReadData(fx.data_vaddr + bias, 8, &v).ok()) {
        ++failures;
        continue;
      }
      // Independent oracle: B + A.
      const std::uint64_t want = bias + addend;
      if (v != want) ++failures;
    }
  }
  std::printf("reloc property (RELATIVE): %d cases, %d failures\n", cases,
              failures);
  CHECK(cases >= 1000);
  CHECK_EQ(failures, 0);
}

// ===========================================================================
// Resolver statuses.
// ===========================================================================
P5EMU_TEST(ImportModel, ResolverDistinguishesOutcomes) {
  loader::RegistryResolver r;
  r.AddStringSymbol("lib", "known", 0x1234);
  r.AddStringSymbol("lib", "dup", 0x1000);
  r.AddStringSymbol("lib", "dup", 0x2000);  // ambiguous

  CHECK_EQ(r.ResolveSymbol("lib", loader::SymbolIdentity::String("known")).status,
           loader::ResolutionStatus::kResolved);
  CHECK_EQ(r.ResolveSymbol("lib", loader::SymbolIdentity::String("known")).address,
           std::uint64_t{0x1234});
  CHECK_EQ(r.ResolveSymbol("lib", loader::SymbolIdentity::String("missing")).status,
           loader::ResolutionStatus::kUnresolved);
  CHECK_EQ(r.ResolveSymbol("lib", loader::SymbolIdentity::String("dup")).status,
           loader::ResolutionStatus::kAmbiguous);
  // A numeric identity is representable but not something a string registry
  // can decide -- a distinct outcome, not a generic failure.
  CHECK_EQ(r.ResolveSymbol("lib", loader::SymbolIdentity::Numeric(7)).status,
           loader::ResolutionStatus::kUnsupportedIdentity);
  CHECK_EQ(r.ResolveSymbol("lib", loader::SymbolIdentity::Unknown()).status,
           loader::ResolutionStatus::kUnsupportedIdentity);
}

P5EMU_TEST(ImportModel, UnresolvedRequiredImportBlocksReadiness) {
  RelocFixtureOptions o = E2EOptions(0x11, 0x22);  // has an import binding site
  const auto fx = BuildRelocElf64(o);
  loader::RegistryResolver empty;  // does not know p5emu_value

  const loader::BinaryReader r(fx.bytes.data(), fx.bytes.size());
  loader::GenericElfImage elf;
  REQUIRE(loader::ReadGenericElf64(r, &elf).ok());
  loader::Ps5ExecutableImage ps5;
  REQUIRE(loader::ParsePs5Executable(r, elf, &ps5).ok());
  loader::LoadPlan plan;
  REQUIRE(loader::BuildLoadPlan(r, ps5, 0, &plan).ok());
  REQUIRE(loader::ValidateLoadPlan(plan).ok());

  loader::ImportBinding binding;
  REQUIRE(loader::ResolveImports(plan, empty, &binding).ok());
  CHECK(!binding.execution_ready);
  CHECK_EQ(binding.status.code(), StatusCode::kUnresolvedImport);

  // And applying it is refused rather than writing address zero.
  memory::MappedGuestMemory mem;
  CHECK_EQ(loader::ApplyLoadPlan(r, plan, binding, {}, &mem).code(),
           StatusCode::kUnresolvedImport);
}

// ===========================================================================
// End-to-end.
// ===========================================================================
P5EMU_TEST(RelocE2E, RelocatedAndBoundProgramExecutesToKnownResult) {
  const std::uint64_t S = 0x500000;   // provider address for p5emu_value
  const std::uint64_t A1 = 0x11;      // RELATIVE addend
  const std::uint64_t A2 = 0x22;      // k64 addend
  const auto fx = BuildRelocElf64(E2EOptions(A1, A2));

  loader::RegistryResolver resolver;
  resolver.AddStringSymbol("", "p5emu_value", S);  // generic module identity

  LR lr = LoadReloc(fx.bytes, 0, resolver);
  REQUIRE(lr.status.ok());

  // Memory was relocated: [data_vaddr] = B + A1 = A1 (bias 0).
  std::uint64_t slot = 0;
  REQUIRE(lr.memory.ReadData(fx.data_vaddr, 8, &slot).ok());
  CHECK_EQ(slot, A1);

  // CPU entry state.
  cpu::CpuState state;
  REQUIRE(loader::MakeEntryState(lr.plan, &state).ok());
  CHECK_EQ(state.rip, fx.entry);

  // Fetch from guest memory, decode, translate, interpret.
  std::vector<std::uint8_t> fetched(23, 0);
  std::size_t avail = 0;
  REQUIRE(lr.memory.FetchCode(fx.entry, 23, fetched.data(), &avail).ok());
  REQUIRE(avail == 23);
  ir::Program ir;
  std::vector<memory::GuestAddr> addrs;
  REQUIRE(cpu::TranslateProgram(fx.entry, fetched.data(), fetched.size(), "e2e",
                                &ir, &addrs)
              .ok());
  REQUIRE(ir::ValidateIr(ir).ok());

  memory::MappedGuestMemory mem2;
  {
    const loader::BinaryReader r(fx.bytes.data(), fx.bytes.size());
    REQUIRE(loader::ApplyLoadPlan(r, lr.plan, lr.binding, {}, &mem2).ok());
  }
  cpu::CpuState rs;
  REQUIRE(loader::MakeEntryState(lr.plan, &rs).ok());
  cpu::ReferenceInterpreter interp(kStepLimit);
  const cpu::ExecutionResult res = interp.Execute(ir, &rs, mem2);
  CHECK_EQ(res.reason, cpu::TerminationReason::kFallthrough);
  // rax = patched code immediate = S + A2 ; rcx = relocated data slot = A1.
  CHECK_EQ(rs.rax, S + A2);
  CHECK_EQ(rs.rcx, A1);
}

P5EMU_TEST(RelocE2E, BoundProgramAlsoAgreesOnAArch64) {
  const std::uint64_t S = 0x500000, A1 = 0x11, A2 = 0x22;
  const auto fx = BuildRelocElf64(E2EOptions(A1, A2));
  loader::RegistryResolver resolver;
  resolver.AddStringSymbol("", "p5emu_value", S);
  LR lr = LoadReloc(fx.bytes, 0, resolver);
  REQUIRE(lr.status.ok());

  cpu::QemuArm64Runner runner(HarnessPath());
  if (!runner.available()) {
    p5emu::test::ReportFailure(__FILE__, __LINE__,
                               "AArch64 harness not available");
    return;
  }
  std::vector<std::uint8_t> fetched(23, 0);
  std::size_t avail = 0;
  REQUIRE(lr.memory.FetchCode(fx.entry, 23, fetched.data(), &avail).ok());
  ir::Program ir;
  std::vector<memory::GuestAddr> addrs;
  REQUIRE(cpu::TranslateProgram(fx.entry, fetched.data(), fetched.size(),
                                "e2e-a64", &ir, &addrs)
              .ok());
  memory::MappedGuestMemory mem;
  {
    const loader::BinaryReader r(fx.bytes.data(), fx.bytes.size());
    REQUIRE(loader::ApplyLoadPlan(r, lr.plan, lr.binding, {}, &mem).ok());
  }
  // The AArch64 backend executes against a single contiguous region; present the
  // same loaded bytes (and the same final addresses) as one flat port. The
  // relocation state is unchanged, so the computation is identical.
  const std::vector<memory::Region> regions = mem.Regions();
  std::uint64_t lo = ~std::uint64_t{0}, hi = 0;
  for (const auto& rg : regions) {
    lo = (rg.guest_base < lo) ? rg.guest_base : lo;
    hi = (rg.guest_base + rg.size > hi) ? rg.guest_base + rg.size : hi;
  }
  memory::LinearGuestMemory flat =
      memory::LinearGuestMemory::MakeCodeAndData(lo, hi - lo);
  for (const auto& rg : regions) {
    std::vector<std::uint8_t> bytes;
    REQUIRE(mem.ReadRange(rg.guest_base, rg.size, &bytes).ok());
    REQUIRE(flat.DebugFill(rg.guest_base, bytes.data(), bytes.size()).ok());
  }

  cpu::CpuState state;
  REQUIRE(loader::MakeEntryState(lr.plan, &state).ok());
  auto backend = cpu::MakeArm64Backend(kStepLimit, &runner);
  const cpu::ExecutionResult res = backend->Execute(ir, &state, flat);
  CHECK_EQ(res.reason, cpu::TerminationReason::kFallthrough);
  CHECK_EQ(state.rax, S + A2);
  CHECK_EQ(state.rcx, A1);
}

// ===========================================================================
// Negative proofs.
// ===========================================================================
P5EMU_TEST(RelocNegative, WrongLoadBiasIsCaught) {
  RelocFixtureOptions o = E2EOptions(0, 0);
  o.symbols.clear();
  o.rela_dyn.clear();
  o.rela_dyn.push_back(RelocEntry{o.data_vaddr + 0,
                                  static_cast<std::uint32_t>(kRRelative), 0,
                                  0x1234});
  const auto fx = BuildRelocElf64(o);
  loader::RegistryResolver resolver;
  const std::uint64_t bias = 0x2000'0000ull;

  // Correct: B + A.
  LR good = LoadReloc(fx.bytes, bias, resolver);
  REQUIRE(good.status.ok());
  std::uint64_t v = 0;
  REQUIRE(good.memory.ReadData(fx.data_vaddr + bias, 8, &v).ok());
  CHECK_EQ(v, bias + 0x1234);

  // Defect: forgets the load bias (uses 0 + A). Different result -> caught.
  loader::ApplyOptions bad;
  bad.inject_wrong_load_bias = true;
  LR defective = LoadReloc(fx.bytes, bias, resolver, bad);
  REQUIRE(defective.status.ok());
  std::uint64_t v2 = 0;
  REQUIRE(defective.memory.ReadData(fx.data_vaddr + bias, 8, &v2).ok());
  CHECK_EQ(v2, std::uint64_t{0x1234});
  CHECK(v2 != v);
}

P5EMU_TEST(RelocNegative, UnresolvedAsZeroIsCaught) {
  const std::uint64_t A2 = 0x22;
  const auto fx = BuildRelocElf64(E2EOptions(0x11, A2));
  loader::RegistryResolver empty;  // p5emu_value is NOT provided

  // Correct behaviour: the unresolved required import refuses to apply.
  LR refused = LoadReloc(fx.bytes, 0, empty);
  CHECK_EQ(refused.status.code(), StatusCode::kUnresolvedImport);

  // Defect: treat the unresolved symbol as address zero and continue. The code
  // immediate becomes 0 + A2 instead of S + A2 -- a silently wrong image.
  loader::ApplyOptions bad;
  bad.inject_unresolved_as_zero = true;
  LR defective = LoadReloc(fx.bytes, 0, empty, bad);
  REQUIRE(defective.status.ok());
  std::vector<std::uint8_t> fetched(23, 0);
  std::size_t avail = 0;
  REQUIRE(defective.memory.FetchCode(fx.entry, 23, fetched.data(), &avail).ok());
  ir::Program ir;
  std::vector<memory::GuestAddr> addrs;
  REQUIRE(cpu::TranslateProgram(fx.entry, fetched.data(), fetched.size(), "neg",
                                &ir, &addrs)
              .ok());
  memory::MappedGuestMemory mem;
  {
    const loader::BinaryReader r(fx.bytes.data(), fx.bytes.size());
    REQUIRE(loader::ApplyLoadPlan(r, defective.plan, defective.binding, bad, &mem)
                .ok());
  }
  cpu::CpuState rs;
  cpu::ReferenceInterpreter interp(kStepLimit);
  interp.Execute(ir, &rs, mem);
  // The defect makes rax = 0 + A2, not S + A2. The test detects the difference.
  CHECK_EQ(rs.rax, A2);
  CHECK(rs.rax != (0x500000ull + A2));
}
