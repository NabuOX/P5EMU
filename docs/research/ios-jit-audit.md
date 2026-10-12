# iOS / iPadOS JIT Constraint Audit

**Phase 0 research artefact.** The goal of this document is to stop P5EMU from
being designed around an assumption that turns out to be false.

Every environment below is separated because they differ in a way that changes
architecture, not just packaging.

---

## 1. Executive summary

**On iOS, a runtime code-generating JIT cannot ship through the App Store. Full
stop.**

Apple enforces W^X in hardware for any process without `CS_DEBUGGED`. Guideline
2.5.2 forbids executing code that "introduces or changes features or
functionality of the app". The JIT entitlement exists on macOS (Hardened
Runtime) but has **no iOS equivalent that third parties can obtain** — except for
vetted browser engines in the EU/Japan.

Therefore the correct architecture is **not** "JIT everywhere with an
interpreter fallback". It is:

> **One IR, multiple execution tiers, where the interpreter is the *primary*
> tier for App Store distribution and the JIT is an opt-in accelerator for
> power users.**

---

## 2. Environment matrix

| Environment | JIT? | Evidence | Confidence |
|---|---|---|---|
| **App Store, distribution-signed** | **NO** | `mmap(2)`: *"A writable, but not executable mapping is returned on iOS, watchOS and tvOS"*; ASR 2.5.2 | HIGH |
| **App Store, EU/Japan alternative browser engine** | **Partially** — approved browser-engine process only. Requires `com.apple.developer.cs.allow-jit` + `com.apple.developer.web-browser-engine.*`. Apple denied iSH's request to extend JIT to non-browser apps | https://developer.apple.com/support/alternative-browser-engines · https://ish.app/blog/ish-jit-and-eu | HIGH |
| **Free Apple ID, dev-signed (7-day sideload)** | **NO** by itself. 7-day expiry is a *profile* constraint, not a JIT one | https://docs.getutm.app/installation/ios | MED-HIGH |
| **Paid account, Development-signed sideload** | **YES**, but only while launched under a debugger (`get-task-allow` + `dynamic-codesigning`) | https://github.com/utmapp/UTM/blob/main/Documentation/iOSDevelopment.md · https://github.com/utmapp/UTM/blob/main/Documentation/TetheredLaunch.md | HIGH |
| **Paid account, Distribution / Ad-hoc / Enterprise** | **NO.** `get-task-allow` and `dynamic-codesigning` are stripped | https://docs.getutm.app/installation/ios (*"If the certificate is listed under Enterprise App, then it is the wrong certificate"*) | HIGH |
| **TrollStore** | **YES.** `UTM.HV.ipa` is pre-arranged for it | https://docs.getutm.app/installation/ios · https://github.com/utmapp/UTM/discussions/4583 | MED-HIGH |
| **Jailbroken** | **YES** (APRR writable+executable) | https://github.com/utmapp/UTM/blob/main/Documentation/Architecture.md | MED-HIGH |
| **Debugger-attach research env** (Jitterbug / StikDebug / StikJIT) | **YES** for `get-task-allow` apps | https://github.com/osy/Jitterbug · https://github.com/appdb-official/StikDebug-appdb | HIGH (mechanism) |

---

## 3. Primary sources

### 3.1 `MAP_JIT` on iOS

> "**MAP_JIT** — Allow mapping pages both `PROT_WRITE` and `PROT_EXEC` when the
> hardened runtime is enabled. Without this flag an attempt to create a mapping
> with both `PROT_WRITE` and `PROT_EXEC` set will fail with `MAP_FAILED` on
> macOS. **A writable, but not executable mapping is returned on iOS, watchOS
> and tvOS.** Usage of this flag requires the caller to have the
> `com.apple.security.cs.allow-jit` entitlement **on macOS**."
> — https://keith.github.io/xcode-man-pages/mmap.2.html

This is the sentence that explains why `MAP_JIT` "works" on iOS and confuses
people: the call succeeds and gives you a writable region that never becomes
executable.

### 3.2 The actual iOS entitlement: `dynamic-codesigning`

Not documented by Apple, but present in **WebKit's own build scripts**, which are
Apple's source:

