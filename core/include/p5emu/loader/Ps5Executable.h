// SPDX-License-Identifier: Apache-2.0
//
// P5EMU is an independent, open-source project. See LICENSE and
// PROVENANCE.md before adding code here.
//
// ============================================================================
// PS5 / PROSPERO EXECUTABLE INTERPRETATION
// ============================================================================
//
// The console-specific layer, kept separate from the generic ELF reader.
//
// WHAT IS PROVEN, WHAT IS DOCUMENTED, WHAT IS UNKNOWN
//
// Constants are FACTS learned from public research and re-typed; names here are
// P5EMU's own. See PROVENANCE.md for the full mapping and classification. The
// important boundary:
//
//   * A tag NUMBER is a fact.
//   * The MEANING of a Sony tag whose defining structure is not documented is
//     UNKNOWN. Such a tag is PRESERVED with its raw value and receives NO
//     interpretation. In particular the PS5-era "_1" variants are UNKNOWN.
//   * Generic dynamic tags (System V ABI) are interpreted generically.
//
// This layer therefore: recognizes the PS5 dialect; parses the dynamic table
// structurally; resolves generic DT_NEEDED module names through the string
// table; and preserves every other entry without inventing meaning. It never
// implements a syscall, an HLE function, or `_1` semantics.
// ============================================================================

#ifndef P5EMU_LOADER_PS5_EXECUTABLE_H
#define P5EMU_LOADER_PS5_EXECUTABLE_H

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "p5emu/core/Status.h"
#include "p5emu/loader/BinaryReader.h"
#include "p5emu/loader/Elf64.h"

namespace p5emu::loader {

// --- Identification --------------------------------------------------------
// ELFOSABI_FREEBSD is a System V ABI value; the PS5 tags/types below are facts
// documented in public PS5 tooling (see PROVENANCE.md).
inline constexpr std::uint8_t kElfOsabiFreeBsd = 9;
inline constexpr std::uint16_t kPs5EtDynExec = 0xfe10;  // PS5 dialect ET_*
inline constexpr std::uint16_t kPs5EtDynamic = 0xfe18;

// --- Sony dynamic tag namespace (numbers are facts) ------------------------
inline constexpr std::uint64_t kDtOsDynlibData = 0x61000000;
inline constexpr std::uint64_t kDtOsProcParam = 0x61000001;
inline constexpr std::uint64_t kDtOsFingerprint = 0x61000007;
inline constexpr std::uint64_t kDtOsModuleInfo = 0x6100000d;
inline constexpr std::uint64_t kDtOsNeededModule = 0x6100000f;
inline constexpr std::uint64_t kDtOsRelro = 0x61000010;
inline constexpr std::uint64_t kDtOsModuleAttr = 0x61000011;
inline constexpr std::uint64_t kDtOsExportLib = 0x61000013;
inline constexpr std::uint64_t kDtOsImportLib = 0x61000015;
inline constexpr std::uint64_t kDtOsHash = 0x61000025;
inline constexpr std::uint64_t kDtOsPltGot = 0x61000027;
inline constexpr std::uint64_t kDtOsJmprel = 0x61000029;
inline constexpr std::uint64_t kDtOsRela = 0x6100002f;
inline constexpr std::uint64_t kDtOsStrtab = 0x61000035;
inline constexpr std::uint64_t kDtOsSymtab = 0x61000039;
inline constexpr std::uint64_t kDtOsHashsz = 0x6100003d;
inline constexpr std::uint64_t kDtOsSymtabsz = 0x6100003f;
// PS5-era "_1" variants start here. Semantics are UNKNOWN; the range is
// recognized only so those entries are reported as unknown rather than
// silently dropped.
inline constexpr std::uint64_t kPs5VariantTagBase = 0x61000041;
inline constexpr std::uint64_t kPs5TagRangeEnd = 0x62000000;

// --- Generic dynamic tags (System V ABI) -----------------------------------
inline constexpr std::int64_t kDtNull = 0;
inline constexpr std::int64_t kDtNeeded = 1;
inline constexpr std::int64_t kDtPltrelsz = 2;
inline constexpr std::int64_t kDtPltgot = 3;
inline constexpr std::int64_t kDtHash = 4;
inline constexpr std::int64_t kDtStrtab = 5;
inline constexpr std::int64_t kDtSymtab = 6;
inline constexpr std::int64_t kDtRela = 7;
inline constexpr std::int64_t kDtRelasz = 8;
inline constexpr std::int64_t kDtRelaent = 9;
inline constexpr std::int64_t kDtStrsz = 10;
inline constexpr std::int64_t kDtSyment = 11;
inline constexpr std::int64_t kDtInit = 12;
inline constexpr std::int64_t kDtFini = 13;
inline constexpr std::int64_t kDtRel = 17;
inline constexpr std::int64_t kDtRelsz = 18;
inline constexpr std::int64_t kDtRelent = 19;
inline constexpr std::int64_t kDtPltrel = 20;
inline constexpr std::int64_t kDtJmprel = 23;
inline constexpr std::int64_t kDtFlags1 = 0x6ffffffb;

// A raw dynamic entry. Always preserved verbatim.
struct DynamicEntry {
  std::int64_t tag = 0;
  std::uint64_t value = 0;
};

// A module the executable references. UNRESOLVED by construction: the loader
// records the requirement and never invents an implementation.
struct ImportModule {
  std::string name;         // resolved from the dynamic string table
  std::int64_t tag = 0;     // tag that named it (DT_NEEDED in Phase 4)
  bool resolved = false;    // always false; resolution is a later HLE phase
};

// A recognized Sony/PS5 dynamic tag whose value is preserved but NOT
// interpreted, because the defining structure is not documented.
struct RecognizedPs5Tag {
  std::uint64_t tag = 0;
  std::uint64_t value = 0;
  bool is_unknown_variant = false;  // true for the PS5 "_1" range (UNKNOWN)
};

struct Ps5ExecutableImage {
  bool is_ps5_dialect = false;
  bool big_endian = false;
  std::uint8_t osabi = 0;
  std::uint16_t e_type = 0;
  std::uint16_t machine = 0;

