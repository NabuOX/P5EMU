// SPDX-License-Identifier: LicenseRef-P5EMU-Proprietary
//
// The CLI. Phase 6 adds the HLE foundation and the guest-to-host call bridge on
// top of the CPU core, AArch64 backend, PS5 ELF loader and relocations: a guest
// import binds to an HLE binding, a guest CALL reaches the dispatcher, and the
// guest resumes with the host return value. The command line still does not run
// a guest; it reports build identity and host properties.

#include <cstdio>

#include "p5emu/core/Log.h"
#include "p5emu/core/Version.h"

int main() {
  std::printf("p5emu %s\n", p5emu::VersionString());
  std::printf(
      "phase: 6 (library: CPU + AArch64 + PS5 ELF loader + relocations + HLE "
      "call bridge; no CLI guest input yet)\n");

  P5EMU_LOG_INFO("logging facility reachable (level=%s)",
                 p5emu::LogLevelName(p5emu::GetLogLevel()));
  return 0;
}
