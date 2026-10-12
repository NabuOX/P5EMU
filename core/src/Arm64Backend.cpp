// SPDX-License-Identifier: Apache-2.0
//
// P5EMU is an independent, open-source project. See LICENSE and
// PROVENANCE.md before adding code here.

#include "p5emu/cpu/Arm64Backend.h"

#include <cstring>
#include <string>
#include <vector>

#include "p5emu/cpu/Arm64Abi.h"
#include "p5emu/cpu/ReferenceInterpreter.h"

namespace p5emu::cpu {
namespace {

StatusCode StatusCodeForFault(std::uint64_t fault_code) {
  switch (fault_code) {
    case P5EMU_A64_MEM_UNMAPPED: return StatusCode::kUnmappedAccess;
    case P5EMU_A64_MEM_ACCESS: return StatusCode::kAccessViolation;
    case P5EMU_A64_MEM_READONLY: return StatusCode::kReadOnlyAccess;
    default: return StatusCode::kUnmappedAccess;
  }
}

// Maps a guest CPU fault kind to a Status. These MUST match the interpreter so
// an interpreter/AArch64 differential comparison agrees on the fault.
StatusCode StatusCodeForCpuFault(std::uint64_t code) {
  switch (code) {
    case P5EMU_A64_CPU_FAULT_DIVIDE: return StatusCode::kDivideError;
    default: return StatusCode::kDivideError;
  }
}

// Maps an HLE helper result code to a Status. These MUST match the host-side
// HLE error model so an interpreter/AArch64 differential comparison agrees.
StatusCode StatusCodeForHle(std::uint64_t code) {
  switch (code) {
    case P5EMU_A64_HLE_NOT_FOUND: return StatusCode::kHleNotFound;
    case P5EMU_A64_HLE_AMBIGUOUS: return StatusCode::kHleAmbiguous;
    case P5EMU_A64_HLE_UNSUPPORTED_IDENTITY:
      return StatusCode::kHleUnsupportedIdentity;
    case P5EMU_A64_HLE_INVALID_ARGUMENT:
      return StatusCode::kHleInvalidArgument;
    case P5EMU_A64_HLE_MEMORY_FAULT: return StatusCode::kHleMemoryFault;
    case P5EMU_A64_HLE_DISPATCH_FAULT: return StatusCode::kHleDispatchFault;
    case P5EMU_A64_HLE_INTERNAL: return StatusCode::kHleInternalError;
    default: return StatusCode::kHleInternalError;
  }
}

class Arm64Backend final : public Backend {
 public:
  Arm64Backend(std::uint64_t step_limit, Arm64Runner* runner,
               Arm64CodeGenOptions options)
      : step_limit_(step_limit), runner_(runner), options_(options) {}

  [[nodiscard]] BackendId id() const noexcept override {
    return BackendId::kArm64;
  }
  [[nodiscard]] const char* name() const noexcept override { return "arm64"; }
  [[nodiscard]] std::uint64_t step_limit() const noexcept override {
    return step_limit_;
  }

  ExecutionResult Execute(const ir::Program& program, CpuState* state,
                          memory::GuestMemoryPort& memory) override;

