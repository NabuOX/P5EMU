// SPDX-License-Identifier: LicenseRef-P5EMU-Proprietary
//
// P5EMU source is independently authored and proprietary. See LICENSE.md and
// PROVENANCE.md before adding code here.

#include "p5emu/cpu/Arm64CodeGen.h"

#include <cstdint>
#include <string>

#include "p5emu/cpu/Arm64Abi.h"
#include "p5emu/cpu/Arm64Emitter.h"
#include "p5emu/cpu/ReferenceInterpreter.h"

namespace p5emu::cpu {
namespace {

using arm64::Assembler;
using arm64::Cond;
using arm64::Reg;

// Scratch registers, following the ABI in Arm64Abi.h. All values live in stack
// slots, so these are free to clobber within a single op.
constexpr Reg kCtx = 19;  // callee-saved: the context pointer
constexpr Reg kT0 = 9;
constexpr Reg kT1 = 10;
constexpr Reg kT2 = 11;  // result
constexpr Reg kT3 = 12;  // cf
constexpr Reg kT4 = 13;  // of
constexpr Reg kT5 = 14;  // zf
constexpr Reg kT6 = 15;  // sf
constexpr Reg kT7 = 16;  // pf
constexpr Reg kT8 = 17;  // af
constexpr Reg kMask = 8;  // width-mask constant (caller-saved, free in flag/shift ops)
constexpr Reg kSp = 31;

// Operand width model, matching the interpreter exactly (ReferenceInterpreter).
constexpr std::uint32_t NormWidth(std::uint32_t w) noexcept {
  return (w == 1 || w == 2 || w == 4) ? w : 8u;
}
constexpr std::uint32_t WidthBits(std::uint32_t w) noexcept {
  return 8u * NormWidth(w);
}
constexpr std::uint64_t WidthMask(std::uint32_t w) noexcept {
  return (NormWidth(w) >= 8) ? ~std::uint64_t{0}
                             : ((std::uint64_t{1} << (8u * NormWidth(w))) - 1u);
}

// EFLAGS bit positions (must match p5emu::cpu::eflags).
constexpr unsigned kCfBit = 0;
constexpr unsigned kPfBit = 2;
constexpr unsigned kAfBit = 4;
constexpr unsigned kZfBit = 6;
constexpr unsigned kSfBit = 7;
constexpr unsigned kOfBit = 11;

constexpr std::uint64_t kInvalidOff = ~std::uint64_t{0};

// Byte offset into P5EmuArm64Ctx for an architectural register access.
std::uint64_t CtxOffsetForRegister(ir::ArchRegister r) {
  switch (r) {
    case ir::ArchRegister::kRax: return P5EMU_A64_CTX_GPR_OFFSET + 0 * 8;
    case ir::ArchRegister::kRbx: return P5EMU_A64_CTX_GPR_OFFSET + 1 * 8;
    case ir::ArchRegister::kRcx: return P5EMU_A64_CTX_GPR_OFFSET + 2 * 8;
    case ir::ArchRegister::kRdx: return P5EMU_A64_CTX_GPR_OFFSET + 3 * 8;
    case ir::ArchRegister::kRsi: return P5EMU_A64_CTX_GPR_OFFSET + 4 * 8;
    case ir::ArchRegister::kRdi: return P5EMU_A64_CTX_GPR_OFFSET + 5 * 8;
    case ir::ArchRegister::kRbp: return P5EMU_A64_CTX_GPR_OFFSET + 6 * 8;
    case ir::ArchRegister::kRsp: return P5EMU_A64_CTX_GPR_OFFSET + 7 * 8;
    case ir::ArchRegister::kR8:  return P5EMU_A64_CTX_GPR_OFFSET + 8 * 8;
    case ir::ArchRegister::kR9:  return P5EMU_A64_CTX_GPR_OFFSET + 9 * 8;
    case ir::ArchRegister::kR10: return P5EMU_A64_CTX_GPR_OFFSET + 10 * 8;
    case ir::ArchRegister::kR11: return P5EMU_A64_CTX_GPR_OFFSET + 11 * 8;
    case ir::ArchRegister::kR12: return P5EMU_A64_CTX_GPR_OFFSET + 12 * 8;
    case ir::ArchRegister::kR13: return P5EMU_A64_CTX_GPR_OFFSET + 13 * 8;
    case ir::ArchRegister::kR14: return P5EMU_A64_CTX_GPR_OFFSET + 14 * 8;
    case ir::ArchRegister::kR15: return P5EMU_A64_CTX_GPR_OFFSET + 15 * 8;
    case ir::ArchRegister::kRip: return P5EMU_A64_CTX_RIP_OFFSET;
    case ir::ArchRegister::kEflags: return P5EMU_A64_CTX_EFLAGS_OFFSET;
    default: return kInvalidOff;
  }
}

class Gen {
 public:
  Gen(const ir::Program& program, const Arm64CodeGenOptions& options,
      Arm64CodeGenResult* out)
      : program_(program), options_(options), out_(out) {}

