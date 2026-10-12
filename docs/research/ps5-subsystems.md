# PS5 Subsystem Map

**Phase 0 research artefact.**

## Evidence tags

| Tag | Meaning |
|---|---|
| **PROVEN** | Observed in the actual upstream source at the cited commit and file path |
| **EXT** | Public external documentation (AMD/Intel/ARM specs, Apple docs) |
| **INFERRED** | Reasoned. The reasoning is stated. Not established fact |
| **UNKNOWN** | No public information. Must be determined by P5EMU's own measurement |

**P5EMU never guesses.** An UNKNOWN stays UNKNOWN until it is measured on
hardware or found in a primary specification. Inventing PS5 behaviour is worse
than not implementing it, because it produces code that looks correct and is not.

Sources, pinned:

| Project | URL | Commit | Licence |
|---|---|---|---|
| KytyPS5 | https://github.com/KytyPS5/KytyPS5 | `16a7fe689a642deeeef85a706e7da305eca6fb85` (2026-10-11) | GPL-2.0-only |
| Kyty | https://github.com/InoriRus/Kyty | `4733b7e1c91b10554a52007903d74dc76c39a230` | MIT |
| shadPS4 | https://github.com/shadps4-emu/shadPS4 | `3a9f57265d28975658f1ca941daa5c879c7a74bd` (2026-10-11) | GPL-2.0-or-later |

Facts below were extracted from those trees. **No code was copied.** See
[`licenses.md`](licenses.md).

---

## 1. Executable loading

| Item | Finding | Tag |
|---|---|---|
| Format | ELF64, **FreeBSD-derived** Sony ABI. `src/loader/elf.h:17-137` | PROVEN |
| `e_type` | `ET_DYNEXEC = 0xfe10`, `ET_DYNAMIC = 0xfe18` | PROVEN |
| Machine/OS ABI | `EM_X86_64 = 62`, `ELFOSABI_FREEBSD = 9` | PROVEN |
| Dynamic tags | Sony namespace `0x61xxxxxx`: `DT_OS_DYNLIBDATA=0x61000000`, `DT_OS_PROCPARAM=0x61000001`, `DT_OS_MODULE_INFO=0x6100000d`, `DT_OS_NEEDED_MODULE=0x6100000f`, `DT_OS_RELRO=0x61000010`, `DT_OS_MODULE_ATTR=0x61000011`, `DT_OS_EXPORT_LIB=0x61000013`, `DT_OS_IMPORT_LIB=0x61000015`, `DT_OS_FINGERPRINT=0x61000007`, `DT_OS_HASH=0x61000025`, `DT_OS_PLTGOT=0x61000027`, `DT_OS_JMPREL=0x61000029`, `DT_OS_RELA=0x6100002f`, `DT_OS_STRTAB=0x61000035`, `DT_OS_SYMTAB=0x61000039`, `DT_OS_HASHSZ=0x6100003d`, `DT_OS_SYMTABSZ=0x6100003f`. PS5-era `_1` variants from `0x61000041` | PROVEN |
| Program headers | `PT_OS_DYNLIBDATA`, `PT_OS_PROCPARAM`, `PT_OS_RELRO` | PROVEN |
| Relocations | `R_X86_64_64`, `_GLOB_DAT`, `_JUMP_SLOT`, `_RELATIVE`, `_DTPMOD64` | PROVEN |
| Self-description | `SelfHeader` struct at `elf.h:139` | PROVEN |
| Game containers | `eboot.bin` in a directory, or ZArchive `.zar` mounted read-only and streamed | PROVEN |
| Firmware | `src/loader/systemContent.cpp` loads sysmodules | PROVEN |
| "crafkit" | **Zero occurrences in KytyPS5 `src/`.** It is a Sony build tool; the emulator consumes its *output* | PROVEN (absent) |
| Semantics of the `_1` tags | Not documented publicly | **UNKNOWN** |

**P5EMU need:** a complete ELF64 loader for this dialect. This is a *format
specification* task, not a code task — the constants above are facts that may be
re-derived and re-typed.

