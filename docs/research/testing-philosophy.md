# Testing Philosophy

Tests are designed from day one because P5EMU's entire correctness argument is
differential: *the reference interpreter defines right, and the ARM64 backend
must agree with it.* Without a testing strategy, that argument is an assertion
rather than a proof.

---

## 1. The central idea

> **The emulator contains a full interpreter for exactly one reason: so that the
> JIT can be differentially tested against it.**

PPSSPP keeps `Core/MIPS/IR/IRInterpreter.cpp` and drives it with a `--compare`
flag across interpreter / IR-JIT / non-IR-JIT backends, scaling per-backend
timeouts `{'interpreter': 20, 'ir': 10, 'jit': 5, 'jit-ir': 5}` and maintaining
an explicit `known_failures` table for architectures reachable *only through
emulation*.

Play! gives every instruction test two implementations — `Run()` (native C++)
and `Compile(Jitter&)` (emitted JIT) — and runs the suite in CI on every host
backend, including WASM.

Neither pattern is copied here. Both are the model.

---

## 2. Test layers

| Layer | Question | Backend needed | Where |
|---|---|---|---|
| **L1 Unit** | Does this function do what it says? | Any | `tests/*_test.cpp` |
| **L2 Contract** | Are the architectural invariants intact? | Any | `tests/cpu_contracts_test.cpp` |
| **L3 Decoder** | Does this byte sequence decode correctly? | Any | `tests/cpu/` |
| **L4 IR** | Is the IR well-formed and does the pass pipeline preserve semantics? | Any | `tests/cpu/` |
| **L5 Differential** | Does every backend agree with the reference? | ≥2 backends | `tests/cpu/differential/` |
| **L6 Integration** | Does a guest program run end to end? | Any | `tests/loader/`, later |
| **L7 Hardware conformance** | Does P5EMU match real PS5 behaviour? | Hardware | Not Phase 0 |
| **L8 Regression** | Did a fix stay fixed? | Any | Per-fix |

---

## 3. Non-negotiable rules

### 3.1 Expected values are authored, never captured

**Never write a test whose expected value was produced by running P5EMU.** That
proves determinism, not correctness.

Every expected value derives from:
- the Intel SDM or AMD64 APM, cited by section, **or**
- a documented hardware measurement, **or**
- an explicit derivation in the test's README

`tests/cpu/cases/**/case.yaml` carries a required `provenance` field.

### 3.2 A harness must be observed to fail

A differential harness that has never caught anything is indistinguishable from
one that cannot catch anything. Phase 1 therefore ships a **deliberately
buggy backend** (fails to set `PF` on `add`) and a test asserting the harness
rejects it on exactly the expected cases. See
[`phase1-design.md`](phase1-design.md) §9.1.

This is unusual enough to need justifying: the test is not about the bug, it is
about the instrument.

### 3.3 An unimplemented path is never a pass

A backend that cannot execute an instruction returns
`kUnimplementedInstruction`. The harness records it as a **coverage gap**,
reported separately from correctness:

```
CPU coverage: 18/20 implemented by reference-interpreter
Gaps: setcc, cmovcc (both planned Phase 2)
```

Silent skips are how emulator projects accumulate unverified behaviour.

### 3.4 State comparison is field-by-field

`memcmp` over `CpuState` would happily pass with a wrong `eflags`. Every
architectural field is compared individually, and failures name the field, both
values, and the byte offset.

### 3.5 Memory comparison is bounded **and** guarded

Compare only declared ranges — *and* assert nothing outside them was written. A
JIT bug that writes out of bounds must be caught, not accidentally ignored by
comparing only the interesting region.

### 3.6 Coverage regression is a failure

If a backend's instruction coverage decreases, the build fails. This is
deliberately annoying: coverage only ever moves backwards through accident.

### 3.7 Every compatibility fix becomes a regression test

Where practical. Where genuinely not practical, the reason is recorded in the
fix. A fix without a test and without a recorded reason is a hack by another
name.

### 3.8 Sanitizers are part of the definition of done

Every PR runs clean under ASan and UBSan. TSan for anything touching threads.

---

## 4. Test corpora — licence hygiene

**Test DATA licences and test CODE licences are different things.** Inheriting a
corpus's provenance is not automatic.

