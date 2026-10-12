// SPDX-License-Identifier: Apache-2.0
//
// Error model. P5EMU never throws across a component boundary; components
// return Status. This keeps the interpreter/JIT boundary exception-free and
// makes the CPU core usable from contexts that cannot unwind (iOS callbacks,
// signal-adjacent code).

#ifndef P5EMU_CORE_STATUS_H
#define P5EMU_CORE_STATUS_H

#include <cstdint>

namespace p5emu {

enum class StatusCode : std::uint32_t {
  kOk = 0,

  // Generic
  kInvalidArgument,
  kUnimplemented,
  kInternal,

  // Memory subsystem
  kOutOfMemory,
  kUnmappedAccess,
  kAccessViolation,
  kReadOnlyAccess,

  // Executable loading
  kMalformedExecutable,
  kUnsupportedExecutable,
  // Loader error taxonomy (Phase 4). Deliberately part of the SAME Status
  // universe: a caller handles loading failures with the same mechanism it
  // handles any other failure, and every category is distinguishable rather
  // than collapsed into one "bad file".
  kInvalidMagic,
  kUnsupportedClass,
  kUnsupportedEndian,
  kUnsupportedMachine,
  kTruncatedInput,
  kIntegerOverflow,
  kTooManyEntries,
  kInvalidProgramHeader,
  kInvalidSegment,
  kInvalidAlignment,
  kOverlappingMapping,
  kInvalidEntryPoint,
  kMalformedDynamicTable,
  kUnsupportedRequiredFeature,
  kImageTooLarge,
  kApplyFailed,
  // Relocation and import resolution (Phase 5).
  kMalformedRelocationTable,
  kUnsupportedRelocation,
  kRelocationOverflow,
  kRelocationTargetInvalid,
  kSymbolIndexInvalid,
  kUnresolvedImport,
  kAmbiguousImport,
  kUnsupportedSymbolIdentity,
  // HLE dispatch (Phase 6).
  kHleNotFound,
  kHleAmbiguous,
  kHleUnsupportedIdentity,
  kHleInvalidArgument,
  kHleMemoryFault,
  kHleDispatchFault,
  kHleInternalError,

  // CPU subsystem
  kDecodeError,
  kUnimplementedInstruction,
  kInvalidIr,
  kBackendError,

  // Execution bounds. A runaway program must terminate deterministically rather
  // than hang; a hang is not a diagnosable failure.
  kExecutionLimit,

  // Runtime profile / evidence boundary (Phase 8).
  // A required runtime field is UNKNOWN (no value may be fabricated for it).
  kUnknownRuntimeField,
  // The selected runtime profile cannot be materialised with the current
  // evidence (e.g. a PS5 profile that requires an unevidenced TLS/thread model).
  kRuntimeProfileUnsupported,

  // SCE / Sony dynamic metadata (Phase 9).
  // A tag/identity was recognized by NUMBER, but its semantics are not evidenced
  // for the requested operation. Recognition is not support.
  kUnsupportedSceMetadata,
  // Contradictory metadata (e.g. duplicate singleton tags with different values).
  kConflictingMetadata,
  // A module/library scope could not be determined uniquely.
  kAmbiguousModuleScope,
};

const char* StatusCodeName(StatusCode code) noexcept;

// Small, trivially copyable, allocation-free on the success path.
class Status {
 public:
  Status() = default;
  Status(StatusCode code) : code_(code) {}

  static Status Ok() noexcept { return Status(); }

  [[nodiscard]] bool ok() const noexcept { return code_ == StatusCode::kOk; }
  [[nodiscard]] StatusCode code() const noexcept { return code_; }
  [[nodiscard]] const char* name() const noexcept {
    return StatusCodeName(code_);
  }

 private:
  StatusCode code_ = StatusCode::kOk;
};

}  // namespace p5emu

#endif  // P5EMU_CORE_STATUS_H