---

## 2. Guest virtual memory

| Item | Finding | Tag |
|---|---|---|
| Page size | **0x4000 (16 KiB)** — `src/kernel/memoryAddressSpace.inc:1006` | PROVEN |
| Identity mapping | **Guest VA == host VA**, same physical pages for CPU and GPU | PROVEN |
| System-managed | `0x0000040000` – `0x07ffffbfff` | PROVEN |
| System-reserved | up to `0x0fffffffff` | PROVEN |
| User | `0x1000000000` – `0xfbffffffff`. **Apple x86-64: starts at `0x7000000000`**, because Rosetta/macOS occupies `0x1000000000`–`0x0fbfffffff` | PROVEN |
| Extended region | `kExtendedMemoryBase = 0x080000000000`, `kExtendedMemorySize = 512 GiB` — `src/kernel/memory.h:27-28` | PROVEN |
| Default map base | `GUEST_DEFAULT_MAP_BASE = 0x200000000` | PROVEN |
| Protection axes | `PROT_CPU_READ/WRITE/EXEC` (0x01/0x02/0x04) **vs** `PROT_GPU_READ/WRITE` (0x10/0x20) — `src/kernel/memory.cpp:58-62` | PROVEN |
| Coherency | By **aliasing the same pages**, not by explicit cache flushes. Credited to shadPS4 by KytyPS5's README | PROVEN |
| Reservation technique | Windows `MEM_REPLACE_PLACEHOLDER`/`VirtualAlloc2`; Linux `memfd_create` + `MAP_FIXED`; Apple `shm_open` (`memfd_create` unavailable) | PROVEN |
| Huge pages | **VERIFIED ABSENT.** No `MADV_HUGEPAGE`, no `MEM_LARGE_PAGES`. All allocation is 16 KiB granular | PROVEN (absent) |
| Region taxonomy | system-managed / system-reserved / user — the same split exists in shadPS4 (`SystemManagedVirtualBase`, `SystemReservedVirtualBase`) | PROVEN |
| PS5 VA width | **Wider than PS4's 40 bits** (`ADDRESS_SPACE_BITS=40` in shadPS4). Reasoning: `HOST_USER_MAX=0xfbffffffff` is already 48-bit, and the 512 GiB region sits above `0x080000000000` | INFERRED |
| dmem layout, kernel reservations | Not enumerated anywhere public | **UNKNOWN** |
| Apple ARM64 layout | Both PS4/PS5 emulators special-case Apple only for x86-64 (Rosetta). **ARM64 host VA layout with 16 KiB pages is unstudied** | **UNKNOWN** |

> **This is P5EMU's highest-risk memory unknown.** The 512 GiB reservation and a
> 4 GiB page-table pool are both far beyond iOS limits. A demand-paged design is
> mandatory on mobile, and it will look nothing like the identity-mapped model
> above.

---

## 3. Kernel / HLE

| Item | Finding | Tag |
|---|---|---|
| Model | **Symbol-database HLE, not a syscall gate.** Guest imports are resolved against a host-side symbol DB | PROVEN |
| Registration DSL | `LIB_VERSION(library, lib_ver, module, major, minor)`, `LIB_FUNC(name, hostfunc)`, `LIB_OBJECT(name, hostobj)` — `src/libs/libs.h:20-45` | PROVEN |
| Resolution record | `SymbolResolve{name, library, library_version, module, module_version_major, module_version_minor, type, func}` | PROVEN |
| Components | `src/loader/symbolDatabase.*`, `src/loader/runtimeLinker.*`, `src/loader/systemContent.*` | PROVEN |
| LLE | Real `libkernel`/sysmodules are loaded and executed for real; the emulator supplies the other side | PROVEN |
| Library count | ~39 HLE libraries in `src/libs/` | PROVEN |
| Strategy | HLE-only. KytyPS5 README: *"External low-level emulation modules are neither required nor planned."* | PROVEN |
| Individual `sceKernel*` semantics | Only `sceKernelSyncOnAddressWait32/64` are individually visible | PROVEN (names) / UNKNOWN (behaviour) |
| Sysmodule version resolution rules | Structure proven; the rule set is not public | **UNKNOWN** |
| `SceKernelError` values | Not public | **UNKNOWN** |
| Guest exception delivery | Not public | **UNKNOWN** |

