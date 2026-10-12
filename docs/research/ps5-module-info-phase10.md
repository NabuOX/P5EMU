# PS5 Evidence Closure Gate — `DT_OS_MODULE_INFO` (Phase 10)

Phase 10 investigates **one** PS5/SCE evidence gap: the dynamic tag
`DT_OS_MODULE_INFO`. The goal is evidence closure, not implementation. **A
rigorous UNKNOWN result is a successful outcome.**

Result in one line: the tag's **number and name are established** (single
source); its **payload semantics remain UNKNOWN**; the implementation threshold
is **not met**, so no semantic behaviour is implemented.

## 1. Research question

What can be established, from admitted public/legal evidence, about
`DT_OS_MODULE_INFO` — its value, name, payload kind, module-id/version encoding,
bit layout, field widths, reserved bits, duplicate/ordering rules, and PS4-vs-PS5
applicability?

## 2. Current P5EMU claims (audited before research)

| Location | Claim |
|---|---|
| `Ps5Executable.h` | `kDtOsModuleInfo = 0x6100000d` (number) |
| `Ps5Executable.cpp` | recognized tag number; preserved verbatim, no semantics |
| `SceMetadata.cpp` | registry entry `{0x6100000d, "DT_OS_MODULE_INFO"}` → `kNumberAndName`, payload `kUnknown` |
| `ps5-subsystems.md` | number listed as PROVEN; no payload claim |
| `ps5-dynamic-linking-phase9.md` | payload/duplicate/ordering = UNKNOWN |

P5EMU currently claims the **number and name only**, and explicitly claims
nothing about the payload. This audit is unchanged by Phase 10.

## 3. Admitted sources

| Source | Revision | File | URL | Licence | PS5-specific? |
|---|---|---|---|---|---|
| KytyPS5 | `16a7fe689a642deeeef85a706e7da305eca6fb85` | `src/loader/elf.h` | `raw.githubusercontent.com/KytyPS5/KytyPS5/<rev>/src/loader/elf.h` | GPL-2.0-only | Yes (PS5 emulator); some tags plausibly PS4-shared |
| P5EMU Phase 4/9 records | this repo | `Ps5Executable.h`, `SceMetadata.cpp` | local | Apache-2.0 | derivative of the KytyPS5 source |

**Excluded:** leaked Sony SDKs/headers, proprietary SDK files, firmware, keys,
game binaries/assets, anonymous snippets, unattributed constants, forum claims
without traceable evidence. None were used.

No GPL code was copied: only the tag number and macro name were re-expressed as
facts (the same class of fact P5EMU already records).

## 4. What the admitted source actually contains

KytyPS5 `elf.h` @ `16a7fe6` defines, among many tags:

```text
constexpr Elf64_Sxword DT_OS_MODULE_INFO   = 0x6100000d;
constexpr Elf64_Sxword DT_OS_MODULE_INFO_1 = 0x61000043;
```

and the generic ELF containers (`Elf64_Dyn` with `union { d_val; d_ptr; }`,
`Elf64_Phdr`, `Elf64_Sym`, `Elf64_Rela`, `tls_info_t`, `SelfHeader`, …).

**It defines NO structure for the `DT_OS_MODULE_INFO` payload.** There is no
module-info struct, no field width, no bit layout, no module-id or version
encoding, and no duplicate/ordering note anywhere in the file. The tag is a bare
integer constant.

## 5. Source independence / provenance analysis

- **Single-source.** Only KytyPS5 defines this tag in the admitted set. P5EMU's
  own records are *derivative* of that same source (circular — not independent
  corroboration).
- No second, independent public source was found that defines the payload.
- Classification: **single-source** for the number/name; **no source at all**
  for payload semantics.

A second source was deliberately *not* fabricated. The PS4 emulator *Kyty*
(`InoriRus/Kyty`, MIT) is recorded in P5EMU provenance as the origin of the
`DT_OS_*` namespace; whether it defines `DT_OS_MODULE_INFO` and any payload was
**not verified in this environment** (no confirmed file path/URL), so it is
recorded as **uncertain provenance**, not corroboration.

## 6. PS4 vs PS5 boundary (hard gate)

| Claim | Platform applicability |
|---|---|
| `DT_OS_MODULE_INFO = 0x6100000d` | PS5 emulator uses it; may be PS4-inherited — **unknown** which |
| `DT_OS_MODULE_INFO_1 = 0x61000043` | PS5-era `_1` spelling (PS5 evidence) |
| payload semantics | **unknown platform applicability** — no PS4 or PS5 source defines it |

