// SPDX-License-Identifier: Apache-2.0
//
// P5EMU is an independent, open-source project. See LICENSE and
// PROVENANCE.md before adding code here.
//
// ============================================================================
// RELOCATIONS
// ============================================================================
//
// Generic x86-64 ELF relocation infrastructure, independent of any PS5 HLE.
//
// Pipeline: raw bytes -> validated Relocation -> (symbol) resolution
// requirement -> application. A parsed relocation carries structural facts and a
// load-bias-resolved target; it holds NO resolver pointer and NO HLE meaning.
//
// Supported types (System V AMD64 psABI formulas; see PROVENANCE.md):
//
//   R_X86_64_NONE      (0)  : nothing
//   R_X86_64_64        (1)  : S + A          (word64)
//   R_X86_64_GLOB_DAT  (6)  : S              (word64)
//   R_X86_64_JUMP_SLOT (7)  : S              (word64)
//   R_X86_64_RELATIVE  (8)  : B + A          (word64)
//
// B = load bias, S = symbol value, A = addend.
//
// RELA ONLY. x86-64 uses RELA (explicit addend). REL (implicit addend) is
// rejected explicitly rather than being misread as RELA.
// ============================================================================

#ifndef P5EMU_LOADER_RELOC_H
#define P5EMU_LOADER_RELOC_H

#include <cstdint>
#include <string>
#include <vector>

#include "p5emu/core/Status.h"
#include "p5emu/loader/BinaryReader.h"
#include "p5emu/loader/Ps5Executable.h"

namespace p5emu::loader {

enum class RelocType : std::uint32_t {
  kNone = 0,
  k64 = 1,
  kGlobDat = 6,
  kJumpSlot = 7,
  kRelative = 8,
  kUnsupported = 0xFFFFFFFFu,
};

const char* RelocTypeName(RelocType type) noexcept;
RelocType RelocTypeFromNumber(std::uint32_t number) noexcept;

// Layout constants (System V AMD64 psABI).
inline constexpr std::uint64_t kRelaEntrySize = 24;  // r_offset,r_info,r_addend
inline constexpr std::uint64_t kSymEntrySize = 24;   // Elf64_Sym
inline constexpr std::uint32_t kMaxRelocations = 1u << 20;
inline constexpr std::uint32_t kMaxSymbols = 1u << 20;

struct Relocation {
  std::uint64_t target_guest_va = 0;  // ELF r_offset + load bias
  RelocType type = RelocType::kNone;
  std::uint32_t symbol_index = 0;
  std::uint64_t addend = 0;  // RELA r_addend bit pattern
  std::uint32_t width = 8;   // bytes written
};

struct ElfSymbol {
  std::string name;
  std::uint64_t elf_value = 0;  // st_value (ELF vaddr)
  std::uint16_t shndx = 0;      // 0 == SHN_UNDEF (import)
  [[nodiscard]] bool IsImport() const noexcept { return shndx == 0; }
};

struct RelocationSet {
  bool present = false;
  std::vector<Relocation> relocations;
  std::vector<ElfSymbol> symbols;  // index 0 is the null symbol
};

// Parses the dynamic symbol table and the RELA relocation tables (DT_RELA and
// DT_JMPREL) referenced by the dynamic table. Load-bias-resolved target VAs are
// stored. Returns kUnsupportedRelocation when a REL table is present.
Status ParseDynamicRelocations(const BinaryReader& reader,
                               const Ps5ExecutableImage& image,
                               std::uint64_t load_bias,
                               RelocationSet* out);

// Computes the 64-bit value a relocation stores.
//   NONE      -> 0 (caller skips it)
//   RELATIVE  -> bias + addend
//   k64       -> symbol_address + addend
//   GLOB_DAT / JUMP_SLOT -> symbol_address
// Overflow in the add is reported as kRelocationOverflow.
Status ComputeRelocationValue(const Relocation& reloc, std::uint64_t load_bias,
                              std::uint64_t symbol_address,
                              std::uint64_t* value_out);

}  // namespace p5emu::loader

#endif  // P5EMU_LOADER_RELOC_H
