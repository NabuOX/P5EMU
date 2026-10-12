// SPDX-License-Identifier: LicenseRef-P5EMU-Proprietary
//
// P5EMU source is independently authored and proprietary. See LICENSE.md and
// PROVENANCE.md before adding code here.
//
// ============================================================================
// HLE SUBSYSTEM
// ============================================================================
//
// High-Level Emulation: guest imports resolved to host implementations.
//
//   ImportRequirement -> HleResolver -> HleRegistry entry
//     -> HleBinding (a deterministic GUEST trampoline address)
//     -> guest CALL reaches the trampoline
//     -> HleDispatcher -> HleFunction(HleCallContext)
//     -> guest return value/state -> guest resumes
//
// ARCHITECTURAL BOUNDARIES:
//
//   * No raw host function pointer ever appears in guest-visible state. A
//     binding is a guest trampoline ADDRESS in a P5EMU-owned synthetic range
//     (TEST/INTERNAL -- NOT a PS5 address). The dispatcher maps address ->
//     implementation.
//   * HLE functions receive a narrow HleCallContext, never the emulator.
//   * The HLE subsystem implements NO Sony library or function. All functions
//     here are P5EMU-owned synthetic ones; real PS5 identities stay UNKNOWN
//     until independently proven.
//   * HLE functions may read/write guest memory and set the return value, but
//     may NOT execute guest code. The context exposes no such operation, so
//     reentrancy is structurally impossible; the dispatcher also rejects a
//     nested dispatch explicitly.
// ============================================================================

#ifndef P5EMU_HLE_HLE_H
#define P5EMU_HLE_HLE_H

#include <cstdint>
#include <string>
#include <vector>

#include "p5emu/core/Status.h"
#include "p5emu/cpu/ExternalCall.h"
#include "p5emu/cpu/GuestMemory.h"
#include "p5emu/cpu/ReferenceInterpreter.h"
#include "p5emu/loader/Import.h"

namespace p5emu::hle {

// The HLE outcome. Distinct states, never one generic failure.
enum class HleStatus : std::uint8_t {
  kOk = 0,
  kNotFound,
  kAmbiguous,
  kUnsupportedIdentity,
  kInvalidArgument,
  kMemoryFault,
  kDispatchFault,
  kInternalError,
};

const char* HleStatusName(HleStatus status) noexcept;

// Maps an HleStatus onto the emulator's Status model.
StatusCode StatusCodeForHle(HleStatus status) noexcept;

// ---------------------------------------------------------------------------
// HleCallContext -- the narrow interface an HLE function receives.
// ---------------------------------------------------------------------------
//
// It exposes exactly what a function needs and nothing more: the guest ABI
// arguments, a checked view of guest memory, and the return-value setter.
// There is deliberately NO way to execute guest code (reentrancy boundary).
class HleCallContext {
 public:
  HleCallContext(cpu::CpuState* state, memory::GuestMemoryPort* memory)
      : state_(state), memory_(memory) {}

  // Number of integer argument registers in the guest ABI (AMD64 System V:
  // rdi, rsi, rdx, rcx, r8, r9).
  static constexpr std::size_t kArgRegisterCount = 6;

  // Reads guest ABI argument `index` (0-based). Out-of-range returns 0.
  [[nodiscard]] std::uint64_t Arg(std::size_t index) const;

  // Sets the guest return register (RAX).
  void SetReturn(std::uint64_t value) noexcept;

  // Checked guest memory access. All access goes through GuestMemoryPort.
  Status ReadGuest(memory::GuestAddr addr, memory::AccessWidth width,
                   std::uint64_t* out);
  Status WriteGuest(memory::GuestAddr addr, memory::AccessWidth width,
                    std::uint64_t value);
  Status ReadBytes(memory::GuestAddr addr, std::uint64_t length,
                   std::vector<std::uint8_t>* out);

  [[nodiscard]] cpu::CpuState* state() const noexcept { return state_; }

  // TEST-ONLY injection knobs. They model two deliberate ABI defects so the
  // tests can prove they are caught. Never set in production paths.
  void SetTestInjections(bool swap_first_two_args,
                         bool wrong_return_register) noexcept {
    swap_first_two_args_ = swap_first_two_args;
    wrong_return_register_ = wrong_return_register;
  }

