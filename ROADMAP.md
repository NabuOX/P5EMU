# P5EMU Roadmap (contributor-oriented)

This is a **contributor map**, not a release plan. It lists real work areas with
concrete, pick-up-able tasks and honest difficulty tags. It does **not** define
the next implementation phase — formal phase planning is a separate gate — and it
invents no completed features.

Legend for effort/entry:

- 🟢 **First contribution** — well-scoped, no deep emulator background needed.
- 🟡 **Intermediate** — one subsystem, some architecture context required.
- 🔴 **Deep** — emulator/reverse-engineering depth; expect design discussion.

Everything below must respect the evidence and provenance standards in
[`CONTRIBUTING.md`](CONTRIBUTING.md). "Unknown PS5 behaviour" stays UNKNOWN until
evidence supports it.

---

## CPU (x86-64 semantics)

- 🟢 Add decoder/round-trip test vectors for encodings already supported.
- 🟢 Add interpreter tests for flag edge cases (carry/borrow/overflow borders).
- 🟡 Extend width/partial-register coverage to more ALU forms.
- 🟡 Expand shift/rotate semantics and their flags against the ISA docs.
- 🔴 Broaden the decoded instruction subset while keeping "reject, never guess".

## ARM64 backend

- 🟢 Add randomized differential cases that stress existing IR shapes.
- 🟡 Improve `Arm64CodeGen` for value/flag shapes currently covered by helpers.
- 🟡 Extend the multi-region harness transport tests (bounds, permissions).
- 🔴 Design a register allocator (today, values live in stack slots).
- 🔴 Native (in-process) runner feasibility on ARM64 hosts.

## Loader / runtime

- 🟢 Add malformed-ELF fixtures and fuzz cases.
- 🟡 More relocation coverage and negative proofs.
- 🟡 Strengthen multi-module ordering/resolution property tests.
- 🔴 Section-permission and mapping models beyond the current subset.

## HLE

- 🟢 More synthetic providers + differential coverage (interpreter vs AArch64).
- 🟡 Sharpen the guest ABI adapter and error model.
- 🔴 HLE surface *architecture* (evidence-first) — not a Sony implementation.

## GPU (research → implementation)

- 🟡 Consolidate RDNA 2 / shader-translation research notes with citations.
- 🔴 Shader IR and translation design (Metal as the primary target).
- 🔴 SPIR-V/Metal pipeline scaffolding (component decisions recorded first).

## Apple / iOS

- 🟡 Keep the JIT/W^X constraint audit current (`docs/research/ios-jit-audit.md`).
- 🔴 Resolve the `pthread_jit_write_protect_np` question under a debugger.
- 🔴 Xcode-free Apple build scaffolding (no iOS app is claimed today).

## Testing / fuzzing

- 🟢 Add cases to existing fuzz/property suites.
- 🟡 New mutation campaigns for parser-facing components.
- 🟡 Reduce flaky/nondeterministic surfaces; strengthen determinism checks.

## Documentation / research

- 🟢 Onboarding and glossary improvements.
- 🟡 Evidence audits for subsystems currently tagged UNKNOWN.
- 🟡 Keep `ps5-subsystems.md` / `ps5-runtime-evidence.md` tag taxonomy consistent.

---

## Good first issues (how to start)

1. Pick a 🟢 item in a subsystem you find interesting.
2. Read the matching architecture doc under [`docs/architecture/`](docs/architecture/).
3. Build and run the tests (`docs/development.md`), then add a focused change.
4. Open a PR using the template — describe the evidence and provenance clearly.

If you are unsure where to start, open a discussion or a feature request; we
would rather help you find a good fit than have you guess.
