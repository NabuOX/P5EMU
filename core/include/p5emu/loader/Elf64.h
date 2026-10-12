// SPDX-License-Identifier: LicenseRef-P5EMU-Proprietary
//
// P5EMU source is independently authored and proprietary. See LICENSE.md and
// PROVENANCE.md before adding code here.
//
// ============================================================================
// GENERIC ELF64
// ============================================================================
//
// A generic, PS5-agnostic ELF64 reader. It knows the System V gABI layout and
// nothing about any console. It deliberately accepts any ELF64 for any machine
// so it remains useful for independently authored ELF fixtures; the
// console-specific constraints live in Ps5Executable.h.
//
// Field layout is the System V ABI / ELF gABI (e_ident[16], then the 64-bit
// header fields). Constants are facts from that specification; see PROVENANCE.md.
// ============================================================================

#ifndef P5EMU_LOADER_ELF64_H
#define P5EMU_LOADER_ELF64_H

#include <cstdint>
#include <vector>

#include "p5emu/core/Status.h"
#include "p5emu/loader/BinaryReader.h"

namespace p5emu::loader {

// --- ELF identification and header constants (System V ABI) ----------------
inline constexpr std::uint8_t kElfIdentSize = 16;
inline constexpr std::uint8_t kElfClass64 = 2;
inline constexpr std::uint8_t kElfDataLsb = 1;
inline constexpr std::uint8_t kElfDataMsb = 2;
inline constexpr std::uint8_t kElfIdentVersion = 1;   // EI_VERSION
inline constexpr std::uint16_t kElfEvCurrent = 1;     // EV_CURRENT
inline constexpr std::uint16_t kElfEtExec = 2;
inline constexpr std::uint16_t kElfEtDyn = 3;
inline constexpr std::uint16_t kElfEmX86_64 = 62;
inline constexpr std::size_t kElf64HeaderSize = 64;
inline constexpr std::uint16_t kElf64PhdrSize = 56;

// Program-header types and flags used by the loader foundation.
inline constexpr std::uint32_t kPtLoad = 1;
inline constexpr std::uint32_t kPtDynamic = 2;
inline constexpr std::uint32_t kPfX = 1;
inline constexpr std::uint32_t kPfW = 2;
inline constexpr std::uint32_t kPfR = 4;

// Refuse absurd tables rather than allocating on an attacker's count.
inline constexpr std::uint32_t kMaxProgramHeaders = 4096;

struct Elf64ProgramHeader {
  std::uint32_t type = 0;
  std::uint32_t flags = 0;
  std::uint64_t offset = 0;
  std::uint64_t vaddr = 0;
  std::uint64_t paddr = 0;
  std::uint64_t filesz = 0;
  std::uint64_t memsz = 0;
  std::uint64_t align = 0;
};

struct GenericElfImage {
  std::uint8_t elf_class = 0;
  std::uint8_t data_encoding = 0;
  std::uint8_t osabi = 0;          // EI_OSABI
  std::uint8_t abi_version = 0;    // EI_ABIVERSION
  bool big_endian = false;

  std::uint16_t type = 0;          // e_type
  std::uint16_t machine = 0;       // e_machine
  std::uint32_t version = 0;       // e_version
  std::uint64_t entry = 0;         // e_entry
  std::uint64_t flags = 0;         // e_flags

  std::vector<Elf64ProgramHeader> program_headers;
};

// Parses the ELF64 header and program-header table. Structural checks only:
// magic, class, endianness, version, header sizes, table bounds and counts.
// It does NOT enforce machine-specific constraints (that is the console layer).
Status ReadGenericElf64(const BinaryReader& reader, GenericElfImage* out);

// Generic segment validation for the PT_LOAD set: filesz <= memsz, the file
// range lies inside the input, the virtual range does not overflow, and p_align
// is a power of two consistent with p_offset and p_vaddr. Overlap policy and
// entry-point resolution are the LoadPlan's responsibility.
Status ValidateGenericLoadSegments(const BinaryReader& reader,
                                   const GenericElfImage& image);

}  // namespace p5emu::loader

#endif  // P5EMU_LOADER_ELF64_H