No PS4 observation is promoted to PS5: there is no PS4 payload evidence to
promote in the first place.

## 7. `DT_OS_MODULE_INFO` claim matrix

| # | Claim | Class | Source | Independence | Conf. |
|---|---|---|---|---|---|
| 1 | numeric tag `0x6100000d` | **A** | KytyPS5 `elf.h` | single-source | high |
| 2 | symbolic name `DT_OS_MODULE_INFO` | **A** | KytyPS5 `elf.h` | single-source | high |
| 3 | `_1` variant number `0x61000043` | **A** | KytyPS5 `elf.h` | single-source | high |
| 4 | dynamic-entry container is generic ELF `d_un` (`d_val`/`d_ptr`) | **A** | ELF gABI | spec | high |
| 5 | MODULE_INFO-specific payload kind (address/size/index/encoded/table) | **UNKNOWN** | — | — | — |
| 6 | module-id encoding | **UNKNOWN** | — | — | — |
| 7 | version encoding | **UNKNOWN** | — | — | — |
| 8 | bit layout | **UNKNOWN** | — | — | — |
| 9 | field widths | **UNKNOWN** | — | — | — |
| 10 | reserved bits | **UNKNOWN** | — | — | — |
| 11 | valid ranges | **UNKNOWN** | — | — | — |
| 12 | duplicate legality | **UNKNOWN** | — | — | — |
| 13 | duplicate-conflict behaviour | **UNKNOWN** (P5EMU policy is synthetic) | — | — | — |
| 14 | ordering requirements | **UNKNOWN** | — | — | — |
| 15 | interaction with other SCE tags | **UNKNOWN** (a `_1` variant exists) | — | — | — |
| 16 | P5EMU runtime consumes it | **D** (does not) | this repo | — | n/a |
| 17 | P5EMU linker consumes it | **D** (does not) | this repo | — | n/a |

## 8. Contradictions

None among sources — because only one source defines the tag, and it defines no
payload. There is nothing to contradict.

## 9. Implementation decision

**Threshold: FAIL.** The Phase-10 evidence threshold requires that a field's
meaning be *explicit* (not inferred from a name), its representation be defined
enough to parse safely, its PS5 applicability be established, its provenance be
traceable, and no material contradiction exist. Fields 5–15 meet **none** of
these. Therefore:

- `DT_OS_MODULE_INFO` remains **`kNumberAndName`** with
  `SceTagPayload::kUnknown` and `semantics_known() == false`.
- **No** payload parsing, module-id, version, bit-layout, duplicate, or ordering
  behaviour is implemented.
- The Phase-9 conflicting-duplicate-tag rule remains a **conservative P5EMU
  synthetic policy**, not PS5 semantics.

The only Phase-10 code change is a **narrow regression test** that strengthens
the UNKNOWN boundary (it fails if a future contributor promotes
`DT_OS_MODULE_INFO` to `kKnownSemantics` or assigns it a payload).

## 10. Evidence required to close each gap

To promote any field 5–15 to `kKnownSemantics`, a contributor must produce:

- an **independent, PS5-specific** public source (spec, official doc, or a
  reproducible observation) that defines the payload structure/encoding; and
- corroboration that is **not derivative** of KytyPS5 (trace the chain); and
- explicit evidence that the behaviour is **PS5**, not merely PS4.

Until then the fields stay UNKNOWN.

## 11. Contributor research tasks (research-only, not implementation-ready)

- Find an **independent PS5-specific** public source for the `DT_OS_MODULE_INFO`
  payload (and confirm or refute the `_1` variant relationship to `0x6100000d`).
- Establish payload field widths / bit layout if any source defines them.
- Establish duplicate legality and ordering.
- Establish PS4-vs-PS5 applicability (is `DT_OS_MODULE_INFO` PS4-inherited?).
- Establish version encoding, **without** attempting NID generation.

These are **research** tasks; they must not be turned into implementation until
the threshold in §9 is met.

## 12. Non-claims

Phase 10 implements no PS5 dynamic-linking semantics, no module-id/version
decoding, no NID generation, and no GPU/CPU/runtime behaviour. It closes the
`DT_OS_MODULE_INFO` gap at its current, honest boundary: **number and name known,
payload UNKNOWN**. See [`ps5-dynamic-linking-phase9.md`](ps5-dynamic-linking-phase9.md).
