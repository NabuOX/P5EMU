# P5EMU License & Provenance Matrix

**Phase 0 research artefact.** Every claim here is tied to an upstream URL. Items
that could not be verified are marked `UNVERIFIED` rather than guessed.

Classification scheme:

| Class | Meaning |
|---|---|
| **A — Directly reusable** | Permissive licence, technically appropriate. Can be imported as a dependency with attribution. |
| **B — Reusable with obligations** | Permissive but with specific duties (attribution, NOTICE, source-disclosure of modifications, patent). Usable, but each import is a legal event that must be logged. |
| **C — Research/architectural reference only** | Read to understand. Do not copy code. Includes all GPL, and permissive code whose technical assumptions do not fit P5EMU. |
| **D — Do not use** | Prohibited, unavailable, or legally/technically unsalvageable. |

The governing principle: **credit does not make incompatible code reusable.**
GPL code read by a non-GPL project produces a derivative work the moment a line
is transcribed, even with attribution. Attribution is necessary, never
sufficient.

---

## 1. Executors and IR / JIT engines

### 1.1 FEX — `FEX-Emu/FEX`

| Field | Finding |
|---|---|
| Upstream | https://github.com/FEX-Emu/FEX |
| Pinned commit | `14c92681f4d62cf84d901460e0358de09c8847a7` (2026-10-07) |
| Licence | **MIT** — https://github.com/FEX-Emu/FEX/blob/main/LICENSE, `Copyright (c) 2019 Ryan Houdek <Sonicadvance1@gmail.com>`, GitHub API `spdx_id: MIT` |
| Guest / host | x86-64 guest, **AArch64 host is the only CI target** (`FEXCore/Readme.md`: "The target host architecture for this library is AArch64") |
| IR | Custom **SSA** IR, 433 ops in 14 groups, generated from `FEXCore/Source/Interface/IR/IR.json` |
| Codegen | vixl macro "splatter", **no instruction selector** (`docs/SourceOutline.md`) |
| Register allocation | **As IR ops**: `AllocateGPR`, `AllocateGPRAfter`, `AllocateFPR`, `SpillRegister`, `FillRegister` |
| x86 decoder | Hand-written table-driven, `FEXCore/Source/Interface/Core/Frontend.cpp` + `X86Tables/`. **Xbyak is not used**; `FEXCore/docs/CPUBackends.md` claiming it is, is stale |
| x86 TSO | `LoadMemTSO`/`StoreMemTSO` + signal-handler backpatching for unaligned ARM faults |
| Code cache | **RWX single mapping, no split W^X, no `MAP_JIT`** (`AllocatorHooks.h:117-139`) |
| Memory model | Guest VA = host VA + base register held in `X1`; host allocator reads `/proc/self/maps` and `prctl(PR_SET_VMA)` |
| iOS | Upstream has **no** Darwin/iOS support. Maintainer, discussion #3267 (2023-11-11, Accepted Answer): "We still don't have plans to support MacOS." |

**Verdict: C, with B available for a narrow slice.**

Technically unsuitable as a dependency: it is Linux/glibc-bound in its
allocator (`/proc/self/maps`, glibc `malloc` hooks), its loader, and its entire
syscall and thunk layer. Its code cache is unconditionally RWX, which is fatal
on iOS. Embedding it would mean rewriting the platform layer anyway.

The MIT licence means the IR/JIT/RA core is legally liftable, and it is the
single most valuable study target in the whole ecosystem. But its AArch64
backend can only be tested on AArch64 hardware, so the fast path cannot be
developed on the current x86-64 development host.

> **Decision:** P5EMU reads FEX for design and writes its own decoder, IR, and
> code cache. See [`cpu-feasibility.md`](cpu-feasibility.md) F-07.

### 1.2 QEMU TCG

