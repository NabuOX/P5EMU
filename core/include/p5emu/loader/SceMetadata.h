// SPDX-License-Identifier: Apache-2.0
//
// P5EMU is an independent, open-source project. See LICENSE and
// PROVENANCE.md before adding code here.
//
// ============================================================================
// TYPED SCE / SONY DYNAMIC METADATA (Phase 9)
// ============================================================================
//
// Phase 4 recognized a set of Sony `DT_OS_*` dynamic tag NUMBERS and preserved
// their values without interpreting them. Phase 9 adds a TYPED boundary that
// keeps two questions strictly separate:
//
//   * Is the tag NUMBER recognized?      (evidence: the tag exists)
//   * Are the tag's SEMANTICS understood? (evidence: a defining structure)
//
// Recognizing a number is NOT support for its behaviour. A tag whose defining
// structure is not evidenced stays `SceTagKnowledge::kNumberAndName` with
// `SceTagPayload::kUnknown`, and no API here will turn it into meaning.
//
// NID note: this file provides NID REPRESENTATION (hold + compare an evidenced
// opaque identifier). It deliberately provides NO NID generation algorithm.
// P5EMU does not know how Sony generates NIDs and does not guess.
//
// See docs/research/ps5-dynamic-linking-phase9.md for the per-item ledger.
// ============================================================================

#ifndef P5EMU_LOADER_SCE_METADATA_H
#define P5EMU_LOADER_SCE_METADATA_H

#include <cstdint>
#include <string>
#include <vector>

#include "p5emu/core/Status.h"
#include "p5emu/loader/Import.h"
#include "p5emu/loader/Ps5Executable.h"

namespace p5emu::loader {

// ---------------------------------------------------------------------------
// Tag knowledge: number vs semantics
// ---------------------------------------------------------------------------
// The payload kind of a dynamic tag, when it is understood. `kUnknown` means the
// defining structure is not evidenced; the value is preserved but must not be
// interpreted.
enum class SceTagPayload : std::uint8_t {
  kUnknown = 0,
  kAddress,
  kSize,
  kIndex,
  kEncodedIdentifier,
  kTableReference,
};
const char* SceTagPayloadName(SceTagPayload p) noexcept;

// How much is known about a tag.
enum class SceTagKnowledge : std::uint8_t {
  kUnrecognized = 0,  // the tag NUMBER is not recognized
  kNumberAndName = 1, // the number and an evidenced name are known; semantics UNKNOWN
  kKnownSemantics = 2,// the payload semantics are evidenced (System V generic)
};
const char* SceTagKnowledgeName(SceTagKnowledge k) noexcept;

struct SceTagInfo {
  std::uint64_t tag = 0;
  const char* name = nullptr;  // evidenced name; nullptr when unrecognized
  SceTagKnowledge knowledge = SceTagKnowledge::kUnrecognized;
  SceTagPayload payload = SceTagPayload::kUnknown;

  // True only when the payload semantics are evidenced. Callers MUST gate any
  // interpretation of `value` on this.
  [[nodiscard]] bool semantics_known() const noexcept {
    return knowledge == SceTagKnowledge::kKnownSemantics;
  }
};

// Classifies a dynamic tag NUMBER. Recognized Sony tags return kNumberAndName
// with payload kUnknown: the name is evidenced, the semantics are not. Generic
// System V tags return kKnownSemantics with a classified payload. Never invents
// meaning from a name.
SceTagInfo LookupSceTag(std::uint64_t tag) noexcept;

// ---------------------------------------------------------------------------
// Typed dynamic entries and module metadata
// ---------------------------------------------------------------------------
// One preserved dynamic entry with its classification. `value` is always the
// raw value from the file.
struct SceDynamicEntry {
  SceTagInfo info;
  std::uint64_t value = 0;
  bool is_ps5_variant = false;  // in the PS5-era "_1" range (semantics UNKNOWN)
};

// The typed dynamic-table metadata for one module. Deterministic: entries are
// bucketed by knowledge, preserving input order within each bucket.
struct SceModuleMetadata {
  bool is_ps5_dialect = false;
  std::vector<SceDynamicEntry> sce_tags;      // recognized Sony/SCE numbers
  std::vector<SceDynamicEntry> generic_tags;  // System V tags (semantics known)
  std::vector<SceDynamicEntry> unrecognized;  // neither: preserved raw
  std::vector<std::string> needed_modules;    // generic DT_NEEDED names (facts)
  bool declares_sce_imports = false;          // DT_OS_IMPORT_LIB present (fact)
  bool declares_sce_exports = false;          // DT_OS_EXPORT_LIB present (fact)
  std::size_t unknown_semantics_count = 0;    // recognized but NOT understood

