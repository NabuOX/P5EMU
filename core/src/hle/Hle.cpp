// SPDX-License-Identifier: LicenseRef-P5EMU-Proprietary
//
// P5EMU source is independently authored and proprietary. See LICENSE.md and
// PROVENANCE.md before adding code here.

#include "p5emu/hle/Hle.h"

namespace p5emu::hle {

const char* HleStatusName(HleStatus status) noexcept {
  switch (status) {
    case HleStatus::kOk: return "ok";
    case HleStatus::kNotFound: return "not-found";
    case HleStatus::kAmbiguous: return "ambiguous";
    case HleStatus::kUnsupportedIdentity: return "unsupported-identity";
    case HleStatus::kInvalidArgument: return "invalid-argument";
    case HleStatus::kMemoryFault: return "memory-fault";
    case HleStatus::kDispatchFault: return "dispatch-fault";
    case HleStatus::kInternalError: return "internal-error";
  }
  return "internal-error";
}

StatusCode StatusCodeForHle(HleStatus status) noexcept {
  switch (status) {
    case HleStatus::kOk: return StatusCode::kOk;
    case HleStatus::kNotFound: return StatusCode::kHleNotFound;
    case HleStatus::kAmbiguous: return StatusCode::kHleAmbiguous;
    case HleStatus::kUnsupportedIdentity:
      return StatusCode::kHleUnsupportedIdentity;
    case HleStatus::kInvalidArgument: return StatusCode::kHleInvalidArgument;
    case HleStatus::kMemoryFault: return StatusCode::kHleMemoryFault;
    case HleStatus::kDispatchFault: return StatusCode::kHleDispatchFault;
    case HleStatus::kInternalError: return StatusCode::kHleInternalError;
  }
  return StatusCode::kHleInternalError;
}

std::uint64_t HleCallContext::Arg(std::size_t index) const {
  if (state_ == nullptr) return 0;
  // TEST-ONLY: modelling a swapped first-two-argument defect.
  if (swap_first_two_args_) {
    if (index == 0) index = 1;
    else if (index == 1) index = 0;
  }
  switch (index) {
    case 0: return state_->rdi;
    case 1: return state_->rsi;
    case 2: return state_->rdx;
    case 3: return state_->rcx;
    case 4: return state_->r8;
    case 5: return state_->r9;
    default: break;
  }
  // Arguments 6+ are on the guest stack (AMD64 System V). The CALL pushed the
  // return address, so the first stack argument is at [rsp + 8].
  if (memory_ == nullptr) return 0;
  const std::uint64_t off = 8u + (index - 6u) * 8u;
  if (state_->rsp > (memory::GuestMemoryPort::kMaxGuestAddr - off)) return 0;
  std::uint64_t v = 0;
  if (!memory_->ReadData(state_->rsp + off, 8, &v).ok()) return 0;
  return v;
}

void HleCallContext::SetReturn(std::uint64_t value) noexcept {
  if (state_ == nullptr) return;
  // TEST-ONLY: modelling a return written to the wrong guest register.
  if (wrong_return_register_) {
    state_->rsi = value;
    return;
  }
  state_->rax = value;
}

Status HleCallContext::ReadGuest(memory::GuestAddr addr,
                                 memory::AccessWidth width,
                                 std::uint64_t* out) {
  if (memory_ == nullptr) return StatusCode::kHleInternalError;
  const Status st = memory_->ReadData(addr, width, out);
  return st.ok() ? StatusCode::kOk : StatusCode::kHleMemoryFault;
}

Status HleCallContext::WriteGuest(memory::GuestAddr addr,
                                  memory::AccessWidth width,
                                  std::uint64_t value) {
  if (memory_ == nullptr) return StatusCode::kHleInternalError;
  const Status st = memory_->WriteData(addr, width, value);
  return st.ok() ? StatusCode::kOk : StatusCode::kHleMemoryFault;
}

Status HleCallContext::ReadBytes(memory::GuestAddr addr, std::uint64_t length,
                                 std::vector<std::uint8_t>* out) {
  if (out == nullptr) return StatusCode::kHleInvalidArgument;
  out->clear();
  // Zero-length is a valid, defined no-op.
  if (length == 0) return StatusCode::kOk;
  // Bound the copy so an attacker-controlled length cannot force an unbounded
  // allocation; the caller sees kHleMemoryFault rather than a host fault.
  constexpr std::uint64_t kMaxRead = 1u << 20;
  if (length > kMaxRead) return StatusCode::kHleMemoryFault;
  out->reserve(static_cast<std::size_t>(length));
  for (std::uint64_t i = 0; i < length; ++i) {
    std::uint64_t byte = 0;
    if (memory_ == nullptr) return StatusCode::kHleInternalError;
    const Status st = memory_->ReadData(addr + i, 1, &byte);
    if (!st.ok()) return StatusCode::kHleMemoryFault;
    out->push_back(static_cast<std::uint8_t>(byte & 0xFFu));
  }
  return StatusCode::kOk;
}

std::uint64_t HleRegistry::Register(const std::string& module,
                                    const loader::SymbolIdentity& symbol,
                                    HleFunction fn) {
  const std::uint64_t trampoline =
      kTrampolineBase + entries_.size() * kTrampolineStride;
  if (trampoline >= kTrampolineEnd) {
    return 0;  // full: refuse rather than overflow the reserved range
  }
  HleEntry e;
  e.module = module;
  e.symbol = symbol;
  e.fn = fn;
  e.trampoline = trampoline;
  entries_.push_back(std::move(e));
  return trampoline;
}

const HleEntry* HleRegistry::Find(const std::string& module,
                                  const loader::SymbolIdentity& symbol,
                                  HleStatus* status) const {
  const HleEntry* found = nullptr;
  int matches = 0;
  for (const HleEntry& e : entries_) {
    if (e.module == module && e.symbol.kind == symbol.kind &&
        e.symbol.name == symbol.name) {
      found = &e;
      ++matches;
    }
  }
  if (matches == 0) {
    if (status != nullptr) *status = HleStatus::kNotFound;
    return nullptr;
  }
  if (matches > 1) {
    if (status != nullptr) *status = HleStatus::kAmbiguous;
    return nullptr;
  }
  if (status != nullptr) *status = HleStatus::kOk;
  return found;
}

const HleEntry* HleRegistry::FindByTrampoline(std::uint64_t address) const {
  for (const HleEntry& e : entries_) {
    if (e.trampoline == address) return &e;
  }
  return nullptr;
}

Status HleDispatcher::HandleExternalCall(std::uint64_t binding_address,
                                              cpu::CpuState* state,
                                              memory::GuestMemoryPort* memory) {
  // Reentrancy boundary: an HLE function may not trigger another dispatch.
  if (active_) {
    return StatusCode::kHleDispatchFault;
  }
  const HleEntry* entry =
      (registry_ != nullptr) ? registry_->FindByTrampoline(binding_address)
                             : nullptr;
  if (entry == nullptr || entry->fn == nullptr) {
    if (trace_enabled_) {
      trace_.push_back(TraceRecord{binding_address, "", "",
                                   HleStatus::kNotFound, 0});
    }
    // TEST-ONLY: modelling an unknown binding silently treated as success.
    if (inject_.unknown_binding_as_success) {
      return StatusCode::kOk;
    }
    return StatusCode::kHleNotFound;
  }

  active_ = true;
  HleCallContext ctx(state, memory);
  ctx.SetTestInjections(inject_.swap_first_two_args,
                        inject_.wrong_return_register);
  const HleStatus st = entry->fn(ctx);
  active_ = false;

  if (trace_enabled_) {
    TraceRecord rec;
    rec.binding = binding_address;
    rec.module = entry->module;
    rec.symbol = entry->symbol.Describe();
    rec.status = st;
    rec.result = (state != nullptr) ? state->rax : 0;
    trace_.push_back(std::move(rec));
  }
  if (st != HleStatus::kOk) {
    return StatusCodeForHle(st);
  }
  return StatusCode::kOk;
}

loader::ResolutionResult HleResolver::ResolveSymbol(
    const std::string& module, const loader::SymbolIdentity& symbol) {
  if (symbol.kind != loader::SymbolIdentityKind::kStringName) {
    loader::ResolutionResult r;
    r.status = loader::ResolutionStatus::kUnsupportedIdentity;
    r.detail = "HLE registry matches string names only";
    return r;
  }
  if (registry_ == nullptr) {
    return loader::ResolutionResult::Unresolved();
  }
  // An empty module means the executable gave no module attribution (generic
  // ELF has no symbol versioning here). Match by symbol name across modules;
  // more than one match is Ambiguous, never a silent pick.
  if (module.empty()) {
    const HleEntry* found = nullptr;
    int matches = 0;
    for (const HleEntry& e : registry_->entries()) {
      if (e.symbol.kind == loader::SymbolIdentityKind::kStringName &&
          e.symbol.name == symbol.name) {
        found = &e;
        ++matches;
      }
    }
    if (matches == 1) {
      return loader::ResolutionResult::Resolved(found->trampoline);
    }
    loader::ResolutionResult r;
    r.status = (matches > 1) ? loader::ResolutionStatus::kAmbiguous
                             : loader::ResolutionStatus::kUnresolved;
    return r;
  }

  HleStatus st = HleStatus::kNotFound;
  const HleEntry* e = registry_->Find(module, symbol, &st);
  if (e != nullptr) {
    return loader::ResolutionResult::Resolved(e->trampoline);
  }
  loader::ResolutionResult r;
  r.status = (st == HleStatus::kAmbiguous)
                 ? loader::ResolutionStatus::kAmbiguous
                 : loader::ResolutionStatus::kUnresolved;
  return r;
}

}  // namespace p5emu::hle