---

## 4. Threading

| Item | Finding | Tag |
|---|---|---|
| Pthread types | `PthreadAttrPrivate`, `PthreadPrivate`, `PthreadMutex{Private,AttrPrivate}`, `PthreadRwlock{Private,AttrPrivate}`, `PthreadCond{Private,AttrPrivate}` — `src/kernel/pthread.h:38-47` | PROVEN |
| Time types | `KernelTimespec{tv_sec,tv_nsec}`, `KernelTimeval{tv_sec,tv_usec}` | PROVEN |
| Fibers | `src/common/threads.{h,cpp}` | PROVEN |
| Host mapping (shadPS4) | **Real host threads** (`pthread_create`/`CreateThread`) plus `ucontext` fibers | PROVEN |
| FPU state init | `_mm_setcsr(0x9fc0)`, `fldcw 0x037f`, alternate signal stack ≥ 64 KB | PROVEN |
| Host mapping (KytyPS5) | **Not identified.** Grep for `CreateThread`/`std::thread`/`ucontext` in `src/kernel/pthread.cpp` found nothing | **UNVERIFIED** |
| PS5 thread counts, scheduling policy | Not public | **UNKNOWN** |

---

## 5. Synchronization

| Item | Finding | Tag |
|---|---|---|
| Primitives | `src/kernel/eventFlag.*`, `eventQueue.*`, `semaphore.*`, `syncOnAddress.*` | PROVEN |
| Named API | `sceKernelSyncOnAddressWait32`, `sceKernelSyncOnAddressWait64` | PROVEN |
| Tests | `SyncOnAddressTests`, `EventQueueLifetimeTests` | PROVEN |
| `SceEventFlag` bit layout | Not public | **UNKNOWN** |
| Wait-flag combinations, timeout semantics | Not public | **UNKNOWN** |

---

## 6. Filesystem

| Item | Finding | Tag |
|---|---|---|
| Implementation | `src/kernel/fileSystem.*`, `KernelFileSystemTests` | PROVEN |
| Containers | ZArchive `.zar` mounted read-only, streamed, no extraction | PROVEN |
| Save data | `src/libs/libSaveData.cpp`, `saveDataMountSlots.h`, `SaveDataMemoryTests`, `SaveDataDialogTests` | PROVEN |
| Patching | `src/loader/gamePatch.*` | PROVEN |
| Path grammar, `sceKernelOpen` flag matrix | Not public | **UNKNOWN** |
| Save-data on-disk format | Not public | **UNKNOWN** |

---

## 7. System libraries

Existence is **PROVEN** (directory listing at the pinned commit). Semantics are
almost entirely **UNKNOWN**.

`libC, libKernel, libSysmodule, libAgcDriver, libVideoOut, libVideoDec2,
libAudio, libAudio2, audio, audio3d, acm, ngs2, ajm, libPad, libSaveData,
libFont, libFontFt, libJson2, libNet, libRtc, libTrophy, libUserService,
libSystemService, libPlayGo, libShare, libRudp, libUlt, libUsbd,
libDeviceService, libTextToSpeech2, libWebBrowserDialog, libConvertKeycode,
libDbgAsan, libDebug, libAmpr, libAppContent, libBluetoothHid, libCes, libHmd2,
libIme, libPngDec, libPngEnc, libPsml, libDialog`

Scale reference: shadPS4's equivalent, `src/core/aerolib/aerolib.inl`, is
**171,520 lines / 9.3 MB** of NID→stub mappings, generated from NID lists.