 private:
  cpu::CpuState* state_;
  memory::GuestMemoryPort* memory_;
  bool swap_first_two_args_ = false;
  bool wrong_return_register_ = false;
};

// TEST-ONLY injection options for the dispatcher.
struct HleInjectOptions {
  bool swap_first_two_args = false;         // swap guest arg registers 0 and 1
  bool wrong_return_register = false;       // write the return to RSI not RAX
  bool unknown_binding_as_success = false;  // treat an unknown binding as kOk
};

// An HLE implementation. It receives only the narrow context.
using HleFunction = HleStatus (*)(HleCallContext&);

// One registered binding.
struct HleEntry {
  std::string module;
  loader::SymbolIdentity symbol;
  HleFunction fn = nullptr;
  std::uint64_t trampoline = 0;
};

// ---------------------------------------------------------------------------
// HleRegistry
// ---------------------------------------------------------------------------
// Assigns each registration a deterministic guest trampoline address. Duplicate
// (module, symbol) registrations are NOT silently last-wins: lookup reports
// kAmbiguous.
class HleRegistry {
 public:
  // TEST/INTERNAL P5EMU range. NOT a PS5 system-module address.
  static constexpr std::uint64_t kTrampolineBase = 0x70000000ull;
  static constexpr std::uint64_t kTrampolineStride = 8ull;
  static constexpr std::uint64_t kTrampolineEnd = 0x70010000ull;

  // Registers a binding; returns its trampoline address, or 0 if the registry
  // is full.
  std::uint64_t Register(const std::string& module,
                         const loader::SymbolIdentity& symbol, HleFunction fn);

  // Finds the single binding for (module, symbol). Sets *status to kOk on a
  // unique match, kNotFound when absent, kAmbiguous when more than one exists.
  // Returns nullptr unless kOk.
  const HleEntry* Find(const std::string& module,
                       const loader::SymbolIdentity& symbol,
                       HleStatus* status) const;

  // Finds the binding owning a trampoline address.
  const HleEntry* FindByTrampoline(std::uint64_t address) const;

  [[nodiscard]] std::size_t Size() const noexcept { return entries_.size(); }
  [[nodiscard]] const std::vector<HleEntry>& entries() const noexcept {
    return entries_;
  }

 private:
  std::vector<HleEntry> entries_;
};

// ---------------------------------------------------------------------------
// HleDispatcher
// ---------------------------------------------------------------------------
// The ExternalCallHandler the CPU core calls at an kExternalCall. It maps a
// guest binding address to a registered implementation and invokes it.
class HleDispatcher final : public cpu::ExternalCallHandler {
 public:
  struct TraceRecord {
    std::uint64_t binding = 0;
    std::string module;
    std::string symbol;
    HleStatus status = HleStatus::kOk;
    std::uint64_t result = 0;
  };

  explicit HleDispatcher(const HleRegistry* registry) : registry_(registry) {}

  Status HandleExternalCall(std::uint64_t binding_address,
                                 cpu::CpuState* state,
                                 memory::GuestMemoryPort* memory) override;

  // Optional trace. Disabled by default; enabling it never changes behaviour.
  void EnableTrace(bool enabled) noexcept { trace_enabled_ = enabled; }
  [[nodiscard]] const std::vector<TraceRecord>& trace() const noexcept {
    return trace_;
  }
  void ClearTrace() { trace_.clear(); }

  // TEST-ONLY.
  void SetInjections(const HleInjectOptions& options) noexcept {
    inject_ = options;
  }

 private:
  const HleRegistry* registry_;
  bool trace_enabled_ = false;
  bool active_ = false;  // reentrancy guard
  std::vector<TraceRecord> trace_;
  HleInjectOptions inject_;
};

// ---------------------------------------------------------------------------
// HleResolver
// ---------------------------------------------------------------------------
// Bridges the Phase-5 import resolver to the registry: an import resolves to
// the binding's guest trampoline address, never a host pointer.
class HleResolver final : public loader::ImportResolver {
 public:
  explicit HleResolver(const HleRegistry* registry) : registry_(registry) {}

  loader::ResolutionResult ResolveSymbol(
      const std::string& module,
      const loader::SymbolIdentity& symbol) override;

 private:
  const HleRegistry* registry_;
};

}  // namespace p5emu::hle

#endif  // P5EMU_HLE_HLE_H
