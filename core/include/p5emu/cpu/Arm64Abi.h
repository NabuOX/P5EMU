// SPDX-License-Identifier: Apache-2.0
//
// P5EMU is an independent, open-source project. See LICENSE and
// PROVENANCE.md before adding code here.
//
// ============================================================================
// P5EMU AArch64 BLOCK ABI  (v1)
// ============================================================================
//
// This header is included by BOTH the host-side code generator and the
// independently compiled AArch64 test harness, so the two cannot disagree about
// the layout of the context block the generated code operates on.
//
// It is deliberately C-compatible: the harness is plain C, and a layout that
// only a C++ compiler could agree on would defeat the point.
//
// WHAT THIS ABI DEFINES
//
//   * one generated function:  uint64_t p5emu_a64_block(P5EmuArm64Ctx* ctx)
//     called with the context pointer in x0. It returns the termination reason.
//   * the context layout below.
//   * the two memory helper functions the generated code calls for every guest
//     load and store. Generated code NEVER assumes guest VA == host VA; it asks
//     the helpers, which are backed by a GuestMemoryPort on the host side and by
//     a plain buffer in the harness.
//
// REGISTER USAGE (AAPCS64)
//
//   x0..x8    arguments to helpers / return value; not live across ops
//   x9..x17   scratch; clobbered by helper calls
//   x19       the context pointer. CALLEE-SAVED, so it survives helper calls.
//             This is the ONLY register live across a helper call.
//   x29, x30  frame pointer / link register, saved in the prologue
//   x18       reserved (platform register on Apple); not used
//
// Every IR value lives in a 16-byte stack slot, addressed from sp. There is no
// register allocation in Phase 2: it is slow and obviously correct, which is the
// right trade for the first backend. See docs/architecture/arm64-backend.md.
//
// NZCV: AArch64's flags are NOT x86 flags. They are used ONLY as a local
// convenience inside one flag-production sequence (carry and overflow of a real
// ADDS/SUBS), and are never treated as the architectural EFLAGS value. EFLAGS is
// always synthesised bit-by-bit from the operands and result.
// ============================================================================

#ifndef P5EMU_CPU_ARM64_ABI_H
#define P5EMU_CPU_ARM64_ABI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Context layout. Offsets are the contract; field names are for readability. */
#define P5EMU_A64_CTX_GPR_OFFSET 0      /* 16 * u64, guest rax..r15 */
#define P5EMU_A64_CTX_RIP_OFFSET 128
#define P5EMU_A64_CTX_EFLAGS_OFFSET 136
#define P5EMU_A64_CTX_STATUS_OFFSET 144 /* OUT: termination reason */
#define P5EMU_A64_CTX_FAULT_OFFSET 152  /* OUT: memory fault code, else 0 */
#define P5EMU_A64_CTX_STEPS_OFFSET 160  /* IN/OUT: step budget / steps used */
#define P5EMU_A64_CTX_MEM_OFFSET 168    /* opaque memory handle */
#define P5EMU_A64_CTX_FN_LOAD_OFFSET 176
#define P5EMU_A64_CTX_FN_STORE_OFFSET 184
#define P5EMU_A64_CTX_FN_HLE_OFFSET 192
#define P5EMU_A64_CTX_FN_ARITH_OFFSET 200 /* Phase 11 arithmetic helper */
#define P5EMU_A64_CTX_XMM_OFFSET 208      /* Phase 12: XMM0..XMM15 */
#define P5EMU_A64_CTX_SIZE 464            /* 208 + 16 * 16 bytes of XMM */

/* Termination reasons. Values MUST equal p5emu::cpu::TerminationReason. */
#define P5EMU_A64_REASON_RUNNING 0
#define P5EMU_A64_REASON_FALLTHROUGH 1
#define P5EMU_A64_REASON_RETURN 2
#define P5EMU_A64_REASON_INDIRECT_JUMP 3
#define P5EMU_A64_REASON_TRAP 4
#define P5EMU_A64_REASON_STEP_LIMIT 5
#define P5EMU_A64_REASON_FAULT 6
#define P5EMU_A64_REASON_IR_INVALID 7
#define P5EMU_A64_REASON_HLE_ERROR 8
#define P5EMU_A64_REASON_CPU_FAULT 9

/* Guest CPU fault kinds (ctx.fault_code when status is CPU_FAULT). */
#define P5EMU_A64_CPU_FAULT_DIVIDE 1

