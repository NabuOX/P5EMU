# CPU Feasibility: executing PS5 x86-64 on Apple ARM64

**Phase 0 research artefact.** Evidence-first. Claims carry URLs; unverified
items are marked as such and are not relied on for the recommendation.

Target: PS5 x86-64 guest → Apple ARM64 (iPhone/iPad first, macOS second).

---

## 1. The central problem, stated honestly

**Every existing PlayStation-family emulator executes guest code natively on an
x86-64 host. None of them has an ARM64 CPU backend.**

| Project | ARM64 host | Evidence |
|---|---|---|
| KytyPS5 | **Absent** | CI asserts x86-64 artifacts: `file … \| grep -q "ELF 64-bit LSB .*x86-64"` (`build.yml:413`); `lipo -verify_arch x86_64` (`:263`). Only two `aarch64` occurrences in `src/`, both for host-GPU page sizes |
| shadPS4 | **Absent** | `src/core/linker.cpp:41-65`: `#ifdef ARCH_X86_64 … #else UNREACHABLE_MSG("RunMainEntry unimplemented for current architecture.")`. On ARM64 it aborts |
| RPCS3 | Present (PPC64 guest) | Not applicable — different guest ISA |
| PCSX2 | **Incomplete by admission** | `CMakeLists.txt:84`: *"Apple Silicon support in PCSX2 is INCOMPLETE. There are currently no EE/VU/IOP recompilers, and games will run VERY slow."* ARM64 source list is four files |

KytyPS5 and shadPS4 both run on Apple Silicon via **Rosetta 2**: x86-64 binaries
on an ARM64 host, so the host translates to x86-64 and the guest code runs
natively on the translated layer.

**Implication:** there is no prior art to copy for P5EMU's core problem. The
design must be derived from general-purpose emulator architecture (FEX, QEMU
TCG, dynarmic) plus x86-64 semantics.

---

## 2. IR design: the four archetypes compared

### FEX — opcode IR + macro splatter

- 433 typed ops in 14 groups (`FEXCore/Source/Interface/IR/IR.json`)
- **Strict scalar/vector separation**: `MUL` vs `VMUL`
- **Explicit `LoadContext`/`StoreContext` for tracked CPU state vs
  `LoadMem`/`StoreMem` for guest memory.** This separation is what makes guest
  state spillable and register-allocatable while guest memory is not — the most
  important single idea in the whole survey
- Explicitly sized IR variables (1/2/4/8 bytes, arbitrary vector element counts)
- `uint32_t` offset references into an arena, not pointers: IR is relocatable
  and cache-dense. SSA number = `AllocationOffset / sizeof(OrderedNode)`
- Flags as first-class ops (`AddWithFlags`, `AdcNZCV`, `ShiftFlags`,
  `RotateFlags`, `LoadPF`/`StorePF`) plus a dedicated
  redundant-flag-elimination pass
- **Register allocation is an IR pass**, emitting `AllocateGPR`,
  `AllocateGPRAfter`, `AllocateFPR`, `SpillRegister`, `FillRegister`
- Codegen: vixl macros concatenated directly, **no instruction selector**
- Multi-block frontend analysis chains *past* unconditional branch-enders

Caveat: FEX's own `FEXCore/docs/OpDispatcher.md:46` admits the IR *"is currently
lacking any idea of flags or PHI nodes"*. **That document is stale** — it
references a "LLVM JIT" that does not exist in the codebase. Do not trust FEX
docs as specifications; read the code.

### QEMU TCG — macro-op IR

- Not SSA; straight-line macro-ops over temp registers
- Block ends at every control-flow or state change; chaining same-page only
- One optimisation peephole pass in `tcg/optimize.c`
- Register allocation is backend-local
- **The only design with proven multi-host portability** (8 backends,
  system + user + TCI interpreter)

Its portability discipline comes at a price that is wrong for PS5: a PS5-class
CPU emits long, well-optimised, low-syscall-frequency code. TCG's design point
was built for the Xbox 360's short basic blocks.

### LLVM IR (RPCS3)

- True SSA, ~150 optimisation passes for free
- ~30 MB binary; cross-compiled LLVM needed for `aarch64-apple-ios`
- **LLVM ORJIT on iOS is known to fail code-sign checks** (see §5)

### dynarmic — op IR with explicit blocks

- Non-SSA ordered op list with explicit block ends and explicit register
  uses/defs
