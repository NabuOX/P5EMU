# PS5 Executable Loading Foundation (Phase 4)

Phase 4 moves P5EMU above the CPU for the first time: it understands and safely
maps the executable format it will eventually run, without executing a
commercial title and without implementing a kernel, HLE, or any Sony material.

```
PS5-style ELF64 bytes
    │  BinaryReader        (bounds- and overflow-checked, endian-aware)
    ▼
GenericElfImage            (generic ELF64 only; no console assumptions)
    │  Ps5ExecutableParser
    ▼
Ps5ExecutableImage         (PS5 dialect; raw + interpreted dynamic table)
    │  BuildLoadPlan / ValidateLoadPlan
    ▼
LoadPlan                   (immutable; R/W/X intent; BSS; unresolved imports)
    │  ApplyLoadPlan          (transactional)
    ▼
MappedGuestMemory          (multi-region, permission-enforcing GuestMemoryPort)
    │  MakeEntryState
    ▼
CpuState{RIP = entry}
```

## 1. Format boundary

Generic ELF64 parsing is completely separate from PS5/Prospero interpretation.

- `core/include/p5emu/loader/BinaryReader.h` — safe reader.
- `core/include/p5emu/loader/Elf64.{h,cpp}` — generic ELF64, machine-agnostic,
  usable by independently authored fixtures.
- `core/include/p5emu/loader/Ps5Executable.{h,cpp}` — the console layer.
- `core/include/p5emu/loader/Loader.{h,cpp}` — plan, validation, apply, entry.

No PS5 assumption appears in `Elf64.*`.

## 2. Safe binary reader

`BinaryReader` enforces, in one place:

- every read is bounds-checked (`CheckRange`);
- every attacker-controlled combination is overflow-checked
  (`AddOverflows`, `MulOverflows`) — `off+len`, `count*entry_size`,
  `vaddr+memsz`;
- no `reinterpret_cast` of the buffer into a struct and no unaligned host load:
  multi-byte scalars are assembled byte by byte, so parsing is correct
  regardless of host alignment and endianness;
- deterministic `Status` failures; a failed read writes nothing.

## 3. Segment validation and the LoadPlan

`ValidateGenericLoadSegments` enforces the ELF-generic segment rules:
`filesz <= memsz`, the file range inside the input, `vaddr+memsz` no overflow,
`p_align` a power of two and congruent with `p_offset`.

`ValidateLoadPlan` enforces the plan rules: permissions non-empty, no two mapped
segments overlapping (explicit overlap policy: overlap is rejected), the entry
point inside an executable mapping, and a total mapped size bound
(`kMaxLoadImageBytes`, 256 MiB) so a single untrusted file cannot demand an
unbounded allocation. The zero-fill (BSS) tail `memsz - filesz` is explicit.

## 4. Transactional apply

`ApplyLoadPlan` stages the whole plan into a fresh `MappedGuestMemory` and
replaces the target only on full success; on any failure the staging port is
discarded and the target is untouched — no partially loaded guest memory. A
test-only `ApplyOptions::fail_after_operations` injects a mid-apply failure and
proves the target is unchanged.

## 5. Permissions

Segments map to `MappedGuestMemory` regions with independent R/W/X from the
`PT_LOAD` flags. The port ENFORCES them on guest access (a write to a read-only
region returns `kReadOnlyAccess`), so there is no blanket RWX model. The loader
populates regions through `WriteImage`/`ZeroFill`, which bypass the final
permissions the way a loader must when filling a read-only text segment before
protections apply.

## 6. Entry handoff

`MakeEntryState` sets `RIP = validated entry point` and nothing else. Stack,
argv/envp, auxv, TLS, and kernel objects are NOT modelled and remain zero;
inventing them would be guessing PS5 process startup.

## 7. PS5 fact provenance

Every console-specific constant in production source has traceable provenance.
"Documented externally" means the fact appears in public source/documentation at
a pinned commit; it is re-typed, not copied, and it is not hardware-verified.

