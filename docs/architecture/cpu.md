# CPU Architecture and Instruction Coverage

The P5EMU x86-64 CPU is one pipeline with two execution backends:

```
x86-64 bytes → Decoder (encoding only) → TranslateToIr (semantics) → P5EMU SSA IR
                                                         │
                                        ┌────────────────┴───────────────┐
                                 ReferenceInterpreter          AArch64 backend
                                   (the differential oracle)     (qemu-aarch64)
```

The decoder knows *encoding*; the translator knows *semantics*; the interpreter
knows only IR; the AArch64 backend lowers the same IR. No backend has an
instruction-specific shortcut, and no family is tested by emitter inspection
alone.

## Supported instruction families

| Family | Forms | Since |
|---|---|---|
| `MOV` | r/r, r/m, r/imm (8/16/32/64) | Phase 1 |
| `MOVZX` / `MOVSX` | 8/16/32-bit sources | Phase 1 |
| `LEA` | 64-bit | Phase 1 |
| `ADD` `SUB` `AND` `OR` `XOR` `CMP` `TEST` | r/m,r · r,r/m · r/m,imm (8/16/32/64) | Phase 1/3 |
| `SHL` `SHR` `SAR` | imm8 and CL counts, 8/16/32/64 | Phase 3 |
| `JMP` `Jcc` `CALL` `RET` | rel8/rel32, all 16 conditions | Phase 1 |
| `NOP` `INT3` | — | Phase 1 |
| **`PUSH`** | `PUSH r64` (0x50+), `PUSH imm8` (0x6A), `PUSH imm32` (0x68) | **Phase 11** |
| **`POP`** | `POP r64` (0x58+) | **Phase 11** |
| **`LEAVE`** | 0xC9 | **Phase 11** |

The decoder reports an unsupported encoding as `kUnimplementedInstruction`; it
never guesses. Recognising an encoding is not support for its semantics.

## Phase 11: the stack family

`PUSH`, `POP` and `LEAVE` are lowered with the **existing** IR operations
(`kGetReg`/`kSetReg` for RSP/RBP, `kLoad`/`kStore` for guest memory, `kAdd`/
`kSub` for the stack adjustment). They add no new IR op and no new backend
lowering, so both backends support them with no backend change.

Semantics (Intel SDM Vol. 2 §2.2.1):

- `PUSH value`  → `RSP -= 8; [RSP] = value`. The store is emitted **before** the
  RSP write, so a guest-memory fault leaves RSP unmodified (atomic-on-fault).
  Pushing RSP pushes the pre-decrement value (RSP is read first).
- `POP r64`     → `r64 = [RSP]; RSP += 8`. If the destination is RSP, the loaded
  value wins (architecturally ESP is incremented *before* the old top is written
  to the destination).
- `LEAVE`       → `RSP := RBP; RBP := [RBP]; RSP := RBP + 8`. The load is emitted
  first, so a fault leaves both registers unmodified. The pop reads from **RBP**,
  not from the old RSP.

Imm64 forms are sign-extended to 64 bits (imm8 and imm32). The 16-bit
operand-size (`0x66`) and memory-source PUSH/POP forms are **not** in the
supported set and are reported as `kUnimplementedInstruction` rather than
approximated.

### Memory-fault behaviour

The memory-fault atomicity of PUSH/POP/LEAVE is defined by IR operation order
(load/store before the register writebacks). The interpreter and AArch64 backend
execute the same IR, so they fault at the same point with the same observable
state. This is a **model abstraction**, not a claim about exact x86
restart semantics: P5EMU does not model the full PS5 exception-delivery model.

### Flags

`PUSH`, `POP`, and `LEAVE` do **not** modify EFLAGS. This is architectural
(x86 defines no flag effect for these instructions), so no flag mask is needed.

## Arithmetic families (Phase 11)

`ADC`, `SBB`, `MUL`, `IMUL`, `DIV`, and `IDIV` are implemented through
decode → IR → interpreter → AArch64. The interpreter and the AArch64 backend
share **one** definition of the semantics: the IR ops carry semantic intent, and
the AArch64 backend lowers them through an auditable arithmetic helper
(`fn_arith`) that mirrors the interpreter exactly (result, flags, and fault).

### New IR operations

- `kAdcFlags(a, b, carry_in, flags_out)` / `kSbbFlags(...)`: the carry-in is an
  explicit 0/1 value read from CF. Result and flags are computed from the true
  triple sum, so a carry caused *only* by the incoming CF is handled.
- `kMulFlags(a, b, flags_out)` / `kIMulFlags(...)`: a DOUBLE-WIDTH product — low
  half in `dest`, high half in `dest2` — plus CF/OF (set iff the full product is
  not representable in the destination width). Signed for `kIMulFlags`.
- `kDivFlags(dividend_hi, dividend_lo, divisor)` / `kIDivFlags(...)`: quotient
  (`dest`) and remainder (`dest2`). No host instruction is named in the IR.

`Op` gained a second result slot (`dest2`), validated to appear only on the
multiply/divide ops.

