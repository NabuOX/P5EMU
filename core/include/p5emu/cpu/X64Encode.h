// SPDX-License-Identifier: LicenseRef-P5EMU-Proprietary
//
// P5EMU source is independently authored and proprietary. See LICENSE.md and
// PROVENANCE.md before adding code here.
//
// ============================================================================
// CANONICAL RE-ENCODER
// ============================================================================
//
// WHY THIS EXISTS
//
// The decode -> re-encode -> compare-bytes round trip is the strongest decoder
// test available: it catches a decoder that reads the right fields but assigns
// them the wrong meaning, which a field-by-field test would miss if the test
// made the same mistake.
//
// THE CANONICALISATION PROBLEM (and why faking byte equality is forbidden)
//
// x86 is NOT a canonical encoding. The same instruction has many valid byte
// sequences:
//
//     48 C7 C0 05 00 00 00     mov rax, 5
//     48 B8 05 00 00 00 00 00 00 00   mov rax, 5
//
// ...and a decoder that accepts both must pick one when re-encoding. Comparing
// the re-encoded bytes against the ORIGINAL bytes would therefore fail on a
// perfectly correct decoder.
//
// The rule this file follows, rather than pretending the problem does not exist:
//
//   * RE-ENCODES into P5EMU's canonical form: one canonical encoding per
//     decoded form.
//   * Tests assert re-encode -> decode -> re-encode is STABLE (idempotent),
//     AND re-encode -> decode yields the SAME decoded instruction fields as the
//     original decode.
//   * Byte equality against the original input is asserted only for
//     instructions whose encoding is already canonical, which is recorded
//     explicitly per test.
//
// This file is INDEPENDENTLY AUTHORED from the decoded form, not a second copy
// of the decode logic: it walks the Instruction struct's fields and emits bytes
// from them. If it shared the decoder's tables, a shared mistake would cancel out
// and the round trip would prove nothing.
//
// Encoding sources: Intel SDM Vol. 2 §2.2.1 (instruction reference),
// §2.2.2.7 (ModR/M), §2.2.2.8 (SIB), §2.2.3.2 (REX).
// ============================================================================

#ifndef P5EMU_CPU_X64_ENCODE_H
#define P5EMU_CPU_X64_ENCODE_H

#include <cstdint>
#include <vector>

#include "p5emu/cpu/X64Decoder.h"

namespace p5emu::x64 {

// Re-encodes `insn` into P5EMU's canonical byte form.
//
// Returns kUnimplementedInstruction for forms the re-encoder does not cover,
// and kInvalidArgument when the Instruction is internally inconsistent (e.g. a
// register operand with no register). Never emits a partial encoding on failure.
Status EncodeCanonical(const Instruction& insn, std::vector<std::uint8_t>* out);

// True when `insn`'s canonical encoding is guaranteed to equal the bytes it was
// decoded from. False where x86 permits several encodings for one semantic form
// (MOV r64 imm64 vs MOV r/m64 imm32 sign-extended, jump/call rel8 vs rel32,
// operand-size ambiguity). Tests consult this rather than assuming byte equality.
bool HasCanonicalEncoding(const Instruction& insn) noexcept;

}  // namespace p5emu::x64

#endif  // P5EMU_CPU_X64_ENCODE_H