- `BlockOfCode` with fast-block linking and a Return-Stack-Buffer optimisation
- IR ops are read once into registers via a cached register file

### Verdict

**Adopt the FEX IR *shape* with a independent implementation.**

Specifically:

| Adopt | Reject | Why |
|---|---|---|
| Explicit CPU-state ops separate from memory ops | FEX's AArch64-flavoured op set | P5EMU needs one host only, but the IR must stay readable and testable on x86-64 dev hosts |
| Sized, typed IR values | 433 hand-tuned ops | Design the opcode set as we need it, smaller and auditable |
| Flags as first-class IR values with an elimination pass | | x86 flag dependencies are a major correctness and performance trap |
| Arena with offset references, not pointers | | Cache density and relocatability for AOT serialisation |
| **Register allocation as an IR pass with explicit allocation ops** | | Makes the host register file a visible, testable resource |
| | QEMU TCG's TB granularity | Wrong shape for PS5-class code |
| | LLVM as a *dependency of core* | Makes the interpreter untestable without a toolchain; 30 MB; ORJIT has iOS signing problems |
| | A "splatter" with no isel | The right call for a single-host target, but it means the IR must be shaped for AArch64. **Defer this decision until the backend actually exists** |

---

## 3. Memory model — the real differentiator

Not the IR. The memory model.

| Project | Guest VA strategy | iOS viability |
|---|---|---|
| FEX | Base register added per guest access; host allocator parses `/proc/self/maps` and `prctl(PR_SET_VMA)` to steal 48-bit VA | ✗ Linux-only primitives |
| QEMU user | Host addr == guest addr in a reserved region | ✓ portable |
| QEMU system | Software MMU, physically-indexed TBs | ✓ but heavy |
| RPCS3 | **~52 GiB of fixed-address reservations** at hardcoded host addresses | ✗ not viable in the sandbox |
| **dynarmic** | **Bring-your-own-memory** via `UserCallbacks` / `ContiguousGuestMemory`; `nullptr` allows guest == host | ✓ **the portable abstraction** |

**Recommendation: dynarmic's shape.** Allocate PS5 guest RAM as one contiguous
host reservation, publish its base to the JIT in a fixed register, and put every
page-permission and DMA side effect behind a narrow virtual interface. The JIT
core then has **zero OS dependency**, and the platform layer is a small, separate
component.

This is why `core/include/p5emu/cpu/CpuModel.h` defines `GuestMemoryPort` with
`IdentityMapped()` / `IdentityBase()` rather than exposing a raw pointer.

### Why guest VA == host VA is worth fighting for

KytyPS5 and shadPS4 both map the guest address space directly onto a single
backing store. Consequences:

- **No TLB, no address translation** on the hot path
- **Cache-coherent GPU access for free** — CPU stores and GPU DMA hit the same
  physical pages, so no explicit cache flush/invalidate protocol is needed. That
  protocol is a notorious correctness *and* performance trap
- CPU and GPU protection are separate bit axes (`PROT_CPU_*` vs `PROT_GPU_*`),
  which models unified memory correctly

P5EMU adopts this where the platform allows it, behind `GuestMemoryPort`.

---

## 4. W^X and the code cache on Apple

Linux offers dual-mapping split-W^X (`memfd_create` + `mprotect`) — one address
range, two mappings, no toggle. **iOS has no equivalent.**

Darwin's model is `MAP_JIT` plus an explicit write-protect toggle:

> **"MAP_JIT — Allow mapping pages both `PROT_WRITE` and `PROT_EXEC` when the
> hardened runtime is enabled. … A writable, but not executable mapping is
> returned on iOS, watchOS and tvOS. Usage of this flag requires the caller to
> have the `com.apple.security.cs.allow-jit` entitlement on macOS."**
> — https://keith.github.io/xcode-man-pages/mmap.2.html

On iOS the syscall succeeds and yields a genuinely writable region that simply
never becomes executable. Without the entitlement, the JIT buffer is a dead end.

Apple's own description of the macOS model:

> **"The `pthread_jit_write_protect_np()` function isn't available on iOS."**
> — https://developer.apple.com/documentation/apple-silicon/porting-just-in-time-compilers-to-apple-silicon

And of the iOS model, for the one process Apple *does* grant this to:

> **"The system uses hardware memory management to enforce that a page of memory
> allocated by your process is either writable, or executable, but not
> simultaneously both (W^X). … When your app targets the arm64e instruction set,
> the `be_memory_inline_jit_restrict_rwx_to_rw_with_witness()` function inserts a
> cryptographically signed pointer authentication code (PAC) into the pointer to
> your JIT memory. … You need to test your JIT code on an iPhone, not in the
> Simulator."**
> — https://developer.apple.com/documentation/browserenginekit/protecting-code-compiled-just-in-time

Note the arm64e/PAC consequence: an iOS JIT backend must emit arm64e code and
handle PAC-signed JIT pointers.

### Code cache survey

| Project | Code cache | iOS verdict |
|---|---|---|
| FEX | **RWX single mapping**, no split W^X, no `MAP_JIT` (`AllocatorHooks.h:117-139`) | ✗ unconditionally fatal |
| QEMU | Dual-mapping on Linux; `MAP_JIT` under `#ifdef CONFIG_DARWIN`; **RWX↔RX flipped per translation step** | ⚠️ Darwin path is right, per-step flip is wrong |
| PCSX2 | x86-64 only | n/a |
| RPCS3 | Not ARM64/iOS-relevant here | n/a |
| **dynarmic** | `MAP_JIT` + RW↔RX `mprotect` toggle; separate RX/RW address spaces gated on `DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT \|\| __APPLE__ \|\| __OpenBSD__` | ✓ **already Apple-correct** |

**Design consequences:**

1. The code cache is a **first-class swappable platform component**, never part
   of the JIT core.
2. Write-protect is toggled **once per block entry/exit, never per instruction**.
3. Guest memory is a contiguous reservation, so the base fits a fixed register
   and most accesses need no bounds check.
4. The backend must never assume it can `mmap` RWX.
5. **Every project surveyed except QEMU and dynarmic has no Apple-aware code
   cache at all.** Writing this correctly is a differentiator, not a detail.

---

## 5. F-07: Can FEX be embedded on iOS?

**Short answer: not as-is. Yes as a study target. There is a proven route, and
it is not App Store compatible.**

### What upstream says

Maintainer `Sonicadvance1`, `FEX-Emu/FEX` discussion **#3267** (2023-11-11),
Accepted Answer: *"We still don't have plans to support MacOS."* At the pinned
commit there are zero upstream issues/PRs for macos/darwin/iOS and zero
`ANDROID` tokens in the tree. `CMakeLists.txt` has no Darwin system branch.

FEX's code cache is RWX with no toggle and no `MAP_JIT` — that alone is fatal on
iOS. Its allocator, ELF loader, syscall layer, and thunk library are all
Linux/X11-shaped.

### Can *any* x86-64 → ARM64 JIT work on iOS?

**Yes — with a debugger attached or a jailbreak. It is a platform policy, not a
project limitation.**

