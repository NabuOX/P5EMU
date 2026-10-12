// SPDX-License-Identifier: Apache-2.0
//
// P5EMU is an independent, open-source project. See LICENSE and
// PROVENANCE.md before adding code here.
//
// ============================================================================
// PROCESS RUNTIME (Phase 7)
// ============================================================================
//
// A generic process/runtime layer above the loader and HLE:
//
//   executable -> dependency graph -> module load plans -> relocations ->
//   imports -> HLE/guest-export bindings -> process address space -> stack ->
//   initial CPU state -> execution
//
// Everything here is P5EMU infrastructure. It is NOT a claim about PS5 address
// policy, PS5 startup stack, PS5 module metadata, or PS5 symbol layout. See
// docs/research/ps5-runtime-evidence.md: those facts are UNKNOWN and are not
// invented.
// ============================================================================

#ifndef P5EMU_RUNTIME_RUNTIME_H
#define P5EMU_RUNTIME_RUNTIME_H

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "p5emu/core/Status.h"
#include "p5emu/cpu/GuestMemory.h"
#include "p5emu/cpu/Ir.h"
#include "p5emu/cpu/MappedGuestMemory.h"
#include "p5emu/hle/Hle.h"
#include "p5emu/loader/BinaryReader.h"
#include "p5emu/loader/Loader.h"

namespace p5emu::runtime {

inline constexpr std::uint32_t kNoOwner = 0xFFFFFFFFu;

// ---------------------------------------------------------------------------
// Address space
// ---------------------------------------------------------------------------
// A DESCRIPTION of the process address space. It is not the memory itself: the
// GuestMemoryPort remains the sole access boundary. Regions carry permissions
// and an owner module id (or kNoOwner for stack/trampoline/heap).
struct AddressRegion {
  memory::GuestAddr base = 0;
  memory::GuestSize size = 0;
  memory::Permissions permissions = memory::kPermNone;
  std::uint32_t owner_module = kNoOwner;
  std::string kind;  // "module", "stack", "trampoline", "heap"
};

class AddressSpace {
 public:
  // Adds a region; rejects overlap (kOverlappingMapping) and overflow.
  Status Add(const AddressRegion& region);
  [[nodiscard]] const AddressRegion* Find(memory::GuestAddr addr) const noexcept;
  [[nodiscard]] bool Overlaps(memory::GuestAddr base,
                              memory::GuestSize size) const noexcept;
  [[nodiscard]] const std::vector<AddressRegion>& regions() const noexcept {
    return regions_;
  }
  [[nodiscard]] std::uint64_t TotalBytes() const noexcept;

 private:
  std::vector<AddressRegion> regions_;
};

// ---------------------------------------------------------------------------
// Guest stack
// ---------------------------------------------------------------------------
// R/W, non-executable. Downward growth. All access goes through GuestMemoryPort.
// The default base is a P5EMU SYNTHETIC address, not a PS5 address.
class GuestStack {
 public:
  static constexpr memory::GuestSize kDefaultSize = 1u << 20;  // 1 MiB
  static constexpr memory::GuestAddr kDefaultBase = 0x7F'0000'0000ull;

  // Creates a stack region [base, base+size). Non-executable, read/write.
  Status Create(memory::GuestAddr base = kDefaultBase,
                memory::GuestSize size = kDefaultSize);

  [[nodiscard]] memory::GuestAddr base() const noexcept { return region_.base; }
  [[nodiscard]] memory::GuestSize size() const noexcept { return region_.size; }
  [[nodiscard]] memory::GuestAddr top() const noexcept {
    return region_.base + region_.size;
  }
  [[nodiscard]] const AddressRegion& region() const noexcept { return region_; }

  // Push/pop 64-bit values. `rsp` is updated. Bounds-checked; a push below the
  // base or a pop above the top fails with kUnmappedAccess.
  Status Push64(memory::GuestMemoryPort* memory, memory::GuestAddr* rsp,
                std::uint64_t value) const;
  Status Pop64(memory::GuestMemoryPort* memory, memory::GuestAddr* rsp,
               std::uint64_t* value) const;