```sh
if [[ "${PLATFORM_NAME}" == iphoneos ]]; then
    if (( $(( ${SDK_VERSION_ACTUAL} )) >= 170400 )); then
        plistbuddy Add :com.apple.developer.cs.allow-jit bool YES
        plistbuddy Add :com.apple.developer.web-browser-engine.webcontent bool YES
    else
        plistbuddy Add :dynamic-codesigning bool YES
    fi
else
    plistbuddy Add :dynamic-codesigning bool YES
fi
```
— https://github.com/WebKit/WebKit/blob/main/Source/JavaScriptCore/Scripts/process-entitlements.sh

Corroborated by UTM: *"You cannot install the normal IPA (UTM.ipa) because it
includes the dynamic-codesigning entitlements which TrollStore does not support."*
— https://docs.getutm.app/installation/ios

Split: **pre-17.4 = `dynamic-codesigning`; 17.4+ = `com.apple.developer.cs.allow-jit`
+ browser-engine entitlement.**

### 3.3 Apple on the macOS JIT model

> "When your app has the Hardened Runtime capability and the
> `com.apple.security.cs.allow-jit` entitlement, it can only **create one memory
> region** with the `MAP_JIT` flag set."
>
> "**The `pthread_jit_write_protect_np()` function isn't available on iOS.**"
> — https://developer.apple.com/documentation/apple-silicon/porting-just-in-time-compilers-to-apple-silicon

> "On platforms where `pthread_jit_write_protect_supported_np()` is true,
> **MAP_JIT regions are never writeable and executable simultaneously.**"
> — https://keith.github.io/xcode-man-pages/pthread_jit_write_protect_np.3.html

### 3.4 Apple on the iOS JIT model (BrowserEngineKit)

> "The system uses hardware memory management to enforce that a page of memory
> allocated by your process is either writable, or executable, but not
> simultaneously both (**W^X**). To toggle the writable or executable status of a
> memory page, your web content extension calls
> `be_memory_inline_jit_restrict_rwx_to_rx_with_witness()` and
> `be_memory_inline_jit_restrict_rwx_to_rw_with_witness()`."
>
> "When your app targets the **arm64e** instruction set, the
> `be_memory_inline_jit_restrict_rwx_to_rw_with_witness()` function inserts a
> **cryptographically signed pointer authentication code (PAC)** into the pointer
> to your JIT memory. … Additionally, your JIT compiler needs to emit **arm64e**
> code that uses PAC-protected pointers. … **You need to test your JIT code on an
> iPhone, not in the Simulator.**"
> — https://developer.apple.com/documentation/browserenginekit/protecting-code-compiled-just-in-time

### 3.5 App Store Review Guidelines

> **2.5.2** "Apps should be self-contained in their bundles, and may not read or
> write data outside the designated container area, nor may they **download,
> install, or execute code which introduces or changes features or functionality
> of the app**, including other apps. Educational apps designed to teach,
> develop, or allow students to test executable code may, in limited
> circumstances, download code provided that such code is not used for other
> purposes."
>
> **4.7** "Mini apps, mini games, streaming games, chatbots, plug-ins, and game
> emulators … **Additionally, retro game console and PC emulator apps can offer to
> download games.** You are responsible for all such software offered in your
> app…"
>
> **4.7.2** "Your app may not extend or expose native platform APIs or
> technologies to the software without prior permission from Apple."
> — https://developer.apple.com/app-store/review/guidelines

Two consequences that are easy to miss:

1. **4.7 permits emulators to download games.** P5EMU may ship on the App Store
   and may accept user-supplied game content, provided P5EMU itself is the app.
2. **4.7.2 constrains the HLE layer.** Any HLE that hands a native `MTLDevice` or
   `MTLCommandQueue` to guest-controlled software needs Apple's prior
   permission. Practically: keep guest↔host surfaces behind P5EMU's own types.

### 3.6 Is there a W^X loophole?

**No.** This is *inference*, and it is labelled as such because no Apple statement
explicitly rejects a W^X-respecting JIT.

The reasoning: Apple's model is not "you may never have executable
runtime-generated pages", it is "you must have the entitlement, and the
entitlement is not granted to App Store apps except browser engines". The policy
question is therefore "did you obtain the grant", not "did you observe W^X".
Apple's refusal to iSH (above) confirms the grant is the gate.

**A JIT that writes only to non-W+X pages is not a workaround.** It is a
different mechanism (interpreted or AOT execution) wearing the same name.

### 3.7 Interpreters under App Store rules

**Explicitly permitted.** Guideline 2.5.2 restricts *downloading/executing*
code; 4.7 allows emulators to download content. The historic Developer Program
License Agreement §3.3.2 read *"Interpreted code may only be used in an
Application if all scripts, code and interpreters are packaged in the Application
and not downloaded."* — **UNVERIFIED**, this is a 2011 third-party quotation of
the DPLA; fetch the live text before relying on it.

