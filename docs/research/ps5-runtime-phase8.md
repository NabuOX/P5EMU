# PS5 Runtime Evidence Ledger (Phase 8)

Phase 8 moves P5EMU's runtime from a purely generic/synthetic model toward an
**evidence-backed PS5 runtime model**, without inventing undocumented PS5
behaviour. This document is the ledger for every PS5 runtime fact considered for
implementation.

**The rule: no PS5-specific implementation without traceable evidence. Unknown
behaviour stays UNKNOWN.**

Phase 8 implements **no** PS5 runtime semantics that this ledger does not
classify as supported. This file records what is known, what is inferred, and —
importantly — what remains unknown.

## Evidence classes

| Class | Meaning |
|---|---|
| **A — Documented fact** | Published in a specification or re-typed from a public, external document. |
| **B — Observed behaviour** | Determined by lawful, reproducible observation. |
| **C — Inference / hypothesis** | Reasoned from other evidence; not yet directly demonstrated. |
| **D — Synthetic P5EMU model** | A P5EMU-owned placeholder. **Not** a PS5 claim. |

Classes are never silently merged. A `C` or `D` item is not promoted to `A`/`B`
without new evidence.

## Source register

All sources below were already admitted by P5EMU provenance policy (see
[`licenses.md`](licenses.md)); Phase 8 reused **no code**, only facts and
architecture understanding. See [`../../PROVENANCE.md`](../../PROVENANCE.md).

| Source | URL / identity | Licence | Used as |
|---|---|---|---|
| System V AMD64 psABI (ELF gABI) | AMD64 ABI, ELF gABI | specification | Documented fact |
| KytyPS5 | `github.com/KytyPS5/KytyPS5` @ `16a7fe6` | GPL-2.0-only | Facts re-typed (no code) |
| Kyty (InoriRus) | `github.com/InoriRus/Kyty` | MIT | Facts re-typed (no code) |
| shadPS4 | `github.com/shadps4-emu/shadPS4` @ `3a9f5726` | GPL-2.0-or-later | Subsystem inventory only |
| PS5 subsystem map (P5EMU) | [`ps5-subsystems.md`](ps5-subsystems.md) | Apache-2.0 | Consolidated inventory |
| Phase 7 evidence audit (P5EMU) | [`ps5-runtime-evidence.md`](ps5-runtime-evidence.md) | Apache-2.0 | Boundary statement |

> No Sony firmware, SDK, key, or game material was consulted. No third-party
> source was copied; the projects above are read as facts and inventory only.

## Runtime ABI gap ledger

`Status` is one of: **IMPLEMENTABLE NOW**, **PARTIALLY KNOWN**, **UNKNOWN**.

### 1. Executable identification / classification

| Fact | Source | Licence | Class | Conf. | Status | Implementation consequence |
|---|---|---|---|---|---|---|
| PS5 dialect `e_type` values `ET_DYNEXEC=0xfe10`, `ET_DYNAMIC=0xfe18` | KytyPS5 `src/loader/elf.h` | GPL-2.0-only | A (external) | high | IMPLEMENTABLE NOW | `ClassifyRuntimeProfile` selects the PS5 profile; `ModuleRole` maps DynExec→executable, Dynamic→shared library |
| `ELFOSABI_FREEBSD = 9` as the PS5 OSABI | System V ABI; KytyPS5 `elf.h` | GPL-2.0-only / spec | A | high | IMPLEMENTABLE NOW | PS5-dialect detection (`is_ps5_dialect`) |
| Generic dynamic tags (`DT_NEEDED`, `DT_RELA`, `DT_SYMTAB`, …) | AMD64 psABI / ELF gABI | spec | A | high | IMPLEMENTABLE NOW | Already parsed in Phase 4/5 |

### 2. Process entry state

| Fact | Source | Licence | Class | Conf. | Status | Implementation consequence |
|---|---|---|---|---|---|---|
| The executable's `e_entry` value is the program's first instruction address | ELF gABI | spec | A | high | IMPLEMENTABLE NOW | `StartupState.entry_rip` = `Known(entry, Documented)` |
| The register/stack *state* PS5 establishes at entry (RSP, RBP, argc/argv, TLS) | — | — | — | — | **UNKNOWN** | PS5 `StartupState` fields are `Unknown`; materialisation is refused |
| Whether PS5 enters at `e_entry` with a standard SysV initial stack | — | — | D→UNKNOWN | low | **UNKNOWN** | Generic builder is a synthetic model only; not a PS5 claim |

### 3. Startup stack, argv/envp/auxv

| Fact | Source | Licence | Class | Conf. | Status | Implementation consequence |
|---|---|---|---|---|---|---|
| PS5 initial RSP value / stack growth / alignment rule | — | — | — | — | **UNKNOWN** | `initial_rsp` / `stack_alignment` = `Unknown` for the PS5 profile |
| PS5 argc / argv / envp presence and layout | — | — | — | — | **UNKNOWN** | `argc`/`argv`/`envp` = `Unknown` for the PS5 profile |
| PS5 auxv-like data and its key set | — | — | — | — | **UNKNOWN** | `auxv` = `Unknown`; raw metadata preserved, not interpreted |
| SysV-style argc/argv/envp/auxv layout | System V AMD64 psABI | spec | A | high | IMPLEMENTABLE NOW (**generic profile only**) | `BuildInitialStack`; classed `Synthetic` when applied |

