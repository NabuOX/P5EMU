// SPDX-License-Identifier: LicenseRef-P5EMU-Proprietary
//
// P5EMU source is independently authored and proprietary. See LICENSE.md and
// PROVENANCE.md before adding code here.
//
// ============================================================================
// FIXTURE PROVENANCE
// ============================================================================
//
// THE RULE
//
// Every instruction fixture that asserts an architectural result must carry
// provenance for its EXPECTED values. Acceptable sources:
//
//   * Intel SDM / AMD64 APM section reference
//   * an independently authored mathematical derivation
//   * an independently generated fixture from an approved permissive test tool
//   * a manually reasoned value, with the reasoning written down
//
// NOT acceptable, ever:
//
//   "Ran P5EMU once and recorded whatever it returned."
//
// That proves determinism, not correctness, and it is the single most damaging
// habit an emulator project can acquire: it makes every later test validate the
// current behaviour rather than the architecture, so a bug that is encoded once
// becomes permanently enshrined.
//
// WHY THIS IS A TYPE AND NOT A COMMENT
//
// A comment is not enforceable, so the requirement is expressed as a type that
// cannot be satisfied by accident:
//
//   * `Expectation` takes its `Provenance` as a REQUIRED constructor argument.
//     There is no default constructor and no setter, so an expectation with no
//     provenance cannot be expressed at all.
//   * `ExpectationSet::Add` runs `ValidateProvenance` and reports ill-formed
//     records instead of storing them, so a bad citation cannot be laundered
//     through the set.
//
// This is scoped to expectations registered through `ExpectationSet`. The
// pipeline fixtures in end_to_end_test.cpp carry their provenance as inline
// citations next to each derived value; scripts/check_provenance.sh enforces the
// file-level SPDX and licence requirements across the whole tree.
// ============================================================================

#ifndef P5EMU_TESTS_FIXTURE_PROVENANCE_H
#define P5EMU_TESTS_FIXTURE_PROVENANCE_H

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "p5emu/core/Status.h"

namespace p5emu::test {

// Where an expected value came from. Kept small and explicit so that adding a
// new kind is a deliberate act.
enum class ProvenanceKind : std::uint8_t {
  // Cited directly from the Intel SDM or the AMD64 APM.
  kIntelSdm = 0,
  kAmdApm,
  // Derived by hand from the architectural rule, with the derivation written
  // out in `detail`.
  kHandDerived,
  // Two's-complement arithmetic worked out independently of any emulator.
  kArithmeticDerivation,
  // Verified against real hardware by a named measurement.
  kHardwareVerified,
  // Produced by an approved permissive third-party tool, named in `detail`.
  // Phase 1 has none; the slot exists so that using one later is explicit.
  kExternalTool,
};

const char* ProvenanceKindName(ProvenanceKind kind) noexcept;

// A string containing nothing but whitespace carries no information.
[[nodiscard]] inline bool IsBlank(const std::string& s) noexcept {
  for (const char c : s) {
    if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
      return false;
    }
  }
  return true;
}

// A citation for one expected value.
struct Provenance {
  ProvenanceKind kind = ProvenanceKind::kHandDerived;
  std::string reference;  // e.g. "Intel SDM Vol.2 §3.1.2" or "0xFF+1 = 0x100"
  std::string detail;     // the derivation or the measurement, in full

  [[nodiscard]] bool IsWellFormed() const noexcept {
    // Whitespace is not evidence: a citation of "   " must not pass, or the
    // check becomes trivially satisfiable by accident.
    return !IsBlank(reference) && !IsBlank(detail);
  }
  [[nodiscard]] std::string Describe() const {
    return std::string(ProvenanceKindName(kind)) + ": " + reference;
  }
};

// Convenience constructors. They take `detail` as a required argument so that a
// bare citation with no reasoning cannot be written by accident.
Provenance FromSdm(std::string section, std::string detail);
Provenance FromApm(std::string section, std::string detail);
Provenance Derived(std::string rule, std::string derivation);
Provenance Arithmetic(std::string derivation);
Provenance FromHardware(std::string measurement, std::string conditions);
Provenance FromExternalTool(std::string tool, std::string version,
                            std::string detail);

// Checks that a citation is usable as evidence.
//
// Rejects:
//   * a missing or whitespace-only reference or detail;
//   * an external-tool citation that does not name a tool AND a version, since
//     "some tool" is not reproducible;
//   * a citation that names P5EMU's own output as its source, which is the
//     specific failure mode the policy above exists to prevent -- such a value
//     only proves determinism, never correctness.
Status ValidateProvenance(const Provenance& prov, std::string* reason);

// One architectural value together with the evidence for it.
//
// There is deliberately no default constructor: provenance cannot be omitted.
struct Expectation {
  std::string name;
  std::uint64_t expected = 0;
  Provenance provenance;

  Expectation(std::string expectation_name, std::uint64_t expected_value,
              Provenance prov)
      : name(std::move(expectation_name)),
        expected(expected_value),
        provenance(std::move(prov)) {}

  Expectation(const Expectation&) = default;
  Expectation(Expectation&&) noexcept = default;
  Expectation& operator=(const Expectation&) = default;
  Expectation& operator=(Expectation&&) noexcept = default;
  ~Expectation() = default;
};

// A collection of expectations whose provenance is validated on insertion.
//
// `Add` returns false and records a failure instead of storing an ill-formed
// expectation, so `Size()` alone never implies that everything in the set is
// justified.
class ExpectationSet {
 public:
  ExpectationSet() = default;

  bool Add(std::string name, std::uint64_t expected, Provenance prov);
  bool Add(Expectation expectation);

  [[nodiscard]] std::size_t Size() const noexcept { return entries_.size(); }
  [[nodiscard]] bool Empty() const noexcept { return entries_.empty(); }
  [[nodiscard]] const std::vector<std::string>& Failures() const noexcept {
    return failures_;
  }
  [[nodiscard]] bool AllWellFormed() const noexcept { return failures_.empty(); }

  [[nodiscard]] const Expectation* Find(const std::string& name) const noexcept;

 private:
  std::vector<Expectation> entries_;
  std::vector<std::string> failures_;
};

}  // namespace p5emu::test

#endif  // P5EMU_TESTS_FIXTURE_PROVENANCE_H
