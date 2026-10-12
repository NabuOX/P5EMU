# HLE and the Guest-to-Host Call Bridge (Phase 6)

Phase 6 adds the first HLE architecture: guest imports resolved to host
implementations, reached by a real guest `CALL`, with the guest resuming after.
It implements **no Sony library or function** — every identity here is
P5EMU-owned and synthetic.

```
ImportRequirement
  -> HleResolver            (string identity -> registry)
  -> HleRegistry entry      (module + symbol + implementation)
  -> HleBinding             (a deterministic GUEST trampoline address)
  -> guest CALL reaches the trampoline (kExternalCall in the IR)
  -> HleDispatcher -> HleFunction(HleCallContext)
  -> guest return value / state -> guest resumes
```

The HLE subsystem is separate from the loader, the relocation parser, the CPU
decoder, the IR, and the backend. The CPU core knows only that control left
guest code for a binding **address**.

## 1. HLE identity

`HleRegistry::Register(module, SymbolIdentity, HleFunction)`. Symbol identity is
the Phase-5 typed identity (`StringName`/`NumericId`/`OpaqueId`/`Unknown`).
Synthetic registrations use `libp5emu_test` / `p5emu_add` / `p5emu_sum6` /
`p5emu_memory` / `p5emu_sub`. No Sony NID or name is invented; the HLE resolver
reports `kUnsupportedIdentity` for anything but a string name it can match.

## 2. Guest ABI

The adapter is explicit and separate from the HLE functions.

- Integer arguments: `RDI, RSI, RDX, RCX, R8, R9` (AMD64 System V order).
- Return value: `RAX`.
- The `CALL` pushes the return address; the HLE binding returns and the pushed
  address is popped, so RSP is balanced by the call/return pair.
- HLE functions must not adjust the guest stack; the pop is performed by the
  interpreter/backend, uniformly for every binding.

The ABI lives in `HleCallContext`; a future PS5 ABI correction changes the
context, not the functions.

## 3. HleCallContext

The narrow interface an HLE function receives:

```
Arg(index)                 // guest ABI argument register
SetReturn(value)           // writes RAX
ReadGuest / WriteGuest     // checked GuestMemoryPort access
ReadBytes                  // bounded bulk read
```

It deliberately exposes **no** way to execute guest code.

## 4. Guest-to-host dispatch

A binding is a **guest trampoline address** in a P5EMU-owned synthetic range
(`0x70000000`, stride 8). Guest-visible binding ≠ raw host pointer: the
dispatcher maps the address to a registered implementation. The trampoline range
is deterministic, overflow-checked, collision-free by construction, and kept
clear of loaded segments.

> TEST/INTERNAL P5EMU RANGE — NOT a PS5 address and NOT a PS5 ABI.

## 5. CPU call/return

A `CALL` whose target is outside the translated code buffer becomes an
`kExternalCall` terminator (`external_target` = binding address, `fallthrough` =
return block). The reference interpreter dispatches it and resumes in the return
block; the AArch64 code generator calls the harness HLE helper and resumes. The
terminator flows through decoder -> IR -> validator -> interpreter -> AArch64
differentially like any other IR.

## 6. Synthetic HLE functions

- `p5emu_add(a,b) -> a+b`
- `p5emu_sub(a,b) -> a-b` (non-commutative; proves argument order)
- `p5emu_sum6(a..f) -> sum` (exercises all six argument registers)
- `p5emu_memory(ptr, add, out) -> *out = *ptr + add` (checked guest memory)

These are architecture tests, not product APIs. The out-of-process AArch64
harness mirrors the same functions so both backends observe identical
guest-visible behaviour.

## 7. Memory safety

All HLE guest access goes through `GuestMemoryPort`. Failures (unmapped,
out-of-bounds, overflowed pointer+length) become `kHleMemoryFault`, never host
memory corruption. Zero-length reads are defined no-ops; bulk reads are bounded
so an attacker-controlled length cannot force a large allocation.

## 8. Resolver integration and unresolved behaviour

`HleResolver` bridges `ImportResolver` to the registry: an import resolves to the
binding's trampoline **address**. A required import with no unique binding leaves
the image **not execution-ready** (`kUnresolvedImport`/`kAmbiguousImport`/
`kUnsupportedSymbolIdentity`); the loader never writes address zero.

## 9. Duplicate / ambiguous registration

Registering the same `(module, symbol)` twice is not last-wins: lookup returns
`kAmbiguous`. A stale or unknown trampoline address returns `kNotFound`.

## 10. Error model

`kHleNotFound`, `kHleAmbiguous`, `kHleUnsupportedIdentity`, `kHleInvalidArgument`,
`kHleMemoryFault`, `kHleDispatchFault`, `kHleInternalError` — part of the single
`Status` model, mapped identically on the host and the harness so the
differential comparison agrees. No host exception crosses the guest boundary.

## 11. Reentrancy rule

An HLE function may read/write guest memory and return, but may **not** execute
guest code. `HleCallContext` has no such operation, so reentrancy is
structurally impossible; the dispatcher also rejects a nested dispatch with
`kHleDispatchFault`.

## 12. Registry / binding lifetime

The dispatcher borrows a `const HleRegistry*`; the caller must keep the registry
alive for the dispatcher's lifetime. Long-lived guest structures hold a guest
trampoline **address**, never a host pointer, so a registry change cannot leave a
dangling host pointer in guest-visible state.

## 13. Transactional binding

HLE binding participates in the Phase-5 transactional apply: trampoline
addresses are written during staging, and the target is replaced only on full
success. A failure mid-binding discards the staging image; there is no
half-written trampoline table and the target is unchanged.

## 14. Call trace

`HleDispatcher::EnableTrace` records, per dispatch: binding address, module,
symbol identity, status, and the guest return value. It is off by default and
never changes behaviour. It does not log guest memory contents.

## 15. PS5 boundary

Phase 6 is **fully synthetic**. No real Sony HLE function is implemented; no Sony
NID, module name, or ABI quirk is guessed. The identity model can carry a
`NumericId`/`OpaqueId` when independent PS5 evidence exists, without redesigning
the resolver or dispatcher. See `PROVENANCE.md`.
