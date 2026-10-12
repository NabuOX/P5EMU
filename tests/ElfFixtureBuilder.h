// SPDX-License-Identifier: LicenseRef-P5EMU-Proprietary
//
// P5EMU source is independently authored and proprietary. See LICENSE.md and
// PROVENANCE.md before adding code here.
//
// ============================================================================
// SYNTHETIC ELF64 FIXTURE BUILDER
// ============================================================================
//
// Builds ELF64 images in memory for loader tests. Everything is authored here
// from the System V ABI field layout; no real executable, firmware, SDK, or
// game binary is used. The images are deliberately small and self-describing.
// ============================================================================

#ifndef P5EMU_TESTS_ELF_FIXTURE_BUILDER_H
#define P5EMU_TESTS_ELF_FIXTURE_BUILDER_H

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace p5emu::test {

struct Elf64FixtureOptions {
  std::vector<std::uint8_t> text;   // executed program bytes (RX segment)
  std::vector<std::uint8_t> data;   // optional RW payload
  std::uint64_t bss_size = 0;       // zero-fill appended to the data segment

  std::vector<std::pair<std::int64_t, std::uint64_t>> dynamic_extra;
  std::vector<std::string> needed_modules;

  std::uint16_t e_type = 0xfe18;    // PS5 dialect ET_DYNAMIC by default
  std::uint8_t osabi = 9;           // ELFOSABI_FREEBSD
  std::uint16_t machine = 62;       // EM_X86_64
  std::uint32_t text_flags = 0x5;   // R|X
  std::uint32_t data_flags = 0x6;   // R|W
  std::uint64_t text_vaddr = 0x400000;
};

struct Elf64Fixture {
  std::vector<std::uint8_t> bytes;
  std::uint64_t entry = 0;
  std::uint64_t text_vaddr = 0;
  std::uint64_t text_size = 0;
  std::uint64_t data_vaddr = 0;
  std::uint64_t data_file_size = 0;
  std::uint64_t bss_size = 0;
  bool has_data = false;
  bool has_dynamic = false;
};

namespace detail {
inline void Put16(std::vector<std::uint8_t>& b, std::size_t off, std::uint16_t v) {
  b[off + 0] = static_cast<std::uint8_t>(v & 0xFF);
  b[off + 1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
}
inline void Put32(std::vector<std::uint8_t>& b, std::size_t off, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) {
    b[off + static_cast<std::size_t>(i)] =
        static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF);
  }
}
inline void Put64(std::vector<std::uint8_t>& b, std::size_t off, std::uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    b[off + static_cast<std::size_t>(i)] =
        static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF);
  }
}
inline std::uint64_t AlignUp(std::uint64_t v, std::uint64_t a) {
  return ((v + a - 1) / a) * a;
}
}  // namespace detail