  Status Run() {    if (program_.entry == ir::kInvalidBlock) {
      return StatusCode::kInvalidArgument;
    }

    value_count_ = MaxValueId() + 1u;
    // One extra slot holds the memory-helper status word.
    status_slot_off_ = 16u * value_count_;
    frame_bytes_ = 16u * (value_count_ + 1u);
    if (frame_bytes_ > 32760u) {
      // The unsigned-offset loads/stores used for slots top out at 4095*8
      // bytes. A program past that needs a different addressing strategy, not a
      // silently-truncated offset.
      return Unsupported("program frame exceeds the Phase-2 slot addressing range");
    }

    if (!BuildBlockLabels()) {
      return StatusCode::kInvalidArgument;
    }

    epilogue_ = asm_.NewLabel();
    fault_exit_ = asm_.NewLabel();
    step_exit_ = asm_.NewLabel();
    hle_error_exit_ = asm_.NewLabel();
    for (const ir::Block& block : program_.blocks) {
      block_labels_[block.id] = asm_.NewLabel();
    }

    EmitPrologue();
    asm_.B(block_labels_[program_.entry]);

    for (const ir::Block& block : program_.blocks) {
      asm_.Bind(block_labels_[block.id]);
      // Runaway protection: a per-block-entry budget, so a generated infinite
      // loop terminates with kStepLimit instead of hanging the host.
      CtxLoad(kT0, P5EMU_A64_CTX_STEPS_OFFSET);
      asm_.Cbz(kT0, step_exit_);
      Raw(arm64::enc::AddSubImm(true, false, kT0, kT0, 1, false));  // sub x9, x9, #1
      CtxStore(kT0, P5EMU_A64_CTX_STEPS_OFFSET);
      for (const ir::Op& op : block.ops) {
        const Status st = EmitOp(op, block.id);
        if (!st.ok()) {
          out_->reason = reason_;
          return st;
        }
      }
      const Status st = EmitTerminator(block);
      if (!st.ok()) {
        out_->reason = reason_;
        return st;
      }
    }

    // Shared exit paths.
    asm_.Bind(step_exit_);
    asm_.MovImm64(kT0, P5EMU_A64_REASON_STEP_LIMIT);
    CtxStore(kT0, P5EMU_A64_CTX_STATUS_OFFSET);
    asm_.B(epilogue_);

    asm_.Bind(fault_exit_);
    // x9 holds the memory-helper status when control reaches here.
    CtxStore(kT0, P5EMU_A64_CTX_FAULT_OFFSET);
    asm_.MovImm64(kT0, P5EMU_A64_REASON_FAULT);
    CtxStore(kT0, P5EMU_A64_CTX_STATUS_OFFSET);
    asm_.B(epilogue_);

    asm_.Bind(hle_error_exit_);
    // x9 holds the HLE helper status when control reaches here.
    CtxStore(kT0, P5EMU_A64_CTX_FAULT_OFFSET);
    asm_.MovImm64(kT0, P5EMU_A64_REASON_HLE_ERROR);
    CtxStore(kT0, P5EMU_A64_CTX_STATUS_OFFSET);
    asm_.B(epilogue_);

    asm_.Bind(epilogue_);
    // Return value: termination reason.
    CtxLoad(0, P5EMU_A64_CTX_STATUS_OFFSET);
    Raw(arm64::enc::AddSubImm(true, false, kSp, 29, 16, false));    // sub sp, x29, #16
    Raw(arm64::enc::LdrU64(kCtx, kSp, 0));                          // restore x19
    Raw(arm64::enc::AddSubImm(false, false, kSp, 29, 0, false));    // mov sp, x29
    Raw(arm64::enc::LdpU64Post(29, 30, kSp, 2));                    // ldp x29,x30,[sp],#16
    Raw(arm64::enc::Ret(30));

    if (!asm_.Finalize(&out_->code)) {
      return StatusCode::kInternal;  // an unbound label would be a codegen bug
    }
    out_->frame_bytes = frame_bytes_;
    out_->value_count = value_count_;
    out_->status = StatusCode::kOk;
    return StatusCode::kOk;
  }

  [[nodiscard]] const std::string& reason() const noexcept { return reason_; }

 private:
  // -- diagnostics --------------------------------------------------------
  Status Unsupported(const char* what) {
    reason_ = what;
    out_->status = StatusCode::kUnimplementedInstruction;
    return StatusCode::kUnimplementedInstruction;
  }
  Status UnsupportedOp(const ir::Op& op, ir::BlockId b) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "unsupported IR op '%s' in block %u",
                  ir::OpKindName(op.kind), static_cast<unsigned>(b));
    reason_ = buf;
    out_->status = StatusCode::kUnimplementedInstruction;
    return StatusCode::kUnimplementedInstruction;
  }

  // -- layout -------------------------------------------------------------
  std::uint64_t MaxValueId() const {
    std::uint64_t m = 0;
    for (const ir::Block& block : program_.blocks) {
      for (const ir::Op& op : block.ops) {
        if (op.dest != ir::kInvalidValue) {
          m = (op.dest > m) ? op.dest : m;
        }
        for (const ir::ValueId a : op.args) {
          m = (a > m) ? a : m;
        }
      }
      if (block.terminator.cond_value != ir::kInvalidValue) {
        m = (block.terminator.cond_value > m) ? block.terminator.cond_value : m;
      }
    }
    return m;
  }

  bool BuildBlockLabels() {
    std::uint64_t max_block = 0;
    for (const ir::Block& block : program_.blocks) {
      max_block = (block.id > max_block) ? block.id : max_block;
    }
    block_labels_.assign(static_cast<std::size_t>(max_block) + 1u, -1);
    return true;
  }

  // -- primitive emit -----------------------------------------------------
  void Raw(std::uint32_t word) { asm_.Raw(word); }

  void LdrOff(Reg rt, Reg rn, std::uint64_t off) {
    if (off % 8u != 0u || off >= 32768u) {
      // Callers pre-check the frame size, so this cannot happen in practice.
      at_ = false;
      return;
    }
    Raw(arm64::enc::LdrU64(rt, rn, static_cast<unsigned>(off)));
  }
  void StrOff(Reg rt, Reg rn, std::uint64_t off) {
    if (off % 8u != 0u || off >= 32768u) {
      at_ = false;
      return;
    }
    Raw(arm64::enc::StrU64(rt, rn, static_cast<unsigned>(off)));
  }

  void SlotLoad(Reg rt, ir::ValueId v) { LdrOff(rt, kSp, SlotOff(v)); }
  void SlotStore(Reg rt, ir::ValueId v) { StrOff(rt, kSp, SlotOff(v)); }
  std::uint64_t SlotOff(ir::ValueId v) const {
    return 16u * static_cast<std::uint64_t>(v);
  }

