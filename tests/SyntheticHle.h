// SPDX-License-Identifier: LicenseRef-P5EMU-Proprietary
//
// P5EMU source is independently authored and proprietary. See LICENSE.md and
// PROVENANCE.md before adding code here.
//
// ============================================================================
// SYNTHETIC P5EMU-OWNED HLE FUNCTIONS
// ============================================================================
//
// TEST-ONLY HLE implementations. They are P5EMU-owned identities (libp5emu_test),
// not Sony libraries. The out-of-process AArch64 harness mirrors these exact
// functions so both backends observe the same guest-visible behaviour.
//
// Registration order matches the harness: index 0 p5emu_add, 1 p5emu_sum6, 2
// p5emu_memory.
// ============================================================================

#ifndef P5EMU_TESTS_SYNTHETIC_HLE_H
#define P5EMU_TESTS_SYNTHETIC_HLE_H

#include <cstdint>

#include "p5emu/hle/Hle.h"

namespace p5emu::test {

inline hle::HleStatus P5emuAdd(hle::HleCallContext& c) {
  c.SetReturn(c.Arg(0) + c.Arg(1));
  return hle::HleStatus::kOk;
}

inline hle::HleStatus P5emuSub(hle::HleCallContext& c) {
  c.SetReturn(c.Arg(0) - c.Arg(1));
  return hle::HleStatus::kOk;
}

inline hle::HleStatus P5emuSum6(hle::HleCallContext& c) {
  std::uint64_t sum = 0;
  for (std::size_t i = 0; i < hle::HleCallContext::kArgRegisterCount; ++i) {
    sum += c.Arg(i);
  }
  c.SetReturn(sum);
  return hle::HleStatus::kOk;
}

// Sums ten arguments: six in registers and four on the guest stack. Exercises
// the register-vs-stack split of the guest ABI.
inline hle::HleStatus P5emuSum10(hle::HleCallContext& c) {
  std::uint64_t sum = 0;
  for (std::size_t i = 0; i < 10; ++i) {
    sum += c.Arg(i);
  }
  c.SetReturn(sum);
  return hle::HleStatus::kOk;
}

// p5emu_memory(ptr, add, out_ptr): *out = *ptr + add; return *out.
inline hle::HleStatus P5emuMemory(hle::HleCallContext& c) {
  std::uint64_t value = 0;
  if (!c.ReadGuest(c.Arg(0), 8, &value).ok()) {
    return hle::HleStatus::kMemoryFault;
  }
  value += c.Arg(1);
  if (!c.WriteGuest(c.Arg(2), 8, value).ok()) {
    return hle::HleStatus::kMemoryFault;
  }
  c.SetReturn(value);
  return hle::HleStatus::kOk;
}

inline constexpr const char* kTestModule = "libp5emu_test";

// Registers the canonical order the AArch64 harness mirrors.
inline void RegisterCanonical(hle::HleRegistry* reg) {
  reg->Register(kTestModule, loader::SymbolIdentity::String("p5emu_add"),
                &P5emuAdd);
  reg->Register(kTestModule, loader::SymbolIdentity::String("p5emu_sum6"),
                &P5emuSum6);
  reg->Register(kTestModule, loader::SymbolIdentity::String("p5emu_memory"),
                &P5emuMemory);
  reg->Register(kTestModule, loader::SymbolIdentity::String("p5emu_sum10"),
                &P5emuSum10);
}

}  // namespace p5emu::test

#endif  // P5EMU_TESTS_SYNTHETIC_HLE_H
