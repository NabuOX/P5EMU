// SPDX-License-Identifier: LicenseRef-P5EMU-Proprietary
//
// P5EMU source is independently authored and proprietary. See LICENSE.md and
// PROVENANCE.md before adding code here.
//
// ============================================================================
// IMPORT MODEL AND RESOLVER
// ============================================================================
//
// The P5EMU-owned contract that a future PS5 HLE layer binds against. It is
// deliberately independent of any Sony structure layout: a requirement is a
// module identity plus a TYPED symbol identity, so an unknown identity is
// representable and no fake name or NID is ever invented.
//
// Symbol identity is typed (`StringName`, `NumericId`, `OpaqueId`, `Unknown`).
// Phase 5 synthetic tests use string identities; PS5 evidence can populate the
// numeric/opaque kinds later without changing the resolver interface.
//
// The resolver is separate from the loader parser: it maps a requirement to
// Resolved / Unresolved / UnsupportedIdentity / Ambiguous / Error, never
// collapsing them into one generic failure.
// ============================================================================

#ifndef P5EMU_LOADER_IMPORT_H
#define P5EMU_LOADER_IMPORT_H

#include <cstdint>
#include <string>
#include <vector>

#include "p5emu/core/Status.h"

namespace p5emu::loader {

// How a symbol is identified. `kUnknown` is a first-class, normal value.
enum class SymbolIdentityKind : std::uint8_t {
  kUnknown = 0,
  kStringName,
  kNumericId,
  kOpaqueId,
};

const char* SymbolIdentityKindName(SymbolIdentityKind kind) noexcept;

struct SymbolIdentity {
  SymbolIdentityKind kind = SymbolIdentityKind::kUnknown;
  std::string name;                  // kStringName
  std::uint64_t numeric_id = 0;      // kNumericId
  std::vector<std::uint8_t> opaque;  // kOpaqueId

  [[nodiscard]] static SymbolIdentity Unknown() { return SymbolIdentity{}; }
  [[nodiscard]] static SymbolIdentity String(std::string n) {
    SymbolIdentity s;
    s.kind = SymbolIdentityKind::kStringName;
    s.name = std::move(n);
    return s;
  }
  [[nodiscard]] static SymbolIdentity Numeric(std::uint64_t id) {
    SymbolIdentity s;
    s.kind = SymbolIdentityKind::kNumericId;
    s.numeric_id = id;
    return s;
  }
  [[nodiscard]] std::string Describe() const;
};

// One place where a requirement is used: a relocation target the resolver's
// address will be written to.
struct BindingSite {
  std::uint64_t target_guest_va = 0;
  std::uint32_t reloc_type = 0;  // numeric R_X86_64_*; kept numeric, no coupling
  std::uint64_t addend = 0;
};

// An immutable parsed requirement. `required` is true when something binds it
// (it has binding sites), which is what gates execution readiness.
struct ImportRequirement {
  std::string module;                    // library/module identity, may be empty
  SymbolIdentity symbol;                 // may be kUnknown
  std::uint32_t symbol_index = 0;        // generic dynsym index (0 = null)
  std::vector<BindingSite> binding_sites;
  bool required = true;

  [[nodiscard]] bool HasBindingSites() const noexcept {
    return !binding_sites.empty();
  }
};

// The outcome of a resolution attempt. Distinct states, never one failure.
enum class ResolutionStatus : std::uint8_t {
  kResolved = 0,
  kUnresolved,
  kUnsupportedIdentity,
  kAmbiguous,
  kError,
};

const char* ResolutionStatusName(ResolutionStatus status) noexcept;

struct ResolutionResult {
  ResolutionStatus status = ResolutionStatus::kUnresolved;
  std::uint64_t address = 0;  // valid only when kResolved
  std::string detail;

  [[nodiscard]] static ResolutionResult Resolved(std::uint64_t addr) {
    return ResolutionResult{ResolutionStatus::kResolved, addr, {}};
  }
  [[nodiscard]] static ResolutionResult Unresolved() {
    return ResolutionResult{ResolutionStatus::kUnresolved, 0, {}};
  }
};

class ImportResolver {
 public:
  virtual ~ImportResolver() = default;
  virtual ResolutionResult ResolveSymbol(const std::string& module,
                                         const SymbolIdentity& symbol) = 0;
};

// A registry-backed resolver, used by tests and by a future HLE layer. It is
// intentionally simple: register concrete (module, name) -> address mappings.
class RegistryResolver final : public ImportResolver {
 public:
  void AddStringSymbol(const std::string& module, const std::string& name,
                       std::uint64_t address);

  ResolutionResult ResolveSymbol(const std::string& module,
                                 const SymbolIdentity& symbol) override;

  [[nodiscard]] std::size_t EntryCount() const noexcept {
    return entries_.size();
  }

 private:
  struct Entry {
    std::string module;
    std::string name;
    std::uint64_t address = 0;
  };
  std::vector<Entry> entries_;
};

}  // namespace p5emu::loader

#endif  // P5EMU_LOADER_IMPORT_H
