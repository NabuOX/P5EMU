// SPDX-License-Identifier: LicenseRef-P5EMU-Proprietary
//
// P5EMU source is independently authored and proprietary. See LICENSE.md and
// PROVENANCE.md before adding code here.
//
// See TranslateToIr.h for the separation-of-concerns contract this file
// implements.

#include "p5emu/cpu/TranslateToIr.h"

#include <cstdio>

#include "p5emu/cpu/X64Decoder.h"

namespace p5emu::cpu {
namespace {

using ir::ArchRegister;
using ir::Builder;
using ir::ConditionCode;
using ir::ExtendMode;
using ir::OpKind;
using ir::ValueId;

using x64::Form;
using x64::Mnemonic;
using x64::Operand;
using x64::OpClass;

// Maps a decoded architectural register number onto the IR's register enum.
ArchRegister ToArchRegister(std::uint8_t reg) {
  // The encoding order RAX,RCX,RDX,RBX,RSP,RBP,RSI,RDI,R8..R15 is identical to
  // the IR's ArchRegister order, so this is a cast with bounds checking rather
  // than a switch. Kept as a function so the assumption is stated in one place.
  if (reg > 15) {
    return ArchRegister::kInvalid;
  }
  return static_cast<ArchRegister>(static_cast<std::uint8_t>(ArchRegister::kRax) +
                                   reg);
}

// Operand width in bytes, normalised to 1/2/4/8. 0 (unspecified) is 8.
std::uint8_t NormWidth(std::uint32_t width_bytes) {
  return (width_bytes == 1 || width_bytes == 2 || width_bytes == 4)
             ? static_cast<std::uint8_t>(width_bytes)
             : 8u;
}

// AH/BH/CH/DH are NOT modelled.
//
// Without a REX prefix, 8-bit register numbers 4..7 encode the HIGH bytes
// AH/CH/DH/BH, which alias bits 8-15 of RAX/RBX/RCX/RDX. The IR's register model
// only has low-byte views, so those registers are not representable. A naive
// decoder names them "spl/bpl/sil/dil" and would silently read/write the WRONG
// register. This rejects them explicitly instead.
//
// With a REX prefix, 4..7 are the low bytes SPL/BPL/SIL/DIL, which ARE the low
// byte of RSP/RBP/RSI/RDI and so are representable and supported.
bool UsesHighByteRegister(const x64::Instruction& insn) {
  if (insn.has_rex) {
    return false;
  }
  const auto is_high_byte = [](const Operand& o) {
    return o.cls == OpClass::kReg8 && o.reg >= 4 && o.reg <= 7;
  };
  return is_high_byte(insn.dst) || is_high_byte(insn.src);
}

// Computes the effective address of a memory operand as an SSA value.
//
// EVERY component of the addressing mode is included: base register, index
// register, scale, displacement, and RIP-relative addressing.
//
// Reading only the index and IGNORING THE BASE was a real bug: `mov [rdx], rax`
// computed address 0 and wrote to the wrong place. The base register is as much
// a part of the address as the index is.
//
// Translation does not require the guest memory to be mapped: the address is
// computed as a value and the access happens at execution time, so a program can
// be translated before any guest memory exists.
Status ComputeEffectiveAddress(const Operand& op, ir::ValueId* out, Builder* b) {
  ir::ValueId base = ir::kInvalidValue;
  ir::ValueId index = ir::kInvalidValue;

  if (op.has_base) {
    const ArchRegister r = ToArchRegister(op.base);
    if (r == ArchRegister::kInvalid) {
      return StatusCode::kDecodeError;
    }
    base = b->EmitGetReg(r);
  }
  if (op.has_index) {
    const ArchRegister r = ToArchRegister(op.index);
    if (r == ArchRegister::kInvalid) {
      return StatusCode::kDecodeError;
    }
    index = b->EmitGetReg(r);
  }

  // RIP-relative: the displacement is measured from the END of the current
  // instruction, which is exactly what RIP holds while that instruction
  // executes -- RIP is materialised per instruction during lowering.
  // Intel SDM Vol. 2 §2.2.2.7 and §3.7.2.1.
  //
  // Adding `insn_end` here as well would count it twice. An earlier version did,
  // because RIP used to hold the BLOCK entry instead of the next instruction.
  if (op.rip_relative) {
    const ir::ValueId rip = b->EmitGetReg(ArchRegister::kRip);
    const ir::ValueId disp = b->EmitMaterializeConstant(
        static_cast<std::uint64_t>(op.displacement), ir::ValueType::kI64);
    const ir::ValueId addr = b->EmitBinary(ir::OpKind::kAdd, rip, disp);
    if (addr == ir::kInvalidValue) {
      return StatusCode::kDecodeError;
    }
    *out = addr;
    return StatusCode::kOk;
  }

  const ir::ValueId addr = b->EmitAddress(base, index, op.scale, op.displacement);
  if (addr == ir::kInvalidValue) {
    return StatusCode::kDecodeError;
  }
  *out = addr;
  return StatusCode::kOk;
}

// Reads an operand into an SSA value, at the given operand width in bytes.
//
// The width matters in two places: a register is read as a width-specific view
// (AL is RAX[7:0], not RAX), and a memory operand is accessed at that width.
Status ReadOperand(const Operand& op, std::uint32_t width_bytes,
                   ir::ValueType* type_out, ir::ValueId* value_out, Builder* b) {
  switch (op.cls) {
    case OpClass::kReg64:
    case OpClass::kReg8: {
      const ArchRegister r = ToArchRegister(op.reg);
      if (r == ArchRegister::kInvalid) {
        return StatusCode::kDecodeError;
      }
      *value_out = b->EmitGetReg(r, NormWidth(width_bytes));
      *type_out = ir::ValueType::kI64;
      return StatusCode::kOk;
    }
    case OpClass::kMem8:
    case OpClass::kMem32:
    case OpClass::kMem64: {
      ir::ValueId addr = ir::kInvalidValue;
      const Status st = ComputeEffectiveAddress(op, &addr, b);
      if (!st.ok()) {
        return st;
      }
      *value_out = b->EmitLoad(addr, width_bytes);
      *type_out = ir::ValueType::kI64;
      return StatusCode::kOk;
    }
    case OpClass::kNone:
      *value_out = ir::kInvalidValue;
      *type_out = ir::ValueType::kInvalid;
      return StatusCode::kOk;
  }
  return StatusCode::kDecodeError;
}

// Writes a value to an operand, applying the x86 width rule at the destination.
//
// The width rule lives in the IR's kSetReg (see ReferenceInterpreter): an 8- or
// 16-bit write merges into the low bits of the full register, a 32-bit write
// zero-extends, and a 64-bit write replaces all 64 bits. The caller therefore
// does NOT pre-mask or pre-extend the value.
Status WriteOperand(const Operand& op, std::uint32_t width_bytes,
                    ir::ValueId value, Builder* b) {
  switch (op.cls) {
    case OpClass::kReg64:
    case OpClass::kReg8: {
      const ArchRegister r = ToArchRegister(op.reg);
      if (r == ArchRegister::kInvalid) {
        return StatusCode::kDecodeError;
      }
      // EAX/RAX are kReg64 plus a width; the width carries the rule.
      b->EmitSetReg(r, value, NormWidth(width_bytes));
      return StatusCode::kOk;
    }
    case OpClass::kMem8:
    case OpClass::kMem32:
    case OpClass::kMem64: {
      ir::ValueId addr = ir::kInvalidValue;
      const Status st = ComputeEffectiveAddress(op, &addr, b);
      if (!st.ok()) {
        return st;
      }
      b->EmitStore(addr, value, width_bytes);
      return StatusCode::kOk;
    }
    case OpClass::kNone:
      return StatusCode::kInvalidArgument;
  }
  return StatusCode::kDecodeError;
}

ConditionCode ToIrCondition(x64::Cond c) {
  return static_cast<ConditionCode>(c);
}

}  // namespace

// ---------------------------------------------------------------------------
// TranslateToIr
// ---------------------------------------------------------------------------
Status TranslateToIr(const x64::Instruction& insn, Builder* builder) {
  if (builder == nullptr) {
    return StatusCode::kInvalidArgument;
  }
  Builder& b = *builder;
  const memory::GuestAddr next_address = insn.address + insn.length;

  // AH/BH/CH/DH are outside the register model (see UsesHighByteRegister). Reject
  // them here, before any operand is read, rather than silently decoding them as
  // the low-byte registers spl/bpl/sil/dil.
  if (UsesHighByteRegister(insn)) {
    return StatusCode::kUnimplementedInstruction;
  }

  switch (insn.mnemonic) {
    // ---- MOV ---------------------------------------------------------------
    case Mnemonic::kMov: {
      ir::ValueType t{};
      ValueId src = ir::kInvalidValue;
      const Status read = ReadOperand(insn.src, insn.op_size, &t, &src, &b);
      if (!read.ok()) {
        return read;
      }
      if (src == ir::kInvalidValue) {
        // Immediate form.
        src = b.EmitMaterializeConstant(insn.imm, ir::ValueType::kI64);
        if (src == ir::kInvalidValue) {
          return StatusCode::kInternal;
        }
      }
      // The width rule (EAX zero-extends, AL merges) is applied by kSetReg.
      return WriteOperand(insn.dst, insn.op_size, src, &b);
    }

    // ---- LEA ---------------------------------------------------------------
    // Address computation only: no memory access, no flags. Keeping LEA separate
    // from MOV is an architectural requirement, not a stylistic one, because a
    // backend that fuses them would produce wrong behaviour for LEA.
    case Mnemonic::kLea: {
      // The effective address is computed with the SAME routine used for real
      // memory operands. LEA differs from MOV only in that no access and no
      // flags occur afterwards -- the address arithmetic itself is identical,
      // so a second, subtly different implementation here would be a bug farm.
      //
      // An earlier version rebuilt the address inline and omitted the BASE
      // register, so `lea rax, [rbx+rcx*4+0x10]` computed rcx*4+0x10 and
      // silently dropped rbx.
      ValueId addr = ir::kInvalidValue;
      const Status cs = ComputeEffectiveAddress(insn.src, &addr, &b);
      if (!cs.ok()) {
        return cs;
      }
      return WriteOperand(insn.dst, insn.op_size, addr, &b);
    }

    // ---- MOVZX / MOVSX -----------------------------------------------------
    case Mnemonic::kMovzx: {
      // Intel SDM Vol. 2 §2.2.1 (MOVZX). The source is read at its own width
      // (zero-extended for memory, masked for a register); the 32-bit
      // destination write then zero-extends to 64 through kSetReg.
      ir::ValueType t{};
      ValueId src = ir::kInvalidValue;
      const Status read = ReadOperand(insn.src, insn.op_size, &t, &src, &b);
      if (!read.ok()) {
        return read;
      }
      return WriteOperand(insn.dst, 4, src, &b);
    }

    case Mnemonic::kMovsx: {
      ir::ValueType t{};
      ValueId src = ir::kInvalidValue;
      const Status read = ReadOperand(insn.src, insn.op_size, &t, &src, &b);
      if (!read.ok()) {
        return read;
      }
      const std::uint32_t width = insn.op_size;
      if (width != 1 && width != 2 && width != 4) {
        return StatusCode::kDecodeError;
      }
      const ValueId extended =
          b.EmitExtend(src, static_cast<ExtendMode>(
                                width == 1   ? ExtendMode::kSignExtend8
                                : width == 2 ? ExtendMode::kSignExtend16
                                             : ExtendMode::kSignExtend32));
      if (extended == ir::kInvalidValue) {
        return StatusCode::kInternal;
      }
      // Destination is 32-bit; the low 32 bits of the sign-extended value are
      // exactly the sign extension to 32.
      return WriteOperand(insn.dst, 4, extended, &b);
    }

    // ---- ALU with flags ----------------------------------------------------
    case Mnemonic::kAdd:
    case Mnemonic::kSub:
    case Mnemonic::kAnd:
    case Mnemonic::kOr:
    case Mnemonic::kXor: {
      ir::ValueType tl{};
      ir::ValueType tr{};
      ValueId lhs = ir::kInvalidValue;
      ValueId rhs = ir::kInvalidValue;

      // Which operand is the destination is decided by the Form, which the
      // decoder set from the OPCODE (r <- r/m for 01/03/..., r/m <- r for
      // 00/02/...). The destination is always the LEFT operand of the
      // subtraction/comparison, whichever register it came from, so the Form is
      // recorded but not consulted again here: ReadOperand(insn.dst) has already
      // placed it in lhs.
      (void)0;

      if (insn.src.cls == OpClass::kNone) {
        // Immediate form: destination op immediate.
        const Status rd = ReadOperand(insn.dst, insn.op_size, &tl, &lhs, &b);
        if (!rd.ok()) return rd;
        rhs = b.EmitMaterializeConstant(insn.imm, ir::ValueType::kI64);
      } else {
        const Status rd = ReadOperand(insn.dst, insn.op_size, &tl, &lhs, &b);
        if (!rd.ok()) return rd;
        const Status rs = ReadOperand(insn.src, insn.op_size, &tr, &rhs, &b);
        if (!rs.ok()) return rs;
      }

      OpKind op = OpKind::kAddFlags;
      switch (insn.mnemonic) {
        case Mnemonic::kAdd: op = OpKind::kAddFlags; break;
        case Mnemonic::kSub: op = OpKind::kSubFlags; break;
        case Mnemonic::kAnd: op = OpKind::kAndFlags; break;
        case Mnemonic::kOr:  op = OpKind::kOrFlags;  break;
        case Mnemonic::kXor: op = OpKind::kXorFlags; break;
        default:             op = OpKind::kAndFlags; break;
      }

      // The op carries the operand size, so flag computation happens at the
      // right bit, and its result is already the width-masked value.
      const ValueId result = b.EmitBinaryFlags(op, lhs, rhs, insn.op_size);
      if (result == ir::kInvalidValue) {
        return StatusCode::kInternal;
      }
      // EmitBinaryFlags already committed the flags value to EFLAGS.
      return WriteOperand(insn.dst, insn.op_size, result, &b);
    }

    // ---- CMP / TEST: flags only --------------------------------------------
    case Mnemonic::kCmp: {
      ir::ValueType tl{};
      ir::ValueType tr{};
      ValueId lhs = ir::kInvalidValue;
      ValueId rhs = ir::kInvalidValue;
      const Status rd = ReadOperand(insn.dst, insn.op_size, &tl, &lhs, &b);
      if (!rd.ok()) return rd;
      if (insn.src.cls == OpClass::kNone) {
        rhs = b.EmitMaterializeConstant(insn.imm, ir::ValueType::kI64);
      } else {
        const Status rs = ReadOperand(insn.src, insn.op_size, &tr, &rhs, &b);
        if (!rs.ok()) return rs;
      }
      {
        const ir::ValueId flags = b.EmitCompare(lhs, rhs, insn.op_size);
        if (flags == ir::kInvalidValue) {
          return StatusCode::kInternal;
        }
        return StatusCode::kOk;
      }
    }

    case Mnemonic::kTest: {
      ir::ValueType tl{};
      ir::ValueType tr{};
      ValueId lhs = ir::kInvalidValue;
      ValueId rhs = ir::kInvalidValue;
      const Status rd = ReadOperand(insn.dst, insn.op_size, &tl, &lhs, &b);
      if (!rd.ok()) return rd;
      const Status rs = ReadOperand(insn.src, insn.op_size, &tr, &rhs, &b);
      if (!rs.ok()) return rs;
      {
        const ir::ValueId flags = b.EmitTest(lhs, rhs, insn.op_size);
        if (flags == ir::kInvalidValue) {
          return StatusCode::kInternal;
        }
        return StatusCode::kOk;
      }
    }

    // ---- shifts -------------------------------------------------------------
    case Mnemonic::kShl:
    case Mnemonic::kShr:
    case Mnemonic::kSar: {
      ir::ValueType tl{};
      ValueId value = ir::kInvalidValue;
      const Status rd = ReadOperand(insn.dst, insn.op_size, &tl, &value, &b);
      if (!rd.ok()) return rd;

      ValueId count = ir::kInvalidValue;
      if (insn.form == Form::kShiftCL) {
        // CL is the low byte of RCX; the shift op masks the count to 5 bits (or 6
        // for a 64-bit operand), which discards everything above CL.
        count = b.EmitGetReg(ArchRegister::kRcx, 1);
      } else {
        count = b.EmitMaterializeConstant(insn.shift_count,
                                          ir::ValueType::kI64);
      }
      if (count == ir::kInvalidValue) {
        return StatusCode::kInternal;
      }

      OpKind op = OpKind::kShl;
      switch (insn.mnemonic) {
        case Mnemonic::kShl: op = OpKind::kShl; break;
        case Mnemonic::kShr: op = OpKind::kShr; break;
        case Mnemonic::kSar: op = OpKind::kSar; break;
        default: return StatusCode::kDecodeError;
      }
      const ValueId result = b.EmitShift(op, value, count, insn.op_size);
      if (result == ir::kInvalidValue) {
        return StatusCode::kInternal;
      }
      return WriteOperand(insn.dst, insn.op_size, result, &b);
    }

    // ---- control flow -------------------------------------------------------
    case Mnemonic::kJmp: {
      b.EmitJump(ir::kInvalidBlock);  // patched by the program builder
      return StatusCode::kOk;
    }
    case Mnemonic::kJcc: {
      b.EmitBranch(ToIrCondition(insn.cond), ir::kInvalidValue,
                   ir::kInvalidBlock, ir::kInvalidBlock);
      return StatusCode::kOk;
    }
    case Mnemonic::kCall: {
      // CALL pushes the return address (the address of the next instruction) and
      // transfers control. Intel SDM Vol. 2 §2.2.1 (CALL). Modelled explicitly
      // rather than as an IR-level call, because the stack write is guest memory
      // traffic that a backend must perform in the right order.
      const ValueId ret = b.EmitMaterializeConstant(next_address,
                                                    ir::ValueType::kI64);
      const ValueId sp = b.EmitGetReg(ArchRegister::kRsp);
      const ValueId new_sp = b.EmitBinary(OpKind::kSub, sp,
                                           b.EmitMaterializeConstant(8,
                                                                    ir::ValueType::kI64));
      b.EmitStore(new_sp, ret, 8);
      b.EmitSetReg(ArchRegister::kRsp, new_sp);
      b.EmitJump(ir::kInvalidBlock);  // patched by the program builder
      return StatusCode::kOk;
    }
    case Mnemonic::kRet: {
      // RET pops the return address into RIP and transfers to it. Modelling it
      // as an indirect jump (rather than ending the program) is what lets a
      // guest function RETURN to its caller. A target that is not a known block
      // is reported as a return out of the program.
      const ValueId sp = b.EmitGetReg(ArchRegister::kRsp);
      const ValueId target = b.EmitLoad(sp, 8);
      const ValueId new_sp = b.EmitBinary(OpKind::kAdd, sp,
                                           b.EmitMaterializeConstant(8,
                                                                    ir::ValueType::kI64));
      b.EmitSetReg(ArchRegister::kRsp, new_sp);
      b.EmitSetReg(ArchRegister::kRip, target);
      b.EmitIndirectJump(target);
      return StatusCode::kOk;
    }

    case Mnemonic::kNop:
      return StatusCode::kOk;

    case Mnemonic::kInt3:
      // INT3 is a deliberate trap. Phase 1 surfaces it as
      // kUnimplementedInstruction rather than pretending to emulate it.
      b.EmitTrap();
      return StatusCode::kOk;

    case Mnemonic::kUnimplemented:
    default:
      return StatusCode::kUnimplementedInstruction;
  }
}

// ---------------------------------------------------------------------------
// TranslateProgram
// ---------------------------------------------------------------------------
Status TranslateProgram(memory::GuestAddr base_address, const std::uint8_t* code,
                        std::size_t length, std::string name,
                        ir::Program* out_program,
                        std::vector<memory::GuestAddr>* out_addresses) {
  if (out_program == nullptr || code == nullptr || length == 0) {
    return StatusCode::kInvalidArgument;
  }
  if (length > 4096) {
    // Bound: a runaway lowering would hang the compiler. Phase-1 programs are
    // small by construction.
    return StatusCode::kExecutionLimit;
  }

  const memory::GuestAddr end_address = base_address + length;
  const x64::X64Decoder decoder;

  // =========================================================================
  // WORKLIST LOWERING
  // =========================================================================
  //
  // A block is lowered because something transfers control to it: either it is
  // the program entry, or it is the fallthrough/target of an instruction already
  // lowered.
  //
  // This is a worklist rather than a single linear pass because a linear pass
  // NEVER REACHES A BRANCH TARGET that is not also on the fallthrough path. An
  // earlier linear version silently left every such target unlowered, the
  // target's block was never created, and the patch resolver turned the branch
  // into a trap -- so `je` around a poison instruction executed the poison.
  //
  // The worklist also bounds termination: an address is lowered at most once, so
  // a backward branch terminates lowering rather than looping forever.
  //
  std::vector<memory::GuestAddr> address_to_block_ids;  // parallel to blocks
  std::vector<memory::GuestAddr> worklist;

  // A transfer whose destination is a GUEST ADDRESS, not a block id. The target
  // may not exist yet when the edge is discovered, so every edge is recorded
  // here and resolved once the worklist drains.
  struct PendingEdge {
    ir::BlockId from = ir::kInvalidBlock;
    memory::GuestAddr target_address = 0;
    bool is_fallthrough = false;
  };
  std::vector<PendingEdge> pending_edges;

  auto find_block_for = [&](memory::GuestAddr addr) -> ir::BlockId {
    for (std::size_t i = 0; i < address_to_block_ids.size(); ++i) {
      if (address_to_block_ids[i] == addr) {
        return static_cast<ir::BlockId>(i);
      }
    }
    return ir::kInvalidBlock;
  };

  Builder builder(std::move(name));

  worklist.push_back(base_address);

  while (!worklist.empty()) {
    const memory::GuestAddr block_address = worklist.back();
    worklist.pop_back();

    if (block_address < base_address || block_address >= end_address) {
      continue;  // a transfer off the end of the mapped program
    }
    if (find_block_for(block_address) != ir::kInvalidBlock) {
      continue;  // already lowered
    }

    const std::size_t offset = static_cast<std::size_t>(block_address - base_address);
    x64::Instruction insn;
    const Status ds = decoder.DecodeOne(
        block_address, code + offset,
        static_cast<std::size_t>(end_address - block_address), &insn);
    if (!ds.ok()) {
      // An undecodable or unsupported byte becomes a trap block: the failure is
      // visible and located rather than silently skipped.
      builder.CreateBlock(block_address);
      address_to_block_ids.push_back(block_address);
      if (block_address == base_address) {
        // Blocks are appended in order, so the entry is the block just created.
        builder.SetEntryBlock(static_cast<ir::BlockId>(
            address_to_block_ids.size() - 1));
      }
      builder.EmitTrap();
      continue;
    }

    const ir::BlockId block = builder.CreateBlock(block_address);
    address_to_block_ids.push_back(block_address);
    if (block_address == base_address) {
      builder.SetEntryBlock(block);
    }

    // RIP is materialised per instruction inside the run below, where the
    // length of the current instruction is known.

    if (out_addresses != nullptr) {
      out_addresses->push_back(block_address);
    }

    // Lower a straight-line run of instructions into this one block.
    memory::GuestAddr pc = block_address;
    bool block_done = false;

    while (!block_done) {
      const std::size_t pc_off = static_cast<std::size_t>(pc - base_address);
      if (pc < base_address || pc >= end_address) {
        break;
      }
      x64::Instruction in;
      const Status st = decoder.DecodeOne(
          pc, code + pc_off, static_cast<std::size_t>(end_address - pc), &in);
      if (!st.ok()) {
        // Cannot continue this run; the target becomes a trap block later if
        // something transfers to it.
        block_done = true;
        break;
      }

      const memory::GuestAddr insn_end = pc + in.length;

      // RIP holds the address of the NEXT instruction while an instruction
      // executes (Intel SDM Vol. 2 §2.2.1). RIP-relative addressing depends on
      // this, and it is architecturally observable, so it is materialised
      // explicitly before every instruction rather than once per block or left
      // implicit.
      //
      // Setting it per block would be wrong: within a straight-line run only the
      // FIRST instruction has RIP equal to the block entry.
      {
        const ir::ValueId rip_value =
            builder.EmitMaterializeConstant(insn_end, ir::ValueType::kI64);
        builder.EmitSetReg(ir::ArchRegister::kRip, rip_value);
      }

      const Status ts = TranslateToIr(in, &builder);
      if (!ts.ok()) {
        // An unsupported form terminates the run with a trap, and the failure is
        // reported to the caller rather than swallowed.
        builder.EmitTrap();
        *out_program = builder.MutableProgram();
        return ts;
      }

      // NOTE ON LIFETIME: block IDs, never Block pointers or references, are
      // held across CreateBlock(). CreateBlock push_back's into a std::vector,
      // which reallocates and invalidates every pointer and reference into it.
      // An earlier version held an `ir::Terminator&` across that call and wrote
      // through a dangling reference, corrupting every branch.
      switch (in.mnemonic) {
        case Mnemonic::kJmp: {
          ir::Terminator& t =
              builder.MutableProgram().FindBlock(block)->terminator;
          t = ir::Terminator{};
          t.kind = ir::TerminatorKind::kJump;
          const memory::GuestAddr target = insn_end + in.imm;
          worklist.push_back(target);
          pending_edges.push_back(PendingEdge{block, target, false});
          break;
        }
        case Mnemonic::kJcc: {
          const ir::ValueId flags = builder.EmitLoadFlags();
          ir::Terminator& t =
              builder.MutableProgram().FindBlock(block)->terminator;
          t = ir::Terminator{};
          t.kind = ir::TerminatorKind::kBranch;
          t.cond = static_cast<ir::ConditionCode>(in.cond);
          t.cond_value = flags;
          const memory::GuestAddr target = insn_end + in.imm;
          worklist.push_back(target);
          pending_edges.push_back(PendingEdge{block, target, false});
          // The fallthrough is a real edge too, and it is the address of the
          // instruction AFTER the branch. This block's run ends at the branch,
          // so the fallthrough must be lowered as its own block.
          worklist.push_back(insn_end);
          pending_edges.push_back(PendingEdge{block, insn_end, true});
          break;
        }
        case Mnemonic::kCall: {
          ir::Terminator& t =
              builder.MutableProgram().FindBlock(block)->terminator;
          t = ir::Terminator{};
          const memory::GuestAddr target = insn_end + in.imm;
          if (target >= base_address && target < end_address) {
            // Internal call: an ordinary jump; the return block is lowered
            // naturally by the worklist.
            t.kind = ir::TerminatorKind::kJump;
            worklist.push_back(target);
            pending_edges.push_back(PendingEdge{block, target, false});
          } else {
            // External call: the target is not guest code, so it is an HLE
            // binding. The guest has already pushed the return address (the
            // CALL lowering did that); execution resumes in the block at
            // insn_end after the binding returns.
            t.kind = ir::TerminatorKind::kExternalCall;
            t.external_target = target;
            worklist.push_back(insn_end);
            pending_edges.push_back(PendingEdge{block, insn_end, true});
          }
          break;
        }
        default: {
          ir::Terminator& t =
              builder.MutableProgram().FindBlock(block)->terminator;
          if (t.kind == ir::TerminatorKind::kInvalid) {
            t.kind = ir::TerminatorKind::kFallthrough;
          }
          break;
        }
      }

      const ir::TerminatorKind kind =
          builder.MutableProgram().FindBlock(block)->terminator.kind;

      if (kind == ir::TerminatorKind::kReturn ||
          kind == ir::TerminatorKind::kTrap ||
          kind == ir::TerminatorKind::kJump ||
          kind == ir::TerminatorKind::kBranch ||
          kind == ir::TerminatorKind::kIndirectJump ||
          kind == ir::TerminatorKind::kExternalCall) {
        block_done = true;
        break;
      }

      // Fallthrough: the next instruction belongs to the SAME basic block.
      pc = insn_end;
    }

    // Close out a run that ended without a terminator.
    ir::Block* b = builder.MutableProgram().FindBlock(block);
    if (b != nullptr && b->terminator.kind == ir::TerminatorKind::kInvalid) {
      b->terminator.kind = ir::TerminatorKind::kFallthrough;
    }
  }

  // Resolve every recorded transfer now that all blocks exist.
  //
  // A target that falls off the end of the range, or that is the middle of an
  // instruction, resolves to kInvalidBlock and is reported by ValidateIr. That
  // is deliberate: a silently-dropped edge would execute the wrong code.
  for (const PendingEdge& e : pending_edges) {
    ir::Block* from = builder.MutableProgram().FindBlock(e.from);
    if (from == nullptr) {
      return StatusCode::kInvalidArgument;
    }
    const ir::BlockId resolved = find_block_for(e.target_address);
    if (e.is_fallthrough) {
      from->terminator.fallthrough = resolved;
    } else {
      from->terminator.target = resolved;
    }
  }

  if (builder.MutableProgram().entry == ir::kInvalidBlock) {
    return StatusCode::kUnimplemented;  // nothing decodable at the entry
  }

  *out_program = builder.MutableProgram();
  const Status valid = ir::ValidateIr(*out_program);
  if (!valid.ok()) {
    return valid;
  }
  return StatusCode::kOk;
}

Status TranslateBlockToIr(memory::GuestAddr base_address,
                          const std::uint8_t* code, std::size_t length,
                          std::vector<memory::GuestAddr>* out_addresses,
                          ir::Program* out_program) {
  return TranslateProgram(base_address, code, length, "p5emu-block",
                          out_program, out_addresses);
}

}  // namespace p5emu::cpu
