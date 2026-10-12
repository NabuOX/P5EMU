# AArch64 Backend (Phase 2)

Phase 2 adds the first real execution backend: it lowers the **existing**
validated P5EMU IR to AArch64 machine code, runs that code in an independent
AArch64 environment, and proves the result matches the `ReferenceInterpreter`
field by field.

```
x86-64 bytes
    │  X64Decoder
    ▼
P5EMU IR ──► ValidateIr
    │                     │
    │                     ├─► ReferenceInterpreter ─┐
    ▼                     │                         ├─► compare (registers, rip,
Arm64CodeGen ─► Arm64 bytes ─► independent AArch64 ─┘   eflags, memory, reason,
                                   execution            status)
```

The backend consumes the same IR as the interpreter. It does not re-implement
x86 semantics; it translates IR operations. Unsupported IR makes it report a
**coverage gap**, never a fabricated result and never a silent fallback.

---

## 1. Execution oracle

The development host is x86-64, so emitted AArch64 cannot run natively. Doing
"we generated code that looks right" would not be proof. Phase 2 therefore runs
the emitted bytes under **qemu-aarch64 user-mode** (qemu-user, GPL-2.0), an
independent implementation of the AArch64 ISA, via the P5EMU harness.

Why this is a real oracle: the instruction *semantics* under test come from
QEMU's AArch64 translator, not from P5EMU. P5EMU only chooses the encoding; QEMU
decides what it does. The comparison then checks P5EMU's chosen encoding against
the reference interpreter's result.

`qemu-aarch64` is test infrastructure only: separate process, not linked, not
vendored, not a build dependency. See `PROVENANCE.md`. A native AArch64 host
would instead use `CodeBuffer` + a direct call (`NativeArm64Runner`), which is
not exercised or claimed on x86-64.

---

## 2. Emitter decision

**A small, independently authored emitter (`Arm64Emitter`), not VIXL.**

The subset Phase 2 emits is about twenty instruction forms. Importing VIXL
(BSD-3-Clause, ~100k lines, its own build system and dependency surface) to
encode twenty forms would add a large audit surface for little benefit, and
would place a third party between P5EMU's IR and the bytes P5EMU claims to
produce. Instead the encoders are written field-by-field from the Arm
Architecture Reference Manual and verified against an independent disassembler
(`aarch64-linux-gnu-objdump`); `Arm64Emitter` unit tests pin the exact words.

The emitter is pure byte generation with no host or runtime dependency, so it is
fully testable on x86-64.

---

## 3. Block ABI (v1)

Defined in `core/include/p5emu/cpu/Arm64Abi.h`, shared verbatim by the generator
and the C harness so the two cannot disagree.

```
uint64_t p5emu_a64_block(P5EmuArm64Ctx* ctx);   // ctx in x0, reason returned
```

Context (`P5EmuArm64Ctx`), 8-byte aligned:

| Offset | Field | Meaning |
|---|---|---|
| 0 | `gpr[16]` | guest rax, rbx, rcx, rdx, rsi, rdi, rbp, rsp, r8..r15 |
| 128 | `rip` | guest RIP |
| 136 | `eflags` | x86 EFLAGS (synthesised) |
| 144 | `status` | OUT: termination reason |
| 152 | `fault_code` | OUT: memory fault code |
| 160 | `steps` | IN: step budget; OUT: remaining |
| 168 | `mem_handle` | opaque pointer passed to helpers |
| 176 | `fn_load` | memory load helper |
| 184 | `fn_store` | memory store helper |

Register discipline (AAPCS64):

- `x19` — the context pointer; **callee-saved**, the only register live across a
  helper call, saved/restored in the prologue/epilogue.
- `x0..x8` — helper arguments / return; `x9..x17` — scratch.
- `x29`/`x30` — frame pointer / link register, saved in the prologue.
- `x18` — reserved (platform register on Apple); unused.

Stack alignment is 16 bytes (AAPCS64). The frame is allocated with `SUB sp,
sp, #imm` (immediate forms, because shifted-register ADD/SUB cannot name SP).