/* Arithmetic helper op selectors (Phase 11). MUST match the harness + host. */
#define P5EMU_A64_ARITH_ADC 0
#define P5EMU_A64_ARITH_SBB 1
#define P5EMU_A64_ARITH_MUL 2
#define P5EMU_A64_ARITH_IMUL 3
#define P5EMU_A64_ARITH_DIV 4
#define P5EMU_A64_ARITH_IDIV 5

/* HLE helper result codes. MUST match the host-side mapping. */
#define P5EMU_A64_HLE_OK 0
#define P5EMU_A64_HLE_NOT_FOUND 1
#define P5EMU_A64_HLE_AMBIGUOUS 2
#define P5EMU_A64_HLE_UNSUPPORTED_IDENTITY 3
#define P5EMU_A64_HLE_INVALID_ARGUMENT 4
#define P5EMU_A64_HLE_MEMORY_FAULT 5
#define P5EMU_A64_HLE_DISPATCH_FAULT 6
#define P5EMU_A64_HLE_INTERNAL 7

/* Guest register file order inside P5EMU_A64_CTX_GPR_OFFSET. */
#define P5EMU_A64_GPR_COUNT 16

/* Memory helper result codes. */
#define P5EMU_A64_MEM_OK 0
#define P5EMU_A64_MEM_UNMAPPED 1
#define P5EMU_A64_MEM_ACCESS 2
#define P5EMU_A64_MEM_READONLY 3

/* Status word returned when the translated instruction stream contains a TRAP
 * terminator: matches StatusCode::kUnimplementedInstruction on the host side is
 * NOT required here because the harness reports it through the status field and
 * the host maps the reason. Kept for symmetry / future use. */

/* The context. Plain integers, no pointers typed, so the same bytes are valid on
 * any host the emulator's ABI is compiled for. */
typedef struct P5EmuArm64Ctx {
  uint64_t gpr[P5EMU_A64_GPR_COUNT]; /* +0    : rax,rbx,rcx,rdx,rsi,rdi,rbp,rsp,r8..r15 */
  uint64_t rip;                      /* +128  */
  uint64_t eflags;                   /* +136  */
  uint64_t status;                   /* +144  : termination reason (OUT) */
  uint64_t fault_code;               /* +152  : memory fault code (OUT) */
  uint64_t steps;                    /* +160  : step budget / used */
  uint64_t mem_handle;               /* +168  : opaque to generated code */
  uint64_t fn_load;                  /* +176  : helper function pointer */
  uint64_t fn_store;                 /* +184  : helper function pointer */
  uint64_t fn_hle;                   /* +192  : HLE dispatch helper pointer */
  uint64_t fn_arith;                 /* +200  : arithmetic helper pointer */
  uint64_t xmm[16 * 2];              /* +208  : XMM0..XMM15, lo/hi pairs */
} P5EmuArm64Ctx;

/* Memory helper signatures, expressed as the generated code calls them.
 *
 *   load(mem_handle, addr, width, status_out) -> value
 *   store(mem_handle, addr, width, value, status_out) -> 0
 *
 * `status_out` receives one of the P5EMU_A64_MEM_* codes; generated code checks
 * it after every access and aborts to a normal exit on a non-zero code.
 */
typedef uint64_t (*p5emu_a64_load_fn)(uint64_t mem_handle, uint64_t addr,
                                      uint64_t width, uint64_t* status_out);
typedef uint64_t (*p5emu_a64_store_fn)(uint64_t mem_handle, uint64_t addr,
                                       uint64_t width, uint64_t value,
                                       uint64_t* status_out);

/* Arithmetic helper (Phase 11). Computes one x86 arithmetic operation with the
 * correct result, flags, and divide-fault detection, so the interpreter and the
 * AArch64 backend share ONE definition of the semantics.
 *
 *   arith(op, width, a, b, flags_in, out2*, flags_out*, fault_out*) -> result
 *   x0=op x1=width x2=a x3=b x4=flags_in x5=&out2 x6=&flags_out x7=&fault_out
 *
 * Returns x0 = primary result (low half / quotient). On success writes the
 * secondary result (high half / remainder) to *out2 and the new partial EFLAGS
 * to *flags_out. On a fault it sets *fault_out to a P5EMU_A64_CPU_FAULT_* code
 * and leaves *out2 and *flags_out untouched, so no partial result is committed.
 */
typedef uint64_t (*p5emu_a64_arith_fn)(uint64_t op, uint64_t width, uint64_t a,
                                       uint64_t b, uint64_t flags_in,
                                       uint64_t* out2, uint64_t* flags_out,
                                       uint64_t* fault_out);

/* The generated entry point's signature. */
typedef uint64_t (*p5emu_a64_block_fn)(P5EmuArm64Ctx* ctx);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* P5EMU_CPU_ARM64_ABI_H */
