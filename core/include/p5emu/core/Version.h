// SPDX-License-Identifier: Apache-2.0
//
// P5EMU is an independent, open-source project. See LICENSE and
// PROVENANCE.md before adding code here.
//
// Every source file in this repository MUST carry an SPDX-License-Identifier
// line. If you are adding a file adapted from another project, the header must
// additionally carry:
//   SPDX-FileCopyrightText: <original copyright holder>
// and PROVENANCE.md must gain an entry recording the origin, the original
// licence and the modifications made.

#ifndef P5EMU_CORE_VERSION_H
#define P5EMU_CORE_VERSION_H

#include <cstdint>

namespace p5emu {

// Monotonic build identity. Not a compatibility claim: it says nothing about
// whether P5EMU can run any commercial software.
struct Version {
  std::uint32_t major;
  std::uint32_t minor;
  std::uint32_t patch;
};

constexpr Version kVersion{0, 1, 0};

const char* VersionString() noexcept;

}  // namespace p5emu

#endif  // P5EMU_CORE_VERSION_H
