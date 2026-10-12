// SPDX-License-Identifier: Apache-2.0
//
// P5EMU is an independent, open-source project. See LICENSE and
// PROVENANCE.md before adding code here.
//
// ============================================================================
// LOADER TESTS (Phase 4)
// ============================================================================
//
//   LoaderElf    generic ELF64 parse and malformed rejection
//   LoaderPs5    PS5/Prospero interpretation; unknown tags not interpreted
//   LoaderPlan   plan validation: overlap, entry, BSS, permissions
//   LoaderApply  transactional apply; failure leaves memory untouched
//   LoaderE2E    synthetic fixture -> loader -> guest memory -> CPU -> result
//
// No commercial/game/firmware/SDK binary is used; every fixture is built in
// memory by ElfFixtureBuilder.h.
// ============================================================================

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "TestHarness.h"

#include "ElfFixtureBuilder.h"

#include "p5emu/core/Status.h"
#include "p5emu/cpu/Arm64Backend.h"
#include "p5emu/cpu/Arm64Runner.h"
#include "p5emu/cpu/MappedGuestMemory.h"
#include "p5emu/cpu/ReferenceInterpreter.h"
#include "p5emu/cpu/TranslateToIr.h"
#include "p5emu/loader/BinaryReader.h"
#include "p5emu/loader/Elf64.h"
#include "p5emu/loader/Loader.h"
#include "p5emu/loader/Ps5Executable.h"

namespace {

using namespace p5emu;          // NOLINT(build/namespaces)
namespace loader = p5emu::loader;
namespace memory = p5emu::memory;
using p5emu::test::BuildElf64Fixture;
using p5emu::test::Elf64FixtureOptions;

constexpr std::uint64_t kStepLimit = 1u << 20;

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

struct Loaded {
  Status status = StatusCode::kInvalidArgument;
  loader::GenericElfImage elf;
  loader::Ps5ExecutableImage ps5;
  loader::LoadPlan plan;
  memory::MappedGuestMemory memory;
};

// Full load: parse -> plan -> validate -> apply.
Loaded LoadFixture(const std::vector<std::uint8_t>& bytes) {
  Loaded out;
  const loader::BinaryReader reader(bytes.data(), bytes.size());
  out.status = loader::ReadGenericElf64(reader, &out.elf);
  if (!out.status.ok()) return out;
  out.status = loader::ValidateGenericLoadSegments(reader, out.elf);
  if (!out.status.ok()) return out;
  out.status = loader::ParsePs5Executable(reader, out.elf, &out.ps5);
  if (!out.status.ok()) return out;
  out.status = loader::BuildLoadPlan(reader, out.ps5, &out.plan);
  if (!out.status.ok()) return out;
  out.status = loader::ValidateLoadPlan(out.plan);
  if (!out.status.ok()) return out;
  out.status = loader::ApplyLoadPlan(reader, out.plan, {}, &out.memory);
  return out;
}

}  // namespace

// ===========================================================================
// Generic ELF.
// ===========================================================================
P5EMU_TEST(LoaderElf, ValidFixtureParses) {
  Elf64FixtureOptions o;
  o.text = {0x90};  // nop
  const auto fx = BuildElf64Fixture(o);

  const loader::BinaryReader reader(fx.bytes.data(), fx.bytes.size());
  loader::GenericElfImage elf;
  REQUIRE(loader::ReadGenericElf64(reader, &elf).ok());
  CHECK_EQ(elf.elf_class, std::uint8_t{2});
  CHECK_EQ(elf.data_encoding, std::uint8_t{1});
  CHECK_EQ(elf.machine, std::uint16_t{62});
  CHECK_EQ(elf.big_endian, false);
  CHECK_EQ(elf.entry, fx.entry);
  CHECK_EQ(elf.program_headers.size(), std::size_t{1});
  CHECK_EQ(elf.program_headers[0].type, loader::kPtLoad);
  CHECK(loader::ValidateGenericLoadSegments(reader, elf).ok());
}

