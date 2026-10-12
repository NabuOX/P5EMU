// SPDX-License-Identifier: Apache-2.0
//
// P5EMU is an independent, open-source project. See LICENSE and
// PROVENANCE.md before adding code here.
//
// ============================================================================
// 128-BIT SIMD VALUE (Phase 12)
// ============================================================================
//
// A host-independent representation of an x86-64 128-bit SIMD value. It is two
// explicit 64-bit halves, NOT a compiler vector extension and NOT a NEON type,
// so the P5 IR and the architectural state never depend on a host ISA.
//
// BYTE ORDER (explicit and tested):
//   `lo` is bytes 0..7  -- the low half, matching the little-endian layout at the
//                          lowest guest memory address.
//   `hi` is bytes 8..15 -- the high half.
//
// Every Phase-12 SIMD operation is defined per 64-bit half, so the AArch64
// backend needs only scalar 64-bit instructions (no NEON encoding). A NEON
// representation is a later optimisation, not part of this foundation.
// ============================================================================

#ifndef P5EMU_CPU_VEC128_H
#define P5EMU_CPU_VEC128_H

#include <cstdint>

namespace p5emu::cpu {

struct Vec128 {
  std::uint64_t lo = 0;  // bytes 0..7  (low  half, lowest address)
  std::uint64_t hi = 0;  // bytes 8..15 (high half)

  friend bool operator==(const Vec128& a, const Vec128& b) noexcept {
    return a.lo == b.lo && a.hi == b.hi;
  }
  friend bool operator!=(const Vec128& a, const Vec128& b) noexcept {
    return !(a == b);
  }
};

// Number of architectural XMM registers (XMM0..XMM15). No YMM/ZMM.
inline constexpr std::uint32_t kXmmCount = 16;

}  // namespace p5emu::cpu

#endif  // P5EMU_CPU_VEC128_H
