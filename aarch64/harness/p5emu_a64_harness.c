/* SPDX-License-Identifier: Apache-2.0
 *
 * P5EMU AArch64 execution harness.
 *
 * TEST INFRASTRUCTURE. Cross-compiled for AArch64 and run by an independent
 * AArch64 emulator (qemu-aarch64). Not linked into p5emu_core, not vendored.
 *
 * MULTI-REGION PROTOCOL (v2). The request carries a table of guest memory
 * regions (base, size, permissions, bytes); helper loads/stores/fetches resolve
 * an address to the correct region and enforce permissions within it. The
 * response carries the modified bytes of each region, in order.
 *
 * Request record:
 *   "P5A2" | u32 version=2 | u64 code_len | u64 step_limit |
 *   u64 gpr[16] | u64 rip | u64 eflags | u32 region_count |
 *   { u64 base | u64 size | u32 perms | u8 bytes[size] }*  |
 *   u8 code[code_len]
 *
 * Response record:
 *   "P5A2" | u32 version=2 | u64 status | u64 fault_code | u64 steps |
 *   u64 gpr[16] | u64 rip | u64 eflags |
 *   { u8 bytes[size] }*
 *
 * Usage: p5emu_a64_harness <request> <response>
 */

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "p5emu/cpu/Arm64Abi.h"

#define PROTOCOL_VERSION 2u
#define MAX_REGIONS 64u
#define MAX_REGION_BYTES (64ull * 1024ull * 1024ull)

#define MEM_R 1u
#define MEM_W 2u
#define MEM_X 4u

typedef struct {
  uint64_t base;
  uint64_t size;
  uint32_t perms;
  uint8_t* data;
} Region;

typedef struct {
  Region regions[MAX_REGIONS];
  uint32_t count;
} RegionTable;

static uint64_t rd_u64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v |= ((uint64_t)p[i]) << (8 * i);
  return v;
}
static uint32_t rd_u32(const uint8_t* p) {
  uint32_t v = 0;
  for (int i = 0; i < 4; ++i) v |= ((uint32_t)p[i]) << (8 * i);
  return v;
}
static void wr_u64(uint8_t* p, uint64_t v) {
  for (int i = 0; i < 8; ++i) p[i] = (uint8_t)((v >> (8 * i)) & 0xffu);
}
static void wr_u32(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; ++i) p[i] = (uint8_t)((v >> (8 * i)) & 0xffu);
}

/* Resolve an address to a region covering [addr, addr+width). */
static Region* find_region(RegionTable* t, uint64_t addr, uint64_t width) {
  for (uint32_t i = 0; i < t->count; ++i) {
    Region* r = &t->regions[i];
    if (addr >= r->base && (addr - r->base) <= r->size &&
        width <= (r->size - (addr - r->base))) {
      return r;
    }
  }
  return NULL;
}

static uint64_t h_load(uint64_t handle, uint64_t addr, uint64_t width,
                       uint64_t* status_out) {
  RegionTable* t = (RegionTable*)handle;
  if (width != 1 && width != 2 && width != 4 && width != 8) {
    *status_out = P5EMU_A64_MEM_ACCESS;
    return 0;
  }
  Region* r = find_region(t, addr, width);
  if (r == NULL) {
    *status_out = P5EMU_A64_MEM_UNMAPPED;
    return 0;
  }
  if ((r->perms & MEM_R) == 0) {
    *status_out = P5EMU_A64_MEM_ACCESS;
    return 0;
  }
  const uint64_t off = addr - r->base;
  uint64_t v = 0;
  for (uint64_t i = 0; i < width; ++i) {
    v |= ((uint64_t)r->data[off + i]) << (8 * i);
  }
  *status_out = P5EMU_A64_MEM_OK;
  return v;
}

