// SPDX-License-Identifier: Apache-2.0
//
// P5EMU is an independent, open-source project. See LICENSE and
// PROVENANCE.md before adding code here.
//
// ============================================================================
// SYNTHETIC ELF64 RELOCATION FIXTURE BUILDER
// ============================================================================
//
// Builds an ELF64 image with a dynamic symbol table, string table, and RELA
// relocation tables, authored here from the System V AMD64 psABI layout. No real
// binary, firmware, SDK, or game file is used.
// ============================================================================

#ifndef P5EMU_TESTS_RELOC_FIXTURE_BUILDER_H
#define P5EMU_TESTS_RELOC_FIXTURE_BUILDER_H

#include <cstdint>
#include <string>
#include <vector>

namespace p5emu::test {

struct RelocSym {
  std::string name;
  std::uint64_t elf_value = 0;  // st_value (ELF vaddr); ignored for imports
  bool import = true;           // true: st_shndx = 0 (SHN_UNDEF)
};

struct RelocEntry {
  std::uint64_t target_elf_va = 0;  // r_offset (ELF vaddr)
  std::uint32_t type = 0;           // R_X86_64_*
  std::uint32_t sym_index = 0;      // dynsym index (0 = null)
  std::int64_t addend = 0;          // r_addend
};

struct RelocFixtureOptions {
  std::vector<std::uint8_t> text;
  std::vector<std::uint8_t> data;  // user data; relocation targets land here
  std::vector<RelocSym> symbols;   // user symbols start at dynsym index 1
  std::vector<RelocEntry> rela_dyn;
  std::vector<RelocEntry> rela_plt;

  std::uint64_t text_vaddr = 0x400000;
  std::uint64_t data_vaddr = 0x401000;
  std::uint64_t entry_offset_in_text = 0;

