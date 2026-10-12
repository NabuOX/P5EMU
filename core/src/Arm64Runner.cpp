// SPDX-License-Identifier: Apache-2.0
//
// P5EMU is an independent, open-source project. See LICENSE and
// PROVENANCE.md before adding code here.

#include "p5emu/cpu/Arm64Runner.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#else
#include <process.h>
#endif

namespace p5emu::cpu {
namespace {

constexpr char kMagic[4] = {'P', '5', 'A', '2'};
constexpr std::uint32_t kProtocolVersion = 2;

void AppendU32(std::vector<std::uint8_t>* b, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) {
    b->push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFu));
  }
}
void AppendU64(std::vector<std::uint8_t>* b, std::uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    b->push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFu));
  }
}

bool ReadU32(const std::vector<std::uint8_t>& b, std::size_t* off,
             std::uint32_t* out) {
  if (*off + 4 > b.size()) return false;
  std::uint32_t v = 0;
  for (int i = 0; i < 4; ++i) {
    v |= static_cast<std::uint32_t>(b[*off + static_cast<std::size_t>(i)])
         << (8 * i);
  }
  *off += 4;
  *out = v;
  return true;
}
bool ReadU64(const std::vector<std::uint8_t>& b, std::size_t* off,
             std::uint64_t* out) {
  if (*off + 8 > b.size()) return false;
  std::uint64_t v = 0;
  for (int i = 0; i < 8; ++i) {
    v |= static_cast<std::uint64_t>(b[*off + static_cast<std::size_t>(i)])
         << (8 * i);
  }
  *off += 8;
  *out = v;
  return true;
}

bool FileExists(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  return f.good();
}

std::string RunDir() {
#if defined(__unix__) || defined(__APPLE__)
  const char* tmp = std::getenv("TMPDIR");
  std::string dir = (tmp != nullptr && *tmp != '\0') ? tmp : "/tmp";
  return dir;
#else
  const char* tmp = std::getenv("TEMP");
  return (tmp != nullptr && *tmp != '\0') ? tmp : ".";
#endif
}

int ProcessId() {
#if defined(__unix__) || defined(__APPLE__)
  return static_cast<int>(::getpid());
#else
  return static_cast<int>(::_getpid());
#endif
}

std::string Quote(const std::string& s) { return "\"" + s + "\""; }

}  // namespace

std::vector<Arm64MemoryRegion> Arm64BatchCase::EffectiveRegions() const {
  if (!regions.empty()) {
    return regions;
  }
  Arm64MemoryRegion r;
  r.base = mem_base;
  r.permissions = memory::kPermReadWriteExecute;
  r.bytes = mem_in;
  return {std::move(r)};
}

Status Arm64Runner::RunRegions(
    const std::vector<std::uint8_t>& code, P5EmuArm64Ctx* ctx,
    const std::vector<Arm64MemoryRegion>& regions_in,
    std::vector<std::vector<std::uint8_t>>* regions_out, std::string* detail) {
  if (ctx == nullptr || regions_out == nullptr) {
    return StatusCode::kInvalidArgument;
  }
  Arm64BatchCase c;
  c.code = code;
  c.ctx = *ctx;
  c.regions = regions_in;
  std::vector<Arm64BatchResult> results;
  const Status st = RunBatch({std::move(c)}, &results, detail);
  if (!st.ok()) {
    return st;
  }
  *ctx = results.front().ctx;
  *regions_out = std::move(results.front().regions_out);
  return StatusCode::kOk;
}

QemuArm64Runner::QemuArm64Runner(std::string harness_path,
                                 std::string emulator_path)
    : harness_path_(std::move(harness_path)),
      emulator_path_(std::move(emulator_path)) {}

bool QemuArm64Runner::available() const {
  return !harness_path_.empty() && FileExists(harness_path_);
}

