# GPU Architecture

**Phase 0 research artefact.** No GPU code is written in Phase 0. This document
evaluates the proposed direction and produces a recommendation.

Proposed direction under review:

```
PS5 guest GPU commands
  → P5EMU guest GPU abstraction
  → shader IR
  → SPIR-V / Vulkan
  → MoltenVK
  → Metal
```

**Verdict: the direction is reasonable for the desktop target and wrong as the
iOS primary path.** Keep the guest GPU abstraction and shader IR; reconsider the
SPIR-V/Vulkan/MoltenVK hop for mobile.

---

## 1. Guest GPU: what is actually known

From KytyPS5 (GPL-2.0-only — facts only, no code) at commit
`16a7fe689a642deeeef85a706e7da305eca6fb85`, and shadPS4 at
`3a9f57265d28975658f1ca941daa5c879c7a74bd`:

| Element | Finding | Status |
|---|---|---|
| GPU codename | **"Prospero"** (`Libs::Graphics::Prospero`, `src/graphics/guest_gpu/gpu_defs.h:6`). PS4's equivalent is **"Liverpool"** | PROVEN |
| Command stream | PM4-type-2 packets. Header `0xC0000000 \| ((len-2)&0x3fff)<<16 \| op<<8 \| r<<2` (`pm4.h:11-14`) | PROVEN |
| Opcodes | `IT_NOP=0x10` … `IT_WRITE_CONST_RAM=0x81`; `IT_SET_SH_REG_INDIRECT=0x63`, `IT_MEM_SEMAPHORE=0x39`, `IT_EVENT_WRITE_EOP`, `IT_SURFACE_SYNC=0x43`, `IT_WAIT_REG_MEM=0x3C` | PROVEN |
| Queue topology | 1 graphics + 56 compute queues (7 pipes × 8), compute base `0x20` (`graphicsRun.h:48-53`) | PROVEN |
| Guest driver ABI | `AgcInit`, `AgcCreateShader`, `AgcGetRegisterDefaults2`, `AgcSetShRegIndirectPatchSetAddress`, `AgcUnknownFuseShaderHalves` (`src/libs/agc.h:29-60`) | PROVEN (names) |
| DCB register defaults | `agcRegisterDefaults.inc` — **GPL, cannot be copied. Must be dumped from hardware or public docs.** | PROVEN (exists) |
| Shader ISA | RDNA2. Families `SOP1, SOP2, SOPK, SOPC, SOPP, VOP1, VOP2, VOP3, VOP3P, VOPC, VINTRP, SMEM, MUBUF, MTBUF, FLAT, DS, MIMG, EXP`; ~1000 opcodes; `consteval` perfect-hash table | PROVEN |
| Shader pipeline | decode → CFG → translate → SSA IR + 8 passes → hand-written SPIR-V. **SPIRV-Tools only.** glslang is build-time only (host tiler GLSL) | PROVEN |
| Host graphics API | **Vulkan 1.3 only. No D3D12. No Metal.** | PROVEN |
| Tiling | `guest_gpu/tile.*` + host GLSL tiler `shaders/gpu_tiler_{color,standard,standard_3d,standard64,standard64_3d}` | PROVEN (exists) |
| Shader resource table (SRT) | First-class IR concept, `ShaderIR.h` + `SrtWalker.cpp` | PROVEN |
| Guest page tables | Modelled via `multiLevelPageTable.h`; exact bit layout not public | UNKNOWN |

Lineage, worth recording: shadPS4 credits **yuzu's Hades compiler** as the
blueprint for its shader compiler; KytyPS5 directs contributors to **AMD's RDNA 2
ISA Reference Guide (doc 70648)**. Both facts point the same way — the shader
compiler must be written against the AMD spec, not against another emulator.

---

## 2. The proposed direction, assessed

```
guest commands → guest GPU abstraction → shader IR → SPIR-V → Vulkan → MoltenVK → Metal
```

| Stage | Verdict |
|---|---|
| guest commands → guest GPU abstraction | **Keep.** Necessary regardless of host API. |
| guest GPU abstraction → shader IR | **Keep.** This is the valuable, portable part. |
| shader IR → SPIR-V | **Reasonable**, but SPIR-V is chosen for Vulkan reasons, not for P5EMU's reasons. |
| SPIR-V → Vulkan | **Works on desktop. Adds a large host dependency and a layer of abstraction P5EMU does not otherwise need.** |
| Vulkan → MoltenVK → Metal | **Works, App Store safe — but see §3. Structurally incomplete.** |

