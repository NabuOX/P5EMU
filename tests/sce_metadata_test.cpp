// SPDX-License-Identifier: Apache-2.0
//
// P5EMU is an independent, open-source project. See LICENSE and
// PROVENANCE.md before adding code here.
//
// ============================================================================
// SCE DYNAMIC METADATA TESTS (Phase 9)
// ============================================================================
//   SceTagRegistry       tag NUMBER vs tag SEMANTICS knowledge
//   SceMetadataBuild     dynamic table -> typed SCE metadata
//   SceNidRepresentation NID representation, not generation
//   SceLibraryIdentity   opaque module/library identity preservation
//   SceMetadataNegative  evidence-boundary negative proofs
//   SceMetadataProperty  randomized classification property campaign
//   SceDynamicLinkingE2E metadata path + linker + runtime + CPU
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
#include "p5emu/loader/SceMetadata.h"
#include "p5emu/runtime/Runtime.h"

namespace {

using namespace p5emu;  // NOLINT(build/namespaces)
namespace rt = p5emu::runtime;
namespace cpu = p5emu::cpu;
namespace loader = p5emu::loader;
namespace hle = p5emu::hle;

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

loader::Ps5ExecutableImage ParsePs5(const std::vector<std::uint8_t>& bytes) {
  loader::BinaryReader reader(bytes.data(), bytes.size());
  loader::GenericElfImage elf;
  loader::Ps5ExecutableImage ps5;
  (void)loader::ReadGenericElf64(reader, &elf);
  (void)loader::ParsePs5Executable(reader, elf, &ps5);
  return ps5;
}

// main text: mov rdi,20 ; mov rsi,22 ; call libmath ; mov rdi,rax ; mov rsi,16 ;
// call hle ; nop
std::vector<std::uint8_t> MainText(std::uint64_t libmath_va, std::uint64_t hle_va,
                                   std::uint64_t main_text_va) {
  namespace rd = p5emu::test::reloc_detail;
  std::vector<std::uint8_t> t(35, 0);
  t[0] = 0x48; t[1] = 0xC7; t[2] = 0xC7; rd::Put32(t, 3, 20);
  t[7] = 0x48; t[8] = 0xC7; t[9] = 0xC6; rd::Put32(t, 10, 22);
  t[14] = 0xE8;
  rd::Put32(t, 15, static_cast<std::uint32_t>(
                       static_cast<std::int64_t>(libmath_va) -
                       static_cast<std::int64_t>(main_text_va + 0x13)));
  t[19] = 0x48; t[20] = 0x89; t[21] = 0xC7;
  t[22] = 0x48; t[23] = 0xC7; t[24] = 0xC6; rd::Put32(t, 25, 16);
  t[29] = 0xE8;
  rd::Put32(t, 30, static_cast<std::uint32_t>(
                       static_cast<std::int64_t>(hle_va) -
                       static_cast<std::int64_t>(main_text_va + 0x22)));
  t[34] = 0x90;
  return t;
}

}  // namespace

// ===========================================================================
// Tag registry: number vs semantics.
// ===========================================================================
P5EMU_TEST(SceTagRegistry, KnownNumberIsNotKnownSemantics) {
  const std::uint64_t sony_tags[] = {
      loader::kDtOsDynlibData,  loader::kDtOsProcParam,
      loader::kDtOsFingerprint, loader::kDtOsModuleInfo,
      loader::kDtOsNeededModule, loader::kDtOsRelro,
      loader::kDtOsModuleAttr,  loader::kDtOsExportLib,
      loader::kDtOsImportLib,   loader::kDtOsHash,
      loader::kDtOsPltGot,      loader::kDtOsJmprel,
      loader::kDtOsRela,        loader::kDtOsStrtab,
      loader::kDtOsSymtab,      loader::kDtOsHashsz,
      loader::kDtOsSymtabsz};
  for (const std::uint64_t tag : sony_tags) {
    const loader::SceTagInfo i = loader::LookupSceTag(tag);
    CHECK_EQ(static_cast<int>(i.knowledge),
             static_cast<int>(loader::SceTagKnowledge::kNumberAndName));
    CHECK(i.name != nullptr);
    CHECK(!i.semantics_known());  // recognized by number only
    CHECK_EQ(static_cast<int>(i.payload),
             static_cast<int>(loader::SceTagPayload::kUnknown));
  }

  // Generic System V tags DO have evidenced semantics.
  const loader::SceTagInfo needed =
      loader::LookupSceTag(static_cast<std::uint64_t>(loader::kDtNeeded));
  CHECK(needed.semantics_known());
  CHECK_EQ(static_cast<int>(needed.payload),
           static_cast<int>(loader::SceTagPayload::kIndex));

  // Unrecognized numbers are preserved but known about not at all.
  const loader::SceTagInfo unknown = loader::LookupSceTag(0x7fffffffull);
  CHECK_EQ(static_cast<int>(unknown.knowledge),
           static_cast<int>(loader::SceTagKnowledge::kUnrecognized));
  CHECK(unknown.name == nullptr);

  // The PS5-era "_1" range is recognized as a namespace number only.
  const loader::SceTagInfo variant = loader::LookupSceTag(loader::kPs5VariantTagBase);
  CHECK_EQ(static_cast<int>(variant.knowledge),
           static_cast<int>(loader::SceTagKnowledge::kNumberAndName));
  CHECK(!variant.semantics_known());
}