Status Arm64Runner::RunBatch(const std::vector<Arm64BatchCase>& cases,
                             std::vector<Arm64BatchResult>* out,
                             std::string* detail) {
  if (out == nullptr) {
    return StatusCode::kInvalidArgument;
  }
  out->clear();
  out->reserve(cases.size());
  for (const Arm64BatchCase& c : cases) {
    const std::vector<Arm64MemoryRegion> regions = c.EffectiveRegions();
    if (regions.size() != 1) {
      if (detail != nullptr) {
        *detail = "this runner transports a single region only";
      }
      return StatusCode::kUnimplementedInstruction;
    }
    Arm64BatchResult r;
    r.ctx = c.ctx;
    const Status st = Run(c.code, &r.ctx, regions.front().base,
                          regions.front().bytes, &r.mem_out, detail);
    if (!st.ok()) {
      return st;
    }
    out->push_back(std::move(r));
  }
  return StatusCode::kOk;
}

namespace {

// Appends one request record for `c` to `req`.
void AppendCase(std::vector<std::uint8_t>* req, const Arm64BatchCase& c) {
  const std::vector<Arm64MemoryRegion> regions = c.EffectiveRegions();
  req->insert(req->end(), kMagic, kMagic + 4);
  AppendU32(req, kProtocolVersion);
  AppendU64(req, c.code.size());
  AppendU64(req, c.ctx.steps);
  for (int i = 0; i < P5EMU_A64_GPR_COUNT; ++i) {
    AppendU64(req, c.ctx.gpr[i]);
  }
  AppendU64(req, c.ctx.rip);
  AppendU64(req, c.ctx.eflags);
  AppendU32(req, static_cast<std::uint32_t>(regions.size()));
  for (const Arm64MemoryRegion& r : regions) {
    AppendU64(req, r.base);
    AppendU64(req, static_cast<std::uint64_t>(r.bytes.size()));
    AppendU32(req, r.permissions);
    req->insert(req->end(), r.bytes.begin(), r.bytes.end());
  }
  req->insert(req->end(), c.code.begin(), c.code.end());
}

}  // namespace