### Form and width coverage

| Family | Forms | Widths |
|---|---|---|
| ADC | r/m,r · r,r/m · r/m,imm (80/81/83 /2) | 8/16/32/64 |
| SBB | r/m,r · r,r/m · r/m,imm (/3) | 8/16/32/64 |
| MUL | one-operand implicit r/m (F6/F7 /4) | 8/16/32/64 |
| IMUL | one-operand (F6/F7 /5), two-operand (0F AF), immediate (69/6B) | 8/16/32/64 |
| DIV | one-operand implicit r/m (F6/F7 /6) | 8/16/32/64 |
| IDIV | one-operand implicit r/m (F6/F7 /7) | 8/16/32/64 |

**8-bit MUL/IMUL/DIV/IDIV (F6 /4-/7) are not implemented**: their implicit high
result/remainder lives in AH, and P5EMU does not open the general high-byte
register boundary (see below). They are reported `kUnimplementedInstruction`,
never approximated.

### Flags

| Instruction | Defined | Undefined (masked, never invented) |
|---|---|---|
| ADC / SBB | CF OF SF ZF AF PF | — |
| MUL / IMUL | CF OF | SF ZF AF PF |
| DIV / IDIV | (none) | all |

## Synchronous CPU fault architecture (Phase 11)

A guest instruction can raise a synchronous CPU fault, modelled distinctly from
every other termination:

- `TerminationReason::kCpuFault` — distinct from `kFault` (guest memory), from
  `kTrap`, and from `kHleError`. The specific fault is carried in
  `ExecutionResult::status`.
- `StatusCode::kDivideError` is the first fault kind (the x86 #DE category).
- The AArch64 backend carries it via `ctx.status = P5EMU_A64_REASON_CPU_FAULT`
  and `ctx.fault_code = P5EMU_A64_CPU_FAULT_DIVIDE`.

This is an architecture-level guest CPU fault, **not** PS5 kernel exception
delivery: no IDT, signal, or vector is modelled.

### Fault commit model (DIV/IDIV)

On `divisor == 0` or a quotient that does not fit the destination width the
instruction commits **nothing** — no quotient, no remainder, no flags. The helper
leaves its outputs untouched and sets the fault slot; the generated code checks
the fault before storing; the interpreter sets the reason/status and breaks
before writing. `CpuArithNegative.DivideFaultCommitsNothing` proves RAX/RDX are
byte-identical to their pre-instruction values on a fault. RIP on a CPU fault is
left at its pre-fault value, matching the memory-fault convention.

## Host-UB audit

- 128-bit intermediates use `__int128` (GCC/Clang on x86-64 and aarch64); no
  signed-overflowing 64-bit arithmetic.
- Unsigned carry/borrow uses `unsigned __int128` or explicit comparisons, never
  signed overflow.
- `IDIV` is guarded: `divisor == 0`, and the `INT_MIN / -1` case (host UB) is
  special-cased and reported as a quotient overflow (#DE). The quotient range is
  checked before anything is written.

## High-byte register boundary

8-bit MUL/DIV architecturally use AH. P5EMU keeps AH/BH/CH/DH outside the general
register model and therefore does not implement the 8-bit implicit forms rather
than opening that boundary for a single instruction. The general high-byte
encodings stay unsupported and are rejected explicitly.

## High-byte registers (Phase 12)

AH/CH/DH/BH are first-class IR registers (`kAh`/`kCh`/`kDh`/`kBh`) aliasing bits
8..15 of RAX/RCX/RDX/RBX. The decoder marks an operand `high_byte` for an 8-bit
register number 4..7 with **no** REX prefix; the interpreter and AArch64 backend
read/write the bits-8..15 subset (and validate that such a register is used at
width 1). With a REX prefix, 4..7 are SPL/BPL/SIL/DIL and are mapped normally.
This is what makes the 8-bit implicit MUL/IMUL/DIV/IDIV results (AH)
representable without a special case.

## Phase 12 additions

- **8-bit MUL/IMUL/DIV/IDIV (F6 /4-/7)**: `AL * r/m8 -> AX`, and `AX / r/m8 ->
  AL:AH`. The implicit high result/remainder is AH.
- **ADC/SBB accumulator-immediate (0x14/0x15, 0x1C/0x1D)**: `AL`/`eAX` with an
  immediate.
- **16-bit PUSH/POP**: `push r16` / `pop r16` (0x66-prefixed), a 2-byte stack
  slot and a 2-byte RSP adjustment.
- **Memory-source PUSH/POP**: `FF /6 PUSH r/m` and `8F /0 POP r/m`. A memory POP
  destination computes its effective address from the **old** RSP; POP RSP itself
  takes the loaded value.

## Remaining gaps

- Out of scope by project policy: REP/string, SSE/AVX, x87, atomics/LOCK, full
  exception delivery, syscalls, an optimizer, and register allocation.

P5EMU does **not** claim complete x86-64 compatibility. Coverage is deliberately
small, honest, and differentially verified.