static uint64_t h_store(uint64_t handle, uint64_t addr, uint64_t width,
                        uint64_t value, uint64_t* status_out) {
  RegionTable* t = (RegionTable*)handle;
  if (width != 1 && width != 2 && width != 4 && width != 8) {
    *status_out = P5EMU_A64_MEM_ACCESS;
    return 0;
  }
  Region* r = find_region(t, addr, width);
  if (r == NULL) {
    *status_out = P5EMU_A64_MEM_UNMAPPED;
    return 0;
  }
  if ((r->perms & MEM_W) == 0) {
    *status_out = P5EMU_A64_MEM_READONLY;
    return 0;
  }
  const uint64_t off = addr - r->base;
  for (uint64_t i = 0; i < width; ++i) {
    r->data[off + i] = (uint8_t)((value >> (8 * i)) & 0xffu);
  }
  *status_out = P5EMU_A64_MEM_OK;
  return 0;
}

/* Arithmetic helper (Phase 11). ONE definition of the x86 arithmetic semantics,
 * shared (by construction) with the host interpreter, so the interpreter and
 * this backend agree on result, flags, and the divide fault. Mirrors
 * ReferenceInterpreter.cpp exactly. */
#define EFLAG_CF (1ull << 0)
#define EFLAG_PF (1ull << 2)
#define EFLAG_AF (1ull << 4)
#define EFLAG_ZF (1ull << 6)
#define EFLAG_SF (1ull << 7)
#define EFLAG_OF (1ull << 11)

static int arith_parity_even(uint64_t v) {
  v &= 0xffu;
  v ^= v >> 4;
  v ^= v >> 2;
  v ^= v >> 1;
  return (v & 1u) == 0;
}

static __int128 arith_sext128(unsigned __int128 v, unsigned total_bits) {
  if (total_bits == 0) return 0;
  if (total_bits >= 128) return (__int128)v;
  unsigned sh = 128u - total_bits;
  return (__int128)(v << sh) >> sh;
}