  void CtxLoad(Reg rt, std::uint64_t off) { LdrOff(rt, kCtx, off); }
  void CtxStore(Reg rt, std::uint64_t off) { StrOff(rt, kCtx, off); }

  void AddSpImm(Reg dst, Reg base, std::uint64_t off) {
    // Handles base == SP, which the shifted-register ADD cannot.
    std::uint64_t rem = off;
    bool first = true;
    while (rem > 0) {
      const std::uint32_t chunk =
          (rem > 4095u) ? 4095u : static_cast<std::uint32_t>(rem);
      Raw(arm64::enc::AddSubImm(false, false, dst, first ? base : dst, chunk,
                                false));
      rem -= chunk;
      first = false;
    }
    if (first) {
      Raw(arm64::enc::AddSubImm(false, false, dst, base, 0, false));
    }
  }

  void SubSpImm(std::uint64_t n) {
    const std::uint32_t lo = static_cast<std::uint32_t>(n & 0xFFFu);
    const std::uint32_t hi = static_cast<std::uint32_t>(n >> 12);
    if (lo != 0u) {
      Raw(arm64::enc::AddSubImm(true, false, kSp, kSp, lo, false));
    }
    if (hi != 0u) {
      Raw(arm64::enc::AddSubImm(true, false, kSp, kSp, hi, true));
    }
  }

  // Boolean helpers on 0/1 values in registers.
  void NotBit(Reg dst, Reg src) {
    Raw(arm64::enc::AddSubImm(true, true, kSp, src, 1, false));  // subs xzr,src,#1
    Raw(arm64::enc::Cset(dst, Cond::kNe));                       // 1 if src == 0
  }
  void XorBits(Reg dst, Reg a, Reg b) {
    Raw(arm64::enc::LogicalReg(2, dst, a, b, 0, 0));
  }
  void OrBits(Reg dst, Reg a, Reg b) {
    Raw(arm64::enc::LogicalReg(1, dst, a, b, 0, 0));
  }
  void AndBits(Reg dst, Reg a, Reg b) {
    Raw(arm64::enc::LogicalReg(0, dst, a, b, 0, 0));
  }

  void EmitPrologue() {
    // Save the frame pointer / link register, then the one callee-saved register
    // we use (x19 holds the context pointer). AAPCS64 requires a callee to
    // preserve x19-x28, so failing to save x19 would corrupt the caller.
    Raw(arm64::enc::StpU64Pre(29, 30, kSp, -2));                  // stp x29,x30,[sp,#-16]!
    Raw(arm64::enc::AddSubImm(false, false, 29, kSp, 0, false));  // add x29, sp, #0
    Raw(arm64::enc::StpU64Pre(kCtx, kCtx, kSp, -2));              // stp x19,x19,[sp,#-16]!
    SubSpImm(frame_bytes_);
    Raw(arm64::enc::MovReg64(kCtx, 0));  // mov x19, x0
  }

  // -- operand access -----------------------------------------------------
  Status LoadRegisterValue(ir::ArchRegister r, Reg rt) {
    const std::uint64_t off = CtxOffsetForRegister(r);
    if (off == kInvalidOff) {
      return Unsupported("invalid architectural register in IR");
    }
    CtxLoad(rt, off);
    return StatusCode::kOk;
  }

