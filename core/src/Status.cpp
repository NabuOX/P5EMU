// SPDX-License-Identifier: Apache-2.0

#include "p5emu/core/Status.h"

namespace p5emu {

const char* StatusCodeName(StatusCode code) noexcept {
  switch (code) {
    case StatusCode::kOk:
      return "ok";
    case StatusCode::kInvalidArgument:
      return "invalid-argument";
    case StatusCode::kUnimplemented:
      return "unimplemented";
    case StatusCode::kInternal:
      return "internal";
    case StatusCode::kOutOfMemory:
      return "out-of-memory";
    case StatusCode::kUnmappedAccess:
      return "unmapped-access";
    case StatusCode::kAccessViolation:
      return "access-violation";
    case StatusCode::kReadOnlyAccess:
      return "read-only-access";
    case StatusCode::kMalformedExecutable:
      return "malformed-executable";
    case StatusCode::kUnsupportedExecutable:
      return "unsupported-executable";
    case StatusCode::kInvalidMagic:
      return "invalid-magic";
    case StatusCode::kUnsupportedClass:
      return "unsupported-class";
    case StatusCode::kUnsupportedEndian:
      return "unsupported-endian";
    case StatusCode::kUnsupportedMachine:
      return "unsupported-machine";
    case StatusCode::kTruncatedInput:
      return "truncated-input";
    case StatusCode::kIntegerOverflow:
      return "integer-overflow";
    case StatusCode::kTooManyEntries:
      return "too-many-entries";
    case StatusCode::kInvalidProgramHeader:
      return "invalid-program-header";
    case StatusCode::kInvalidSegment:
      return "invalid-segment";
    case StatusCode::kInvalidAlignment:
      return "invalid-alignment";
    case StatusCode::kOverlappingMapping:
      return "overlapping-mapping";
    case StatusCode::kInvalidEntryPoint:
      return "invalid-entry-point";
    case StatusCode::kMalformedDynamicTable:
      return "malformed-dynamic-table";
    case StatusCode::kUnsupportedRequiredFeature:
      return "unsupported-required-feature";
    case StatusCode::kImageTooLarge:
      return "image-too-large";
    case StatusCode::kApplyFailed:
      return "apply-failed";
    case StatusCode::kMalformedRelocationTable:
      return "malformed-relocation-table";
    case StatusCode::kUnsupportedRelocation:
      return "unsupported-relocation";
    case StatusCode::kRelocationOverflow:
      return "relocation-overflow";
    case StatusCode::kRelocationTargetInvalid:
      return "relocation-target-invalid";
    case StatusCode::kSymbolIndexInvalid:
      return "symbol-index-invalid";
    case StatusCode::kUnresolvedImport:
      return "unresolved-import";
    case StatusCode::kAmbiguousImport:
      return "ambiguous-import";
    case StatusCode::kUnsupportedSymbolIdentity:
      return "unsupported-symbol-identity";
    case StatusCode::kHleNotFound:
      return "hle-not-found";
    case StatusCode::kHleAmbiguous:
      return "hle-ambiguous";
    case StatusCode::kHleUnsupportedIdentity:
      return "hle-unsupported-identity";
    case StatusCode::kHleInvalidArgument:
      return "hle-invalid-argument";
    case StatusCode::kHleMemoryFault:
      return "hle-memory-fault";
    case StatusCode::kHleDispatchFault:
      return "hle-dispatch-fault";
    case StatusCode::kHleInternalError:
      return "hle-internal-error";
    case StatusCode::kDecodeError:
      return "decode-error";
    case StatusCode::kUnimplementedInstruction:
      return "unimplemented-instruction";
    case StatusCode::kInvalidIr:
      return "invalid-ir";
    case StatusCode::kBackendError:
      return "backend-error";
    case StatusCode::kExecutionLimit:
      return "execution-limit";
  }
  return "unknown";
}

}  // namespace p5emu