static uint64_t h_arith(uint64_t op, uint64_t width, uint64_t a, uint64_t b,
                        uint64_t c, uint64_t* out2, uint64_t* flags_out,
                        uint64_t* fault_out) {
  unsigned bits = (unsigned)(8u * width);
  uint64_t m = (width >= 8) ? ~(uint64_t)0 : (((uint64_t)1 << bits) - 1u);
  a &= m;
  b &= m;
  uint64_t f = 0;
  uint64_t result = 0;
  uint64_t second = 0;
  *fault_out = 0;

  if (op == P5EMU_A64_ARITH_ADC) {
    unsigned __int128 s = (unsigned __int128)a + b + c;
    result = (uint64_t)s & m;
    if ((s >> bits) & 1u) f |= EFLAG_CF;
    uint64_t sa = (a >> (bits - 1u)) & 1u, sb = (b >> (bits - 1u)) & 1u,
             sr = (result >> (bits - 1u)) & 1u;
    if (sa == sb && sr != sa) f |= EFLAG_OF;
    if ((int)(a & 0xfu) + (int)(b & 0xfu) + (int)c > 15) f |= EFLAG_AF;
  } else if (op == P5EMU_A64_ARITH_SBB) {
    result = (a - b - c) & m;
    if (a < b || (a == b && c == 1)) f |= EFLAG_CF;
    uint64_t sa = (a >> (bits - 1u)) & 1u, sb = (b >> (bits - 1u)) & 1u,
             sr = (result >> (bits - 1u)) & 1u;
    if (sa != sb && sr != sa) f |= EFLAG_OF;
    if ((int)(a & 0xfu) - (int)(b & 0xfu) - (int)c < 0) f |= EFLAG_AF;
  } else if (op == P5EMU_A64_ARITH_MUL) {
    unsigned __int128 p = (unsigned __int128)a * b;
    result = (uint64_t)p & m;
    second = (uint64_t)(p >> bits) & m;
    if (second != 0) f |= EFLAG_CF | EFLAG_OF;
  } else if (op == P5EMU_A64_ARITH_IMUL) {
    __int128 p = arith_sext128((unsigned __int128)a, bits) *
                 arith_sext128((unsigned __int128)b, bits);
    result = (uint64_t)p & m;
    second = (uint64_t)(p >> bits) & m;
    if (p != arith_sext128((unsigned __int128)result, bits)) {
      f |= EFLAG_CF | EFLAG_OF;
    }
  } else if (op == P5EMU_A64_ARITH_DIV || op == P5EMU_A64_ARITH_IDIV) {
    unsigned __int128 ud = ((unsigned __int128)a << bits) | b; /* a=hi b=lo */
    if (op == P5EMU_A64_ARITH_DIV) {
      if (c == 0) { *fault_out = P5EMU_A64_CPU_FAULT_DIVIDE; return 0; }
      unsigned __int128 q = ud / (unsigned __int128)c;
      unsigned __int128 r = ud % (unsigned __int128)c;
      if (q > (unsigned __int128)m) {
        *fault_out = P5EMU_A64_CPU_FAULT_DIVIDE;
        return 0;
      }
      result = (uint64_t)q & m;
      second = (uint64_t)r & m;
    } else {
      __int128 dividend = arith_sext128(ud, 2u * bits);
      __int128 d = arith_sext128((unsigned __int128)c, bits);
      if (d == 0) { *fault_out = P5EMU_A64_CPU_FAULT_DIVIDE; return 0; }
      __int128 q;
      __int128 r;
      if (d == -1) {
        if (dividend == ((__int128)1 << 127)) {
          *fault_out = P5EMU_A64_CPU_FAULT_DIVIDE;
          return 0;
        }
        q = -dividend;
        r = 0;
      } else {
        q = dividend / d;
        r = dividend % d;
      }
      __int128 qmin = -((__int128)1 << (bits - 1u));
      __int128 qmax = ((__int128)1 << (bits - 1u)) - 1;
      if (q < qmin || q > qmax) {
        *fault_out = P5EMU_A64_CPU_FAULT_DIVIDE;
        return 0;
      }
      result = (uint64_t)q & m;
      second = (uint64_t)r & m;
    }
  }

  /* ZF/SF/PF are defined only for ADC/SBB here; MUL/IMUL leave them undefined,
   * and DIV/IDIV write no defined flags at all. They are set identically on the
   * host side so the differential comparison agrees. */
  if (op == P5EMU_A64_ARITH_ADC || op == P5EMU_A64_ARITH_SBB) {
    if (result == 0) f |= EFLAG_ZF;
    if ((result >> (bits - 1u)) & 1u) f |= EFLAG_SF;
    if (arith_parity_even(result)) f |= EFLAG_PF;
  }

  if (out2) *out2 = second;
  if (flags_out) *flags_out = f;
  return result;
}

/* HLE dispatch (TEST INFRASTRUCTURE, P5EMU-owned synthetics). The trampoline
 * base/stride MUST match the host HleRegistry. NOT a PS5 address. */
#define HLE_BASE 0x70000000ull
#define HLE_STRIDE 8ull

static uint64_t hle_dispatch(uint64_t binding, uint64_t ctx_ptr,
                             uint64_t* status_out) {
  P5EmuArm64Ctx* ctx = (P5EmuArm64Ctx*)ctx_ptr;
  RegionTable* t = (RegionTable*)ctx->mem_handle;
  *status_out = P5EMU_A64_HLE_OK;
  if (binding == HLE_BASE + 0u * HLE_STRIDE) {
    return ctx->gpr[5] + ctx->gpr[4];  /* p5emu_add(rdi,rsi) */
  }
  if (binding == HLE_BASE + 1u * HLE_STRIDE) {
    return ctx->gpr[5] + ctx->gpr[4] + ctx->gpr[3] + ctx->gpr[2] +
           ctx->gpr[8] + ctx->gpr[9];  /* p5emu_sum6 */
  }
  if (binding == HLE_BASE + 2u * HLE_STRIDE) {
    uint64_t st = 0;
    uint64_t v = h_load((uint64_t)(uintptr_t)t, ctx->gpr[5], 8, &st);
    if (st != P5EMU_A64_MEM_OK) { *status_out = P5EMU_A64_HLE_MEMORY_FAULT; return 0; }
    v += ctx->gpr[4];
    h_store((uint64_t)(uintptr_t)t, ctx->gpr[3], 8, v, &st);
    if (st != P5EMU_A64_MEM_OK) { *status_out = P5EMU_A64_HLE_MEMORY_FAULT; return 0; }
    return v;  /* p5emu_memory(ptr,add,out) */
  }
  if (binding == HLE_BASE + 3u * HLE_STRIDE) {
    uint64_t sum = ctx->gpr[5] + ctx->gpr[4] + ctx->gpr[3] + ctx->gpr[2] +
                   ctx->gpr[8] + ctx->gpr[9];
    const uint64_t rsp = ctx->gpr[7];
    for (uint64_t i = 0; i < 4; ++i) {
      uint64_t st = 0;
      const uint64_t v = h_load((uint64_t)(uintptr_t)t, rsp + 8 + i * 8, 8, &st);
      if (st != P5EMU_A64_MEM_OK) { *status_out = P5EMU_A64_HLE_MEMORY_FAULT; return 0; }
      sum += v;
    }
    return sum;  /* p5emu_sum10 (args 6..9 from the stack) */
  }
  *status_out = P5EMU_A64_HLE_NOT_FOUND;
  return 0;
}

