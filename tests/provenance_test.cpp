// SPDX-License-Identifier: LicenseRef-P5EMU-Proprietary
//
// P5EMU source is independently authored and proprietary. See LICENSE.md and
// PROVENANCE.md before adding code here.
//
// ============================================================================
// PROVENANCE ENFORCEMENT
// ============================================================================
//
// These tests exist because the rule they encode is otherwise not enforceable:
// an expected architectural value must be traceable to a specification, a
// written derivation, or a named measurement -- and must never be traceable to
// P5EMU's own output.
//
// The rules under test are stated in FixtureProvenance.h.
// ============================================================================

#include <cstdint>
#include <string>

#include "FixtureProvenance.h"
#include "TestHarness.h"

using p5emu::StatusCode;
using p5emu::test::Arithmetic;
using p5emu::test::Derived;
using p5emu::test::Expectation;
using p5emu::test::ExpectationSet;
using p5emu::test::FromApm;
using p5emu::test::FromExternalTool;
using p5emu::test::FromHardware;
using p5emu::test::FromSdm;
using p5emu::test::Provenance;
using p5emu::test::ProvenanceKind;
using p5emu::test::ProvenanceKindName;
using p5emu::test::ValidateProvenance;

P5EMU_TEST(Provenance, EveryKindHasAName) {
  // A kind with no name would render as "unknown" and quietly lose its meaning
  // in a review.
  CHECK_EQ(std::string(ProvenanceKindName(ProvenanceKind::kIntelSdm)),
           std::string("intel-sdm"));
  CHECK_EQ(std::string(ProvenanceKindName(ProvenanceKind::kAmdApm)),
           std::string("amd-apm"));
  CHECK_EQ(std::string(ProvenanceKindName(ProvenanceKind::kHandDerived)),
           std::string("hand-derived"));
  CHECK_EQ(
      std::string(ProvenanceKindName(ProvenanceKind::kArithmeticDerivation)),
      std::string("arithmetic"));
  CHECK_EQ(std::string(ProvenanceKindName(ProvenanceKind::kHardwareVerified)),
           std::string("hardware-verified"));
  CHECK_EQ(std::string(ProvenanceKindName(ProvenanceKind::kExternalTool)),
           std::string("external-tool"));
}

P5EMU_TEST(Provenance, ConstructorsProduceWellFormedCitations) {
  const Provenance sdm = FromSdm("Intel SDM Vol.2 §3.5.1", "ADD sets CF on carry");
  CHECK(sdm.IsWellFormed());
  CHECK_EQ(sdm.kind, ProvenanceKind::kIntelSdm);
  CHECK(ValidateProvenance(sdm, nullptr).ok());

  const Provenance apm = FromApm("APM Vol.1 §3.1", "SUB sets flags");
  CHECK(apm.IsWellFormed());
  CHECK(ValidateProvenance(apm, nullptr).ok());

  const Provenance derived = Derived("ADD is commutative", "2 + 3 = 5 either way");
  CHECK(derived.IsWellFormed());
  CHECK(ValidateProvenance(derived, nullptr).ok());

  const Provenance arith = Arithmetic("0xFF + 1 = 0x100, CF = 1");
  CHECK(arith.IsWellFormed());
  CHECK(ValidateProvenance(arith, nullptr).ok());

  const Provenance hw = FromHardware("observed rax == 105",
                                     "CPU model X, revision 2, single run");
  CHECK(hw.IsWellFormed());
  CHECK(ValidateProvenance(hw, nullptr).ok());

  const Provenance tool =
      FromExternalTool("SomeApprovedTool", "1.4.2", "emitted the fixture bytes");
  CHECK(tool.IsWellFormed());
  CHECK(ValidateProvenance(tool, nullptr).ok());
}