---

## 4. What UTM actually does

UTM is the only shipping full-system iOS emulator, and therefore the reference
case.

### GPU path — they avoided Vulkan entirely

> "UTM also includes a SPICE client written for Metal. … allows for some
> para-virtualization as the QXL graphics driver running in the guest OS can send
> low-level draw commands directly to Metal APIs."
> — https://getutm.app/faq
> — https://github.com/utmapp/UTM/blob/main/Documentation/Architecture.md

**Inference (high confidence):** the shipping iOS render path is
*guest software rasteriser → CPU framebuffer → Metal texture → present.* No
Vulkan, no MoltenVK, no GPU passthrough. P5EMU should assume this baseline.

### CPU tiers

| Artifact | Channel | JIT | Notes |
|---|---|---|---|
| `UTM.deb` | Jailbreak | **Yes** | APRR |
| `UTM.ipa` | AltStore (dev-signed) | **Yes** (semi-tethered) | Needs `dynamic-codesigning` + `get-task-allow` + debugger attach from iOS 14 |
| `UTM.HV.ipa` | TrollStore | **Yes** | + Hypervisor on M1 iPad |
| `UTM.SE.ipa` | AltStore / enterprise / **App Store** | **No** | Threaded interpreter |

> "**UTM/QEMU requires dynamic code generation (JIT) for maximum performance. JIT
> on iOS devices require either a jailbroken device, or one of the various
> workarounds found for specific versions of iOS.** … **UTM SE ('slow edition')
> uses a threaded interpreter which performs better than a traditional
> interpreter but still slower than JIT.** This technique is similar to what iSH
> does for dynamic execution. As a result, UTM SE does not require jailbreaking
> or any JIT workarounds and can be sideloaded as a regular app."
> — https://github.com/utmapp/UTM

> "For JIT to work on the latest version of iOS, it must be launched through the
> debugger. … **You cannot launch UTM from the home screen in iOS 14 or it will
> not work properly!**"
> — https://github.com/utmapp/UTM/blob/main/Documentation/TetheredLaunch.md

### iSH's interpreter — the only measured data point available

> "It's not quite a JIT since it doesn't target machine code. Instead it
> generates an array of pointers to functions called **gadgets**, and each gadget
> ends with a **tailcall** to the next function; like the **threaded code**
> technique used by some Forth interpreters. The result is a **speedup of roughly
> 3–5x** compared to emulation using a simpler switch dispatch."
> — https://github.com/ish-app/ish

That 3–5× is the single most useful performance number in this document. It means
the *dispatch mechanism* matters enormously and is worth engineering deliberately
from the start — it is not an optimisation to add later.

---

## 5. Recommended strategy

### Tier 1 — App Store (primary)

**Threaded-code interpreter over the IR. No JIT, no AOT-to-native.**

- Threaded dispatch with **hand-written ARM64 assembly gadgets** and computed
  `br` chains (iSH's 3–5× is the justification; UTM SE independently chose the
  same technique)
- Pre-linked into the binary. This is AOT of the *interpreter*, not of guest
  code — 100% guideline-clean
- Zero entitlement risk
- **This is the tier Apple permits, and it should be developed first and kept
  permanently.** It is the reference backend regardless.

### Tier 2 — Sideloaded / development

Same IR, JIT backend compiled in but **gated at runtime** on:

1. Profile carries `dynamic-codesigning` (< 17.4) or `com.apple.developer.cs.allow-jit`
   (17.4+), **and**
2. The process is debugger-attached (`get-task-allow`)

UTM's `UTM.ipa` and StikDebug/Jitterbug are the working recipes. Covers
TrollStore and jailbreak for free.

### Tier 3 — macOS

Fully documented and unambiguous: `MAP_JIT` +
`pthread_jit_write_protect_np()`, or the hardened allowlist-callback model
(`com.apple.security.cs.jit-write-allowlist` +
`pthread_jit_write_with_callback_np`). Note Apple's explicit statement that the
macOS W^X mode is *"not available on iOS"* — **so the desktop and mobile code
cache implementations share the IR, not the allocator.**

### Tier 4 — Ruled out

**Block-based/WASM JIT.** Granted to WebKit, not third parties. Guideline 4.7.2
also blocks exposing native platform APIs to guest-driven software.