> This is a multi-year subsystem, and it is the one where "compatibility" will be
> measured in percentage of libraries stubbed. It is also the one where a
> game-specific hack is most tempting — which is exactly what shadPS4's
> `AGENTS.md` rule forbids and what P5EMU adopts.

---

## 8. Graphics command processing

| Item | Finding | Tag |
|---|---|---|
| GPU codename | **Prospero** | PROVEN |
| Packet format | PM4-type-2, header `0xC0000000` — `pm4.h:11-14` | PROVEN |
| Opcode range | `IT_NOP=0x10` … `IT_WRITE_CONST_RAM=0x81` | PROVEN |
| Selected opcodes | `IT_MEM_SEMAPHORE=0x39`, `IT_WAIT_REG_MEM=0x3C`, `IT_SURFACE_SYNC=0x43`, `IT_SET_SH_REG_INDIRECT=0x63` | PROVEN |
| Dispatch | `command_processor/pm4Dispatch.cpp`, `pm4Handlers.cpp` | PROVEN |
| Queue topology | 1 graphics + 56 compute (7 pipes × 8 queues), compute base `0x20` — `graphicsRun.h:48-53` | PROVEN |
| Submission | Dedicated host GPU thread. `SendCommand`, `SendCommandSync`, `SubmitFlipPreparation`, `SuspendPoint` | PROVEN |
| AGC ABI | `AgcInit`, `AgcCreateShader`, `AgcGetRegisterDefaults2`, `AgcSetShRegIndirectPatchSetAddress` | PROVEN (names) |
| DCB defaults | `agcRegisterDefaults.inc` — **GPL, not reusable. Must be dumped from hardware or found in public docs** | PROVEN (exists) |
| Queue work distribution, EOP/EOS/LMEM ordering | Opcode IDs are public; the state machine is not | **UNKNOWN** |
| `AgcUnknown*` semantics | Obfuscated SDK-derived names | **UNKNOWN** |
| Tiling / G-mode layout | Existence proven (`guest_gpu/tile.*`, host GLSL tiler `gpu_tiler_*.inc`); bit layout not | PROVEN (exists) / **UNKNOWN** (layout) |

---

## 9. AMD RDNA2 instruction decoding

| Item | Finding | Tag |
|---|---|---|
| ISA | RDNA2. Reference: **AMD RDNA 2 ISA Reference Guide, doc 70648** | EXT |
| Families | `SOP1, SOP2, SOPK, SOPC, SOPP, VOP1, VOP2, VOP3, VOP3P, VOPC, VINTRP, SMEM, MUBUF, MTBUF, FLAT, DS, MIMG, EXP` — `ShaderDecoder.h:18-36` | PROVEN |
| Opcode count | ~1000 entries in `enum class Opcode` | PROVEN |
| Decode strategy | Hand-written, `consteval` compile-time perfect-hash `OpcodeTable` — zero runtime decode cost | PROVEN |
| Unsupported handling | `enum class Opcode { UNKNOWN, UNSUPPORTED, ... }` — "I saw it and I can't do it" beats silent miscompilation | PROVEN (mechanism) |
| **Actual coverage** | Gaps demonstrably exist; the percentage is unpublished | **UNKNOWN** |

P5EMU must write its own decoder against doc 70648. The doc is public vendor
documentation; the encodings are facts, not copyrightable expression.

---

## 10. Shader translation

