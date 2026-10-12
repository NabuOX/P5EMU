// SPDX-License-Identifier: Apache-2.0
//
// P5EMU is an independent, open-source project. See LICENSE and
// PROVENANCE.md before adding code here.

#include "p5emu/core/Version.h"

namespace p5emu {

const char* VersionString() noexcept {
  static const char kString[] = "0.1.0";
  return kString;
}

}  // namespace p5emu