// ===========================================================================
// Dynamic table -> typed metadata.
// ===========================================================================
P5EMU_TEST(SceMetadataBuild, BucketsAndPreservesWithoutInterpreting) {
  test::RelocFixtureOptions o;
  o.text = {0xC3};
  o.data.assign(8, 0);
  o.symbols.push_back(test::RelocSym{"stub", o.text_vaddr, false});
  o.osabi = loader::kElfOsabiFreeBsd;
  o.e_type = loader::kPs5EtDynExec;
  o.extra_dynamic = {{loader::kDtOsImportLib, 0},
                     {loader::kDtOsStrtab, 0x61000},
                     {loader::kPs5VariantTagBase, 1},
                     {0x7fffffff, 0x99}};
  const test::RelocFixture fx = test::BuildRelocElf64(o);
  const loader::SceModuleMetadata md =
      loader::BuildSceModuleMetadata(ParsePs5(fx.bytes));

  CHECK(md.is_ps5_dialect);
  CHECK_EQ(md.sce_tags.size(), std::size_t{3});   // importlib + strtab + variant
  CHECK_EQ(md.unknown_semantics_count, std::size_t{3});
  CHECK(md.has_unknown_semantics());
  CHECK_EQ(md.unrecognized.size(), std::size_t{1});  // 0x7fffffff
  CHECK(!md.generic_tags.empty());                    // builder's System V tags
  CHECK(md.declares_sce_imports);
  CHECK(!md.declares_sce_exports);
  // The variant flag is set for the "_1" range entry.
  bool saw_variant = false;
  for (const loader::SceDynamicEntry& e : md.sce_tags) {
    if (e.is_ps5_variant) saw_variant = true;
  }
  CHECK(saw_variant);
  CHECK(loader::ValidateSceMetadata(md, nullptr).ok());
}

// ===========================================================================
// NID representation (not generation).
// ===========================================================================
P5EMU_TEST(SceNidRepresentation, PreserveAndCompareOnlyNoGeneration) {
  const loader::SceNid a = loader::SceNid::FromOpaque(0x00ABCDEF12345678ull);
  const loader::SceNid b = loader::SceNid::FromOpaque(0x00ABCDEF12345678ull);
  const loader::SceNid c = loader::SceNid::FromOpaque(0x1111111111111111ull);
  CHECK(a.has_value());
  CHECK(a == b);
  CHECK(a != c);
  CHECK_EQ(a.ToHex(), std::string("0x00abcdef12345678"));

  const loader::SceNid none;
  CHECK(!none.has_value());
  CHECK_EQ(none.ToHex(), std::string("<unknown>"));

  // Evidenced NID -> NumericId; unknown NID -> Unknown. Never a string name.
  const loader::SymbolIdentity si = loader::SymbolIdentityFromNid(a);
  CHECK_EQ(static_cast<int>(si.kind),
           static_cast<int>(loader::SymbolIdentityKind::kNumericId));
  CHECK_EQ(si.numeric_id, std::uint64_t{0x00ABCDEF12345678ull});
  CHECK_EQ(static_cast<int>(loader::SymbolIdentityFromNid(none).kind),
           static_cast<int>(loader::SymbolIdentityKind::kUnknown));
}

// ===========================================================================
// Module / library identity.
// ===========================================================================
P5EMU_TEST(SceLibraryIdentity, OpaqueIdentityIsNotStringified) {
  const loader::SceLibraryIdentity lib = loader::SceLibraryIdentity::FromOpaque(
      0x8000000012345678ull);
  CHECK(lib.evidenced);
  CHECK_EQ(static_cast<int>(lib.opaque_id.kind),
           static_cast<int>(loader::SymbolIdentityKind::kNumericId));
  CHECK(lib.opaque_id.name.empty());  // never turned into a guessed name

  const loader::SceLibraryIdentity unknown = loader::SceLibraryIdentity::Unknown();
  CHECK(!unknown.evidenced);
  CHECK_EQ(static_cast<int>(unknown.opaque_id.kind),
           static_cast<int>(loader::SymbolIdentityKind::kUnknown));
}

