// SPDX-License-Identifier: LicenseRef-P5EMU-Proprietary

#include "FixtureProvenance.h"

namespace p5emu::test {

const char* ProvenanceKindName(ProvenanceKind kind) noexcept {
  switch (kind) {
    case ProvenanceKind::kIntelSdm:           return "intel-sdm";
    case ProvenanceKind::kAmdApm:             return "amd-apm";
    case ProvenanceKind::kHandDerived:        return "hand-derived";
    case ProvenanceKind::kArithmeticDerivation: return "arithmetic";
    case ProvenanceKind::kHardwareVerified:   return "hardware-verified";
    case ProvenanceKind::kExternalTool:       return "external-tool";
  }
  return "unknown";
}

Provenance FromSdm(std::string section, std::string detail) {
  Provenance p;
  p.kind = ProvenanceKind::kIntelSdm;
  p.reference = std::move(section);
  p.detail = std::move(detail);
  return p;
}

Provenance FromApm(std::string section, std::string detail) {
  Provenance p;
  p.kind = ProvenanceKind::kAmdApm;
  p.reference = std::move(section);
  p.detail = std::move(detail);
  return p;
}

Provenance Derived(std::string rule, std::string derivation) {
  Provenance p;
  p.kind = ProvenanceKind::kHandDerived;
  p.reference = std::move(rule);
  p.detail = std::move(derivation);
  return p;
}

Provenance Arithmetic(std::string derivation) {
  Provenance p;
  p.kind = ProvenanceKind::kArithmeticDerivation;
  p.reference = "two's-complement arithmetic";
  p.detail = std::move(derivation);
  return p;
}

Provenance FromHardware(std::string measurement, std::string conditions) {
  Provenance p;
  p.kind = ProvenanceKind::kHardwareVerified;
  p.reference = std::move(measurement);
  p.detail = std::move(conditions);
  return p;
}

Provenance FromExternalTool(std::string tool, std::string version,
                            std::string detail) {
  Provenance p;
  p.kind = ProvenanceKind::kExternalTool;
  // The version is part of the reference so that "we ran a tool" is pinned to
  // something reproducible rather than to whatever the tool currently does.
  p.reference = tool + " " + version;
  p.detail = std::move(detail);
  return p;
}

namespace {

bool ContainsIgnoreCase(const std::string& haystack, const char* needle) {
  std::string h;
  h.reserve(haystack.size());
  for (const char c : haystack) {
    h.push_back(static_cast<char>(
        (c >= 'A' && c <= 'Z') ? (c - 'A' + 'a') : c));
  }
  std::string n;
  for (const char* p = needle; *p != '\0'; ++p) {
    n.push_back(static_cast<char>(
        (*p >= 'A' && *p <= 'Z') ? (*p - 'A' + 'a') : *p));
  }
  return h.find(n) != std::string::npos;
}

}  // namespace

Status ValidateProvenance(const Provenance& prov, std::string* reason) {
  const auto reject = [&](const char* why) {
    if (reason != nullptr) {
      *reason = why;
    }
    return StatusCode::kInvalidArgument;
  };

  if (IsBlank(prov.reference)) {
    return reject("provenance has no reference");
  }
  if (IsBlank(prov.detail)) {
    return reject("provenance has no derivation or measurement detail");
  }

  if (prov.kind == ProvenanceKind::kExternalTool) {
    // "reference" is "<tool> <version>"; without a space there is no version.
    if (prov.reference.find(' ') == std::string::npos) {
      return reject("external-tool provenance must name a tool AND a version");
    }
  }

  // The whole point of this type: a value may not cite P5EMU's own output as its
  // source. Doing so records current behaviour and then tests it forever.
  const std::string combined = prov.reference + " " + prov.detail;
  if (ContainsIgnoreCase(combined, "p5emu") ||
      ContainsIgnoreCase(combined, "recorded from") ||
      ContainsIgnoreCase(combined, "as observed by") ||
      ContainsIgnoreCase(combined, "golden output")) {
    return reject(
        "provenance may not cite P5EMU's own output as the source of an "
        "expected value");
  }

  if (reason != nullptr) {
    reason->clear();
  }
  return StatusCode::kOk;
}

bool ExpectationSet::Add(std::string name, std::uint64_t expected,
                         Provenance prov) {
  return Add(Expectation(std::move(name), expected, std::move(prov)));
}

bool ExpectationSet::Add(Expectation expectation) {
  std::string reason;
  const Status st = ValidateProvenance(expectation.provenance, &reason);
  if (!st.ok()) {
    failures_.push_back(expectation.name + ": " + reason);
    return false;
  }
  entries_.push_back(std::move(expectation));
  return true;
}

const Expectation* ExpectationSet::Find(
    const std::string& name) const noexcept {
  for (const Expectation& e : entries_) {
    if (e.name == name) {
      return &e;
    }
  }
  return nullptr;
}

}  // namespace p5emu::test
