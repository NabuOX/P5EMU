// SPDX-License-Identifier: LicenseRef-P5EMU-Proprietary
//
// P5EMU source is independently authored and proprietary. See LICENSE.md and
// PROVENANCE.md before adding code here.
//
// ============================================================================
// BOUNDS-CHECKED BINARY READER
// ============================================================================
//
// A reader over an immutable byte buffer for parsing untrusted files.
//
// Guarantees, all enforced here rather than at every call site:
//
//   * Every read is bounds-checked.
//   * Every combination of attacker-controlled values is overflow-checked:
//     off+len, count*entry_size, vaddr+memsz. `AddOverflows`/`MulOverflows`
//     are used for all of them.
//   * No reinterpret_cast of the buffer into a struct, and no unaligned host
//     load. Multi-byte scalars are assembled one byte at a time, so parsing is
//     correct regardless of host alignment and endianness.
//   * Failures are deterministic Status values; the reader never throws, never
//     reads out of bounds, and never partially writes a destination on failure.
// ============================================================================

#ifndef P5EMU_LOADER_BINARY_READER_H
#define P5EMU_LOADER_BINARY_READER_H

#include <cstddef>
#include <cstdint>

#include "p5emu/core/Status.h"

namespace p5emu::loader {

class BinaryReader {
 public:
  BinaryReader(const std::uint8_t* data, std::size_t size) noexcept
      : data_(data), size_(size) {}

  [[nodiscard]] std::size_t size() const noexcept { return size_; }
  [[nodiscard]] const std::uint8_t* data() const noexcept { return data_; }

  // Succeeds iff [off, off+len) lies fully inside the buffer and off+len does
  // not wrap a 64-bit value.
  [[nodiscard]] Status CheckRange(std::uint64_t off,
                                  std::uint64_t len) const noexcept {
    if (AddOverflows(off, len)) {
      return StatusCode::kIntegerOverflow;
    }
    if (off + len > static_cast<std::uint64_t>(size_)) {
      return StatusCode::kTruncatedInput;
    }
    return StatusCode::kOk;
  }

  Status U8(std::uint64_t off, std::uint8_t* out) const noexcept {
    if (out == nullptr) return StatusCode::kInvalidArgument;
    if (off >= static_cast<std::uint64_t>(size_)) {
      return StatusCode::kTruncatedInput;
    }
    *out = data_[static_cast<std::size_t>(off)];
    return StatusCode::kOk;
  }

  Status U16(std::uint64_t off, bool big_endian,
             std::uint16_t* out) const noexcept {
    if (out == nullptr) return StatusCode::kInvalidArgument;
    const Status st = CheckRange(off, 2);
    if (!st.ok()) return st;
    const std::uint8_t* p = data_ + static_cast<std::size_t>(off);
    const std::uint16_t v =
        big_endian ? static_cast<std::uint16_t>((p[0] << 8) | p[1])
                   : static_cast<std::uint16_t>(p[0] | (p[1] << 8));
    *out = v;
    return StatusCode::kOk;
  }

  Status U32(std::uint64_t off, bool big_endian,
             std::uint32_t* out) const noexcept {
    if (out == nullptr) return StatusCode::kInvalidArgument;
    const Status st = CheckRange(off, 4);
    if (!st.ok()) return st;
    const std::uint8_t* p = data_ + static_cast<std::size_t>(off);
    std::uint32_t v = 0;
    if (big_endian) {
      for (int i = 0; i < 4; ++i) v = (v << 8) | p[i];
    } else {
      for (int i = 3; i >= 0; --i) v = (v << 8) | p[i];
    }
    *out = v;
    return StatusCode::kOk;
  }

  Status U64(std::uint64_t off, bool big_endian,
             std::uint64_t* out) const noexcept {
    if (out == nullptr) return StatusCode::kInvalidArgument;
    const Status st = CheckRange(off, 8);
    if (!st.ok()) return st;
    const std::uint8_t* p = data_ + static_cast<std::size_t>(off);
    std::uint64_t v = 0;
    if (big_endian) {
      for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
    } else {
      for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
    }
    *out = v;
    return StatusCode::kOk;
  }

  Status S32(std::uint64_t off, bool big_endian,
             std::int32_t* out) const noexcept {
    if (out == nullptr) return StatusCode::kInvalidArgument;
    std::uint32_t v = 0;
    const Status st = U32(off, big_endian, &v);
    if (!st.ok()) return st;
    *out = static_cast<std::int32_t>(v);
    return StatusCode::kOk;
  }

  Status S64(std::uint64_t off, bool big_endian,
             std::int64_t* out) const noexcept {
    if (out == nullptr) return StatusCode::kInvalidArgument;
    std::uint64_t v = 0;
    const Status st = U64(off, big_endian, &v);
    if (!st.ok()) return st;
    *out = static_cast<std::int64_t>(v);
    return StatusCode::kOk;
  }

  Status Copy(std::uint64_t off, std::uint8_t* dest,
              std::uint64_t len) const noexcept {
    if (dest == nullptr && len != 0) return StatusCode::kInvalidArgument;
    const Status st = CheckRange(off, len);
    if (!st.ok()) return st;
    for (std::uint64_t i = 0; i < len; ++i) {
      dest[static_cast<std::size_t>(i)] =
          data_[static_cast<std::size_t>(off + i)];
    }
    return StatusCode::kOk;
  }

  [[nodiscard]] static bool AddOverflows(std::uint64_t a,
                                         std::uint64_t b) noexcept {
    return a > (~std::uint64_t{0} - b);
  }
  [[nodiscard]] static bool MulOverflows(std::uint64_t a,
                                         std::uint64_t b) noexcept {
    if (a == 0 || b == 0) return false;
    return a > (~std::uint64_t{0} / b);
  }

 private:
  const std::uint8_t* data_;
  std::size_t size_;
};

}  // namespace p5emu::loader

#endif  // P5EMU_LOADER_BINARY_READER_H
