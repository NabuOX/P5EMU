// SPDX-License-Identifier: Apache-2.0
//
// P5EMU is an independent, open-source project. See LICENSE and
// PROVENANCE.md before adding code here.
//
// ============================================================================
// LOADER MALFORMED-INPUT AND MUTATION CAMPAIGN (Phase 4)
// ============================================================================
//
// A binary parser must not crash, read out of bounds, or leave partial state on
// hostile input. LoaderMalformed covers the named categories; LoaderFuzz runs a
// large deterministic mutation campaign whose invariant is:
//
//   the parser either returns a valid, internally consistent plan
//   or a deterministic error Status -- and nothing else.
//
// Run under ASan/UBSan, this is the memory-safety proof for the loader.
// ============================================================================

#include <cstdint>
#include <string>
#include <vector>

#include "TestHarness.h"

#include "ElfFixtureBuilder.h"
#include "RelocFixtureBuilder.h"

#include "p5emu/core/Status.h"
#include "p5emu/cpu/MappedGuestMemory.h"
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
using p5emu::test::BuildElf64Fixture;
using p5emu::test::Elf64FixtureOptions;

void Put16(std::vector<std::uint8_t>& b, std::size_t o, std::uint16_t v) {
  b[o + 0] = static_cast<std::uint8_t>(v & 0xFF);
  b[o + 1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
}
void Put32(std::vector<std::uint8_t>& b, std::size_t o, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) {
    b[o + static_cast<std::size_t>(i)] =
        static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF);
  }
}
void Put64(std::vector<std::uint8_t>& b, std::size_t o, std::uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    b[o + static_cast<std::size_t>(i)] =
        static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF);
  }
}

// Runs the full load pipeline and returns the first failing Status (or ok).
// Used as the invariant check: it must never crash and never partially apply.
Status TryLoad(const std::vector<std::uint8_t>& bytes) {
  const loader::BinaryReader r(bytes.data(), bytes.size());
  loader::GenericElfImage elf;
  Status st = loader::ReadGenericElf64(r, &elf);
  if (!st.ok()) return st;
  st = loader::ValidateGenericLoadSegments(r, elf);
  if (!st.ok()) return st;
  loader::Ps5ExecutableImage ps5;
  st = loader::ParsePs5Executable(r, elf, &ps5);
  if (!st.ok()) return st;
  loader::LoadPlan plan;
  st = loader::BuildLoadPlan(r, ps5, &plan);
  if (!st.ok()) return st;
  st = loader::ValidateLoadPlan(plan);
  if (!st.ok()) return st;
  memory::MappedGuestMemory mem;
  st = loader::ApplyLoadPlan(r, plan, {}, &mem);
  if (!st.ok()) return st;
  // A successful load must yield a validated plan and exactly the regions the
  // plan describes.
  if (plan.segments.size() != mem.RegionCount()) {
    return StatusCode::kInternal;
  }
  return StatusCode::kOk;
}

// Two-segment fixture (text + data with BSS) plus a dynamic table.
Elf64FixtureOptions RichFixture() {
  Elf64FixtureOptions o;
  o.text = {0x90, 0x90, 0x90};
  o.data = {0x11, 0x22, 0x33, 0x44};
  o.bss_size = 0x20;
  o.needed_modules = {"libSceLibcInternal.sprx"};
  o.dynamic_extra = {{0x61000015, 0}, {0x61000050, 0x42}};
  return o;
}

}  // namespace

