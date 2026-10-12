# P5EMU

An independent PlayStation 5 emulator, designed from day one for ARM64 and
ultimately iPhone/iPad.

**Status: Phase 6 — HLE foundation and guest-to-host call bridge. A synthetic
guest executable's import is bound to an HLE binding (a guest trampoline
address), the guest `CALL` reaches the dispatcher, a P5EMU-owned host
implementation runs, the guest return value is delivered, and execution resumes.
The architecture is backend-independent (reference interpreter and AArch64).
No Sony HLE is implemented; no Sony material is present.**

```
PS5 x86-64 guest
      ↓
P5EMU loader / memory / HLE
      ↓
P5EMU CPU execution layer
      ↓
x86-64 decode → P5EMU IR → { Interpreter | ARM64 backend }
                                      ↓
                                 Apple ARM64
```

---

## What exists right now

Phase 1 CPU pipeline, tested end to end:

- **`X64Decoder`** — a documented x86-64 subset: prefixes, ModRM, SIB, REX,
  displacements, immediate and control-flow forms. Unsigned and unsupported
  encodings are rejected, never guessed at.
- **`TranslateToIr`** — x86-64 semantics lowered to P5EMU SSA IR: ALU and flag
  behaviour, register aliasing and 32-bit zero-extension, load/store, LEA,
  RIP-relative addressing, branches, CALL/RET.
- **`Ir` + `ValidateIr`** — the shared typed IR, with detailed validation that
  runs before any backend executes.
- **`ReferenceInterpreter`** — executes IR only (never an encoding), and is the
  differential oracle.
- **`Differential` harness** — compares backends field by field, with a
  deliberately buggy backend proving the harness can actually detect a
  divergence.
- A self-contained test harness (57 CTest suites), a provenance gate, and a
  research corpus.

Phase 2 adds the first real execution backend:

- **`Arm64Emitter`** — a small, independently authored AArch64 encoder for the
  emitted subset, verified against an independent disassembler.
- **`Arm64CodeGen`** — lowers the same validated IR to AArch64 (stack-slot
  values, synthesised flags, helper-called memory, no register allocator).
- **`Arm64Backend`** — implements the Phase-1 `Backend` contract; consumes the
  same IR as the interpreter and reports a coverage gap for anything outside the
  subset, never a fabricated result.
- **Independent execution** — emitted AArch64 runs under `qemu-aarch64` via a
  cross-compiled harness (test infrastructure, not linked or vendored), and its
  result is compared field-by-field against the reference interpreter.

Phase 3 expands CPU semantics:

- **8/16/32/64-bit operand widths** with real width-specific flag production,
  partial-register read/write rules, and memory access width.
- **SHL/SHR/SAR** with correct x86 count masking (5 bits, or 6 for 64-bit) and
  shift flags, immediate and `CL` counts.
- **Architecturally undefined flag bits** excluded from comparison through a
  fixture-specific observable mask, never by inventing values.
- **Batched randomized differential testing** — thousands of cases through one
  AArch64 execution, and coverage-gap results cleanly separated from real
  divergences.

Phase 4 adds the executable loading foundation:

- **`BinaryReader`** — bounds- and overflow-checked, endian-aware parsing of
  untrusted input.
- **Generic ELF64** (`Elf64`) fully separated from the **PS5/Prospero** layer
  (`Ps5Executable`), which recognizes the PS5 dialect and preserves unknown
  dynamic tags without inventing semantics.
- **`LoadPlan`** — an immutable, fully validated description (R/W/X intent,
  explicit BSS, unresolved module requirements) applied **transactionally** to a
  permission-enforcing `MappedGuestMemory`.
- **End-to-end** — a synthetic ELF64 fixture loads, the CPU fetches its code
  from guest memory at the ELF entry point, and runs it to a known result
  (interpreter and AArch64).

Phase 5 adds relocations and the import/resolution contract:

- **Generic x86-64 relocations** (`R_X86_64_NONE/64/RELATIVE/GLOB_DAT/JUMP_SLOT`,
  RELA only) with an explicit **load-bias** model, overflow-checked arithmetic,
  and transactional application staged before final guest protections.
- **Import requirements** in a P5EMU-owned, Sony-layout-independent model with a
  typed symbol identity, and a **resolver** that distinguishes
  resolved/unresolved/unsupported/ambiguous/error. A required unresolved import
  blocks execution readiness — never a silent address zero.

See [`docs/architecture/loader.md`](docs/architecture/loader.md) and
[`docs/architecture/relocations.md`](docs/architecture/relocations.md).

Phase 6 adds the HLE foundation:

- **HLE subsystem** (`HleRegistry`, `HleBinding`, `HleCallContext`,
  `HleDispatcher`, `HleResolver`) separate from the loader and CPU.
- **Guest-to-host bridge** through deterministic **guest trampoline addresses**
  (a P5EMU-owned range), never raw host pointers.
- **`kExternalCall` IR** reached by a real guest `CALL`; the interpreter and the
  AArch64 backend both dispatch it and resume the guest.
- **Synthetic P5EMU-owned providers** (`p5emu_add`/`p5emu_sum6`/`p5emu_memory`)
  — no Sony implementation.

See [`docs/architecture/hle.md`](docs/architecture/hle.md).

Phase 7 adds the runtime and multi-module foundation:

- **Multi-module loading** (`LoadProcess`) — a deterministic dependency order,
  a synthetic load-bias allocator, and one resolution policy (a module's own
  exported symbols first, then HLE). The whole load is **transactional**: a
  failed load is never partly visible.
- **Guest stack and process startup** — a checked R/W stack and a generic
  SysV-style initial stack. The exact PS5 layout is documented as unknown, not
  invented (`docs/research/ps5-runtime-evidence.md`).
- **Multi-region AArch64 transport** (harness protocol v2) — the out-of-process
  backend carries the whole guest address space, with per-region permissions
  enforced in the harness. A cross-module `CALL` to a guest symbol becomes
  intra-program control flow; HLE calls remain external.
- **Proof** — a guest->guest->HLE program reaches the same result and
  termination reason through the interpreter and through AArch64.

See [`docs/architecture/runtime.md`](docs/architecture/runtime.md).

See [`docs/architecture/v1.md`](docs/architecture/v1.md) §8 for acceptance
criteria and [`docs/research/phase1-design.md`](docs/research/phase1-design.md)
for the Phase 1 design.

## Quick start

```bash
scripts/verify.sh              # configure, build, test (warnings as errors)
scripts/verify.sh --asan       # same under AddressSanitizer
scripts/verify.sh --release    # optimised
scripts/check_provenance.sh    # provenance gate
```

Verified on WSL2 / Ubuntu with GCC 15.2.0 and Clang 21.1.8, `-Werror`, ASan and
UBSan. No third-party dependencies; configure requires no network access.

---

## An independent project

P5EMU is **not** a fork of any existing emulator, and no third-party source has
been copied, vendored, or adapted into it.

Every existing PlayStation emulator is GPL. Every interesting technique P5EMU
needs has therefore been demonstrated only in GPL code. P5EMU reads those
projects for *architecture* and writes every line itself. Facts — address
constants, ELF tag values, packet opcodes, instruction encodings — may be
learned, re-derived, and re-typed. GPL source lines may not be copied, with or
without attribution.

- Full analysis with upstream URLs and pinned commits:
  [`docs/research/licenses.md`](docs/research/licenses.md)
- Adaptations: [`PROVENANCE.md`](PROVENANCE.md) — the adaptations register is
  empty, by design: no third-party source has been adapted into P5EMU
- Mechanical enforcement: `scripts/check_provenance.sh`

