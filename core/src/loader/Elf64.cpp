// SPDX-License-Identifier: LicenseRef-P5EMU-Proprietary
//
// P5EMU source is independently authored and proprietary. See LICENSE.md and
// PROVENANCE.md before adding code here.

#include "p5emu/loader/Elf64.h"

namespace p5emu::loader {
namespace {

bool IsPowerOfTwo(std::uint64_t v) noexcept {
  return v != 0 && (v & (v - 1)) == 0;
}

}  // namespace

Status ReadGenericElf64(const BinaryReader& reader, GenericElfImage* out) {
  if (out == nullptr) {
    return StatusCode::kInvalidArgument;
  }
  *out = GenericElfImage{};

  if (reader.size() < kElf64HeaderSize) {
    return StatusCode::kTruncatedInput;
  }

  // e_ident
  std::uint8_t magic[4] = {0, 0, 0, 0};
  for (std::uint64_t i = 0; i < 4; ++i) {
    const Status st = reader.U8(i, &magic[i]);
    if (!st.ok()) return st;
  }
  if (magic[0] != 0x7F || magic[1] != 'E' || magic[2] != 'L' || magic[3] != 'F') {
    return StatusCode::kInvalidMagic;
  }

  std::uint8_t cls = 0;
  std::uint8_t data = 0;
  std::uint8_t ident_version = 0;
  std::uint8_t osabi = 0;
  std::uint8_t abiversion = 0;
  if (!reader.U8(4, &cls).ok() || !reader.U8(5, &data).ok() ||
      !reader.U8(6, &ident_version).ok() || !reader.U8(7, &osabi).ok() ||
      !reader.U8(8, &abiversion).ok()) {
    return StatusCode::kTruncatedInput;
  }

  if (cls != kElfClass64) {
    return StatusCode::kUnsupportedClass;
  }
  if (data != kElfDataLsb && data != kElfDataMsb) {
    return StatusCode::kUnsupportedEndian;
  }
  const bool big_endian = (data == kElfDataMsb);
  if (ident_version != kElfIdentVersion) {
    return StatusCode::kMalformedExecutable;
  }

  out->elf_class = cls;
  out->data_encoding = data;
  out->osabi = osabi;
  out->abi_version = abiversion;
  out->big_endian = big_endian;

  // 64-bit header fields.
  std::uint16_t e_type = 0, e_machine = 0;
  std::uint32_t e_version = 0;
  std::uint64_t e_entry = 0, e_phoff = 0;
  std::uint32_t e_flags = 0;
  std::uint16_t e_ehsize = 0, e_phentsize = 0, e_phnum = 0;
  if (!reader.U16(16, big_endian, &e_type).ok() ||
      !reader.U16(18, big_endian, &e_machine).ok() ||
      !reader.U32(20, big_endian, &e_version).ok() ||
      !reader.U64(24, big_endian, &e_entry).ok() ||
      !reader.U64(32, big_endian, &e_phoff).ok() ||
      !reader.U32(48, big_endian, &e_flags).ok() ||
      !reader.U16(52, big_endian, &e_ehsize).ok() ||
      !reader.U16(54, big_endian, &e_phentsize).ok() ||
      !reader.U16(56, big_endian, &e_phnum).ok()) {
    return StatusCode::kTruncatedInput;
  }

  if (e_version != kElfEvCurrent) {
    return StatusCode::kMalformedExecutable;
  }
  out->type = e_type;
  out->machine = e_machine;
  out->version = e_version;
  out->entry = e_entry;
  out->flags = e_flags;

  if (e_phnum == 0) {
    return StatusCode::kOk;  // no program headers: valid, nothing to load
  }
  if (e_phnum > kMaxProgramHeaders) {
    return StatusCode::kTooManyEntries;
  }
  if (e_phentsize != kElf64PhdrSize) {
    return StatusCode::kInvalidProgramHeader;
  }

  const std::uint64_t table_bytes =
      static_cast<std::uint64_t>(e_phentsize) * e_phnum;
  if (BinaryReader::MulOverflows(e_phentsize, e_phnum)) {
    return StatusCode::kIntegerOverflow;
  }
  {
    const Status st = reader.CheckRange(e_phoff, table_bytes);
    if (!st.ok()) {
      return st;
    }
  }

  out->program_headers.reserve(e_phnum);
  for (std::uint32_t i = 0; i < e_phnum; ++i) {
    const std::uint64_t off =
        e_phoff + static_cast<std::uint64_t>(i) * e_phentsize;
    Elf64ProgramHeader ph;
    if (!reader.U32(off + 0, big_endian, &ph.type).ok() ||
        !reader.U32(off + 4, big_endian, &ph.flags).ok() ||
        !reader.U64(off + 8, big_endian, &ph.offset).ok() ||
        !reader.U64(off + 16, big_endian, &ph.vaddr).ok() ||
        !reader.U64(off + 24, big_endian, &ph.paddr).ok() ||
        !reader.U64(off + 32, big_endian, &ph.filesz).ok() ||
        !reader.U64(off + 40, big_endian, &ph.memsz).ok() ||
        !reader.U64(off + 48, big_endian, &ph.align).ok()) {
      return StatusCode::kTruncatedInput;
    }
    out->program_headers.push_back(ph);
  }
  return StatusCode::kOk;
}

Status ValidateGenericLoadSegments(const BinaryReader& reader,
                                   const GenericElfImage& image) {
  for (const Elf64ProgramHeader& ph : image.program_headers) {
    if (ph.type != kPtLoad) {
      continue;
    }
    // filesz <= memsz. The tail is the zero-fill (BSS) region.
    if (ph.filesz > ph.memsz) {
      return StatusCode::kInvalidSegment;
    }
    // The file range must lie inside the input.
    {
      const Status st = reader.CheckRange(ph.offset, ph.filesz);
      if (!st.ok()) {
        return st;
      }
    }
    // The virtual range must not overflow the 64-bit address space.
    if (BinaryReader::AddOverflows(ph.vaddr, ph.memsz)) {
      return StatusCode::kIntegerOverflow;
    }
    // p_align: 0/1 mean "no alignment requirement"; otherwise a power of two,
    // and p_vaddr and p_offset must be congruent modulo p_align.
    if (ph.align > 1) {
      if (!IsPowerOfTwo(ph.align)) {
        return StatusCode::kInvalidAlignment;
      }
      if ((ph.vaddr % ph.align) != (ph.offset % ph.align)) {
        return StatusCode::kInvalidAlignment;
      }
    }
  }
  return StatusCode::kOk;
}

}  // namespace p5emu::loader