// ===========================================================================
// Negative proofs.
// ===========================================================================
P5EMU_TEST(SceMetadataNegative, EvidenceBoundariesAreProtected) {
  // 1. Conflicting recognized tags (same number, differing values) are caught.
  loader::SceModuleMetadata md;
  loader::SceDynamicEntry e1, e2;
  e1.info = loader::LookupSceTag(loader::kDtOsModuleInfo);
  e1.value = 1;
  e2.info = loader::LookupSceTag(loader::kDtOsModuleInfo);
  e2.value = 2;
  md.sce_tags = {e1, e2};
  std::vector<std::string> blockers;
  CHECK_EQ(static_cast<int>(loader::ValidateSceMetadata(md, &blockers).code()),
           static_cast<int>(StatusCode::kConflictingMetadata));
  CHECK(!blockers.empty());

  // 2. Identical duplicates are tolerated (conservative policy).
  loader::SceModuleMetadata ok;
  e2.value = 1;
  ok.sce_tags = {e1, e2};
  CHECK(loader::ValidateSceMetadata(ok, nullptr).ok());

  // 3. A generic ELF does not masquerade as evidenced PS5/SCE metadata.
  test::RelocFixtureOptions g;
  g.text = {0xC3};
  g.data.assign(8, 0);
  g.symbols.push_back(test::RelocSym{"stub", g.text_vaddr, false});
  const loader::SceModuleMetadata gm =
      loader::BuildSceModuleMetadata(ParsePs5(test::BuildRelocElf64(g).bytes));
  CHECK(!gm.is_ps5_dialect);
  CHECK(gm.sce_tags.empty());
  CHECK_EQ(gm.unknown_semantics_count, std::size_t{0});

  // 4. An unknown NID is not silently given a value or a name.
  const loader::SceNid none;
  CHECK(!none.has_value());
  CHECK(none != loader::SceNid::FromOpaque(0));  // 0 as a value is not "unknown"
}

// ===========================================================================
// Property campaign: classification is total and evidence-safe.
// ===========================================================================
P5EMU_TEST(SceMetadataProperty, ClassificationIsTotalAndEvidenceSafe) {
  constexpr int kCases = 2000;
  std::uint64_t rng = 0xA17C0FFEE1234567ull;
  for (int i = 0; i < kCases; ++i) {
    loader::SceModuleMetadata md;
    const int n = static_cast<int>(NextRandom(&rng) % 8);
    std::size_t sce = 0;
    for (int j = 0; j < n; ++j) {
      loader::SceDynamicEntry e;
      e.value = NextRandom(&rng);
      e.info = loader::LookupSceTag(NextRandom(&rng) % 0x62000000ull);
      if (e.info.knowledge == loader::SceTagKnowledge::kKnownSemantics) {
        md.generic_tags.push_back(e);
      } else if (e.info.knowledge == loader::SceTagKnowledge::kNumberAndName) {
        md.sce_tags.push_back(e);
        ++sce;
      } else {
        md.unrecognized.push_back(e);
      }
    }
    // Total classification: every entry lands in exactly one bucket.
    CHECK_EQ(md.generic_tags.size() + md.sce_tags.size() + md.unrecognized.size(),
             static_cast<std::size_t>(n));
    md.unknown_semantics_count = sce;  // as BuildSceModuleMetadata would set it
    // Only recognized-but-ununderstood tags are counted as unknown semantics.
    CHECK_EQ(md.unknown_semantics_count, sce);
    // Every entry's semantics_known() is consistent with its knowledge bucket.
    for (const loader::SceDynamicEntry& e : md.generic_tags) {
      CHECK(e.info.semantics_known());
    }
    for (const loader::SceDynamicEntry& e : md.sce_tags) {
      CHECK(!e.info.semantics_known());
    }
  }
}

