# P5EMU

An independent, experimental PlayStation 5 emulator architecture, designed from
day one for portability, with ARM64 and ultimately iPhone/iPad as important
long-term targets.

**Status: Phase 0–12.** The architecture foundation (Phases 0–7) is complete,
Phase 8 adds an **evidence-bounded PS5 runtime model**, Phase 9 adds a **typed,
evidence-safe representation of PS5/SCE executable and dynamic-linking metadata**,
Phase 11 expands the x86-64 CPU (ADC/SBB/MUL/IMUL/DIV/IDIV, a synchronous CPU
fault model, PUSH/POP/LEAVE, AH/CH/DH/BH), and Phase 12 adds a **host-independent
SIMD foundation** (XMM0–XMM15 `Vec128` state, MOVD/MOVQ/MOVDQA/MOVDQU,
PXOR/PAND/POR/PADDQ). The pipeline runs end to end on synthetic inputs: x86-64
decode → P5EMU IR → { reference interpreter | AArch64 backend }, with a
transactional ELF/relocation loader, an HLE guest-to-host bridge, and a
multi-module, multi-region process runtime. PS5 runtime and dynamic-link facts are
represented explicitly as known or UNKNOWN; undocumented behaviour is not
fabricated. **No commercial game runs today; there is no PS5 compatibility claim;
and no Sony material is present.** See [Licence](#licence) for the current terms.

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

## Status at a glance

**Implemented today (verified):** a documented x86-64 decoder subset; the P5EMU
SSA IR plus its validator; a reference interpreter that is the differential
oracle; the AArch64 emitter, code generator and backend, executed
independently under `qemu-aarch64`; differential, randomized, fuzz and property
test infrastructure; a generic ELF64 + PS5-layer loader; relocations and an
import resolver; the HLE guest-to-host bridge; and a transactional, multi-module,
multi-region process runtime.

**Research in progress:** the GPU path (RDNA 2 / shader translation), Apple
platform constraints (W^X / JIT), and the evidence audits that keep PS5 behaviour
honestly tagged. See [`ROADMAP.md`](ROADMAP.md).

**Long-term goals:** a portable, backend-independent PS5 emulator architecture
that can run on Apple ARM64, built by a broad, collaborative community. None of
this is a compatibility or availability claim today.

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
- A self-contained test harness (103 CTest suites), a provenance gate, and a
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
| [`docs/architecture/cpu.md`](docs/architecture/cpu.md) | CPU pipeline, supported instruction families/forms, stack family, deferred arithmetic families |
| [`docs/development.md`](docs/development.md) | Development guide: environment, build, tests, sanitizers, AArch64/qemu, provenance |
| [`docs/research/ps5-runtime-evidence.md`](docs/research/ps5-runtime-evidence.md) | PS5 startup/runtime evidence audit — the P5EMU-vs-PS5 boundary |
| [`docs/research/ps5-runtime-phase8.md`](docs/research/ps5-runtime-phase8.md) | Phase 8 evidence ledger — per-fact PS5 runtime classification (known/UNKNOWN) |
| [`docs/research/ps5-dynamic-linking-phase9.md`](docs/research/ps5-dynamic-linking-phase9.md) | Phase 9 evidence ledger — per-tag SCE dynamic metadata (number vs semantics) |
| [`docs/research/licenses.md`](docs/research/licenses.md) | License matrix, classifications A–D, the differential argument |
| [`docs/research/cpu-feasibility.md`](docs/research/cpu-feasibility.md) | IR design comparison, memory models, W^X, can FEX run on iOS |
| [`docs/research/ios-jit-audit.md`](docs/research/ios-jit-audit.md) | Execution-environment matrix, App Store constraints, tiered strategy |
| [`docs/research/gpu-architecture.md`](docs/research/gpu-architecture.md) | MoltenVK/Metal gaps, phased backend strategy |
| [`docs/research/ps5-subsystems.md`](docs/research/ps5-subsystems.md) | Subsystem map with PROVEN / EXT / INFERRED / UNKNOWN tagging |
| [`docs/research/phase1-design.md`](docs/research/phase1-design.md) | Phase 1 milestone design — CPU core, IR, decoder, differential harness |
| [`docs/research/testing-philosophy.md`](docs/research/testing-philosophy.md) | Differential testing strategy and corpus licence hygiene |

---

## Contributing

P5EMU is preparing for a public, collaborative open-source release. If you would
like to help:

- [`CONTRIBUTING.md`](CONTRIBUTING.md) — where to contribute, the evidence
  standard, and the provenance rules.
- [`ROADMAP.md`](ROADMAP.md) — work areas, with good first contributions.
- [`docs/development.md`](docs/development.md) — build, test, sanitizers, the
  AArch64/qemu harness, and where the documentation lives.
- [`CODE_OF_CONDUCT.md`](CODE_OF_CONDUCT.md) — how we collaborate.
- [`SECURITY.md`](SECURITY.md) — how security issues will be reported.

Contributions must respect the provenance rules: no Sony SDK/firmware/keys/game
material, and no copied incompatible source.

---

## Licence

P5EMU is open-source software licensed under the **Apache License 2.0**. See
[`LICENSE`](LICENSE) for the full text and [`NOTICE`](NOTICE) for the copyright
notice.

```
Copyright 2026 P5EMU contributors
```

The Apache-2.0 licence includes an express patent grant and permits commercial
and non-commercial use, modification, and redistribution (with the usual
attribution and NOTICE obligations). It does not grant trademark rights.

P5EMU contains no third-party code. Projects studied for research, and external
test tools, keep their own licences and are not relicensed by P5EMU — see
[`PROVENANCE.md`](PROVENANCE.md) and
[`docs/research/licenses.md`](docs/research/licenses.md). No Sony material is
present, and copyleft source is never acceptable in this repository.

---

## Relationship to other projects

P5EMU is independent. It is not affiliated with, endorsed by, or derived from
Sony Interactive Entertainment. It studied the work of FEX, QEMU, PCSX2, RPCS3,
dynarmic, Play!, PPSSPP, Kyty, KytyPS5, shadPS4, Madeira, UTM, MoltenVK,
SPIRV-Cross, LLVM and VIXL — see [`ACKNOWLEDGEMENTS.md`](ACKNOWLEDGEMENTS.md).
