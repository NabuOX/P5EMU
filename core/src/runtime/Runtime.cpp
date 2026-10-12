// SPDX-License-Identifier: Apache-2.0
//
// P5EMU is an independent, open-source project. See LICENSE and
// PROVENANCE.md before adding code here.

#include "p5emu/runtime/Runtime.h"

#include <algorithm>
#include <set>

#include "p5emu/cpu/TranslateToIr.h"

namespace p5emu::runtime {
namespace cpu = p5emu::cpu;
namespace {

bool Overlap(memory::GuestAddr a0, memory::GuestSize as,
             memory::GuestAddr b0, memory::GuestSize bs) noexcept {
  const memory::GuestAddr a1 = a0 + as;
  const memory::GuestAddr b1 = b0 + bs;
  return a0 < b1 && b0 < a1;
}

std::uint64_t MaxValueId(const ir::Program& p) {
  std::uint64_t m = 0;
  for (const ir::Block& b : p.blocks) {
    for (const ir::Op& op : b.ops) {
      if (op.dest != ir::kInvalidValue && op.dest > m) m = op.dest;
      for (const ir::ValueId a : op.args) {
        if (a != ir::kInvalidValue && a > m) m = a;
      }
    }
    if (b.terminator.cond_value != ir::kInvalidValue &&
        b.terminator.cond_value > m) {
      m = b.terminator.cond_value;
    }
  }
  return m;
}

}  // namespace

// ---------------------------------------------------------------------------
// AddressSpace
// ---------------------------------------------------------------------------
Status AddressSpace::Add(const AddressRegion& region) {
  if (region.size == 0) return StatusCode::kInvalidArgument;
  if (region.base > (memory::GuestMemoryPort::kMaxGuestAddr - region.size)) {
    return StatusCode::kIntegerOverflow;
  }
  for (const AddressRegion& r : regions_) {
    if (Overlap(region.base, region.size, r.base, r.size)) {
      return StatusCode::kOverlappingMapping;
    }
  }
  regions_.push_back(region);
  return StatusCode::kOk;
}

const AddressRegion* AddressSpace::Find(memory::GuestAddr addr) const noexcept {
  for (const AddressRegion& r : regions_) {
    if (addr >= r.base && (addr - r.base) < r.size) return &r;
  }
  return nullptr;
}

bool AddressSpace::Overlaps(memory::GuestAddr base,
                            memory::GuestSize size) const noexcept {
  for (const AddressRegion& r : regions_) {
    if (Overlap(base, size, r.base, r.size)) return true;
  }
  return false;
}

std::uint64_t AddressSpace::TotalBytes() const noexcept {
  std::uint64_t t = 0;
  for (const AddressRegion& r : regions_) t += r.size;
  return t;
}

// ---------------------------------------------------------------------------
// GuestStack
// ---------------------------------------------------------------------------
Status GuestStack::Create(memory::GuestAddr base, memory::GuestSize size) {
  if (size == 0) return StatusCode::kInvalidArgument;
  if (base > (memory::GuestMemoryPort::kMaxGuestAddr - size)) {
    return StatusCode::kIntegerOverflow;
  }
  region_ = AddressRegion{base, size, memory::kPermReadWrite, kNoOwner,
                          "stack"};
  return StatusCode::kOk;
}

Status GuestStack::Push64(memory::GuestMemoryPort* memory,
                          memory::GuestAddr* rsp, std::uint64_t value) const {
  if (memory == nullptr || rsp == nullptr) return StatusCode::kInvalidArgument;
  if (*rsp < region_.base + 8) return StatusCode::kUnmappedAccess;  // underflow
  *rsp -= 8;
  return memory->WriteData(*rsp, 8, value);
}

Status GuestStack::Pop64(memory::GuestMemoryPort* memory,
                         memory::GuestAddr* rsp, std::uint64_t* value) const {
  if (memory == nullptr || rsp == nullptr || value == nullptr) {
    return StatusCode::kInvalidArgument;
  }
  if (*rsp > top() - 8) return StatusCode::kUnmappedAccess;  // overrun
  const Status st = memory->ReadData(*rsp, 8, value);
  if (!st.ok()) return st;
  *rsp += 8;
  return StatusCode::kOk;
}

// ---------------------------------------------------------------------------
// Module lifecycle
// ---------------------------------------------------------------------------
const char* ModuleStateName(ModuleState state) noexcept {
  switch (state) {
    case ModuleState::kParsed: return "parsed";
    case ModuleState::kValidated: return "validated";
    case ModuleState::kPlanned: return "planned";
    case ModuleState::kResolved: return "resolved";
    case ModuleState::kMapped: return "mapped";
    case ModuleState::kReady: return "ready";
    case ModuleState::kRunning: return "running";
  }
  return "unknown";
}

bool IsValidModuleTransition(ModuleState from, ModuleState to) noexcept {
  // Forward-only, exactly one step.
  return static_cast<std::uint8_t>(to) ==
         static_cast<std::uint8_t>(from) + 1u;
}

// ---------------------------------------------------------------------------
// Repository
// ---------------------------------------------------------------------------
void InMemoryModuleRepository::Add(std::string name,
                                   std::vector<std::uint8_t> bytes) {
  if (modules_.find(name) == modules_.end()) order_.push_back(name);
  modules_[std::move(name)] = std::move(bytes);
}

const std::vector<std::uint8_t>* InMemoryModuleRepository::Fetch(
    const std::string& name) const {
  const auto it = modules_.find(name);
  return (it == modules_.end()) ? nullptr : &it->second;
}

// ---------------------------------------------------------------------------
// Dependency graph
// ---------------------------------------------------------------------------
Status BuildDependencyGraph(
    const std::string& root,
    const std::map<std::string, std::vector<std::string>>& deps,
    const GraphOptions& options, DependencyGraph* out) {
  if (out == nullptr) return StatusCode::kInvalidArgument;
  *out = DependencyGraph{};

  std::set<std::string> visited;  // fully processed (black)
  std::set<std::string> on_stack; // gray
  std::vector<std::string> order;

  // Iterative DFS with an explicit cycle path.
  struct Frame {
    std::string name;
    std::size_t child = 0;
    std::vector<std::string> children;
  };
  std::vector<Frame> stack;
  stack.push_back(Frame{root, 0, {}});
  on_stack.insert(root);

  // Seed the root's children.
  {
    const auto it = deps.find(root);
    if (it != deps.end()) stack.back().children = it->second;
  }

  while (!stack.empty()) {
    Frame& f = stack.back();
    if (f.child < f.children.size()) {
      const std::string dep = f.children[f.child++];
      if (on_stack.count(dep) != 0) {
        // Cycle: reconstruct path.
        std::string cyc;
        bool started = false;
        for (const Frame& fr : stack) {
          if (fr.name == dep) started = true;
          if (started) cyc += fr.name + " -> ";
        }
        cyc += dep;
        out->cycle = cyc;
        return StatusCode::kMalformedExecutable;
      }
      if (visited.count(dep) != 0) continue;  // already loaded once
      const auto it = deps.find(dep);
      if (it == deps.end()) {
        // Not declared: treat as missing (repository must provide it / deps).
        out->missing.push_back(dep);
        continue;
      }
      stack.push_back(Frame{dep, 0, it->second});
      on_stack.insert(dep);
      continue;
    }
    // All children done: post-order -> dependencies before dependents.
    visited.insert(f.name);
    on_stack.erase(f.name);
    order.push_back(f.name);
    stack.pop_back();
  }

  if (!out->missing.empty()) {
    std::sort(out->missing.begin(), out->missing.end());
    out->missing.erase(std::unique(out->missing.begin(), out->missing.end()),
                       out->missing.end());
    return StatusCode::kUnsupportedRequiredFeature;
  }

  if (options.inject_reverse_order) {
    std::reverse(order.begin(), order.end());
  }
  out->order = std::move(order);
  return StatusCode::kOk;
}

// ---------------------------------------------------------------------------
// Load-bias allocation
// ---------------------------------------------------------------------------
Status LoadBiasAllocator::Allocate(memory::GuestSize span,
                                   memory::GuestAddr* bias_out) {
  if (bias_out == nullptr) return StatusCode::kInvalidArgument;
  if (span > kStride) return StatusCode::kImageTooLarge;
  if (next_ + kStride > kEnd) return StatusCode::kIntegerOverflow;
  *bias_out = next_;
  if (!inject_overlap_) {
    next_ += kStride;  // TEST-ONLY defect: do not advance -> overlapping slots
  }
  return StatusCode::kOk;
}

// ---------------------------------------------------------------------------
// Cross-module resolution
// ---------------------------------------------------------------------------
const char* ResolutionSourceName(ResolutionSource source) noexcept {
  switch (source) {
    case ResolutionSource::kNone: return "none";
    case ResolutionSource::kGuestExport: return "guest-export";
    case ResolutionSource::kHle: return "hle";
  }
  return "none";
}

ProcessResolution ResolveIdentity(
    const std::vector<LoadedModule>& modules, const hle::HleRegistry* hle_reg,
    const loader::SymbolIdentity& symbol, const ResolverOptions& options) {
  ProcessResolution r;

  // Count guest exports with this identity.
  const ExportedSymbol* guest = nullptr;
  int guest_matches = 0;
  for (const LoadedModule& m : modules) {
    for (const ExportedSymbol& e : m.exports) {
      if (e.symbol.kind == symbol.kind && e.symbol.name == symbol.name) {
        guest = &e;
        ++guest_matches;
      }
    }
  }
  if (guest_matches > 1) {
    r.status = StatusCode::kAmbiguousImport;
    return r;
  }

  hle::HleEntry const* hle_entry = nullptr;
  if (hle_reg != nullptr) {
    // An import requirement carries no module attribution (generic ELF), so HLE
    // is matched by symbol name across modules; more than one match is ambiguous.
    int hle_matches = 0;
    for (const hle::HleEntry& e : hle_reg->entries()) {
      if (e.symbol.kind == symbol.kind && e.symbol.name == symbol.name) {
        hle_entry = &e;
        ++hle_matches;
      }
    }
    if (hle_matches > 1) {
      r.status = StatusCode::kAmbiguousImport;
      return r;
    }
  }

  // Explicit priority. Never accidental.
  const bool hle_wins =
      options.inject_hle_override || options.policy == ResolutionPolicy::kHleFirst;

  if (guest != nullptr && hle_entry != nullptr && hle_wins) {
    r.source = ResolutionSource::kHle;
    r.address = hle_entry->trampoline;
    return r;
  }
  if (guest != nullptr) {
    r.source = ResolutionSource::kGuestExport;
    r.address = guest->guest_va;
    return r;
  }
  if (hle_entry != nullptr) {
    r.source = ResolutionSource::kHle;
    r.address = hle_entry->trampoline;
    return r;
  }
  r.status = StatusCode::kUnresolvedImport;
  return r;
}

// ---------------------------------------------------------------------------
// Merge / link
// ---------------------------------------------------------------------------
Status MergePrograms(const std::vector<const ir::Program*>& programs,
                     std::size_t entry_index, ir::Program* out) {
  if (out == nullptr || entry_index >= programs.size()) {
    return StatusCode::kInvalidArgument;
  }
  *out = ir::Program{};
  std::uint64_t block_offset = 0;
  std::uint64_t value_offset = 0;
  for (std::size_t pi = 0; pi < programs.size(); ++pi) {
    const ir::Program* p = programs[pi];
    if (p == nullptr) return StatusCode::kInvalidArgument;
    for (const ir::Block& b : p->blocks) {
      ir::Block nb = b;
      nb.id = static_cast<ir::BlockId>(b.id + block_offset);
      const auto rblock = [&](ir::BlockId id) -> ir::BlockId {
        return id == ir::kInvalidBlock
                   ? id
                   : static_cast<ir::BlockId>(id + block_offset);
      };
      nb.terminator.target = rblock(nb.terminator.target);
      nb.terminator.fallthrough = rblock(nb.terminator.fallthrough);
      if (nb.terminator.cond_value != ir::kInvalidValue) {
        nb.terminator.cond_value =
            static_cast<ir::ValueId>(nb.terminator.cond_value + value_offset);
      }
      for (ir::Op& op : nb.ops) {
        if (op.dest != ir::kInvalidValue) {
          op.dest = static_cast<ir::ValueId>(op.dest + value_offset);
        }
        for (ir::ValueId& a : op.args) {
          if (a != ir::kInvalidValue) {
            a = static_cast<ir::ValueId>(a + value_offset);
          }
        }
      }
      out->blocks.push_back(std::move(nb));
    }
    if (pi == entry_index) {
      out->entry = static_cast<ir::BlockId>(p->entry + block_offset);
    }
    block_offset += p->blocks.size();
    value_offset += MaxValueId(*p) + 1u;
  }
  out->name = programs[entry_index]->name;
  return StatusCode::kOk;
}

void LinkGuestCalls(ir::Program* program) {
  for (ir::Block& b : program->blocks) {
    if (b.terminator.kind != ir::TerminatorKind::kExternalCall) continue;
    const ir::Block* target =
        program->FindBlockByAddress(b.terminator.external_target);
    if (target == nullptr) continue;  // an HLE binding: leave as-is
    // A cross-module guest call: it is ordinary CALL/RET control flow.
    b.terminator.kind = ir::TerminatorKind::kJump;
    b.terminator.target = target->id;
    b.terminator.fallthrough = ir::kInvalidBlock;
    b.terminator.external_target = 0;
  }
}

// ---------------------------------------------------------------------------
// Initial stack builder
// ---------------------------------------------------------------------------
namespace {

Status StackWrite(memory::GuestMemoryPort* mem, memory::GuestAddr addr,
                  const std::uint8_t* p, std::uint64_t len) {
  for (std::uint64_t i = 0; i < len; ++i) {
    const Status st = mem->WriteData(addr + i, 1, p[i]);
    if (!st.ok()) return st;
  }
  return StatusCode::kOk;
}

}  // namespace

Status BuildInitialStack(memory::GuestMemoryPort* memory, const GuestStack& stack,
                         const std::vector<std::string>& args,
                         const std::vector<std::string>& env,
                         const std::vector<std::pair<std::uint64_t, std::uint64_t>>& auxv,
                         const StartupOptions& options, ProcessStartup* out) {
  if (memory == nullptr || out == nullptr) return StatusCode::kInvalidArgument;
  *out = ProcessStartup{};

  memory::GuestAddr cursor = stack.top();
  const auto need = [&](memory::GuestSize n) -> Status {
    if (cursor < stack.base() + n) return StatusCode::kImageTooLarge;
    return StatusCode::kOk;
  };

  // 1. Strings at the highest addresses.
  std::vector<memory::GuestAddr> arg_ptr;
  arg_ptr.reserve(args.size());
  for (const std::string& s : args) {
    const std::uint64_t len = s.size() + 1;  // include NUL
    const Status n = need(len);
    if (!n.ok()) return n;
    cursor -= len;
    const Status w = StackWrite(
        memory, cursor, reinterpret_cast<const std::uint8_t*>(s.c_str()), len);
    if (!w.ok()) return w;
    arg_ptr.push_back(cursor);
  }
  std::vector<memory::GuestAddr> env_ptr;
  env_ptr.reserve(env.size());
  for (const std::string& s : env) {
    const std::uint64_t len = s.size() + 1;
    const Status n = need(len);
    if (!n.ok()) return n;
    cursor -= len;
    const Status w = StackWrite(
        memory, cursor, reinterpret_cast<const std::uint8_t*>(s.c_str()), len);
    if (!w.ok()) return w;
    env_ptr.push_back(cursor);
  }

  // Align down to 16 before the pointer arrays.
  cursor &= ~memory::GuestAddr{15};

  // 2. Auxiliary vector (AT_NULL terminated), written in order.
  if (options.append_auxv_null) {
    const Status n = need(16);
    if (!n.ok()) return n;
    cursor -= 16;
    const std::uint8_t zero[16] = {0};
    const Status w = StackWrite(memory, cursor, zero, 16);
    if (!w.ok()) return w;
  }
  for (auto it = auxv.rbegin(); it != auxv.rend(); ++it) {
    const Status n = need(16);
    if (!n.ok()) return n;
    cursor -= 16;
    std::uint8_t b[16];
    for (int i = 0; i < 8; ++i) {
      b[i] = static_cast<std::uint8_t>((it->first >> (8 * i)) & 0xFFu);
      b[8 + i] = static_cast<std::uint8_t>((it->second >> (8 * i)) & 0xFFu);
    }
    const Status w = StackWrite(memory, cursor, b, 16);
    if (!w.ok()) return w;
  }

  // 3. envp array (NULL terminated).
  if (options.append_null_envp) {
    const Status n = need(8);
    if (!n.ok()) return n;
    cursor -= 8;
    const std::uint8_t zero[8] = {0};
    const Status w = StackWrite(memory, cursor, zero, 8);
    if (!w.ok()) return w;
  }
  for (auto it = env_ptr.rbegin(); it != env_ptr.rend(); ++it) {
    const Status n = need(8);
    if (!n.ok()) return n;
    cursor -= 8;
    std::uint8_t b[8];
    for (int i = 0; i < 8; ++i) {
      b[i] = static_cast<std::uint8_t>((*it >> (8 * i)) & 0xFFu);
    }
    const Status w = StackWrite(memory, cursor, b, 8);
    if (!w.ok()) return w;
  }

  // 4. argv array (NULL terminated).
  if (options.append_null_argv) {
    const Status n = need(8);
    if (!n.ok()) return n;
    cursor -= 8;
    const std::uint8_t zero[8] = {0};
    const Status w = StackWrite(memory, cursor, zero, 8);
    if (!w.ok()) return w;
  }
  out->argv = arg_ptr;  // argv[0..n-1] in order
  for (auto it = arg_ptr.rbegin(); it != arg_ptr.rend(); ++it) {
    const Status n = need(8);
    if (!n.ok()) return n;
    cursor -= 8;
    std::uint8_t b[8];
    for (int i = 0; i < 8; ++i) {
      b[i] = static_cast<std::uint8_t>((*it >> (8 * i)) & 0xFFu);
    }
    const Status w = StackWrite(memory, cursor, b, 8);
    if (!w.ok()) return w;
  }

  // 5. argc.
  {
    const Status n = need(8);
    if (!n.ok()) return n;
    cursor -= 8;
    const std::uint64_t argc =
        options.argc_override != 0 ? options.argc_override : args.size();
    std::uint8_t b[8];
    for (int i = 0; i < 8; ++i) {
      b[i] = static_cast<std::uint8_t>((argc >> (8 * i)) & 0xFFu);
    }
    const Status w = StackWrite(memory, cursor, b, 8);
    if (!w.ok()) return w;
    out->argc = argc;
  }

  // 6. 16-byte aligned RSP.
  cursor &= ~memory::GuestAddr{15};
  out->initial_rsp = cursor;
  out->envp = env_ptr;
  out->auxv = auxv;
  return StatusCode::kOk;
}

// ---------------------------------------------------------------------------
// Process load
// ---------------------------------------------------------------------------
namespace {

// Parses one module and builds its plan at the given bias.
Status ParseAndPlan(const loader::BinaryReader& reader, std::uint64_t bias,
                    LoadedModule* m) {
  loader::GenericElfImage elf;
  Status st = loader::ReadGenericElf64(reader, &elf);
  if (!st.ok()) return st;
  st = loader::ValidateGenericLoadSegments(reader, elf);
  if (!st.ok()) return st;
  loader::Ps5ExecutableImage ps5;
  st = loader::ParsePs5Executable(reader, elf, &ps5);
  if (!st.ok()) return st;
  st = loader::BuildLoadPlan(reader, ps5, bias, &m->plan);
  if (!st.ok()) return st;
  st = loader::ValidateLoadPlan(m->plan);
  if (!st.ok()) return st;
  m->imports = m->plan.import_requirements;
  // Exports: this module's DEFINED dynsym symbols (generic; no Sony layout).
  for (const loader::ElfSymbol& sym : m->plan.symbols) {
    if (sym.IsImport() || sym.name.empty()) continue;
    ExportedSymbol e;
    e.module_id = m->id;
    e.symbol = loader::SymbolIdentity::String(sym.name);
    e.guest_va = sym.elf_value + bias;
    m->exports.push_back(std::move(e));
  }
  return StatusCode::kOk;
}

}  // namespace

Status LoadProcess(const ModuleRepository& repo, const std::string& root_name,
                   const std::map<std::string, std::vector<std::string>>& deps,
                   const GuestStack& stack, const hle::HleRegistry* hle_reg,
                   const LoadProcessOptions& options, LoadedProcess* out) {
  if (out == nullptr) return StatusCode::kInvalidArgument;
  *out = LoadedProcess{};
  out->status = StatusCode::kOk;

  GraphOptions go;
  go.inject_reverse_order = options.inject_reverse_deps;
  DependencyGraph graph;
  {
    const Status gs = BuildDependencyGraph(root_name, deps, go, &graph);
    if (!gs.ok()) {
      out->status = gs;
      for (const std::string& m : graph.missing) {
        out->blockers.push_back("missing module: " + m);
      }
      if (!graph.cycle.empty()) {
        out->blockers.push_back("dependency cycle: " + graph.cycle);
      }
      return gs;
    }
  }

  // ---- Stage 1: parse all modules, allocate biases, plan, address space ----
  LoadBiasAllocator alloc;
  if (options.inject_overlap_bias) alloc.SetInjectOverlap(true);
  AddressSpace space;

  std::vector<LoadedModule> modules;
  std::vector<std::vector<std::uint8_t>> keepalive;  // backing bytes
  keepalive.reserve(graph.order.size());
  modules.reserve(graph.order.size());

  for (std::size_t i = 0; i < graph.order.size(); ++i) {
    const std::string& name = graph.order[i];
    const std::vector<std::uint8_t>* bytes = repo.Fetch(name);
    if (bytes == nullptr) {
      out->status = StatusCode::kUnsupportedRequiredFeature;
      out->blockers.push_back("module not fetchable: " + name);
      return out->status;
    }
    keepalive.push_back(*bytes);
    const loader::BinaryReader reader(keepalive.back().data(),
                                      keepalive.back().size());

    // Span from the ELF program headers (bounded).
    loader::GenericElfImage elf;
    Status st = loader::ReadGenericElf64(reader, &elf);
    if (!st.ok()) { out->status = st; return st; }
    memory::GuestSize span = 0;
    for (const loader::Elf64ProgramHeader& ph : elf.program_headers) {
      if (ph.type == loader::kPtLoad) {
        const memory::GuestSize end = ph.vaddr + ph.memsz;
        if (end > span) span = end;
      }
    }
    if (span == 0) span = 0x1000;

    memory::GuestAddr bias = 0;
    st = alloc.Allocate(span, &bias);
    if (!st.ok()) { out->status = st; return st; }

    LoadedModule m;
    m.id = static_cast<std::uint32_t>(i);
    m.name = name;
    m.load_bias = bias;
    m.state = ModuleState::kParsed;
    st = ParseAndPlan(reader, bias, &m);
    if (!st.ok()) { out->status = st; return st; }
    m.state = ModuleState::kPlanned;

    // Address-space regions (module segments + stack + HLE trampolines).
    for (const loader::PlannedSegment& s : m.plan.segments) {
      AddressRegion ar;
      ar.base = s.vaddr;
      ar.size = s.memsz;
      ar.permissions = s.permissions;
      ar.owner_module = m.id;
      ar.kind = "module";
      st = space.Add(ar);
      if (!st.ok()) {
        out->status = st;
        out->blockers.push_back("module region overlap in " + name);
        return st;
      }
    }
    modules.push_back(std::move(m));
  }

  // Stack region.
  if (stack.size() != 0) {
    const Status ss = space.Add(stack.region());
    if (!ss.ok()) { out->status = ss; return ss; }
  }
  // HLE trampoline region (reserved; no memory).
  {
    AddressRegion tr;
    tr.base = hle::HleRegistry::kTrampolineBase;
    tr.size = hle::HleRegistry::kTrampolineEnd - hle::HleRegistry::kTrampolineBase;
    tr.permissions = memory::kPermRead | memory::kPermExecute;
    tr.kind = "trampoline";
    const Status ts = space.Add(tr);
    if (!ts.ok()) { out->status = ts; return ts; }
  }

  // ---- Stage 2: resolve imports (guest export or HLE) ----
  std::vector<std::vector<std::pair<std::uint32_t, std::uint64_t>>> resolved(
      modules.size());  // per module: (symbol_index, address)
  for (LoadedModule& m : modules) {
    bool all = true;
    for (const loader::ImportRequirement& req : m.imports) {
      const ProcessResolution pr = ResolveIdentity(
          modules, hle_reg, req.symbol, options.resolver);
      if (!pr.status.ok()) {
        all = false;
        out->status = pr.status;
        out->blockers.push_back("unresolved import in " + m.name + ": " +
                                req.symbol.Describe());
        continue;
      }
      resolved[m.id].push_back({req.symbol_index, pr.address});
      if (pr.source == ResolutionSource::kHle) {
        m.state = ModuleState::kResolved;  // provisional
      }
    }
    if (all) {
      if (m.state == ModuleState::kPlanned) m.state = ModuleState::kResolved;
    }
  }
  if (out->status.code() != StatusCode::kOk) {
    return out->status;  // not execution-ready; memory untouched
  }

  // ---- Stage 3: commit memory transactionally ----
  memory::MappedGuestMemory staging;
  std::int64_t ops = 0;
  const auto quota = [&]() -> Status {
    if (options.fail_after_operations >= 0 &&
        ops >= options.fail_after_operations) {
      return StatusCode::kApplyFailed;
    }
    ++ops;
    return StatusCode::kOk;
  };

  for (const AddressRegion& r : space.regions()) {
    if (r.kind != "module" && r.kind != "stack") continue;  // trampoline: no mem
    const Status q = quota();
    if (!q.ok()) { out->status = q; return q; }
    const Status st =
        staging.AddRegion({r.base, r.size, r.permissions});
    if (!st.ok()) { out->status = st; return st; }
  }

  // Copy file bytes + BSS per module.
  for (const LoadedModule& m : modules) {
    const std::vector<std::uint8_t>& bytes = keepalive[m.id];
    const loader::BinaryReader reader(bytes.data(), bytes.size());
    for (const loader::PlannedSegment& s : m.plan.segments) {
      if (s.filesz != 0) {
        const Status q = quota();
        if (!q.ok()) { out->status = q; return q; }
        std::vector<std::uint8_t> buf(static_cast<std::size_t>(s.filesz), 0);
        if (!reader.Copy(s.file_offset, buf.data(), s.filesz).ok()) {
          out->status = StatusCode::kTruncatedInput;
          return out->status;
        }
        if (!staging.WriteImage(s.vaddr, buf.data(), s.filesz).ok()) {
          out->status = StatusCode::kApplyFailed;
          return out->status;
        }
      }
      if (s.ZeroFillSize() != 0) {
        const Status q = quota();
        if (!q.ok()) { out->status = q; return q; }
        if (!staging.ZeroFill(s.vaddr + s.filesz, s.ZeroFillSize()).ok()) {
          out->status = StatusCode::kApplyFailed;
          return out->status;
        }
      }
    }
  }

  // Apply relocations, using guest-export / HLE addresses for symbol relocs.
  for (const LoadedModule& m : modules) {
    for (const loader::Relocation& r : m.plan.relocations) {
      if (r.type == loader::RelocType::kNone) continue;
      const Status q = quota();
      if (!q.ok()) { out->status = q; return q; }
      std::uint64_t sym_addr = 0;
      const bool needs = (r.type == loader::RelocType::k64 ||
                          r.type == loader::RelocType::kGlobDat ||
                          r.type == loader::RelocType::kJumpSlot);
      if (needs) {
        if (r.symbol_index >= m.plan.symbols.size()) {
          out->status = StatusCode::kSymbolIndexInvalid;
          return out->status;
        }
        const loader::ElfSymbol& sym = m.plan.symbols[r.symbol_index];
        if (!sym.IsImport()) {
          sym_addr = sym.elf_value + m.load_bias;
        } else {
          bool found = false;
          for (const auto& pr : resolved[m.id]) {
            if (pr.first == r.symbol_index) {
              sym_addr = pr.second;
              found = true;
              break;
            }
          }
          if (!found) {
            out->status = StatusCode::kUnresolvedImport;
            out->blockers.push_back("relocation symbol unresolved in " + m.name);
            return out->status;
          }
        }
      }
      std::uint64_t value = 0;
      if (!loader::ComputeRelocationValue(r, m.load_bias, sym_addr, &value).ok()) {
        out->status = StatusCode::kRelocationOverflow;
        return out->status;
      }
      std::uint8_t b[8];
      for (int i = 0; i < 8; ++i) {
        b[i] = static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFu);
      }
      if (!staging.WriteImage(r.target_guest_va, b, 8).ok()) {
        out->status = StatusCode::kRelocationTargetInvalid;
        return out->status;
      }
    }
  }