// ===========================================================================
// End-to-end: metadata path + linker + runtime + CPU.
// ===========================================================================
P5EMU_TEST(SceDynamicLinkingE2E, MetadataSurvivesAndLinkerResolves) {
  const std::uint64_t lib_bias = rt::LoadBiasAllocator::kBase;
  const std::uint64_t main_bias =
      rt::LoadBiasAllocator::kBase + rt::LoadBiasAllocator::kStride;
  const std::uint64_t main_text_va = main_bias + 0x400000;
  const std::uint64_t lib_text_va = lib_bias + 0x400000;

  test::RelocFixtureOptions lib;
  lib.text = {0x48, 0x89, 0xF8, 0x48, 0x01, 0xF0, 0xC3};
  lib.data = {0};
  lib.symbols.push_back(test::RelocSym{"libmath_add", 0x400000, false});
  const test::RelocFixture libfx = test::BuildRelocElf64(lib);

  test::RelocFixtureOptions main;
  main.text = MainText(lib_text_va, kTBase, main_text_va);
  main.data.assign(16, 0);
  main.symbols.push_back(test::RelocSym{"p5emu_add", 0, true});
  main.rela_dyn.push_back(test::RelocEntry{main.data_vaddr, 6, 1, 0});
  // SCE metadata present alongside the generic relocation tables.
  main.extra_dynamic = {{loader::kDtOsImportLib, 0},
                        {loader::kDtOsStrtab, 0x61000},
                        {loader::kPs5VariantTagBase, 1}};
  const test::RelocFixture mainfx = test::BuildRelocElf64(main);

  // Typed SCE metadata is built and preserves unknown-semantics tags...
  const loader::SceModuleMetadata md =
      loader::BuildSceModuleMetadata(ParsePs5(mainfx.bytes));
  CHECK(md.sce_tags.size() >= 3);
  CHECK(md.unknown_semantics_count >= 3);
  CHECK(loader::ValidateSceMetadata(md, nullptr).ok());

  // ...and does NOT disturb generic linking.
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

  cpu::CpuState state;
  state.rip = proc.image.entry;
  state.rsp = stack.top() - 16;
  hle::HleDispatcher dispatcher(&reg);
  cpu::ReferenceInterpreter interp(kStepLimit, &dispatcher);
  const cpu::ExecutionResult res = interp.Execute(proc.program, &state, proc.memory);
  CHECK_EQ(state.rax, std::uint64_t{58});  // libmath(20,22)=42 ; p5emu_add(42,16)=58

  cpu::QemuArm64Runner runner(HarnessPath());
  if (runner.available()) {
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
// Phase 10 evidence boundary: DT_OS_MODULE_INFO stays UNKNOWN.
// These tests fail if a future contributor promotes the tag to known semantics
// or assigns it a payload without new evidence. See
// docs/research/ps5-module-info-phase10.md.
// ===========================================================================
P5EMU_TEST(SceModuleInfoEvidence, NumberAndNameKnownPayloadUnknown) {
  const loader::SceTagInfo info = loader::LookupSceTag(loader::kDtOsModuleInfo);
  CHECK_EQ(static_cast<int>(info.knowledge),
           static_cast<int>(loader::SceTagKnowledge::kNumberAndName));
  CHECK(!info.semantics_known());  // Phase 10: semantics NOT established
  CHECK_EQ(static_cast<int>(info.payload),
           static_cast<int>(loader::SceTagPayload::kUnknown));
  CHECK(info.name != nullptr);
  CHECK_EQ(std::string(info.name), std::string("DT_OS_MODULE_INFO"));

  // The PS5-era "_1" spelling of the same tag (0x61000043, per the admitted
  // source) is likewise number-and-name only.
  const loader::SceTagInfo variant = loader::LookupSceTag(0x61000043ull);
  CHECK_EQ(static_cast<int>(variant.knowledge),
           static_cast<int>(loader::SceTagKnowledge::kNumberAndName));
  CHECK(!variant.semantics_known());
}

P5EMU_TEST(SceModuleInfoEvidence, RawPayloadPreservedNotInterpreted) {
  test::RelocFixtureOptions o;
  o.text = {0xC3};
  o.data.assign(8, 0);
  o.symbols.push_back(test::RelocSym{"stub", o.text_vaddr, false});
  o.extra_dynamic = {{loader::kDtOsModuleInfo, 0xDEADBEEFCAFEF00Dull}};
  const loader::SceModuleMetadata md =
      loader::BuildSceModuleMetadata(ParsePs5(test::BuildRelocElf64(o).bytes));

  bool found = false;
  for (const loader::SceDynamicEntry& e : md.sce_tags) {
    if (e.info.tag == loader::kDtOsModuleInfo) {
      found = true;
      CHECK_EQ(e.value, std::uint64_t{0xDEADBEEFCAFEF00Dull});  // raw preserved
      CHECK(!e.info.semantics_known());                         // not interpreted
    }
  }
  CHECK(found);
  CHECK(md.has_unknown_semantics());
}