App Store Review Guideline **2.5.2** (https://developer.apple.com/app-store/review/guidelines):

> "Apps should be self-contained in their bundles … nor may they **download,
> install, or execute code which introduces or changes features or functionality
> of the app** …"

**4.7** explicitly permits emulators:

> "**4.7** Mini apps, mini games, streaming games, chatbots, plug-ins, and game
> emulators … **retro game console and PC emulator apps can offer to download
> games** …"

But **4.7.2**: *"Your app may not extend or expose native platform APIs or
technologies to the software without prior permission from Apple."* This is a
real constraint on any HLE layer that hands native Metal objects to guest code.

There is **no App Store-grantable JIT entitlement**. `com.apple.security.cs.allow-jit`
is a *macOS Hardened Runtime* entitlement — xemu ships both it and
`com.apple.security.cs.allow-unsigned-executable-memory` in `xemu.entitlements`,
and neither has an iOS equivalent. On iOS, W^X is enforced by the kernel unless
the process is `CS_DEBUGGED`.

iSH applied for an interoperability extension and was refused:

> "Until now, Safari is the first and to date only software on iPhone that is
> permitted to use JIT compilation technology. … In July, we filed an
> interoperability request with Apple to expand the scope of JIT compilation to
> non-browser apps. … **after two months of deliberation Apple has denied our
> request.**"
> — https://ish.app/blog/ish-jit-and-eu

### The proof that it works: Madeira

https://github.com/willfaust/Madeira — `GPL-3.0-or-later`, 1992★, last push
2026-10-06. Runs x86-64 Windows PC games on a **jailed** iPhone via
FEX-Emu + Wine + DXMT.

Mechanism, per `docs/JIT.md`: attach a debugger via **StikJIT** (built-in, iOS
26+, uses the device's remote pairing file) or **StikDebug** (+ LocalDevVPN).
The app waits *"up to 90 seconds for both the `CS_DEBUGGED` flag and a live
debugger connection."*

Its own README is explicit about the cost:

> "**Because JIT needs a debugger, Madeira cannot be offered on the App Store.**"

And about the requirement, which is the key constraint for P5EMU:

> "- **JIT**, which iOS only allows while a debugger is attached."

Madeira vendors `https://github.com/willfaust/FEX` branch `ios-port-2607`
(head `3bec2ac498bf78156ab47c0c194b0e8cb2849756`, still MIT). **Whether that
branch uses `MAP_JIT` or an RWX toggle is UNVERIFIED** — the research read the
branch head and its licence but not the diff. This is the single highest-value
thing to read next, and it determines the whole code-cache design.

---

## 6. Risks specific to x86-64 → ARM64

### 6.1 AVX on 128-bit NEON

FEX's AVX emulation historically **required SVE2 with 256-bit registers, which
no Apple part has**. The stated direction is *"implementing AVX on top of 128-bit
[ASIMD] … This will work even on Apple M1 (with ASIMD)."*

Whether the 128-bit-only path has fully landed in `main` at the pinned commit is
**UNVERIFIED**. The last directly verified statement is from 2023.

Apple GPU families are A11=Apple4 … A17/M3/M4=Apple9. **NEON is 128-bit on every
one of them. SVE2 is not available on any.** AVX/AVX2/AVX-512 guest code must
therefore be emulated on 128-bit ASIMD, and a 256-bit AVX register cannot be
mapped 1:1 onto host state.

**This is P5EMU's largest single CPU-side unknown.** Budget it explicitly.

### 6.2 Fault granularity

x86 permits unaligned access; AArch64 does not for many types. FEX's release
notes discuss fault-granularity handling and a runtime visualiser that shows
which accesses will fault (`FEAT_LSE2` reduces the count). Any P5EMU backend must
insert fences or take a signal for every such access. This has a direct
performance cost on ARM64 and **no x86-64 host can measure it** — meaning ARM64
performance work will be harder to validate than everything else in Phase 2.

### 6.3 AVX-512 and SME

Apple has neither. Guest AVX-512 must be fully emulated. SME is not on the table
at all.

### 6.4 Self-modifying code

Both KytyPS5 and shadPS4 use write-protect + fault, i.e. the Linux/SIGSEGV
approach. On iOS, signal handlers are restricted in ways that affect this. FEX
does SMC detection via guest-code CRC + page length (`ValidateCode`,
`ThreadRemoveCodeEntry`). A `GuestMemoryPort::NotifyWriteToExecutableRegion` hook
exists in `CpuModel.h` for exactly this.

---

## 7. Recommended CPU architecture

```
                    guest bytes (x86-64)
                            |
                 +----------v-----------+
                 |  Decoder (portable C++)|   table-driven, no x86 host needed
                 |  reports UNIMPLEMENTED |
                 |  rather than guessing   |
                 +----------+-----------+
                            |
                      DecodedInstruction        <- the shared contract
                            |
                 +----------v-----------+
                 |  IrBuilder + IR pass  |   SSA, sized/typed values,
                 |  pipeline              |   flags first-class,
                 |                        |   RA expressed as IR ops
                 +----------+-----------+
                            |
                            IrBlock
              +-------------+-------------+
              |                           |
   +----------v----------+     +----------v---------+
   | ReferenceInterpreter |     |  Arm64Backend     |
   | (portable C++)       |     |  (AArch64 host)   |
   | THE ORACLE           |     |  performance      |
   +----------+----------+     +----------+---------+
              |                           |
              +-------------+-------------+
                            |
              execute(Decoder(bytes), backend)
                            |
                   IDENTICAL BY CONSTRUCTION
```

### Backend 1 — `ReferenceInterpreter`

- Portable C++, no host assumptions, no assembly
- **Always compiled in, on every platform, in every build**
- Structurally incapable of disagreeing with the ARM64 backend because it is the
  definition of correct
- Optimised for *clarity and debuggability*, not speed
- **The differential oracle.** This is not optional; PPSSPP keeps a full IR
  interpreter for exactly this reason, and Play! runs every codegen test twice
  (native C++ reference vs JIT) on every host backend in CI

### Backend 2 — `Arm64Backend`

- AArch64 host only; **not built on the Phase 1 development host (x86-64)**
- Phase 2 spike, not Phase 0/1 work
- Must route all guest memory through `GuestMemoryPort` unless `IdentityMapped()`
- Must have a **swappable code cache** with a Darwin `MAP_JIT` implementation
- Emits **arm64e** with PAC-signed pointers if targeting iOS

### Explicitly not adopted

| Not adopted | Because |
|---|---|
| LLVM as a `core/` dependency | Makes the interpreter untestable without a toolchain; 30 MB; ORJIT has iOS code-sign failures |
| QEMU TCG | TB granularity wrong for PS5-class code; per-step RWX flip wrong for iOS |
| PCSX2-style hand-written-per-host emitters | The reason PCSX2 has no ARM64 EE backend four years on |
| RPCS3's fixed 52 GiB VA reservations | Not viable in the iOS sandbox |

### LLVM remains available

As an **offline AOT compiler** for shipping a pre-translated code cache — which
is exactly what the App Store tier requires. `llc -mtriple=aarch64-apple-macos`
needs no JIT. And as the **correctness reference** for AArch64 encodings
(`AArch64InstrInfo.td`).

### AArch64 emitter choice

| Option | Licence | Note |
|---|---|---|
| **VIXL** (https://gitlab.arm.com/runtimes/vixl) | BSD-3-Clause | Arm-official, actively maintained, purpose-built for runtime emission, **and ships an A64 simulator** for host-independent testing |
| Hand-written encoder | Ours | Two production precedents (Play!, PPSSPP). Fastest, most work |
| LLVM MC (`MCStreamer`) | Apache-2.0 + exception | Best correctness, heaviest, slowest per instruction |
| Play! `AArch64Assembler` | BSD-2-Clause | Reusable with attribution. Note its unusual `wZR=31 / xZR=30 / xSP=31` encoding |
| Keystone | **GPL-2.0-only** | ✗ Rejected |

**Recommendation: start with a hand-written encoder behind a narrow interface,
using VIXL as the reference oracle in tests.** The alternative is starting with
VIXL and having no independent check on it.

Note: `llvm/lib/Target/AArch64/AArch64Encoding.h` **no longer exists** (404). Its
replacement is `getBinaryCodeForInstr()` in `AArch64MCCodeEmitter.cpp`, generated
by `llvm/utils/TableGen/CodeEmitterGen.cpp`.

---

## 8. What must not be assumed

| Assumption | Status |
|---|---|
| FEX can be dropped into an iOS app | **FALSE.** Linux-bound allocator/loader/syscalls; RWX code cache; upstream refuses macOS |
| x86-64 → ARM64 JIT is impossible on iOS | **FALSE.** Madeira does it, 1992★, on jailed hardware |
| A JIT can ship on the App Store | **FALSE.** Requires a debugger attach or jailbreak |
| AVX will "just work" on ARM64 | **UNVERIFIED.** 128-bit NEON path in FEX is not confirmed landed |
| LLVM ORJIT works on iOS device | **Known to fail** code-sign checks (`LLVM-On-iOS`: *"the app was terminated with SIGTERM CODESIGN"*) |
| Apple has SVE2 / 256-bit SIMD | **FALSE.** No Apple part has it |
| "tri-mode plugins" exist in QEMU | **FALSE.** Searched issues and docs; the term does not exist upstream |
| Dylib embedding or dyld tricks bypass W^X | Not attempted and not a plan |

---

## 9. Verdict

**Feasible, but P5EMU's CPU layer is the dominant cost of the project and has no
prior art in the console-emulation space.**

The plan that survives this evidence:

1. **Interpreter first**, as the specification, not as a fallback.
2. **Differential testing from the first instruction**, because the oracle is
   cheap to maintain and the alternative (debugging two backends that both look
   right) is not.
3. **ARM64 backend as a Phase 2 spike**, gated behind a clean `ExecutionBackend`
   interface, so it can be deferred without architectural rework.
4. **Interpreter is the App Store tier.** It is not a degraded mode; it is the
   only tier Apple permits without a debugger.
5. **Code cache as a platform component from day one**, with the Darwin `MAP_JIT`
   implementation designed before the backend, not retrofitted.
6. **Budget AVX-on-NEON explicitly.** It is the known-hardest guest feature and
   it cannot be measured on the development host.
