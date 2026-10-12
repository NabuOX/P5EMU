// SPDX-License-Identifier: Apache-2.0
//
// Contract tests for the CPU model.
//
// These are not trivia. CpuState is the object the interpreter mutates and the
// differential harness compares field by field; the termination-reason and
// backend-id strings appear verbatim in differential failure output. If any of
// that drifts silently, every future differential test becomes untrustworthy.
// This test fails loudly instead.
//
// Phase 0 asserted the layout of placeholder types that Phase 1 replaced. The
// layout assertions below describe the CURRENT state, which is what a future
// backend would have to map.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>

#include "TestHarness.h"
#include "p5emu/cpu/CpuModel.h"

namespace {

using namespace p5emu;   // NOLINT(build/namespaces) - test-local convenience
using namespace p5emu::cpu;

// Offsetof guard: if a field is ever reordered, a backend that maps the struct
// directly breaks silently. This makes that break loud.
#define P5EMU_EXPECT_OFFSET(type, field, expected)                  \
  CHECK_EQ(static_cast<std::size_t>(offsetof(type, field)),          \
           static_cast<std::size_t>(expected))

P5EMU_TEST(CpuContracts, StateLayoutIsFixedAndDocumented) {
  // 16 GPRs (rax..r15) = 16 x 8 = 128
  // rip               =        8 -> 136
  // eflags            =        8 -> 144
  // executed_ops      =        8 -> 152
  CHECK_EQ(sizeof(CpuState), static_cast<std::size_t>(152));

  P5EMU_EXPECT_OFFSET(CpuState, rax, 0);
  P5EMU_EXPECT_OFFSET(CpuState, rbx, 8);
  P5EMU_EXPECT_OFFSET(CpuState, rcx, 16);
  P5EMU_EXPECT_OFFSET(CpuState, rdx, 24);
  P5EMU_EXPECT_OFFSET(CpuState, rsi, 32);
  P5EMU_EXPECT_OFFSET(CpuState, rdi, 40);
  P5EMU_EXPECT_OFFSET(CpuState, rbp, 48);
  P5EMU_EXPECT_OFFSET(CpuState, rsp, 56);
  P5EMU_EXPECT_OFFSET(CpuState, r8, 64);
  P5EMU_EXPECT_OFFSET(CpuState, r9, 72);
  P5EMU_EXPECT_OFFSET(CpuState, r10, 80);
  P5EMU_EXPECT_OFFSET(CpuState, r11, 88);
  P5EMU_EXPECT_OFFSET(CpuState, r12, 96);
  P5EMU_EXPECT_OFFSET(CpuState, r13, 104);
  P5EMU_EXPECT_OFFSET(CpuState, r14, 112);
  P5EMU_EXPECT_OFFSET(CpuState, r15, 120);
  P5EMU_EXPECT_OFFSET(CpuState, rip, 128);
  P5EMU_EXPECT_OFFSET(CpuState, eflags, 136);
  P5EMU_EXPECT_OFFSET(CpuState, executed_ops, 144);

  // 8-byte aligned and a whole number of 8-byte words, so a backend can map it
  // in 64-bit units without unaligned-access handling.
  CHECK_EQ(sizeof(CpuState) % 8, static_cast<std::size_t>(0));
  static_assert(alignof(CpuState) >= 8,
                "CpuState must be 8-byte aligned so a backend can map it in "
                "64-bit units");
  CHECK(alignof(CpuState) >= 8);
}

P5EMU_TEST(CpuContracts, StateDefaultsToZeroAndIsTriviallyCopyable) {
  // Rule 6 in CpuModel.h: state is compared field by field, never with memcmp.
  // That comparison, and the snapshotting the differential harness does,
  // depend on the state being a plain value type with a defined all-zero
  // default. A member with a non-trivial constructor would quietly break both.
  static_assert(std::is_trivially_copyable<CpuState>::value,
                "CpuState must be trivially copyable: the differential harness "
                "snapshots it per backend");
  static_assert(std::is_standard_layout<CpuState>::value,
                "CpuState must be standard layout: offsets are asserted above "
                "and relied on by the harness");

  const CpuState fresh;
  CHECK_EQ(fresh.rax, std::uint64_t{0});
  CHECK_EQ(fresh.r15, std::uint64_t{0});
  CHECK_EQ(fresh.rip, std::uint64_t{0});
  CHECK_EQ(fresh.eflags, std::uint64_t{0});
  CHECK_EQ(fresh.executed_ops, std::uint64_t{0});

  CpuState a;
  a.rax = 0x1122334455667788ull;
  a.eflags = 0x202;
  CpuState b = a;
  CHECK_EQ(b.rax, a.rax);
  CHECK_EQ(b.eflags, a.eflags);
}

P5EMU_TEST(CpuContracts, TerminationReasonNamesAreStable) {
  // These strings appear in differential failure output and coverage reports,
  // so they are part of the harness's observable behaviour.
  CHECK(std::strcmp(TerminationReasonName(TerminationReason::kRunning),
                    "running") == 0);
  CHECK(std::strcmp(TerminationReasonName(TerminationReason::kFallthrough),
                    "fallthrough") == 0);
  CHECK(std::strcmp(TerminationReasonName(TerminationReason::kReturn),
                    "return") == 0);
  // A RET that transfers to an address which was never lowered is reported
  // distinctly from a real return, so the two cannot be confused in a diff.
  CHECK(std::strcmp(TerminationReasonName(TerminationReason::kIndirectJump),
                    "indirect-jump") == 0);
  CHECK(std::strcmp(TerminationReasonName(TerminationReason::kFault), "fault") ==
        0);
  CHECK(std::strcmp(TerminationReasonName(TerminationReason::kStepLimit),
                    "step-limit") == 0);
}

P5EMU_TEST(CpuContracts, BackendIdNamesAreStable) {
  CHECK(std::strcmp(BackendIdName(BackendId::kReferenceInterpreter),
                    "reference-interpreter") == 0);
  CHECK(std::strcmp(BackendIdName(BackendId::kBuggyTestOnly),
                    "buggy-test-only") == 0);
}

P5EMU_TEST(CpuContracts, ExecutionTierNamesAreStable) {
  // kArm64 is declared but not implemented. Naming it keeps the gap visible in
  // coverage reports instead of omitting it silently.
  CHECK(std::strcmp(ExecutionTierName(ExecutionTier::kReferenceInterpreter),
                    "reference-interpreter") == 0);
  CHECK(std::strcmp(ExecutionTierName(ExecutionTier::kArm64), "arm64") == 0);
}

P5EMU_TEST(CpuContracts, TheCpuModelHeaderIncludesTheRealDeclarations) {
  // CpuModel.h replaced the Phase-0 placeholder types with the real ones. If a
  // declaration is removed from the umbrella header, a reader arriving at "the
  // CPU model" finds a stub again -- this is the contract that it does not.
  // Compile-time: simply naming each type requires its declaration to be
  // reachable through p5emu/cpu/CpuModel.h alone.
  static_assert(sizeof(ir::Program) > 0, "ir::Program must be reachable");
  static_assert(sizeof(ir::Block) > 0, "ir::Block must be reachable");
  static_assert(sizeof(x64::Instruction) > 0,
                "x64::Instruction must be reachable");
  static_assert(sizeof(memory::GuestMemoryPort) > 0,
                "memory::GuestMemoryPort must be reachable");
  CHECK(true);
}

}  // namespace