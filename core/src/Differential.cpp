// SPDX-License-Identifier: LicenseRef-P5EMU-Proprietary
//
// P5EMU source is independently authored and proprietary. See LICENSE.md and
// PROVENANCE.md before adding code here.

#include "p5emu/cpu/Differential.h"

#include <cstdio>
#include <memory>

#include "p5emu/cpu/LinearGuestMemory.h"

namespace p5emu::cpu::diff {
namespace {

// Builds a fresh, isolated memory port from the request's initial image.
//
// The LinearGuestMemory factories RETURN by value; LinearGuestMemory is
// non-copyable in practice (it owns a vector and SMC counters), so the port is
// heap-allocated per backend and referenced by pointer.
bool MakeIsolatedMemory(const ComparisonRequest& request,
                        std::unique_ptr<memory::LinearGuestMemory>* out,
                        std::string* error) {
  if (request.memory_size == 0) {
    *error = "request has no memory region";
    return false;
  }
  // Bound: a test that accidentally asks for a gigabyte should fail loudly here
  // rather than allocate it.
  if (request.memory_size > (std::size_t{16} << 20)) {
    *error = "memory region larger than 16 MiB; suspicious for a test fixture";
    return false;
  }

  switch (request.permissions) {
    case memory::kPermReadWriteExecute:
      out->reset(new memory::LinearGuestMemory(
          memory::LinearGuestMemory::MakeCodeAndData(request.memory_base,
                                                     request.memory_size)));
      break;
    case memory::kPermReadWrite:
      out->reset(new memory::LinearGuestMemory(
          memory::LinearGuestMemory::MakeDataOnly(request.memory_base,
                                                  request.memory_size)));
      break;
    case memory::kPermExecuteOnly:
      out->reset(new memory::LinearGuestMemory(
          memory::LinearGuestMemory::MakeExecuteOnly(request.memory_base,
                                                     request.memory_size)));
      break;
    default:
      *error = "unsupported permission combination for the linear test port";
      return false;
  }

  if (!request.initial_memory.empty()) {
    if (request.initial_memory.size() > request.memory_size) {
      *error = "initial image larger than the memory region";
      return false;
    }
    const Status st = out->get()->DebugFill(request.memory_base,
                                            request.initial_memory.data(),
                                            request.initial_memory.size());
    if (!st.ok()) {
      *error = std::string("could not seed initial memory: ") + st.name();
      return false;
    }
  }
  return true;
}

}  // namespace

const char* VerdictName(Verdict v) noexcept {
  switch (v) {
    case Verdict::kAgree:       return "agree";
    case Verdict::kDiverge:     return "diverge";
    case Verdict::kCoverageGap: return "coverage-gap";
    case Verdict::kError:       return "error";
  }
  return "unknown";
}

ComparisonResult RunComparison(const ComparisonRequest& request,
                               const std::vector<Backend*>& backends) {
  ComparisonResult result;

  if (request.program == nullptr || backends.size() < 2) {
    result.verdict = Verdict::kError;
    result.summary = (request.program == nullptr)
                         ? "request has no program"
                         : "at least two backends are required for a "
                           "comparison to mean anything";
    return result;
  }

  // The IR must be valid before ANY backend sees it. A backend that crashes on
  // malformed IR would be reported as a divergence, which would be a misleading
  // diagnosis.
  ir::ValidationReport validation;
  const Status valid = ir::ValidateIrDetailed(*request.program, &validation);
  if (!valid.ok()) {
    result.verdict = Verdict::kError;
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "IR failed validation before execution: rule=%s detail=%s",
                  validation.rule, validation.detail.c_str());
    result.summary = buf;
    return result;
  }

  struct Run {
    CpuState state;
    std::unique_ptr<memory::LinearGuestMemory> memory;
    ExecutionResult execution;
    std::string name;
  };

  std::vector<Run> runs;
  runs.reserve(backends.size());

  for (Backend* backend : backends) {
    if (backend == nullptr) {
      result.verdict = Verdict::kError;
      result.summary = "null backend in list";
      return result;
    }

    Run run;
    run.name = backend->name();
    run.state = request.initial_state;

    std::string error;
    if (!MakeIsolatedMemory(request, &run.memory, &error)) {
      result.verdict = Verdict::kError;
      result.summary = error;
      return result;
    }

    run.execution =
        backend->Execute(*request.program, &run.state, *run.memory);

    BackendReport report;
    report.name = run.name;
    report.status = run.execution.status;
    report.reason = run.execution.reason;
    report.executed_ops = run.execution.executed_ops;
    // A backend that reports kUnimplementedInstruction has a COVERAGE GAP, not
    // a failure and not a pass.
    report.implemented =
        (run.execution.status.code() != StatusCode::kUnimplementedInstruction);
    result.reports.push_back(report);

    if (!report.implemented) {
      char buf[256];
      std::snprintf(buf, sizeof(buf),
                    "%s did not implement this program (%s); recorded as a "
                    "coverage gap, not as agreement",
                    run.name.c_str(), report.status.name());
      result.coverage_gaps.emplace_back(buf);
    }

    runs.push_back(std::move(run));
  }