| Item | Finding | Tag |
|---|---|---|
| Pipeline | 5 stages: `frontend/decode` → `frontend/cfg` → `frontend/translate` → `ir` + passes → `backend/spirv` | PROVEN |
| IR | Custom SSA. `Value.h`, `Type.h`, `Block.h`, `opcodes/ValueOpcodes.inc` | PROVEN |
| Passes | `ConstantPropagation`, `DeadCodeElimination`, `ReadLaneElimination`, `SsaRewrite`, `ShaderInfoCollection`, `ResourceMaterialization`, `ResourceTracking`, `SrtWalker` | PROVEN |
| Backend | Hand-written SPIR-V emitter, split `spirvEmitter{Alu,Memory,Image,Flow,Analysis,Bvh,Mesh,Module,Program,Tessellation}` | PROVEN |
| Libraries | **SPIRV-Tools + SPIRV-Headers only.** NOT glslang, NOT SPIRV-Cross, NOT shaderc for the translation path. glslang is build-time only, for host tiler GLSL | PROVEN |
| Stages | VS, PS, CS, GS(mesh), Tessellation | PROVEN |
| Shader fetch | SH/SHADER_READ_MEM registers. `ShaderParams{code, user_data[40] /* 32 SGPRs + merged s0:s7 */, hash, back_code}` | PROVEN |
| Pipeline caching | `BuildStageStaticKey()` — the right granularity for a PSO cache | PROVEN |
| SRT | First-class IR concept: `ResourceKind` enum + `SrtWalker` | PROVEN |
| SRT binary layout | Not public | **UNKNOWN** |
| Pipeline cache serialization format | Not public | **UNKNOWN** |

> ~40 files in GPL. The RDNA2 encodings are public; the translation *strategy* is
> GPL-encoded engineering. It must be written fresh.

---

## 11. Resource tracking

| Item | Finding | Tag |
|---|---|---|
| Runtime caches | `bufferCache`, `textureCache`, `samplerCache`, `streamBuffer`, `faultManager` | PROVEN |
| Memory tracking | `memoryTracker.*`, `pageManager.*`, `regionDefinitions.h`, `regionManager.h`, `rangeSet.h` | PROVEN |
| Page tables | `multiLevelPageTable.h` (MLPT) modelling AMD g-TT style tables | PROVEN |
| Compile-time | `ResourceTracking`, `ResourceMaterialization`, `SrtWalker` passes | PROVEN |
| Tests | `ResourceTrackingTests`, `ResourceMaterializationTests`, `ImagePageTableTests`, `MemoryTrackerTests`, `PageManagerTests` | PROVEN |
| Residency / eviction rules | Not public | **UNKNOWN** |
| Guest page-table bit layout (g-TT/UMMU) | Modelled but not documented | **UNKNOWN** |

---

## 12. Audio

| Item | Finding | Tag |
|---|---|---|
| Components | `audio.{h,cpp}`, `audio_internal.h`, `audio3d.*`, `acm.*`, `ngs2.{h,cpp}`, `libAudio.cpp`, `libAudio2.cpp` | PROVEN |
| Codecs | `src/libs/ajm/`: AAC, ATRAC9, MP3, Opus, FFmpeg decoders | PROVEN |
| Haptics | `dualSenseHaptics.*`, `padData.h` | PROVEN |
| Host backend | SDL3 audio | PROVEN |
| Tests | `AudioOutTimingTests`, `AudioOut2PortTests`, `Ngs2SamplerTests`, `PadHapticsTests` | PROVEN |
| NGS2 packet format, handle semantics, sampler graph, HRTF parameters | Not public | **UNKNOWN** |
| `sceAudioOut2` port topology | Not public | **UNKNOWN** |
| DualSense haptic waveform format | Not public | **UNKNOWN** |

---

## 13. Input

| Item | Finding | Tag |
|---|---|---|
| Components | `libPad.cpp`, `libController.cpp`, `controller.h`, `padData.h`, `libBluetoothHid.cpp`, `libUsbd.cpp`, `libSystemGesture.cpp`, `dualSenseBluetooth.*` | PROVEN |
| Host input | `graphics/presentation/window/hostInput.{h,cpp}` | PROVEN |
| Tests | `ControllerSettingsTests`, `PadHapticsTests` | PROVEN |
| `ScePad` report format, DualSense HID layout, lightbar/HID/rumble reports | Not public | **UNKNOWN** |
| **Touchpad and touchscreen gesture API** | **Not implemented anywhere public.** PS5's touchpad has no public treatment | **UNKNOWN** |

---

## 14. Video decode