| Field | Finding |
|---|---|
| Upstream | https://gitlab.com/qemu-project/qemu |
| Pinned commit | master `615ece3c406b262996e569f29ea6eed0d81c4d8d` (2026-10-09) |
| Licence | **GPL-2.0-or-later** (`COPYING` line 245: "any later version"). GitHub mirror reports `NOASSERTION` |
| Critical nuance | `COPYING`/`LICENSE` point 3: *"The Tiny Code Generator (TCG) is mostly under the BSD or MIT licenses; but some parts may be GPLv2 or other licenses."* |
| IR | TCG macro-op IR over temp registers, **not SSA**. No cross-block optimisation |
| Blocks | TB ends at every control-flow or state change; direct chaining is same-page only |
| W^X | **Darwin recipe already present** in `tcg/region.c`: `PROT_NONE` start, `MAP_PRIVATE\|MAP_ANONYMOUS`, `+ MAP_JIT` under `#ifdef CONFIG_DARWIN`, with the comment *"Applicable to both iOS and macOS (Apple Silicon)"* |
| ARM64 host | Supported (`docs/about/build-platforms.rst` lists `hvf, kvm, tcg, whpx, xen` for Arm) |
| iOS | Not a listed platform. `configure` has **no `ios` token**. Patch series *"tcg: implement JIT for iOS and Apple Silicon"* (qemu-devel, 2020-11) exists — **whether merged is UNVERIFIED** |

**Verdict: C.**

Two reasons, either sufficient. Legally, TCG is *"mostly"* permissive but not
uniformly, so every file would need individual audit and any GPLv2 hit taints
the result. Technically, TB-granular translation with a Linux-signal-based SMC
mechanism is the wrong shape for PS5-class code, and its per-translation RWX↔RX
`mprotect` flip is *worse* on iOS than a `MAP_JIT` toggle.

One concrete takeaway is worth recording: the Darwin code-buffer code path in
`tcg/region.c` is the reference implementation for a `MAP_JIT` code cache.

### 1.3 PCSX2

| Field | Finding |
|---|---|
| Upstream | https://github.com/PCSX2/pcsx2 |
| Pinned commit | `c53b0c9c924331b02eca7b74addb8cca4d39d329` (2026-10-10) |
| Licence | **GPL-3.0-or-later** (`COPYING.GPLv3`; in-tree headers say `SPDX-License-Identifier: GPL-3.0+`) |
| ARM64 host | **Incomplete by the project's own admission.** `CMakeLists.txt:84` emits: *"UNSUPPORTED CONFIGURATION … Apple Silicon support in PCSX2 is INCOMPLETE. There are currently no EE/VU/IOP recompilers, and games will run VERY slow."* The ARM64 source list is four files |
| IR | None. Hand-written per-host macro emitters in `common/emitter/` |
| HLE | **Closed source and not in this repository** at this commit (`rg HLE CMakeLists.txt` → 0 hits) |

**Verdict: C — and the single most important negative result in this matrix.**

PCSX2 is the proof that a hand-written-per-host recompiler with no portable IR
turns "add a second host architecture" into a multi-year project. P5EMU's
target is ARM64 *first*, so this is the failure mode P5EMU must design against.

The one transferable idea is `microVU_IR.h`: a small domain IR that feeds two
independent emitters.

### 1.4 RPCS3

| Field | Finding |
|---|---|
| Upstream | https://github.com/RPCS3/rpcs3 |
| Pinned commit | `2ba2f75bcea7ef6bdd687046c7fcfb790faf9853` (2026-10-11) |
| Licence | **GPL-2.0** (`LICENSE` is GPLv2 text; whether "or later" is carried is **UNVERIFIED**) |
| IR | **LLVM IR**, compiled via ORJIT. `rpcs3/Emu/CPU/CPUTranslator.*`, `Backends/AArch64/AArch64JIT.*` |
| ARM64 host | First-class: Linux aarch64, macOS arm64 (`macos-15`), Windows arm64 CI |
| Memory model | ~**52 GiB of fixed-address host reservations** in `rpcs3/Emu/Memory/vm.cpp` (`g_base_addr`, `g_exec_addr`, `g_hook_addr`, `g_stat_addr`) |

