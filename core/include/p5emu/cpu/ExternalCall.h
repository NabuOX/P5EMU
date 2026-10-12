// SPDX-License-Identifier: LicenseRef-P5EMU-Proprietary
//
// P5EMU source is independently authored and proprietary. See LICENSE.md and
// PROVENANCE.md before adding code here.
//
// ============================================================================
// EXTERNAL CALL HANDLER
// ============================================================================
//
// The narrow seam between the CPU core and the HLE subsystem.
//
// An `kExternalCall` terminator is a transfer to a guest address that is not
// guest code -- an HLE import. The interpreter (and, out of process, the
// AArch64 harness) hands that address to an ExternalCallHandler, which maps it
// to a registered host implementation.
//
// The CPU core knows NOTHING about Sony libraries, module names, or HLE
// functions. It knows only that control left guest code for a binding address.
// The handler sees only the guest state and the guest memory port -- never
// arbitrary emulator internals.
// ============================================================================

#ifndef P5EMU_CPU_EXTERNAL_CALL_H
#define P5EMU_CPU_EXTERNAL_CALL_H

#include <cstdint>

#include "p5emu/core/Status.h"

namespace p5emu::memory {
class GuestMemoryPort;
}

namespace p5emu::cpu {

struct CpuState;

class ExternalCallHandler {
 public:
  virtual ~ExternalCallHandler() = default;

  // Handles an external call to `binding_address`.
  //
  // `state` is the guest CPU state (in/out): the handler reads the guest ABI
  // argument registers and writes the return register. `memory` is the guest
  // memory port; all guest memory access must go through it.
  //
  // Returns kOk on success. Any other Status terminates execution with
  // TerminationReason::kHleError.
  virtual Status HandleExternalCall(std::uint64_t binding_address,
                                    CpuState* state,
                                    memory::GuestMemoryPort* memory) = 0;
};

}  // namespace p5emu::cpu

#endif  // P5EMU_CPU_EXTERNAL_CALL_H