### Non-negotiable cross-cutting requirements

- **Request the increased memory limit entitlement.** UTM does this
  (`DEVELOPER_ACCOUNT_PAID = YES` → *"increased memory limit entitlement"*). A
  PS5-class guest working set will not fit the default iOS cap. This is a hard
  blocker for *any* tier and is easy to overlook.
- **The iOS JIT backend must emit arm64e with PAC-signed pointers**, and must be
  tested on a device, never in the Simulator.
- **Never assume `mmap` RWX works.** Even with the entitlement, only **one**
  `MAP_JIT` region is permitted under Hardened Runtime.

---

## 6. What this means for the portable core

The core must not assume any of this. Concretely, the design rules that follow:

| Rule | Reason |
|---|---|
| No `mmap` in `core/` | The code cache is a platform component (see [`gpu-architecture.md`](gpu-architecture.md) for the same rule on the GPU side) |
| Code cache behind an interface | Windows/Linux/Darwin implementations differ fundamentally |
| The interpreter is always built | It is the App Store tier and the test oracle |
| No privileged syscalls in the core | iOS forbids them; the interpreter must be sufficient on its own |
| No host threads assumed beyond the process | No `fork`/`exec` — iOS cannot start other programs. Everything runs in one process, as threads |
| No blocking console I/O | iOS apps have no console |

The first rule is already enforced: `core/` contains no `mmap`, no
`/proc/self/maps`, no signal handlers. The iOS-facing work belongs in
`apps/ios/`, which does not exist yet and is not in Phase 0 scope.

---

## 7. Unverified / unknown

| # | Item | Status |
|---|---|---|
| 1 | `pthread_create_with_jit_allocator` | **DOES NOT EXIST as an Apple API.** Found in no Apple doc or header. The real primitives are `pthread_jit_write_protect_np`, `pthread_jit_write_with_callback_np`, `pthread_jit_write_freeze_callbacks_np`, and the `BE_*_inline_jit_restrict_*_with_witness` family. **Do not put this name in any design document.** |
| 2 | iOS version that introduced `MAP_JIT` | **UNVERIFIED.** `mmap(2)` states no version. To pin it, diff `sys/mman.h` across two Xcode SDK versions |
| 3 | WWDC session on iOS JIT | **NONE.** WWDC 2023 session 10060 is privacy manifests; 10061 is dependency signing; 10289 is a signing/distribution Q&A. Apple's only substantive iOS JIT doc is the BrowserEngineKit page quoted above |
| 4 | Current DPLA §3.3.2 wording | **UNVERIFIED.** Only a 2011 third-party quotation available |
| 5 | Whether Apple review has ever explicitly rejected a W^X-consistent JIT | **UNVERIFIED.** The "no loophole" conclusion is inference from the entitlement model plus the iSH denial |
| 6 | Whether `pthread_jit_write_protect_np` is permitted in a StikJIT-attached process | **UNVERIFIED and important.** If yes, P5EMU gets proper W^X on iOS; if no, an RWX toggle within a debuggable process. **This is a one-day experiment and it determines the code-cache design** |
| 7 | Contents of `willfaust/FEX` branch `ios-port-2607` | **UNVERIFIED.** Branch head SHA and MIT licence confirmed; the diff was not read. Whether it uses `MAP_JIT` or an RWX toggle is unknown. **Highest-value next read.** |
| 8 | iSH's 3–5× measurement conditions | Reported by the author without methodology. Directionally reliable, not a benchmark |
| 9 | Performance ceiling of a threaded interpreter on modern A-series silicon | **UNVERIFIED.** No published measurement found |

---

## 8. Verdict

| Question | Answer |
|---|---|
| Should P5EMU support an interpreter fallback? | **Not as a fallback — as the primary App Store tier, permanently.** |
| Should P5EMU support a JIT? | **Yes, gated at runtime**, for sideloaded/development/TrollStore/jailbreak. Never as the only path. |
| Is there an AOT/recompilation path worth researching? | **Yes.** LLVM `llc -mtriple=aarch64-apple-ios` needs no JIT and would let P5EMU ship pre-translated blocks. FEX already has the AOT cache concept (`FEXOfflineCompiler`, `PatchableGuestRIP`/CRC). **This is the only route that combines performance with App Store distribution.** |
| Should the project be designed around an unsupported assumption? | **No.** Design around: interpreter always works, JIT is optional, AOT is the performance tier that can ship. |
