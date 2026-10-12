# Runtime, Multi-Module Loading, and Process Startup (Phase 7)

Phase 7 adds the first *runtime* layer: loading more than one module, resolving
cross-module and HLE imports through one deterministic policy, building a real
guest stack and an initial process stack, and transporting a multi-region guest
address space to the out-of-process AArch64 backend.

It implements **no PS5 startup behaviour**. Every layout here is P5EMU-owned and
synthetic. The boundary between what P5EMU defines and what is genuinely unknown
about the PS5 is recorded in `docs/research/ps5-runtime-evidence.md` and
restated in §11.

```
ModuleRepository  (name -> ELF bytes)
  -> DependencyGraph      (dependencies before dependents, once each, cycle/missing checked)
  -> LoadBiasAllocator    (one synthetic bias per module)
  -> per-module: Parse -> Plan -> address-space regions
  -> ResolveIdentity      (guest export first, then HLE; one policy, ambiguity fails)
  -> transactional apply  (segments + relocations into a staging port)
  -> TranslateProgram / MergePrograms / LinkGuestCalls
  -> LoadedProcess        (ready only if every stage succeeded)
```

The runtime is separate from the loader, the relocation parser, the import
resolver, the HLE registry, the IR, and the backends. It composes them; it does
not duplicate them.

## 1. Address space

`AddressSpace` holds named `AddressRegion`s (`base`, `size`, `permissions`,
`owner_module`, `kind`) with a **no-overlap** policy: adding a region that
overlaps an existing one fails with `kOverlappingMapping`. A process adds the
module segments (per-segment permissions from the `LoadPlan`), the stack region
(R/W, non-executable), and the HLE trampoline region (reserved; carries no
backing bytes). Regions are the unit of transport to the backend.

## 2. Guest stack

`GuestStack` is a read/write, non-executable region with a checked top. `Push64`
and `Pop64` fail on under/overflow and never wrap. `BuildInitialStack` lays out
an initial process stack in the SysV style (argc, argv pointers, NULL, envp
pointers, NULL, auxv pairs, NULL) with 16-byte alignment at the final RSP and
checked writes. The **exact** PS5 stack contents (vector registers area, loader
metadata, auxv key set, TV/thread pointers) are unknown; P5EMU defines a generic
GNU/Linux-style shape and says so.

## 3. Module lifecycle

`ModuleState` is a one-way lattice:

```
Found -> Parsed -> Planned -> Mapped -> Relocated -> Resolved -> Ready
```

`IsValidModuleTransition` rejects skips and regressions, so a module cannot be
reported ready without every prior stage having run.

## 4. Dependency graph

`BuildDependencyGraph(root, deps)` produces a deterministic order in which a
module always appears **after** every module it depends on, each module exactly
once. Missing dependencies fail (`kMissingDependency`); cycles fail
(`kDependencyCycle`). `GraphOptions::inject_reverse_order` perturbs the order so
the negative test can prove the invariant is actually checked.

## 5. Load bias

`LoadBiasAllocator` hands out biases from a synthetic layout
(`kBase = 0x20000000`, `kEnd = 0x70000000`, `kStride = 0x10000000`; five slots).
This is **not** PS5 ASLR — P5EMU does not model Sony's address policy. The
allocator returns distinct, increasing biases and fails once the space is
exhausted. `SetInjectOverlap` (test-only) returns duplicate biases so the
negative test can prove callers detect collisions.

## 6. Identity resolution

`ResolveIdentity(identity, repos, hle)` applies one policy: a module's **own
exported symbols win**, and only if no guest export matches does the HLE
registry get a chance. Resolution is by the Phase-5 typed `SymbolIdentity`.
Several matches is `kAmbiguousImport`; none is `kUnresolvedImport`. The policy is
explicit and testable rather than implicit in call order.

## 7. Merge and link

`TranslateProgram` lowers each module's executable segment to IR independently.
`MergePrograms` concatenates them into one program, remapping block and value
ids, and marks the root module's entry. `LinkGuestCalls` then rewrites every
`kExternalCall` whose target is a *guest* block (a resolved cross-module symbol)
into an internal jump, so cross-module calls are intra-program control flow while
HLE bindings remain external calls. `ValidateIr` runs on the merged program
before it is ever executed.

