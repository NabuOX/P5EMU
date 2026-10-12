# PS5 Executable & Dynamic-Linking Evidence Ledger (Phase 9)

Phase 9 expands P5EMU's **evidence-backed representation** of PS5/SCE
executable and dynamic-linking metadata, while keeping the Phase 8 rule intact:
**no PS5-specific implementation without traceable evidence; unknown behaviour
stays UNKNOWN.**

This ledger records every SCE/Sony dynamic-tag claim considered for
implementation, per the project evidence classes:

| Class | Meaning |
|---|---|
| **A — Documented fact** | Published in a spec or re-typed from a public external document. |
| **B — Observed behaviour** | Determined by lawful, reproducible observation. |
| **C — Inference / hypothesis** | Reasoned from other evidence. |
| **D — Synthetic P5EMU model** | A P5EMU-owned placeholder, **not** a PS5 claim. |

## Source register

No code was copied. Facts and architecture were re-expressed independently.

| Source | Identity / revision | Licence | Usage |
|---|---|---|---|
| ELF gABI / System V AMD64 psABI | specification | spec | Documented fact |
| KytyPS5 | `github.com/KytyPS5/KytyPS5` @ `16a7fe689a642deeeef85a706e7da305eca6fb85` | GPL-2.0-only | Tag numbers + names (facts only) |
| Kyty | `github.com/InoriRus/Kyty` | MIT | ELF tag namespace facts |
| shadPS4 | `github.com/shadps4-emu/shadPS4` @ `3a9f57265d28975658f1ca941daa5c879c7a74bd` | GPL-2.0-or-later | Subsystem inventory only |
| P5EMU Phase 4/7/8 ledger | this repository | Apache-2.0 | Prior classifications |

> No Sony SDK, firmware, keys, or game material was consulted. GPL-licensed
> project source was read for tag *numbers/names* only; no GPL code enters
> Apache-2.0 P5EMU.

## Sony / SCE dynamic tags — individual audit

For each tag the Phase-9 representation records: the numeric value (fact), an
evidenced **name** (fact), and a payload classification. **Crucially, the name
does not establish the payload**: P5EMU marks every Sony tag
`SceTagKnowledge::kNumberAndName` with `SceTagPayload::kUnknown` (semantics
UNKNOWN) and `semantics_known() == false`.

| Tag number | Evidenced name | Payload type | Duplicate legal? | Order matters? | Affects runtime today? |
|---|---|---|---|---|---|
| `0x61000000` | `DT_OS_DYNLIBDATA` | **UNKNOWN** | UNKNOWN | UNKNOWN | No |
| `0x61000001` | `DT_OS_PROCPARAM` | **UNKNOWN** | UNKNOWN | UNKNOWN | No |
| `0x61000007` | `DT_OS_FINGERPRINT` | **UNKNOWN** | UNKNOWN | UNKNOWN | No |
| `0x6100000d` | `DT_OS_MODULE_INFO` | **UNKNOWN** | UNKNOWN | UNKNOWN | No |
| `0x6100000f` | `DT_OS_NEEDED_MODULE` | **UNKNOWN** | UNKNOWN | UNKNOWN | No |
| `0x61000010` | `DT_OS_RELRO` | **UNKNOWN** | UNKNOWN | UNKNOWN | No |
| `0x61000011` | `DT_OS_MODULE_ATTR` | **UNKNOWN** | UNKNOWN | UNKNOWN | No |
| `0x61000013` | `DT_OS_EXPORT_LIB` | **UNKNOWN** | UNKNOWN | UNKNOWN | No (flag only) |
| `0x61000015` | `DT_OS_IMPORT_LIB` | **UNKNOWN** | UNKNOWN | UNKNOWN | No (flag only) |
| `0x61000025` | `DT_OS_HASH` | **UNKNOWN** | UNKNOWN | UNKNOWN | No |
| `0x61000027` | `DT_OS_PLTGOT` | **UNKNOWN** | UNKNOWN | UNKNOWN | No |
| `0x61000029` | `DT_OS_JMPREL` | **UNKNOWN** | UNKNOWN | UNKNOWN | No |
| `0x6100002f` | `DT_OS_RELA` | **UNKNOWN** | UNKNOWN | UNKNOWN | No |
| `0x61000035` | `DT_OS_STRTAB` | **UNKNOWN** | UNKNOWN | UNKNOWN | No |
| `0x61000039` | `DT_OS_SYMTAB` | **UNKNOWN** | UNKNOWN | UNKNOWN | No |
| `0x6100003d` | `DT_OS_HASHSZ` | **UNKNOWN** | UNKNOWN | UNKNOWN | No |
| `0x6100003f` | `DT_OS_SYMTABSZ` | **UNKNOWN** | UNKNOWN | UNKNOWN | No |
| `0x61000041`–`0x61ffffff` | `DT_OS_VARIANT` (PS5-era `_1`) | **UNKNOWN** | UNKNOWN | UNKNOWN | No |