P5EMU_TEST(LoaderElf, MalformedHeadersAreRejectedByCategory) {
  Elf64FixtureOptions o;
  o.text = {0x90};
  const auto good = BuildElf64Fixture(o);

  auto expect = [&](const std::vector<std::uint8_t>& bytes, StatusCode want) {
    const loader::BinaryReader r(bytes.data(), bytes.size());
    loader::GenericElfImage elf;
    const Status st = loader::ReadGenericElf64(r, &elf);
    if (st.code() != want) {
      char buf[128];
      std::snprintf(buf, sizeof(buf), "expected %s got %s",
                    StatusCodeName(want), st.name());
      p5emu::test::ReportFailure(__FILE__, __LINE__, buf);
    }
  };

  // Empty -> truncated (too short for a header).
  expect({}, StatusCode::kTruncatedInput);

  // Bad magic.
  {
    auto b = good.bytes; b[1] = 'X';
    expect(b, StatusCode::kInvalidMagic);
  }
  // Wrong class.
  {
    auto b = good.bytes; b[4] = 1;  // ELFCLASS32
    expect(b, StatusCode::kUnsupportedClass);
  }
  // Wrong endianness.
  {
    auto b = good.bytes; b[5] = 0;
    expect(b, StatusCode::kUnsupportedEndian);
  }
  // Truncated header.
  {
    auto b = good.bytes; b.resize(20);
    expect(b, StatusCode::kTruncatedInput);
  }
  // Invalid phentsize.
  {
    auto b = good.bytes;
    b.resize(b.size() < 64 ? 64 : b.size());
    b[54] = 0x20;  // e_phentsize = 32, not 56
    expect(b, StatusCode::kInvalidProgramHeader);
  }
  // Absurd phnum.
  {
    auto b = good.bytes;
    b.resize(b.size() < 64 ? 64 : b.size());
    b[56] = 0xFF; b[57] = 0xFF;
    expect(b, StatusCode::kTooManyEntries);
  }
  // Truncated program-header table.
  {
    auto b = good.bytes;
    b.resize(64 + 10);  // claims 1 phdr of 56 bytes but only 10 present
    expect(b, StatusCode::kTruncatedInput);
  }
}

// ===========================================================================
// PS5 layer.
// ===========================================================================
P5EMU_TEST(LoaderPs5, DialectModulesAndUnknownTags) {
  Elf64FixtureOptions o;
  o.text = {0x90};
  o.needed_modules = {"libkernel.sprx"};
  // PS5 import/export tags and an unknown tag, preserved not interpreted.
  o.dynamic_extra = {
      {0x61000015, 0x1234},   // DT_OS_IMPORT_LIB (number is a fact)
      {0x61000013, 0x5678},   // DT_OS_EXPORT_LIB
      {0x61000050, 0x9ABC},   // PS5-era "_1" range: UNKNOWN semantics
      {0x00ABCDEF, 0x0000},   // entirely unknown tag
  };
  const auto fx = BuildElf64Fixture(o);
  auto loaded = LoadFixture(fx.bytes);
  REQUIRE(loaded.status.ok());

  CHECK(loaded.ps5.is_ps5_dialect);
  CHECK_EQ(loaded.ps5.machine, std::uint16_t{62});
  CHECK_EQ(loaded.ps5.osabi, std::uint8_t{9});

  // Generic DT_NEEDED resolves a module name through the string table.
  REQUIRE(loaded.ps5.needed_modules.size() == 1);
  CHECK_EQ(loaded.ps5.needed_modules[0].name, std::string("libkernel.sprx"));
  CHECK_EQ(loaded.ps5.needed_modules[0].resolved, false);

  // PS5 tags preserved; the "_1" variant flagged unknown.
  CHECK(loaded.ps5.declares_ps5_imports);
  CHECK(loaded.ps5.declares_ps5_exports);
  bool saw_variant = false;
  for (const auto& t : loaded.ps5.ps5_tags) {
    if (t.tag == 0x61000050) saw_variant = t.is_unknown_variant;
  }
  CHECK(saw_variant);

  // The unknown tag survives with its value and no invented meaning.
  REQUIRE(loaded.ps5.unknown_tags.size() == 1);
  CHECK_EQ(loaded.ps5.unknown_tags[0].tag, std::int64_t{0x00ABCDEF});

  // And the plan carries the unresolved requirement.
  REQUIRE(loaded.plan.unresolved_imports.size() == 1);
  CHECK_EQ(loaded.plan.unresolved_imports[0].name,
           std::string("libkernel.sprx"));
}

P5EMU_TEST(LoaderPs5, WrongMachineIsRejected) {
  Elf64FixtureOptions o;
  o.text = {0x90};
  o.machine = 183;  // EM_AARCH64; EM_X86_64 is 62
  const auto fx = BuildElf64Fixture(o);
  const loader::BinaryReader r(fx.bytes.data(), fx.bytes.size());
  loader::GenericElfImage elf;
  REQUIRE(loader::ReadGenericElf64(r, &elf).ok());
  loader::Ps5ExecutableImage ps5;
  CHECK_EQ(loader::ParsePs5Executable(r, elf, &ps5).code(),
           StatusCode::kUnsupportedMachine);
}

