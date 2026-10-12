---
name: Research / evidence report
about: Record a documented fact, observed behaviour, or inference about PS5 or x86-64/AArch64 behaviour
title: "[research] "
labels: research
assignees: ""
---

<!--
P5EMU keeps its claims honest by labelling their source. Use this template to add
or correct a claim. Do NOT include Sony firmware, keys, sysmodules, SDK code, or
copyrighted game material — describe behaviour, cite lawful public sources, and
keep concrete artefacts out of the repo.
-->

## Claim

The single, falsifiable statement this report is about.

## Classification

- [ ] **Documented fact** — published in a specification or vendor doc (cite it)
- [ ] **Observed behaviour** — measured/reproduced (describe how)
- [ ] **Inference / hypothesis** — reasoned from other evidence (state the reasoning)
- [ ] **Synthetic P5EMU model** — a P5EMU-owned placeholder, *not* claimed to be PS5 behaviour

If the honest answer is **UNKNOWN**, say so explicitly and stop there.

## Sources / how it was observed

- Public source(s) with URLs and, where possible, pinned versions/commits:
- For observed behaviour: the experiment, tool, and steps used.

## What this changes in P5EMU

Which code or doc should be updated, and which existing doc currently tags this
as UNKNOWN or differently (e.g. `docs/research/ps5-subsystems.md`,
`docs/research/ps5-runtime-evidence.md`).

## Provenance

- [ ] No third-party code is reused (facts only)
- [ ] A third-party source was adapted — details provided below, and recorded in
      `PROVENANCE.md` / `THIRD_PARTY_LICENSES/`

## Confidence

- High / medium / low, and what would raise it.