**Verdict: C.**

The LLVM-IR-as-translation-contract decision is instructive, and the project
structure (TS components, per-component test harnesses, documentation culture) is
an organisational blueprint.

The memory model is a hard blocker on iOS: fixed multi-gigabyte `MAP_FIXED`-style
reservations at hardcoded host addresses are not viable in the App Store sandbox.

Note also that RPCS3's README carries an explicit "AI Use" policy requiring
human ownership of contributed code. P5EMU adopts an equivalent rule; see
[`architecture/v1.md`](../architecture/v1.md).

### 1.5 dynarmic (x86-64 → AArch64) — **status warning**

| Field | Finding |
|---|---|
| Pinned commit | `github.com/lioncash/dynarmic` @ last push 2024-03-05, **archived** |
| Licence | **0BSD** |
| Upstream status | `pretzelrpc/dynarmic`, `merryhime/dynarmic`, `MerryMage/dynarmic` all **404**. Mirrors: `lioncash/dynarmic` (archived), `yuzu-mirror/dynarmic`, `suyu-emu/dynarmic` |
| Guest | **x86-64** |
| Memory model | Bring-your-own-memory via `UserCallbacks` / `ContiguousGuestMemory` |
| Apple code cache | Already correct: `#ifdef MAP_JIT`, separate RX/RW address spaces gated on `DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT \|\| __APPLE__ \|\| __OpenBSD__` |
| Blocks | `BlockOfCode` with `IsSingleBlock` / `MaxFastBlockSize` fast-block linking |

> **Upstream availability is the blocker, not the licence.** Vendoring an
> archived library with no maintainer and no security/bugfix channel is a
> supply-chain decision that must be made explicitly, not by accident.

**Verdict: B if pinned to a fork; otherwise C.**

If P5EMU wants a code-cache or block-linking reference implementation, this is
the best-licensed and most directly relevant one available. Vendoring requires
pinning a fork, recording the pin, and accepting that P5EMU owns its
maintenance.

### 1.6 Play! (PS2)

| Field | Finding |
|---|---|
| Upstream | https://github.com/jpd002/Play- (**note:** `github.com/RPCS3/Play-` is 404) |
| Pinned commit | `jpd002/Play-` @ default branch `master`, HEAD 2026-10 |
| Licence | **BSD-2-Clause** — `License.txt`, `Copyright (c) 2006-2026, Jean-Philip Desjardins` |
| IR | None. Hand-written multi-host "Jitter" code generator |
| AArch64 backend | `Play--CodeGen/src/AArch64Assembler.cpp` — a full hand-written AArch64 assembler |
| **Testing** | **~70-file differential suite.** Each test subclasses `CTest` with `Run()` (native C++ reference) and `Compile(Jitter&)` (emits JIT code). Registered in CI as `CodeGenTestSuite`, **including under Emscripten/WASM in Node** |

**Verdict: A (as a test-design template) / B (as a code reference).**

This is the most valuable *methodological* result in the matrix. Play! proves the
"native reference function vs JIT-emitted function, assert both, run on every
host backend in CI" pattern works at scale for a hand-written code generator.

The BSD-2-Clause `AArch64Assembler` is genuinely reusable code (classification
B: attribution + modified-file marking). Note its unusual encoding: `wZR` is 31
while `xZR` is 30 and `xSP` is 31. Verify before relying on it.

### 1.7 PPSSPP