 private:
  AddressRegion region_{};
};

// ---------------------------------------------------------------------------
// Startup descriptor and initial stack image
// ---------------------------------------------------------------------------
// Fields whose policy is UNKNOWN (PS5 argc/argv/auxv/TLS) are only populated by
// the generic builder; PS5 mode leaves them as-is.
struct ProcessStartup {
  memory::GuestAddr entry_rip = 0;
  memory::GuestAddr initial_rsp = 0;
  std::uint64_t argc = 0;
  std::vector<memory::GuestAddr> argv;  // guest VAs
  std::vector<memory::GuestAddr> envp;  // guest VAs
  std::vector<std::pair<std::uint64_t, std::uint64_t>> auxv;
};

// Builds a generic System-V-style initial stack image: argument strings, argv
// pointer array, envp, auxv, argc, and a 16-byte-aligned RSP. Every pointer is
// a guest VA. All arithmetic is checked. Does NOT claim PS5 uses this layout.
struct StartupOptions {
  std::uint64_t argc_override = 0;   // if nonzero, force argc to this
  bool append_null_argv = true;
  bool append_null_envp = true;
  bool append_auxv_null = true;
};

Status BuildInitialStack(memory::GuestMemoryPort* memory, const GuestStack& stack,
                         const std::vector<std::string>& args,
                         const std::vector<std::string>& env,
                         const std::vector<std::pair<std::uint64_t, std::uint64_t>>& auxv,
                         const StartupOptions& options,
                         ProcessStartup* out);

// ---------------------------------------------------------------------------
// Module lifecycle
// ---------------------------------------------------------------------------
enum class ModuleState : std::uint8_t {
  kParsed = 0,
  kValidated,
  kPlanned,
  kResolved,
  kMapped,
  kReady,
  kRunning,
};

const char* ModuleStateName(ModuleState state) noexcept;
// Forward-only lifecycle: each state may advance exactly one step.
bool IsValidModuleTransition(ModuleState from, ModuleState to) noexcept;

// ---------------------------------------------------------------------------
// Modules
// ---------------------------------------------------------------------------
struct ExportedSymbol {
  std::uint32_t module_id = 0;
  loader::SymbolIdentity symbol;
  memory::GuestAddr guest_va = 0;
};

struct LoadedModule {
  std::uint32_t id = 0;
  std::string name;
  std::uint64_t load_bias = 0;
  loader::LoadPlan plan;
  std::vector<std::string> dependencies;
  std::vector<loader::ImportRequirement> imports;
  std::vector<ExportedSymbol> exports;
  ModuleState state = ModuleState::kParsed;
};

// ---------------------------------------------------------------------------
// Module repository
// ---------------------------------------------------------------------------
class ModuleRepository {
 public:
  virtual ~ModuleRepository() = default;
  [[nodiscard]] virtual const std::vector<std::uint8_t>* Fetch(
      const std::string& name) const = 0;
};

// In-memory synthetic repository. No filesystem emulation.
class InMemoryModuleRepository final : public ModuleRepository {
 public:
  void Add(std::string name, std::vector<std::uint8_t> bytes);
  [[nodiscard]] const std::vector<std::uint8_t>* Fetch(
      const std::string& name) const override;
  [[nodiscard]] const std::vector<std::string>& names() const noexcept {
    return order_;
  }

 private:
  std::vector<std::string> order_;
  std::map<std::string, std::vector<std::uint8_t>> modules_;
};

// ---------------------------------------------------------------------------
// Dependency graph
// ---------------------------------------------------------------------------
struct DependencyGraph {
  std::vector<std::string> order;    // dependencies before dependents
  std::vector<std::string> missing;  // required but absent
  std::string cycle;                 // one cycle as "a -> b -> a", or empty
  [[nodiscard]] bool ok() const noexcept {
    return missing.empty() && cycle.empty();
  }
};

struct GraphOptions {
  bool inject_reverse_order = false;  // TEST-ONLY defect
};

// Builds a deterministic load order: dependencies before dependents, each
// dependency loaded once, cycles reported.
Status BuildDependencyGraph(
    const std::string& root,
    const std::map<std::string, std::vector<std::string>>& deps,
    const GraphOptions& options, DependencyGraph* out);

// ---------------------------------------------------------------------------
// Load-bias allocation
// ---------------------------------------------------------------------------
// Deterministic synthetic layout: sequential, aligned, non-overlapping.
// P5EMU SYNTHETIC PROCESS LAYOUT -- NOT PS5 ADDRESS POLICY.
class LoadBiasAllocator {
 public:
  static constexpr memory::GuestAddr kBase = 0x2000'0000ull;
  static constexpr memory::GuestAddr kEnd = 0x7000'0000ull;  // below the trampolines
  // Each module gets a fixed 256 MiB slot: aligned, non-overlapping, and
  // trivially predictable (module i -> kBase + i*kStride).
  static constexpr memory::GuestAddr kStride = 0x1000'0000ull;

