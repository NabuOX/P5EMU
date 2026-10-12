// SPDX-License-Identifier: LicenseRef-P5EMU-Proprietary
//
// P5EMU source is independently authored and proprietary. See LICENSE.md and
// PROVENANCE.md before adding code here.
//
// ============================================================================
// MAPPED GUEST MEMORY
// ============================================================================
//
// A multi-region GuestMemoryPort whose regions carry INDEPENDENT R/W/X
// permissions. This is the loader's apply target: unlike the single-region
// LinearGuestMemory, it can host a read+execute text segment and a read+write
// data segment with different permissions, and it ENFORCES them on guest
// accesses. There is no blanket RWX model.
//
// The loader populates regions through WriteImage/ZeroFill, which are NOT part
// of GuestMemoryPort and intentionally bypass the region's final permissions --
// a loader must be able to fill a read-only text segment before protections
// apply. Guest WriteData enforces the declared permissions.
// ============================================================================

#ifndef P5EMU_CPU_MAPPED_GUEST_MEMORY_H
#define P5EMU_CPU_MAPPED_GUEST_MEMORY_H

#include <cstdint>
#include <vector>

#include "p5emu/cpu/GuestMemory.h"

namespace p5emu::memory {

class MappedGuestMemory final : public GuestMemoryPort {
 public:
  struct RegionSpec {
    GuestAddr base = 0;
    GuestSize size = 0;
    Permissions permissions = kPermNone;
  };

  MappedGuestMemory() = default;

  // Adds a zero-filled region. Fails with kOverlappingMapping if it overlaps an
  // existing region (overlap policy: no overlapping regions).
  Status AddRegion(const RegionSpec& spec);

  // Loader-only writes that bypass the region's declared permissions. The
  // region MUST exist and the range must lie inside it.
  Status WriteImage(GuestAddr addr, const std::uint8_t* src, std::uint64_t len);
  Status ZeroFill(GuestAddr addr, GuestSize len);

  // -- GuestMemoryPort ------------------------------------------------------
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

  [[nodiscard]] std::vector<Region> Regions() const override;
  Status ReadRange(GuestAddr addr, GuestSize len,
                   std::vector<std::uint8_t>* out) override;
  Status WriteRange(GuestAddr addr, GuestSize len,
                    const std::uint8_t* in) override;

  [[nodiscard]] std::size_t RegionCount() const noexcept {
    return regions_.size();
  }
  [[nodiscard]] std::uint64_t ExecutableWriteNotifications() const noexcept {
    return executable_write_notifications_;
  }

  // Copying is forbidden (shared backing store); moving is allowed so an
  // apply can build a staging port and swap it in on success.
  MappedGuestMemory(const MappedGuestMemory&) = delete;
  MappedGuestMemory& operator=(const MappedGuestMemory&) = delete;
  MappedGuestMemory(MappedGuestMemory&&) noexcept = default;
  MappedGuestMemory& operator=(MappedGuestMemory&&) noexcept = default;
  ~MappedGuestMemory() override = default;

 private:
  struct RegionData {
    GuestAddr base = 0;
    GuestSize size = 0;
    Permissions permissions = kPermNone;
    std::vector<std::uint8_t> bytes;
  };

  // Region containing [addr, addr+width) or nullptr. Width need not be a valid
  // access width here; callers validate that first.
  [[nodiscard]] RegionData* Find(GuestAddr addr, GuestSize width) noexcept;
  [[nodiscard]] const RegionData* Find(GuestAddr addr,
                                       GuestSize width) const noexcept;

  std::vector<RegionData> regions_;
  std::uint64_t executable_write_notifications_ = 0;
};

}  // namespace p5emu::memory

#endif  // P5EMU_CPU_MAPPED_GUEST_MEMORY_H
