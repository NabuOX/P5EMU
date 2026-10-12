# Provenance Log

Every line of P5EMU is either independently authored or adapted. This file
records which, with enough specificity to reconstruct and audit the decision.

---

## Standing rule

P5EMU is an **independent implementation informed by public research and
documented architecture behaviour, with no copied copyleft source code.**

P5EMU does **not** claim strict clean-room separation. Strict clean-room means
design and implementation are performed by parties that never saw the source in
question. P5EMU's design was produced by engineers who had read upstream project
sources during research, so that separation has not been established.

| Statement | True |
|---|---|
| No copyleft source copied, vendored, transcribed, or adapted | **Yes** |
| Source expression independently authored | **Yes** |
| Designers had read upstream sources | **Yes** |
| Strict clean-room separation established and independently verified | **No** |

What this permits and what it does not:

- **Permits:** learning architectural facts, encodings, format constants and
  design approaches, then expressing them independently.
- **Does not permit:** copying source lines, tables, expressions, or comment
  text, from any source, copyleft or otherwise, without an explicit recorded
  reuse decision under [`LICENSE.md`](LICENSE.md) §3.

---

## Record format

Each adaptation or imported component gets one block:

```
### <id>
- origin project      : <name>
- origin URL          : <exact upstream URL>
- origin commit       : <sha, if pinned>
- origin file/component: <path>
- original licence    : <SPDX id> (<url to licence file>)
- copyright notice    : <verbatim "Copyright ..." lines>
- modifications made  : <what P5EMU changed>
- decision            : <authorised reuse | independent reimplementation | rejected>
- rationale           : <why>
- date                : <YYYY-MM-DD>
```

---

## Independent reimplementations

Components written for P5EMU, informed by public research and documented
architecture behaviour. No upstream source was copied.

| Component | Phase | Scope | Notes |
|---|---|---|---|
| Build system, logging, status, test harness | 0 | Whole | No upstream reference |
| CPU architectural contract (`CpuModel.h`) | 0 | Whole | Interface design is P5EMU's |
| `GuestMemoryPort`, `LinearGuestMemory` | 1 | Whole | Interface designed from the constraint list, not from any implementation |
| x86-64 decoder | 1 | Whole | Encodings derived from Intel SDM / AMD64 APM; tables and control flow authored here |
| P5EMU SSA IR and `ValidateIr` | 1 | Whole | Property set derived from research conclusions; implementation authored here |
| IR builder | 1 | Whole | — |
| `ReferenceInterpreter` | 1 | Whole | — |
| Differential harness, deliberately-buggy backend | 1 | Whole | Test methodology informed by Play! (BSD-2-Clause) and PPSSPP (GPL-2.0-or-later) research; no code taken |
| AArch64 instruction emitter (`Arm64Emitter`) | 2 | Whole | Encodings derived field-by-field from the ARM ARM (DD0487); verified against binutils disassembly. No assembler library used |
| AArch64 code generator (`Arm64CodeGen`) | 2 | Whole | Lowers P5EMU IR to AArch64; strategy authored here (stack-slot values, synthesised flags) |
| AArch64 backend, runner, code buffer | 2 | Whole | Backend contract, out-of-process runner and W^X buffer authored here |
| AArch64 execution harness (`aarch64/harness/`) | 2 | Whole | Test infrastructure; maps emitted code and calls it. Contains no P5EMU semantics |
| Operand-width semantics (8/16/32/64) in IR, interpreter and backend | 3 | Whole | Width rules from Intel SDM Vol. 1 §3.4 / Vol. 2 §2.2.1, authored here |
| SHL/SHR/SAR semantics and count masking | 3 | Whole | Intel SDM Vol. 2 §2.2.2 and §3.5; count masking and flags authored here |
| Batched harness protocol and randomized differential driver | 3 | Whole | Protocol and driver authored here; reuses the same comparison primitives |
| Bounds-checked binary reader (`BinaryReader`) | 4 | Whole | Overflow/endian/alignment rules authored here |
| Generic ELF64 reader (`Elf64`) | 4 | Whole | Field layout from the System V gABI; parsing authored here |
| PS5/Prospero interpretation layer (`Ps5Executable`) | 4 | Whole | Console-specific facts re-typed from public research; see the table below |
| `LoadPlan`, validation, transactional apply, entry handoff (`Loader`) | 4 | Whole | — |
| `MappedGuestMemory` (multi-region, permission-enforcing port) | 4 | Whole | — |
| Synthetic ELF fixture builder (`tests/ElfFixtureBuilder.h`) | 4 | Whole | Images authored here; no real binary used |
| Relocation model, parser and application (`Reloc`) | 5 | Whole | Formulas from the System V AMD64 psABI; parsing/apply authored here |
| Import requirement model and resolver (`Import`) | 5 | Whole | Model and resolver interface authored here; no Sony layout used |
| Load-bias model and transactional relocation apply (`Loader`) | 5 | Whole | — |
| HLE subsystem (`Hle`: registry, dispatcher, call context, resolver) | 6 | Whole | Authored here; fully synthetic, no Sony implementation |
| kExternalCall IR + interpreter/backend dispatch | 6 | Whole | Authored here |
| Runtime layer (`runtime::Runtime`: address space, stack, startup, graph, bias, resolution, merge, `LoadProcess`) | 7 | Whole | Composes the loader/reloc/HLE subsystems; all layouts synthetic. See `docs/research/ps5-runtime-evidence.md` for the PS5 evidence boundary |
| Multi-region AArch64 transport (harness protocol v2) | 7 | Whole | Region table authored here; extends the Phase-2/3 protocol. Still test infrastructure |

