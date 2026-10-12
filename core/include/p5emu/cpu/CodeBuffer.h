// SPDX-License-Identifier: LicenseRef-P5EMU-Proprietary
//
// P5EMU source is independently authored and proprietary. See LICENSE.md and
// PROVENANCE.md before adding code here.
//
// ============================================================================
// CODE BUFFER
// ============================================================================
//
// A platform-neutral abstraction for a region of generated machine code.
//
// It separates the two things a code cache must keep distinct:
//
//   * a WRITABLE generation buffer -- bytes are produced here, then
//   * an EXECUTABLE mapping -- the same bytes, made executable once complete.
//
// W^X POLICY
//
// The mapping is never simultaneously writable and executable. Install() maps
// the region writable, copies the code in, then flips it to read+execute with
// mprotect. There is no RWX window. On Apple platforms the same interface would
// be implemented with two address views (a writable alias and an executable
// alias) or MAP_JIT; this class is the seam that makes that replacement a
// change to one file rather than to the backend. Apple JIT is NOT implemented
// in Phase 2.
//
// The AArch64 that Install() holds is NOT executed by this process on the
// development host (x86-64). Execution happens through an independent AArch64
// environment; see Arm64Runner.h. This class is still exercised on the host to
// prove the mapping mechanics.
// ============================================================================

#ifndef P5EMU_CPU_CODE_BUFFER_H
#define P5EMU_CPU_CODE_BUFFER_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace p5emu::cpu {

class CodeBuffer {
 public:
  CodeBuffer() = default;
  ~CodeBuffer();

  CodeBuffer(const CodeBuffer&) = delete;
  CodeBuffer& operator=(const CodeBuffer&) = delete;

  // Copies `code` into a fresh mapping and makes it read+execute.
  //
  // Returns false if the platform has no implementation, the code is empty, or
  // mapping/protection fails. On failure the buffer is left empty.
  bool Install(const std::vector<std::uint8_t>& code);

  // Read-only view of the executable bytes. Empty before a successful Install.
  [[nodiscard]] const std::uint8_t* data() const noexcept { return exec_; }
  [[nodiscard]] std::size_t size() const noexcept { return size_; }

  // Callable entry address, or nullptr. Interpreting it as a function pointer is
  // the caller's responsibility and is host-architecture specific.
  [[nodiscard]] void* entry() const noexcept { return exec_; }

  [[nodiscard]] bool empty() const noexcept { return exec_ == nullptr; }

  // Whether this host has a real W^X implementation. False on platforms without
  // one, where Install() fails cleanly rather than creating a RWX mapping.
  [[nodiscard]] static bool Supported() noexcept;

 private:
  void Reset();

  void* map_ = nullptr;
  std::uint8_t* exec_ = nullptr;
  std::size_t size_ = 0;
  std::size_t map_size_ = 0;
};

}  // namespace p5emu::cpu

#endif  // P5EMU_CPU_CODE_BUFFER_H