| P5EMU concept | Fact | Source | Classification | Implementation |
|---|---|---|---|---|
| Guest machine | `EM_X86_64 = 62` | System V ABI / ELF gABI | PROVEN_FROM_SOURCE | `Elf64.h` |
| Guest OS ABI | `ELFOSABI_FREEBSD = 9` | System V ABI | PROVEN_FROM_SOURCE | `Ps5Executable.h` |
| PS5 executable types | `ET_DYNEXEC = 0xfe10`, `ET_DYNAMIC = 0xfe18` | KytyPS5 `src/loader/elf.h` @ `16a7fe689a642deeeef85a706e7da305eca6fb85` | DOCUMENTED_EXTERNALLY | `Ps5Executable.h` |
| Sony dynamic tag namespace | `DT_OS_* = 0x610000xx` (module/lib/reloc/strtab tags) | KytyPS5 `elf.h` @ `16a7fe6` | DOCUMENTED_EXTERNALLY | `Ps5Executable.h` |
| PS5-era `_1` dynamic variants | range from `0x61000041` | KytyPS5 `elf.h` @ `16a7fe6` | **UNKNOWN** (semantics) | preserved, not interpreted |
| Sony program-header types | `PT_OS_DYNLIBDATA`, `PT_OS_PROCPARAM`, `PT_OS_RELRO` | KytyPS5 `elf.h` @ `16a7fe6` | **UNKNOWN** (numeric values not published in our research) | not matched |
| Sony symbol/import structures | layout of `DT_OS_*` targets | not documented publicly | **UNKNOWN** | not decoded |

Consequently the loader discovers *module* requirements via the generic
`DT_NEEDED` tag (name resolved through the dynamic string table) and records
that an image declares PS5 imports/exports, but it does NOT decode the Sony
symbol table: that structure is UNKNOWN and is left for a later phase.

## 8. Error taxonomy

Loading uses the project's single `Status` model. Categories:
`kInvalidMagic`, `kUnsupportedClass`, `kUnsupportedEndian`, `kUnsupportedMachine`,
`kTruncatedInput`, `kIntegerOverflow`, `kTooManyEntries`,
`kInvalidProgramHeader`, `kInvalidSegment`, `kInvalidAlignment`,
`kOverlappingMapping`, `kInvalidEntryPoint`, `kMalformedDynamicTable`,
`kUnsupportedRequiredFeature`, `kImageTooLarge`, `kApplyFailed`. Diagnostics
never contain file content.

## 9. Out of scope (deliberately)

No SELF/encrypted-executable handling, no keys, no firmware, no SDK, no kernel,
no syscalls, no HLE implementations, no threads, no TLS startup, no GPU/audio.
Encountering a protected/encrypted container is a documented boundary, not a
target.

## 10. Typed SCE dynamic metadata (Phase 9)

Phase 9 adds a typed boundary (`loader::SceMetadata`) over the Phase-4 dynamic
table. It separates two questions that were previously implicit:

- **Is the tag NUMBER recognized?** and **are its SEMANTICS understood?**

`LookupSceTag(number)` returns an `SceTagInfo` whose `SceTagKnowledge` is one of
`kUnrecognized`, `kNumberAndName`, or `kKnownSemantics`. Every Sony `DT_OS_*` tag
(and the `_1` variant range) is `kNumberAndName` with `SceTagPayload::kUnknown`
and `semantics_known() == false`: the name is evidenced, the payload is not.
Only System V generic tags are `kKnownSemantics`.

`BuildSceModuleMetadata(image)` deterministically buckets the dynamic entries
into `sce_tags` / `generic_tags` / `unrecognized`, preserves every raw value, and
counts `unknown_semantics_count`. `ValidateSceMetadata` applies one conservative
P5EMU policy: a recognized tag that repeats with **differing** values is
`kConflictingMetadata` (a synthetic policy, not a PS5 claim).

`SceNid` provides NID **representation only** (hold + compare an evidenced opaque
identifier, and convert to `SymbolIdentity::NumericId`). There is deliberately
**no NID generation algorithm**. `SceLibraryIdentity` preserves module/library
identity as an opaque `SymbolIdentity`; it is never turned into a guessed name.

Nothing in this layer consumes an unknown-semantics tag as if it were supported,
and the linker/relocation pipeline is unchanged. See
[`../research/ps5-dynamic-linking-phase9.md`](../research/ps5-dynamic-linking-phase9.md).