| Item | Finding | Tag |
|---|---|---|
| Implementation | `libVideoDec2.cpp`, `videoDec2Decoder.{h,cpp}` | PROVEN |
| Types | `enum class Result { Ok, ApiFail, AccessUnit, FrameBufferSize, OversizeDecode }`, `Config{codec_type, max_width, max_height}`, `Input{data, size, pts, dts, attached_data}`, `FrameBuffer`, `Output{valid, error_frame, ...}`, `TIMESTAMP_INVALID` | PROVEN |
| Host decode | FFmpeg (`ffmpeg-core`), VP9 + WebM enabled | PROVEN |
| Presentation | `presentation/videoOut.{h,cpp}`, `presenter.h`, `vulkanWindow.cpp`, `swapchain.cpp`, `systemOverlay.*` | PROVEN |
| AVPlayer | `libAvPlayer` HLE, `AvPlayerFileTests`; ffmpeg statically linked | PROVEN |
| `codec_type` enum values, PS5 HW codec support, `attached_data` semantics | Not public | **UNKNOWN** |

---

## 15. Network

| Item | Finding | Tag |
|---|---|---|
| Components | `libNet.cpp`, `network.{h,cpp}`, `libRudp.cpp`, `libPlayGo.cpp`, `libUserService.cpp`, `libShare.cpp`; shadPS4 `src/core/net/` | PROVEN (exists) |
| Protocol | Not public | **UNKNOWN** |

> **Out of scope for P5EMU's initial phases.** No network stack is planned.

---

## 16. Timing

| Item | Finding | Tag |
|---|---|---|
| Guest clock | `src/loader/timer.{h,cpp}`, used as the log timestamp source | PROVEN |
| Host clock | `src/common/timer.{h,cpp}`, `src/common/platform/sysTimer.h` | PROVEN |
| RTC | `src/libs/libRtc.cpp` (`sceKernelRtc*`), `src/common/dateTime.*` | PROVEN |
| Tests | `AudioOutTimingTests` | PROVEN |
| Exact clock source, tick rate, drift model | Not public | **UNKNOWN** |

---

## 17. Debugging and logging

| Item | Finding | Tag |
|---|---|---|
| Log | `src/common/logging/log.{h,cpp}` — `Log::Color`, `Log::Direction::Silent`, `LOGF_COLOR` | PROVEN |
| HLE tracing | `src/libs/guestPrintf.cpp`, `PrintName()` logs every HLE call as `[thread][h:mm:ss.mmm][lib::module::func]()`, toggled by `PRINT_NAME_ENABLE` — `src/libs/libs.h:61-76` | PROVEN |
| Debug | `src/common/debug.{h,cpp}`, `assert.*`, `hostException.*`, `profiler.*` + Tracy | PROVEN |
| Platform | `sysDbg.h` + `sysLinuxDbg.cpp`/`sysWindowsDbg.cpp` | PROVEN |
| Guest debug libs | `libDebug.cpp`, `libDbgAsan.cpp` | PROVEN |
| shadPS4 | `src/core/{debugger,debug_state,devtools/}` + RenderDoc capture; `documents/Debugging/Debugging.md` | PROVEN |

> This is one of the two areas P5EMU can copy *in spirit* immediately. The
> per-HLE-call trace format is cheap and enormously valuable.

---

## 18. CPU execution model (the PS4/PS5 approach, for contrast)

| Item | Finding | Tag |
|---|---|---|
| Model | **Native x86-64 on an x86-64 host + static instruction rewriting.** No JIT, no Dynarmic, no LLVM, no interpreter | PROVEN |
| Patcher | `src/loader/guestInstructionPatcher.h` — `GetGuestInstructionHostFeatures() → {sse4a, rdpid, clwb}`; `InstructionPatchCounts{found, native, trapped}` | PROVEN |
| Escape hatch | `src/loader/x64InstructionEmulator.h::TryEmulate(void*)` | PROVEN |
| shadPS4 | `src/core/cpu_patches.{h,cpp}`; `RedZonePatchResult` with instruction counters | PROVEN |
| Derivation | `guestInstructionPatcher.h:1-2` carries `SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project` | PROVEN |
| **ARM64 host** | **VERIFIED ABSENT in both.** KytyPS5 CI asserts x86-64 artifacts; macOS = Rosetta. Only two `aarch64` `#elif`s exist, for host-GPU page sizes | PROVEN (absent) |