### PS5 executable-format facts (Phase 4)

Console-specific constants are **facts learned from public research and
re-typed**; P5EMU's identifier names are its own. No GPL source expression was
copied. Classification follows `docs/research/ps5-subsystems.md`.

| P5EMU concept | Fact | Source | Classification | Implementation |
|---|---|---|---|---|
| Guest machine | `EM_X86_64 = 62` | System V ABI / ELF gABI | PROVEN_FROM_SOURCE | `core/include/p5emu/loader/Elf64.h` |
| Guest OS ABI | `ELFOSABI_FREEBSD = 9` | System V ABI | PROVEN_FROM_SOURCE | `core/include/p5emu/loader/Ps5Executable.h` |
| PS5 executable types | `ET_DYNEXEC = 0xfe10`, `ET_DYNAMIC = 0xfe18` | KytyPS5 `src/loader/elf.h` @ `16a7fe689a642deeeef85a706e7da305eca6fb85` | DOCUMENTED_EXTERNALLY | `Ps5Executable.h` |
| Sony dynamic tag namespace | `DT_OS_* = 0x610000xx` | KytyPS5 `elf.h` @ `16a7fe6` | DOCUMENTED_EXTERNALLY | `Ps5Executable.h` |
| PS5-era `_1` dynamic variant range | from `0x61000041` | KytyPS5 `elf.h` @ `16a7fe6` | **UNKNOWN** | preserved, not interpreted |
| Sony program-header types | `PT_OS_DYNLIBDATA` / `PT_OS_PROCPARAM` / `PT_OS_RELRO` | KytyPS5 `elf.h` @ `16a7fe6` | **UNKNOWN** (numeric values not published in our research) | not matched |
| Sony symbol/import structure layout | targets of `DT_OS_*` | not public | **UNKNOWN** | not decoded; module names use generic `DT_NEEDED` only |

### Generic relocation facts (Phase 5)

Relocation semantics are **PROVEN_FROM_SOURCE**: the System V Application Binary
Interface, AMD64 Architecture Processor Supplement (x86-64 psABI).

| P5EMU concept | Fact | Source | Classification | Implementation |
|---|---|---|---|---|
| Relocation numbers | `R_X86_64_NONE=0`, `_64=1`, `_GLOB_DAT=6`, `_JUMP_SLOT=7`, `_RELATIVE=8` | AMD64 psABI, relocation types | PROVEN_FROM_SOURCE | `core/include/p5emu/loader/Reloc.h` |
| Formulas | `_64 = S+A`, `_GLOB_DAT = S`, `_JUMP_SLOT = S`, `_RELATIVE = B+A` | AMD64 psABI §4.4 | PROVEN_FROM_SOURCE | `core/src/loader/Reloc.cpp` |
| RELA layout | `Elf64_Rela{r_offset,r_info,r_addend}`, 24 bytes; `r_info = sym<<32 | type` | AMD64 psABI §4.4 | PROVEN_FROM_SOURCE | `Reloc.h` / `Reloc.cpp` |
| Symbol layout | `Elf64_Sym{st_name,st_info,st_other,st_shndx,st_value,st_size}`, 24 bytes; `SHN_UNDEF=0` | ELF gABI + psABI | PROVEN_FROM_SOURCE | `Reloc.cpp` |
| Dynamic relocation tags | `DT_RELA=7`, `DT_RELASZ=8`, `DT_RELAENT=9`, `DT_JMPREL=23`, `DT_PLTRELSZ=2`, `DT_PLTREL=20`, `DT_SYMTAB=6`, `DT_SYMENT=11`, `DT_STRTAB=5`, `DT_STRSZ=10` | ELF gABI dynamic tags | PROVEN_FROM_SOURCE | `Ps5Executable.h` |

### HLE (Phase 6)

Phase 6 is **fully synthetic**. No Sony library, function name, NID, or ABI quirk
is present in production source. The only guest-call ABI implemented is the
AMD64 System V integer calling convention (PROVEN_FROM_SOURCE), used for the
synthetic P5EMU-owned test functions. A future PS5 HLE function may only be
added once its identity, ABI, and behaviour are each independently evidenced and
classified — until then it stays UNKNOWN.