| Field | Finding |
|---|---|
| Upstream | https://github.com/hrydgard/ppsspp |
| Licence | **GPL-2.0-or-later** (`LICENSE.TXT`: BSD-3 PSPSDK header followed by full GPLv2 text) |
| IR | Custom IR at `Core/MIPS/IR/` — **there is no top-level `ir/` folder** in current master |
| AArch64 backend | `Common/Arm64Emitter.{h,cpp}` — hand-written, **not** Dynarmic |
| **Testing** | Cross-backend differential: `Core/MIPS/IR/IRInterpreter.cpp` is a full IR interpreter, driven by `headless/Compare.cpp` behind the `--compare` flag. `test.py` scales per-backend timeouts `{'interpreter':20, 'ir':10, 'jit':5, 'jit-ir':5}` |
| Hardware ground truth | `docs/pspautotests-hardware.md`: connect a real PSP via PSPLink, record stdout into `.expected` |

**Verdict: C.**

The method is the contribution: *the emulator contains a full IR interpreter
precisely so that the JIT can be differentially tested against it.* That is the
single strongest argument for P5EMU's mandatory `ReferenceInterpreter`.

The `known_failures` table is instructive too — it is explicitly the list of
architectures reachable *only through emulation* (riscv64, loongarch64 FPU
NaN/FTZ differences). P5EMU will hit exactly this class of problem on AArch64.

---

## 2. PlayStation-family projects

### 2.1 KytyPS5

| Field | Finding |
|---|---|
| Upstream | https://github.com/KytyPS5/KytyPS5 |
| Pinned commit | `16a7fe689a642deeeef85a706e7da305eca6fb85` (2026-10-11) |
| Licence | **`GPL-2.0-only`** per README. The `LICENSE` file is verbatim FSF GPLv2 with **no copyright header and no grant line**; `LICENSES/Kyty-MIT.txt` shows the MIT upstream was relicensed into GPL for the whole |
| Host CPU | **x86-64 only.** CI asserts x86-64 ELF artifacts (`build.yml:413`) and `lipo -verify_arch x86_64` (`:263`). macOS is x86-64 under Rosetta 2 |
| CPU execution | **No JIT, no Dynarmic, no LLVM, no interpreter.** Native x86-64 + static instruction patching (`src/loader/guestInstructionPatcher.h`, `jit.h` byte stubs). Only escape hatch: `x64InstructionEmulator.h::TryEmulate()` |
| Loader | ELF64, FreeBSD-derived Sony ABI. `ET_DYNEXEC=0xfe10`, `ELFOSABI_FREEBSD=9`, `DT_OS_*` tags in `0x61xxxxxx` (`src/loader/elf.h:17-137`) |
| Memory | 16 KiB pages. Guest VA == host VA. System-managed `0x40000–0x7ffffbfff`, system-reserved to `0x0fffffffff`, user `0x1000000000–0xfbffffffff`. `kExtendedMemoryBase=0x080000000000`, 512 GiB (`src/kernel/memory.h:27-28`) |
| GPU | Vulkan 1.3 only. PM4-type-2 packets. RDNA2 decoder → CFG → SSA IR (8 passes) → hand-written SPIR-V. SPIRV-Tools only; glslang is build-time only |
| HLE | Symbol-database HLE (`LIB_VERSION`/`LIB_FUNC`), ~39 libraries, per-symbol version resolution |
| Derivation | `src/loader/guestInstructionPatcher.h:1-2` carries `SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project` |

**Verdict: C — and note it is GPL-2.0-*only*.**

The "only" matters. `GPL-2.0-only` code cannot be relicensed to GPL-3.0. Taking
even one line would lock P5EMU to GPL-2.0 forever, foreclosing GPL-3.0-only
dependencies (most modern tooling) and, more importantly, creating an App Store
conflict.

Its **subsystem inventory** is the most valuable output available: it is a
complete, evidence-cited list of what a PS5 emulator must implement. That
inventory is reproduced with source citations in
[`ps5-subsystems.md`](ps5-subsystems.md).

Also relevant: `crafkit` appears **zero times** in the source tree. The
emulator consumes crafkit's *output*; crafkit itself is a Sony build tool and
is not something P5EMU will ever touch.

### 2.2 Kyty (InoriRus)

