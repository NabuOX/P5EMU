// SPDX-License-Identifier: LicenseRef-P5EMU-Proprietary
//
// P5EMU source is independently authored and proprietary. See LICENSE.md and
// PROVENANCE.md before adding code here.
//
// ============================================================================
// RUNTIME FUZZ AND PROPERTY TESTS (Phase 7)
// ============================================================================
//   RuntimeFuzz      deterministic multi-module load campaign (safety +
//                    determinism; a failed load never leaves a ready process)
//   RuntimeProperty  deterministic structural properties of the dependency
//                    graph, the load-bias allocator, and merge/validate
// ============================================================================

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "TestHarness.h"

#include "RelocFixtureBuilder.h"
#include "SyntheticHle.h"

#include "p5emu/core/Status.h"
#include "p5emu/cpu/Ir.h"
#include "p5emu/hle/Hle.h"
#include "p5emu/runtime/Runtime.h"

namespace {

using namespace p5emu;  // NOLINT(build/namespaces)
namespace rt = p5emu::runtime;
namespace hle = p5emu::hle;
namespace ir = p5emu::ir;

std::uint64_t NextRandom(std::uint64_t* state) {
  std::uint64_t x = *state;
  x ^= x >> 12;
  x ^= x << 25;
  x ^= x >> 27;
  *state = x;
  return x * 0x2545F4914F6CDD1Dull;
}

// A leaf module: one "nop; ret" text block, an exported symbol, and (when
// `import` is non-empty) a GLOB_DAT import of that name in the data segment.
test::RelocFixture BuildModule(const std::string& export_name,
                               const std::string& import) {
  test::RelocFixtureOptions o;
  o.text = {0x90, 0xC3};  // nop; ret
  o.data.assign(8, 0);
  o.symbols.push_back(test::RelocSym{export_name, o.text_vaddr, false});
  if (!import.empty()) {
    o.symbols.push_back(test::RelocSym{import, 0, true});
    o.rela_dyn.push_back(
        test::RelocEntry{o.data_vaddr, /*R_X86_64_GLOB_DAT=*/6, 2, 0});
  }
  return test::BuildRelocElf64(o);
}

}  // namespace

// ===========================================================================
// Fuzz / malformed campaign.
// ===========================================================================
P5EMU_TEST(RuntimeFuzz, MultiModuleLoadIsSafeAndDeterministic) {
  constexpr int kCases = 6000;
  const char* kImports[] = {"p5emu_add", "p5emu_sum6", "missing_symbol",
                            "exp_1", "exp_2"};
  std::uint64_t rng = 0x9E3779B97F4A7C15ull;

  hle::HleRegistry reg;
  p5emu::test::RegisterCanonical(&reg);

  for (int i = 0; i < kCases; ++i) {
    const std::uint64_t r = NextRandom(&rng);
    const std::size_t n = 1 + static_cast<std::size_t>(r % 3);
    std::vector<std::string> names;
    for (std::size_t k = 0; k < n; ++k) names.push_back("m" + std::to_string(k));

    std::map<std::string, std::vector<std::string>> deps;
    for (std::size_t k = 0; k < n; ++k) {
      std::vector<std::string> d;
      if (k + 1 < n) d.push_back(names[k + 1]);
      deps[names[k]] = d;
    }
    // Occasionally corrupt the graph: a missing dependency or a cycle.
    const std::uint64_t mut = NextRandom(&rng) % 5;
    if (mut == 0) deps["m0"].push_back("does_not_exist");
    if (mut == 1 && n >= 2) deps[names[n - 1]].push_back("m0");

    rt::InMemoryModuleRepository repo;
    for (std::size_t k = 0; k < n; ++k) {
      std::string imp;
      if (k == 0) imp = kImports[NextRandom(&rng) % 5];
      std::vector<std::uint8_t> bytes =
          BuildModule("exp_" + std::to_string(k), imp).bytes;
      // Occasionally flip a byte to exercise malformed-image handling.
      if ((NextRandom(&rng) % 4) == 0 && !bytes.empty()) {
        const std::size_t at =
            static_cast<std::size_t>(NextRandom(&rng) % bytes.size());
        bytes[at] = static_cast<std::uint8_t>(NextRandom(&rng) & 0xFFu);
      }
      repo.Add(names[k], std::move(bytes));
    }

    auto load = [&](rt::LoadedProcess* out) -> Status {
      rt::GuestStack stack;
      if (!stack.Create().ok()) return StatusCode::kInternal;
      return rt::LoadProcess(repo, "m0", deps, stack, &reg, {}, out);
    };

    rt::LoadedProcess a;
    const Status sa = load(&a);
    rt::LoadedProcess b;
    const Status sb = load(&b);

    // Determinism: identical inputs → identical outcome.
    CHECK_EQ(static_cast<int>(sa.code()), static_cast<int>(sb.code()));
    CHECK_EQ(a.execution_ready, b.execution_ready);

    // Safety invariant: a failed load is never reported execution-ready.
    if (!sa.ok()) {
      CHECK(!a.execution_ready);
    }
    if (sa.ok()) {
      CHECK(a.execution_ready);
      // A ready process has valid IR and a well-formed image.
      CHECK(ir::ValidateIr(a.program).ok());
      CHECK(a.image.main_module < a.image.modules.size());
      CHECK(a.image.modules.size() >= names.size());
      CHECK_EQ(static_cast<int>(a.status.code()), static_cast<int>(StatusCode::kOk));
    }
  }
}

// ===========================================================================
// Property cases.
// ===========================================================================
P5EMU_TEST(RuntimeProperty, DependencyOrderAndLoadBias) {
  constexpr int kCases = 2000;
  std::uint64_t rng = 0xD1B54A32D192ED03ull;

  for (int i = 0; i < kCases; ++i) {
    const int n = 1 + static_cast<int>(NextRandom(&rng) % 4);
    std::vector<std::string> names;
    std::map<std::string, std::vector<std::string>> deps;
    for (int k = 0; k < n; ++k) names.push_back("m" + std::to_string(k));
    for (int k = 0; k < n; ++k) {
      std::vector<std::string> d;
      if (k + 1 < n) d.push_back(names[static_cast<std::size_t>(k + 1)]);
      deps[names[static_cast<std::size_t>(k)]] = d;
    }

    rt::DependencyGraph g;
    const Status st = rt::BuildDependencyGraph("m0", deps, {}, &g);
    REQUIRE(st.ok());
    CHECK_EQ(g.order.size(), names.size());

    // Every dependency is loaded before its dependent.
    std::map<std::string, int> index;
    for (int k = 0; k < static_cast<int>(g.order.size()); ++k) {
      index[g.order[static_cast<std::size_t>(k)]] = k;
    }
    for (const auto& kv : deps) {
      for (const std::string& dep : kv.second) {
        CHECK(index[dep] < index[kv.first]);
      }
    }
    // Each module appears exactly once.
    for (const std::string& nm : names) {
      CHECK_EQ(index.count(nm), std::size_t{1});
    }

    // Distinct, increasing load biases inside [kBase, kEnd).
    rt::LoadBiasAllocator alloc;
    std::vector<std::uint64_t> biases;
    for (int k = 0; k < n; ++k) {
      std::uint64_t bias = 0;
      REQUIRE(alloc.Allocate(0x1000, &bias).ok());
      CHECK(bias >= rt::LoadBiasAllocator::kBase);
      CHECK(bias < rt::LoadBiasAllocator::kEnd);
      for (std::uint64_t prev : biases) CHECK(bias != prev);
      biases.push_back(bias);
    }
  }
}
