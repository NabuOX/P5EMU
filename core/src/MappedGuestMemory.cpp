// SPDX-License-Identifier: Apache-2.0
//
// P5EMU is an independent, open-source project. See LICENSE and
// PROVENANCE.md before adding code here.

#include "p5emu/cpu/MappedGuestMemory.h"

namespace p5emu::memory {

MappedGuestMemory::RegionData* MappedGuestMemory::Find(GuestAddr addr,
                                                       GuestSize width) noexcept {
  for (RegionData& r : regions_) {
    if (addr >= r.base && (addr - r.base) <= r.size &&
        width <= (r.size - (addr - r.base))) {
      return &r;
    }
  }
  return nullptr;
}

const MappedGuestMemory::RegionData* MappedGuestMemory::Find(
    GuestAddr addr, GuestSize width) const noexcept {
  for (const RegionData& r : regions_) {
    if (addr >= r.base && (addr - r.base) <= r.size &&
        width <= (r.size - (addr - r.base))) {
      return &r;
    }
  }
  return nullptr;
}

Status MappedGuestMemory::AddRegion(const RegionSpec& spec) {
  if (spec.size == 0) {
    return StatusCode::kInvalidArgument;
  }
  // Defensive bound: refuse a region large enough to be a denial-of-service
  // allocation, independent of the loader's own plan-level limit.
  constexpr GuestSize kMaxRegionBytes = 1ull << 28;
  if (spec.size > kMaxRegionBytes) {
    return StatusCode::kImageTooLarge;
  }
  // base + size must not wrap the guest address space.
  if (spec.base > (GuestMemoryPort::kMaxGuestAddr - spec.size)) {
    return StatusCode::kIntegerOverflow;
  }
  for (const RegionData& r : regions_) {
    const GuestAddr a_end = spec.base + spec.size;
    const GuestAddr b_end = r.base + r.size;
    if (spec.base < b_end && r.base < a_end) {
      return StatusCode::kOverlappingMapping;
    }
  }
  RegionData rd;
  rd.base = spec.base;
  rd.size = spec.size;
  rd.permissions = spec.permissions;
  rd.bytes.assign(static_cast<std::size_t>(spec.size), 0);
  regions_.push_back(std::move(rd));
  return StatusCode::kOk;
}

Status MappedGuestMemory::WriteImage(GuestAddr addr, const std::uint8_t* src,
                                     std::uint64_t len) {
  if (src == nullptr && len != 0) {
    return StatusCode::kInvalidArgument;
  }
  RegionData* r = Find(addr, len);
  if (r == nullptr) {
    return StatusCode::kUnmappedAccess;
  }
  const GuestSize off = addr - r->base;
  for (std::uint64_t i = 0; i < len; ++i) {
    r->bytes[static_cast<std::size_t>(off + i)] = src[i];
  }
  return StatusCode::kOk;
}

Status MappedGuestMemory::ZeroFill(GuestAddr addr, GuestSize len) {
  RegionData* r = Find(addr, len);
  if (r == nullptr) {
    return StatusCode::kUnmappedAccess;
  }
  const GuestSize off = addr - r->base;
  for (GuestSize i = 0; i < len; ++i) {
    r->bytes[static_cast<std::size_t>(off + i)] = 0;
  }
  return StatusCode::kOk;
}

Status MappedGuestMemory::ReadData(GuestAddr addr, AccessWidth width,
                                   std::uint64_t* out) {
  if (out == nullptr) return StatusCode::kInvalidArgument;
  if (!IsValidAccessWidth(width)) return StatusCode::kInvalidArgument;
  const Status wrap = GuestMemoryPort::CheckRangeNoWrap(addr, width);
  if (!wrap.ok()) return wrap;
  const RegionData* r = Find(addr, width);
  if (r == nullptr) return StatusCode::kUnmappedAccess;
  if ((r->permissions & kPermRead) == 0) return StatusCode::kAccessViolation;
  const GuestSize off = addr - r->base;
  std::uint64_t v = 0;
  for (AccessWidth i = 0; i < width; ++i) {
    v |= static_cast<std::uint64_t>(r->bytes[static_cast<std::size_t>(off + i)])
         << (8 * i);
  }
  *out = v;
  return StatusCode::kOk;
}

Status MappedGuestMemory::WriteData(GuestAddr addr, AccessWidth width,
                                    std::uint64_t value) {
  if (!IsValidAccessWidth(width)) return StatusCode::kInvalidArgument;
  const Status wrap = GuestMemoryPort::CheckRangeNoWrap(addr, width);
  if (!wrap.ok()) return wrap;
  RegionData* r = Find(addr, width);
  if (r == nullptr) return StatusCode::kUnmappedAccess;
  if ((r->permissions & kPermWrite) == 0) return StatusCode::kReadOnlyAccess;
  const GuestSize off = addr - r->base;
  for (AccessWidth i = 0; i < width; ++i) {
    r->bytes[static_cast<std::size_t>(off + i)] =
        static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFu);
  }
  if ((r->permissions & kPermExecute) != 0) {
    NotifyWriteToExecutable(addr, width);
  }
  return StatusCode::kOk;
}

