// SPDX-License-Identifier: Apache-2.0
//
// P5EMU is an independent, open-source project. See LICENSE and
// PROVENANCE.md before adding code here.

#include "p5emu/loader/Ps5Executable.h"

#include <optional>

namespace p5emu::loader {
// Maps a guest virtual address to a file offset using the PT_LOAD segments.
std::optional<std::uint64_t> GuestVaToFileOffset(const Ps5ExecutableImage& img,
                                                 std::uint64_t va) {
  for (const Elf64ProgramHeader& ph : img.load_segments) {
    if (va >= ph.vaddr && (va - ph.vaddr) < ph.memsz) {
      const std::uint64_t delta = va - ph.vaddr;
      if (BinaryReader::AddOverflows(ph.offset, delta)) {
        return std::nullopt;
      }
      return ph.offset + delta;
    }
  }
  return std::nullopt;
}

namespace {

constexpr std::uint64_t kDynamicEntrySize = 16;
constexpr std::uint64_t kMaxDynamicEntries = 4096;
constexpr std::uint64_t kMaxStringLength = 4096;

// Reads a NUL-terminated string at `va`, bounded by `limit` bytes.
Status ReadGuestString(const BinaryReader& reader, const Ps5ExecutableImage& img,
                       std::uint64_t va, std::uint64_t limit,
                       std::string* out) {
  out->clear();
  const std::optional<std::uint64_t> off = GuestVaToFileOffset(img, va);
  if (!off.has_value()) {
    return StatusCode::kMalformedDynamicTable;
  }
  std::uint64_t max_len = limit;
  if (max_len > kMaxStringLength) {
    max_len = kMaxStringLength;
  }
  for (std::uint64_t i = 0; i < max_len; ++i) {
    std::uint8_t c = 0;
    const Status st = reader.U8(off.value() + i, &c);
    if (!st.ok()) {
      return StatusCode::kMalformedDynamicTable;
    }
    if (c == 0) {
      return StatusCode::kOk;
    }
    out->push_back(static_cast<char>(c));
  }
  // No terminator inside the bound.
  return StatusCode::kMalformedDynamicTable;
}

bool IsRecognizedPs5Tag(std::uint64_t tag) {
  switch (tag) {
    case kDtOsDynlibData:
    case kDtOsProcParam:
    case kDtOsFingerprint:
    case kDtOsModuleInfo:
    case kDtOsNeededModule:
    case kDtOsRelro:
    case kDtOsModuleAttr:
    case kDtOsExportLib:
    case kDtOsImportLib:
    case kDtOsHash:
    case kDtOsPltGot:
    case kDtOsJmprel:
    case kDtOsRela:
    case kDtOsStrtab:
    case kDtOsSymtab:
    case kDtOsHashsz:
    case kDtOsSymtabsz:
      return true;
    default:
      return false;
  }
}

}  // namespace

Status ParsePs5Executable(const BinaryReader& reader,
                          const GenericElfImage& elf,
                          Ps5ExecutableImage* out) {
  if (out == nullptr) {
    return StatusCode::kInvalidArgument;
  }
  *out = Ps5ExecutableImage{};

  out->osabi = elf.osabi;
  out->e_type = elf.type;
  out->machine = elf.machine;
  out->entry = elf.entry;
  out->big_endian = elf.big_endian;

  if (elf.machine != kElfEmX86_64) {
    return StatusCode::kUnsupportedMachine;
  }
  out->is_ps5_dialect = (elf.type == kPs5EtDynExec || elf.type == kPs5EtDynamic ||
                         elf.osabi == kElfOsabiFreeBsd);

  for (const Elf64ProgramHeader& ph : elf.program_headers) {
    if (ph.type == kPtLoad) {
      out->load_segments.push_back(ph);
    }
  }

  // Locate the dynamic table via PT_DYNAMIC (generic). PS5-specific program
  // header types have no published numeric values in our research, so they are
  // not matched here rather than guessed.
  const Elf64ProgramHeader* dyn = nullptr;
  for (const Elf64ProgramHeader& ph : elf.program_headers) {
    if (ph.type == kPtDynamic) {
      dyn = &ph;
      break;
    }
  }
  if (dyn == nullptr) {
    return StatusCode::kOk;
  }
  out->has_dynamic = true;

  if (dyn->filesz < kDynamicEntrySize) {
    return StatusCode::kMalformedDynamicTable;
  }
  const std::uint64_t max_entries = dyn->filesz / kDynamicEntrySize;
  if (max_entries > kMaxDynamicEntries) {
    return StatusCode::kTooManyEntries;
  }

  // Parse raw entries until DT_NULL. A missing terminator is malformed.
  bool terminated = false;
  for (std::uint64_t i = 0; i < max_entries; ++i) {
    const std::uint64_t off = dyn->offset + i * kDynamicEntrySize;
    std::int64_t tag = 0;
    std::uint64_t value = 0;
    const Status st1 = reader.S64(off, elf.big_endian, &tag);
    const Status st2 = reader.U64(off + 8, elf.big_endian, &value);
    if (!st1.ok() || !st2.ok()) {
      return StatusCode::kTruncatedInput;
    }
    if (tag == kDtNull) {
      terminated = true;
      break;
    }
    out->dynamic_raw.push_back(DynamicEntry{tag, value});
  }
  if (!terminated) {
    return StatusCode::kMalformedDynamicTable;
  }

  // First pass: string table location.
  for (const DynamicEntry& e : out->dynamic_raw) {
    if (e.tag == kDtStrtab) {
      out->strtab_addr = e.value;
    } else if (e.tag == kDtStrsz) {
      out->strtab_size = e.value;
    } else if (e.tag == kDtRela) {
      out->rela_addr = e.value;
    } else if (e.tag == kDtRelasz) {
      out->rela_size = e.value;
    } else if (e.tag == kDtRelaent) {
      out->rela_ent = e.value;
    } else if (e.tag == kDtJmprel) {
      out->jmprel_addr = e.value;
    } else if (e.tag == kDtPltrelsz) {
      out->pltrel_size = e.value;
    } else if (e.tag == kDtPltrel) {
      out->pltrel_type = e.value;
    } else if (e.tag == kDtSymtab) {
      out->symtab_addr = e.value;
    } else if (e.tag == kDtSyment) {
      out->syment = e.value;
    }
  }

  // Second pass: interpret what is safe to interpret; preserve the rest.
  for (const DynamicEntry& e : out->dynamic_raw) {
    const std::uint64_t utag = static_cast<std::uint64_t>(e.tag);
    if (e.tag == kDtNeeded) {
      ImportModule m;
      m.tag = e.tag;
      m.resolved = false;
      if (out->strtab_addr.has_value() && out->strtab_size.has_value()) {
        const Status st = ReadGuestString(
            reader, *out, out->strtab_addr.value() + e.value,
            out->strtab_size.value(), &m.name);
        if (!st.ok()) {
          return st;
        }
      }
      out->needed_modules.push_back(std::move(m));
    } else if (utag >= kPs5VariantTagBase && utag < kPs5TagRangeEnd) {
      // PS5-era "_1" variants: recognized range, UNKNOWN semantics.
      out->ps5_tags.push_back(RecognizedPs5Tag{utag, e.value, true});
    } else if (IsRecognizedPs5Tag(utag)) {
      // Recognized tag number; the defining structure is not documented, so the
      // value is PRESERVED, never interpreted.
      out->ps5_tags.push_back(RecognizedPs5Tag{utag, e.value, false});
      if (utag == kDtOsImportLib) {
        out->declares_ps5_imports = true;
      } else if (utag == kDtOsExportLib) {
        out->declares_ps5_exports = true;
      }
    } else {
      // Generic tag we understand, or a tag we do not: keep the raw entry.
      switch (e.tag) {
        case kDtStrtab:
        case kDtStrsz:
        case kDtPltrelsz:
        case kDtPltgot:
        case kDtHash:
        case kDtSymtab:
        case kDtRela:
        case kDtRelasz:
        case kDtRelaent:
        case kDtSyment:
        case kDtInit:
        case kDtFini:
        case kDtRel:
        case kDtRelsz:
        case kDtRelent:
        case kDtPltrel:
        case kDtJmprel:
        case kDtFlags1:
          break;  // known generic tag, nothing more to interpret here
        default:
          out->unknown_tags.push_back(e);
          break;
      }
    }
  }
  return StatusCode::kOk;
}

}  // namespace p5emu::loader