This is the model P5EMU is *not* adopting, and the reason is in
[`cpu-feasibility.md`](cpu-feasibility.md) §1.

---

## 19. Testing (as practised elsewhere)

| Project | Approach | Tag |
|---|---|---|
| KytyPS5 | 31 CTest binaries, unit-level, on Win/Linux/macOS CI. Fault-injection hooks under `KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS` | PROVEN |
| shadPS4 | Effectively none in-tree. `AGENTS.md`: *"Do not include tests. If emulation behaviour is changed, write and share hardware tests in the form of OpenOrbis homebrew."* | PROVEN |
| PPSSPP | Cross-backend `--compare`, hardware-recorded `.expected` files | PROVEN |
| Play! | Native-vs-JIT differential per test, every host backend in CI | PROVEN |

> KytyPS5 has **no end-to-end guest-program regression test** in CI. P5EMU
> should not repeat that: the differential harness is the whole defence, and it
> needs an end-to-end leg.

---

## 20. Summary: what P5EMU must determine itself

Ordered by expected difficulty × impact.

### Blockers

| # | Unknown | How to attack |
|---|---|---|
| 1 | **x86-64 → ARM64 execution strategy** | Phase 1/2 spike. No surveyed project has this |
| 2 | **Complete PS5 guest address space**, dmem layout, kernel reservations | Hardware probe reading `auxv`/AT_PHDR and observing allocation behaviour |
| 3 | **AGC DCB register defaults** | Dump from real hardware, or find public documentation |
| 4 | **PM4/AGC state machine** beyond opcode IDs | Hardware instrumentation + differential rendering |
| 5 | **RDNA2 shader coverage** | Build decode/translate with first-class `UNSUPPORTED` reporting and instrument it from day one |

### High impact

6. Every `sceKernel*` semantic, error code, edge case
7. Sysmodule version resolution rules
8. NGS2 packet framing, sampler graph, 3D-audio HRTF space
9. SRT binary layout per stage/pipeline state
10. Guest page-table (g-TT/UMMU) format, residency, eviction
11. Render-target G-mode layout and compression
12. Guest clock source, tick rate, drift
13. `ScePad`/DualSense HID layouts, haptics waveforms
14. `sceVideoDec2` codec types, HW support, buffer negotiation
15. Firmware module inventory and versioning
16. Save-data mount slots and on-disk format
17. Guest exception/signal delivery semantics
18. Apple ARM64 host VA layout with 16 KiB pages (new — nobody has done this)
19. PS5 touchpad/touchscreen gesture API (new — nobody has done this)

### Explicitly unverified in this research

- Whether the `_1` dynamic tag variants change any semantics
- Whether `InoriRus/Kyty` is still maintained
- KytyPS5's pthread host-mapping mechanism
- `ZArchive` and `opus` licences
- The provenance of Sony-derived knowledge in these projects. KytyPS5 states no
  SDK is bundled, but where specific constants came from is not stated.
  **Where P5EMU depends on a specific, verify it against hardware independently.**

---

## 21. Standing rules

1. **UNKNOWN stays UNKNOWN.** No guessed PS5 behaviour enters the codebase.
2. **No Sony material, ever.** No firmware, keys, sysmodules, SDKs, game data,
   trademarks, or UI assets. Not even "just for a test".
3. **Facts may be learned and re-typed. Lines may not be copied.** Constants and
   encodings are facts; GPL source lines are a licence event.
4. **Hardware-verified behaviour beats code archaeology.** A fact P5EMU measured
   itself is P5EMU's fact, even if someone else found it too.
5. **No game-specific hacks.** Fixes address the underlying emulation behaviour.
   (Adopted verbatim from shadPS4's `AGENTS.md`.)