 private:
  std::uint64_t step_limit_;
  Arm64Runner* runner_;
  Arm64CodeGenOptions options_;
};

ExecutionResult Arm64Backend::Execute(const ir::Program& program,
                                      CpuState* state,
                                      memory::GuestMemoryPort& memory) {
  ExecutionResult result;

  if (state == nullptr) {
    result.status = StatusCode::kInvalidArgument;
    result.reason = TerminationReason::kFault;
    return result;
  }

  // The IR must be valid before codegen. The interpreter does the same; agreeing
  // on validity is what keeps the comparison meaningful.
  const Status valid = ir::ValidateIr(program);
  if (!valid.ok()) {
    result.status = valid;
    result.reason = TerminationReason::kIrInvalid;
    result.final_rip = state->rip;
    return result;
  }

  Arm64CodeGenResult cg;
  const Status gen = GenerateArm64(program, options_, &cg);
  if (!gen.ok()) {
    // Unsupported IR (or a structurally unusable program) is a coverage gap, not
    // a failure. NEVER fall back to interpreting.
    result.status = gen;
    result.reason = TerminationReason::kRunning;
    result.final_rip = state->rip;
    return result;
  }

  if (runner_ == nullptr || !runner_->available()) {
    result.status = StatusCode::kUnimplementedInstruction;
    result.reason = TerminationReason::kRunning;
    result.final_rip = state->rip;
    return result;
  }

  // Transport: this backend runs out-of-process, so it needs the guest memory
  // image. Every region the process exposes is transported, in order, with its
  // permissions; the harness enforces them within each region.
  const std::vector<memory::Region> regions = memory.Regions();
  if (regions.empty()) {
    result.status = StatusCode::kUnimplementedInstruction;
    result.reason = TerminationReason::kRunning;
    result.final_rip = state->rip;
    return result;
  }
  std::vector<Arm64MemoryRegion> transport;
  transport.reserve(regions.size());
  for (const memory::Region& region : regions) {
    Arm64MemoryRegion tr;
    tr.base = region.guest_base;
    tr.permissions = region.permissions;
    const Status read =
        memory.ReadRange(region.guest_base, region.size, &tr.bytes);
    if (!read.ok()) {
      result.status = StatusCode::kUnimplementedInstruction;
      result.reason = TerminationReason::kRunning;
      result.final_rip = state->rip;
      return result;
    }
    transport.push_back(std::move(tr));
  }

  P5EmuArm64Ctx ctx{};
  std::memset(&ctx, 0, sizeof(ctx));
  ctx.gpr[0] = state->rax;
  ctx.gpr[1] = state->rbx;
  ctx.gpr[2] = state->rcx;
  ctx.gpr[3] = state->rdx;
  ctx.gpr[4] = state->rsi;
  ctx.gpr[5] = state->rdi;
  ctx.gpr[6] = state->rbp;
  ctx.gpr[7] = state->rsp;
  ctx.gpr[8] = state->r8;
  ctx.gpr[9] = state->r9;
  ctx.gpr[10] = state->r10;
  ctx.gpr[11] = state->r11;
  ctx.gpr[12] = state->r12;
  ctx.gpr[13] = state->r13;
  ctx.gpr[14] = state->r14;
  ctx.gpr[15] = state->r15;
  ctx.rip = state->rip;
  ctx.eflags = state->eflags;
  ctx.steps = step_limit_;
  for (std::uint32_t i = 0; i < kXmmCount; ++i) {
    ctx.xmm[i * 2u] = state->xmm[i].lo;
    ctx.xmm[i * 2u + 1u] = state->xmm[i].hi;
  }

  std::vector<std::vector<std::uint8_t>> regions_out;
  std::string detail;
  const Status ran =
      runner_->RunRegions(cg.code, &ctx, transport, &regions_out, &detail);
  if (!ran.ok()) {
    result.status = ran;
    result.reason = TerminationReason::kRunning;
    result.final_rip = state->rip;
    return result;
  }

  // Write final state back.
  state->rax = ctx.gpr[0];
  state->rbx = ctx.gpr[1];
  state->rcx = ctx.gpr[2];
  state->rdx = ctx.gpr[3];
  state->rsi = ctx.gpr[4];
  state->rdi = ctx.gpr[5];
  state->rbp = ctx.gpr[6];
  state->rsp = ctx.gpr[7];
  state->r8 = ctx.gpr[8];
  state->r9 = ctx.gpr[9];
  state->r10 = ctx.gpr[10];
  state->r11 = ctx.gpr[11];
  state->r12 = ctx.gpr[12];
  state->r13 = ctx.gpr[13];
  state->r14 = ctx.gpr[14];
  state->r15 = ctx.gpr[15];
  state->rip = ctx.rip;
  state->eflags = ctx.eflags;
  state->executed_ops = ctx.steps;
  for (std::uint32_t i = 0; i < kXmmCount; ++i) {
    state->xmm[i].lo = ctx.xmm[i * 2u];
    state->xmm[i].hi = ctx.xmm[i * 2u + 1u];
  }

  if (regions_out.size() == regions.size()) {
    for (std::size_t i = 0; i < regions.size(); ++i) {
      if (regions_out[i].size() == regions[i].size) {
        (void)memory.WriteRange(regions[i].guest_base, regions[i].size,
                                regions_out[i].data());
      }
    }
  }

  result.reason = static_cast<TerminationReason>(ctx.status);
  switch (result.reason) {
    case TerminationReason::kFault:
      result.status = StatusCodeForFault(ctx.fault_code);
      break;
    case TerminationReason::kHleError:
      result.status = StatusCodeForHle(ctx.fault_code);
      break;
    case TerminationReason::kCpuFault:
      result.status = StatusCodeForCpuFault(ctx.fault_code);
      break;
    case TerminationReason::kTrap:
      result.status = StatusCode::kUnimplementedInstruction;
      break;
    case TerminationReason::kStepLimit:
      result.status = StatusCode::kExecutionLimit;
      break;
    case TerminationReason::kIrInvalid:
      result.status = StatusCode::kInvalidIr;
      break;
    case TerminationReason::kRunning:
      // The generated function never returns kRunning; the interpreter maps a
      // still-running finish to kFallthrough. Match that.
      result.reason = TerminationReason::kFallthrough;
      result.status = StatusCode::kOk;
      break;
    default:
      result.status = StatusCode::kOk;
      break;
  }
  result.executed_ops = ctx.steps;
  result.final_rip = ctx.rip;
  return result;
}

}  // namespace

std::unique_ptr<Backend> MakeArm64Backend(std::uint64_t step_limit,
                                          Arm64Runner* runner,
                                          Arm64CodeGenOptions options) {
  return std::make_unique<Arm64Backend>(step_limit, runner, options);
}

}  // namespace p5emu::cpu