## 8. Transactionality

`LoadProcess` is atomic. It parses and validates everything, resolves every
import, applies segments and relocations into a **staging** port, and only then
commits state (memory, image, address space, program). Any failure returns the
specific status with `execution_ready == false` and leaves the caller's state
untouched. A failed load is never partly visible.

## 9. Multi-region AArch64 transport (harness protocol v2)

The AArch64 backend runs out-of-process (qemu-aarch64 + the P5EMU harness), so it
must transport the whole guest address space across a process boundary. Protocol
v2 carries a **region table**: each request names every region with its base,
size, permission mask, and bytes; the harness resolves each helper load/store/
fetch to the region that covers the address and enforces that region's
permissions. Responses carry each region's final bytes in order. The transport
rejects overlapping regions, zero/oversized counts, and oversized region bytes,
and preserves permissions exactly. A single-region process degrades to the
original shape; the multi-module proof (`RuntimeMultiModule`) runs the same
program through the interpreter and through AArch64 and requires the same result
and termination reason.

## 10. Tests

- `runtime_test.cpp` — address space, stack, startup stack, graph, bias,
  lifecycle, resolution, merge, the multi-module guest->guest->HLE end-to-end
  (interpreter **and** AArch64), stack-argument HLE, negative proofs, readiness.
- `runtime_fuzz_test.cpp` — `RuntimeFuzz`: a 6000-case deterministic multi-module
  load campaign (mutated images, corrupted graphs) asserting safety and
  determinism (a failed load is never ready). `RuntimeProperty`: 2000 cases over
  graph order and load-bias invariants.

## 11. Evidence boundary (non-claims)

P5EMU does **not** claim to reproduce the PS5. Specifically, the following remain
UNKNOWN here and are modelled generically, not copied: the initial stack/RSP
layout, argc/argv/envp/auxv contents and key set, TLS/thread-pointer setup,
module-init ordering and constructors, address-space layout and ASLR, and the
loader metadata passed to a title. See `docs/research/ps5-runtime-evidence.md`
for the per-item audit.

## 12. Runtime profile and evidence boundary (Phase 8)

Phase 8 adds an explicit **runtime profile** layer (`runtime::RuntimeProfile`)
that stops the generic Phase-7 model from masquerading as PS5 behaviour:

- Every runtime fact carries an **evidence class** (Documented / Observed /
  Inferred / Synthetic) and every startup field is **Known / Unknown /
  Unsupported**.
- `RuntimeProfileKind` separates `kGenericSynthetic` (the Phase-7 SysV model,
  all fields Known and classed Synthetic) from `kPs5`.
- `StartupState` structurally represents entry RIP, RSP/RBP, thread pointer,
  argc/argv/envp/auxv, and stack alignment, plus a runtime profile and the
  generic concrete image.
- `MaterializeStartup` succeeds **only** when every required field is Known.
  A PS5 profile (entry Known/Documented, the rest Unknown) is **refused** with
  `kUnknownRuntimeField` and a blocker list — values are never fabricated.
- `CheckThreadPointerSupport` fails explicitly (`kRuntimeProfileUnsupported`)
  when the thread pointer is not evidenced: there is no fake thread pointer.
- `Ps5LoadBiasEvidence()` records that PS5 load-address policy is UNKNOWN, so the
  synthetic `LoadBiasAllocator` is never relabelled as PS5 ASLR.
- `DescribeModuleMetadata`/`ModuleRole`/`InitOrderModel` describe only what
  evidence supports: dependency names and Sony tag *numbers* are facts, while
  init/fini ordering, role for a generic ELF, and Sony symbol semantics are
  UNKNOWN and preserved, not interpreted.

The per-fact ledger is `docs/research/ps5-runtime-phase8.md`. This changes no
emulator behaviour: it makes the *absence* of PS5 evidence explicit and
machine-checkable.
