// SPDX-License-Identifier: Apache-2.0
//
// P5EMU is an independent, open-source project. See LICENSE and
// PROVENANCE.md before adding code here.

#include "p5emu/cpu/LinearGuestMemory.h"

#include <cstring>

namespace p5emu::memory {
namespace {

// Computes the byte offset of `addr` within the region, or returns a Status.
//
// Every access path funnels through this so that bounds and permission
// behaviour cannot drift between ReadData, WriteData, FetchCode and CheckAccess.
Status Resolve(const Region& region, GuestAddr addr, AccessWidth width,
               std::uint32_t required_permissions, GuestSize* offset_out) {
  const Status range = GuestMemoryPort::CheckRangeNoWrap(addr, width);
  if (!range.ok()) {
    return range;
  }
  if (!region.Contains(addr)) {
    return StatusCode::kUnmappedAccess;
  }
  const GuestSize offset = addr - region.guest_base;

  // The access must lie wholly inside the region. `Contains` already proved the
  // first byte is inside; this proves the last.
  if ((region.size - offset) < width) {
    return StatusCode::kUnmappedAccess;
  }
  if ((region.permissions & required_permissions) != required_permissions) {
    // Distinguish "this region is not writable" from a general access violation,
    // because the two lead to different debugging conclusions: a read-only fault
    // means a permission problem, an unmapped/permission fault on a region that
    // has no read permission at all means a policy problem (e.g. fetching from
    // a data-only page).
    if (required_permissions == kPermWrite &&
        (region.permissions & kPermWrite) == 0) {
      return StatusCode::kReadOnlyAccess;
    }
    return StatusCode::kAccessViolation;
  }

  *offset_out = offset;
  return StatusCode::kOk;
}

}  // namespace

LinearGuestMemory LinearGuestMemory::MakeCodeAndData(GuestAddr base,
                                                      GuestSize size) {
  LinearGuestMemory m;
  m.region_.guest_base = base;
  m.region_.size = size;
  m.region_.permissions = kPermRead | kPermWrite | kPermExecute;
  m.region_.name = "linear-code-data";
  m.bytes_.assign(static_cast<std::size_t>(size), 0u);
  return m;
}

LinearGuestMemory LinearGuestMemory::MakeDataOnly(GuestAddr base,
                                                  GuestSize size) {
  LinearGuestMemory m;
  m.region_.guest_base = base;
  m.region_.size = size;
  m.region_.permissions = kPermRead | kPermWrite;
  m.region_.name = "linear-data-only";
  m.bytes_.assign(static_cast<std::size_t>(size), 0u);
  return m;
}

LinearGuestMemory LinearGuestMemory::MakeExecuteOnly(GuestAddr base,
                                                     GuestSize size) {
  LinearGuestMemory m;
  m.region_.guest_base = base;
  m.region_.size = size;
  m.region_.permissions = kPermExecute;
  m.region_.name = "linear-execute-only";
  m.bytes_.assign(static_cast<std::size_t>(size), 0u);
  return m;
}

Status LinearGuestMemory::ReadData(GuestAddr addr, AccessWidth width,
                                   std::uint64_t* out) {
  if (out == nullptr) {
    return StatusCode::kInvalidArgument;
  }

  GuestSize offset = 0;
  const Status st = Resolve(region_, addr, width, kPermRead, &offset);
  if (!st.ok()) {
    return st;
  }

  std::uint64_t value = 0;
  for (AccessWidth i = 0; i < width; ++i) {
    value |= static_cast<std::uint64_t>(bytes_[static_cast<std::size_t>(offset + i)])
             << (8u * i);
  }
  *out = value;
  return StatusCode::kOk;
}

Status LinearGuestMemory::WriteData(GuestAddr addr, AccessWidth width,
                                    std::uint64_t value) {
  GuestSize offset = 0;
  const Status st = Resolve(region_, addr, width, kPermWrite, &offset);
  if (!st.ok()) {
    return st;
  }

  // All bounds and permissions are proven before a single byte is written, so a
  // failed write leaves the region byte-identical.
  for (AccessWidth i = 0; i < width; ++i) {
    bytes_[static_cast<std::size_t>(offset + i)] =
        static_cast<std::uint8_t>((value >> (8u * i)) & 0xffu);
  }

  if ((region_.permissions & kPermExecute) != 0) {
    ++executable_write_notifications_;
    last_executable_write_address_ = addr;
    NotifyWriteToExecutable(addr, width);
  }

  return StatusCode::kOk;
}

Status LinearGuestMemory::FetchCode(GuestAddr addr, std::size_t length,
                                    std::uint8_t* out,
                                    std::size_t* available_out) {
  if (available_out == nullptr) {
    return StatusCode::kInvalidArgument;
  }
  *available_out = 0;
  if (out == nullptr && length != 0) {
    return StatusCode::kInvalidArgument;
  }
  if (length == 0) {
    return StatusCode::kOk;
  }

  GuestSize offset = 0;
  // kPermWrite is intentionally not required: a page may be r-x or --x.
  const Status st = Resolve(region_, addr, 1, kPermExecute, &offset);
  if (!st.ok()) {
    return st;
  }

  const GuestSize remaining = region_.size - offset;
  const std::size_t take =
      (remaining < static_cast<GuestSize>(length))
          ? static_cast<std::size_t>(remaining)
          : length;

  if (take != 0) {
    std::memcpy(out, bytes_.data() + static_cast<std::size_t>(offset), take);
  }
  *available_out = take;
  return StatusCode::kOk;
}

Status LinearGuestMemory::CheckAccess(GuestAddr addr, AccessWidth width,
                                      std::uint32_t required_permissions) {
  GuestSize offset = 0;
  return Resolve(region_, addr, width, required_permissions, &offset);
}

bool LinearGuestMemory::IsMapped(GuestAddr addr) const noexcept {
  return region_.Contains(addr);
}

std::uint32_t LinearGuestMemory::PermissionsAt(GuestAddr addr) const noexcept {
  return region_.Contains(addr) ? region_.permissions : kPermNone;
}

void LinearGuestMemory::NotifyWriteToExecutable(GuestAddr, AccessWidth) noexcept {
  // A linear region with no SMC tracking has nothing to invalidate. The hook
  // exists so that a real port can, and so that tests can observe the call.
}

DirectAccessStatus LinearGuestMemory::QueryDirectAccess(
    GuestAddr addr, GuestSize length, std::uint64_t* host_base_out) const {
  if (host_base_out == nullptr) {
    return DirectAccessStatus::kUnmapped;
  }
  if (length == 0) {
    *host_base_out = 0;
    return DirectAccessStatus::kOk;
  }

  // Overflow of [addr, addr+length) makes the question meaningless.
  if (addr > (kMaxGuestAddr - length)) {
    return DirectAccessStatus::kUnmapped;
  }
  if (!region_.Contains(addr)) {
    return DirectAccessStatus::kUnmapped;
  }

  const GuestSize offset = addr - region_.guest_base;
  if ((region_.size - offset) < length) {
    // A span that leaves the single region cannot be one contiguous access.
    // A segmented port could still answer kOk by composing; this port does not.
    return DirectAccessStatus::kCrossesRegion;
  }
  if ((region_.permissions & kPermExecute) == 0) {
    return DirectAccessStatus::kNoExecute;
  }

  // This port's backing store is a host vector, so a direct host range exists
  // for every executable span. A non-identity port returns kNotIdentityMapped
  // here instead, and backends must fall back -- see the header.
  *host_base_out = reinterpret_cast<std::uint64_t>(bytes_.data()) + offset;
  return DirectAccessStatus::kOk;
}

Status LinearGuestMemory::DebugFill(GuestAddr addr, const void* source,
                                    std::size_t length) {
  if (source == nullptr && length != 0) {
    return StatusCode::kInvalidArgument;
  }
  // A bulk fill is not a single access, so it must NOT be validated as one. The
  // access-width check accepts only 1/2/4/8 bytes; using it here rejected every
  // seed image larger than eight bytes, which silently prevented the
  // differential harness from seeding a realistic memory image.
  if (length != 0 && addr > (GuestMemoryPort::kMaxGuestAddr - length)) {
    return StatusCode::kInvalidArgument;
  }
  if (!region_.Contains(addr)) {
    return StatusCode::kUnmappedAccess;
  }
  const GuestSize offset = addr - region_.guest_base;
  if ((region_.size - offset) < length) {
    return StatusCode::kUnmappedAccess;
  }
  if (length != 0) {
    std::memcpy(bytes_.data() + static_cast<std::size_t>(offset), source, length);
  }
  return StatusCode::kOk;
}

Status LinearGuestMemory::DebugFillZero(GuestAddr addr, std::size_t length) {
  if (length == 0) {
    return StatusCode::kOk;
  }
  GuestSize offset = 0;
  if (!region_.Contains(addr)) {
    return StatusCode::kUnmappedAccess;
  }
  offset = addr - region_.guest_base;
  if ((region_.size - offset) < length) {
    return StatusCode::kUnmappedAccess;
  }
  std::memset(bytes_.data() + static_cast<std::size_t>(offset), 0, length);
  return StatusCode::kOk;
}

Status LinearGuestMemory::DebugPeek(GuestAddr addr, void* dest,
                                    std::size_t length) {
  if (dest == nullptr && length != 0) {
    return StatusCode::kInvalidArgument;
  }
  if (length == 0) {
    return StatusCode::kOk;
  }
  if (!region_.Contains(addr)) {
    return StatusCode::kUnmappedAccess;
  }
  const GuestSize offset = addr - region_.guest_base;
  if ((region_.size - offset) < length) {
    return StatusCode::kUnmappedAccess;
  }
  std::memcpy(dest, bytes_.data() + static_cast<std::size_t>(offset), length);
  return StatusCode::kOk;
}

std::vector<Region> LinearGuestMemory::Regions() const {
  std::vector<Region> regions;
  if (region_.size != 0 && !bytes_.empty()) {
    regions.push_back(region_);
  }
  return regions;
}

Status LinearGuestMemory::ReadRange(GuestAddr addr, GuestSize len,
                                    std::vector<std::uint8_t>* out) {
  if (out == nullptr) {
    return StatusCode::kInvalidArgument;
  }
  out->clear();
  if (len == 0) {
    return StatusCode::kOk;
  }
  if (!region_.Contains(addr)) {
    return StatusCode::kUnmappedAccess;
  }
  const GuestSize offset = addr - region_.guest_base;
  if ((region_.size - offset) < len) {
    return StatusCode::kUnmappedAccess;
  }
  out->assign(bytes_.begin() + static_cast<std::ptrdiff_t>(offset),
              bytes_.begin() + static_cast<std::ptrdiff_t>(offset + len));
  return StatusCode::kOk;
}

Status LinearGuestMemory::WriteRange(GuestAddr addr, GuestSize len,
                                     const std::uint8_t* in) {
  if (in == nullptr && len != 0) {
    return StatusCode::kInvalidArgument;
  }
  if (len == 0) {
    return StatusCode::kOk;
  }
  if (!region_.Contains(addr)) {
    return StatusCode::kUnmappedAccess;
  }
  const GuestSize offset = addr - region_.guest_base;
  if ((region_.size - offset) < len) {
    return StatusCode::kUnmappedAccess;
  }
  std::memcpy(bytes_.data() + static_cast<std::size_t>(offset), in,
              static_cast<std::size_t>(len));
  return StatusCode::kOk;
}

}  // namespace p5emu::memory