// ===========================================================================
// Plan validation.
// ===========================================================================
P5EMU_TEST(LoaderPlan, EntryMustBeExecutable) {
  Elf64FixtureOptions o;
  o.text = {0x90};
  o.text_flags = 0x6;  // R|W but NOT X: entry cannot be fetched
  const auto fx = BuildElf64Fixture(o);
  const loader::BinaryReader r(fx.bytes.data(), fx.bytes.size());
  loader::GenericElfImage elf;
  REQUIRE(loader::ReadGenericElf64(r, &elf).ok());
  REQUIRE(loader::ValidateGenericLoadSegments(r, elf).ok());
  loader::Ps5ExecutableImage ps5;
  REQUIRE(loader::ParsePs5Executable(r, elf, &ps5).ok());
  loader::LoadPlan plan;
  REQUIRE(loader::BuildLoadPlan(r, ps5, &plan).ok());
  CHECK_EQ(loader::ValidateLoadPlan(plan).code(),
           StatusCode::kInvalidEntryPoint);
}

P5EMU_TEST(LoaderPlan, PermissionsAndBssSurviveIntoThePlan) {
  Elf64FixtureOptions o;
  o.text = {0x90};
  o.data = {0xAA, 0xBB};
  o.bss_size = 0x100;  // zero-fill tail
  const auto fx = BuildElf64Fixture(o);
  auto loaded = LoadFixture(fx.bytes);
  REQUIRE(loaded.status.ok());
  REQUIRE(loaded.plan.segments.size() == 2);
  const auto& text = loaded.plan.segments[0];
  const auto& data = loaded.plan.segments[1];
  CHECK_EQ(text.permissions, memory::kPermRead | memory::kPermExecute);
  CHECK_EQ(data.permissions, memory::kPermRead | memory::kPermWrite);
  CHECK_EQ(data.ZeroFillSize(), std::uint64_t{0x100});
}

P5EMU_TEST(LoaderPlan, OverlappingSegmentsAreRejected) {
  loader::LoadPlan plan;
  plan.entry = 0x1000;
  plan.segments.push_back({0x1000, 0x10, 0x10, 0, memory::kPermRead | memory::kPermExecute, 0});
  plan.segments.push_back({0x1008, 0x10, 0x10, 0x10, memory::kPermRead, 0});
  CHECK_EQ(loader::ValidateLoadPlan(plan).code(),
           StatusCode::kOverlappingMapping);
}

P5EMU_TEST(LoaderPlan, FileszGreaterThanMemszIsRejected) {
  loader::LoadPlan plan;
  plan.entry = 0x1000;
  plan.segments.push_back({0x1000, 0x20, 0x10, 0, memory::kPermRead | memory::kPermExecute, 0x1000});
  CHECK_EQ(loader::ValidateLoadPlan(plan).code(), StatusCode::kInvalidSegment);
}

// ===========================================================================
// Transactional apply.
// ===========================================================================
P5EMU_TEST(LoaderApply, FailureLeavesTargetUntouched) {
  Elf64FixtureOptions o;
  o.text = {0x90};
  o.data = {0xAA, 0xBB};
  o.bss_size = 0x40;
  const auto fx = BuildElf64Fixture(o);
  const loader::BinaryReader r(fx.bytes.data(), fx.bytes.size());
  loader::GenericElfImage elf;
  REQUIRE(loader::ReadGenericElf64(r, &elf).ok());
  REQUIRE(loader::ValidateGenericLoadSegments(r, elf).ok());
  loader::Ps5ExecutableImage ps5;
  REQUIRE(loader::ParsePs5Executable(r, elf, &ps5).ok());
  loader::LoadPlan plan;
  REQUIRE(loader::BuildLoadPlan(r, ps5, &plan).ok());
  REQUIRE(loader::ValidateLoadPlan(plan).ok());

  // Pre-populate the target with a sentinel region.
  memory::MappedGuestMemory target;
  REQUIRE(target.AddRegion({0x9000, 0x100, memory::kPermReadWrite}).ok());
  const std::uint8_t marker[4] = {1, 2, 3, 4};
  REQUIRE(target.WriteImage(0x9000, marker, 4).ok());
  REQUIRE(target.RegionCount() == 1);

  // Fail after the first apply operation (mid-way through a multi-segment plan).
  loader::ApplyOptions opts;
  opts.fail_after_operations = 1;
  const Status st = loader::ApplyLoadPlan(r, plan, opts, &target);
  CHECK_EQ(st.code(), StatusCode::kApplyFailed);

  // The visible guest state is UNCHANGED: still the sentinel region only.
  CHECK_EQ(target.RegionCount(), std::size_t{1});
  CHECK_EQ(target.PermissionsAt(0x9000), std::uint32_t{memory::kPermReadWrite});
  std::uint64_t v = 0;
  REQUIRE(target.ReadData(0x9000, 1, &v).ok());
  CHECK_EQ(v, std::uint64_t{1});
  CHECK(!target.IsMapped(fx.text_vaddr));

  // A full apply then succeeds and replaces the memory.
  const Status ok = loader::ApplyLoadPlan(r, plan, {}, &target);
  CHECK(ok.ok());
  CHECK_EQ(target.RegionCount(), plan.segments.size());
}

