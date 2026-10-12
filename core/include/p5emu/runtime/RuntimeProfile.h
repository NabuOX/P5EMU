// SPDX-License-Identifier: Apache-2.0
//
// P5EMU is an independent, open-source project. See LICENSE and
// PROVENANCE.md before adding code here.
//
// ============================================================================
// RUNTIME PROFILE AND EVIDENCE BOUNDARY (Phase 8)
// ============================================================================
//
// Phase 7 built a GENERIC, synthetic process runtime. Phase 8 keeps that model
// but stops it from silently masquerading as PS5 behaviour:
//
//   * every runtime fact carries an EVIDENCE class (documented / observed /
//     inferred / synthetic);
//   * every startup field is explicitly KNOWN, UNKNOWN, or UNSUPPORTED;
//   * a PS5 runtime profile is representable even when its fields are UNKNOWN,
//     and materialising it FAILS EXPLICITLY rather than inventing values.
//
// This file invents no PS5 address, stack, TLS, or symbol layout. Where PS5
// evidence is absent the field is UNKNOWN. See
// docs/research/ps5-runtime-phase8.md for the per-fact ledger.
// ============================================================================

#ifndef P5EMU_RUNTIME_RUNTIME_PROFILE_H
#define P5EMU_RUNTIME_RUNTIME_PROFILE_H

#include <cstdint>
#include <string>
#include <vector>

#include "p5emu/core/Status.h"
#include "p5emu/cpu/GuestMemory.h"
#include "p5emu/loader/Ps5Executable.h"
#include "p5emu/runtime/Runtime.h"

namespace p5emu::runtime {

// ---------------------------------------------------------------------------
// Evidence class
// ---------------------------------------------------------------------------
// The same taxonomy the project uses in docs/research: a claim is only as strong
// as its weakest supporting evidence, and the classes are never silently merged.
enum class EvidenceClass : std::uint8_t {
  kDocumented = 0,  // published in a specification or vendor documentation
  kObserved,        // determined by lawful, reproducible observation
  kInferred,        // reasoned from other evidence
  kSynthetic,       // a P5EMU-owned model, NOT a PS5 claim
};
const char* EvidenceClassName(EvidenceClass c) noexcept;

// ---------------------------------------------------------------------------
// Field availability
// ---------------------------------------------------------------------------
enum class FieldState : std::uint8_t {
  kKnown = 0,    // value is present and meaningful
  kUnknown,      // PS5 behaviour not evidenced; no value may be fabricated
  kUnsupported,  // understood to be required, but deliberately not implemented
};
const char* FieldStateName(FieldState s) noexcept;

// A single startup field. `value` is meaningful only when state == kKnown.
struct StartupField {
  FieldState state = FieldState::kUnknown;
  std::uint64_t value = 0;
  EvidenceClass evidence = EvidenceClass::kSynthetic;

  static StartupField Known(std::uint64_t v,
                            EvidenceClass e = EvidenceClass::kSynthetic) noexcept {
    return StartupField{FieldState::kKnown, v, e};
  }
  static StartupField Unknown() noexcept {
    return StartupField{FieldState::kUnknown, 0, EvidenceClass::kInferred};
  }
  static StartupField Unsupported() noexcept {
    return StartupField{FieldState::kUnsupported, 0, EvidenceClass::kInferred};
  }
  [[nodiscard]] bool known() const noexcept { return state == FieldState::kKnown; }
};

// ---------------------------------------------------------------------------
// Runtime profile
// ---------------------------------------------------------------------------
// The explicit separation that keeps generic SysV assumptions from becoming PS5
// claims: a process runs under exactly one profile, and only the generic profile
// may be materialised from the Phase-7 synthetic startup model.
enum class RuntimeProfileKind : std::uint8_t {
  kGenericSynthetic = 0,  // P5EMU synthetic SysV-style model (NOT PS5)
  kPs5,                   // PS5 profile; fields may be UNKNOWN/unsupported
};
const char* RuntimeProfileName(RuntimeProfileKind k) noexcept;

// The structured guest startup state. It can represent known, unknown, and
// unsupported fields without inventing values.
struct StartupState {
  RuntimeProfileKind profile = RuntimeProfileKind::kGenericSynthetic;

  StartupField entry_rip;        // program counter at first instruction
  StartupField initial_rsp;      // stack pointer at entry
  StartupField initial_rbp;      // frame pointer at entry
  StartupField thread_pointer;   // TLS base (FS/GS); UNKNOWN for PS5
  StartupField argc;             // argument count (where applicable)
  StartupField argv;             // argv array presence/location
  StartupField envp;             // envp array presence/location
  StartupField auxv;             // auxv presence/location
  StartupField stack_alignment;  // ABI-required RSP alignment at entry

