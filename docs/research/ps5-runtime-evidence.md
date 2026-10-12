# PS5 Runtime & Process-Startup Evidence Audit (Phase 7)

A narrow audit of what is actually known about PS5 process/runtime startup, so
Phase 7 implements a **generic** process model and does not invent PS5 behaviour.

Sources are the pinned Phase-0 research (`docs/research/ps5-subsystems.md`, pinned
commits: KytyPS5 `16a7fe689a642deeeef85a706e7da305eca6fb85`, Kyty
`4733b7e1c91b10554a52007903d74dc76c39a230`, shadPS4
`3a9f57265d28975658f1ca941daa5c879c7a74bd`) plus the public ELF/AMD64 psABI.

## Classifications

| Fact | Evidence | Classification | Implemented? | Blocked by |
|---|---|---|---|---|
| ELF64, FreeBSD-derived ABI, `EM_X86_64=62`, `ELFOSABI_FREEBSD=9` | KytyPS5 `src/loader/elf.h`; System V ABI | DOCUMENTED_EXTERNALLY / PROVEN_FROM_SPEC | Yes (Phase 4) | — |
| PS5 `e_type` (`ET_DYNEXEC=0xfe10`, `ET_DYNAMIC=0xfe18`) | KytyPS5 `elf.h` | DOCUMENTED_EXTERNALLY | Yes (recognized) | — |
| Sony dynamic tag namespace `0x610000xx` | KytyPS5 `elf.h` | DOCUMENTED_EXTERNALLY | Numbers recognized, values not interpreted | layout UNKNOWN |
| Module dependency list | generic `DT_NEEDED` | PROVEN_FROM_SPEC | Yes (generic graph) | PS5 module semantics UNKNOWN |
| Module naming (e.g. `libkernel.sprx`) | KytyPS5 research (names only) | DOCUMENTED_EXTERNALLY | Only as opaque strings | real names/versions UNKNOWN |
| PS5 symbol/NID representation | Sony import/export structures | **UNKNOWN** | No | layout not public; no guessing |
| PS5 import/export metadata layout | `DT_OS_*` targets | **UNKNOWN** | No (synthetic export model only) | layout not public |
| Initial stack layout (argc/argv/envp/auxv) | not in public evidence | **UNKNOWN** | Generic synthetic layout only | PS5 stack policy UNKNOWN |
| Initial RSP value / stack base | not in public evidence | **UNKNOWN** | Synthetic deterministic layout only | PS5 address policy UNKNOWN |
| Stack alignment | not in public evidence | **UNKNOWN** | Generic 16-byte alignment (SysV) | PS5 policy UNKNOWN |
| Auxiliary vector contents | not in public evidence | **UNKNOWN** | Representable, not populated as PS5 | PS5 auxv UNKNOWN |
| TLS startup / TCB | not in public evidence | **UNKNOWN** | No | PS5 TLS UNKNOWN |
| Module init/fini routines | not in public evidence | **UNKNOWN** | No | PS5 init semantics UNKNOWN |
| Process address policy / ASLR | not in public evidence | **UNKNOWN** | Synthetic deterministic layout only | PS5 policy UNKNOWN |
| Homebrew executable conventions | no authoritative public spec located | **UNKNOWN** | No | no primary evidence |

## Conclusions for Phase 7

1. Implement a **generic** process model: address space, guest stack, startup
   descriptor/stack builder, module graph, load-bias allocation. All are P5EMU
   infrastructure, explicitly **not** claims about PS5.
2. Do not fabricate PS5 argc/argv/auxv/TLS/init-routines/address policy. Fields
   whose policy is UNKNOWN are either absent or left at a documented P5EMU
   synthetic default.
3. Module dependency semantics: generic `DT_NEEDED` only. Real PS5 module
   metadata is UNKNOWN and is not parsed.
4. Symbol identity stays the Phase-5 typed identity; no Sony NID is invented.

This audit is the Phase-8 gate: **no real PS5 runtime/HLE behaviour may be
implemented until the UNKNOWN rows acquire independent, primary evidence.**
