// SPDX-License-Identifier: Apache-2.0
//
// P5EMU is an independent, open-source project. See LICENSE and
// PROVENANCE.md before adding code here.

#include "p5emu/loader/SceMetadata.h"

#include <cstdio>
#include <map>

namespace p5emu::loader {
namespace {

// A Sony/SCE tag whose NUMBER is evidenced. Its semantics are NOT: the payload
// is kUnknown and `semantics_known()` is false. Names are P5EMU's own labels for
// tag numbers learned from public research (see PROVENANCE.md).
struct SonyTagName {
  std::uint64_t tag;
  const char* name;
};

constexpr SonyTagName kSonyTags[] = {
    {kDtOsDynlibData, "DT_OS_DYNLIBDATA"},
    {kDtOsProcParam, "DT_OS_PROCPARAM"},
    {kDtOsFingerprint, "DT_OS_FINGERPRINT"},
    {kDtOsModuleInfo, "DT_OS_MODULE_INFO"},
    {kDtOsNeededModule, "DT_OS_NEEDED_MODULE"},
    {kDtOsRelro, "DT_OS_RELRO"},
    {kDtOsModuleAttr, "DT_OS_MODULE_ATTR"},
    {kDtOsExportLib, "DT_OS_EXPORT_LIB"},
    {kDtOsImportLib, "DT_OS_IMPORT_LIB"},
    {kDtOsHash, "DT_OS_HASH"},
    {kDtOsPltGot, "DT_OS_PLTGOT"},
    {kDtOsJmprel, "DT_OS_JMPREL"},
    {kDtOsRela, "DT_OS_RELA"},
    {kDtOsStrtab, "DT_OS_STRTAB"},
    {kDtOsSymtab, "DT_OS_SYMTAB"},
    {kDtOsHashsz, "DT_OS_HASHSZ"},
    {kDtOsSymtabsz, "DT_OS_SYMTABSZ"},
};

const char* SonyTagNameFor(std::uint64_t tag) noexcept {
  for (const SonyTagName& t : kSonyTags) {
    if (t.tag == tag) return t.name;
  }
  return nullptr;
}

SceTagInfo Known(std::uint64_t tag, const char* name, SceTagPayload p) noexcept {
  SceTagInfo i;
  i.tag = tag;
  i.name = name;
  i.knowledge = SceTagKnowledge::kKnownSemantics;
  i.payload = p;
  return i;
}

}  // namespace

const char* SceTagPayloadName(SceTagPayload p) noexcept {
  switch (p) {
    case SceTagPayload::kUnknown: return "unknown";
    case SceTagPayload::kAddress: return "address";
    case SceTagPayload::kSize: return "size";
    case SceTagPayload::kIndex: return "index";
    case SceTagPayload::kEncodedIdentifier: return "encoded-identifier";
    case SceTagPayload::kTableReference: return "table-reference";
  }
  return "unknown";
}

const char* SceTagKnowledgeName(SceTagKnowledge k) noexcept {
  switch (k) {
    case SceTagKnowledge::kUnrecognized: return "unrecognized";
    case SceTagKnowledge::kNumberAndName: return "number-and-name";
    case SceTagKnowledge::kKnownSemantics: return "known-semantics";
  }
  return "unrecognized";
}

SceTagInfo LookupSceTag(std::uint64_t tag) noexcept {
  // System V generic tags: semantics are evidenced by the ELF/AMD64 psABI.
  switch (tag) {
    case static_cast<std::uint64_t>(kDtNull):
      return Known(tag, "DT_NULL", SceTagPayload::kUnknown);
    case static_cast<std::uint64_t>(kDtNeeded):
      return Known(tag, "DT_NEEDED", SceTagPayload::kIndex);
    case static_cast<std::uint64_t>(kDtPltrelsz):
      return Known(tag, "DT_PLTRELSZ", SceTagPayload::kSize);
    case static_cast<std::uint64_t>(kDtPltgot):
      return Known(tag, "DT_PLTGOT", SceTagPayload::kAddress);
    case static_cast<std::uint64_t>(kDtHash):
      return Known(tag, "DT_HASH", SceTagPayload::kAddress);
    case static_cast<std::uint64_t>(kDtStrtab):
      return Known(tag, "DT_STRTAB", SceTagPayload::kAddress);
    case static_cast<std::uint64_t>(kDtSymtab):
      return Known(tag, "DT_SYMTAB", SceTagPayload::kAddress);
    case static_cast<std::uint64_t>(kDtRela):
      return Known(tag, "DT_RELA", SceTagPayload::kAddress);
    case static_cast<std::uint64_t>(kDtRelasz):
      return Known(tag, "DT_RELASZ", SceTagPayload::kSize);
    case static_cast<std::uint64_t>(kDtRelaent):
      return Known(tag, "DT_RELAENT", SceTagPayload::kSize);
    case static_cast<std::uint64_t>(kDtStrsz):
      return Known(tag, "DT_STRSZ", SceTagPayload::kSize);
    case static_cast<std::uint64_t>(kDtSyment):
      return Known(tag, "DT_SYMENT", SceTagPayload::kSize);
    case static_cast<std::uint64_t>(kDtInit):
      return Known(tag, "DT_INIT", SceTagPayload::kAddress);
    case static_cast<std::uint64_t>(kDtFini):
      return Known(tag, "DT_FINI", SceTagPayload::kAddress);
    case static_cast<std::uint64_t>(kDtRel):
      return Known(tag, "DT_REL", SceTagPayload::kAddress);
    case static_cast<std::uint64_t>(kDtRelsz):
      return Known(tag, "DT_RELSZ", SceTagPayload::kSize);
    case static_cast<std::uint64_t>(kDtRelent):
      return Known(tag, "DT_RELENT", SceTagPayload::kSize);
    case static_cast<std::uint64_t>(kDtPltrel):
      return Known(tag, "DT_PLTREL", SceTagPayload::kEncodedIdentifier);
    case static_cast<std::uint64_t>(kDtJmprel):
      return Known(tag, "DT_JMPREL", SceTagPayload::kAddress);
    case static_cast<std::uint64_t>(kDtFlags1):
      return Known(tag, "DT_FLAGS_1", SceTagPayload::kEncodedIdentifier);
    default:
      break;
  }

  // Recognized Sony/SCE tag numbers: name evidenced, semantics UNKNOWN.
  if (const char* name = SonyTagNameFor(tag); name != nullptr) {
    SceTagInfo i;
    i.tag = tag;
    i.name = name;
    i.knowledge = SceTagKnowledge::kNumberAndName;
    i.payload = SceTagPayload::kUnknown;  // semantics NOT established
    return i;
  }
  if (tag >= kPs5VariantTagBase && tag < kPs5TagRangeEnd) {
    // In the PS5-era "_1" range: recognized as a Sony-namespace number, but no
    // evidenced name and no semantics.
    SceTagInfo i;
    i.tag = tag;
    i.name = "DT_OS_VARIANT";
    i.knowledge = SceTagKnowledge::kNumberAndName;
    i.payload = SceTagPayload::kUnknown;
    return i;
  }

  SceTagInfo unknown;
  unknown.tag = tag;
  unknown.name = nullptr;
  unknown.knowledge = SceTagKnowledge::kUnrecognized;
  unknown.payload = SceTagPayload::kUnknown;
  return unknown;
}

SceModuleMetadata BuildSceModuleMetadata(const Ps5ExecutableImage& image) {
  SceModuleMetadata md;
  md.is_ps5_dialect = image.is_ps5_dialect;

  for (const DynamicEntry& e : image.dynamic_raw) {
    const std::uint64_t tag = static_cast<std::uint64_t>(e.tag);
    SceDynamicEntry out;
    out.info = LookupSceTag(tag);
    out.value = e.value;
    out.is_ps5_variant = (tag >= kPs5VariantTagBase && tag < kPs5TagRangeEnd);

    switch (out.info.knowledge) {
      case SceTagKnowledge::kKnownSemantics:
        md.generic_tags.push_back(out);
        break;
      case SceTagKnowledge::kNumberAndName:
        // Recognized by number, but NOT understood. Does not affect behavior.
        md.sce_tags.push_back(out);
        ++md.unknown_semantics_count;
        break;
      case SceTagKnowledge::kUnrecognized:
      default:
        md.unrecognized.push_back(out);
        break;
    }
  }

  md.needed_modules.reserve(image.needed_modules.size());
  for (const ImportModule& m : image.needed_modules) {
    md.needed_modules.push_back(m.name);
  }
  md.declares_sce_imports = image.declares_ps5_imports;
  md.declares_sce_exports = image.declares_ps5_exports;
  return md;
}

Status ValidateSceMetadata(const SceModuleMetadata& md,
                           std::vector<std::string>* blockers) {
  // Conservative, SYNTHETIC policy (not a PS5 claim): a recognized tag that
  // appears more than once with DIFFERING values is contradictory.
  std::map<std::uint64_t, std::uint64_t> first_value;
  bool conflict = false;
  for (const SceDynamicEntry& e : md.sce_tags) {
    auto it = first_value.find(e.info.tag);
    if (it == first_value.end()) {
      first_value.emplace(e.info.tag, e.value);
    } else if (it->second != e.value) {
      conflict = true;
      if (blockers != nullptr) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "conflicting-sce-tag:0x%llx",
                      static_cast<unsigned long long>(e.info.tag));
        blockers->push_back(buf);
      }
    }
  }
  return conflict ? StatusCode::kConflictingMetadata : StatusCode::kOk;
}

SceNid SceNid::FromOpaque(std::uint64_t value) noexcept {
  SceNid n;
  n.known_ = true;
  n.value_ = value;
  return n;
}

std::string SceNid::ToHex() const {
  if (!known_) return "<unknown>";
  char buf[32];
  std::snprintf(buf, sizeof(buf), "0x%016llx",
                static_cast<unsigned long long>(value_));
  return buf;
}

SymbolIdentity SymbolIdentityFromNid(const SceNid& nid) {
  if (!nid.has_value()) return SymbolIdentity::Unknown();
  return SymbolIdentity::Numeric(nid.value());
}

}  // namespace p5emu::loader