Status QemuArm64Runner::RunBatch(const std::vector<Arm64BatchCase>& cases,
                                 std::vector<Arm64BatchResult>* out,
                                 std::string* detail) {
  if (out == nullptr) {
    return StatusCode::kInvalidArgument;
  }
  if (cases.empty()) {
    out->clear();
    return StatusCode::kOk;
  }
  if (!available()) {
    if (detail != nullptr) {
      *detail = "AArch64 harness not found: " + harness_path_;
    }
    return StatusCode::kUnimplemented;
  }

  std::vector<std::uint8_t> req;
  for (const Arm64BatchCase& c : cases) {
    AppendCase(&req, c);
  }

  static int counter = 0;
  const std::string base = RunDir() + "/p5emu_a64_batch_" +
                           std::to_string(ProcessId()) + "_" +
                           std::to_string(counter++);
  const std::string req_path = base + ".req";
  const std::string resp_path = base + ".resp";

  {
    std::ofstream f(req_path, std::ios::binary | std::ios::trunc);
    if (!f.good()) {
      if (detail != nullptr) *detail = "cannot write request file " + req_path;
      return StatusCode::kInternal;
    }
    f.write(reinterpret_cast<const char*>(req.data()),
            static_cast<std::streamsize>(req.size()));
    if (!f.good()) {
      if (detail != nullptr) *detail = "short write of request file";
      return StatusCode::kInternal;
    }
  }

  const std::string cmd = Quote(emulator_path_) + " " + Quote(harness_path_) +
                          " " + Quote(req_path) + " " + Quote(resp_path) +
                          " >/dev/null 2>&1";
  const int rc = std::system(cmd.c_str());
  std::remove(req_path.c_str());
  if (rc != 0) {
    std::remove(resp_path.c_str());
    if (detail != nullptr) {
      *detail = "AArch64 harness exited with status " + std::to_string(rc);
    }
    return StatusCode::kBackendError;
  }

  std::vector<std::uint8_t> resp;
  {
    std::ifstream in(resp_path, std::ios::binary);
    if (!in.good()) {
      if (detail != nullptr) *detail = "harness produced no response file";
      return StatusCode::kBackendError;
    }
    in.seekg(0, std::ios::end);
    const std::streamoff len = in.tellg();
    in.seekg(0, std::ios::beg);
    resp.resize(static_cast<std::size_t>(len));
    in.read(reinterpret_cast<char*>(resp.data()), len);
  }
  std::remove(resp_path.c_str());

  out->clear();
  out->reserve(cases.size());
  std::size_t off = 0;
  for (const Arm64BatchCase& c : cases) {
    if (resp.size() < off + 4 || std::memcmp(resp.data() + off, kMagic, 4) != 0) {
      if (detail != nullptr) *detail = "harness response has a bad magic";
      return StatusCode::kBackendError;
    }
    off += 4;
    std::uint32_t version = 0;
    Arm64BatchResult r;
    if (!ReadU32(resp, &off, &version) || version != kProtocolVersion) {
      if (detail != nullptr) *detail = "harness response version mismatch";
      return StatusCode::kBackendError;
    }
    if (!ReadU64(resp, &off, &r.ctx.status) ||
        !ReadU64(resp, &off, &r.ctx.fault_code) ||
        !ReadU64(resp, &off, &r.ctx.steps)) {
      if (detail != nullptr) *detail = "harness response is truncated";
      return StatusCode::kBackendError;
    }
    for (int i = 0; i < P5EMU_A64_GPR_COUNT; ++i) {
      if (!ReadU64(resp, &off, &r.ctx.gpr[i])) {
        if (detail != nullptr) *detail = "harness response is truncated (gpr)";
        return StatusCode::kBackendError;
      }
    }
    if (!ReadU64(resp, &off, &r.ctx.rip) ||
        !ReadU64(resp, &off, &r.ctx.eflags)) {
      if (detail != nullptr) *detail = "harness response is truncated";
      return StatusCode::kBackendError;
    }
    if (resp.size() - off < c.mem_in.size()) {
      if (detail != nullptr) *detail = "harness response is truncated (memory)";
      return StatusCode::kBackendError;
    }
    const std::vector<Arm64MemoryRegion> regions = c.EffectiveRegions();
    r.regions_out.reserve(regions.size());
    for (const Arm64MemoryRegion& reg : regions) {
      if (resp.size() - off < reg.bytes.size()) {
        if (detail != nullptr) *detail = "harness response is truncated (region)";
        return StatusCode::kBackendError;
      }
      r.regions_out.emplace_back(
          resp.begin() + static_cast<std::ptrdiff_t>(off),
          resp.begin() +
              static_cast<std::ptrdiff_t>(off + reg.bytes.size()));
      off += reg.bytes.size();
    }
    if (regions.size() == 1) {
      r.mem_out = r.regions_out.front();
    }
    out->push_back(std::move(r));
  }
  return StatusCode::kOk;
}

Status QemuArm64Runner::Run(const std::vector<std::uint8_t>& code,
                            P5EmuArm64Ctx* ctx, memory::GuestAddr mem_base,
                            const std::vector<std::uint8_t>& mem_in,
                            std::vector<std::uint8_t>* mem_out,
                            std::string* detail) {
  if (ctx == nullptr || mem_out == nullptr) {
    return StatusCode::kInvalidArgument;
  }
  std::vector<Arm64MemoryRegion> regions;
  {
    Arm64MemoryRegion r;
    r.base = mem_base;
    r.permissions = memory::kPermReadWriteExecute;
    r.bytes = mem_in;
    regions.push_back(std::move(r));
  }
  std::vector<std::vector<std::uint8_t>> regions_out;
  const Status st = RunRegions(code, ctx, regions, &regions_out, detail);
  if (!st.ok()) {
    return st;
  }
  *mem_out = std::move(regions_out.front());
  return StatusCode::kOk;
}

std::unique_ptr<Arm64Runner> MakeArm64RunnerFromEnvironment() {
  const char* harness = std::getenv("P5EMU_A64_HARNESS");
  if (harness == nullptr || *harness == '\0') {
    return nullptr;
  }
  const char* qemu = std::getenv("P5EMU_A64_QEMU");
  const std::string emulator = (qemu != nullptr && *qemu != '\0')
                                   ? std::string(qemu)
                                   : std::string("qemu-aarch64");
  auto runner = std::make_unique<QemuArm64Runner>(std::string(harness), emulator);
  if (!runner->available()) {
    return nullptr;
  }
  return runner;
}

}  // namespace p5emu::cpu
