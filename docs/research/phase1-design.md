# Phase 1 Design: decoder → IR → reference interpreter

**This document designs Phase 1. The CPU core it describes is now implemented
and tested**: `X64Decoder`, `TranslateToIr`, `ir::Program` + `ValidateIr`,
`ReferenceInterpreter`, and the `Differential` harness. The design below remains
the reference for what those components are meant to be.

---

## 1. The milestone

```
a tiny controlled x86-64 instruction stream
        ↓
P5EMU decoder
        ↓
P5EMU IR
        ↓
reference interpreter
        ↓
a KNOWN expected result
```

Later, and only later:

```
        same IR
        ↓
        ARM64 backend
        ↓
        the SAME expected result
```

The property that makes this worth doing carefully:

> `execute(Decoder(bytes), ReferenceInterpreter) == execute(Decoder(bytes), Arm64Backend)`

must be **mechanically checkable**, not asserted. Phase 1 builds the machinery
that will later check it, and proves the machinery works on one backend.

### Why the IR is in Phase 1 rather than after the ARM64 backend

If Phase 1 stopped at "decoder + interpreter", the interpreter's semantics would
be fused to the decoder's output shape, and introducing an IR afterwards would
mean rewriting both. The IR is the *contract between backends*; it has to exist
before the second backend does, or the second backend will be written against the
first one's accidents.

---

## 2. Scope

### In scope

| Deliverable | Done when |
|---|---|
| A handful of x86-64 instructions decoded correctly | Each has a hand-verified expected encoding and expected result |
| An IR with the properties in §3 | Passes its own invariant tests |
| A reference interpreter over that IR | Produces the known expected result |
| A differential harness that *exists and is exercised* | Runs with two backends, reports a deliberate mismatch as a failure |
| A test corpus format that will scale to thousands of cases | Adding a case requires adding one file, not writing code |
| Encoding/decoding round-trip verification | Catches decoder corruption early |

### Explicitly out of scope

| Not in Phase 1 | Why |
|---|---|
| The ARM64 backend | It cannot be built or tested on an x86-64 host. Adding untestable code is worse than none |
| Any PS5-specific instruction support | The PS5 is a normal x86-64 machine. Nothing about x86-64 decoding is PS5-specific |
| Any `sceKernel*`, loader, GPU, audio, input | None of it is needed to validate the CPU contract |
| SIMD/AVX | Apple has no 256-bit SIMD. That problem is real and large; it belongs in its own phase with its own risk assessment |
| x87 | Real, large, and a separate project. The `CpuState` reserves `st[8]` and `x87_status` so the layout does not have to move later |
| Performance work | Phase 1 establishes correctness. A fast wrong answer is worth less than a slow right one |
| More than a handful of instructions | Breadth is Phase 2. Depth on ~20 instructions that exercise flags, memory, branches, and the IR is worth more than 200 shallow ones |

---

## 3. IR requirements

The IR must have these properties from the first line, or retrofitting them will
be expensive.

| # | Property | Reason |
|---|---|---|
| 1 | **CPU-state ops separate from memory ops.** `LoadContext`/`StoreContext` for architectural state; `LoadMem`/`StoreMem` for guest memory | The single most important design idea adopted from FEX's IR. It makes state spillable and register-allocatable while guest memory is not |
| 2 | **Sized, typed values.** Every value has an explicit width (1/2/4/8 bytes, or a vector with an element count) | x86 is a variable-width ISA. Width-implicit IR forces the IR to grow special cases forever |
| 3 | **Flags are values, not side effects.** `AddWithFlags`, `ShiftFlags`, `LoadFlags`, `StoreFlags` | x86 flag dependencies are both a correctness trap and a performance trap. An IR that hides flags behind instructions cannot optimise them |
| 4 | **Arena-allocated, offset-referenced.** Nodes addressed by `uint32_t` offset, not pointer | Relocatable, cache-dense, and AOT-serialisable later without a redesign |
| 5 | **Register allocation expressible as IR ops.** `AllocateGPR`, `AllocateGPRAfter`, `AllocateFPR`, `SpillRegister`, `FillRegister` | Makes the host register file a visible, testable resource rather than backend folklore |
| 6 | **Explicit, checked validation.** `ValidateIr()` runs in debug builds and rejects malformed IR | A malformed IR entering the JIT is a memory-corruption bug. Catching it in the interpreter is dramatically cheaper |
| 7 | **Deterministic construction** | Two runs from the same guest bytes must produce byte-identical IR. Otherwise differential testing and AOT caching both break |
| 8 | **No host types anywhere.** No `uint64_t` host register, no host pointer, no host enum | The IR must be portable to any backend, including a hypothetical AOT compiler for a third host |