// ===========================================================================
// End-to-end: fixture -> loader -> guest memory -> CPU -> known result.
// ===========================================================================
P5EMU_TEST(LoaderE2E, FixtureExecutesFromGuestMemory) {
  // mov eax, 0x2A ; mov ecx, 0x10 ; add eax, ecx   => eax = 0x3A
  const std::vector<std::uint8_t> program = {
      0xB8, 0x2A, 0x00, 0x00, 0x00, 0xB9, 0x10, 0x00, 0x00, 0x00, 0x01, 0xC8};
  Elf64FixtureOptions o;
  o.text = program;
  o.text_flags = 0x5;  // R|X (single region, so the AArch64 backend can follow)
  const auto fx = BuildElf64Fixture(o);
  auto loaded = LoadFixture(fx.bytes);
  REQUIRE(loaded.status.ok());

  // Loader -> CPU entry state (RIP only).
  cpu::CpuState state;
  REQUIRE(loader::MakeEntryState(loaded.plan, &state).ok());
  CHECK_EQ(state.rip, fx.entry);

  // Fetch the program bytes FROM GUEST MEMORY.
  std::vector<std::uint8_t> fetched(program.size(), 0);
  std::size_t available = 0;
  const Status fc =
      loaded.memory.FetchCode(fx.entry, program.size(), fetched.data(), &available);
  REQUIRE(fc.ok());
  REQUIRE(available == program.size());
  for (std::size_t i = 0; i < program.size(); ++i) {
    CHECK_EQ(fetched[i], program[i]);
  }

  // Decode -> IR -> validate -> interpret.
  ir::Program ir;
  std::vector<memory::GuestAddr> addresses;
  REQUIRE(cpu::TranslateProgram(fx.entry, fetched.data(), fetched.size(), "e2e", &ir,
                           &addresses)
              .ok());
  REQUIRE(ir::ValidateIr(ir).ok());

  memory::MappedGuestMemory mem2;
  {
    // A fresh, identical apply so the interpreter owns its memory.
    const loader::BinaryReader r(fx.bytes.data(), fx.bytes.size());
    REQUIRE(loader::ApplyLoadPlan(r, loaded.plan, {}, &mem2).ok());
  }
  cpu::CpuState run_state;
  REQUIRE(loader::MakeEntryState(loaded.plan, &run_state).ok());
  cpu::ReferenceInterpreter interp(kStepLimit);
  const cpu::ExecutionResult res = interp.Execute(ir, &run_state, mem2);
  CHECK_EQ(res.reason, cpu::TerminationReason::kFallthrough);
  CHECK_EQ(run_state.rax, std::uint64_t{0x3A});
}

P5EMU_TEST(LoaderE2E, LoadedProgramAlsoAgreesOnAArch64) {
  const std::vector<std::uint8_t> program = {
      0xB8, 0x2A, 0x00, 0x00, 0x00, 0xB9, 0x10, 0x00, 0x00, 0x00, 0x01, 0xC8};
  Elf64FixtureOptions o;
  o.text = program;
  o.text_flags = 0x5;
  const auto fx = BuildElf64Fixture(o);
  auto loaded = LoadFixture(fx.bytes);
  REQUIRE(loaded.status.ok());

  cpu::QemuArm64Runner runner(HarnessPath());
  if (!runner.available()) {
    p5emu::test::ReportFailure(__FILE__, __LINE__,
                               "AArch64 harness not available");
    return;
  }

  std::vector<std::uint8_t> fetched(program.size(), 0);
  std::size_t available = 0;
  REQUIRE(loaded.memory.FetchCode(fx.entry, program.size(), fetched.data(),
                                  &available)
              .ok());
  ir::Program ir;
  std::vector<memory::GuestAddr> addresses;
  REQUIRE(cpu::TranslateProgram(fx.entry, fetched.data(), fetched.size(), "e2e-a64",
                           &ir, &addresses)
              .ok());

  memory::MappedGuestMemory mem;
  const loader::BinaryReader r(fx.bytes.data(), fx.bytes.size());
  REQUIRE(loader::ApplyLoadPlan(r, loaded.plan, {}, &mem).ok());

  cpu::CpuState state;
  REQUIRE(loader::MakeEntryState(loaded.plan, &state).ok());
  auto backend = cpu::MakeArm64Backend(kStepLimit, &runner);
  const cpu::ExecutionResult res = backend->Execute(ir, &state, mem);
  CHECK_EQ(res.reason, cpu::TerminationReason::kFallthrough);
  CHECK_EQ(state.rax, std::uint64_t{0x3A});
}
