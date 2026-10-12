// SPDX-License-Identifier: Apache-2.0
//
// P5EMU is an independent, open-source project. See LICENSE and
// PROVENANCE.md before adding code here.
//
// ============================================================================
// GUEST MEMORY PORT
// ============================================================================
//
// DESIGN CONSTRAINT (Phase-1 review correction)
//
// Guest VA == host VA is NOT assumed, and is NOT a core architectural
// assumption. It is merely one strategy, available on some host platforms, and
// P5EMU must not be built so that adding another strategy requires changing CPU
// semantics.
//
// A backend may use any of:
//   * identity mapping, where the host address space permits and it is safe
//   * reserved mappings at fixed host addresses
//   * translated mappings, e.g. page tables or a TLB-like structure
//   * segmented mappings, e.g. several discontiguous regions presented as one
//     contiguous guest range
//
// without changing the meaning of a single IR memory operation.
//
// Consequences encoded in this interface:
//
//   1. Guests are addressed exclusively in guest virtual addresses. No method
//      takes or returns a host pointer.
//
//   2. There is NO accessor exposing a host pointer for direct load/store use.
//      `IdentityMapping()` reports whether a *fast path* is available, and
//      `TranslateForDirectAccess()` is an opt-in query a backend may use to
//      enable a direct path. Returning `kDirectAccessUnsupported` is a normal,
//      expected answer, never an error condition. CPU semantics are defined by
//      Read/Write/Fetch, and are identical whether or not a backend uses it.
//
//   3. Every access is bounded and range-checked by the port. A backend must not
//      be able to widen access by asking a different question.
//
//   4. Failure is deterministic: the same access against the same port state
//      yields the same Status. Faults are reported, never thrown, and never
//      allowed to escape as host hardware faults.
//
//   5. Self-modifying-code support is a first-class hook, because "guest wrote
//      to a page containing translated code" must invalidate that code on
//      platforms where the guest and host share pages.
//
// ============================================================================

#ifndef P5EMU_CPU_GUEST_MEMORY_H
#define P5EMU_CPU_GUEST_MEMORY_H

#include <cstddef>
#include <cstdint>
#include <vector>

#include "p5emu/core/Status.h"

namespace p5emu::memory {

// Address type. A full 64-bit guest virtual address. Deliberately NOT the same
// type as anything the host uses.
using GuestAddr = std::uint64_t;
using GuestSize = std::uint64_t;

// Alignment of every region in P5EMU's own guest model. Chosen as a power of
// two that suits both current development hosts and future targets.
//
// This is a P5EMU implementation parameter, NOT a discovered fact about any
// console. Phase 1 does not emulate any target's page size. It is a constant
// here so that every port implementation agrees, and so that a future
// target-specific page size is a change to this one constant rather than an
// architectural change.
inline constexpr GuestSize kGuestPageSize = 0x1000;

inline constexpr bool IsPageAligned(GuestAddr addr) noexcept {
  return (addr % kGuestPageSize) == 0;
}

inline constexpr GuestAddr PageBase(GuestAddr addr) noexcept {
  return addr & ~(kGuestPageSize - 1);
}

// Access width in bytes. 1, 2, 4 or 8.
using AccessWidth = std::uint32_t;

inline constexpr bool IsValidAccessWidth(AccessWidth width) noexcept {
  return width == 1 || width == 2 || width == 4 || width == 8;
}

// Permission bits, deliberately separate per-axis so that a future GPU-visible
// port can extend them without changing the CPU-visible contract.
//
// These are plain constants rather than an enum, because callers combine them
// with `|` and an enum would force a cast at every call site.
using Permissions = std::uint32_t;

inline constexpr Permissions kPermNone = 0;
inline constexpr Permissions kPermRead = 1u << 0;
inline constexpr Permissions kPermWrite = 1u << 1;
inline constexpr Permissions kPermExecute = 1u << 2;

// The three permission sets a simple region can have. Named so that a caller
// cannot accidentally construct a combination no port supports.
inline constexpr Permissions kPermReadWrite = kPermRead | kPermWrite;
inline constexpr Permissions kPermReadWriteExecute =
    kPermRead | kPermWrite | kPermExecute;
inline constexpr Permissions kPermExecuteOnly = kPermExecute;

// ---------------------------------------------------------------------------
// Region
// ---------------------------------------------------------------------------
// One mapped region of guest virtual address space. Regions may be discontiguous
// in host memory and need not be page-aligned relative to each other; this is
// what permits segmented mappings.
//
// No host pointer is stored. A port may keep one internally, but it is not part
// of the interface, because exposing it would let the abstraction leak.
// ---------------------------------------------------------------------------
struct Region {
  GuestAddr guest_base = 0;
  GuestSize size = 0;
  std::uint32_t permissions = kPermNone;
  const char* name = "";

  constexpr bool Contains(GuestAddr addr) const noexcept {
    return addr >= guest_base && (addr - guest_base) < size;
  }
};

// ---------------------------------------------------------------------------
// Direct-access capability
// ---------------------------------------------------------------------------
// Reported by ports that can map a guest range onto a contiguous host range at a
// fixed offset. A backend MAY use this to emit direct addressing. A backend
// MUST produce identical results whether or not it uses this path.
//
// The presence of this query is not an architectural assumption that identity
// mapping exists. It is an optimisation query whose negative answer is normal.
enum class DirectAccess : std::uint8_t {
  kUnsupported = 0,  // port cannot offer direct access at all
  kAvailable,        // TranslateForDirectAccess() will succeed for this range
};

// Why a direct-access query failed. Part of the result so callers can report
// meaningfully, and so behaviour is inspectable in tests.
enum class DirectAccessStatus : std::uint8_t {
  kOk = 0,
  kNoDirectAccess,   // port does not support it
  kNotIdentityMapped, // guest range is not identity-mapped in this port
  kUnmapped,         // guest range is not mapped at all
  kCrossesRegion,    // range spans two regions; cannot be one direct access
  kNoExecute,        // not executable, so not usable as a translation target
};

// ---------------------------------------------------------------------------
// GuestMemoryPort
// ---------------------------------------------------------------------------
class GuestMemoryPort {
 public:
  virtual ~GuestMemoryPort() = default;