| Field | Finding |
|---|---|
| Upstream | https://github.com/InoriRus/Kyty |
| Pinned commit | `4733b7e1c91b10554a52007903d74dc76c39a230` |
| Licence | **MIT** — `Copyright (c) 2021 InoriRus` |
| Status | Appears unmaintained. **UNVERIFIED** |

**Verdict: A in principle, C in practice.**

The only permissively-licensed PS-family emulator in the survey. But it never
implemented PS5 graphics, the RDNA2 compiler, AGC, NGS2, AVPlayer, the PS5 HLE
library set, or the 512 GiB extended region. None of the hard PS5 problems exist
upstream of KytyPS5, and all the PS5 work is GPL. You cannot assemble "MIT Kyty
+ PS5 features" from any available source.

### 2.3 shadPS4

| Field | Finding |
|---|---|
| Upstream | https://github.com/shadps4-emu/shadPS4 |
| Pinned commit | `3a9f57265d28975658f1ca941daa5c879c7a74bd` (2026-10-11) |
| Licence | **`GPL-2.0-or-later`**, unambiguous. Per-file `SPDX-License-Identifier` + REUSE `REUSE.toml`. Cleanest licence metadata in the survey |
| CPU execution | Native x86-64 + static patching. `src/core/linker.cpp:41-65` is `#ifdef ARCH_X86_64 … #else UNREACHABLE_MSG("RunMainEntry unimplemented for current architecture.")` — **on a non-x86-64 host it aborts** |
| Memory | 40-bit VA (`ADDRESS_SPACE_BITS=40`), 16 KiB pages, MLPT + `boost::icl` |
| GPU | "Liverpool". Vulkan only. GCN decoder (`externals/gcn`) → IR → SPIR-V (`externals/sirit`) |
| Testing | Effectively none in-tree. `AGENTS.md` mandates *"Do not include tests. If emulation behaviour is changed, write and share hardware tests in the form of OpenOrbis homebrew."* |

**Verdict: C.**

Two things are worth stealing despite the licence, because they are *process*,
not code:

1. `AGENTS.md`: *"Do not include any game-specific or shader-specific hacks.
   Fixes must address the underlying emulation behavior."* P5EMU adopts this
   verbatim. It is the single most effective defence against emulator rot.
2. The 9.3 MB `aerolib.inl` NID→stub database shows the *shape* of the HLE
   problem (and its sheer size) without requiring anyone to reproduce it.

### 2.4 RPCS3 (as PS3 reference)

Already covered in §1.4. As a PS3-specific reference it is indirect: Cell BE +
RSX has little in common with PS5 x86-64 + RDNA2. Its value is structural.

---

## 3. Apple graphics and shader tooling

### 3.1 MoltenVK

| Field | Finding |
|---|---|
| Upstream | https://github.com/KhronosGroup/MoltenVK |
| Licence | **Apache-2.0** |
| What it is | Vulkan 1.4 (subset) on Metal. Runtime SPIR-V → MSL via SPIRV-Cross inside `vkCreateShaderModule()` |
| App Store | **Yes.** *"MoltenVK does not use any private or undocumented API calls or features, so your app will be compatible with all standard distribution channels, including Apple's App Store."* Caveat: `MVK_USE_METAL_PRIVATE_API=1` **disqualifies** the app |
| Min runtime | macOS 12.0, iOS 15, tvOS 15 |
| Compliance | *"at present MoltenVK is not fully compliant with the Vulkan specification"* |
| Overhead | *"the slowest step involves converting shaders from SPIR-V to MSL source code"* — mitigate with `vkGetPipelineCacheData()`. No published overhead benchmark exists (**UNVERIFIED**) |

**Verdict: A.**

Safe, permissive, and it establishes that a Vulkan→Metal path is App
Store-legal. But see [`gpu-architecture.md`](gpu-architecture.md) for why it is
not P5EMU's primary iOS path.

### 3.2 SPIRV-Cross

