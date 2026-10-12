// SPDX-License-Identifier: LicenseRef-P5EMU-Proprietary
//
// P5EMU source is independently authored and proprietary. See LICENSE.md and
// PROVENANCE.md before adding code here.

#include "p5emu/loader/Loader.h"

#include <algorithm>

namespace p5emu::loader {
namespace {

std::uint32_t PermissionsFromFlags(std::uint32_t ph_flags) {
  std::uint32_t perms = memory::kPermNone;
  if ((ph_flags & kPfR) != 0) perms |= memory::kPermRead;
  if ((ph_flags & kPfW) != 0) perms |= memory::kPermWrite;
  if ((ph_flags & kPfX) != 0) perms |= memory::kPermExecute;
  return perms;
}

bool RangesOverlap(std::uint64_t a0, std::uint64_t a1, std::uint64_t b0,
                   std::uint64_t b1) noexcept {
  return a0 < b1 && b0 < a1;
}

// True when the relocation type references a symbol (needs S).
bool TypeNeedsSymbol(RelocType type) noexcept {
  return type == RelocType::k64 || type == RelocType::kGlobDat ||
         type == RelocType::kJumpSlot;
}

}  // namespace

Status BuildLoadPlan(const BinaryReader& reader, const Ps5ExecutableImage& image,
                     std::uint64_t load_bias, LoadPlan* out) {
  if (out == nullptr) {
    return StatusCode::kInvalidArgument;
  }
  *out = LoadPlan{};
  out->bias = load_bias;
  if (BinaryReader::AddOverflows(image.entry, load_bias)) {
    return StatusCode::kIntegerOverflow;
  }
  out->entry = image.entry + load_bias;
  out->name = image.is_ps5_dialect ? "ps5-executable" : "elf64-executable";

  for (const Elf64ProgramHeader& ph : image.load_segments) {
    if (ph.memsz == 0) continue;
    if (BinaryReader::AddOverflows(ph.vaddr, load_bias)) {
      return StatusCode::kIntegerOverflow;
    }
    PlannedSegment s;
    s.vaddr = ph.vaddr + load_bias;
    s.filesz = ph.filesz;
    s.memsz = ph.memsz;
    s.file_offset = ph.offset;
    s.permissions = PermissionsFromFlags(ph.flags);
    s.align = ph.align;
    out->segments.push_back(s);
  }
  std::sort(out->segments.begin(), out->segments.end(),
            [](const PlannedSegment& a, const PlannedSegment& b) {
              return a.vaddr < b.vaddr;
            });

  out->unresolved_imports = image.needed_modules;

  // Relocations: parse (RELA), store, and build import requirements.
  RelocationSet rs;
  const Status rst = ParseDynamicRelocations(reader, image, load_bias, &rs);
  if (!rst.ok()) {
    return rst;
  }
  out->relocations = std::move(rs.relocations);
  out->symbols = std::move(rs.symbols);

  for (const Relocation& r : out->relocations) {
    if (!TypeNeedsSymbol(r.type)) continue;
    if (r.symbol_index >= out->symbols.size()) {
      return StatusCode::kSymbolIndexInvalid;
    }
    const ElfSymbol& sym = out->symbols[r.symbol_index];
    if (!sym.IsImport()) continue;  // defined in image: no requirement

    ImportRequirement* req = nullptr;
    for (ImportRequirement& e : out->import_requirements) {
      if (e.symbol_index == r.symbol_index) {
        req = &e;
        break;
      }
    }
    if (req == nullptr) {
      ImportRequirement fresh;
      fresh.module = "";  // generic ELF: no module attribution available
      fresh.symbol = sym.name.empty() ? SymbolIdentity::Unknown()
                                      : SymbolIdentity::String(sym.name);
      fresh.symbol_index = r.symbol_index;
      fresh.required = true;
      out->import_requirements.push_back(std::move(fresh));
      req = &out->import_requirements.back();
    }
    BindingSite site;
    site.target_guest_va = r.target_guest_va;
    site.reloc_type = static_cast<std::uint32_t>(r.type);
    site.addend = r.addend;
    req->binding_sites.push_back(site);
  }

  if (image.is_ps5_dialect) {
    out->notes.emplace_back(
        "PS5/Prospero ELF dialect recognized (EM_X86_64; FreeBSD-derived ABI).");
  }
  if (!image.ps5_tags.empty()) {
    out->notes.emplace_back(
        "PS5-specific dynamic tags preserved without interpretation.");
  }
  if (!out->import_requirements.empty()) {
    out->notes.emplace_back(
        "Imported symbols discovered from RELA tables; resolution is a separate "
        "resolver step.");
  }
  if (!image.unknown_tags.empty()) {
    out->notes.emplace_back("Unknown dynamic tags preserved verbatim.");
  }
  return StatusCode::kOk;
}

Status BuildLoadPlan(const BinaryReader& reader, const Ps5ExecutableImage& image,
                     LoadPlan* out) {
  return BuildLoadPlan(reader, image, 0, out);
}

Status ValidateLoadPlan(const LoadPlan& plan) {
  std::uint64_t total_bytes = 0;
  for (const PlannedSegment& s : plan.segments) {
    if (s.filesz > s.memsz) return StatusCode::kInvalidSegment;
    if (BinaryReader::AddOverflows(s.vaddr, s.memsz)) {
      return StatusCode::kIntegerOverflow;
    }
    if (s.permissions == memory::kPermNone) return StatusCode::kInvalidSegment;
    if (s.align > 1 && (s.align & (s.align - 1)) != 0) {
      return StatusCode::kInvalidAlignment;
    }
    if (s.align > 1 && (s.vaddr % s.align) != (s.file_offset % s.align)) {
      return StatusCode::kInvalidAlignment;
    }
    if (BinaryReader::AddOverflows(total_bytes, s.memsz) ||
        (total_bytes + s.memsz) > kMaxLoadImageBytes) {
      return StatusCode::kImageTooLarge;
    }
    total_bytes += s.memsz;
  }

  for (std::size_t i = 0; i < plan.segments.size(); ++i) {
    for (std::size_t j = i + 1; j < plan.segments.size(); ++j) {
      const PlannedSegment& a = plan.segments[i];
      const PlannedSegment& b = plan.segments[j];
      if (RangesOverlap(a.vaddr, a.vaddr + a.memsz, b.vaddr, b.vaddr + b.memsz)) {
        return StatusCode::kOverlappingMapping;
      }
    }
  }

  bool entry_ok = false;
  for (const PlannedSegment& s : plan.segments) {
    if (plan.entry >= s.vaddr && (plan.entry - s.vaddr) < s.memsz &&
        (s.permissions & memory::kPermExecute) != 0) {
      entry_ok = true;
      break;
    }
  }
  if (!entry_ok) return StatusCode::kInvalidEntryPoint;

  // Relocations: supported type, valid target, valid symbol index.
  for (const Relocation& r : plan.relocations) {
    if (r.type == RelocType::kUnsupported) {
      return StatusCode::kUnsupportedRelocation;
    }
    if (r.type != RelocType::kNone) {
      bool target_ok = false;
      for (const PlannedSegment& s : plan.segments) {
        if (r.target_guest_va >= s.vaddr &&
            (r.target_guest_va - s.vaddr) + r.width <= s.memsz) {
          target_ok = true;
          break;
        }
      }
      if (!target_ok) return StatusCode::kRelocationTargetInvalid;
    }
    if (TypeNeedsSymbol(r.type) && r.symbol_index >= plan.symbols.size()) {
      return StatusCode::kSymbolIndexInvalid;
    }
  }
  return StatusCode::kOk;
}

Status ResolveImports(const LoadPlan& plan, ImportResolver& resolver,
                      ImportBinding* out) {
  if (out == nullptr) return StatusCode::kInvalidArgument;
  *out = ImportBinding{};
  out->execution_ready = true;

  for (const ImportRequirement& req : plan.import_requirements) {
    ResolvedRequirement rr;
    rr.requirement = req;
    const ResolutionResult res = resolver.ResolveSymbol(req.module, req.symbol);
    rr.status = res.status;
    rr.address = res.address;
    out->resolved.push_back(std::move(rr));

    if (!req.required || !req.HasBindingSites()) {
      continue;  // a declared dependency with no binding sites does not gate
    }
    if (res.status != ResolutionStatus::kResolved) {
      out->execution_ready = false;
      switch (res.status) {
        case ResolutionStatus::kAmbiguous:
          out->status = StatusCode::kAmbiguousImport;
          break;
        case ResolutionStatus::kUnsupportedIdentity:
          out->status = StatusCode::kUnsupportedSymbolIdentity;
          break;
        case ResolutionStatus::kError:
          out->status = StatusCode::kApplyFailed;
          break;
        case ResolutionStatus::kUnresolved:
        default:
          out->status = StatusCode::kUnresolvedImport;
          break;
      }
    }
  }
  return StatusCode::kOk;
}

namespace {

// Resolved address for a symbol index, using the image's own symbols or the
// binding. Returns false and sets `why` when it cannot be resolved.
bool SymbolAddress(const LoadPlan& plan, const ImportBinding& binding,
                   std::uint32_t index, std::uint64_t bias,
                   std::uint64_t* out, StatusCode* why) {
  if (index >= plan.symbols.size()) {
    *why = StatusCode::kSymbolIndexInvalid;
    return false;
  }
  const ElfSymbol& sym = plan.symbols[index];
  if (!sym.IsImport()) {
    if (BinaryReader::AddOverflows(sym.elf_value, bias)) {
      *why = StatusCode::kRelocationOverflow;
      return false;
    }
    *out = sym.elf_value + bias;
    return true;
  }
  for (const ResolvedRequirement& rr : binding.resolved) {
    if (rr.requirement.symbol_index == index &&
        rr.status == ResolutionStatus::kResolved) {
      *out = rr.address;
      return true;
    }
  }
  *why = StatusCode::kUnresolvedImport;
  return false;
}

}  // namespace

Status ApplyLoadPlan(const BinaryReader& reader, const LoadPlan& plan,
                     const ImportBinding& binding, const ApplyOptions& options,
                     memory::MappedGuestMemory* target) {
  if (target == nullptr) return StatusCode::kInvalidArgument;

  const Status valid = ValidateLoadPlan(plan);
  if (!valid.ok()) return valid;

  // A required binding that is not resolved prevents execution readiness. Never
  // silently continue with an unresolved import (unless the TEST-ONLY defect
  // injection deliberately bypasses the guard, so the tests can observe it).
  if (!binding.execution_ready && !options.inject_unresolved_as_zero) {
    return binding.status.ok() ? StatusCode::kUnresolvedImport : binding.status;
  }

  memory::MappedGuestMemory staging;
  std::int64_t ops = 0;
  auto quota = [&]() -> Status {
    if (options.fail_after_operations >= 0 &&
        ops >= options.fail_after_operations) {
      return StatusCode::kApplyFailed;
    }
    ++ops;
    return StatusCode::kOk;
  };

  // 1. Regions.
  for (const PlannedSegment& s : plan.segments) {
    const Status q = quota();
    if (!q.ok()) return q;
    const Status st = staging.AddRegion({s.vaddr, s.memsz, s.permissions});
    if (!st.ok()) return st;
  }

  // 2. File-backed bytes.
  std::vector<std::uint8_t> buffer;
  for (const PlannedSegment& s : plan.segments) {
    if (s.filesz == 0) continue;
    const Status q = quota();
    if (!q.ok()) return q;
    buffer.assign(static_cast<std::size_t>(s.filesz), 0);
    if (!reader.Copy(s.file_offset, buffer.data(), s.filesz).ok()) {
      return StatusCode::kTruncatedInput;
    }
    if (!staging.WriteImage(s.vaddr, buffer.data(), s.filesz).ok()) {
      return StatusCode::kApplyFailed;
    }
  }

  // 3. BSS.
  for (const PlannedSegment& s : plan.segments) {
    const std::uint64_t zf = s.ZeroFillSize();
    if (zf == 0) continue;
    const Status q = quota();
    if (!q.ok()) return q;
    if (!staging.ZeroFill(s.vaddr + s.filesz, zf).ok()) {
      return StatusCode::kApplyFailed;
    }
  }

  // 4. Relocations, applied to the staging image (before final protections).
  for (const Relocation& r : plan.relocations) {
    if (r.type == RelocType::kNone) continue;
    const Status q = quota();
    if (!q.ok()) return q;

    std::uint64_t sym_addr = 0;
    if (TypeNeedsSymbol(r.type)) {
      StatusCode why = StatusCode::kUnresolvedImport;
      if (!SymbolAddress(plan, binding, r.symbol_index, plan.bias, &sym_addr,
                         &why)) {
        if (options.inject_unresolved_as_zero) {
          sym_addr = 0;  // TEST-ONLY defect: unresolved becomes address zero
        } else {
          return why;
        }
      }
    }
    std::uint64_t value = 0;
    const std::uint64_t reloc_bias =
        options.inject_wrong_load_bias ? 0 : plan.bias;
    const Status cv =
        ComputeRelocationValue(r, reloc_bias, sym_addr, &value);
    if (!cv.ok()) return cv;

    std::uint8_t bytes[8];
    for (int i = 0; i < 8; ++i) {
      bytes[i] = static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFu);
    }
    if (!staging.WriteImage(r.target_guest_va, bytes, 8).ok()) {
      return StatusCode::kRelocationTargetInvalid;
    }
  }

  *target = std::move(staging);
  return StatusCode::kOk;
}

Status ApplyLoadPlan(const BinaryReader& reader, const LoadPlan& plan,
                     const ApplyOptions& options,
                     memory::MappedGuestMemory* target) {
  ImportBinding empty;
  empty.execution_ready = true;
  // Requirements with binding sites but no resolver are unresolved.
  for (const ImportRequirement& req : plan.import_requirements) {
    ResolvedRequirement rr;
    rr.requirement = req;
    rr.status = ResolutionStatus::kUnresolved;
    empty.resolved.push_back(std::move(rr));
    if (req.required && req.HasBindingSites()) {
      empty.execution_ready = false;
      empty.status = StatusCode::kUnresolvedImport;
    }
  }
  return ApplyLoadPlan(reader, plan, empty, options, target);
}

Status MakeEntryState(const LoadPlan& plan, cpu::CpuState* out) {
  if (out == nullptr) return StatusCode::kInvalidArgument;
  const Status valid = ValidateLoadPlan(plan);
  if (!valid.ok()) return valid;
  *out = cpu::CpuState{};
  out->rip = plan.entry;
  return StatusCode::kOk;
}

}  // namespace p5emu::loader
