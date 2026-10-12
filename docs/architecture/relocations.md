# Relocations and Import Resolution (Phase 5)

Phase 5 extends the loader with generic x86-64 ELF relocation infrastructure and
the canonical import/resolution contract a future PS5 HLE layer will bind
against. It does not implement HLE.

```
ELF64 bytes
  → BinaryReader → Elf64 → Ps5Executable        (Phase 4 parse)
  → BuildLoadPlan (relocations + symbols + import requirements)
  → ResolveImports   (resolver: resolved/unresolved/unsupported/ambiguous)
  → ApplyLoadPlan    (transactional: map, copy, BSS, relocate)
  → MappedGuestMemory (R/W/X preserved and enforced)
  → CpuState{RIP = entry + bias}
  → CPU fetch → decode → IR → interpreter / AArch64
```

## 1. Relocation representation

`Relocation` holds only structural facts plus a load-bias-resolved target:

```
target_guest_va = ELF r_offset + load bias
type            = RelocType (numeric R_X86_64_* mapping)
symbol_index    = dynsym index (0 = null)
addend          = RELA r_addend (bit pattern)
width           = bytes written (8 for the supported subset)
```

Parsing (`ParseDynamicRelocations`) is separate from application
(`ComputeRelocationValue` + the apply loop). A parsed relocation never contains
a resolver pointer or any HLE meaning.

## 2. Supported types and formulas

System V AMD64 psABI (PROVEN_FROM_SOURCE). B = load bias, S = symbol value,
A = addend.

| Number | Name | Formula | Width |
|---|---|---|---|
| 0 | `R_X86_64_NONE` | (none) | — |
| 1 | `R_X86_64_64` | S + A | 8 |
| 6 | `R_X86_64_GLOB_DAT` | S | 8 |
| 7 | `R_X86_64_JUMP_SLOT` | S | 8 |
| 8 | `R_X86_64_RELATIVE` | B + A | 8 |

Every add is overflow-checked (`kRelocationOverflow`). Any other type becomes
`kUnsupportedRelocation` and fails the plan explicitly — a relocation required
for execution is never silently ignored.

## 3. RELA only

x86-64 uses RELA (explicit addend). Phase 5 supports RELA only: `DT_RELA` +
`DT_RELAENT` (must be 24) and `DT_JMPREL`/`DT_PLTRELSZ` with `DT_PLTREL ==
DT_RELA`. A `DT_PLTREL` naming REL (17) is rejected with `kUnsupportedRelocation`
rather than being misread as RELA. Entry size, table size divisibility, table VA
range, and entry count are all validated (bounded by `kMaxRelocations`).

## 4. Load-bias model

`final_guest_va = elf_va + bias`. It is explicit and singular:

- Segments map at `p_vaddr + bias`; the entry is `e_entry + bias`.
- `RELATIVE` writes `bias + addend`.
- A defined symbol's value is `st_value + bias`.

Bias 0 reproduces Phase-4 behaviour. Synthetic `ET_DYN`-style fixtures are tested
at several bases and the results change exactly according to the formula. No PS5
ASLR behaviour is assumed; this is generic loader machinery.

## 5. Import requirement model

`ImportRequirement` is P5EMU-owned and independent of any Sony layout:

```
module          library/module identity (may be empty for generic ELF)
symbol          typed SymbolIdentity: StringName | NumericId | OpaqueId | Unknown
symbol_index    generic dynsym index
binding_sites   target VAs + reloc type + addend where the value is written
required        true when it has binding sites (gates execution readiness)
```

Symbol identity is typed so an unknown identity is representable. Phase 5 does
**not** invent NIDs or reverse-engineer Sony symbol tables; synthetic tests use
string identities. PS5 evidence can populate `NumericId`/`OpaqueId` later without
changing the resolver.

## 6. Resolver lifecycle

`ImportResolver::ResolveSymbol(module, symbol)` returns one of:
`kResolved` (with an address), `kUnresolved`, `kUnsupportedIdentity`,
`kAmbiguous`, `kError` — never a single generic failure. `RegistryResolver` is a
concrete registry-backed implementation used by tests and a future HLE layer.

`ResolveImports(plan, resolver, &binding)` calls the resolver for every
requirement and sets `binding.execution_ready`. A requirement that is `required`
(has binding sites) and not `kResolved` makes the image **not execution-ready**,
with a status naming the reason (`kUnresolvedImport`, `kAmbiguousImport`,
`kUnsupportedSymbolIdentity`, ...).

## 7. Unresolved import behaviour

An unresolved required import must never become address zero. `ApplyLoadPlan`
refuses to apply when the binding is not execution-ready, returning the
specific status. Declared dependencies with no binding sites (a bare
`DT_NEEDED`) are recorded but do not gate readiness.

## 8. Transactional relocation

Relocations participate in the same staging model as Phase 4: the plan is
applied into a fresh `MappedGuestMemory`, relocations are written to the staging
image, and the target is replaced only on full success. A failure mid-way
(regions, copies, BSS, or relocation N) discards the staging port; previous
relocations are never committed and no half-patched memory is visible.
`ApplyOptions::fail_after_operations` injects a mid-apply failure to prove it.

## 9. Permissions during relocation

Relocations are applied through the loader-only `WriteImage` (which bypasses
final protections) while staging. The region permissions come from the plan and
are never changed, so final guest R/W/X equals the plan's intent. A relocation
that patches a final R-X segment succeeds during staging; afterwards a guest
write to that region is rejected while instruction fetch remains allowed.

## 10. PS5 boundary

Generic ELF and relocation semantics are PROVEN_FROM_SOURCE (psABI). PS5-specific
dynamic tags remain DOCUMENTED_EXTERNALLY, and the `_1` tag semantics and the
Sony symbol/import structure layout remain **UNKNOWN** — the loader records that
an image declares PS5 imports/exports but does not decode the Sony symbol table.
All Sony-specific facts and their classification are in `PROVENANCE.md`.

## 11. Error taxonomy

`kMalformedRelocationTable`, `kUnsupportedRelocation`, `kRelocationOverflow`,
`kRelocationTargetInvalid`, `kSymbolIndexInvalid`, `kUnresolvedImport`,
`kAmbiguousImport`, `kUnsupportedSymbolIdentity` — added to the single `Status`
model, not a parallel error universe.
