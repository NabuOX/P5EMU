# Acknowledgements

P5EMU is an independent, proprietary project. It is not affiliated with,
endorsed by, or derived from Sony Interactive Entertainment, and it contains no
Sony code, firmware, keys, or assets.

This file records **architectural and factual research**, not code reuse. The
code-reuse record is [`../PROVENANCE.md`](../PROVENANCE.md); its adaptations
register is empty by design, because no third-party source has been adapted into
P5EMU.

---

## Research and reference projects

P5EMU's architecture was derived by reading these projects' public source,
documentation and licences. Reading a project to learn how a problem is solved
is legitimate; copying its code is not, and none was copied.

| Project | Upstream | Licence | What was learned |
|---|---|---|---|
| FEX | https://github.com/FEX-Emu/FEX | MIT | x86-64 → AArch64 IR and register-allocation-as-IR-pass design; AArch64 "splatter" codegen without an isel stage; AOT cache concept |
| QEMU | https://gitlab.com/qemu-project/qemu | GPL-2.0-or-later | Basic-block translation discipline; the `CONFIG_DARWIN` + `MAP_JIT` code-generation-buffer recipe |
| dynarmic | https://github.com/lioncash/dynarmic | 0BSD | Bring-your-own-memory abstraction; separate RX/RW code-cache address spaces on Apple platforms |
| PCSX2 | https://github.com/PCSX2/pcsx2 | GPL-3.0-or-later | Why a hand-written-per-host recompiler with no portable IR makes a second host architecture a rewrite |
| RPCS3 | https://github.com/RPCS3/rpcs3 | GPL-2.0 | Using a compiler IR as the translation contract; per-component test harness structure |
| Kyty | https://github.com/InoriRus/Kyty | MIT | The FreeBSD-derived PlayStation ELF dynamic-tag namespace |
| KytyPS5 | https://github.com/KytyPS5/KytyPS5 | GPL-2.0-only | Inventory of PS5 subsystems that need emulating (names only) |
| shadPS4 | https://github.com/shadps4-emu/shadPS4 | GPL-2.0-or-later | PlayStation memory-region taxonomy; the "no game-specific hacks" engineering rule |
| Play! | https://github.com/jpd002/Play- | BSD-2-Clause | Native-implementation-vs-JIT differential test design |
| PPSSPP | https://github.com/hrydgard/ppsspp | GPL-2.0-or-later | Cross-backend `--compare` differential execution across interpreter and JIT |
| Madeira | https://github.com/willfaust/Madeira | GPL-3.0-or-later | Evidence that an x86-64 → ARM64 JIT can run on jailed iOS hardware via debugger attach |
| UTM | https://github.com/utmapp/UTM | GPL-2.0 | Threaded-interpreter ("slow edition") tier for JIT-prohibited environments |

## Standards and specifications

| Source | Role |
|---|---|
| AMD RDNA 2 ISA Reference Guide (doc 70648) | Shader encoding reference |
| Intel SDM / AMD64 APM | x86-64 architectural semantics |
| ARM DDI 0487 (A-profile ARM) | AArch64 encoding reference |
| Vulkan Specification | Host graphics contract |
| Metal Shading Language Specification | Future native Metal backend |
| Apple App Store Review Guidelines §2.5.2, §4.7 | iOS distribution constraints |
| Apple "Porting Just-in-Time Compilers to Apple Silicon" | W^X model on macOS |
| Apple BrowserEngineKit JIT-protection documentation | W^X + PAC model on iOS |