  // -- ops ----------------------------------------------------------------
  Status EmitOp(const ir::Op& op, ir::BlockId bid) {
    switch (op.kind) {
      case ir::OpKind::kNop:
        break;

      case ir::OpKind::kConst:
        asm_.MovImm64(kT0, op.imm);
        SlotStore(kT0, op.dest);
        break;

      case ir::OpKind::kGetReg: {
        const Status st = LoadRegisterValue(op.reg, kT0);
        if (!st.ok()) return st;
        // A narrow read observes only the low bits (AL is RAX[7:0]).
        const std::uint32_t w = NormWidth(op.width);
        if (w < 8) {
          asm_.MovImm64(kMask, WidthMask(op.width));
          Raw(arm64::enc::LogicalReg(0, kT0, kT0, kMask, 0, 0));  // and
        }
        SlotStore(kT0, op.dest);
        break;
      }

      case ir::OpKind::kSetReg: {
        SlotLoad(kT0, op.args[0]);
        const std::uint64_t off = CtxOffsetForRegister(op.reg);
        if (off == kInvalidOff) {
          return Unsupported("invalid architectural register in IR");
        }
        const std::uint32_t w = NormWidth(op.width);
        if (w == 8) {
          CtxStore(kT0, off);  // RAX: replace all 64 bits
        } else if (w == 4) {
          if (!options_.inject_no_zero_extend) {
            Raw(arm64::enc::MovReg32(kT0, kT0));  // EAX: zero-extend into RAX
          }
          CtxStore(kT0, off);
        } else {
          // AL/AX: preserve the upper bits (read-modify-write).
          CtxLoad(kT1, off);
          asm_.MovImm64(kMask, WidthMask(op.width));
          Raw(arm64::enc::LogicalReg(0, kT0, kT0, kMask, 0, 0));  // v & M
          asm_.MovImm64(kMask, ~WidthMask(op.width));
          Raw(arm64::enc::LogicalReg(0, kT1, kT1, kMask, 0, 0));  // old & ~M
          Raw(arm64::enc::LogicalReg(1, kT0, kT1, kT0, 0, 0));    // merge
          CtxStore(kT0, off);
        }
        break;
      }

      case ir::OpKind::kLoad: {
        // x0 = mem_handle, x1 = addr, x2 = width, x3 = &status
        CtxLoad(0, P5EMU_A64_CTX_MEM_OFFSET);
        SlotLoad(1, op.args[0]);
        asm_.MovImm64(2, op.width);
        AddSpImm(3, kSp, status_slot_off_);
        CtxLoad(4, P5EMU_A64_CTX_FN_LOAD_OFFSET);
        Raw(arm64::enc::Blr(4));
        LdrOff(kT0, kSp, status_slot_off_);
        asm_.Cbnz(kT0, fault_exit_);
        SlotStore(0, op.dest);  // loaded value returned in x0
        break;
      }

      case ir::OpKind::kStore: {
        // x0 = mem_handle, x1 = addr, x2 = width, x3 = value, x4 = &status
        CtxLoad(0, P5EMU_A64_CTX_MEM_OFFSET);
        SlotLoad(1, op.args[0]);
        asm_.MovImm64(2, op.width);
        SlotLoad(3, op.args[1]);
        AddSpImm(4, kSp, status_slot_off_);
        CtxLoad(5, P5EMU_A64_CTX_FN_STORE_OFFSET);
        Raw(arm64::enc::Blr(5));
        LdrOff(kT0, kSp, status_slot_off_);
        asm_.Cbnz(kT0, fault_exit_);
        break;
      }

      case ir::OpKind::kAdd:
        SlotLoad(kT0, op.args[0]);
        SlotLoad(kT1, op.args[1]);
        Raw(arm64::enc::AddSubReg(options_.inject_add_sub_swap, false, kT2, kT0,
                                  kT1, 0, 0));
        SlotStore(kT2, op.dest);
        break;
      case ir::OpKind::kSub:
        SlotLoad(kT0, op.args[0]);
        SlotLoad(kT1, op.args[1]);
        Raw(arm64::enc::AddSubReg(!options_.inject_add_sub_swap, false, kT2, kT0,
                                  kT1, 0, 0));
        SlotStore(kT2, op.dest);
        break;
      case ir::OpKind::kAnd:
      case ir::OpKind::kOr:
      case ir::OpKind::kXor: {
        const unsigned opc = (op.kind == ir::OpKind::kAnd)   ? 0u
                             : (op.kind == ir::OpKind::kOr)  ? 1u
                                                             : 2u;
        SlotLoad(kT0, op.args[0]);
        SlotLoad(kT1, op.args[1]);
        Raw(arm64::enc::LogicalReg(opc, kT2, kT0, kT1, 0, 0));
        SlotStore(kT2, op.dest);
        break;
      }

      case ir::OpKind::kAddFlags:
        EmitArithFlags(false, op.args[0], op.args[1], op.dest, op.args[2],
                       op.width);
        break;
      case ir::OpKind::kSubFlags:
        EmitArithFlags(true, op.args[0], op.args[1], op.dest, op.args[2],
                       op.width);
        break;
      case ir::OpKind::kAndFlags:
        EmitLogicFlags(0, op.args[0], op.args[1], op.dest, op.args[2],
                       op.width);
        break;
      case ir::OpKind::kOrFlags:
        EmitLogicFlags(1, op.args[0], op.args[1], op.dest, op.args[2],
                       op.width);
        break;
      case ir::OpKind::kXorFlags:
        EmitLogicFlags(2, op.args[0], op.args[1], op.dest, op.args[2],
                       op.width);
        break;

      case ir::OpKind::kCmp:
        EmitArithFlags(true, op.args[0], op.args[1], ir::kInvalidValue,
                       op.args[2], op.width);
        break;
      case ir::OpKind::kTest:
        EmitLogicFlags(0, op.args[0], op.args[1], ir::kInvalidValue, op.args[2],
                       op.width);
        break;

      case ir::OpKind::kShl:
      case ir::OpKind::kShr:
      case ir::OpKind::kSar:
        EmitShift(op);
        break;

      case ir::OpKind::kLoadFlags:
        CtxLoad(kT0, P5EMU_A64_CTX_EFLAGS_OFFSET);
        SlotStore(kT0, op.dest);
        break;
      case ir::OpKind::kStoreFlags:
        SlotLoad(kT0, op.args[0]);
        CtxStore(kT0, P5EMU_A64_CTX_EFLAGS_OFFSET);
        break;
      case ir::OpKind::kTestCc: {
        SlotLoad(kT0, op.args[0]);
        EmitConditionToBit(op.cond, kT0, kT1);
        SlotStore(kT1, op.dest);
        break;
      }

      case ir::OpKind::kExtend: {
        SlotLoad(kT0, op.args[0]);
        switch (op.mode) {
          case ir::ExtendMode::kNone:
            break;
          case ir::ExtendMode::kZeroExtend32:
            Raw(arm64::enc::MovReg32(kT0, kT0));  // writing W zero-extends to X
            break;
          case ir::ExtendMode::kSignExtend8:
            Raw(arm64::enc::Sxtb(kT0, kT0));
            break;
          case ir::ExtendMode::kSignExtend16:
            Raw(arm64::enc::Sxth(kT0, kT0));
            break;
          case ir::ExtendMode::kSignExtend32:
            Raw(arm64::enc::Sxtw(kT0, kT0));
            break;
        }
        SlotStore(kT0, op.dest);
        break;
      }

      case ir::OpKind::kTruncate: {
        SlotLoad(kT0, op.args[0]);
        if (op.width < 64u) {
          const std::uint64_t mask =
              (op.width >= 64u) ? ~std::uint64_t{0}
                                : ((std::uint64_t{1} << op.width) - 1u);
          asm_.MovImm64(kT1, mask);
          Raw(arm64::enc::LogicalReg(0, kT0, kT0, kT1, 0, 0));  // and
        }
        SlotStore(kT0, op.dest);
        break;
      }

      case ir::OpKind::kAddrCalc: {
        // addr = imm + sign_extend(imm_high) + index * width
        const std::uint64_t base =
            op.imm + static_cast<std::uint64_t>(
                         static_cast<std::int64_t>(op.imm_high));
        asm_.MovImm64(kT2, base);
        if (!op.args.empty()) {
          SlotLoad(kT1, op.args[0]);
          unsigned lsl = 0;
          switch (op.width) {
            case 1: lsl = 0; break;
            case 2: lsl = 1; break;
            case 4: lsl = 2; break;
            case 8: lsl = 3; break;
            default: return UnsupportedOp(op, bid);
          }
          Raw(arm64::enc::AddSubReg(false, false, kT2, kT2, kT1, 0, lsl));
        }
        SlotStore(kT2, op.dest);
        break;
      }

      default:
        return UnsupportedOp(op, bid);
    }
    if (!at_) {
      return Unsupported("value frame too large for the Phase-2 addressing");
    }
    return StatusCode::kOk;
  }

