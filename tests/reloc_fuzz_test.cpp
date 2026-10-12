// SPDX-License-Identifier: LicenseRef-P5EMU-Proprietary
//
// P5EMU source is independently authored and proprietary. See LICENSE.md and
// PROVENANCE.md before adding code here.
//
// ============================================================================
// RELOCATION MUTATION CAMPAIGN (Phase 5)
// ============================================================================
//
// Deterministic malformed-input testing focused on relocation and symbol
// tables. Invariant: a mutated image either yields a valid, internally
// consistent relocation model and load plan, or a deterministic error Status --
// never a crash, OOB, or UB. Run under ASan/UBSan this is the safety proof.
// ============================================================================

#include <cstdint>
#include <string>
#include <vector>

#include "TestHarness.h"

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
using p5emu::test::BuildRelocElf64;
using p5emu::test::RelocEntry;
using p5emu::test::RelocFixtureOptions;
using p5emu::test::RelocSym;

void Put64(std::vector<std::uint8_t>& b, std::size_t o, std::uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    b[o + static_cast<std::size_t>(i)] =
        static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF);
  }
}

RelocFixtureOptions RelocFuzzBase() {
  namespace rd = p5emu::test::reloc_detail;
  RelocFixtureOptions o;
  o.text.assign(23, 0);
  o.text[0] = 0x48; o.text[1] = 0xB8;               // mov rax, imm64 (imm@2)
  o.text[10] = 0x48; o.text[11] = 0xB9;             // mov rcx, imm64 (imm@12)
  o.text[20] = 0x48; o.text[21] = 0x8B; o.text[22] = 0x09;  // mov rcx,[rcx]
  rd::Put64(o.text, 12, o.data_vaddr);
  o.data.assign(8, 0);
  o.symbols.push_back(RelocSym{"p5emu_value", 0, true});
  o.rela_dyn.push_back(RelocEntry{o.text_vaddr + 2, 1 /*k64*/, 1, 0x22});
  o.rela_dyn.push_back(RelocEntry{o.data_vaddr, 8 /*RELATIVE*/, 0, 0x11});
  return o;
}

// Full relocation-aware pipeline; returns the first failing Status.
Status TryLoadReloc(const std::vector<std::uint8_t>& bytes, std::uint64_t bias) {
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
  st = loader::BuildLoadPlan(r, ps5, bias, &plan);
  if (!st.ok()) return st;
  st = loader::ValidateLoadPlan(plan);
  if (!st.ok()) return st;
  loader::RegistryResolver resolver;
  resolver.AddStringSymbol("", "p5emu_value", 0x500000);
  loader::ImportBinding binding;
  st = loader::ResolveImports(plan, resolver, &binding);
  if (!st.ok()) return st;
  memory::MappedGuestMemory mem;
  st = loader::ApplyLoadPlan(r, plan, binding, {}, &mem);
  if (!st.ok()) return st;
  // A successful load must be internally consistent.
  if (plan.segments.size() != mem.RegionCount()) return StatusCode::kInternal;
  return StatusCode::kOk;
}

}  // namespace

P5EMU_TEST(RelocFuzz, RelocationMutationCampaign) {
  const auto base = BuildRelocElf64(RelocFuzzBase());
  const std::size_t n = base.bytes.size();
  std::uint64_t cases = 0;

  const std::uint64_t biases[] = {0ull, 0x1000'0000ull,
                                  0xFFFF'FFFF'FFFF'0000ull};

  auto run = [&](const std::vector<std::uint8_t>& bytes) {
    for (std::uint64_t b : biases) {
      ++cases;
      const Status st = TryLoadReloc(bytes, b);
      if (st.code() == StatusCode::kInternal) {
        p5emu::test::ReportFailure(__FILE__, __LINE__,
                                   "inconsistent successful reloc load");
      }
    }
  };

  // 1. Every bit of the file (tables live in the data segment).
  for (std::size_t i = 0; i < n; ++i) {
    for (int bit = 0; bit < 8; ++bit) {
      auto b = base.bytes;
      b[i] = static_cast<std::uint8_t>(b[i] ^ (1u << bit));
      run(b);
    }
  }

  // 2. Truncations at a stride.
  for (std::size_t len = 0; len <= n; len += 3) {
    run(std::vector<std::uint8_t>(
        base.bytes.begin(),
        base.bytes.begin() + static_cast<std::ptrdiff_t>(len)));
  }

  // 3. Extreme values in the relocation/dynamic region (data segment).
  const std::uint64_t extremes[] = {0ull, 1ull, 0xFFFF'FFFF'FFFF'FFFFull,
                                    0x7FFF'FFFF'FFFF'FFFFull};
  for (std::uint64_t v : extremes) {
    for (std::size_t off = 0x2000; off + 8 <= n; off += 8) {
      auto b = base.bytes;
      Put64(b, off, v);
      run(b);
    }
  }

  std::printf("relocation mutation campaign: %llu deterministic cases\n",
              static_cast<unsigned long long>(cases));
  CHECK(cases >= 3000);
}