  // Concrete generic stack image. Populated ONLY for the generic profile; the
  // PS5 profile leaves it empty and must fail materialisation instead.
  ProcessStartup generic;

  [[nodiscard]] bool is_synthetic_model() const noexcept {
    return profile == RuntimeProfileKind::kGenericSynthetic;
  }
};

// Classifies a loaded executable's runtime profile. `is_ps5_dialect` comes from
// the loader's PS5 interpretation layer (e_type / OSABI facts). This is the ONLY
// switch that selects the PS5 profile; it is never inferred from generic ELF.
RuntimeProfileKind ClassifyRuntimeProfile(bool is_ps5_dialect) noexcept;

// Generic profile: all fields KNOWN and classed SYNTHETIC. The layout is the
// Phase-7 model, not a PS5 claim.
StartupState MakeGenericStartupState(const ProcessStartup& startup);

// PS5 profile: entry_rip is KNOWN (the executable's ELF entry value, a
// DOCUMENTED fact), and every other field -- RSP, RBP, argc/argv/envp/auxv,
// thread pointer, stack alignment -- is UNKNOWN. No PS5 value is invented.
StartupState MakePs5StartupState(memory::GuestAddr entry_rip);

// Materialises a StartupState into a concrete ProcessStartup.
//   * generic profile (all required fields known): succeeds; `*out` = the stored
//     image.
//   * any profile with UNSUPPORTED required fields: kRuntimeProfileUnsupported.
//   * any profile with UNKNOWN required fields: kUnknownRuntimeField.
// On failure, `*blockers` names the offending fields and `*out` is untouched --
// unknown values are NEVER fabricated.
Status MaterializeStartup(const StartupState& state, ProcessStartup* out,
                          std::vector<std::string>* blockers);

// TLS / thread-pointer support. Fails explicitly (kRuntimeProfileUnsupported)
// when the thread pointer is not a KNOWN field. There is no fake thread pointer.
Status CheckThreadPointerSupport(const StartupState& state);

// PS5 load-address / ASLR policy is UNKNOWN. Phase 7's LoadBiasAllocator is a
// SYNTHETIC test layout. This exposes that boundary as a typed value so the
// synthetic strategy is never relabelled as PS5 ASLR.
StartupField Ps5LoadBiasEvidence() noexcept;

// ---------------------------------------------------------------------------
// Module metadata foundation
// ---------------------------------------------------------------------------
// Describes only what evidence supports. Unknown raw metadata (Sony tag values,
// PS5-era "_1" variants) is PRESERVED as counts, never interpreted.
enum class ModuleRole : std::uint8_t {
  kUnknown = 0,  // role not evidenced (e.g. a generic ELF64)
  kExecutable,
  kSharedLibrary,
};
const char* ModuleRoleName(ModuleRole r) noexcept;

struct ModuleMetadataInfo {
  ModuleRole role = ModuleRole::kUnknown;
  std::vector<std::string> dependencies;   // generic DT_NEEDED names (facts)
  std::size_t generic_dynamic_entries = 0;
  std::size_t recognized_sce_tags = 0;     // Sony tag numbers preserved verbatim
  std::size_t preserved_unknown_tags = 0;  // raw entries kept, not interpreted
  bool declares_sce_imports = false;       // DT_OS_IMPORT_LIB present (fact)
  bool declares_sce_exports = false;       // DT_OS_EXPORT_LIB present (fact)
  // Module init/fini hook metadata. PS5 initialization semantics are UNKNOWN, so
  // these remain UNKNOWN rather than being filled from a generic convention.
  FieldState initialization_metadata = FieldState::kUnknown;
  FieldState finalization_metadata = FieldState::kUnknown;
};
ModuleMetadataInfo DescribeModuleMetadata(
    const loader::Ps5ExecutableImage& image) noexcept;

// Module-initialization ordering model. P5EMU's dependency order is SYNTHETIC;
// PS5 startup ordering is UNKNOWN.
enum class InitOrderModel : std::uint8_t {
  kSyntheticDependenciesFirst = 0,
  kPs5Unknown,
};
const char* InitOrderModelName(InitOrderModel m) noexcept;

}  // namespace p5emu::runtime

#endif  // P5EMU_RUNTIME_RUNTIME_PROFILE_H
