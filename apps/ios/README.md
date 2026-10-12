# apps/ios — NOT IMPLEMENTED

**This directory is a placeholder. There is no iOS application, no Xcode
project, and no Apple code in this repository.**

Phase 0 establishes a portable core that must build and be testable without
Xcode. Adding an Xcode project now would:

- make the portable core dependent on a macOS host to verify
- imply iOS support that does not exist
- violate the Phase 0 constraint that the core must not depend on Xcode

---

## When this directory gets created

Phase 4, after the portable core is proven on desktop and after the
distribution questions in [`../../docs/research/ios-jit-audit.md`](../../docs/research/ios-jit-audit.md)
are settled.

## Constraints that will apply

These are established now so the app is not designed by accident later.

| # | Constraint | Source |
|---|---|---|
| 1 | **No runtime JIT in the App Store tier.** Interpreter only | ASR 2.5.2; `mmap(2)`: *"A writable, but not executable mapping is returned on iOS"* |
| 2 | **JIT is gated at runtime** on `dynamic-codesigning` (<17.4) or `com.apple.developer.cs.allow-jit` (17.4+) **plus** debugger attach | UTM's documented requirement |
| 3 | **The interpreter is a permanent tier**, not a degraded mode | It is the only tier Apple permits without a debugger |
| 4 | **JIT code must be arm64e with PAC-signed pointers**, and tested on device, never in the Simulator | https://developer.apple.com/documentation/browserenginekit/protecting-code-compiled-just-in-time |
| 5 | **Only one `MAP_JIT` region** is permitted under Hardened Runtime | https://developer.apple.com/documentation/apple-silicon/porting-just-in-time-compilers-to-apple-silicon |
| 6 | **Request the increased memory limit entitlement.** A PS5-class working set does not fit the default cap | UTM: `DEVELOPER_ACCOUNT_PAID = YES` |
| 7 | **Everything runs in one process.** No `fork`/`exec`; the "server" becomes a thread | iOS cannot start other programs |
| 8 | **Guideline 4.7.2**: HLE surfaces stay behind P5EMU's own types. Do not hand native Metal objects to guest software | https://developer.apple.com/app-store/review/guidelines |
| 9 | **Metal, not Vulkan, is the native path.** MoltenVK is a fallback, not the design centre | [`gpu-architecture.md`](../../docs/research/gpu-architecture.md) |
| 10 | **No Sony trademarks, UI assets, icons, sounds, or firmware.** The UI is original | Project requirement |

## UI direction (for later)

An original, console-style dashboard UI — good usability, clean information
hierarchy, console-like ergonomics. **Not** a reproduction of any Sony interface.
No Sony assets, no PlayStation typography or iconography, no DualSense imagery,
no firmware strings.

---

## The one-day experiment worth doing early

Whether `pthread_jit_write_protect_np` is permitted in a StikJIT-attached process
is **UNVERIFIED** and determines the entire code-cache design: proper W^X versus
an RWX toggle inside a debuggable process. See
[`ios-jit-audit.md` §7.6](../../docs/research/ios-jit-audit.md).
