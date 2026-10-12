// SPDX-License-Identifier: Apache-2.0
//
// P5EMU is an independent, open-source project. See LICENSE and
// PROVENANCE.md before adding code here.

#include "p5emu/cpu/Ir.h"

#include <cstdio>

namespace p5emu::ir {

// ---------------------------------------------------------------------------
// Type helpers
// ---------------------------------------------------------------------------
std::uint32_t TypeWidthBits(ValueType type) noexcept {
  switch (type) {
    case ValueType::kI8:
      return 8;
    case ValueType::kI16:
      return 16;
    case ValueType::kI32:
      return 32;
    case ValueType::kI64:
      return 64;
    case ValueType::kFlags:
    case ValueType::kV128:
    case ValueType::kF32:
    case ValueType::kF64:
    case ValueType::kInvalid:
      break;
  }
  return 0;
}

std::uint32_t TypeArchRegister(ValueType type) noexcept {
  // x86 architectural registers are 64 bits wide. I8/I16/I32 are *views* onto
  // the low bits of a register, not registers in their own right. F32/F64/V128
  // are not modelled in Phase 1.
  return (type == ValueType::kI64) ? 64u : 0u;
}

const char* TypeName(ValueType type) noexcept {
  switch (type) {
    case ValueType::kInvalid:
      return "invalid";
    case ValueType::kI8:
      return "i8";
    case ValueType::kI16:
      return "i16";
    case ValueType::kI32:
      return "i32";
    case ValueType::kI64:
      return "i64";
    case ValueType::kFlags:
      return "flags";
    case ValueType::kV128:
      return "v128";
    case ValueType::kF32:
      return "f32";
    case ValueType::kF64:
      return "f64";
  }
  return "unknown";
}

const char* ConditionCodeName(ConditionCode cc) noexcept {
  switch (cc) {
    case ConditionCode::kO:  return "o";
    case ConditionCode::kNO: return "no";
    case ConditionCode::kB:  return "b";
    case ConditionCode::kAE: return "ae";
    case ConditionCode::kE:  return "e";
    case ConditionCode::kNE: return "ne";
    case ConditionCode::kBE: return "be";
    case ConditionCode::kA:  return "a";
    case ConditionCode::kS:  return "s";
    case ConditionCode::kNS: return "ns";
    case ConditionCode::kP:  return "p";
    case ConditionCode::kNP: return "np";
    case ConditionCode::kL:  return "l";
    case ConditionCode::kGE: return "ge";
    case ConditionCode::kLE: return "le";
    case ConditionCode::kG:  return "g";
  }
  return "?";
}

std::uint32_t ArchRegisterNumber(ArchRegister reg) noexcept {
  // Architectural encoding 0..15. kRsp is 4 and kRbp is 5, matching x86-64.
  if (reg >= ArchRegister::kRax && reg <= ArchRegister::kR15) {
    return static_cast<std::uint32_t>(reg) -
           static_cast<std::uint32_t>(ArchRegister::kRax);
  }
  // High-byte views alias RAX/RCX/RDX/RBX (numbers 0..3).
  switch (reg) {
    case ArchRegister::kAh: return 0;
    case ArchRegister::kCh: return 1;
    case ArchRegister::kDh: return 2;
    case ArchRegister::kBh: return 3;
    default: break;
  }
  return 0xffu;
}

bool IsXmmRegister(ArchRegister reg) noexcept {
  return reg >= ArchRegister::kXmm0 && reg <= ArchRegister::kXmm15;
}

std::uint32_t XmmRegisterIndex(ArchRegister reg) noexcept {
  if (!IsXmmRegister(reg)) {
    return 0xFFFFFFFFu;
  }
  return static_cast<std::uint32_t>(reg) -
         static_cast<std::uint32_t>(ArchRegister::kXmm0);
}

const char* ArchRegisterName(ArchRegister reg) noexcept {
  {
    static const char* const kXmmNames[16] = {
        "xmm0",  "xmm1",  "xmm2",  "xmm3",  "xmm4",  "xmm5",  "xmm6",  "xmm7",
        "xmm8",  "xmm9",  "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15"};
    if (IsXmmRegister(reg)) {
      return kXmmNames[XmmRegisterIndex(reg)];
    }
  }
  switch (reg) {
    case ArchRegister::kInvalid:        return "<invalid>";
    case ArchRegister::kRax:            return "rax";
    case ArchRegister::kRcx:            return "rcx";
    case ArchRegister::kRdx:            return "rdx";
    case ArchRegister::kRbx:            return "rbx";
    case ArchRegister::kRsp:            return "rsp";
    case ArchRegister::kRbp:            return "rbp";
    case ArchRegister::kRsi:            return "rsi";
    case ArchRegister::kRdi:            return "rdi";
    case ArchRegister::kR8:             return "r8";
    case ArchRegister::kR9:             return "r9";
    case ArchRegister::kR10:            return "r10";
    case ArchRegister::kR11:            return "r11";
    case ArchRegister::kR12:            return "r12";
    case ArchRegister::kR13:            return "r13";
    case ArchRegister::kR14:            return "r14";
    case ArchRegister::kR15:            return "r15";
    case ArchRegister::kRip:            return "rip";
    case ArchRegister::kEflags:         return "eflags";
    case ArchRegister::kAh:             return "ah";
    case ArchRegister::kCh:             return "ch";
    case ArchRegister::kDh:             return "dh";
    case ArchRegister::kBh:             return "bh";
    case ArchRegister::kInvalidSentinel:return "<sentinel>";
    // XMM registers are handled by the early return above; listed as default so
    // the switch stays exhaustive without repeating 16 cases.
    default: break;
  }
  return "<unknown>";
}

const char* TerminatorKindName(TerminatorKind kind) noexcept {
  switch (kind) {
    case TerminatorKind::kInvalid:     return "invalid";
    case TerminatorKind::kFallthrough: return "fallthrough";
    case TerminatorKind::kJump:        return "jump";
    case TerminatorKind::kBranch:      return "branch";
    case TerminatorKind::kIndirectJump: return "indirect_jump";
    case TerminatorKind::kExternalCall: return "external_call";
    case TerminatorKind::kReturn:      return "return";
    case TerminatorKind::kTrap:        return "trap";
  }
  return "unknown";
}

const char* OpKindName(OpKind kind) noexcept {
  switch (kind) {
    case OpKind::kInvalid:     return "invalid";
    case OpKind::kConst:       return "const";
    case OpKind::kGetReg:      return "getreg";
    case OpKind::kSetReg:      return "setreg";
    case OpKind::kLoad:        return "load";
    case OpKind::kStore:       return "store";
    case OpKind::kAdd:         return "add";
    case OpKind::kSub:         return "sub";
    case OpKind::kAnd:         return "and";
    case OpKind::kOr:          return "or";
    case OpKind::kXor:         return "xor";
    case OpKind::kAddFlags:    return "add_flags";
    case OpKind::kSubFlags:    return "sub_flags";
    case OpKind::kAndFlags:    return "and_flags";
    case OpKind::kOrFlags:     return "or_flags";
    case OpKind::kXorFlags:    return "xor_flags";
    case OpKind::kLoadFlags:   return "load_flags";
    case OpKind::kStoreFlags:  return "store_flags";
    case OpKind::kTest:        return "test";
    case OpKind::kCmp:         return "cmp";
    case OpKind::kShl:         return "shl";
    case OpKind::kShr:         return "shr";
    case OpKind::kSar:         return "sar";
    case OpKind::kExtend:      return "extend";
    case OpKind::kTruncate:    return "truncate";
    case OpKind::kAdcFlags:    return "adc_flags";
    case OpKind::kSbbFlags:    return "sbb_flags";
    case OpKind::kMulFlags:    return "mul_flags";
    case OpKind::kIMulFlags:   return "imul_flags";
    case OpKind::kDivFlags:    return "div_flags";
    case OpKind::kIDivFlags:   return "idiv_flags";
    case OpKind::kGetXmm:      return "get_xmm";
    case OpKind::kSetXmm:      return "set_xmm";
    case OpKind::kVecMake:     return "vec_make";
    case OpKind::kVecExtractLow64: return "vec_extract_low64";
    case OpKind::kVecLoad:     return "vec_load";
    case OpKind::kVecStore:    return "vec_store";
    case OpKind::kVecXor:      return "vec_xor";
    case OpKind::kVecAnd:      return "vec_and";
    case OpKind::kVecOr:       return "vec_or";
    case OpKind::kVecAdd64x2:  return "vec_add64x2";
    case OpKind::kAddrCalc:    return "addr_calc";
    case OpKind::kTestCc:      return "test_cc";
    case OpKind::kNop:         return "nop";
  }
  return "unknown";
}

const char* ExtendModeName(ExtendMode mode) noexcept {
  switch (mode) {
    case ExtendMode::kNone:         return "none";
    case ExtendMode::kZeroExtend32: return "zext32";
    case ExtendMode::kSignExtend8:  return "sext8";
    case ExtendMode::kSignExtend16: return "sext16";
    case ExtendMode::kSignExtend32: return "sext32";
  }
  return "unknown";
}

// ---------------------------------------------------------------------------
// Program accessors
// ---------------------------------------------------------------------------
const Block* Program::FindBlock(BlockId id) const noexcept {
  for (const Block& b : blocks) {
    if (b.id == id) {
      return &b;
    }
  }
  return nullptr;
}

Block* Program::FindBlock(BlockId id) noexcept {
  for (Block& b : blocks) {
    if (b.id == id) {
      return &b;
    }
  }
  return nullptr;
}

const Block* Program::FindBlockByAddress(memory::GuestAddr address) const noexcept {
  for (const Block& b : blocks) {
    if (b.entry_address == address) {
      return &b;
    }
  }
  return nullptr;
}

Block* Program::FindBlockByAddress(memory::GuestAddr address) noexcept {
  for (Block& b : blocks) {
    if (b.entry_address == address) {
      return &b;
    }
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// Deterministic dump
// ---------------------------------------------------------------------------
namespace {

void AppendRole(std::string* out, OperandRole role) {
  switch (role) {
    case OperandRole::kDest:      *out += "dest";      return;
    case OperandRole::kSrc:       *out += "src";       return;
    case OperandRole::kSrc2:      *out += "src2";      return;
    case OperandRole::kSrc3:      *out += "src3";      return;
    case OperandRole::kFlags:     *out += "flags";     return;
    case OperandRole::kFlagsOut:  *out += "flags_out"; return;
    case OperandRole::kReg:       *out += "reg";       return;
    case OperandRole::kWidth:     *out += "width";     return;
    case OperandRole::kMode:      *out += "mode";      return;
    case OperandRole::kCond:      *out += "cond";      return;
    case OperandRole::kAddr:      *out += "addr";      return;
    case OperandRole::kImm:      *out += "imm";       return;
    case OperandRole::kSImm:     *out += "simm";      return;
    case OperandRole::kScale:    *out += "scale";     return;
  }
  *out += "?";
}

}  // namespace

std::string Dump(const Program& program) {
  std::string out;
  char line[512];

  std::snprintf(line, sizeof(line), "program %s\n", program.name.c_str());
  out += line;
  std::snprintf(line, sizeof(line), "entry %u blocks %zu\n",
                static_cast<unsigned>(program.entry), program.blocks.size());
  out += line;

  // Iterate in stored order, not by id lookup, so the dump is deterministic
  // regardless of how ids were assigned.
  for (const Block& block : program.blocks) {
    std::snprintf(line, sizeof(line), "\nblock %u @0x%llx\n",
                  static_cast<unsigned>(block.id),
                  static_cast<unsigned long long>(block.entry_address));
    out += line;

    for (std::size_t i = 0; i < block.ops.size(); ++i) {
      const Op& op = block.ops[i];
      out += "  ";
      std::snprintf(line, sizeof(line), "%04zu: %-12s",
                    i, OpKindName(op.kind));
      out += line;

      if (op.dest != kInvalidValue) {
        std::snprintf(line, sizeof(line), " v%u =", static_cast<unsigned>(op.dest));
        out += line;
      } else {
        out += "  ";
      }

      std::snprintf(line, sizeof(line), " [0x%llx]", static_cast<unsigned long long>(
                                              op.source_address));
      out += line;

      for (std::size_t a = 0; a < op.args.size(); ++a) {
        out += " ";
        out += (a < op.roles.size()) ? "?" : "?";
        AppendRole(&out, a < op.roles.size() ? op.roles[a] : OperandRole::kSrc);
        std::snprintf(line, sizeof(line), "=v%u", static_cast<unsigned>(op.args[a]));
        out += line;
      }
      if (op.reg != ArchRegister::kInvalid) {
        std::snprintf(line, sizeof(line), " reg=%s", ArchRegisterName(op.reg));
        out += line;
      }
      if (op.width != 0) {
        std::snprintf(line, sizeof(line), " width=%u", op.width);
        out += line;
      }
      if (op.mode != ExtendMode::kNone) {
        std::snprintf(line, sizeof(line), " mode=%s", ExtendModeName(op.mode));
        out += line;
      }
      if (op.kind == OpKind::kAddrCalc) {
        std::snprintf(line, sizeof(line), " scale=%u disp=%lld",
                      op.width,
                      static_cast<long long>(static_cast<std::int64_t>(op.imm_high)));
        out += line;
      } else if (op.kind == OpKind::kConst) {
        std::snprintf(line, sizeof(line), " imm=0x%llx",
                      static_cast<unsigned long long>(op.imm));
        out += line;
      }
      if (op.kind == OpKind::kTestCc) {
        std::snprintf(line, sizeof(line), " cc=%s", ConditionCodeName(op.cond));
        out += line;
      }
      out += "\n";
    }

    out += "  term: ";
    out += TerminatorKindName(block.terminator.kind);
    if (block.terminator.kind == TerminatorKind::kBranch) {
      std::snprintf(line, sizeof(line), " %s v%u -> b%u else b%u",
                    ConditionCodeName(block.terminator.cond),
                    static_cast<unsigned>(block.terminator.cond_value),
                    static_cast<unsigned>(block.terminator.target),
                    static_cast<unsigned>(block.terminator.fallthrough));
      out += line;
    } else if (block.terminator.kind == TerminatorKind::kJump) {
      std::snprintf(line, sizeof(line), " -> b%u",
                    static_cast<unsigned>(block.terminator.target));
      out += line;
    } else if (block.terminator.kind == TerminatorKind::kExternalCall) {
      std::snprintf(line, sizeof(line), " @0x%llx -> b%u",
                    static_cast<unsigned long long>(
                        block.terminator.external_target),
                    static_cast<unsigned>(block.terminator.fallthrough));
      out += line;
    }
    out += "\n";
  }
  return out;
}

}  // namespace p5emu::ir
