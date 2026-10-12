// SPDX-License-Identifier: LicenseRef-P5EMU-Proprietary
//
// P5EMU source is independently authored and proprietary. See LICENSE.md and
// PROVENANCE.md before adding code here.

#ifndef P5EMU_CPU_LINEAR_GUEST_MEMORY_H
#define P5EMU_CPU_LINEAR_GUEST_MEMORY_H

#include <cstdint>
#include <vector>

#include "p5emu/cpu/GuestMemory.h"

namespace p5emu::memory {

// ---------------------------------------------------------------------------
// LinearGuestMemory
// ---------------------------------------------------------------------------
// A contiguous, single-allocation GuestMemoryPort for tests and Phase-1
// execution.
//
// It implements the *contract* of GuestMemoryPort faithfully -- bounded access,
// atomic all-or-nothing failure, separate code/data permissions, deterministic
// Status, SMC notification -- so that tests written against it exercise the same
// behaviour a future real port must provide.
//
// It deliberately does NOT model segmentation, page tables, demand paging, or
// any target memory map. It also offers a direct-access fast path, so tests can
// verify that code using the fast path and code using ReadData/WriteData agree.
//
class LinearGuestMemory final : public GuestMemoryPort {
 public:
  // A read/execute data region: readable, writable, and fetchable. This is the
  // shape most test fixtures need.
  static LinearGuestMemory MakeCodeAndData(GuestAddr base, GuestSize size);

  // A read/write data region that is NOT fetchable, so NX behaviour is testable.
  static LinearGuestMemory MakeDataOnly(GuestAddr base, GuestSize size);

  // A region that is fetchable but not readable or writable (execute-only).
  static LinearGuestMemory MakeExecuteOnly(GuestAddr base, GuestSize size);

  Status ReadData(GuestAddr addr, AccessWidth width,
                  std::uint64_t* out) override;
  Status WriteData(GuestAddr addr, AccessWidth width,
                   std::uint64_t value) override;
  Status FetchCode(GuestAddr addr, std::size_t length, std::uint8_t* out,
                   std::size_t* available_out) override;
  Status CheckAccess(GuestAddr addr, AccessWidth width,
                     std::uint32_t required_permissions) override;
  [[nodiscard]] bool IsMapped(GuestAddr addr) const noexcept override;
  [[nodiscard]] std::uint32_t PermissionsAt(GuestAddr addr) const noexcept override;
  void NotifyWriteToExecutable(GuestAddr addr,
                               AccessWidth width) noexcept override;
  DirectAccessStatus QueryDirectAccess(GuestAddr addr, GuestSize length,
                                       std::uint64_t* host_base_out) const override;

  // Bulk transport for out-of-process backends. This port has exactly one region.
  [[nodiscard]] std::vector<Region> Regions() const override;
  Status ReadRange(GuestAddr addr, GuestSize len,
                   std::vector<std::uint8_t>* out) override;
  Status WriteRange(GuestAddr addr, GuestSize len,
                    const std::uint8_t* in) override;

  // -- Test/introspection surface -----------------------------------------
  // Bytes successfully written to an executable region since the last reset.
  // Used to assert that SMC notification actually fires.
  [[nodiscard]] std::uint64_t ExecutableWriteNotifications() const noexcept {
    return executable_write_notifications_;
  }
  void ResetExecutableWriteNotifications() noexcept {
    executable_write_notifications_ = 0;
  }
  [[nodiscard]] GuestAddr LastExecutableWriteAddress() const noexcept {
    return last_executable_write_address_;
  }

  [[nodiscard]] GuestAddr Base() const noexcept { return region_.guest_base; }
  [[nodiscard]] GuestSize Size() const noexcept { return region_.size; }

  // Host-side initialisation, for fixture setup only. Not part of the port
  // interface. Fails if the span is out of range.
  Status DebugFill(GuestAddr addr, const void* source, std::size_t length);
  Status DebugFillZero(GuestAddr addr, std::size_t length);

  // Reads raw bytes for fixture assertions, bypassing permission checks the way
  // a debugger would. Not part of the port interface.
  Status DebugPeek(GuestAddr addr, void* dest, std::size_t length);

  // Default-constructs an EMPTY port: no region, zero bytes.
  //
  // Every access on it fails with kUnmappedAccess, which is a well-defined and
  // safe state rather than an uninitialised one. This makes a default-constructed
  // port safe to declare and to pass around before it is configured.
  LinearGuestMemory() = default;

  // Copying is forbidden: two ports sharing one backing store would silently
  // defeat the differential harness's isolation guarantee. Moving is allowed,
  // because the factories return by value.
  LinearGuestMemory(const LinearGuestMemory&) = delete;
  LinearGuestMemory& operator=(const LinearGuestMemory&) = delete;

  LinearGuestMemory(LinearGuestMemory&&) noexcept = default;
  LinearGuestMemory& operator=(LinearGuestMemory&&) noexcept = default;

  ~LinearGuestMemory() override = default;

 private:
  Region region_{};
  std::vector<std::uint8_t> bytes_;

  std::uint64_t executable_write_notifications_ = 0;
  GuestAddr last_executable_write_address_ = 0;
};

}  // namespace p5emu::memory

#endif  // P5EMU_CPU_LINEAR_GUEST_MEMORY_H