Evidence class: the **numbers** and **names** are class A (re-typed from the
public KytyPS5 `elf.h`); the **payload semantics, duplicate rules, and ordering**
are **UNKNOWN**. Nothing in the table changes execution.

Names that *look* like they imply a payload (e.g. `DT_OS_STRTAB`, `DT_OS_RELA`)
are explicitly NOT treated as evidence of the payload: inferring semantics from a
name alone is a class C guess, and P5EMU keeps those payloads `kUnknown`.

## Generic (System V) dynamic tags — known semantics

These retain evidenced semantics (class A, ELF/AMD64 psABI) and are represented
with `SceTagKnowledge::kKnownSemantics` and a classified payload
(address/size/index/encoded-identifier):

`DT_NULL`, `DT_NEEDED`, `DT_PLTRELSZ`, `DT_PLTGOT`, `DT_HASH`, `DT_STRTAB`,
`DT_SYMTAB`, `DT_RELA`, `DT_RELASZ`, `DT_RELAENT`, `DT_STRSZ`, `DT_SYMENT`,
`DT_INIT`, `DT_FINI`, `DT_REL`, `DT_RELSZ`, `DT_RELENT`, `DT_PLTREL`, `DT_JMPREL`,
`DT_FLAGS_1`.

## Module / library identity

| Question | Status | Consequence |
|---|---|---|
| Generic dependency **names** (`DT_NEEDED`) | **Known** (class A) | Preserved as strings (`needed_modules`) |
| Sony library/module identifier **encoding** | **UNKNOWN** | `SceLibraryIdentity` keeps an opaque `SymbolIdentity`; never stringified |
| Presence of SCE import/export declarations (`DT_OS_IMPORT_LIB`/`EXPORT_LIB`) | **Known** (class A) | `declares_sce_imports`/`exports` flags only |
| Meaning of `DT_OS_IMPORT_LIB`/`EXPORT_LIB` payloads | **UNKNOWN** | Not decoded |

## NID representation vs generation

| Aspect | Status |
|---|---|
| **Representation** of an evidenced opaque NID (hold + compare) | Implemented (`SceNid`) |
| Conversion of an evidenced NID to `SymbolIdentity::NumericId` | Implemented |
| **Generation algorithm** (name → NID hash) | **UNKNOWN — deliberately not implemented** |

P5EMU can preserve and compare Sony identifiers without knowing how they are
generated. There is no `ComputeNid`-style function, by design.

## Typed metadata architecture (implemented)

```
ELF dynamic entries (Phase 4, bounds-checked)
  → LookupSceTag(number)            // number+name known, semantics may be UNKNOWN
  → BuildSceModuleMetadata(image)   // buckets: sce_tags / generic_tags / unrecognized
  → ValidateSceMetadata(...)        // conservative conflict detection
```

- `SceTagKnowledge` separates **recognized number** from **known semantics**.
- `SceTagPayload` is `kUnknown` for every Sony tag until a defining structure is
  evidenced.
- `SceModuleMetadata` preserves raw values and counts
  `unknown_semantics_count`; nothing consumes an unknown-semantics tag.

## Explicitly UNKNOWN (do not fabricate)

- The payload semantics of every Sony `DT_OS_*` tag and the `_1` variant range.
- Whether any Sony dynamic tag may legally repeat, and whether order matters.
- Sony library/module identifier encoding, version semantics, and the NID
  generation algorithm.
- The meaning of `DT_OS_IMPORT_LIB`/`DT_OS_EXPORT_LIB` payload structures.

## Non-claims

Phase 9 does **not** implement PS5 dynamic-linking semantics, NID generation,
Sony symbol structures, or any GPU behaviour. It provides a typed, evidence-safe
representation that **preserves** PS5 metadata, **labels** what is understood,
and **fails explicitly** where semantics are unsupported.

Phase 10 investigated one of these gaps — `DT_OS_MODULE_INFO` — and concluded the
payload semantics remain UNKNOWN (number and name known, single source, no
payload structure): see [`ps5-module-info-phase10.md`](ps5-module-info-phase10.md).
