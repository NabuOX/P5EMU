// SPDX-License-Identifier: Apache-2.0
//
// P5EMU is an independent, open-source project. See LICENSE and
// PROVENANCE.md before adding code here.

#include "p5emu/runtime/RuntimeProfile.h"

namespace p5emu::runtime {
namespace {

// Required startup fields, in a stable order, so blocker reports are
// deterministic. `initial_rbp` is required because a profile that cannot state
// it must not silently default it.
struct RequiredField {
  const char* name;
  const StartupField* field;
};

std::vector<RequiredField> RequiredFields(const StartupState& s) {
  return {
      {"entry_rip", &s.entry_rip},
      {"initial_rsp", &s.initial_rsp},
      {"initial_rbp", &s.initial_rbp},
      {"thread_pointer", &s.thread_pointer},
      {"argc", &s.argc},
      {"argv", &s.argv},
      {"envp", &s.envp},
      {"auxv", &s.auxv},
      {"stack_alignment", &s.stack_alignment},
  };
}

}  // namespace

const char* EvidenceClassName(EvidenceClass c) noexcept {
  switch (c) {
    case EvidenceClass::kDocumented: return "documented";
    case EvidenceClass::kObserved: return "observed";
    case EvidenceClass::kInferred: return "inferred";
    case EvidenceClass::kSynthetic: return "synthetic";
  }
  return "unknown";
}

const char* FieldStateName(FieldState s) noexcept {
  switch (s) {
    case FieldState::kKnown: return "known";
    case FieldState::kUnknown: return "unknown";
    case FieldState::kUnsupported: return "unsupported";
  }
  return "unknown";
}

const char* RuntimeProfileName(RuntimeProfileKind k) noexcept {
  switch (k) {
    case RuntimeProfileKind::kGenericSynthetic: return "generic-synthetic";
    case RuntimeProfileKind::kPs5: return "ps5";
  }
  return "unknown";
}

const char* ModuleRoleName(ModuleRole r) noexcept {
  switch (r) {
    case ModuleRole::kUnknown: return "unknown";
    case ModuleRole::kExecutable: return "executable";
    case ModuleRole::kSharedLibrary: return "shared-library";
  }
  return "unknown";
}

const char* InitOrderModelName(InitOrderModel m) noexcept {
  switch (m) {
    case InitOrderModel::kSyntheticDependenciesFirst: return "synthetic-dependencies-first";
    case InitOrderModel::kPs5Unknown: return "ps5-unknown";
  }
  return "unknown";
}

RuntimeProfileKind ClassifyRuntimeProfile(bool is_ps5_dialect) noexcept {
  return is_ps5_dialect ? RuntimeProfileKind::kPs5
                        : RuntimeProfileKind::kGenericSynthetic;
}

StartupState MakeGenericStartupState(const ProcessStartup& startup) {
  StartupState s;
  s.profile = RuntimeProfileKind::kGenericSynthetic;
  s.entry_rip = StartupField::Known(startup.entry_rip, EvidenceClass::kSynthetic);
  s.initial_rsp =
      StartupField::Known(startup.initial_rsp, EvidenceClass::kSynthetic);
  s.initial_rbp = StartupField::Known(0, EvidenceClass::kSynthetic);
  // The generic model configures no TLS; the thread pointer is a synthetic 0,
  // NOT a PS5 value.
  s.thread_pointer = StartupField::Known(0, EvidenceClass::kSynthetic);
  s.argc = StartupField::Known(startup.argc, EvidenceClass::kSynthetic);
  s.argv = StartupField::Known(startup.argv.size(), EvidenceClass::kSynthetic);
  s.envp = StartupField::Known(startup.envp.size(), EvidenceClass::kSynthetic);
  s.auxv = StartupField::Known(startup.auxv.size(), EvidenceClass::kSynthetic);
  s.stack_alignment = StartupField::Known(16, EvidenceClass::kSynthetic);
  s.generic = startup;
  return s;
}

StartupState MakePs5StartupState(memory::GuestAddr entry_rip) {
  StartupState s;
  s.profile = RuntimeProfileKind::kPs5;
  // The entry VALUE comes from the executable's ELF header: documented. Whether
  // the PS5 enters with this state is a separate question and stays UNKNOWN.
  s.entry_rip = StartupField::Known(entry_rip, EvidenceClass::kDocumented);
  s.initial_rsp = StartupField::Unknown();
  s.initial_rbp = StartupField::Unknown();
  s.thread_pointer = StartupField::Unknown();
  s.argc = StartupField::Unknown();
  s.argv = StartupField::Unknown();
  s.envp = StartupField::Unknown();
  s.auxv = StartupField::Unknown();
  s.stack_alignment = StartupField::Unknown();
  return s;
}

Status MaterializeStartup(const StartupState& state, ProcessStartup* out,
                          std::vector<std::string>* blockers) {
  if (out == nullptr) {
    return StatusCode::kInvalidArgument;
  }
  std::vector<std::string> unsupported;
  std::vector<std::string> unknown;
  for (const RequiredField& rf : RequiredFields(state)) {
    if (rf.field->state == FieldState::kUnsupported) {
      unsupported.emplace_back(rf.name);
    } else if (rf.field->state == FieldState::kUnknown) {
      unknown.emplace_back(rf.name);
    }
  }
  if (blockers != nullptr) {
    blockers->clear();
    for (const std::string& n : unsupported) {
      blockers->push_back("unsupported:" + n);
    }
    for (const std::string& n : unknown) {
      blockers->push_back("unknown:" + n);
    }
  }
  if (!unsupported.empty()) {
    return StatusCode::kRuntimeProfileUnsupported;
  }
  if (!unknown.empty()) {
    return StatusCode::kUnknownRuntimeField;
  }
  // All required fields are known. Only the generic profile carries a concrete
  // image; a PS5 profile cannot reach here with all fields known today, and if
  // it ever does, its own image must have been built explicitly.
  *out = state.generic;
  return StatusCode::kOk;
}

Status CheckThreadPointerSupport(const StartupState& state) {
  if (state.thread_pointer.state != FieldState::kKnown) {
    return StatusCode::kRuntimeProfileUnsupported;
  }
  return StatusCode::kOk;
}

StartupField Ps5LoadBiasEvidence() noexcept { return StartupField::Unknown(); }

ModuleMetadataInfo DescribeModuleMetadata(
    const loader::Ps5ExecutableImage& image) noexcept {
  ModuleMetadataInfo info;
  info.dependencies.reserve(image.needed_modules.size());
  for (const loader::ImportModule& m : image.needed_modules) {
    info.dependencies.push_back(m.name);
  }
  info.recognized_sce_tags = image.ps5_tags.size();
  info.preserved_unknown_tags = image.unknown_tags.size();
  const std::size_t accounted =
      info.recognized_sce_tags + info.preserved_unknown_tags;
  info.generic_dynamic_entries = image.dynamic_raw.size() > accounted
                                     ? image.dynamic_raw.size() - accounted
                                     : 0;
  info.declares_sce_imports = image.declares_ps5_imports;
  info.declares_sce_exports = image.declares_ps5_exports;

  if (image.is_ps5_dialect) {
    if (image.e_type == loader::kPs5EtDynExec) {
      info.role = ModuleRole::kExecutable;
    } else if (image.e_type == loader::kPs5EtDynamic) {
      info.role = ModuleRole::kSharedLibrary;
    }
    // Any other PS5-dialect e_type: role stays UNKNOWN rather than guessed.
  }
  // A generic ELF64 carries no evidenced PS5 role; role stays kUnknown.
  return info;
}

}  // namespace p5emu::runtime
