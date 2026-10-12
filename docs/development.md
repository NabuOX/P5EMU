# Development Guide

How to build, test, and verify P5EMU. For contribution rules, see
[`../CONTRIBUTING.md`](../CONTRIBUTING.md).

> **Platform support today:** P5EMU's core is developed and verified on
> **Linux** and on **Windows via WSL2/Ubuntu**. **macOS and iOS build support
> does not exist yet** and is not claimed. `apps/ios/` is a placeholder only.

---

## 1. Supported environment

| Item | Supported |
|---|---|
| OS | WSL2 + Ubuntu (primary, as used for all verification), or native Linux |
| Windows role | Host for the editor and for WSL2; the build runs **inside WSL2**, not on the Windows filesystem toolchain |
| Compilers verified | GCC 15.2.0 and Clang 21.1.8 |
| Build system | CMake ≥ 3.20 + Ninja |
| Language standard | C++20 / C20 |
| Dependencies | **None.** Configure performs no network access (air-gap friendly) |

The project's own verification is run under warnings-as-errors, plus
AddressSanitizer and UndefinedBehaviorSanitizer.

### AArch64 test toolchain (only for the AArch64 suites)

The AArch64 backend executes emitted code through an **independent** AArch64
environment, used strictly as test infrastructure:

- `aarch64-linux-gnu-gcc` (GCC/binutils) — cross-compiles the harness;
- `qemu-aarch64` (qemu-user) — runs the harness.

Both are external tools; they are not linked into `core/` and are not vendored
(see [`../PROVENANCE.md`](../PROVENANCE.md)). If the cross compiler is not found,
the tests still build, and the AArch64 suites report a clear coverage gap rather
than a fake result.

---

## 2. Windows + WSL2 workflow

P5EMU is developed on Windows with WSL2. The recommended layout is to keep the
repository inside WSL2 for best build performance (e.g. under the Linux
filesystem), and edit from Windows if you prefer. Whether the tree lives under
`/mnt/c/...` (Windows drive, as in this repository) or under the WSL2 Linux home
directory, the build commands are the same — run them inside WSL2:

```bash
wsl -d Ubuntu
cd "/path/to/P5EMU"
```

---

## 3. Build

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DP5EMU_WERROR=ON \
  -DP5EMU_BUILD_TESTS=ON \
  -DP5EMU_BUILD_CLI=ON
cmake --build build --parallel
```

Useful options:

| Option | Default | Meaning |
|---|---|---|
| `-DCMAKE_BUILD_TYPE` | `RelWithDebInfo` | build configuration |
| `-DP5EMU_WERROR` | `OFF` | treat warnings as errors (the project uses `ON`) |
| `-DP5EMU_BUILD_TESTS` | `ON` | build the test binary |
| `-DP5EMU_BUILD_CLI` | `ON` | build the CLI (`p5emu-cli`) |
| `-DP5EMU_SANITIZER` | `none` | `none` \| `address` \| `undefined` \| `thread` |

Build outputs land in `build/bin/` (executables) and `build/lib/`.

---

## 4. Run the tests

The test binary holds every suite; each CTest entry runs one suite by name.

```bash
./build/bin/p5emu_tests            # all tests
./build/bin/p5emu_tests Runtime    # only suites whose name contains "Runtime"
ctest --test-dir build --output-on-failure
```

Expected baseline at the Phase 0–7 checkpoint: **268 tests, 69 CTest suites,
0 failures.**

---

## 5. Sanitizers

Configure a separate build directory per sanitizer:

```bash
cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DP5EMU_SANITIZER=address -DP5EMU_WERROR=ON
cmake --build build-asan --parallel
ctest --test-dir build-asan --output-on-failure
```

`undefined` and (where relevant) `thread` work the same way. The convenience
script runs the common combinations:

```bash
scripts/verify.sh              # configure + build + ctest (warnings as errors)
scripts/verify.sh --asan       # same under AddressSanitizer
scripts/verify.sh --ubsan      # same under UndefinedBehaviorSanitizer
scripts/verify.sh --release    # optimised
```

---

## 6. Provenance check

Before opening a PR, run the provenance gate:

```bash
scripts/check_provenance.sh
```

It mechanically checks for: no vendored `third_party/` tree, no copyleft SPDX
tags in source, SPDX headers present on source files, no large binary blobs, and
a non-empty `PROVENANCE.md`. It does **not** replace human review of provenance
(see [`../PROVENANCE.md`](../PROVENANCE.md) and
[`research/licenses.md`](research/licenses.md)).

---

## 7. AArch64 / qemu suites

When `aarch64-linux-gnu-gcc` is found at configure time, CMake cross-compiles the
harness automatically and defines its path as the default. You can also point
the tests at a harness explicitly:

```bash
export P5EMU_A64_HARNESS="/abs/path/to/p5emu_a64_harness"
export P5EMU_A64_QEMU="qemu-aarch64"   # optional; this is the default
```

The harness is built by `scripts/build_aarch64_harness.sh`. See
[`../aarch64/harness/`](../aarch64/harness/) and
[`architecture/arm64-backend.md`](architecture/arm64-backend.md).

---

## 8. Where the documentation lives

| Area | Document |
|---|---|
| Component boundaries / layers | [`architecture/v1.md`](architecture/v1.md) |
| AArch64 backend | [`architecture/arm64-backend.md`](architecture/arm64-backend.md) |
| Loader | [`architecture/loader.md`](architecture/loader.md) |
| Relocations / imports | [`architecture/relocations.md`](architecture/relocations.md) |
| HLE bridge | [`architecture/hle.md`](architecture/hle.md) |
| Runtime / multi-module | [`architecture/runtime.md`](architecture/runtime.md) |
| Licence matrix / upstream research | [`research/licenses.md`](research/licenses.md) |
| PS5 subsystem tags | [`research/ps5-subsystems.md`](research/ps5-subsystems.md) |
| PS5 runtime evidence | [`research/ps5-runtime-evidence.md`](research/ps5-runtime-evidence.md) |
| Testing philosophy | [`research/testing-philosophy.md`](research/testing-philosophy.md) |
| iOS/JIT constraints | [`research/ios-jit-audit.md`](research/ios-jit-audit.md) |

---

## 9. Common tasks

- **Add a test file:** create `tests/<name>_test.cpp`, then register it in
  `tests/CMakeLists.txt` (`P5EMU_TEST_SOURCES` + an `add_test` entry). An
  unregistered `*_test.cpp` is a **configure error** on purpose.
- **CLI smoke test:** `./build/bin/p5emu-cli`.
- **Disassembly of emitted AArch64 (developer aid):** `scripts/arm64_disasm.sh`.