  std::uint16_t e_type = 3;   // ET_DYN
  std::uint8_t osabi = 0;     // System V
  std::uint16_t machine = 62;
};

struct RelocFixture {
  std::vector<std::uint8_t> bytes;
  std::uint64_t entry = 0;
  std::uint64_t text_vaddr = 0;
  std::uint64_t data_vaddr = 0;
  std::uint64_t data_size = 0;
};

namespace reloc_detail {
inline void Put16(std::vector<std::uint8_t>& b, std::size_t o, std::uint16_t v) {
  for (int i = 0; i < 2; ++i)
    b[o + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF);
}
inline void Put32(std::vector<std::uint8_t>& b, std::size_t o, std::uint32_t v) {
  for (int i = 0; i < 4; ++i)
    b[o + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF);
}
inline void Put64(std::vector<std::uint8_t>& b, std::size_t o, std::uint64_t v) {
  for (int i = 0; i < 8; ++i)
    b[o + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF);
}
inline std::uint64_t Align8(std::uint64_t v) { return (v + 7u) & ~std::uint64_t{7}; }
}  // namespace reloc_detail

inline RelocFixture BuildRelocElf64(const RelocFixtureOptions& o) {
  using reloc_detail::Align8;
  using reloc_detail::Put16;
  using reloc_detail::Put32;
  using reloc_detail::Put64;

  RelocFixture fx;
  fx.text_vaddr = o.text_vaddr;
  fx.data_vaddr = o.data_vaddr;
  fx.entry = o.text_vaddr + o.entry_offset_in_text;

  // --- string table ------------------------------------------------------
  std::vector<std::uint8_t> dynstr;
  dynstr.push_back(0);
  std::vector<std::uint32_t> name_off(o.symbols.size(), 0);
  for (std::size_t i = 0; i < o.symbols.size(); ++i) {
    name_off[i] = static_cast<std::uint32_t>(dynstr.size());
    for (char c : o.symbols[i].name) dynstr.push_back(static_cast<std::uint8_t>(c));
    dynstr.push_back(0);
  }

  // --- dynamic symbols ---------------------------------------------------
  const std::uint64_t nsyms = 1 + o.symbols.size();
  std::vector<std::uint8_t> dynsym(static_cast<std::size_t>(nsyms * 24), 0);
  for (std::size_t i = 0; i < o.symbols.size(); ++i) {
    const std::size_t base = static_cast<std::size_t>((i + 1) * 24);
    Put32(dynsym, base + 0, name_off[i]);
    dynsym[base + 4] = 0;  // st_info
    dynsym[base + 5] = 0;  // st_other
    Put16(dynsym, base + 6, o.symbols[i].import ? 0u : 0xfff1u);  // st_shndx
    Put64(dynsym, base + 8, o.symbols[i].elf_value);
    Put64(dynsym, base + 16, 0);  // st_size
  }

  // --- RELA tables -------------------------------------------------------
  auto build_rela = [](const std::vector<RelocEntry>& entries) {
    std::vector<std::uint8_t> out(entries.size() * 24, 0);
    for (std::size_t i = 0; i < entries.size(); ++i) {
      const std::size_t base = i * 24;
      Put64(out, base + 0, entries[i].target_elf_va);
      const std::uint64_t info =
          (static_cast<std::uint64_t>(entries[i].sym_index) << 32) |
          static_cast<std::uint64_t>(entries[i].type);
      Put64(out, base + 8, info);
      Put64(out, base + 16, static_cast<std::uint64_t>(entries[i].addend));
    }
    return out;
  };
  const std::vector<std::uint8_t> rela = build_rela(o.rela_dyn);
  const std::vector<std::uint8_t> plt = build_rela(o.rela_plt);

  // --- data layout -------------------------------------------------------
  const std::uint64_t user_size = o.data.size();
  const std::uint64_t off_dynsym = Align8(user_size);
  const std::uint64_t off_dynstr = off_dynsym + dynsym.size();
  const std::uint64_t off_rela = Align8(off_dynstr + dynstr.size());
  const std::uint64_t off_plt = off_rela + rela.size();
  const std::uint64_t off_dyn = Align8(off_plt + plt.size());
  // Dynamic entries: STRTAB, STRSZ, SYMTAB, SYMENT, RELA, RELASZ, RELAENT,
  // JMPREL, PLTRELSZ, PLTREL, NULL.
  const std::uint64_t dyn_entries = 11;
  const std::uint64_t dyn_bytes = dyn_entries * 16;
  const std::uint64_t data_size = off_dyn + dyn_bytes;

  const std::uint64_t symtab_va = o.data_vaddr + off_dynsym;
  const std::uint64_t strtab_va = o.data_vaddr + off_dynstr;
  const std::uint64_t rela_va = o.data_vaddr + off_rela;
  const std::uint64_t plt_va = o.data_vaddr + off_plt;

  std::vector<std::uint8_t> data(static_cast<std::size_t>(data_size), 0);
  for (std::size_t i = 0; i < o.data.size(); ++i) data[i] = o.data[i];
  for (std::size_t i = 0; i < dynsym.size(); ++i)
    data[static_cast<std::size_t>(off_dynsym) + i] = dynsym[i];
  for (std::size_t i = 0; i < dynstr.size(); ++i)
    data[static_cast<std::size_t>(off_dynstr) + i] = dynstr[i];
  for (std::size_t i = 0; i < rela.size(); ++i)
    data[static_cast<std::size_t>(off_rela) + i] = rela[i];
  for (std::size_t i = 0; i < plt.size(); ++i)
    data[static_cast<std::size_t>(off_plt) + i] = plt[i];

  std::uint64_t e = off_dyn;
  const auto put_dyn = [&](std::int64_t tag, std::uint64_t val) {
    Put64(data, static_cast<std::size_t>(e), static_cast<std::uint64_t>(tag));
    Put64(data, static_cast<std::size_t>(e) + 8, val);
    e += 16;
  };
  put_dyn(5, strtab_va);        // DT_STRTAB
  put_dyn(10, dynstr.size());   // DT_STRSZ
  put_dyn(6, symtab_va);        // DT_SYMTAB
  put_dyn(11, 24);              // DT_SYMENT
  put_dyn(7, rela_va);          // DT_RELA
  put_dyn(8, rela.size());      // DT_RELASZ
  put_dyn(9, 24);               // DT_RELAENT
  put_dyn(23, plt_va);          // DT_JMPREL
  put_dyn(2, plt.size());       // DT_PLTRELSZ
  put_dyn(20, 7);               // DT_PLTREL (DT_RELA)
  put_dyn(0, 0);                // DT_NULL

  fx.data_size = data_size;

  // --- file --------------------------------------------------------------
  const std::uint64_t phoff = 64;
  const std::uint16_t phnum = 3;
  const std::uint64_t text_off = 0x1000;
  const std::uint64_t data_off = 0x2000;
  const std::uint64_t file_size = data_off + data_size;
  fx.bytes.assign(static_cast<std::size_t>(file_size), 0);
  auto& b = fx.bytes;

  b[0] = 0x7F; b[1] = 'E'; b[2] = 'L'; b[3] = 'F';
  b[4] = 2;  // ELFCLASS64
  b[5] = 1;  // ELFDATA2LSB
  b[6] = 1;  // EV_CURRENT
  b[7] = o.osabi;
  Put16(b, 16, o.e_type);
  Put16(b, 18, o.machine);
  Put32(b, 20, 1);
  Put64(b, 24, fx.entry);
  Put64(b, 32, phoff);
  Put16(b, 52, 64);
  Put16(b, 54, 56);
  Put16(b, 56, phnum);

  std::uint64_t ph = phoff;
  const auto write_ph = [&](std::uint32_t type, std::uint32_t flags,
                            std::uint64_t offset, std::uint64_t vaddr,
                            std::uint64_t filesz, std::uint64_t memsz,
                            std::uint64_t align) {
    Put32(b, ph + 0, type);
    Put32(b, ph + 4, flags);
    Put64(b, ph + 8, offset);
    Put64(b, ph + 16, vaddr);
    Put64(b, ph + 24, vaddr);
    Put64(b, ph + 32, filesz);
    Put64(b, ph + 40, memsz);
    Put64(b, ph + 48, align);
    ph += 56;
  };
  write_ph(1, 0x5, text_off, o.text_vaddr, o.text.size(), o.text.size(), 0x1000);
  write_ph(1, 0x6, data_off, o.data_vaddr, data_size, data_size, 0x1000);
  write_ph(2, 0x6, data_off + off_dyn, o.data_vaddr + off_dyn, dyn_bytes,
           dyn_bytes, 8);

  for (std::size_t i = 0; i < o.text.size(); ++i)
    b[static_cast<std::size_t>(text_off) + i] = o.text[i];
  for (std::size_t i = 0; i < data.size(); ++i)
    b[static_cast<std::size_t>(data_off) + i] = data[i];

  return fx;
}

}  // namespace p5emu::test

#endif  // P5EMU_TESTS_RELOC_FIXTURE_BUILDER_H