| Corpus | Licence | Verdict |
|---|---|---|
| `iced` | MIT | ✅ Decoder **and encoder** — enables decode→encode round-trip that Zydis alone cannot provide |
| `Zydis` | MIT | ✅ Decoder/formatter. No encoder |
| `tinyemu` | MIT | ✅ Out-of-process whole-machine oracle |
| `v86` | BSD-2-Clause | ✅ Core only. **Do not redistribute its BIOS blobs** |
| Play!'s codegen tests | BSD-2-Clause | ✅ Design template |
| KVM selftests | GPL-2.0-only | ❌ Richest source of authoritative assertions in existence. **Read, re-implement from the spec, never vendor** |
| QEMU `tests/tcg/` | GPL-2.0-or-later | ❌ Re-implement the harness; regenerate your own guest binaries |
| FEX `*-tests-bins` | **No licence file**; derived from GCC testsuite (GPL-3.0-or-later) | ❌ Do not vendor. Regenerate, or run as an external oracle and record *our own* outputs |
| Unicorn | GPL-2.0 | ❌ Out-of-process cross-check only |
| `pspautotests`, `ppsspp-frametest-ci` | **No licence file** | ❌ No licence ≠ permissive |
| Intel SDM / AMD64 APM | Copyrighted books | ❌ Read. Transcribe *behaviour* into independently-written tests |

The lesson from FEX's binary corpus is the important one: a genuinely useful test
corpus can still be unusable because nobody wrote a licence file. That risk is
mitigated by generating our own binaries from the GCC testsuite under our own
terms rather than vendoring someone else's.

---

## 5. Suite status

Phase 0 established three suites. They still run, and the CPU contract suite was
rewritten for the Phase 1 state layout (see below).

| Suite | What it covers | Status |
|---|---|---|
| `p5emu.core` | Logging levels, sinks, truncation; `Status` defaults and names | ✅ |
| `p5emu.version` | Version identity | ✅ |
| `p5emu.cpu.contracts` | `CpuState` size and field offsets, trivial-copyability, termination-reason and backend-id name stability | ✅ |

Phase 1 added the CPU pipeline suites:

| Suite | What it covers |
|---|---|
| `p5emu.memory.port` | `GuestMemoryPort` contract: bounds, permissions, little-endian access, SMC |
| `p5emu.ir.validate` / `p5emu.ir.build` | IR validation (positive and negative) and construction determinism |
| `p5emu.decoder` / `.roundtrip` / `.malformed` | x86-64 decode, canonical re-encode, malformed input |
| `p5emu.interpreter` / `.flags` / `.controlflow` / `.memory` | reference execution, flag semantics, branches/terminators, load/store |
| `p5emu.differential` / `.gate` | backend agreement, and proof the harness detects a seeded divergence |
| `p5emu.endtoend` | raw bytes → decode → IR → validate → execute |
| `p5emu.provenance` | expected-value provenance enforcement |

**The contract tests keep finding real bugs.** During Phase 0 the `CpuState`
layout test failed twice before the offsets were derived by measurement. During
Phase 1 the differential gate and the sanitizer builds found a heap overflow in
the interpreter's value store that no functional test had exposed.

Every suite is registered as a CTest entry, and `tests/CMakeLists.txt` fails at
configure time if a `*_test.cpp` file exists without being registered, so a test
cannot silently stop running.

The harness itself (`tests/TestHarness.h`) is a small self-contained
implementation rather than a vendored GoogleTest: vendoring a dependency buys
little and costs auditability. Swapping to GoogleTest would be a
`tests/CMakeLists.txt` change plus deleting one file.

---

## 6. Planned suites

| Suite | Phase | Covers |
|---|---|---|
| `p5emu.memory` | 2 | Protection bits, DMA, fault recovery |
| `p5emu.loader` | 3 | ELF dialect, relocations, dynamic tags |
| `p5emu.gpu.decode` | 3 | RDNA2 instruction decoding, `UNSUPPORTED` coverage |
| `p5emu.gpu.shader` | 3 | Shader IR passes, resource tracking |
| `p5emu.hle` | 4 | Library surface, symbol/version resolution |
| `p5emu.platform.apple` | 4 | `MAP_JIT` code cache, W^X discipline |

---

## 7. Anti-patterns

| Anti-pattern | Why it is banned |
|---|---|
| `memcmp` over `CpuState` | Passes with a wrong `eflags` |
| Recording expectations from P5EMU's own output | Proves determinism, not correctness |
| `if (backend == arm64) return;` in differential tests | A skip that reads as a pass |
| Test with no stated purpose | Nobody dares delete it, nobody dares fix it |
| Fix with no test and no recorded reason | A hack by another name |
| A fake GPU or speculative syscall with a "TODO" | Produces code that must be thrown away and gives false confidence |
| Weakening a test to make it pass | The test was right |
| Comparing only the interesting memory range | Hides out-of-bounds writes |