Status MappedGuestMemory::FetchCode(GuestAddr addr, std::size_t length,
                                    std::uint8_t* out,
                                    std::size_t* available_out) {
  if (available_out != nullptr) *available_out = 0;
  if (out == nullptr && length != 0) return StatusCode::kInvalidArgument;
  const RegionData* r = Find(addr, 0);
  if (r == nullptr) return StatusCode::kUnmappedAccess;
  if ((r->permissions & kPermExecute) == 0) return StatusCode::kAccessViolation;
  const GuestSize off = addr - r->base;
  const GuestSize remaining = r->size - off;
  const std::size_t avail =
      (remaining > length) ? length : static_cast<std::size_t>(remaining);
  for (std::size_t i = 0; i < avail; ++i) {
    out[i] = r->bytes[static_cast<std::size_t>(off) + i];
  }
  if (available_out != nullptr) *available_out = avail;
  return StatusCode::kOk;
}

Status MappedGuestMemory::CheckAccess(GuestAddr addr, AccessWidth width,
                                      std::uint32_t required_permissions) {
  if (!IsValidAccessWidth(width)) return StatusCode::kInvalidArgument;
  const Status wrap = GuestMemoryPort::CheckRangeNoWrap(addr, width);
  if (!wrap.ok()) return wrap;
  const RegionData* r = Find(addr, width);
  if (r == nullptr) return StatusCode::kUnmappedAccess;
  if ((r->permissions & required_permissions) != required_permissions) {
    return StatusCode::kAccessViolation;
  }
  return StatusCode::kOk;
}

bool MappedGuestMemory::IsMapped(GuestAddr addr) const noexcept {
  return Find(addr, 0) != nullptr;
}

std::uint32_t MappedGuestMemory::PermissionsAt(GuestAddr addr) const noexcept {
  const RegionData* r = Find(addr, 0);
  return (r != nullptr) ? r->permissions : kPermNone;
}

void MappedGuestMemory::NotifyWriteToExecutable(GuestAddr addr,
                                                AccessWidth width) noexcept {
  (void)addr;
  (void)width;
  ++executable_write_notifications_;
}

DirectAccessStatus MappedGuestMemory::QueryDirectAccess(
    GuestAddr addr, GuestSize length, std::uint64_t* host_base_out) const {
  (void)addr;
  (void)length;
  (void)host_base_out;
  // No direct-access fast path: the loader port is not identity-mapped and does
  // not need one in Phase 4.
  return DirectAccessStatus::kNoDirectAccess;
}

std::vector<Region> MappedGuestMemory::Regions() const {
  std::vector<Region> out;
  out.reserve(regions_.size());
  for (const RegionData& r : regions_) {
    Region reg;
    reg.guest_base = r.base;
    reg.size = r.size;
    reg.permissions = r.permissions;
    reg.name = "mapped";
    out.push_back(reg);
  }
  return out;
}

Status MappedGuestMemory::ReadRange(GuestAddr addr, GuestSize len,
                                    std::vector<std::uint8_t>* out) {
  if (out == nullptr) return StatusCode::kInvalidArgument;
  out->clear();
  if (len == 0) return StatusCode::kOk;
  const RegionData* r = Find(addr, len);
  if (r == nullptr) return StatusCode::kUnmappedAccess;
  const GuestSize off = addr - r->base;
  out->assign(r->bytes.begin() + static_cast<std::ptrdiff_t>(off),
              r->bytes.begin() + static_cast<std::ptrdiff_t>(off + len));
  return StatusCode::kOk;
}

Status MappedGuestMemory::WriteRange(GuestAddr addr, GuestSize len,
                                     const std::uint8_t* in) {
  if (in == nullptr && len != 0) return StatusCode::kInvalidArgument;
  if (len == 0) return StatusCode::kOk;
  RegionData* r = Find(addr, len);
  if (r == nullptr) return StatusCode::kUnmappedAccess;
  const GuestSize off = addr - r->base;
  for (GuestSize i = 0; i < len; ++i) {
    r->bytes[static_cast<std::size_t>(off + i)] = in[i];
  }
  return StatusCode::kOk;
}

}  // namespace p5emu::memory