  std::uint64_t entry = 0;
  std::vector<Elf64ProgramHeader> load_segments;

  bool has_dynamic = false;
  std::vector<DynamicEntry> dynamic_raw;

  std::optional<std::uint64_t> strtab_addr;
  std::optional<std::uint64_t> strtab_size;

  // Generic dynamic relocation and symbol-table pointers (System V ABI). PS5
  // keeps these in the same Sony namespace for its own tables, but these generic
  // tags are what Phase 5 parses; their semantics are the AMD64 psABI's.
  std::optional<std::uint64_t> rela_addr;
  std::optional<std::uint64_t> rela_size;
  std::optional<std::uint64_t> rela_ent;
  std::optional<std::uint64_t> jmprel_addr;
  std::optional<std::uint64_t> pltrel_size;
  std::optional<std::uint64_t> pltrel_type;
  std::optional<std::uint64_t> symtab_addr;
  std::optional<std::uint64_t> syment;

  // Requirements discovered, unresolved. Phase 4 discovers module names via the
  // generic DT_NEEDED tag; it does not decode Sony symbol structures.
  std::vector<ImportModule> needed_modules;
  bool declares_ps5_imports = false;   // DT_OS_IMPORT_LIB present
  bool declares_ps5_exports = false;   // DT_OS_EXPORT_LIB present

  // Recognized Sony tags (values preserved, no semantics) and everything else.
  std::vector<RecognizedPs5Tag> ps5_tags;
  std::vector<DynamicEntry> unknown_tags;
};

// Parses the PS5 interpretation layer over a generic ELF image.
//
// Requires EM_X86_64 (else kUnsupportedMachine). A plain ELF64 for x86-64 is
// accepted and flagged `is_ps5_dialect == false`, so synthetic fixtures can
// exercise the whole pipeline without pretending to be a console binary.
Status ParsePs5Executable(const BinaryReader& reader,
                          const GenericElfImage& elf,
                          Ps5ExecutableImage* out);

// Maps a guest virtual address to a file offset using the PT_LOAD segments, or
// nullopt if the address is not inside a loadable segment. Generic; used by the
// relocation parser.
std::optional<std::uint64_t> GuestVaToFileOffset(const Ps5ExecutableImage& image,
                                                 std::uint64_t va);

}  // namespace p5emu::loader

#endif  // P5EMU_LOADER_PS5_EXECUTABLE_H