P5EMU_TEST(Provenance, CitationsWithoutEvidenceAreRejected) {
  Provenance no_reference;
  no_reference.kind = ProvenanceKind::kIntelSdm;
  no_reference.detail = "some reasoning";
  CHECK(!no_reference.IsWellFormed());
  CHECK(!ValidateProvenance(no_reference, nullptr).ok());

  Provenance no_detail;
  no_detail.kind = ProvenanceKind::kIntelSdm;
  no_detail.reference = "Intel SDM Vol.2 §3.5.1";
  CHECK(!no_detail.IsWellFormed());
  CHECK(!ValidateProvenance(no_detail, nullptr).ok());

  // Whitespace is not evidence either.
  Provenance blank;
  blank.kind = ProvenanceKind::kHandDerived;
  blank.reference = "   ";
  blank.detail = "\t\n";
  CHECK(!blank.IsWellFormed());
  CHECK(!ValidateProvenance(blank, nullptr).ok());
}

P5EMU_TEST(Provenance, ExternalToolMustBeNamedWithAVersion) {
  Provenance unnamed;
  unnamed.kind = ProvenanceKind::kExternalTool;
  unnamed.reference = "SomeTool";
  unnamed.detail = "generated the expected value";
  // A tool without a version is not reproducible, so it is not evidence.
  CHECK(!ValidateProvenance(unnamed, nullptr).ok());

  std::string reason;
  CHECK(!ValidateProvenance(unnamed, &reason).ok());
  CHECK(!reason.empty());
}

P5EMU_TEST(Provenance, P5EmusOwnOutputIsNotAcceptableEvidence) {
  // This is the specific habit the rule exists to prevent: running the
  // implementation once and freezing whatever it returned. Such a fixture
  // proves determinism, never correctness, and it enshrines any bug it captures.
  Provenance captured;
  captured.kind = ProvenanceKind::kHandDerived;
  captured.reference = "P5EMU output";
  captured.detail = "ran the reference interpreter and recorded rax";
  CHECK(!ValidateProvenance(captured, nullptr).ok());

  Provenance golden;
  golden.kind = ProvenanceKind::kExternalTool;
  golden.reference = "Tool 1.0";
  golden.detail = "golden output captured from the previous build";
  CHECK(!ValidateProvenance(golden, nullptr).ok());

  Provenance observed;
  observed.kind = ProvenanceKind::kHardwareVerified;
  observed.reference = "as observed by P5EMU";
  observed.detail = "flags matched";
  CHECK(!ValidateProvenance(observed, nullptr).ok());
}

P5EMU_TEST(Provenance, ExpectationCannotBeBuiltWithoutProvenance) {
  // Expectation has no default constructor and no provenance setter, so an
  // expectation with no evidence is not expressible. This test exists to make
  // that structural guarantee visible to a reader, and to fail loudly if
  // somebody later adds a default constructor for convenience.
  Expectation e("add.rax", 15,
                Derived("5 + 10", "mov rax,5; mov rcx,10; add rax,rcx"));
  CHECK_EQ(e.name, std::string("add.rax"));
  CHECK_EQ(e.expected, std::uint64_t{15});
  CHECK(ValidateProvenance(e.provenance, nullptr).ok());

  static_assert(!std::is_default_constructible<Expectation>::value,
                "Expectation must not be default constructible: provenance is "
                "required and must not be skippable");
  static_assert(!std::is_assignable<Expectation&, Provenance>::value,
                "Expectation provenance must not be replaceable after "
                "construction");
}

P5EMU_TEST(Provenance, ExpectationSetRejectsIllFormedEntries) {
  ExpectationSet set;

  CHECK(set.Add("add.rax", 15, FromSdm("Intel SDM Vol.2 §2.2.1", "5 + 10 = 15")));
  CHECK(set.Add("loop.sum", 55, Arithmetic("sum 1..10 = 10*11/2 = 55")));
  CHECK_EQ(set.Size(), std::size_t{2});
  CHECK(set.AllWellFormed());
  CHECK(set.Find("loop.sum") != nullptr);
  CHECK_EQ(set.Find("loop.sum")->expected, std::uint64_t{55});
  CHECK(set.Find("absent") == nullptr);

  // A bad entry is reported and NOT stored, so Size() never implies that
  // everything in the set is justified.
  Provenance bad;
  bad.kind = ProvenanceKind::kHandDerived;
  bad.reference = "P5EMU output";
  bad.detail = "recorded whatever it returned";
  CHECK(!set.Add("bad.entry", 7, bad));
  CHECK_EQ(set.Size(), std::size_t{2});
  CHECK_EQ(set.Failures().size(), std::size_t{1});
  CHECK(!set.AllWellFormed());
  CHECK(set.Find("bad.entry") == nullptr);
}