---

## External test infrastructure (Phase 2)

Phase 2 proves that emitted AArch64 actually executes by running it in an
**independent** AArch64 environment. The following external tools are used as
**test infrastructure only**. None is linked into `p5emu_core`, vendored, or
required to build or use P5EMU itself; all are invoked as separate programs from
the test suite.

| Tool | Licence | Role | Why acceptable |
|---|---|---|---|
| `qemu-aarch64` (qemu-user) | GPL-2.0 | Executes the emitted AArch64. The AArch64 ISA semantics that P5EMU is checked against come from this independent implementation | Separate process, never linked or redistributed; invoked only by tests |
| `aarch64-linux-gnu-gcc` / `-objdump` (binutils, GCC) | GPL-3.0-or-later | Cross-compiles the harness; disassembles emitted bytes for developer inspection | Separate tools, not linked; used for a test binary and diagnostics only |
| `aarch64-linux-gnu-objdump` | GPL-3.0-or-later | Encoder verification during development and the `scripts/arm64_disasm.sh` debugging path | Diagnostic only |

These tools are **not** dependencies of the P5EMU build. `scripts/verify.sh` and
the core library do not reference them; only the AArch64 test suites do, and they
report a clear failure (never a silent pass) when the tools are absent. No
copyleft code was copied into P5EMU: the emitted AArch64 comes from P5EMU's own
`Arm64Emitter`, and the external tools only *execute* or *display* it.

---

## Adaptations

*(none)*

---

## Imports

*(none)*

No third-party code has been imported into P5EMU. This section stays empty until
a component passes the review in [`LICENSE.md`](LICENSE.md) §3.

---

## Projects studied as architectural and factual reference

Read during research. **No code was taken from any of them.** A permissive
licence (FEX MIT, Play! BSD-2-Clause, dynarmic 0BSD, VIXL BSD-3-Clause) does not
by itself authorise reuse; that requires a positive recorded decision.

Full analysis with URLs, commits and licences:
[`docs/research/licenses.md`](docs/research/licenses.md).

| Project | Licence | Role in P5EMU |
|---|---|---|
| FEX (`FEX-Emu/FEX`) | MIT | CPU IR architecture; AArch64 codegen without isel; register-allocation-as-IR-pass |
| QEMU TCG | GPL-2.0-or-later (TCG partly BSD/MIT) | Block formation discipline; Darwin `MAP_JIT` code-buffer recipe |
| dynarmic (`lioncash/dynarmic`, archived) | 0BSD | Bring-your-own-memory abstraction shape; Apple code-cache separation |
| PCSX2 | GPL-3.0-or-later | Negative result: no portable IR makes a second host a rewrite |
| RPCS3 | GPL-2.0 | Compiler IR as translation contract; project structure |
| Kyty | MIT | FreeBSD-derived PlayStation ELF tag namespace (facts) |
| KytyPS5 | GPL-2.0-only | PS5 subsystem inventory, names only |
| shadPS4 | GPL-2.0-or-later | PS4 subsystem inventory; the "no game-specific hacks" rule |
| Play! (`jpd002/Play-`) | BSD-2-Clause | Native-vs-JIT differential test pattern |
| PPSSPP | GPL-2.0-or-later | Cross-backend `--compare` differential harness concept |
| Madeira | GPL-3.0-or-later | Evidence x86-64→ARM64 JIT runs on jailed iOS |
| UTM | GPL-2.0 | Threaded-interpreter tier for no-JIT environments |
| MoltenVK | Apache-2.0 | Candidate host GPU bridge |
| SPIRV-Cross | Apache-2.0 OR MIT | Candidate SPIR-V → MSL path |
| LLVM | Apache-2.0 WITH LLVM-exception | Candidate offline AOT compiler |
| VIXL | BSD-3-Clause | Candidate AArch64 macro assembler |
| iced / Zydis / tinyemu | MIT | Candidate test-only cross-check tools |

---

## Material that must never enter this repository

- PlayStation firmware, system software, or extracted sysmodules
- Console crypt keys, or any key material
- Game executables, discs, save data, or container dumps
- Sony SDKs, GDK/OpenGDK headers, or developer tools
- Sony trademarks, UI assets, icons, sounds, fonts, firmware artwork
- GPL, LGPL, AGPL, CDDL, MPL or other copyleft source
- Out-of-tree patch/dump directories on developer machines, including local
  copies of other projects' GPL-derived working trees. Those are GPL-derived
  working material from another project and are **not** P5EMU inputs.

`scripts/check_provenance.sh` performs a mechanical check for copyleft SPDX tags
in source, missing SPDX headers, vendored `third_party/` trees, large binaries,
and a missing provenance log. It does not and cannot replace human review of
this file.