inline Elf64Fixture BuildElf64Fixture(const Elf64FixtureOptions& opts) {
  using detail::AlignUp;
  using detail::Put16;
  using detail::Put32;
  using detail::Put64;

  Elf64Fixture fx;
  fx.text_vaddr = opts.text_vaddr;
  fx.text_size = opts.text.size();
  fx.entry = opts.text_vaddr;  // entry is the first byte of the text segment

  const bool has_dynamic =
      !opts.dynamic_extra.empty() || !opts.needed_modules.empty();
  const bool has_data = has_dynamic || !opts.data.empty() || opts.bss_size > 0;
  fx.has_data = has_data;
  fx.has_dynamic = has_dynamic;

  const std::uint64_t phoff = 64;
  const std::uint16_t phnum = static_cast<std::uint16_t>(
      1u + (has_data ? 1u : 0u) + (has_dynamic ? 1u : 0u));
  const std::uint64_t text_off = 0x1000;
  std::uint64_t data_off = AlignUp(text_off + fx.text_size, 0x1000);
  fx.data_vaddr = opts.text_vaddr + AlignUp(fx.text_size, 0x1000);

  // String table: leading NUL, then each module name NUL-terminated.
  std::vector<std::uint8_t> strtab;
  strtab.push_back(0);
  std::vector<std::uint64_t> name_offsets;
  for (const std::string& n : opts.needed_modules) {
    name_offsets.push_back(strtab.size());
    for (char c : n) strtab.push_back(static_cast<std::uint8_t>(c));
    strtab.push_back(0);
  }

  const std::uint64_t user_data_size = opts.data.size();
  const std::uint64_t entry_count =
      has_dynamic
          ? (opts.dynamic_extra.size() + opts.needed_modules.size() + 2u + 1u)
          : 0u;  // +DT_STRTAB +DT_STRSZ +DT_NULL
  const std::uint64_t dynamic_bytes = entry_count * 16u;
  const std::uint64_t dynamic_off = data_off + user_data_size;
  const std::uint64_t dynamic_vaddr = fx.data_vaddr + user_data_size;
  const std::uint64_t strtab_off = dynamic_off + dynamic_bytes;
  const std::uint64_t strtab_vaddr = fx.data_vaddr + user_data_size + dynamic_bytes;
  const std::uint64_t data_file_size =
      user_data_size + dynamic_bytes + strtab.size();
  fx.data_file_size = data_file_size;

  const std::uint64_t file_size =
      has_data ? (data_off + data_file_size) : (text_off + fx.text_size);
  fx.bytes.assign(static_cast<std::size_t>(file_size), 0);
  auto& b = fx.bytes;

  // --- ELF header --------------------------------------------------------
  b[0] = 0x7F;
  b[1] = 'E';
  b[2] = 'L';
  b[3] = 'F';
  b[4] = 2;  // ELFCLASS64
  b[5] = 1;  // ELFDATA2LSB
  b[6] = 1;  // EV_CURRENT
  b[7] = opts.osabi;
  b[8] = 0;
  Put16(b, 16, opts.e_type);
  Put16(b, 18, opts.machine);
  Put32(b, 20, 1);  // e_version
  Put64(b, 24, fx.entry);
  Put64(b, 32, phoff);
  Put64(b, 40, 0);  // e_shoff
  Put32(b, 48, 0);  // e_flags
  Put16(b, 52, 64); // e_ehsize
  Put16(b, 54, 56); // e_phentsize
  Put16(b, 56, phnum);
  Put16(b, 58, 0);
  Put16(b, 60, 0);
  Put16(b, 62, 0);

  std::uint64_t ph = phoff;
  const auto write_ph = [&](std::uint32_t type, std::uint32_t flags,
                            std::uint64_t offset, std::uint64_t vaddr,
                            std::uint64_t filesz, std::uint64_t memsz,
                            std::uint64_t align) {
    Put32(b, ph + 0, type);
    Put32(b, ph + 4, flags);
    Put64(b, ph + 8, offset);
    Put64(b, ph + 16, vaddr);
    Put64(b, ph + 24, vaddr);  // p_paddr
    Put64(b, ph + 32, filesz);
    Put64(b, ph + 40, memsz);
    Put64(b, ph + 48, align);
    ph += 56;
  };

  write_ph(1 /*PT_LOAD*/, opts.text_flags, text_off, fx.text_vaddr,
           fx.text_size, fx.text_size, 0x1000);
  if (has_data) {
    write_ph(1 /*PT_LOAD*/, opts.data_flags, data_off, fx.data_vaddr,
             data_file_size, data_file_size + opts.bss_size, 0x1000);
  }
  if (has_dynamic) {
    write_ph(2 /*PT_DYNAMIC*/, 0x6 /*R|W*/, dynamic_off, dynamic_vaddr,
             dynamic_bytes, dynamic_bytes, 8);
  }

  // --- segment payloads --------------------------------------------------
  for (std::size_t i = 0; i < opts.text.size(); ++i) {
    b[static_cast<std::size_t>(text_off) + i] = opts.text[i];
  }
  for (std::size_t i = 0; i < opts.data.size(); ++i) {
    b[static_cast<std::size_t>(data_off) + i] = opts.data[i];
  }

  // --- dynamic table and string table ------------------------------------
  if (has_dynamic) {
    std::uint64_t e = dynamic_off;
    const auto put_dyn = [&](std::int64_t tag, std::uint64_t val) {
      Put64(b, static_cast<std::size_t>(e), static_cast<std::uint64_t>(tag));
      Put64(b, static_cast<std::size_t>(e) + 8, val);
      e += 16;
    };
    put_dyn(5 /*DT_STRTAB*/, strtab_vaddr);
    put_dyn(10 /*DT_STRSZ*/, strtab.size());
    for (std::size_t i = 0; i < opts.needed_modules.size(); ++i) {
      put_dyn(1 /*DT_NEEDED*/, name_offsets[i]);
    }
    for (const auto& d : opts.dynamic_extra) {
      put_dyn(d.first, d.second);
    }
    put_dyn(0 /*DT_NULL*/, 0);

    for (std::size_t i = 0; i < strtab.size(); ++i) {
      b[static_cast<std::size_t>(strtab_off) + i] = strtab[i];
    }
  }

  (void)dynamic_bytes;
  return fx;
}

}  // namespace p5emu::test

#endif  // P5EMU_TESTS_ELF_FIXTURE_BUILDER_H