  [[nodiscard]] memory::GuestAddr next() const noexcept { return next_; }
  // Reserves one aligned slot. Fails when a module exceeds kStride
  // (kImageTooLarge) or the arena is exhausted (kIntegerOverflow).
  Status Allocate(memory::GuestSize span, memory::GuestAddr* bias_out);

  // TEST-ONLY: hand out the same slot twice (models an overlap-allocator defect).
  void SetInjectOverlap(bool enabled) noexcept { inject_overlap_ = enabled; }

 private:
  memory::GuestAddr next_ = kBase;
  bool inject_overlap_ = false;
};

// ---------------------------------------------------------------------------
// Cross-module resolution
// ---------------------------------------------------------------------------
enum class ResolutionSource : std::uint8_t {
  kNone = 0,
  kGuestExport,
  kHle,
};
const char* ResolutionSourceName(ResolutionSource source) noexcept;

// Resolution policy: which source wins when both a guest export and an HLE
// binding exist for the same identity. Explicit, never accidental.
enum class ResolutionPolicy : std::uint8_t {
  kGuestFirst = 0,  // guest export wins; HLE is the fallback
  kHleFirst,        // HLE wins
};

struct ResolverOptions {
  ResolutionPolicy policy = ResolutionPolicy::kGuestFirst;
  // TEST-ONLY defect: force HLE to win even under kGuestFirst.
  bool inject_hle_override = false;
};

struct ProcessResolution {
  Status status = StatusCode::kOk;  // kOk, kUnresolvedImport, kAmbiguousImport
  ResolutionSource source = ResolutionSource::kNone;
  memory::GuestAddr address = 0;
};

// Resolves an identity against the loaded modules' exports and the HLE
// registry, using the explicit policy.
ProcessResolution ResolveIdentity(
    const std::vector<LoadedModule>& modules, const hle::HleRegistry* hle_reg,
    const loader::SymbolIdentity& symbol, const ResolverOptions& options);

// ---------------------------------------------------------------------------
// Process image (the result of a transactional multi-module load)
// ---------------------------------------------------------------------------
struct ProcessImage {
  std::vector<LoadedModule> modules;  // load order; modules[0] is the root
  std::uint32_t main_module = 0;
  AddressSpace address_space;
  memory::GuestAddr entry = 0;
  std::vector<std::string> diagnostics;
};

// A module description is supplied by the callers: the repository provides the
// bytes by logical name, and a deps map provides each module's dependencies.
// Because Sony export layout is UNKNOWN, a module's exports are derived from its
// own DEFINED dynsym symbols rather than fabricated from unknown structures.

struct LoadProcessOptions {
  ResolverOptions resolver;
  bool inject_overlap_bias = false;          // TEST-ONLY defect
  bool inject_reverse_deps = false;          // TEST-ONLY defect
  std::int64_t fail_after_operations = -1;   // TEST-ONLY rollback injection
};

struct LoadedProcess {
  Status status = StatusCode::kInvalidArgument;
  ProcessImage image;
  memory::MappedGuestMemory memory;
  // Combined IR for all modules (blocks at each module's guest VAs).
  ir::Program program;
  // Readiness diagnostics.
  bool execution_ready = false;
  std::vector<std::string> blockers;
};

// Loads a process transactionally: dependency graph -> parse all -> plan ->
// biases -> address space -> resolve imports -> apply -> link calls -> merge IR.
// A failure at any step leaves `out->memory` and `out->image` empty (no partial
// process).
Status LoadProcess(const ModuleRepository& repo, const std::string& root_name,
                   const std::map<std::string, std::vector<std::string>>& deps,
                   const GuestStack& stack, const hle::HleRegistry* hle_reg,
                   const LoadProcessOptions& options, LoadedProcess* out);

// Merges several IR programs into one, remapping block and value ids. The entry
// is programs[entry_index]'s entry. Returns false if any program is empty.
Status MergePrograms(const std::vector<const ir::Program*>& programs,
                     std::size_t entry_index, ir::Program* out);

// Converts a kExternalCall whose target is a block in `program` into an internal
// jump, turning a cross-module guest call into ordinary CALL/RET control flow.
// External calls whose target is not a block (HLE bindings) are left unchanged.
void LinkGuestCalls(ir::Program* program);

}  // namespace p5emu::runtime

#endif  // P5EMU_RUNTIME_RUNTIME_H