  [[nodiscard]] bool has_unknown_semantics() const noexcept {
    return unknown_semantics_count != 0;
  }
};

// Builds the typed metadata from an already-parsed executable image. This does
// NOT consume raw file bytes: parsing (bounds/overflow-checked) happened in the
// Phase-4 layer. It never interprets an unknown-semantics tag.
SceModuleMetadata BuildSceModuleMetadata(const Ps5ExecutableImage& image);

// Structural validation of the typed metadata. Conservative P5EMU policy: a
// recognized tag that appears more than once WITH DIFFERING VALUES is reported
// as conflicting (a synthetic policy, not a PS5 claim), naming the tag numbers
// in `blockers`. Identical duplicates are tolerated. Returns kConflictingMetadata
// when a conflict exists, else kOk.
Status ValidateSceMetadata(const SceModuleMetadata& md,
                           std::vector<std::string>* blockers);

// ---------------------------------------------------------------------------
// NID representation (NOT generation)
// ---------------------------------------------------------------------------
// An evidenced Sony identifier (NID) preserved as an opaque value and made
// comparable. There is intentionally no `ComputeNid(name)` here: P5EMU has no
// NID generation algorithm and does not invent one.
class SceNid {
 public:
  SceNid() = default;

  // Preserves an evidenced identifier value. This is representation only.
  [[nodiscard]] static SceNid FromOpaque(std::uint64_t value) noexcept;

  [[nodiscard]] bool has_value() const noexcept { return known_; }
  [[nodiscard]] std::uint64_t value() const noexcept { return value_; }
  [[nodiscard]] std::string ToHex() const;

  friend bool operator==(const SceNid& a, const SceNid& b) noexcept {
    return a.known_ == b.known_ && a.value_ == b.value_;
  }
  friend bool operator!=(const SceNid& a, const SceNid& b) noexcept {
    return !(a == b);
  }

 private:
  bool known_ = false;
  std::uint64_t value_ = 0;
};

// Converts an evidenced NID into the loader's typed SymbolIdentity. An evidenced
// NID becomes `NumericId`; an unknown NID becomes `Unknown`. It never becomes a
// string name.
SymbolIdentity SymbolIdentityFromNid(const SceNid& nid);

// ---------------------------------------------------------------------------
// Module / library identity
// ---------------------------------------------------------------------------
// A library or module identifier. Preserved as an opaque identity; P5EMU never
// turns an opaque numeric id into a human-readable name by guessing.
struct SceLibraryIdentity {
  SymbolIdentity opaque_id;  // typically NumericId or OpaqueId, or Unknown
  bool evidenced = false;    // false => nothing about this id is established

  [[nodiscard]] static SceLibraryIdentity FromOpaque(std::uint64_t id) {
    SceLibraryIdentity s;
    s.opaque_id = SymbolIdentity::Numeric(id);
    s.evidenced = true;
    return s;
  }
  [[nodiscard]] static SceLibraryIdentity Unknown() { return SceLibraryIdentity{}; }
};

}  // namespace p5emu::loader

#endif  // P5EMU_LOADER_SCE_METADATA_H