**No Sony material is present or permitted**: no firmware, keys, sysmodules,
SDKs, game data, trademarks, UI assets, or sounds. See
[`docs/research/ps5-subsystems.md`](docs/research/ps5-subsystems.md) for the
boundary between public knowledge and prohibited material.

---

## Engineering rules

1. **Evidence over guesses.** Every non-obvious claim carries a URL. Unknown
   things are labelled `UNKNOWN`, not quietly invented.
2. **The interpreter is the specification, not a fallback.** It is the
   differential oracle, and the only execution tier Apple permits on iOS without
   a debugger.
3. **No compatibility claims that have not been demonstrated.**
4. **No weakening of tests to make them pass.** A failing test is information.
5. **No game-specific hacks.** Fixes address the underlying emulation behaviour.
6. **No commit or push without explicit authorisation.**
7. **Human review of AI-generated code is mandatory and recorded.**

---

## Documentation

| Document | Contents |
|---|---|
| [`docs/architecture/v1.md`](docs/architecture/v1.md) | Component boundaries, layer model, deliberate non-decisions |
| [`docs/architecture/arm64-backend.md`](docs/architecture/arm64-backend.md) | AArch64 backend: ABI, emitter decision, subset, flags, W^X, execution oracle |
| [`docs/architecture/loader.md`](docs/architecture/loader.md) | PS5 executable loader: format boundary, plan, transactional apply, PS5 fact provenance |
| [`docs/architecture/relocations.md`](docs/architecture/relocations.md) | Relocations, load bias, import requirement model, resolver lifecycle |
| [`docs/architecture/hle.md`](docs/architecture/hle.md) | HLE: identity, registry, guest ABI, dispatch, trampolines, reentrancy |
| [`docs/architecture/runtime.md`](docs/architecture/runtime.md) | Runtime: address space, stack, process startup, module graph, load bias, resolution, multi-region transport |
| [`docs/research/ps5-runtime-evidence.md`](docs/research/ps5-runtime-evidence.md) | PS5 startup/runtime evidence audit — the P5EMU-vs-PS5 boundary |
| [`docs/research/licenses.md`](docs/research/licenses.md) | License matrix, classifications A–D, the differential argument |
| [`docs/research/cpu-feasibility.md`](docs/research/cpu-feasibility.md) | IR design comparison, memory models, W^X, can FEX run on iOS |
| [`docs/research/ios-jit-audit.md`](docs/research/ios-jit-audit.md) | Execution-environment matrix, App Store constraints, tiered strategy |
| [`docs/research/gpu-architecture.md`](docs/research/gpu-architecture.md) | MoltenVK/Metal gaps, phased backend strategy |
| [`docs/research/ps5-subsystems.md`](docs/research/ps5-subsystems.md) | Subsystem map with PROVEN / EXT / INFERRED / UNKNOWN tagging |
| [`docs/research/phase1-design.md`](docs/research/phase1-design.md) | Phase 1 milestone design — CPU core, IR, decoder, differential harness |
| [`docs/research/testing-philosophy.md`](docs/research/testing-philosophy.md) | Differential testing strategy and corpus licence hygiene |

---

## Licence

**Proprietary, all rights reserved.** See [`LICENSE.md`](LICENSE.md). The project
was formerly a Phase-0 candidate for Apache-2.0; that was superseded by the
decision to keep P5EMU proprietary. No third-party code is vendored, and
copyleft source is never acceptable here.

---

## Relationship to other projects

P5EMU is independent. It is not affiliated with, endorsed by, or derived from
Sony Interactive Entertainment. It studied the work of FEX, QEMU, PCSX2, RPCS3,
dynarmic, Play!, PPSSPP, Kyty, KytyPS5, shadPS4, Madeira, UTM, MoltenVK,
SPIRV-Cross, LLVM and VIXL — see [`ACKNOWLEDGEMENTS.md`](ACKNOWLEDGEMENTS.md).
