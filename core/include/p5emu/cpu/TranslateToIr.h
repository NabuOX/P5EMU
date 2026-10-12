// SPDX-License-Identifier: Apache-2.0
//
// P5EMU is an independent, open-source project. See LICENSE and
// PROVENANCE.md before adding code here.
//
// ============================================================================
// DECODE -> IR TRANSLATION
// ============================================================================
//
// SEPARATION OF CONCERNS (hard architectural requirement)
//
//   X64Decoder.cpp   knows x86-64 ENCODING: prefixes, opcode, ModRM, SIB.
//   THIS FILE        knows x86-64 SEMANTICS: what MOV does, which flags ADD
//                    sets, when a 32-bit write zero-extends to 64.
//   ReferenceInterpreter.cpp  knows only P5EMU IR semantics, and can never see
//                    a decoded instruction.
//
// The interpreter is the specification, so it must not be reachable from x86
// encodings. If it were, a backend could implement an encoding quirk instead of
// the IR's semantics, and the differential comparison would prove nothing.
//
// SEMANTIC SOURCES
//
// Intel SDM Vol. 2 (instruction set reference) and Vol. 3 (for the 32-bit
// destination zero-extension rule, §5.3.1 "General-Purpose Registers in 64-Bit
// Mode" / the MOV description in Vol. 2 §2.2.1). Flag semantics are Vol. 2
// §3.1.2. No third-party implementation was consulted.
// ============================================================================

#ifndef P5EMU_CPU_TRANSLATE_TO_IR_H
#define P5EMU_CPU_TRANSLATE_TO_IR_H

#include <vector>

#include "p5emu/core/Status.h"
#include "p5emu/cpu/GuestMemory.h"
#include "p5emu/cpu/Ir.h"
#include "p5emu/cpu/IrBuilder.h"
#include "p5emu/cpu/X64Decoder.h"

namespace p5emu::cpu {

// Translates one decoded instruction into the current block.
//
// Control-flow instructions set the block terminator. A terminator that
// transfers control leaves the builder's current block pointing at the
// instruction AFTER the branch, which is where linear decoding continues; the
// caller is responsible for creating the target block if it wants to reach it.
// That arrangement is what makes `jmp` forward-over-a-skipped-region work
// without a separate reachability pass.
Status TranslateToIr(const x64::Instruction& insn, ir::Builder* builder);

// Decodes and translates a straight-line byte sequence into a Program.
//
// Control-flow instructions terminate the program: everything after an
// unconditional transfer is not translated, because reaching it requires
// knowing the dynamic control flow, which is Phase 2's job. This function is
// for straight-line code and for building blocks, NOT for whole-program
// lowering.
//
// Returns kOk with `program` populated and validated, or a failure status.
// `out_provenance_addresses` receives the guest address of each translated
// instruction, so a caller can report where a mismatch came from.
Status TranslateBlockToIr(memory::GuestAddr base_address,
                          const std::uint8_t* code, std::size_t length,
                          std::vector<memory::GuestAddr>* out_addresses,
                          ir::Program* out_program);

// Lowers a whole guest byte sequence into a multi-block Program, following
// control flow within the supplied byte range.
//
// This is what the end-to-end test and the differential harness use. It builds a
// block per basic block, resolves branch targets to blocks, and terminates at a
// return. Targets outside `code`/`length` become a trap terminator rather than a
// silently-dropped branch.
//
// This is a LINEAR lowering of the supplied bytes. It is not a complete
// translator: it does not model the stack, calls, or self-modifying code, and it
// refuses a backward branch that would not terminate.
Status TranslateProgram(memory::GuestAddr base_address, const std::uint8_t* code,
                        std::size_t length, std::string name,
                        ir::Program* out_program,
                        std::vector<memory::GuestAddr>* out_addresses);

}  // namespace p5emu::cpu

#endif  // P5EMU_CPU_TRANSLATE_TO_IR_H
