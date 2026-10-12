# THIRD_PARTY LICENSES

Each third-party dependency added to P5EMU gets:

1. A directory or file here named `<component>-<version>.<ext>`, containing the
   **verbatim upstream licence text**.
2. An entry in the dependency table below.
3. A `PROVENANCE.md` record naming where it came from and how it is used.
4. An `ACKNOWLEDGEMENTS.md` entry.

---

## Current dependencies

**None.**

Phase 1 has zero third-party dependencies, and configure-time performs zero
network access. The build works air-gapped by design: an auditable dependency
surface is worth more on day one than the convenience of `FetchContent`.

---

## Candidate dependencies (researched, not adopted)

Each was evaluated during Phase 0 research. See
[`../docs/research/licenses.md`](../docs/research/licenses.md) for the full
matrix with upstream URLs and pinned commits.

| Component | SPDX | Phase | Intended use |
|---|---|---|---|
| **SPIRV-Cross** | Apache-2.0 OR MIT | GPU 2 | SPIR-V → MSL for the Metal backend |
| **MoltenVK** | Apache-2.0 | GPU 4 | Optional Vulkan-on-Metal for desktop |
| **VIXL** (`gitlab.arm.com/runtimes/vixl`) | BSD-3-Clause | CPU 2 | AArch64 macro assembler, **or** test oracle |
| **LLVM** | Apache-2.0 WITH LLVM-exception | CPU 2 | Offline AOT compiler only. **Not** a `core/` dependency |
| **iced** | MIT | CPU 1 | Test-only: decoder/encoder cross-check |
| **Zydis** | MIT | CPU 1 | Test-only: disassembly golden tests |
| **tinyemu** | MIT | CPU 1 | Test-only: out-of-process oracle |
| **Play!** `AArch64Assembler` | BSD-2-Clause | CPU 2 | Possible reference for AArch64 encoding |
| **SPIRV-Tools** | Apache-2.0 | GPU 3 | SPIR-V optimisation |

### Rejected

| Component | SPDX | Why rejected |
|---|---|---|
| QEMU TCG | GPL-2.0-or-later | Per-file MIT/BSD requires audit; TB granularity wrong for PS5 |
| PCSX2 | GPL-3.0-or-later | Closed-source HLE; no ARM64 EE backend |
| RPCS3 | GPL-2.0 | GPL; 52 GiB fixed-VA model iOS-incompatible |
| dynarmic | 0BSD | **Upstream archived/404.** Vendoring an unmaintained fork needs an explicit decision |
| Madeira's FEX iOS port | GPL-3.0 | Study, do not copy |
| Keystone | GPL-2.0-only | Copyleft; VIXL covers the same ground |
| FEX's `External/vixl` fork | **UNVERIFIED** | Licence could not be confirmed. Use Arm upstream |
| DXC Metal CodeGen | MIT + Apple's closed tool | Requires Apple's macOS-only Metal Shader Converter |
| Unicorn | GPL-2.0 | GPL. Out-of-process cross-check only |
| TinyEMU BIOS blobs | — | Not redistributable |

---

## Rule

**Attribution is necessary and never sufficient.** A licence obligation cannot be
discharged by writing a credit line. See
[`../docs/research/licenses.md`](../docs/research/licenses.md) §6 for why P5EMU
is independently authored (Apache-2.0) and vendors no copyleft source.