**NZCV policy.** AArch64 flags are *not* x86 flags and are never used as the
architectural EFLAGS value. They are read only inside one arithmetic sequence
(the carry and overflow of a real `ADDS`/`SUBS`, which happen to match x86 CF/OF
for add, and CF=¬C / OF=V for subtract). Every EFLAGS bit is then synthesised
explicitly and stored as a value. Nothing depends on the two flag models
coinciding beyond the two bits that genuinely do.

**Portability.** The ABI names no OS. The only platform-specific piece is the
code buffer (`CodeBuffer`), which is the single seam an Apple `MAP_JIT`
implementation would replace. Apple JIT is not implemented in Phase 2.

---

## 4. Supported IR subset

Phase 2 supported, all 64-bit:

- `kConst`, `kNop`
- `kGetReg`, `kSetReg`
- `kAdd`, `kSub`, `kAnd`, `kOr`, `kXor`
- `kAddFlags`, `kSubFlags`, `kAndFlags`, `kOrFlags`, `kXorFlags`
- `kCmp`, `kTest`
- `kLoadFlags`, `kStoreFlags`, `kTestCc` (all sixteen condition codes)
- `kExtend` (`kZeroExtend32`, sign-extend 8/16/32), `kTruncate`
- `kAddrCalc` (base + index*scale + displacement)
- `kLoad`, `kStore` (1/2/4/8 bytes, via helpers)
- Terminators: `kFallthrough`, `kJump`, `kBranch`, `kIndirectJump`, `kReturn`,
  `kTrap`

Phase 3 added:

- **`kShl` / `kShr` / `kSar`**, immediate and `CL` counts, at every width.
- **8/16/32/64-bit operand widths** for the ops above: the ops carry an operand
  size, and the width drives flag computation, partial-register read/write
  masks, shift count masking, and memory access width.

The backend now implements every IR op the validator accepts. The only explicit
refusal left is a program whose stack-slot frame exceeds the unsigned-offset
addressing range (over ~2047 values), which is reported as
`kUnimplementedInstruction` with no code emitted.

---

## 4a. Operand-width model

`Op::width` is the x86 operand size in bytes (1/2/4/8), set by the translator
from the decoded instruction and carried by `kGetReg`/`kSetReg`, the
flag-producing ALU ops, and the shifts. The semantics, matching the interpreter:

- **Result**: `(a op b)` masked to the width.
- **Flags** are computed at bit `8*width - 1`:
  - ADD: CF is `(a+b) > mask`, OF is signed overflow at the width's sign bit.
  - SUB/CMP: CF is the borrow `a < b` (width-independent once operands are
    masked), OF is signed overflow at the width's sign bit.
  - Logic: ZF/SF/PF from the width-masked result; CF/OF cleared.
  - ZF, PF and AF are width-independent once operands and result are masked.
- **Partial registers**: `kGetReg` reads the low `width` bytes; `kSetReg` merges
  8/16-bit writes into the low bytes, zero-extends 32-bit writes, and replaces
  all 64 bits for 64-bit writes.
- **Shift counts** are masked to 5 bits, or 6 bits for a 64-bit operand, per
  Intel SDM Vol. 2 §3.5.

A 16-bit operand uses the 64-bit register container; `op_size` carries the true
width and the translator and backend use it rather than the operand class. For
narrow widths the backend cannot use the native AArch64 carry flag (which would
be the carry out of bit 63), so it synthesises CF/OF/SF from the width's sign
bit explicitly.

AH/BH/CH/DH are **explicitly unsupported**: without a REX prefix, 8-bit register
numbers 4-7 are the high bytes, which the IR's low-byte register model cannot
represent. They are rejected with `kUnimplementedInstruction` rather than being
silently decoded as SPL/BPL/SIL/DIL (which is what a naive decoder does).

---

## 4b. Architecturally undefined flags

Some x86 flags are architecturally UNDEFINED for certain inputs — most notably
OF after a shift by more than one, and CF/OF once a shift count reaches the
operand size; AF is undefined for shifts. The interpreter still leaves *some*
value in those bits (it must), but requiring two backends to agree on invented
values would be requiring them to agree on behaviour the ISA does not specify.

The differential harness therefore supports a **fixture-specific observable
mask** (`ComparisonRequest::eflags_observable_mask`, default: all bits). Bits
outside the mask are ignored on every backend. Shift fixtures compute the mask
from the effective shift count and the operand size. There is NO global masking
of flags: the default observes every bit, and each narrowing is justified from
the ISA.