// ===========================================================================
// Curated malformed categories.
// ===========================================================================
P5EMU_TEST(LoaderMalformed, SegmentAndHeaderCategoriesAreDistinguished) {
  const auto base = BuildElf64Fixture(RichFixture());
  auto check = [&](const std::vector<std::uint8_t>& b, StatusCode want) {
    const Status st = TryLoad(b);
    if (st.code() != want) {
      char msg[128];
      std::snprintf(msg, sizeof(msg), "expected %s got %s",
                    StatusCodeName(want), st.name());
      p5emu::test::ReportFailure(__FILE__, __LINE__, msg);
    }
  };

  // Baseline loads.
  check(base.bytes, StatusCode::kOk);

  // Text PT_LOAD is at file offset 64; its fields: type@0 flags@4 offset@8
  // vaddr@16 paddr@24 filesz@32 memsz@40 align@48.
  const std::size_t text_ph = 64;
  const std::size_t data_ph = 64 + 56;
  const std::size_t dyn_ph = 64 + 112;

  // filesz > memsz.
  {
    auto b = base.bytes;
    Put64(b, text_ph + 32, 0x100);  // filesz
    Put64(b, text_ph + 40, 0x10);   // memsz
    check(b, StatusCode::kInvalidSegment);
  }
  // Segment file range outside the input.
  {
    auto b = base.bytes;
    Put64(b, text_ph + 8, 0xFFFF'0000ull);  // p_offset far past EOF
    check(b, StatusCode::kTruncatedInput);
  }
  // Virtual address overflow.
  {
    auto b = base.bytes;
    Put64(b, text_ph + 16, 0xFFFF'FFFF'FFFF'FFF0ull);  // p_vaddr
    Put64(b, text_ph + 40, 0x100);                     // memsz -> vaddr+memsz wraps
    check(b, StatusCode::kIntegerOverflow);
  }
  // Impossible alignment (not a power of two).
  {
    auto b = base.bytes;
    Put64(b, text_ph + 48, 3);  // p_align
    check(b, StatusCode::kInvalidAlignment);
  }
  // Overlapping segments (data moved onto text). Uses a fixture without a
  // dynamic table so segment overlap is what is exercised, not the dynamic
  // string table resolution that a moved data segment would also disturb.
  {
    Elf64FixtureOptions o2;
    o2.text = {0x90};
    o2.data = {0x11};
    const auto f2 = BuildElf64Fixture(o2);
    auto b = f2.bytes;
    Put64(b, data_ph + 16, f2.text_vaddr);  // p_vaddr = text vaddr
    Put64(b, data_ph + 24, f2.text_vaddr);  // p_paddr
    Put64(b, data_ph + 48, 0x1000);         // keep alignment congruent
    check(b, StatusCode::kOverlappingMapping);
  }
  // Entry outside any executable mapping.
  {
    auto b = base.bytes;
    Put64(b, 24, 0x9999'0000ull);  // e_entry
    check(b, StatusCode::kInvalidEntryPoint);
  }
  // Malformed dynamic table (truncate the PT_DYNAMIC filesz to zero).
  {
    auto b = base.bytes;
    Put64(b, dyn_ph + 32, 0);  // p_filesz = 0
    check(b, StatusCode::kMalformedDynamicTable);
  }
  // Empty and short inputs.
  check({}, StatusCode::kTruncatedInput);
  { auto b = base.bytes; b.resize(7); check(b, StatusCode::kTruncatedInput); }
}

P5EMU_TEST(LoaderMalformed, DuplicateAndUnknownDynamicTagsDoNotCrash) {
  // Duplicate DT_STRTAB entries: parse must succeed; later value wins, no crash.
  Elf64FixtureOptions o;
  o.text = {0x90};
  o.data = {0};
  o.dynamic_extra = {{5 /*DT_STRTAB*/, 0x400000}, {0x1234, 1}, {0x1234, 2}};
  const auto fx = BuildElf64Fixture(o);
  const loader::BinaryReader r(fx.bytes.data(), fx.bytes.size());
  loader::GenericElfImage elf;
  REQUIRE(loader::ReadGenericElf64(r, &elf).ok());
  loader::Ps5ExecutableImage ps5;
  REQUIRE(loader::ParsePs5Executable(r, elf, &ps5).ok());
  // The duplicate generic tag and the unknown tag are preserved, not invented.
  CHECK(ps5.unknown_tags.size() >= 2);
}

P5EMU_TEST(LoaderMalformed, CheckedArithmeticHelpers) {
  CHECK(loader::BinaryReader::AddOverflows(0xFFFF'FFFF'FFFF'FFFFull, 1));
  CHECK(!loader::BinaryReader::AddOverflows(1, 2));
  CHECK(loader::BinaryReader::MulOverflows(0xFFFF'FFFF'FFFF'FFFFull, 2));
  CHECK(!loader::BinaryReader::MulOverflows(0x1000, 0x1000));
  CHECK(!loader::BinaryReader::MulOverflows(0, 0xFFFF'FFFF'FFFF'FFFFull));
}

// ===========================================================================
// Deterministic mutation campaign.
// ===========================================================================
P5EMU_TEST(LoaderFuzz, DeterministicMutationCampaign) {
  const auto base = BuildElf64Fixture(RichFixture());
  const std::size_t n = base.bytes.size();
  std::uint64_t cases = 0;

  auto run = [&](const std::vector<std::uint8_t>& bytes) {
    ++cases;
    const Status st = TryLoad(bytes);
    // Invariant: any Status is acceptable, but a successful load must have
    // produced a valid, internally consistent plan (checked inside TryLoad).
    if (st.code() == StatusCode::kInternal) {
      p5emu::test::ReportFailure(__FILE__, __LINE__,
                                 "inconsistent successful load");
    }
  };

  // 1. Truncate at every prefix length.
  for (std::size_t len = 0; len <= n; ++len) {
    run(std::vector<std::uint8_t>(base.bytes.begin(),
                                  base.bytes.begin() +
                                      static_cast<std::ptrdiff_t>(len)));
  }

  // 2. Flip every bit of the header + program-header region (the structural
  //    fields a hostile file would attack).
  const std::size_t struct_region = (64 + 3 * 56 < n) ? (64 + 3 * 56) : n;
  for (std::size_t i = 0; i < struct_region; ++i) {
    for (int bit = 0; bit < 8; ++bit) {
      auto b = base.bytes;
      b[i] = static_cast<std::uint8_t>(b[i] ^ (1u << bit));
      run(b);
    }
  }

  // 3. Set each 64-bit header/phdr field to extreme values.
  const std::uint64_t extremes[] = {0ull, 1ull, 0xFFFF'FFFF'FFFF'FFFFull,
                                    0x8000'0000'0000'0000ull, 0x1'0000'0000ull};
  const std::size_t field_offsets[] = {24, 32, 40};  // e_entry, e_phoff, e_shoff
  for (std::size_t fo : field_offsets) {
    for (std::uint64_t v : extremes) {
      auto b = base.bytes;
      if (fo + 8 <= b.size()) {
        Put64(b, fo, v);
        run(b);
      }
    }
  }
  // phdr 64-bit fields across the three entries.
  for (std::size_t e = 0; e < 3; ++e) {
    for (std::size_t rel : {std::size_t{8}, std::size_t{16}, std::size_t{32},
                            std::size_t{40}, std::size_t{48}}) {
      for (std::uint64_t v : extremes) {
        auto b = base.bytes;
        const std::size_t off = 64 + e * 56 + rel;
        if (off + 8 <= b.size()) {
          Put64(b, off, v);
          run(b);
        }
      }
    }
  }

  // 4. Mutate counts/sizes in the header (16/32-bit fields).
  for (std::uint16_t v : {std::uint16_t{0}, std::uint16_t{1},
                          std::uint16_t{0xFFFF}}) {
    auto b = base.bytes; Put16(b, 54, v); run(b);  // e_phentsize
    b = base.bytes; Put16(b, 56, v); run(b);        // e_phnum
  }
  for (std::uint32_t v : {0u, 0xFFFFFFFFu}) {
    auto b = base.bytes; Put32(b, 20, v); run(b);  // e_version
    b = base.bytes; Put32(b, 48, v); run(b);       // e_flags
  }

  // Well over the 2000-case hard gate.
  std::printf("loader mutation campaign: %llu deterministic cases\n",
              static_cast<unsigned long long>(cases));
  CHECK(cases >= 2000);
}
