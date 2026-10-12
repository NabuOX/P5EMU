// SPDX-License-Identifier: LicenseRef-P5EMU-Proprietary
//
// P5EMU source is independently authored and proprietary. See LICENSE.md and
// PROVENANCE.md before adding code here.

#include "p5emu/cpu/CodeBuffer.h"

#if defined(__unix__) || defined(__APPLE__)
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace p5emu::cpu {

namespace {
constexpr std::size_t kPageSizeFallback = 4096;

std::size_t PageSize() {
#if defined(__unix__) || defined(__APPLE__)
  const long p = ::sysconf(_SC_PAGESIZE);
  if (p > 0) {
    return static_cast<std::size_t>(p);
  }
#endif
  return kPageSizeFallback;
}

std::size_t RoundUp(std::size_t value, std::size_t align) {
  return ((value + align - 1) / align) * align;
}
}  // namespace

CodeBuffer::~CodeBuffer() { Reset(); }

bool CodeBuffer::Supported() noexcept {
#if defined(__unix__) || defined(__APPLE__)
  return true;
#else
  return false;
#endif
}

void CodeBuffer::Reset() {
#if defined(__unix__) || defined(__APPLE__)
  if (map_ != nullptr) {
    ::munmap(map_, map_size_);
  }
#endif
  map_ = nullptr;
  exec_ = nullptr;
  size_ = 0;
  map_size_ = 0;
}

bool CodeBuffer::Install(const std::vector<std::uint8_t>& code) {
  Reset();
  if (code.empty()) {
    return false;
  }

#if defined(__unix__) || defined(__APPLE__)
  const std::size_t page = PageSize();
  const std::size_t map_size = RoundUp(code.size(), page);

  // Step 1: writable mapping. Protections are RW -- NOT RWX.
  void* mem = ::mmap(nullptr, map_size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mem == MAP_FAILED) {
    return false;
  }

  auto* bytes = static_cast<std::uint8_t*>(mem);
  for (std::size_t i = 0; i < code.size(); ++i) {
    bytes[i] = code[i];
  }

  // Step 2: flip to read+execute. From here the mapping is not writable, which
  // is the whole point: there is no window in which a stray write could change
  // code that is about to run, and no page is ever both.
  if (::mprotect(mem, map_size, PROT_READ | PROT_EXEC) != 0) {
    ::munmap(mem, map_size);
    return false;
  }

  map_ = mem;
  exec_ = bytes;
  size_ = code.size();
  map_size_ = map_size;
  return true;
#else
  // No implementation on this platform. Failing cleanly is correct: allocating a
  // permanent RWX page to "make it work" would be a security regression.
  (void)code;
  return false;
#endif
}

}  // namespace p5emu::cpu
