// SPDX-License-Identifier: LicenseRef-P5EMU-Proprietary
//
// P5EMU source is independently authored and proprietary. See LICENSE.md and
// PROVENANCE.md before adding code here.
//
// ============================================================================
// AArch64 RUNNER
// ============================================================================
//
// Executes a generated AArch64 function against an initial state and memory
// image, and returns the final state and memory.
//
// On the x86-64 development host, generated AArch64 cannot run natively, so the
// only honest runner shells out to an INDEPENDENT AArch64 execution environment:
// qemu-aarch64 user-mode running the P5EMU test harness (a separate program, see
// aarch64/harness/). The AArch64 instruction semantics therefore come from an
// implementation P5EMU did not write.
//
// That external emulator is TEST INFRASTRUCTURE. It is not built, linked, or
// vendored into P5EMU, and P5EMU's own build never depends on it. The runner is
// what isolates that dependency to one file.
//
// A native AArch64 host would instead map the code with CodeBuffer and call it
// directly; NativeArm64Runner (below, compile-guarded) is that path. It is not
// exercised on x86-64 and is not claimed to be.
// ============================================================================

#ifndef P5EMU_CPU_ARM64_RUNNER_H
#define P5EMU_CPU_ARM64_RUNNER_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "p5emu/core/Status.h"
#include "p5emu/cpu/Arm64Abi.h"
#include "p5emu/cpu/GuestMemory.h"

namespace p5emu::cpu {

// One guest memory region for transport: a base address, a permission mask
// (guest R/W/X bits, matching memory::kPerm*), and its bytes.
struct Arm64MemoryRegion {
  memory::GuestAddr base = 0;
  std::uint32_t permissions = 0;
  std::vector<std::uint8_t> bytes;
};

// One case in a batch: emitted code, initial context, and the guest memory
// image. `regions` (if non-empty) is the exact region list to transport;
// otherwise the single region [mem_base, mem_base + mem_in.size()) is used.
struct Arm64BatchCase {
  std::vector<std::uint8_t> code;
  P5EmuArm64Ctx ctx{};
  memory::GuestAddr mem_base = 0;
  std::vector<std::uint8_t> mem_in;
  std::vector<Arm64MemoryRegion> regions;

  // The region list to execute: `regions` if set, else a single region built
  // from mem_base/mem_in (always at least one region).
  [[nodiscard]] std::vector<Arm64MemoryRegion> EffectiveRegions() const;
};

// The result of one batched case.
struct Arm64BatchResult {
  P5EmuArm64Ctx ctx{};
  std::vector<std::uint8_t> mem_out;
  std::vector<std::vector<std::uint8_t>> regions_out;
};

class Arm64Runner {
 public:
  virtual ~Arm64Runner() = default;

  [[nodiscard]] virtual const char* name() const noexcept = 0;

  // Whether the runner can actually execute. A runner that reports false makes
  // the backend report a coverage gap, never a fabricated result.
  [[nodiscard]] virtual bool available() const = 0;

  // Runs `code` with `*ctx` as the initial context; `*ctx` is overwritten with
  // the final context. `mem_in` is the initial image of the guest region
  // [mem_base, mem_base + mem_in.size()); `mem_out` receives the final image.
  virtual Status Run(const std::vector<std::uint8_t>& code, P5EmuArm64Ctx* ctx,
                     memory::GuestAddr mem_base,
                     const std::vector<std::uint8_t>& mem_in,
                     std::vector<std::uint8_t>* mem_out,
                     std::string* detail) = 0;

  // Runs `code` with a list of guest memory regions. `*regions_out` receives the
  // final bytes of each region, in the same order. The default implementation
  // routes through RunBatch (so it needs exactly one region); runners that
  // transport a region table override it.
  virtual Status RunRegions(
      const std::vector<std::uint8_t>& code, P5EmuArm64Ctx* ctx,
      const std::vector<Arm64MemoryRegion>& regions_in,
      std::vector<std::vector<std::uint8_t>>* regions_out, std::string* detail);

  // Runs many cases in one execution. The default implementation simply calls
  // Run() per case (correct, but pays process-startup per case); runners that
  // can amortise startup override it. Used by the randomized differential suite,
  // which needs thousands of cases.
  virtual Status RunBatch(const std::vector<Arm64BatchCase>& cases,
                          std::vector<Arm64BatchResult>* out,
                          std::string* detail);
};

// Runs emitted AArch64 under an external AArch64 emulator executing the P5EMU
// harness program.
class QemuArm64Runner final : public Arm64Runner {
 public:
  // `harness_path` is the cross-compiled harness binary; `emulator_path` is the
  // emulator executable (default "qemu-aarch64", resolved on PATH).
  explicit QemuArm64Runner(std::string harness_path,
                           std::string emulator_path = "qemu-aarch64");

  [[nodiscard]] const char* name() const noexcept override {
    return "qemu-aarch64";
  }
  [[nodiscard]] bool available() const override;

  Status Run(const std::vector<std::uint8_t>& code, P5EmuArm64Ctx* ctx,
             memory::GuestAddr mem_base,
             const std::vector<std::uint8_t>& mem_in,
             std::vector<std::uint8_t>* mem_out, std::string* detail) override;

  // True batching: one emulator process serves every case.
  Status RunBatch(const std::vector<Arm64BatchCase>& cases,
                  std::vector<Arm64BatchResult>* out,
                  std::string* detail) override;

 private:
  std::string harness_path_;
  std::string emulator_path_;
};

// Builds the default runner from the environment:
//   P5EMU_A64_HARNESS  path to the harness binary (required)
//   P5EMU_A64_QEMU     emulator executable (default "qemu-aarch64")
// Returns nullptr when P5EMU_A64_HARNESS is unset or the file is absent, so the
// caller treats execution as an explicit coverage gap.
std::unique_ptr<Arm64Runner> MakeArm64RunnerFromEnvironment();

}  // namespace p5emu::cpu

#endif  // P5EMU_CPU_ARM64_RUNNER_H