  // ZF, SF, PF from the result in kT2 at the given width (bits = 8*width).
  // Writes kT5=zf, kT6=sf, kT7=pf. Clobbers native flags.
  void EmitZfSfPfAf(unsigned bits) {
    Raw(arm64::enc::AddSubImm(true, true, kSp, kT2, 0, false));  // cmp kT2,#0
    Raw(arm64::enc::Cset(kT5, Cond::kEq));                       // ZF
    if (options_.inject_clear_zf) {
      Raw(arm64::enc::Movz(kT5, 0, 0));  // test-only defect: ZF forced to 0
    }
    Raw(arm64::enc::LsrImm(kT6, kT2, bits - 1u));  // SF = bit (bits-1)
    Raw(arm64::enc::Ubfx(kT6, kT6, 0, 1));
    Raw(arm64::enc::LogicalReg(2, kT7, kT2, kT2, 1, 4));  // PF fold
    Raw(arm64::enc::LogicalReg(2, kT7, kT7, kT7, 1, 2));
    Raw(arm64::enc::LogicalReg(2, kT7, kT7, kT7, 1, 1));
    Raw(arm64::enc::Ubfx(kT7, kT7, 0, 1));
    Raw(arm64::enc::AddSubImm(true, true, kSp, kT7, 1, false));
    Raw(arm64::enc::Cset(kT7, Cond::kNe));  // PF = 1 if even
  }

  // AF = bit 4 of (a ^ b ^ result). Sets kT8.
  void EmitAf() {
    Raw(arm64::enc::LogicalReg(2, kT8, kT0, kT1, 0, 0));
    Raw(arm64::enc::LogicalReg(2, kT8, kT8, kT2, 0, 0));
    Raw(arm64::enc::Ubfx(kT8, kT8, kAfBit, 1));
  }

  // Accumulates kT3(cf),kT4(of),kT5(zf),kT6(sf),kT7(pf),kT8(af) into the EFLAGS
  // value and stores it. Uses kT0 as the accumulator.
  void AccumulateFlags(ir::ValueId flags_out) {
    Raw(arm64::enc::LogicalReg(1, kT0, kSp, kT3, 0, kCfBit));  // orr kT0,xzr,cf
    Raw(arm64::enc::LogicalReg(1, kT0, kT0, kT4, 0, kOfBit));
    Raw(arm64::enc::LogicalReg(1, kT0, kT0, kT5, 0, kZfBit));
    Raw(arm64::enc::LogicalReg(1, kT0, kT0, kT6, 0, kSfBit));
    Raw(arm64::enc::LogicalReg(1, kT0, kT0, kT7, 0, kPfBit));
    Raw(arm64::enc::LogicalReg(1, kT0, kT0, kT8, 0, kAfBit));
    SlotStore(kT0, flags_out);
  }

  // result -> kT2, flags -> flags_out. Width-aware for ADD/SUB/CMP.
  void EmitArithFlags(bool is_sub, ir::ValueId a, ir::ValueId b,
                      ir::ValueId dest, ir::ValueId flags_out,
                      std::uint32_t width) {
    const std::uint32_t w = NormWidth(width);
    const unsigned bits = WidthBits(width);
    SlotLoad(kT0, a);
    SlotLoad(kT1, b);
    const bool sub = is_sub ^ options_.inject_add_sub_swap;

    if (w == 8) {
      // 64-bit: the native AArch64 carry and overflow flags ARE x86 CF/OF.
      Raw(arm64::enc::AddSubReg(sub, true, kT2, kT0, kT1, 0, 0));  // adds/subs
      Raw(arm64::enc::Cset(kT3, sub ? Cond::kCc : Cond::kCs));     // CF
      Raw(arm64::enc::Cset(kT4, Cond::kVs));                       // OF
    } else {
      // Narrow: compute the flags at bit (8*width - 1) explicitly. A native
      // 64-bit ADDS would set carry from bit 63, which is the wrong bit.
      asm_.MovImm64(kMask, WidthMask(width));
      Raw(arm64::enc::LogicalReg(0, kT0, kT0, kMask, 0, 0));  // a &= M
      Raw(arm64::enc::LogicalReg(0, kT1, kT1, kMask, 0, 0));  // b &= M
      if (!sub) {
        Raw(arm64::enc::AddSubReg(false, false, kT2, kT0, kT1, 0, 0));  // s=a+b
        Raw(arm64::enc::LsrImm(kT3, kT2, bits));
        Raw(arm64::enc::Ubfx(kT3, kT3, 0, 1));                // CF = (s>>bits)&1
        Raw(arm64::enc::LogicalReg(0, kT2, kT2, kMask, 0, 0));  // result = s & M
      } else {
        Raw(arm64::enc::AddSubReg(true, false, kT2, kT0, kT1, 0, 0));  // d=a-b
        Raw(arm64::enc::LogicalReg(0, kT2, kT2, kMask, 0, 0));         // result
        Raw(arm64::enc::AddSubReg(true, true, kSp, kT0, kT1, 0, 0));   // SUBS
        Raw(arm64::enc::Cset(kT3, Cond::kCc));                         // CF
      }
      // OF = (cond ? ~(sa^sb) : (sa^sb)) & (sr ^ sa), from the sign bits.
      Raw(arm64::enc::Ubfx(kT5, kT0, bits - 1u, 1));  // sa
      Raw(arm64::enc::Ubfx(kT6, kT1, bits - 1u, 1));  // sb
      Raw(arm64::enc::LogicalReg(2, kT6, kT5, kT6, 0, 0));  // sa^sb
      Raw(arm64::enc::Ubfx(kT5, kT2, bits - 1u, 1));        // sr
      Raw(arm64::enc::Ubfx(kT4, kT0, bits - 1u, 1));        // sa again
      Raw(arm64::enc::LogicalReg(2, kT5, kT5, kT4, 0, 0));  // sr^sa
      if (!sub) {
        NotBit(kT6, kT6);  // add: ~(sa^sb)
      }
      Raw(arm64::enc::LogicalReg(0, kT4, kT6, kT5, 0, 0));  // OF
    }

    EmitZfSfPfAf(bits);
    EmitAf();
    if (dest != ir::kInvalidValue) {
      SlotStore(kT2, dest);
    }
    AccumulateFlags(flags_out);
  }