| Field | Finding |
|---|---|
| Upstream | https://github.com/KhronosGroup/SPIRV-Cross |
| Licence | **Apache-2.0** (repo); `spirv_msl.hpp` is **Apache-2.0 OR MIT** |
| Relevance | `CompilerMSL` — SPIR-V → MSL, plus `get_shader_resources()` reflection and `add_msl_resource_binding()` for deterministic Metal descriptor indices |

**Verdict: A.** The recommended SPIR-V → MSL path if P5EMU needs one.

### 3.3 Tint / Dawn

Upstream https://dawn.googlesource.com/tint — **Apache-2.0**. The standalone
`tint` mirror is **deprecated/read-only**; *"Please switch to the Dawn repo."*

**Verdict: B.** Usable, but depends on Dawn's build system and the project is
moving. Phase-3 candidate at best.

### 3.4 DirectXShaderCompiler "Metal CodeGen"

Upstream https://github.com/microsoft/DirectXShaderCompiler — **MIT**, but
generating Metal requires Apple's **closed-source, macOS-only** Metal Shader
Converter at build time.

**Verdict: D for runtime use.** Not shippable as a runtime library on iOS.

### 3.5 LLVM

| Field | Finding |
|---|---|
| Upstream | https://llvm.org/ |
| Licence | **Apache-2.0 WITH LLVM-exception** — https://llvm.org/LICENSE.txt |
| The exception | Permits embedding LLVM in Object form without complying with Apache-2.0 §4(a),(b),(d). Names GPLv2 by name and permits waiving conflicting sections |
| As offline AOT compiler | Fully supported. `llc -mtriple=aarch64-apple-macos`, `opt`, cross-compilation. **No JIT required** |
| As runtime emitter | `MCStreamer` is *"best thought of as an assembler API"* (https://llvm.org/docs/CodeGenerator.html). Stopping at the byte buffer is viable |
| AArch64 encodings | `llvm/lib/Target/AArch64/AArch64InstrInfo.td`. **`AArch64Encoding.h` no longer exists (404)** — do not design around it |

**Verdict: A.**

Licence-compatible with both closed-source and GPLv2 consumers. The heaviest and
slowest option for a JIT hot path, but excellent for offline AOT and as the
correctness reference for encoding.

### 3.6 VIXL

| Field | Finding |
|---|---|
| Upstream | **https://gitlab.arm.com/runtimes/vixl** (`github.com/arm/libvixl` is 404; `Linaro/vixl` frozen; `armvixl/vixl` archived) |
| Licence | **BSD-3-Clause** — `LICENCE` fetched verbatim from the Arm GitLab |
| What it is | *"Programmatic **assemblers** to generate A64, A32 or T32 code at runtime."* Also ships a disassembler and **an A64 simulator** |

**Verdict: A.** The best off-the-shelf AArch64 emitter: permissive,
Arm-official, actively maintained, purpose-built for runtime use, and the
bundled simulator enables host-independent testing of emitted code.

Note: FEX vendors a **fork** of vixl at `External/vixl` whose licence is
**UNVERIFIED** (404 on every licence path). If vixl is used, use upstream Arm,
not the fork.

---

## 4. Test corpora

A hard rule applies throughout: **test DATA licences and test CODE licences are
different**, and inheriting a corpus's provenance is not automatic.

| Corpus | Licence | Usable as P5EMU test data? |
|---|---|---|
| **iced** — https://github.com/icedland/iced | **MIT** | **YES — best in class.** Full x86/x64 decoder *and* encoder. The encoder enables decode→encode round-trip tests that Zydis alone cannot provide |
| **Zydis** — https://github.com/zyantific/zydis | **MIT** | **YES.** Decoder + formatter. Disassembler only, no encoder |
| **tinyemu** — https://github.com/sysprog21/tinyemu | **MIT** | **YES.** Whole-system x86 emulator usable as an out-of-process oracle. URL redirects; use the canonical path |
| **v86** — https://github.com/copy/v86 | **BSD-2-Clause** | Core yes. **The bundled BIOS/option-ROM blobs must not be redistributed** |
| Play! `Play--CodeGen/tests/` | BSD-2-Clause | **YES** — as a *design* template for the codegen differential harness |
| **KVM selftests** — `tools/testing/selftests/kvm` | GPL-2.0-only (assumed; no LICENSE in that dir) | **NO.** Richest source of authoritative architectural assertions in existence, and hardware-bound. Read, re-implement from the spec, never vendor |
| **QEMU `tests/tcg/`** | GPL-2.0-or-later (`tests/tcg/COPYING` is 404) | **NO.** The method is gold standard (compile-for-guest, run under TCG, diff output). Re-implement the harness; regenerate your own binaries |
| **FEX `fex-gcc-target-tests-bins`** and siblings | **NO LICENSE FILE** in any of the four repos; content derives from GCC `gcc/testsuite/gcc.target/i386` | **NO — do not vendor.** Excellent corpus, unusable licence hygiene. Either regenerate from the GCC testsuite under our own terms, or run as an external oracle and record *our own* expected outputs |
| **Unicorn** — https://github.com/unicorn-engine/unicorn | **GPL-2.0** (header inconsistency: `unicorn.h` says LGPL2) | **NO** for linking. Legitimate as an out-of-process cross-check only |
| **pspautotests**, **ppsspp-frametest-ci** | **NO LICENSE FILE** (verified 404) | **NO.** No licence ≠ permissive. Treat as all-rights-reserved |
| **Bochs** — https://github.com/bochs-emu/Bochs | LGPL-2.1 | **NO.** No permissive conformance corpus found |
| **libx86emu** | Custom, **not OSI**, no-advertising clause | **Technically permissive in substance, licence-cleanliness awkward.** Avoid |
| distorm3 | — | Repo **404.** Use iced instead |
| Intel SDM / AMD64 APM | Copyrighted books | **NO.** Transcribe *behaviour* into independently-written tests |

---

## 5. Summary matrix

| Project | SPDX | Class | One-line rationale |
|---|---|---|---|
| FEX | MIT | **C** | Best design reference; technically Linux-bound, RWX code cache, AArch64-only |
| QEMU TCG | GPL-2.0-or-later | **C** | Per-file MIT/BSD requires audit; TB granularity wrong for PS5 |
| PCSX2 | GPL-3.0-or-later | **C** | Closed-source HLE; proves why portable IR is mandatory |
| RPCS3 | GPL-2.0 | **C** | LLVM weight; 52 GiB fixed-VA model is iOS-incompatible |
| xemu | GPL-2.0-or-later | **C** | QEMU fork. But read `xemu.entitlements` — proof of macOS JIT entitlements |
| dynarmic | 0BSD | **B** | Only permissively-licensed x86-64→AArch64 JIT. **Archived upstream** |
| Play! | BSD-2-Clause | **A/B** | Reusable AArch64 assembler; gold-standard differential test pattern |
| PPSSPP | GPL-2.0-or-later | **C** | `--compare` cross-backend differential method is the lesson |
| Madeira | GPL-3.0-or-later | **C** | Proves x86-64→AArch64 JIT runs on jailed iOS. Do not copy the port |
| UTM | GPL-2.0 | **C** | Threaded-interpreter tier precedent |
| Kyty | MIT | **C** | Permissive but contains no PS5 technology |
| KytyPS5 | **GPL-2.0-only** | **C** | Best PS5 subsystem inventory. Copying locks P5EMU to GPL-2.0 |
| shadPS4 | GPL-2.0-or-later | **C** | Adopt its `AGENTS.md` no-hacks rule; code stays off-limits |
| MoltenVK | Apache-2.0 | **A** | App Store safe Vulkan-on-Metal |
| SPIRV-Cross | Apache-2.0 OR MIT | **A** | SPIR-V → MSL + reflection |
| Tint/Dawn | Apache-2.0 | **B** | Deprecated standalone repo |
| DXC Metal | MIT + Apple's closed tool | **D** | Not runtime-shippable |
| LLVM | Apache-2.0 WITH LLVM-exception | **A** | Safe; ideal for offline AOT, heavy for JIT hot path |
| VIXL (Arm upstream) | BSD-3-Clause | **A** | Best AArch64 emitter + simulator |
| VIXL (FEX fork) | **UNVERIFIED** | **C** | Do not use until verified |
| iced | MIT | **A** | Decoder + encoder for tests |
| Zydis | MIT | **A** | Decoder/formatter for tests |
| tinyemu | MIT | **A** | Out-of-process oracle |
| v86 | BSD-2-Clause | **A** (core) | Do not redistribute its BIOS blobs |
| KVM selftests | GPL-2.0-only | **D** | Read; re-implement from spec |
| QEMU tests/tcg | GPL-2.0-or-later | **D** | Re-implement harness |
| FEX `*-tests-bins` | No licence file | **D** | Do not vendor |
| Unicorn | GPL-2.0 | **D** | Out-of-process only |
| pspautotests / frametest-ci | No licence file | **D** | All-rights-reserved by omission |
| Bochs | LGPL-2.1 | **D** | No permissive corpus |
| libx86emu | Non-OSI custom | **D** | Avoid |
| distorm3 | 404 | **D** | Use iced |
| Intel SDM / AMD64 APM | Copyrighted | **D** | Read only |

---

## 6. The decision this matrix forces

**P5EMU must be independently authored, and must not vendor copyleft source.**

> **Phase-1 review correction (2026-10).** This section previously concluded
> that P5EMU should also adopt a permissive project-wide licence. That
> conclusion is **withdrawn**. Dependency compatibility does not determine our
> own licence, and the §4 provenance relationship below has not been reviewed by
> counsel.

> **Phase-7.1 owner decision (2026-10).** P5EMU's outbound licence is now
> **Apache-2.0** (see [`../../LICENSE`](../../LICENSE) and
> [`../../NOTICE`](../../NOTICE)). This changes P5EMU's own terms only. It does
> **not** relicense any project listed in this matrix: every external project
> and tool keeps its own licence, and P5EMU's Apache-2.0 grant does not extend
> to them. The provenance conclusions above remain binding.

The reasoning chain:

1. Every PS5 emulator that has solved any of the hard problems is GPL-2.0 or
   GPL-3.0. KytyPS5 is `GPL-2.0-**only**`.
2. GPL-2.0 §2(b) makes a work that copies GPL-2.0 code a derivative work
   requiring the whole work to be GPL-2.0. Attribution does not avoid this.
3. `GPL-2.0-only` code **cannot** be relicensed to GPL-3.0. Taking one line of
   KytyPS5 locks P5EMU to GPL-2.0, foreclosing GPL-3.0-only tooling.
4. Apple App Store distribution is a live target. Apple's EULA restricts
   reverse-engineering and imposes terms GPLv3 treats as additional restrictions.
5. Therefore: read these projects, extract *architectures* and *publicly
   documented format constants*, write every line of code ourselves.

### What is legitimately reusable

The distinctions matter. **Facts are not code.** Address constants, DT tag
values, PM4 opcode numbers, RDNA2 instruction encodings, and AMD's published
ISA documentation can be learned, re-derived, and re-typed. A *design* — guest
VA == host VA, symbol-database HLE, a five-stage shader recompiler — is an idea
anyone may implement in their own structure. A *line of GPL source* is a
derivative work, full stop.

### Phase-0 mechanical enforcement

`scripts/check_provenance.sh` fails the build on: any copyleft SPDX tag in our
own tree, any source file missing an SPDX header, any vendored `third_party/`
subtree without a licence record, any file over 2 MB, and a missing provenance
log. It cannot prove the provenance relationship — that is a human and legal
judgement — but it makes accidental contamination visible.
