// SPDX-License-Identifier: Apache-2.0
//
// P5EMU is an independent, open-source project. See LICENSE and
// PROVENANCE.md before adding code here.

#include "p5emu/loader/Reloc.h"

#include <optional>

namespace p5emu::loader {

const char* RelocTypeName(RelocType type) noexcept {
  switch (type) {
    case RelocType::kNone: return "R_X86_64_NONE";
    case RelocType::k64: return "R_X86_64_64";
    case RelocType::kGlobDat: return "R_X86_64_GLOB_DAT";
    case RelocType::kJumpSlot: return "R_X86_64_JUMP_SLOT";
    case RelocType::kRelative: return "R_X86_64_RELATIVE";
    case RelocType::kUnsupported: return "unsupported";
  }
  return "unsupported";
}

RelocType RelocTypeFromNumber(std::uint32_t number) noexcept {
  switch (number) {
    case 0: return RelocType::kNone;
    case 1: return RelocType::k64;
    case 6: return RelocType::kGlobDat;
    case 7: return RelocType::kJumpSlot;
    case 8: return RelocType::kRelative;
    default: return RelocType::kUnsupported;
  }
}

namespace {

bool TypeNeedsSymbolLocal(RelocType type) noexcept {
  return type == RelocType::k64 || type == RelocType::kGlobDat ||
         type == RelocType::kJumpSlot;
}

// Reads one Elf64_Sym and resolves its name through the dynamic string table.
bool ReadSymbol(const BinaryReader& reader, const Ps5ExecutableImage& image,
                std::uint64_t sym_file_off, std::uint64_t index,
                ElfSymbol* out) {
  const std::uint64_t off = sym_file_off + index * kSymEntrySize;
  std::uint32_t st_name = 0;
  std::uint8_t st_info = 0, st_other = 0;
  std::uint16_t st_shndx = 0;
  std::uint64_t st_value = 0, st_size = 0;
  if (!reader.U32(off + 0, image.big_endian, &st_name).ok() ||
      !reader.U8(off + 4, &st_info).ok() ||
      !reader.U8(off + 5, &st_other).ok() ||
      !reader.U16(off + 6, image.big_endian, &st_shndx).ok() ||
      !reader.U64(off + 8, image.big_endian, &st_value).ok() ||
      !reader.U64(off + 16, image.big_endian, &st_size).ok()) {
    return false;
  }
  (void)st_info;
  (void)st_other;
  (void)st_size;
  out->name.clear();
  out->elf_value = st_value;
  out->shndx = st_shndx;
  if (st_name != 0 && image.strtab_addr.has_value() &&
      image.strtab_size.has_value() && st_name < image.strtab_size.value()) {
    const std::uint64_t name_va = image.strtab_addr.value() + st_name;
    const std::optional<std::uint64_t> noff =
        GuestVaToFileOffset(image, name_va);
    if (noff.has_value()) {
      std::uint64_t remaining = image.strtab_size.value() - st_name;
      if (remaining > 4096) remaining = 4096;
      for (std::uint64_t i = 0; i < remaining; ++i) {
        std::uint8_t c = 0;
        if (!reader.U8(noff.value() + i, &c).ok()) break;
        if (c == 0) break;
        out->name.push_back(static_cast<char>(c));
      }
    }
  }
  return true;
}

}  // namespace

Status ParseDynamicRelocations(const BinaryReader& reader,
                               const Ps5ExecutableImage& image,
                               std::uint64_t load_bias,
                               RelocationSet* out) {
  if (out == nullptr) {
    return StatusCode::kInvalidArgument;
  }
  *out = RelocationSet{};
  if (!image.has_dynamic) {
    return StatusCode::kOk;
  }

  // Phase 5 supports RELA only. DT_PLTREL naming REL (or anything else) is
  // rejected rather than misread as RELA.
  if (image.pltrel_type.has_value() && image.pltrel_type.value() != 7u) {
    return StatusCode::kUnsupportedRelocation;
  }

  // --- RELA tables (parsed first; the symbol set derives from these) ------
  const auto parse_table = [&](std::uint64_t addr, std::uint64_t size,
                               std::uint64_t ent) -> Status {
    if (ent != kRelaEntrySize) {
      return StatusCode::kMalformedRelocationTable;
    }
    if (size % kRelaEntrySize != 0) {
      return StatusCode::kMalformedRelocationTable;
    }
    const std::uint64_t count = size / kRelaEntrySize;
    if (count > kMaxRelocations) {
      return StatusCode::kTooManyEntries;
    }
    const std::optional<std::uint64_t> tbl = GuestVaToFileOffset(image, addr);
    if (!tbl.has_value()) {
      return StatusCode::kMalformedRelocationTable;
    }
    for (std::uint64_t i = 0; i < count; ++i) {
      const std::uint64_t off = tbl.value() + i * kRelaEntrySize;
      std::uint64_t r_offset = 0, r_info = 0;
      std::int64_t r_addend = 0;
      if (!reader.U64(off + 0, image.big_endian, &r_offset).ok() ||
          !reader.U64(off + 8, image.big_endian, &r_info).ok() ||
          !reader.S64(off + 16, image.big_endian, &r_addend).ok()) {
        return StatusCode::kTruncatedInput;
      }
      const std::uint32_t type_num =
          static_cast<std::uint32_t>(r_info & 0xFFFFFFFFu);
      const std::uint32_t sym_index = static_cast<std::uint32_t>(r_info >> 32);
      if (BinaryReader::AddOverflows(r_offset, load_bias)) {
        return StatusCode::kRelocationOverflow;
      }
      Relocation r;
      r.target_guest_va = r_offset + load_bias;
      r.type = RelocTypeFromNumber(type_num);
      r.symbol_index = sym_index;
      r.addend = static_cast<std::uint64_t>(r_addend);
      r.width = 8;
      out->relocations.push_back(r);
    }
    return StatusCode::kOk;
  };

  if (image.rela_addr.has_value()) {
    const Status st = parse_table(image.rela_addr.value(),
                                  image.rela_size.value_or(0),
                                  image.rela_ent.value_or(kRelaEntrySize));
    if (!st.ok()) return st;
  }
  if (image.jmprel_addr.has_value()) {
    const Status st = parse_table(image.jmprel_addr.value(),
                                  image.pltrel_size.value_or(0), kRelaEntrySize);
    if (!st.ok()) return st;
  }
  out->present = !out->relocations.empty();

  // --- Symbol table: only the indices a relocation actually references -----
  // A generic ELF64 without DT_SYMTABSZ gives no symbol count; reading to the
  // end of the segment would over-read garbage. Parse exactly [0, max_index].
  if (image.symtab_addr.has_value()) {
    if (!image.syment.has_value() || image.syment.value() != kSymEntrySize) {
      return StatusCode::kMalformedRelocationTable;
    }
    const std::optional<std::uint64_t> sym_off =
        GuestVaToFileOffset(image, image.symtab_addr.value());
    if (!sym_off.has_value()) {
      return StatusCode::kMalformedRelocationTable;
    }
    std::uint32_t max_sym = 0;
    bool any = false;
    for (const Relocation& r : out->relocations) {
      if (TypeNeedsSymbolLocal(r.type)) {
        max_sym = (r.symbol_index > max_sym) ? r.symbol_index : max_sym;
        any = true;
      }
    }
    const std::uint64_t count = any ? (static_cast<std::uint64_t>(max_sym) + 1u)
                                    : 1u;  // at least the null symbol
    if (count > kMaxSymbols) {
      return StatusCode::kTooManyEntries;
    }
    out->symbols.resize(static_cast<std::size_t>(count));
    for (std::uint64_t i = 0; i < count; ++i) {
      if (!ReadSymbol(reader, image, sym_off.value(), i, &out->symbols[i])) {
        return StatusCode::kMalformedRelocationTable;
      }
    }
  }
  return StatusCode::kOk;
}

Status ComputeRelocationValue(const Relocation& reloc, std::uint64_t load_bias,
                              std::uint64_t symbol_address,
                              std::uint64_t* value_out) {
  if (value_out == nullptr) {
    return StatusCode::kInvalidArgument;
  }
  switch (reloc.type) {
    case RelocType::kNone:
      *value_out = 0;
      return StatusCode::kOk;
    case RelocType::kRelative: {
      if (BinaryReader::AddOverflows(load_bias, reloc.addend)) {
        return StatusCode::kRelocationOverflow;
      }
      *value_out = load_bias + reloc.addend;
      return StatusCode::kOk;
    }
    case RelocType::k64: {
      if (BinaryReader::AddOverflows(symbol_address, reloc.addend)) {
        return StatusCode::kRelocationOverflow;
      }
      *value_out = symbol_address + reloc.addend;
      return StatusCode::kOk;
    }
    case RelocType::kGlobDat:
    case RelocType::kJumpSlot:
      *value_out = symbol_address;
      return StatusCode::kOk;
    case RelocType::kUnsupported:
    default:
      return StatusCode::kUnsupportedRelocation;
  }
}

}  // namespace p5emu::loader