### Explicitly rejected

| Rejected | Because |
|---|---|
| LLVM IR | Makes the interpreter untestable without a toolchain, adds ~30 MB, and ORJIT has iOS code-sign failures |
| Untyped op list | Cannot express flag or width semantics; every backend re-derives them differently |
| Bytecode/stack VM | Loses the type and flag information the backends need; slower to interpret than SSA |
| Building the IR per backend | Defeats the entire differential-testing premise |

---

## 4. Instruction selection for Phase 1

Chosen to exercise every IR feature, not for coverage. Roughly 20 instructions.

| Group | Instructions | Exercises |
|---|---|---|
| Registers/moves | `mov r64, imm64`; `mov r/m64, r64`; `mov r64, r/m64` | Property 1, sized values, memory ops |
| ALU | `add r64, r/m64`; `xor r64, r/m64`; `and r64, r/m64` | Flags: OF/SF/ZF/AF/PF/CF generation |
| Flag read | `setcc r8`; `cmovcc r64, r/m64` | Property 3 — flags as *values*, consumed later. This is the group that most implementations get wrong |
| Shift | `shl r/m64, imm8`; `shr r64, cl` | Flags + variable source. AF and CF semantics differ between the two |
| Memory | `mov r64, [rbx+disp32]`; `mov [rbx+rax*4], ecx`; `push r64`; `pop r64` | Addressing modes, scale, width, red-zone/stack interaction |
| Control flow | `jmp rel32`; `jcc rel32`; `call rel32`; `ret` | Block formation, block chaining |
| Encoding edge | `mov eax, imm32` (zero-extends to 64) | The 32-bit-write-zero-extends rule, which is a classic source of divergence |
| Trap | `int3` / `ud2` → `kUnimplementedInstruction` | **Property 9 below** |
| Sequence | A ~40-instruction block touching memory, branches, and flags | Whole-block differential |

**Deliberately excluded from Phase 1**, each for a stated reason:

| Excluded | Reason |
|---|---|
| `imul`, `idiv` | Quotient/remainder edge cases (INT_MIN/-1, divide-by-zero) deserve their own tests, not a footnote |
| String ops (`movs`, `stos`, …) | `rep` prefixes interact with the `DF` flag and with block formation. Non-trivial |
| `lea` | Address computation is interesting but the coverage is already reached by the memory group |
| SSE/AVX | Apple has no 256-bit SIMD; needs a dedicated risk assessment |
| x87 | Own project |
| `syscall` | Meaningless until there is a kernel to call |

---

## 9. The differential harness

This is the part that must be right, because everything later depends on it.

### Shape

```
for each test case:
    bytes  = case.guest_bytes
    expected = case.expected_final_state    # authored, hand-verified

    ir     = Decoder.DecodeBlock(bytes)
          -> ValidateIr(ir)                   # fail loudly on malformed IR

    for backend in BackendRegistry:
        state  = case.initial_state
        memory = case.initial_memory
        st     = backend.Execute(ir, &state, &memory)

        if st == kUnimplementedInstruction:
            # EXPLICIT, NOT a silent skip.
            # A backend that cannot execute something must say so.
            record(coverage_gap = {case, backend})
            continue

        assert st == kOk
        assert CompareState(state, expected) == 0
        assert CompareMemory(memory, expected_memory) == 0
```

### Non-negotiable properties

1. **Both backends must run every case in Phase 1.** With one backend the
   harness is unproven. Phase 1 therefore includes a **deliberately restricted
   second backend** (see §9.1) purely to prove the harness detects divergence.

2. **State comparison is field-by-field, not by memcmp.** `memcmp` over
   `CpuState` would pass while ignoring a wrong `eflags`. Every architectural
   field is compared individually, and failure output names the field, both
   values, and the byte offset.

3. **Memory comparison is bounded and explicit.** Compare only the ranges the
   case declares, plus a guard check that nothing outside them was written.
   A JIT bug that writes out of bounds must be *caught*, not accidentally
   ignored by comparing only the interesting region.

4. **An unimplemented backend is a coverage gap, not a pass.** It is reported
   separately so it can never be confused with correctness.

5. **The expected result is authored, not captured.** Hand-derived from the
   Intel SDM. A harness that records "whatever the interpreter produced" proves
   only that the interpreter is deterministic.

6. **The harness runs under sanitizers in CI.**

### 9.1 The deliberate divergence test

Phase 1 must demonstrate that the harness *can fail*. A harness never observed
to fail is an untested harness.

Phase 1 therefore ships one backend that is correct except for one documented,
named bug — e.g. it fails to set `PF` on `add`. A test asserts:

```
the buggy backend is REJECTED by the harness on exactly these cases
and on no others
```

This is a test of the test infrastructure, and it is deleted the moment it has
served its purpose. It exists because a differential harness that has never
caught anything is indistinguishable from a harness that cannot.

### 9.2 Coverage accounting

Every instruction a backend does not implement is recorded. The report is
reviewed, because silent coverage regression is how emulator projects die:

```
CPU coverage: 18/20 instructions implemented by reference-interpreter
Gaps: SETcc (planned phase 2), CMOVcc (planned phase 2)
```

---

## 10. Test corpus format

Designed so a thousand cases need no code.

```
tests/cpu/cases/<arch>/<mnemonic>/
  <case-name>/
    case.yaml        # guest bytes, initial state, expected state
    README.md        # why this case exists (required, not optional)
```

`case.yaml` carries:

- `guest_bytes` — hex, with a comment naming the exact encoding
- `initial_state` / `initial_memory`
- `expected_state` / `expected_memory`
- `provenance` — **required**. Either `Intel SDM §N` or `hardware-verified` with
  the measurement, or a link to an AMD64 APM section

`provenance` is not bureaucracy. Without it, a wrong expected value survives
forever because nobody re-derives it. With it, a reviewer can check the claim
against a primary source.

The README requirement exists for the same reason: a case with no stated purpose
is a case nobody dares delete, and nobody dares fix when it disagrees with the
architecture.

---

## 11. Milestones within Phase 1

| # | Milestone | Verification |
|---|---|---|
| 1.1 | Extend `GuestMemoryPort` with a simple linear-memory implementation | Unit tests; contract tests pass unchanged |
| 1.2 | IR types + `ValidateIr` | Invariant tests, including deliberately malformed IR that must be rejected |
| 1.3 | Decoder for the instruction set in §4 | Round-trip: decode → re-encode → compare bytes |
| 1.4 | IR builder + passes | Pass-by-pass IR dumps reviewed |
| 1.5 | Reference interpreter | Each instruction against a hand-derived expected result |
| 1.6 | Differential harness + the deliberate-divergence backend | **The harness is proven to fail** |
| 1.7 | Whole-block end-to-end case | Single sequence exercising everything above |
| 1.8 | Documentation: IR spec, decoder coverage, harness usage | Review |

Milestone 1.6 is the gate. If 1.6 is not green, nothing after it is trustworthy.

---

## 12. What Phase 1 explicitly does not prove

Stated plainly so Phase 1 cannot be over-claimed:

- **It does not prove the IR is the right IR.** Only that it is *a* correct IR.
  Phase 2's ARM64 backend may prove it is the wrong shape, and changing an IR is
  expensive. This risk is accepted deliberately: an IR that is slightly wrong but
  correct today beats no IR at all.
- **It proves nothing about ARM64.** No ARM64 code is written or tested.
- **It proves nothing about performance.**
- **It says nothing about PS5 compatibility.** Zero PS5-specific behaviour is
  exercised. PS5 executables are x86-64; that fact is the *only* PS5 input to
  this phase.
- **It does not validate the memory model beyond a linear region.** Real guest
  memory needs page tables, protection bits, and DMA.

---

## 13. Risks

| Risk | Severity | Mitigation |
|---|---|---|
| IR shape is wrong for a real ARM64 backend | **High** | Accepted deliberately. Revisit at ~100 IR ops in Phase 2, before the backend is written |
| Expected values derived from the interpreter rather than the SDM | **High** | `provenance` is a required field. The harness never auto-records expectations |
| Differential harness never proven to fail | **High** | Milestone 1.6 is a hard gate with a deliberate bug |
| Phase 1 expands into "just a few more instructions" | Medium | §4 is an explicit list with explicit exclusions. Adding an instruction is a Phase 2 decision |
| Over-confidence from a green suite | Medium | §12 states explicitly what is not proved |
| SIMD deferred too long | Medium | Called out here and in `cpu-feasibility.md` §6.1 as the largest guest-feature risk |

---

## 14. Recommendation

**Proceed with Phase 1 exactly as scoped.**

The reasoning: Phase 1's job is not to emulate anything. It is to build the
mechanism that makes every later claim falsifiable — a decoder, an IR, a
reference interpreter, and a differential harness that has been *observed to
catch a real divergence*.

Everything expensive in this project (the ARM64 backend, the shader compiler,
the memory model, the iOS distribution story) rests on being able to prove
correctness. Phase 1 builds that capability, on ~20 instructions, in a way that
scales to thousands.

Scope discipline matters more here than speed. The single most likely failure
mode for P5EMU is not technical — it is Phase 1 quietly becoming Phase 4.