  // ---- compare everything after the first run against the first ---------
  const Run& reference = runs.front();

  for (std::size_t i = 1; i < runs.size(); ++i) {
    const Run& other = runs[i];

    // A backend that reported kUnimplementedInstruction never executed, so its
    // state is untouched input. Comparing it would emit state-difference noise
    // that reflects only "it did not run", which is exactly the misleading
    // output the coverage-gap distinction exists to prevent. It is recorded
    // above as a gap and skipped here.
    //
    // This does NOT weaken divergence detection: a backend is skipped ONLY when
    // it explicitly reported non-implementation. A backend that claims to
    // implement the program and then produces the wrong state is compared
    // normally and fails as a divergence.
    if (!result.reports[i].implemented) {
      continue;
    }

    // Termination must match first: if one backend trapped and the other did
    // not, the states are not comparable and reporting a register difference
    // would be noise.
    if (other.execution.reason != reference.execution.reason) {
      Difference d;
      d.field = "termination-reason";
      d.expected = static_cast<std::uint64_t>(reference.execution.reason);
      d.actual = static_cast<std::uint64_t>(other.execution.reason);
      d.backend_a = reference.name;
      d.backend_b = other.name;
      result.differences.push_back(d);
      continue;
    }

    if (other.execution.status.code() != reference.execution.status.code()) {
      Difference d;
      d.field = "status";
      d.expected = static_cast<std::uint64_t>(reference.execution.status.code());
      d.actual = static_cast<std::uint64_t>(other.execution.status.code());
      d.backend_a = reference.name;
      d.backend_b = other.name;
      result.differences.push_back(d);
    }

    // Field-by-field state comparison. CompareCpuState stops at the first
    // difference and names it; that is enough, because the point is to detect
    // divergence, not to enumerate all of it. `eflags_observable_mask` excludes
    // architecturally undefined bits, per the fixture.
    StateDifference sd;
    if (!CompareCpuState(reference.state, other.state, &sd,
                         request.eflags_observable_mask)) {
      Difference d;
      d.field = sd.field != nullptr ? sd.field : "<state>";
      d.expected = sd.expected;
      d.actual = sd.actual;
      d.backend_a = reference.name;
      d.backend_b = other.name;
      result.differences.push_back(d);
    }

    // Declared memory windows.
    for (const MemoryWindow& window : request.windows) {
      if (window.length == 0) {
        continue;
      }
      StateDifference md;
      if (!CompareMemoryWindow(*reference.memory, *other.memory, window.addr,
                               window.length, &md)) {
        Difference d;
        d.field = md.field != nullptr ? md.field : "<memory>";
        d.expected = md.expected;
        d.actual = md.actual;
        d.backend_a = reference.name;
        d.backend_b = other.name;
        result.differences.push_back(d);
      }
    }
  }

  bool any_gap = false;
  for (const BackendReport& report : result.reports) {
    if (!report.implemented) {
      any_gap = true;
    }
  }

  if (!result.differences.empty()) {
    result.verdict = Verdict::kDiverge;
    char buf[512];
    std::snprintf(buf, sizeof(buf), "%s: %zu divergence(s); first: %s [%s vs %s] expected=0x%llx actual=0x%llx",
                  request.name.c_str(), result.differences.size(),
                  result.differences.front().field.c_str(),
                  result.differences.front().backend_a.c_str(),
                  result.differences.front().backend_b.c_str(),
                  static_cast<unsigned long long>(result.differences.front().expected),
                  static_cast<unsigned long long>(result.differences.front().actual));
    result.summary = buf;
  } else if (any_gap) {
    result.verdict = Verdict::kCoverageGap;
    char buf[320];
    std::snprintf(buf, sizeof(buf),
                  "%s: no divergence, but %zu backend(s) did not implement the "
                  "program (coverage gap, not a pass)",
                  request.name.c_str(), result.coverage_gaps.size());
    result.summary = buf;
  } else {
    result.verdict = Verdict::kAgree;
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "%s: %zu backend(s) agree (reason=%s, ops=%llu)",
                  request.name.c_str(), runs.size(),
                  TerminationReasonName(reference.execution.reason),
                  static_cast<unsigned long long>(reference.execution.executed_ops));
    result.summary = buf;
  }

  return result;
}

}  // namespace p5emu::cpu::diff
