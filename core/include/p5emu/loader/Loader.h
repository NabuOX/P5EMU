// SPDX-License-Identifier: Apache-2.0
//
// P5EMU is an independent, open-source project. See LICENSE and
// PROVENANCE.md before adding code here.
//
// ============================================================================
// LOAD PLAN, RELOCATION, AND IMPORT RESOLUTION
// ============================================================================
//
// The safety boundary: untrusted file -> parse -> validate -> PLAN -> resolve ->
// apply. Nothing is written to guest memory while parsing. A plan is built and
// validated in full first; only a valid plan may be applied, and applying is
// transactional (a failure leaves the caller's memory untouched).
//
// Load bias:  final_guest_va = elf_va + bias.  It is explicit and singular,
// never scattered through the loader.
// ============================================================================

#ifndef P5EMU_LOADER_LOADER_H
#define P5EMU_LOADER_LOADER_H

#include <cstdint>
#include <string>
#include <vector>

#include "p5emu/core/Status.h"
#include "p5emu/cpu/MappedGuestMemory.h"
#include "p5emu/cpu/ReferenceInterpreter.h"
#include "p5emu/loader/BinaryReader.h"
#include "p5emu/loader/Elf64.h"
#include "p5emu/loader/Import.h"
#include "p5emu/loader/Ps5Executable.h"
#include "p5emu/loader/Reloc.h"

namespace p5emu::loader {

// The loader refuses to map an image whose total mapped memory exceeds this.
inline constexpr std::uint64_t kMaxLoadImageBytes = 1ull << 28;

// One mapped range described by the plan. `memsz - filesz` is the zero-fill
// (BSS) tail, represented explicitly rather than implied.
struct PlannedSegment {
  std::uint64_t vaddr = 0;
  std::uint64_t filesz = 0;
  std::uint64_t memsz = 0;
  std::uint64_t file_offset = 0;
  std::uint32_t permissions = 0;  // memory::kPermRead|kPermWrite|kPermExecute
  std::uint64_t align = 0;

  [[nodiscard]] std::uint64_t ZeroFillSize() const noexcept {
    return memsz - filesz;
  }
};

struct LoadPlan {
  std::string name;  // diagnostic label only; no file content
  std::uint64_t bias = 0;
  std::uint64_t entry = 0;
  std::vector<PlannedSegment> segments;      // sorted by vaddr
  std::vector<ImportModule> unresolved_imports;  // DT_NEEDED declarations
  std::vector<ImportRequirement> import_requirements;
  std::vector<Relocation> relocations;
  std::vector<ElfSymbol> symbols;
  std::vector<std::string> notes;
};

// Builds a plan from the parsed image at the given load bias. The reader is
// required because relocation and symbol tables live in the file.
Status BuildLoadPlan(const BinaryReader& reader, const Ps5ExecutableImage& image,
                     std::uint64_t load_bias, LoadPlan* out);
// Convenience: bias 0 (matches Phase 4 behaviour).
Status BuildLoadPlan(const BinaryReader& reader, const Ps5ExecutableImage& image,
                     LoadPlan* out);

// Validates an entire plan (per-segment invariants, overlap policy, entry
// resolution, permission intent, relocation targets and types, symbol indices).
Status ValidateLoadPlan(const LoadPlan& plan);

// The state of one requirement after resolution.
struct ResolvedRequirement {
  ImportRequirement requirement;
  ResolutionStatus status = ResolutionStatus::kUnresolved;
  std::uint64_t address = 0;  // valid iff kResolved
};

struct ImportBinding {
  bool execution_ready = false;
  Status status = StatusCode::kOk;  // why not ready, when applicable
  std::vector<ResolvedRequirement> resolved;
};

// Resolves every requirement in the plan against `resolver`. A required
// requirement (one with binding sites) that is not resolved makes the image not
// execution-ready, with a status that names the reason.
Status ResolveImports(const LoadPlan& plan, ImportResolver& resolver,
                      ImportBinding* out);

struct ApplyOptions {
  // TEST-ONLY fault injection: perform at most this many apply operations, then
  // fail, to prove the target port is left untouched. Negative disables.
  std::int64_t fail_after_operations = -1;

  // TEST-ONLY. RELATIVE relocations omit the load bias (use 0 + addend). Used
  // to prove the differential/relocation tests catch a wrong-bias defect.
  bool inject_wrong_load_bias = false;

  // TEST-ONLY. Treat an unresolved symbol as address 0 and continue instead of
  // refusing. Used to prove the readiness guard catches an unresolved-as-zero
  // defect.
  bool inject_unresolved_as_zero = false;
};

// Applies a plan to `target`, transactionally, including relocations.
//
// The plan is staged into a fresh port and `*target` is replaced only on full
// success. Relocations are applied to the staging image (before final guest
// protections take effect) and never change the planned R/W/X permissions.
Status ApplyLoadPlan(const BinaryReader& reader, const LoadPlan& plan,
                     const ImportBinding& binding, const ApplyOptions& options,
                     memory::MappedGuestMemory* target);

// Convenience for plans with no required imports (Phase 4 behaviour).
Status ApplyLoadPlan(const BinaryReader& reader, const LoadPlan& plan,
                     const ApplyOptions& options,
                     memory::MappedGuestMemory* target);

// Minimal loader -> CPU handoff. Phase 5 initializes RIP to the validated,
// bias-applied entry point and nothing else.
Status MakeEntryState(const LoadPlan& plan, cpu::CpuState* out);

}  // namespace p5emu::loader

#endif  // P5EMU_LOADER_LOADER_H
