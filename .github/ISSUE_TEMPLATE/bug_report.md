---
name: Bug report
about: Report a defect in P5EMU (incorrect behaviour, crash, or failing test)
title: "[bug] "
labels: bug
assignees: ""
---

<!--
Thanks for filing a bug. P5EMU is evidence-driven, so please be as precise as
you can. Do NOT paste Sony firmware, keys, or copyrighted game material here.
-->

## Summary

A clear, one-paragraph description of the defect.

## Expected vs. actual behaviour

- **Expected:**
- **Actual:**

## Reproduction

Steps or a minimal, synthetic input that reproduces it. If you can, reduce it to
a fixture built from P5EMU-owned/synthetic data (see `tests/` for examples) —
this makes fixes and regression tests far easier.

```
# commands, if any
```

## Which component?

- [ ] x86-64 decoder
- [ ] IR / `TranslateToIr` / `ValidateIr`
- [ ] reference interpreter
- [ ] AArch64 backend / emitter / codegen
- [ ] loader (ELF64 / PS5 layer)
- [ ] relocations / imports
- [ ] HLE bridge
- [ ] runtime / multi-module / guest memory
- [ ] tests / fuzzing / tooling
- [ ] documentation
- [ ] other:

## Evidence classification

P5EMU separates documented facts, observed behaviour, inference, and synthetic
models. Which is this bug about?

- [ ] Documented fact (cite the source)
- [ ] Observed behaviour
- [ ] Inference / hypothesis
- [ ] Synthetic P5EMU model
- [ ] Not sure

## Environment

- OS / environment (e.g. WSL2 Ubuntu 24.04):
- Compiler + version (e.g. GCC 15.2.0):
- Commit / tag:
- Sanitizer in use, if any:
- AArch64 harness in use (`P5EMU_A64_HARNESS`)?:

## Logs / output

```
paste relevant output here (redact anything sensitive; no Sony/game material)
```