static uint8_t* read_file(const char* path, size_t* size_out) {
  FILE* f = fopen(path, "rb");
  if (f == NULL) return NULL;
  if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
  const long len = ftell(f);
  if (len < 0) { fclose(f); return NULL; }
  rewind(f);
  uint8_t* buf = (uint8_t*)malloc((size_t)len);
  if (buf == NULL) { fclose(f); return NULL; }
  if (fread(buf, 1, (size_t)len, f) != (size_t)len) { free(buf); fclose(f); return NULL; }
  fclose(f);
  *size_out = (size_t)len;
  return buf;
}

static int execute_record(const uint8_t* req, size_t req_size, size_t* off_io,
                          uint8_t** resp, size_t* resp_len, size_t* resp_cap) {
  size_t off = *off_io;
  if (off + 4 > req_size) return 1;
  if (req[off] != 'P' || req[off+1] != '5' || req[off+2] != 'A' || req[off+3] != '2') return 1;
  off += 4;
  if (rd_u32(req + off) != PROTOCOL_VERSION) return 1;
  off += 4;
  const uint64_t code_len = rd_u64(req + off); off += 8;
  const uint64_t step_limit = rd_u64(req + off); off += 8;

  P5EmuArm64Ctx ctx;
  memset(&ctx, 0, sizeof(ctx));
  for (int i = 0; i < P5EMU_A64_GPR_COUNT; ++i) { ctx.gpr[i] = rd_u64(req + off); off += 8; }
  ctx.rip = rd_u64(req + off); off += 8;
  ctx.eflags = rd_u64(req + off); off += 8;

  if (off + 4 > req_size) return 1;
  const uint32_t region_count = rd_u32(req + off); off += 4;
  if (region_count == 0 || region_count > MAX_REGIONS) return 1;

  static RegionTable table;
  table.count = region_count;
  uint64_t total = 0;
  for (uint32_t i = 0; i < region_count; ++i) {
    if (off + 20 > req_size) return 1;
    const uint64_t base = rd_u64(req + off); off += 8;
    const uint64_t size = rd_u64(req + off); off += 8;
    const uint32_t perms = rd_u32(req + off); off += 4;
    if (size > MAX_REGION_BYTES) return 1;
    total += size;
    if (total > (256ull << 20)) return 1;
    /* Reject overlap with an earlier region. */
    for (uint32_t j = 0; j < i; ++j) {
      const Region* r = &table.regions[j];
      if (base < r->base + r->size && r->base < base + size) return 1;
    }
    if (off + size > req_size) return 1;
    table.regions[i].base = base;
    table.regions[i].size = size;
    table.regions[i].perms = perms;
    table.regions[i].data = (uint8_t*)malloc(size ? (size_t)size : 1);
    if (table.regions[i].data == NULL) return 1;
    if (size) memcpy(table.regions[i].data, req + off, (size_t)size);
    off += size;
  }

  if (off + code_len > req_size) return 1;
  const uint8_t* code_in = req + off;
  off += code_len;
  *off_io = off;

  ctx.steps = step_limit;
  ctx.mem_handle = (uint64_t)(uintptr_t)&table;
  ctx.fn_load = (uint64_t)(uintptr_t)&h_load;
  ctx.fn_store = (uint64_t)(uintptr_t)&h_store;
  ctx.fn_hle = (uint64_t)(uintptr_t)&hle_dispatch;
  ctx.fn_arith = (uint64_t)(uintptr_t)&h_arith;

  const long page = sysconf(_SC_PAGESIZE);
  const size_t page_sz = (page > 0) ? (size_t)page : 4096u;
  const size_t map_size = ((code_len + page_sz - 1u) / page_sz) * page_sz;
  void* code_map = mmap(NULL, map_size, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (code_map == MAP_FAILED) return 1;
  memcpy(code_map, code_in, code_len);
  if (mprotect(code_map, map_size, PROT_READ | PROT_EXEC) != 0) {
    munmap(code_map, map_size);
    return 1;
  }
  p5emu_a64_block_fn fn = (p5emu_a64_block_fn)code_map;
  (void)fn(&ctx);
  munmap(code_map, map_size);

  /* Response size: header + region bytes. */
  size_t rec = 4 + 4 + 8 + 8 + 8 + ((size_t)P5EMU_A64_GPR_COUNT * 8) + 8 + 8;
  for (uint32_t i = 0; i < region_count; ++i) rec += (size_t)table.regions[i].size;
  if (*resp_len + rec > *resp_cap) {
    size_t cap = (*resp_cap == 0) ? 4096 : *resp_cap;
    while (cap < *resp_len + rec) cap *= 2;
    uint8_t* grown = (uint8_t*)realloc(*resp, cap);
    if (grown == NULL) return 1;
    *resp = grown; *resp_cap = cap;
  }
  uint8_t* w = *resp + *resp_len;
  size_t ro = 0;
  w[ro++]='P'; w[ro++]='5'; w[ro++]='A'; w[ro++]='2';
  wr_u32(w + ro, PROTOCOL_VERSION); ro += 4;
  wr_u64(w + ro, ctx.status); ro += 8;
  wr_u64(w + ro, ctx.fault_code); ro += 8;
  wr_u64(w + ro, ctx.steps); ro += 8;
  for (int i = 0; i < P5EMU_A64_GPR_COUNT; ++i) { wr_u64(w + ro, ctx.gpr[i]); ro += 8; }
  wr_u64(w + ro, ctx.rip); ro += 8;
  wr_u64(w + ro, ctx.eflags); ro += 8;
  for (uint32_t i = 0; i < region_count; ++i) {
    const size_t s = (size_t)table.regions[i].size;
    if (s) memcpy(w + ro, table.regions[i].data, s);
    ro += s;
    free(table.regions[i].data);
  }
  *resp_len += rec;
  return 0;
}

int main(int argc, char** argv) {
  if (argc != 3) {
    fprintf(stderr, "usage: %s <request> <response>\n", argv[0]);
    return 2;
  }
  size_t req_size = 0;
  uint8_t* req = read_file(argv[1], &req_size);
  if (req == NULL) { fprintf(stderr, "harness: cannot read request\n"); return 2; }

  uint8_t* resp = NULL;
  size_t resp_len = 0, resp_cap = 0;
  size_t off = 0;
  while (off < req_size) {
    if (execute_record(req, req_size, &off, &resp, &resp_len, &resp_cap) != 0) {
      fprintf(stderr, "harness: malformed request record at offset %zu\n", off);
      free(resp); free(req); return 2;
    }
  }
  FILE* out = fopen(argv[2], "wb");
  if (out == NULL || fwrite(resp, 1, resp_len, out) != resp_len) {
    fprintf(stderr, "harness: cannot write response\n");
    if (out) fclose(out);
    free(resp); free(req); return 2;
  }
  fclose(out);
  free(resp); free(req);
  return 0;
}