  // ---- Stage 4: translate + merge + link ----
  std::vector<ir::Program> programs(modules.size());
  std::vector<const ir::Program*> program_ptrs(modules.size(), nullptr);
  for (const LoadedModule& m : modules) {
    // Find an executable segment and translate its bytes.
    const loader::PlannedSegment* exec = nullptr;
    for (const loader::PlannedSegment& s : m.plan.segments) {
      if ((s.permissions & memory::kPermExecute) != 0 && s.filesz != 0) {
        exec = &s;
        break;
      }
    }
    if (exec == nullptr) {
      out->status = StatusCode::kInvalidEntryPoint;
      out->blockers.push_back("module has no executable segment: " + m.name);
      return out->status;
    }
    std::vector<std::uint8_t> code(static_cast<std::size_t>(exec->filesz), 0);
    std::size_t avail = 0;
    const Status fc = staging.FetchCode(exec->vaddr, code.size(), code.data(),
                                        &avail);
    if (!fc.ok() || avail != code.size()) {
      out->status = StatusCode::kApplyFailed;
      return out->status;
    }
    std::vector<memory::GuestAddr> addrs;
    const Status ts = cpu::TranslateProgram(exec->vaddr, code.data(),
                                            code.size(), m.name,
                                            &programs[m.id], &addrs);
    if (!ts.ok()) { out->status = ts; return ts; }
    program_ptrs[m.id] = &programs[m.id];
  }
  const std::size_t main_index = modules.empty() ? 0 : modules.size() - 1;
  const Status ms = MergePrograms(program_ptrs, main_index, &out->program);
  if (!ms.ok()) { out->status = ms; return ms; }
  LinkGuestCalls(&out->program);
  const Status vs = ir::ValidateIr(out->program);
  if (!vs.ok()) { out->status = vs; return vs; }

  // ---- Commit ----
  out->memory = std::move(staging);
  out->image.modules = std::move(modules);
  out->image.main_module = static_cast<std::uint32_t>(out->image.modules.size() - 1);
  out->image.entry = out->image.modules.back().plan.entry;
  out->image.address_space = std::move(space);
  out->execution_ready = true;
  out->status = StatusCode::kOk;
  return StatusCode::kOk;
}

}  // namespace p5emu::runtime
