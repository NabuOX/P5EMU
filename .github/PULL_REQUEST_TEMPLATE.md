# Pull request

<!--
Keep changes focused, evidence-driven, and provenance-clean.
Do NOT include Sony firmware, keys, sysmodules, SDK code, or copyrighted game
material. See CONTRIBUTING.md and PROVENANCE.md.
-->

## What this changes

A concise description of the change and the subsystem(s) it touches.

## Why

The problem it solves or the capability it adds.

## Evidence

Every non-obvious claim must carry a source. Classify the evidence:

- [ ] Documented fact (cite source):
- [ ] Observed behaviour (how observed):
- [ ] Inference / hypothesis (reasoning):
- [ ] Synthetic P5EMU model (state that it is not claimed to be PS5 behaviour):
- [ ] No behavioural claim (e.g. docs/tooling only)

If any PS5 behaviour is knowingly **UNKNOWN**, state that here rather than
guessing.

## Tests

- [ ] Added/updated tests for the change
- [ ] No test was weakened, skipped, or deleted to obtain a green run
- [ ] `scripts/check_provenance.sh` passes
- [ ] `p5emu_tests` passes (report the count)
- [ ] `ctest` passes (report the suite count)

## Provenance / licensing

- [ ] This change contains no third-party code, OR
- [ ] Third-party reuse is described below and recorded in `PROVENANCE.md` /
      `THIRD_PARTY_LICENSES/` (project, URL, licence, what was reused,
      obligations)

AI-assisted? If yes, confirm a human reviewed every line:

- [ ] Human review completed and recorded here

## Checklist

- [ ] Scope is focused (one coherent change)
- [ ] Code matches surrounding style; builds with warnings-as-errors
- [ ] New source files carry an `SPDX-License-Identifier` header consistent with
      the repo's current policy
- [ ] No change to `apps/ios/` that implies iOS support that does not exist