  // AND/OR/XOR/TEST flags: ZF, SF, PF from the result; CF, OF, AF cleared.
  void EmitLogicFlags(unsigned opc, ir::ValueId a, ir::ValueId b,
                      ir::ValueId dest, ir::ValueId flags_out,
                      std::uint32_t width) {
    const std::uint32_t w = NormWidth(width);
    const unsigned bits = WidthBits(width);
    SlotLoad(kT0, a);
    SlotLoad(kT1, b);
    Raw(arm64::enc::LogicalReg(opc, kT2, kT0, kT1, 0, 0));
    if (w < 8) {
      asm_.MovImm64(kMask, WidthMask(width));
      Raw(arm64::enc::LogicalReg(0, kT2, kT2, kMask, 0, 0));
    }
    Raw(arm64::enc::AddSubImm(true, true, kSp, kT2, 0, false));
    Raw(arm64::enc::Cset(kT5, Cond::kEq));                  // ZF
    Raw(arm64::enc::LsrImm(kT6, kT2, bits - 1u));
    Raw(arm64::enc::Ubfx(kT6, kT6, 0, 1));                  // SF
    Raw(arm64::enc::LogicalReg(2, kT7, kT2, kT2, 1, 4));    // PF fold
    Raw(arm64::enc::LogicalReg(2, kT7, kT7, kT7, 1, 2));
    Raw(arm64::enc::LogicalReg(2, kT7, kT7, kT7, 1, 1));
    Raw(arm64::enc::Ubfx(kT7, kT7, 0, 1));
    Raw(arm64::enc::AddSubImm(true, true, kSp, kT7, 1, false));
    Raw(arm64::enc::Cset(kT7, Cond::kNe));                  // PF
    Raw(arm64::enc::LogicalReg(1, kT0, kSp, kT5, 0, kZfBit));
    Raw(arm64::enc::LogicalReg(1, kT0, kT0, kT6, 0, kSfBit));
    Raw(arm64::enc::LogicalReg(1, kT0, kT0, kT7, 0, kPfBit));
    if (dest != ir::kInvalidValue) {
      SlotStore(kT2, dest);
    }
    SlotStore(kT0, flags_out);
  }

