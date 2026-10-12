// SPDX-License-Identifier: LicenseRef-P5EMU-Proprietary
//
// P5EMU source is independently authored and proprietary. See LICENSE.md and
// PROVENANCE.md before adding code here.

#include "p5emu/core/Version.h"

namespace p5emu {

const char* VersionString() noexcept {
  static const char kString[] = "0.1.0";
  return kString;
}

}  // namespace p5emu
