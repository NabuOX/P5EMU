// SPDX-License-Identifier: Apache-2.0

#include "p5emu/cpu/CpuModel.h"

namespace p5emu::cpu {

const char* ExecutionTierName(ExecutionTier tier) noexcept {
  switch (tier) {
    case ExecutionTier::kReferenceInterpreter:
      return "reference-interpreter";
    case ExecutionTier::kArm64:
      return "arm64";
  }
  return "unknown";
}

}  // namespace p5emu::cpu