  // SHL/SHR/SAR at a given operand width, with x86 count masking, result, and
  // flags. OF is architecturally undefined except for count == 1; the emitted
  // code produces a deterministic 0 there and the fixture's observable mask
  // excludes it, so no invented behaviour is asserted.
  void EmitShift(const ir::Op& op) {
    const std::uint32_t w = NormWidth(op.width);
    const unsigned bits = WidthBits(op.width);
    const std::uint64_t m = WidthMask(op.width);
    const bool is_shl = op.kind == ir::OpKind::kShl;
    const bool is_sar = op.kind == ir::OpKind::kSar;

    SlotLoad(kT0, op.args[0]);  // value
    SlotLoad(kT1, op.args[1]);  // count
    asm_.MovImm64(kMask, m);
    Raw(arm64::enc::LogicalReg(0, kT0, kT0, kMask, 0, 0));       // v &= M
    const unsigned count_mask_bits =
        (options_.inject_wide_shift_count || w == 8) ? 6u : 5u;
    Raw(arm64::enc::Ubfx(kT1, kT1, 0, count_mask_bits));         // mask count

    const int l_zero = asm_.NewLabel();
    const int l_ge = asm_.NewLabel();
    const int l_done = asm_.NewLabel();
    const int l_end = asm_.NewLabel();

    asm_.Cbz(kT1, l_zero);
    if (bits <= 32u) {
      // Only 8- and 16-bit operands can have count >= operand size after
      // masking (32/64-bit counts are masked below their size).
      asm_.MovImm64(kT2, bits);
      Raw(arm64::enc::AddSubReg(true, true, kSp, kT1, kT2, 0, 0));  // cmp
      asm_.BCond(arm64::Cond::kCs, l_ge);
    }

    // ---- 1 <= count < bits ----
    if (is_shl) {
      Raw(arm64::enc::Lslv(kT2, kT0, kT1));
      Raw(arm64::enc::LogicalReg(0, kT2, kT2, kMask, 0, 0));
      asm_.MovImm64(kT5, bits);
      Raw(arm64::enc::AddSubReg(true, false, kT5, kT5, kT1, 0, 0));  // bits-count
      Raw(arm64::enc::Lsrv(kT3, kT0, kT5));
      Raw(arm64::enc::Ubfx(kT3, kT3, 0, 1));  // CF
    } else if (!is_sar) {
      Raw(arm64::enc::Lsrv(kT2, kT0, kT1));
      Raw(arm64::enc::LogicalReg(0, kT2, kT2, kMask, 0, 0));
      Raw(arm64::enc::AddSubImm(true, false, kT5, kT1, 1, false));  // count-1
      Raw(arm64::enc::Lsrv(kT3, kT0, kT5));
      Raw(arm64::enc::Ubfx(kT3, kT3, 0, 1));  // CF
    } else {
      Raw(arm64::enc::Sbfx(kT2, kT0, 0, bits));  // sign-extend v
      Raw(arm64::enc::Asrv(kT2, kT2, kT1));
      Raw(arm64::enc::LogicalReg(0, kT2, kT2, kMask, 0, 0));
      Raw(arm64::enc::AddSubImm(true, false, kT5, kT1, 1, false));  // count-1
      Raw(arm64::enc::Lsrv(kT3, kT0, kT5));
      Raw(arm64::enc::Ubfx(kT3, kT3, 0, 1));  // CF
    }
    // OF for count == 1: is_one & (msb_v ^ msb_r) for SHL, is_one & msb_v for
    // SHR, and 0 for SAR.
    Raw(arm64::enc::AddSubImm(true, true, kSp, kT1, 1, false));  // cmp count,#1
    Raw(arm64::enc::Cset(kT4, arm64::Cond::kEq));                // is_one
    Raw(arm64::enc::Ubfx(kT5, kT0, bits - 1u, 1));               // msb_v
    if (is_sar) {
      Raw(arm64::enc::MovReg64(kT5, kSp));  // SAR OF = 0
    } else {
      Raw(arm64::enc::Ubfx(kT6, kT2, bits - 1u, 1));  // msb_r
      if (is_shl) {
        Raw(arm64::enc::LogicalReg(2, kT5, kT5, kT6, 0, 0));  // msb_v ^ msb_r
      }
    }
    Raw(arm64::enc::LogicalReg(0, kT4, kT4, kT5, 0, 0));  // OF
    asm_.B(l_done);

    // ---- count >= bits (8- and 16-bit only) ----
    asm_.Bind(l_ge);
    if (is_sar) {
      Raw(arm64::enc::Sbfx(kT2, kT0, 0, bits));
      Raw(arm64::enc::Asrv(kT2, kT2, kT1));
      Raw(arm64::enc::LogicalReg(0, kT2, kT2, kMask, 0, 0));
    } else {
      asm_.MovImm64(kT2, 0);
    }
    asm_.MovImm64(kT3, 0);  // CF deterministic 0 (undefined; masked out)
    asm_.MovImm64(kT4, 0);  // OF deterministic 0 (undefined; masked out)
    asm_.B(l_done);

    // ---- count == 0: result unchanged, ALL flags unchanged ----
    asm_.Bind(l_zero);
    SlotStore(kT0, op.dest);
    CtxLoad(kT5, P5EMU_A64_CTX_EFLAGS_OFFSET);
    SlotStore(kT5, op.args[2]);
    asm_.B(l_end);

    // ---- flags tail ----
    asm_.Bind(l_done);
    EmitZfSfPfAf(bits);
    Raw(arm64::enc::LogicalReg(1, kT0, kSp, kT3, 0, kCfBit));
    Raw(arm64::enc::LogicalReg(1, kT0, kT0, kT4, 0, kOfBit));
    Raw(arm64::enc::LogicalReg(1, kT0, kT0, kT5, 0, kZfBit));
    Raw(arm64::enc::LogicalReg(1, kT0, kT0, kT6, 0, kSfBit));
    Raw(arm64::enc::LogicalReg(1, kT0, kT0, kT7, 0, kPfBit));
    SlotStore(kT2, op.dest);
    SlotStore(kT0, op.args[2]);
    asm_.Bind(l_end);
  }

  // Evaluates `cc` from a flags word in `flags_reg`, writing 0/1 to `dst`.
  // Bits: cf=0, zf=6, sf=7, of=11, pf=2.
  void EmitConditionToBit(ir::ConditionCode cc, Reg flags_reg, Reg dst) {
    const Reg cf = kT3;
    const Reg zf = kT5;
    const Reg sf = kT6;
    const Reg of = kT4;
    const Reg pf = kT7;
    Raw(arm64::enc::Ubfx(cf, flags_reg, kCfBit, 1));
    Raw(arm64::enc::Ubfx(zf, flags_reg, kZfBit, 1));
    Raw(arm64::enc::Ubfx(sf, flags_reg, kSfBit, 1));
    Raw(arm64::enc::Ubfx(of, flags_reg, kOfBit, 1));
    Raw(arm64::enc::Ubfx(pf, flags_reg, kPfBit, 1));

    switch (cc) {
      case ir::ConditionCode::kO:  Raw(arm64::enc::MovReg64(dst, of)); break;
      case ir::ConditionCode::kNO: NotBit(dst, of); break;
      case ir::ConditionCode::kB:  Raw(arm64::enc::MovReg64(dst, cf)); break;
      case ir::ConditionCode::kAE: NotBit(dst, cf); break;
      case ir::ConditionCode::kE:  Raw(arm64::enc::MovReg64(dst, zf)); break;
      case ir::ConditionCode::kNE: NotBit(dst, zf); break;
      case ir::ConditionCode::kBE: OrBits(dst, cf, zf); break;
      case ir::ConditionCode::kA:  OrBits(dst, cf, zf); NotBit(dst, dst); break;
      case ir::ConditionCode::kS:  Raw(arm64::enc::MovReg64(dst, sf)); break;
      case ir::ConditionCode::kNS: NotBit(dst, sf); break;
      case ir::ConditionCode::kP:  Raw(arm64::enc::MovReg64(dst, pf)); break;
      case ir::ConditionCode::kNP: NotBit(dst, pf); break;
      case ir::ConditionCode::kL:  XorBits(dst, sf, of); break;
      case ir::ConditionCode::kGE: XorBits(dst, sf, of); NotBit(dst, dst); break;
      case ir::ConditionCode::kLE:
        XorBits(dst, sf, of);
        OrBits(dst, dst, zf);
        break;
      case ir::ConditionCode::kG: {
        const Reg t = kT8;
        XorBits(t, sf, of);
        NotBit(t, t);
        NotBit(dst, zf);
        AndBits(dst, dst, t);
        break;
      }
    }
  }