### 4. TLS / thread pointer

| Fact | Source | Licence | Class | Conf. | Status | Implementation consequence |
|---|---|---|---|---|---|---|
| PS5 TLS model, thread pointer (FS/GS base), initial TCB layout | — | — | — | — | **UNKNOWN** | `thread_pointer` = `Unknown`; `CheckThreadPointerSupport` fails with `kRuntimeProfileUnsupported`. **No fake thread pointer.** |
| PS5 thread/creator startup semantics | — | — | — | — | **UNKNOWN** | Not represented beyond an explicit gap |

### 5. Module metadata / dependencies / init

| Fact | Source | Licence | Class | Conf. | Status | Implementation consequence |
|---|---|---|---|---|---|---|
| Module dependency *names* via generic `DT_NEEDED` | ELF gABI | spec | A | high | IMPLEMENTABLE NOW | `ModuleMetadataInfo.dependencies` |
| Presence of Sony import/export metadata (`DT_OS_IMPORT_LIB`, `DT_OS_EXPORT_LIB`) | KytyPS5 `elf.h` | GPL-2.0-only | A (existence) | high | PARTIALLY KNOWN | `declares_sce_imports`/`exports` flags; **structure UNKNOWN** |
| Sony `DT_OS_*` and PS5 `_1` tag *numbers* | KytyPS5 `elf.h` | GPL-2.0-only | A (numbers) | high | PARTIALLY KNOWN | Tag numbers preserved verbatim; `_1` semantics UNKNOWN |
| Module initialization / finalization ordering and hooks | — | — | — | — | **UNKNOWN** | `initialization_metadata`/`finalization_metadata` = `Unknown`; `InitOrderModel::kPs5Unknown` |
| Sony symbol / NID algorithm and import structure layout | — | — | — | — | **UNKNOWN** | `SymbolIdentity` keeps `NumericId`/`OpaqueId`/`Unknown`; no NID algorithm invented |

### 6. Address space / load bias

| Fact | Source | Licence | Class | Conf. | Status | Implementation consequence |
|---|---|---|---|---|---|---|
| PS5 load-address policy / ASLR | — | — | — | — | **UNKNOWN** | `Ps5LoadBiasEvidence()` = `Unknown`; the Phase-7 `LoadBiasAllocator` is labelled SYNTHETIC, **not** PS5 ASLR |
| PS5 address-space region taxonomy | shadPS4 (inventory) | GPL-2.0-or-later | C | low | **UNKNOWN** | Not implemented; synthetic regions only |

### 7. Dynamic linking

| Fact | Source | Licence | Class | Conf. | Status | Implementation consequence |
|---|---|---|---|---|---|---|
| x86-64 relocation types and formulas (`R_X86_64_*`) | AMD64 psABI §4.4 | spec | A | high | IMPLEMENTABLE NOW | Phase 5 (unchanged) |
| Generic import/export resolution policy | P5EMU design | Apache-2.0 | D | n/a | IMPLEMENTABLE NOW | Guest-export-first then HLE; ambiguity is a failure |
| PS5 dynamic linker resolution *order* | — | — | — | — | **UNKNOWN** | Not claimed; resolution is explicit and testable, not PS5-faithful |

## Explicitly UNKNOWN (do not fabricate)

- PS5 initial RSP value, stack base/growth, and entry alignment.
- PS5 argc/argv/envp/auxv presence, layout, and the auxv key set.
- PS5 TLS/thread model and the initial thread-control-block layout.
- PS5 module initialization/finalization ordering and hook mechanism.
- PS5 load-address / ASLR policy and address-space layout.
- The Sony NID algorithm and Sony symbol/import structure layout.
- PS5-era `DT_OS_*_1` variant semantics.

## Generic-vs-PS5 boundary (implemented)

Phase 8 introduces an explicit separation:

- **Generic/synthetic profile** (`RuntimeProfileKind::kGenericSynthetic`): the
  Phase-7 SysV-style model. Every field is `Known` and classed `Synthetic`.
  Materialises successfully for tests and E2E.
- **PS5 profile** (`RuntimeProfileKind::kPs5`): `entry_rip` is `Known`
  (Documented); every other required field is `Unknown`. **Materialisation is
  refused** (`kUnknownRuntimeField`) with a blocker list — never fabricated.

Generic SysV assumptions can therefore no longer silently become PS5 claims:
only the generic profile yields concrete startup values, and a PS5 profile fails
explicitly until evidence supports each field.

## Non-claims

Phase 8 does **not** claim: commercial-game compatibility, complete PS5
emulation, or PS5-faithful startup/runtime/linker/TLS behaviour. It provides an
evidence-bounded runtime model that fails explicitly wherever the PS5 evidence
is absent.