The weak link is the Vulkan hop. P5EMU does not need Vulkan's portability; it
needs Metal's efficiency on Apple hardware. Going through Vulkan means paying
MoltenVK's translation cost and accepting MoltenVK's feature gaps, to obtain a
portability P5EMU will never use.

---

## 3. MoltenVK feature gaps on iOS

MoltenVK is **Apache-2.0** and **App Store safe**:

> "MoltenVK does not use any private or undocumented API calls or features, so
> your app will be compatible with all standard distribution channels, including
> Apple's App Store."
> — https://github.com/KhronosGroup/MoltenVK

Note the caveat: `MVK_USE_METAL_PRIVATE_API=1` **disqualifies** the app.

### 3.1 Structurally impossible, not a performance problem

| Capability | Status | Evidence |
|---|---|---|
| **Geometry shaders** | **UNSUPPORTED.** Metal has no geometry shader stage | https://developer.apple.com/metal/Metal-Feature-Set-Tables.pdf · https://github.com/KhronosGroup/MoltenVK/issues/1524 |
| **Tessellation shaders** | **UNSUPPORTED on iOS.** Feature table shows Tessellation only for Mac families | Metal Feature Set Tables |
| **Mesh shaders** | Metal **has** object/mesh shaders (Metal 3, `MTLGPUFamily.apple7`+). **MoltenVK exposes no mesh-shader extension.** Unreachable through MoltenVK | MSL Spec §2.20 · MoltenVK extension list |

Geometry and tessellation shaders are **not slow through MoltenVK; they are
absent**. If PS5 content or a P5EMU shader ever needs a stage Metal lacks,
MoltenVK is a dead end rather than a tuning problem.

### 3.2 Supported, with caveats

| Capability | Status on iOS | Evidence |
|---|---|---|
| Subgroup / SIMD-group ops | `VK_KHR_shader_subgroup_extended_types`, `_rotate`, `VK_EXT_subgroup_size_control` — *"Requires Metal 2.2 and Apple family 4"* (A11+). Ballot/vote — *Apple GPU family 4* | MoltenVK User Guide |
| **Subgroup width** | **Fixed at 32.** Apple simdgroups are 32-wide; `thread_execution_width` deprecated in Metal 3 | MoltenVK `Whats_New.md` |
| `VK_EXT_shader_atomic_float` | Requires **Metal 3.0** (≈ A14+/A15+). Not on older devices | MoltenVK User Guide |
| Buffer atomics (int64) | Supported; *"the full set of 64-bit atomic operations is supported on all platforms starting with Apple9"* | Metal Feature Set Tables |
| Image atomics | Only an internal note (*"always use a texel buffer for atomic storage images"*). **No public limitation statement — UNVERIFIED.** Assume "works, slow" | MoltenVK `Whats_New.md` |
| Descriptor indexing | Supported. Tier 1: **96/128 textures, 16 samplers**. *"the maximum number of unique samplers per app is 96"* on iOS/tvOS. Tier 1 argument buffers **cannot include writable textures** | MoltenVK User Guide · Apple argument-buffer docs |
| Timeline semaphores | Listed supported, but **emulated** on Metal via fences | MoltenVK User Guide |
| Swapchain | **Only 2 or 3 images.** MoltenVK recommends 3 | MoltenVK User Guide |
| Pipeline statistics queries | **Not supported** | MoltenVK User Guide |
| `VkAllocationCallbacks` | **Ignored** | MoltenVK User Guide |
| Compute shaders | Supported. Limits (family 8+): 31 buffer args, 128 texture args, 31 threadgroup args per kernel | Metal Feature Set Tables |

### 3.3 MoltenVK's own documented overheads

- *"the slowest step involves converting shaders from SPIR-V to MSL source code
  format"* → mitigate with `vkGetPipelineCacheData()`
- **Fast-math is on by default** (`MVK_CONFIG_FAST_MATH` mode 2) and *"may
  optimize floating point operations in ways that violate the IEEE 754
  standard"*. **For an emulator this is a debuggability landmine — ship mode 0 or
  1 in debug builds.**
- No published overhead benchmark vs native Vulkan exists (**UNVERIFIED**). Do not
  quote a percentage.