  // -- terminators --------------------------------------------------------
  Status EmitTerminator(const ir::Block& block) {
    const ir::Terminator& t = block.terminator;
    switch (t.kind) {
      case ir::TerminatorKind::kFallthrough:
        EmitExitWithReason(P5EMU_A64_REASON_FALLTHROUGH);
        break;
      case ir::TerminatorKind::kReturn:
        EmitExitWithReason(P5EMU_A64_REASON_RETURN);
        break;
      case ir::TerminatorKind::kTrap:
        // The interpreter reports kTrap with StatusCode::kUnimplementedInstruction;
        // the backend maps the reason back to that status.
        EmitExitWithReason(P5EMU_A64_REASON_TRAP);
        break;
      case ir::TerminatorKind::kJump:
        if (t.target == ir::kInvalidBlock ||
            static_cast<std::size_t>(t.target) >= block_labels_.size() ||
            block_labels_[t.target] < 0) {
          return Unsupported("jump to an unknown block");
        }
        asm_.B(block_labels_[t.target]);
        break;
      case ir::TerminatorKind::kBranch: {
        if (t.target == ir::kInvalidBlock || t.fallthrough == ir::kInvalidBlock) {
          return Unsupported("branch without both successors");
        }
        SlotLoad(kT0, t.cond_value);
        EmitConditionToBit(t.cond, kT0, kT1);
        asm_.Cbnz(kT1, block_labels_[t.target]);
        asm_.B(block_labels_[t.fallthrough]);
        break;
      }
      case ir::TerminatorKind::kIndirectJump: {
        SlotLoad(kT0, t.cond_value);
        for (const ir::Block& target : program_.blocks) {
          asm_.MovImm64(kT1, target.entry_address);
          Raw(arm64::enc::AddSubReg(true, true, kSp, kT0, kT1, 0, 0));  // cmp
          asm_.BCond(Cond::kEq, block_labels_[target.id]);
        }
        // No matching block: a return out of the program.
        EmitExitWithReason(P5EMU_A64_REASON_RETURN);
        break;
      }
      case ir::TerminatorKind::kExternalCall: {
        // HLE dispatch: call the harness helper with the guest binding address
        // and context pointer. The helper returns the guest return value and
        // writes a status. The binding is a GUEST address; no host function
        // pointer ever crosses into guest-visible state.
        if (t.fallthrough == ir::kInvalidBlock ||
            static_cast<std::size_t>(t.fallthrough) >= block_labels_.size()) {
          return Unsupported("external call without a return block");
        }
        asm_.MovImm64(0, t.external_target);            // x0 = binding address
        Raw(arm64::enc::MovReg64(1, kCtx));             // x1 = ctx
        AddSpImm(2, kSp, status_slot_off_);             // x2 = &status
        CtxLoad(3, P5EMU_A64_CTX_FN_HLE_OFFSET);        // x3 = fn_hle
        Raw(arm64::enc::Blr(3));                        // x0 = return value
        LdrOff(kT0, kSp, status_slot_off_);             // helper status
        asm_.Cbnz(kT0, hle_error_exit_);
        CtxStore(0, P5EMU_A64_CTX_GPR_OFFSET + 0 * 8);  // rax = return value
        // Pop the return address the CALL pushed: rsp += 8.
        CtxLoad(kT0, P5EMU_A64_CTX_GPR_OFFSET + 7 * 8);
        Raw(arm64::enc::AddSubImm(false, false, kT0, kT0, 8, false));
        CtxStore(kT0, P5EMU_A64_CTX_GPR_OFFSET + 7 * 8);
        asm_.B(block_labels_[t.fallthrough]);
        break;
      }
      case ir::TerminatorKind::kInvalid:
      default:
        return Unsupported("invalid terminator");
    }
    return StatusCode::kOk;
  }

  void EmitExitWithReason(std::uint64_t reason) {
    asm_.MovImm64(kT0, reason);
    CtxStore(kT0, P5EMU_A64_CTX_STATUS_OFFSET);
    asm_.B(epilogue_);
  }

  const ir::Program& program_;
  const Arm64CodeGenOptions& options_;
  Arm64CodeGenResult* out_;
  Assembler asm_;
  std::vector<int> block_labels_;
  int epilogue_ = -1;
  int fault_exit_ = -1;
  int step_exit_ = -1;
  int hle_error_exit_ = -1;
  std::uint64_t value_count_ = 0;
  std::uint64_t status_slot_off_ = 0;
  std::uint64_t frame_bytes_ = 0;
  std::string reason_;
  bool at_ = true;  // false if an addressing limit was exceeded
};

}  // namespace

Status GenerateArm64(const ir::Program& program,
                     const Arm64CodeGenOptions& options,
                     Arm64CodeGenResult* out) {
  if (out == nullptr) {
    return StatusCode::kInvalidArgument;
  }
  out->code.clear();
  out->reason.clear();
  Gen gen(program, options, out);
  const Status st = gen.Run();
  out->status = st;
  // Propagate the diagnostic for EVERY failure path, including the early
  // refusals (invalid entry, oversized frame) that do not go through the op loop.
  out->reason = gen.reason();
  return st;
}

}  // namespace p5emu::cpu
