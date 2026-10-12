# Contributing to P5EMU

Thanks for your interest. P5EMU is an independent, experimental PlayStation 5
emulator architecture built for portability (x86-64 guest → P5EMU IR → reference
interpreter or AArch64 backend), with Apple platforms as an important long-term
target. It is at an early, architecture-first stage, and careful contributors
are exactly what it needs.

This document explains how to contribute, what we accept, and the evidence and
provenance standards every change must meet.

> P5EMU is open-source software licensed under the **Apache License 2.0** (see
> [`LICENSE`](LICENSE) and [`NOTICE`](NOTICE)). By submitting a contribution you
> agree that it may be distributed under that licence. No copyright assignment
> or CLA is required.

---

## 1. Read these first

- [`CODE_OF_CONDUCT.md`](CODE_OF_CONDUCT.md) — how we treat each other.
- [`docs/development.md`](docs/development.md) — build, test, sanitizers, the
  AArch64/qemu harness, and where the architecture docs live.
- [`PROVENANCE.md`](PROVENANCE.md) — how P5EMU records origin and reuse.
- [`docs/research/licenses.md`](docs/research/licenses.md) — the per-project
  licence matrix that supports the provenance discipline.
- [`ROADMAP.md`](ROADMAP.md) — work areas, including good first contributions.

## 2. Repository layout

```
core/        portable emulator core (decoder, IR, interpreter, loaders, HLE, runtime)
frontend/    user-facing tools (currently a minimal CLI)
aarch64/     the AArch64 execution harness (test infrastructure only)
tests/       the test suites and fixtures
docs/        architecture and research documentation
scripts/     build/test/provenance helper scripts
apps/ios/    placeholder; no iOS app exists yet
```

## 3. Development setup

See [`docs/development.md`](docs/development.md) for the supported environment
(Windows + WSL2 / Ubuntu, GCC or Clang, CMake + Ninja) and the full build, test,
sanitizer and provenance workflow.

The short version:

```bash
scripts/verify.sh              # configure, build, test (warnings as errors)
scripts/verify.sh --asan       # the same under AddressSanitizer
scripts/check_provenance.sh    # the provenance gate
```

## 4. Contribution areas

Contributions are welcome across the project. Representative areas:

- **x86-64 CPU semantics** — decoder coverage, instruction semantics, flag
  behaviour, width rules.
- **ARM64 translation / backend** — code generation, ABI, the emitted subset.
- **Decoder and P5 IR** — IR design, validation, lowering.
- **Loader / runtime** — ELF64, relocations, imports, the process and
  multi-module runtime.
- **HLE** — the guest-to-host call bridge and (later, evidence-permitting) the
  HLE surface architecture.
- **GPU research and implementation** — RDNA 2 / shader translation research.
- **Metal / Apple platform work** — feasibility, W^X/JIT constraints, native
  Metal direction.
- **Testing / fuzzing** — differential campaigns, property tests, malformed
  input campaigns.
- **Performance** — interpreter and generated-code performance.
- **Documentation** — architecture notes, evidence audits, onboarding.
- **Reverse-engineering research** — from lawful, public evidence only (see §6).
- **Build / CI / tooling** — reproducibility, developer ergonomics.

## 5. The evidence standard

P5EMU distinguishes four kinds of claim, and **code and docs must not silently
blend them**:

| Kind | Meaning | How it is marked |
|---|---|---|
| **Documented fact** | Published in a specification or vendor documentation | cite the source (URL/section) |
| **Observed behaviour** | Determined by lawful, reproducible observation/experiment | record how it was observed |
| **Inference / hypothesis** | Our reasoned guess from other evidence | label it as inference |
| **Synthetic P5EMU model** | A P5EMU-owned placeholder that is *not* claimed to be PS5 behaviour | label it synthetic |

**Unknown PS5 behaviour stays explicitly UNKNOWN** until evidence supports it.
Do not fill an unknown with a plausible-looking constant. The existing pattern to
follow is `docs/research/ps5-subsystems.md` and
`docs/research/ps5-runtime-evidence.md`, which tag every claim.

## 6. Provenance rules (mandatory)

Do **not** submit any of the following:

- proprietary Sony SDK code, headers, or tools;
- Sony firmware, system software, sysmodules, or extracted data;
- encryption keys or key material of any kind;
- copyrighted game files, assets, saves, or container dumps;
- leaked material;
- source copied from an incompatible licence;
- code whose origin or licence cannot be established.

If a contribution is derived from or adapts third-party work, you **must**
identify, in the pull request and in `PROVENANCE.md`/`THIRD_PARTY_LICENSES/`
as applicable:

- the source project;
- its upstream URL/identity;
- its licence;
- exactly what was reused or adapted;
- the obligations that follow (notices, attribution, etc.).

**Research inspired by another emulator must distinguish architectural learning
from copied implementation.** Reading a project to understand how a problem is
solved is welcome; copying its source is not. See `PROVENANCE.md` and
`docs/research/licenses.md` for how P5EMU draws that line (it does not claim
"clean-room").

## 7. Code style and testing

- Match the surrounding code. The project uses C++20 and C20, builds with
  warnings-as-errors, and keeps `core/` free of platform/Xcode dependencies.
- Every new source file carries an `SPDX-License-Identifier:` header consistent
  with the repository's current policy.
- Add or extend tests for behaviour changes. Do not weaken, skip, or delete a
  test to get a green run.
- New test files must be registered in `tests/CMakeLists.txt` (an unregistered
  `*_test.cpp` is a configure error by design).
- Run `scripts/check_provenance.sh` and the test suite before opening a PR.

## 8. AI-assisted contributions

AI-generated or AI-assisted code is permitted **only with human review**, and
the review must be recorded in the pull request. The human author is responsible
for correctness, style, licensing, and provenance of every line.

## 9. Pull requests

- Keep changes focused; one coherent change per PR.
- Describe **what** changed, **why**, and the **evidence** that supports it.
- Note any licence/provenance implications explicitly (see §6).
- Expect review to be evidence-driven: "it works on my machine" is not evidence
  for an emulation claim.
- **Do not commit or push to the project's main history without maintainer
  authorisation** while the project is in its pre-publication phase.

## 10. Questions

Open an issue using the appropriate template, or start a discussion. For
security-sensitive matters, follow [`SECURITY.md`](SECURITY.md).
