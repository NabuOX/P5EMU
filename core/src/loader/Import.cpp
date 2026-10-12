// SPDX-License-Identifier: LicenseRef-P5EMU-Proprietary
//
// P5EMU source is independently authored and proprietary. See LICENSE.md and
// PROVENANCE.md before adding code here.

#include "p5emu/loader/Import.h"

namespace p5emu::loader {

const char* SymbolIdentityKindName(SymbolIdentityKind kind) noexcept {
  switch (kind) {
    case SymbolIdentityKind::kUnknown: return "unknown";
    case SymbolIdentityKind::kStringName: return "string-name";
    case SymbolIdentityKind::kNumericId: return "numeric-id";
    case SymbolIdentityKind::kOpaqueId: return "opaque-id";
  }
  return "unknown";
}

std::string SymbolIdentity::Describe() const {
  switch (kind) {
    case SymbolIdentityKind::kStringName:
      return "name:" + name;
    case SymbolIdentityKind::kNumericId:
      return "numeric:0x" + std::to_string(numeric_id);
    case SymbolIdentityKind::kOpaqueId:
      return "opaque:" + std::to_string(opaque.size()) + " bytes";
    case SymbolIdentityKind::kUnknown:
    default:
      return "unknown";
  }
}

const char* ResolutionStatusName(ResolutionStatus status) noexcept {
  switch (status) {
    case ResolutionStatus::kResolved: return "resolved";
    case ResolutionStatus::kUnresolved: return "unresolved";
    case ResolutionStatus::kUnsupportedIdentity: return "unsupported-identity";
    case ResolutionStatus::kAmbiguous: return "ambiguous";
    case ResolutionStatus::kError: return "error";
  }
  return "unknown";
}

void RegistryResolver::AddStringSymbol(const std::string& module,
                                       const std::string& name,
                                       std::uint64_t address) {
  entries_.push_back(Entry{module, name, address});
}

ResolutionResult RegistryResolver::ResolveSymbol(
    const std::string& module, const SymbolIdentity& symbol) {
  if (symbol.kind != SymbolIdentityKind::kStringName) {
    // The registry only knows string names. A numeric/opaque identity is not an
    // error -- it is simply not something this resolver can decide.
    ResolutionResult r;
    r.status = ResolutionStatus::kUnsupportedIdentity;
    r.detail = "registry resolver handles string names only";
    return r;
  }

  std::uint64_t found = 0;
  int matches = 0;
  for (const Entry& e : entries_) {
    if (e.module == module && e.name == symbol.name) {
      found = e.address;
      ++matches;
    }
  }
  if (matches == 0) {
    return ResolutionResult::Unresolved();
  }
  if (matches > 1) {
    ResolutionResult r;
    r.status = ResolutionStatus::kAmbiguous;
    r.detail = "more than one provider for this symbol";
    return r;
  }
  return ResolutionResult::Resolved(found);
}

}  // namespace p5emu::loader