---

## 5. Register allocation

There is none, and that is deliberate. Every IR value lives in a 16-byte stack
slot addressed from `sp`; guest GPRs live in the context block and are
loaded/stored per access. This is slow and obviously correct, which is the right
trade for a first backend and for a differential oracle.

The value→slot mapping is a straight function of the value id, and the frame
size is checked against the unsigned-offset addressing limit at generation time
(the frame is refused, not truncated, if it would overflow). Replacing this with
a real allocator is a local change behind the same ABI.

---

## 6. Flags

For each supported flag-producing op the backend computes every EFLAGS bit the
interpreter produces, so the comparison can be exact:

- **ADD**: CF, OF come from `ADDS` native C/V; ZF, SF, PF, AF are synthesised.
- **SUB / CMP**: CF = ¬C and OF = V from `SUBS`; ZF, SF, PF, AF synthesised.
- **AND/OR/XOR/TEST**: ZF, SF, PF synthesised; CF and OF cleared (matching the
  interpreter, which clears the architecturally-undefined AF as a documented
  choice).

PF is computed by folding the low byte and comparing parity; AF is bit 4 of
`a ^ b ^ result`. Both are the same identities the interpreter uses, written in
AArch64.

---

## 7. Memory

Generated code never assumes guest VA == host VA. Every load/store becomes a
call to `fn_load`/`fn_store`, which are backed by `GuestMemoryPort` on the host
side and by a flat buffer in the harness. Bounds and width are enforced by the
helper; an out-of-range access returns a fault code, and the generated code
branches to a normal fault exit that records the reason.

The host-side backend copies the guest image in and out through a new,
*additive* port capability (`Regions`, `ReadRange`, `WriteRange`). These are
transport methods: a backend that uses them must produce results identical to one
that does not, and a port that does not implement them reports "unsupported"
(a hard coverage gap), never an empty region.

---

## 8. Code buffer and W^X

`CodeBuffer` maps a region read-write, copies the code in, then flips it to
read+execute with `mprotect`. There is no RWX window and no page is ever both
writable and executable. On platforms without an implementation it fails cleanly
rather than allocating RWX. The harness uses the same discipline directly.

---

## 9. Differential and negative testing

`Arm64Differential.*` runs the reference interpreter and the AArch64 backend over
the same IR, initial state and memory, through the existing `RunComparison`.
There is no ARM64-specific comparison logic; the harness treats it like any
backend. `RunComparison` distinguishes three outcomes: `kAgree`, `kDiverge`, and
`kCoverageGap` (a backend reported `implemented == false`). A coverage gap emits
NO state-difference noise from a run that never happened, but a backend that
claims to implement a program and then diverges is still compared normally and
fails.

Phase 3 adds:

- `Arm64Widths.*`, `Arm64PartialReg.*`, `Arm64Shifts.*`, `Arm64MemWidth.*`,
  `Arm64Edge.*`, `Arm64ControlFlow.*` — curated differential suites.
- `Arm64NegGate.*` — two further injected code-generation defects (a 32-bit write
  that fails to zero-extend, and a shift count masked to the wrong width), each
  required to be caught as `kDiverge`.
- `Arm64Random.*` — 2500 randomized cases (see below).

---

## 9a. Batched randomized differential testing

`Arm64Random.BatchedRandomizedDifferential` generates 2500 hand-built IR programs
from a fixed seed, mixing widths, ALU and logic operations, shift counts, and
memory widths. Each case is executed by the reference interpreter on the host and
by the AArch64 backend, and compared with the same primitives the harness uses.

The harness protocol is **batched**: the request file is a concatenation of
request records and the response file a concatenation of response records, so a
single qemu-aarch64 process serves every case. This is what makes thousands of
cases practical — the 2500-case suite runs in a fraction of a second rather than
paying process startup per case. A divergence records the seed and case index and
reproduces exactly.


---

## 10. Debugging

Set `P5EMU_A64_DUMP_DIR` and run `p5emu_tests Arm64Debug.` to write the emitted
bytes; `scripts/arm64_disasm.sh <file>` disassembles them with an independent
tool. This is diagnostic infrastructure, not user UI.