  // -- Data access ----------------------------------------------------------
  // Reads `width` bytes little-endian from `addr`. Must succeed fully or fail
  // atomically: a partially-satisfied access returns a Status and must not
  // modify the destination.
  virtual Status ReadData(GuestAddr addr, AccessWidth width,
                          std::uint64_t* out) = 0;

  // Writes `width` bytes little-endian to `addr`, all-or-nothing.
  virtual Status WriteData(GuestAddr addr, AccessWidth width,
                           std::uint64_t value) = 0;

  // -- Instruction fetch ---------------------------------------------------
  // Separate from data access on purpose: a region may be executable but not
  // readable, and a future guest model may enforce NX.
  //
  // `length` is a byte count requested by the caller, not an instruction size.
  // The port must return the number of bytes actually available from `addr` in
  // `*available_out` even when `length` cannot be satisfied, so a caller
  // decodable at a page boundary can advance rather than fault.
  virtual Status FetchCode(GuestAddr addr, std::size_t length,
                           std::uint8_t* out, std::size_t* available_out) = 0;

  // -- Introspection -------------------------------------------------------
  // Bounds-checked containment test. Returns kUnmappedAccess when absent.
  virtual Status CheckAccess(GuestAddr addr, AccessWidth width,
                             std::uint32_t required_permissions) = 0;

  // Zero-extends the caller's address range check; a caller wanting "is this
  // whole span mapped" uses this with the span length.
  virtual bool IsMapped(GuestAddr addr) const noexcept = 0;

  // Permissions of the region containing `addr`, or kPermNone if unmapped.
  virtual std::uint32_t PermissionsAt(GuestAddr addr) const noexcept = 0;

  // -- Self-modifying code -------------------------------------------------
  // Called after a successful write to any address inside an executable region.
  // A port that implements SMC detection does its work here; a port that
  // does not may ignore it. Never called for a failed write.
  virtual void NotifyWriteToExecutable(GuestAddr addr,
                                       AccessWidth width) noexcept = 0;

  // -- Direct-access optimisation query ------------------------------------
  // Optional optimisation. A backend that gets anything other than kOk MUST
  // fall back to ReadData/WriteData and MUST produce identical results.
  //
  // `host_base_out`, when it succeeds, is the host address such that
  // host_base + (guest_addr - addr) addresses the same byte. It is valid only
  // for spans fully inside [addr, addr + length).
  virtual DirectAccessStatus QueryDirectAccess(GuestAddr addr,
                                               GuestSize length,
                                               std::uint64_t* host_base_out) const = 0;

  // Convenience: may a backend use direct addressing for this range at all?
  [[nodiscard]] bool IdentityMapped(GuestAddr addr,
                                    GuestSize length) const noexcept {
    std::uint64_t ignored = 0;
    return QueryDirectAccess(addr, length, &ignored) ==
           DirectAccessStatus::kOk;
  }

  // -- Bulk transport (Phase 2) --------------------------------------------
  //
  // An out-of-process execution backend (for example, running emitted AArch64
  // under an external AArch64 emulator) must copy guest memory in and out. These
  // methods add that capability WITHOUT changing what Read/Write/Fetch mean: a
  // backend that uses them must produce byte-identical results to one that does
  // not. They are a transport convenience, not a second memory model.
  //
  // The default implementations report "unsupported", so a port that does not
  // need transport -- and any test double -- keeps compiling unchanged. A
  // backend that needs transport must treat "unsupported" as a hard coverage
  // gap, never as an empty region.
  [[nodiscard]] virtual std::vector<Region> Regions() const { return {}; }

  virtual Status ReadRange(GuestAddr addr, GuestSize len,
                           std::vector<std::uint8_t>* out) {
    (void)addr;
    (void)len;
    (void)out;
    return StatusCode::kUnimplemented;
  }

  virtual Status WriteRange(GuestAddr addr, GuestSize len,
                            const std::uint8_t* in) {
    (void)addr;
    (void)len;
    (void)in;
    return StatusCode::kUnimplemented;
  }

  // -- Deterministic helpers ----------------------------------------------
  // Range check that performs no host access. Shared by all implementations so
  // that overflow behaviour is identical everywhere.
  //
  // Returns kInvalidArgument if [addr, addr+width) would wrap the address
  // space. The wrap check is explicit because silent wraparound in a guest
  // memory port is a sandbox escape primitive.
  [[nodiscard]] static Status CheckRangeNoWrap(GuestAddr addr,
                                               AccessWidth width) noexcept {
    if (!IsValidAccessWidth(width)) {
      return StatusCode::kInvalidArgument;
    }
    if (addr > (kMaxGuestAddr - width)) {
      return StatusCode::kInvalidArgument;
    }
    return StatusCode::kOk;
  }

  static constexpr GuestAddr kMaxGuestAddr = ~GuestAddr{0};
};

}  // namespace p5emu::memory

#endif  // P5EMU_CPU_GUEST_MEMORY_H