P5EMU_TEST(Provenance, PhaseOneExpectationsAreAllJustified) {
  // A representative sample of the architectural values the Phase-1 suites
  // assert, each registered with the evidence it actually rests on. This is the
  // registry the rule is aimed at: if a value is asserted anywhere, it belongs
  // in here with a citation.
  ExpectationSet set;

  // Flags, Intel SDM Vol. 2 §3.1.2 (CMP) and §3.5 (ADD/SUB).
  CHECK(set.Add("cmp.equal.zf", 1,
                FromSdm("Intel SDM Vol.2 §3.1.2",
                        "CMP subtracts without storing; equal operands set ZF")));
  CHECK(set.Add("add.carry.cf", 1,
                FromSdm("Intel SDM Vol.2 §3.5.1",
                        "0xFF + 1 = 0x100 exceeds 8 bits, so CF = 1")));

  // Arithmetic, derived independently of any emulator.
  CHECK(set.Add("arith.loop.sum", 55, Arithmetic("10*11/2 = 55")));
  CHECK(set.Add("arith.zero.extend", 0xFFFFFFFFull,
                Derived("32-bit write zero-extends",
                        "0xFFFFFFFF written to a 32-bit register clears the "
                        "upper 32 bits of the 64-bit register")));

  // Control flow, Intel SDM Vol. 2 §2.2.1.
  CHECK(set.Add("call.ret.depth", 2,
                FromSdm("Intel SDM Vol.2 §2.2.1",
                        "CALL pushes the return address; RET pops it, so a "
                        "callee and its caller are both entered")));

  // Address computation, Intel SDM Vol. 2 §2.2.1 (LEA) and §2.2.2.7.
  CHECK(set.Add("lea.composite", 0x1000 + 7 * 4 + 0x10,
                FromSdm("Intel SDM Vol.2 §2.2.1",
                        "LEA = base + index*scale + displacement = "
                        "0x1000 + 7*4 + 0x10")));
  CHECK(set.Add("rip.relative", 0x1000 + 7 + 0x0FFA,
                Derived("RIP-relative displacement is measured from the END of "
                        "the instruction",
                        "next_instruction = 0x1000 + 7; "
                        "address = next_instruction + 0x0FFA")));

  // 8-bit subregister aliasing, Intel SDM Vol. 1 §3.4.2.1.
  CHECK(set.Add("al.alias", 0x2A,
                Derived("writing AL clears the upper 56 bits of RAX",
                        "rax = 0x1234; mov al, 0x2A -> rax = 0x2A")));

  if (!set.AllWellFormed()) {
    for (const std::string& f : set.Failures()) {
      p5emu::test::ReportFailure(__FILE__, __LINE__, f.c_str());
    }
    return;
  }
  CHECK_EQ(set.Size(), std::size_t{8});
  CHECK_EQ(set.Failures().size(), std::size_t{0});
}

P5EMU_TEST(Provenance, PhaseOneUsesNoExternalToolFixtures) {
  // PROVENANCE.md records that Phase 1 has no approved third-party fixture
  // generator. If that ever changes it must be a deliberate decision with a
  // licence review, not a new test that happens to call a tool.
  ExpectationSet set;
  CHECK(set.Add("sdm.only", 1,
                FromSdm("Intel SDM Vol.2 §3.1.2", "ZF set on equality")));
  CHECK(set.Add("derived.only", 55, Arithmetic("10*11/2 = 55")));

  for (const std::string& f : set.Failures()) {
    p5emu::test::ReportFailure(__FILE__, __LINE__, f.c_str());
  }
  CHECK(set.AllWellFormed());
  CHECK_EQ(set.Size(), std::size_t{2});
}