---

## 4. MSL as a direct target

Yes, and the tooling is mature.

| Tool | Licence | Input → MSL | Note |
|---|---|---|---|
| **SPIRV-Cross `CompilerMSL`** | Apache-2.0 (`spirv_msl.hpp` is Apache-2.0 OR MIT) | SPIR-V → MSL | Mature, mature enough that MoltenVK itself uses it. Also gives `get_shader_resources()` reflection and `add_msl_resource_binding()` for deterministic Metal descriptor indices |
| **Tint / Dawn** | Apache-2.0 | WGSL → MSL | The standalone `dawn.googlesource.com/tint` repo is **deprecated/read-only**; *"Please switch to the Dawn repo."* Moving target |
| **DXC `-metal`** | MIT | HLSL → Metal | Requires Apple's **closed-source, macOS-only** Metal Shader Converter at build time. **Not shippable as a runtime library on iOS** |
| **shaderc** | Apache-2.0 | GLSL/HLSL → SPIR-V | **No Metal backend.** Not a candidate |

**Correction to a common belief:** there is no `-fspv-version=msl`, no
`spvVersion: 4.0`, and no "MTLLanguage SPIR-V dialect" in the MSL Specification
or Xcode Metal compiler options. The only real dialect is DXC's
`-fspv-target-env=` (vulkan1.0…1.4). **Treat any such flag as nonexistent.**

### Metal device floor

| Device | MTLGPUFamily |
|---|---|
| A11 | Apple4 |
| A12 | Apple5 |
| A13 | Apple6 |
| A14 | Apple7 |
| A15/A16 | Apple8 |
| A17/M3/M4 | Apple9 |

Metal 3 and Metal 4 both require **A14+**. Apple's mapping is documented at
https://docs.developer.apple.com/documentation/metal/mtlgpufamily/apple9

---

## 5. Recommended phased strategy

### Phase 0 — now

**No GPU code. No Vulkan dependency. No MoltenVK dependency.**

Document the boundary and get it wrong-proof:
`core/gpu/guest/` and `core/gpu/shader/` will contain **no host API calls at
all**. The host GPU API appears only in `core/gpu/host/`, behind an interface,
and no platform-specific header is reachable from `core/gpu/guest/`.

### Phase 1 — iOS v1: software rasteriser → Metal present

**Directly follow UTM.** Guest rasterisation writes into a CPU-visible buffer;
that buffer becomes a `MTLBuffer` in `.shared`/`.managed` storage mode; a
full-screen blit presents it.

- No shader compiler at all
- App Store compliant by construction
- **This is the fastest path to a shippable product**, and it is the tier that
  must work before anything else is worth optimising
- Establishes the guest GPU command processor and the resource model, which are
  needed at every later tier

### Phase 2 — SPIR-V → MSL via SPIRV-Cross, directly

- Integrate `CompilerMSL` in-process. Apache-2.0, actively maintained, already
  proven inside MoltenVK
- **Bake MSL at build time** with `spirv-cross --msl` and ship `.metal` source in
  the app bundle. Do **not** do MoltenVK-style runtime conversion on a phone; the
  MoltenVK User Guide itself flags it as the slowest pipeline step
- Use `MSLResourceBinding` / `add_msl_resource_binding()` so the Vulkan-style
  bindmap maps onto Metal indices deterministically
- **Design the shader IR around vertex/fragment/compute only**, with Metal mesh
  shaders as the tessellation replacement. Accept subgroup=32 at the IR level now
  rather than discovering it later

### Phase 3 — native Metal backend (optional)

Only if SPIRV-Cross output quality becomes the bottleneck. Realistic options:
Tint/Dawn (with its build-system cost and deprecation history), or accepting MSL
hand-written for hot paths.

**Do not pre-build this abstraction.** SPIRV-Cross is good enough for years, and
an unused abstraction is a maintenance liability.

### Phase 4 — desktop Vulkan

Keep the Vulkan backend for Windows/Linux desktop. It is where P5EMU's developers
can iterate fastest, and MoltenVK covers macOS if a Vulkan-centric build is
wanted.

---

## 6. Cross-cutting requirements

Non-negotiable regardless of phase:

| Requirement | Reason |
|---|---|
| **Argument buffers, Tier 2** | 96 unique samplers per app on iOS is a hard ceiling. Tier 2 unlocks 1,000,000 textures/stage |
| **3 swapchain images** | MoltenVK's own recommendation; 2 causes visible hitching |
| **Persist shader/pipeline cache to disk at first boot** | Non-negotiable on a phone; MSL/SPIR-V translation is the slowest pipeline step |
| **`MVK_CONFIG_FAST_MATH=0` in debug builds** | Otherwise IEEE-754 divergence becomes undebuggable |
| **Guest VA == host VA where possible** | Gives cache-coherent GPU access with no flush protocol. See [`cpu-feasibility.md`](cpu-feasibility.md) §3 |
| **Separate CPU and GPU protection axes** | `PROT_CPU_*` vs `PROT_GPU_*`, as both KytyPS5 and shadPS4 model it |
| **Process-lifetime reservation ⇒ recoverable GPU faults** | Unmapped pages fault on access rather than needing eager residency tracking |
| **Shader IR has no host API types** | Otherwise the "portable core" fiction collapses the first time Metal and Vulkan disagree |

---

## 7. Risks

| Risk | Severity | Status |
|---|---|---|
| Geometry/tessellation stage needed and Metal has none | **High** | Structural. Apple guidance: *"Apply mesh shader techniques to replace tessellation functionality"* — requires `apple7`+ (A14+) and iOS 16+ |
| Metal fast-math masks FP bugs in translated shaders | Medium | Mitigated by config |
| Sampler/texture ceiling | Medium | Mitigated by Tier 2 argument buffers |
| Subgroup size fixed at 32, guest wave64 | Medium | Design the IR around it now |
| PS5 extended memory (512 GiB reservation) far exceeds iOS limits | **High** | Demand-paged design required on mobile. The memory strategy must differ fundamentally |
| PS5 tile/G-mode layout and compression formats | Medium | Layout not public; must be derived from AMD docs + hardware |
| RDNA2 decode coverage gaps | Medium | `Opcode::UNSUPPORTED` exists in existing implementations, implying gaps. Coverage unpublished |
| DCB register defaults | Medium | GPL in KytyPS5. Must be dumped from hardware or obtained from public docs |
| MoltenVK image atomics | Low | UNVERIFIED but likely workable |

---

## 8. Unverified

| # | Item | Status |
|---|---|---|
| 1 | MoltenVK numeric overhead vs native Vulkan | **UNVERIFIED.** No credible primary-source measurement exists |
| 2 | MoltenVK image-atomics public limitation | **UNVERIFIED.** Only an internal note |
| 3 | Metal tile shaders / imageblocks on Apple silicon | **UNVERIFIED.** Feature table excerpt consulted did not cover imageblock rows |
| 4 | Exact MoltenVK version behind the extension lists | Read from `main`, not a pinned tag |
| 5 | `-fspv-version=msl` / MTLLanguage SPIR-V dialect | **DOES NOT EXIST.** Treated as nonexistent |
| 6 | PS5 render-target G-mode layout and compression | UNKNOWN publicly |
| 7 | PS5 guest GPU page-table (g-TT/UMMU) bit layout | UNKNOWN publicly |
| 8 | Meaning of `AgcUnknown*` names | UNKNOWN. Obviously SDK-derived obfuscated names |

---

## 9. Verdict

| Question | Answer |
|---|---|
| Is `guest → guest GPU abstraction → shader IR → SPIR-V/Vulkan → MoltenVK → Metal` reasonable? | **Reasonable for the desktop target. Not the right primary path for iOS.** |
| Reusable concepts | Guest command processor; five-stage recompiler (decode → CFG → translate → IR → backend); SRT as a first-class IR concept; compile-time `consteval` opcode hash; explicit `UNSUPPORTED` handling; GPU fault manager; host GPU thread with submission + suspend points |
| Apple/MoltenVK incompatibilities | **Geometry and tessellation stages are structurally absent.** Mesh shaders unreachable through MoltenVK. Sampler/texture ceilings. Subgroup fixed at 32. Pipeline statistics unsupported. `VkAllocationCallbacks` ignored. Swapchain limited to 3 |
| Should a native Metal backend remain an option? | **Yes — as Phase 3, not now.** SPIR-V → SPIRV-Cross → MSL is sufficient and better tested for years |
| GPU work in Phase 0? | **None.** Documentation and interface boundaries only. A fake GPU is worse than no GPU: it produces code that must be thrown away and gives false confidence |
