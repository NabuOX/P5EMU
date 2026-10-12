// SPDX-License-Identifier: LicenseRef-P5EMU-Proprietary
//
// P5EMU source is independently authored and proprietary. See LICENSE.md and
// PROVENANCE.md before adding code here.
//
// ============================================================================
// DIFFERENTIAL HARNESS
// ============================================================================
//
// WHAT THIS DOES, AND WHY IT IS THE CRITICAL PRODUCT OF PHASE 1
//
// Given validated IR, an initial CpuState, and an initial memory image, it runs
// each backend in isolation and compares:
//
//   * every general-purpose register, field by field
//   * RIP
//   * EFLAGS
//   * a declared memory window, byte by byte
//   * termination reason and Status
//
// A harness that has never been observed to fail is indistinguishable from a
// harness that cannot fail. Therefore Phase 1 includes a backend with one
// deliberate, DOCUMENTED semantic bug, and the gate is: the harness must catch
// it, for the right reason (a state/result mismatch), not by crashing and not
// by a forced assertion.
//
// DESIGN CONSTRAINTS THAT MAKE THE COMPARISON TRUSTWORTHY
//
//   1. ISOLATION. Each backend gets its own CpuState copy and its own memory
//      port built from the same initial image. A backend cannot pass by reading
//      the other's results.
//   2. FIELD-BY-FIELD, NEVER memcmp. A memcmp over a state struct passes with a
//      wrong EFLAGS.
//   3. BOUNDED MEMORY. The window is declared. Nothing outside it is compared,
//      and nothing outside it is allowed to differ silently: the harness also
//      checks a guard region if one is supplied.
//   4. HONEST STATUS. A backend reporting kUnimplementedInstruction is recorded
//      as a COVERAGE GAP, separately from correctness. It is never counted as a
//      pass and never silently skipped.
//   5. DETERMINISM. Two runs of the same comparison produce the same verdict.
// ============================================================================

#ifndef P5EMU_CPU_DIFFERENTIAL_H
#define P5EMU_CPU_DIFFERENTIAL_H

#include <string>
#include <vector>

#include "p5emu/core/Status.h"
#include "p5emu/cpu/Backend.h"
#include "p5emu/cpu/GuestMemory.h"
#include "p5emu/cpu/Ir.h"
#include "p5emu/cpu/ReferenceInterpreter.h"

namespace p5emu::cpu::diff {

// What to compare, and the memory region to inspect after execution.
struct MemoryWindow {
  memory::GuestAddr addr = 0;
  std::size_t length = 0;
};

struct ComparisonRequest {
  std::string name;              // fixture name, for diagnostics
  const ir::Program* program = nullptr;

  CpuState initial_state;

  // Initial guest memory image, copied into a fresh port per backend.
  memory::GuestAddr memory_base = 0;
  memory::GuestSize memory_size = 0;
  std::vector<std::uint8_t> initial_memory;
  memory::Permissions permissions = memory::kPermReadWriteExecute;

  // Regions to compare after execution. Empty means no memory comparison.
  std::vector<MemoryWindow> windows;

  // Which EFLAGS bits are OBSERVABLE for this comparison. Bits outside the mask
  // are ignored on every backend. This is how architecturally UNDEFINED flags
  // (for example OF after a shift by more than one) are excluded from the
  // comparison: the fixture declares what the ISA actually defines, instead of
  // demanding that two backends agree on invented behaviour.
  //
  // It is fixture-specific on purpose. There is NO global masking of flags: the
  // default observes every bit.
  std::uint64_t eflags_observable_mask = eflags::kAllMask;

  std::uint64_t step_limit = ReferenceInterpreter::kDefaultStepLimit;
};

enum class Verdict : std::uint8_t {
  kAgree = 0,          // every backend produced the same result
  kDiverge,            // a real state/result mismatch was detected
  kCoverageGap,        // no divergence, but at least one backend did not
                       // implement the program. NOT a pass: distinct from
                       // kAgree so a gap can never be read as agreement.
  kError,              // the harness could not run (invalid IR, bad request)
};

const char* VerdictName(Verdict v) noexcept;

// One specific difference, with enough detail to act on.
struct Difference {
  std::string field;          // "rax", "eflags", "mem[0x1000]", "status", ...
  std::uint64_t expected = 0;
  std::uint64_t actual = 0;
  std::string backend_a;
  std::string backend_b;
};

struct BackendReport {
  std::string name;
  Status status;
  TerminationReason reason = TerminationReason::kRunning;
  std::uint64_t executed_ops = 0;
  bool implemented = true;    // false == coverage gap
};

struct ComparisonResult {
  Verdict verdict = Verdict::kError;
  std::vector<BackendReport> reports;
  std::vector<Difference> differences;
  std::string summary;

  // Coverage accounting, kept separate from correctness so a gap can never be
  // mistaken for a pass.
  std::vector<std::string> coverage_gaps;
};

// Runs every backend in `backends` against the request and compares.
//
// The FIRST backend is the reference oracle. Later backends are compared against
// it. A single backend yields kAgree, which is why the caller must pass at least
// two for the comparison to mean anything.
ComparisonResult RunComparison(const ComparisonRequest& request,
                               const std::vector<Backend*>& backends);

}  // namespace p5emu::cpu::diff

#endif  // P5EMU_CPU_DIFFERENTIAL_H
