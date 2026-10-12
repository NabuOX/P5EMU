# SPDX-License-Identifier: Apache-2.0

# P5EMU test suites
#
# One binary, several CTest entries, so a failure names the subsystem.
# Suites are added to CMakeLists.txt; adding a file without registering it fails
# a test in Phase 1's CI contract.

# Every test source, registered by name.
#
# This list is checked against the directory below, so dropping in a new
# *_test.cpp without registering it is a CONFIGURE ERROR rather than a suite
# that silently never runs. A test that never runs is worse than no test.
set(P5EMU_TEST_SOURCES
  TestMain.cpp
  FixtureProvenance.cpp
  core_test.cpp
  cpu_contracts_test.cpp
  guest_memory_test.cpp
  ir_test.cpp
  decoder_test.cpp
  interpreter_test.cpp
  differential_test.cpp
  end_to_end_test.cpp
  provenance_test.cpp
  arm64_test.cpp
  widths_test.cpp
  loader_test.cpp
  loader_fuzz_test.cpp
  reloc_test.cpp
  reloc_fuzz_test.cpp
  hle_test.cpp
  hle_fuzz_test.cpp
  runtime_test.cpp
  runtime_fuzz_test.cpp
  runtime_profile_test.cpp
  sce_metadata_test.cpp
)

file(GLOB P5EMU_TEST_FILES_ON_DISK CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp")
foreach(_test_file ${P5EMU_TEST_FILES_ON_DISK})
  get_filename_component(_test_name "${_test_file}" NAME)
  if(NOT _test_name IN_LIST P5EMU_TEST_SOURCES)
    message(FATAL_ERROR
      "tests/${_test_name} exists but is not listed in P5EMU_TEST_SOURCES in "
      "tests/CMakeLists.txt. Register it, and add a matching add_test entry, "
      "or the suite would silently never run.")
  endif()
endforeach()

add_executable(p5emu_tests ${P5EMU_TEST_SOURCES})

# --- AArch64 execution harness (test infrastructure) -----------------------
#
# The Phase-2 AArch64 backend executes emitted code through an independent
# AArch64 environment: qemu-aarch64 user-mode running a small P5EMU harness.
# Both are EXTERNAL tools, used only as test infrastructure. They are not
# linked into p5emu_core and not vendored. See PROVENANCE.md.
#
# When the cross toolchain is absent the tests still build; the AArch64
# differential tests then fail with a clear "harness not found" message rather
# than silently skipping the independent-execution proof.
find_program(P5EMU_A64_CROSS_CC NAMES aarch64-linux-gnu-gcc)
find_program(P5EMU_QEMU_AARCH64 NAMES qemu-aarch64)

if(P5EMU_A64_CROSS_CC)
  set(P5EMU_A64_HARNESS_PATH
      "${CMAKE_BINARY_DIR}/aarch64-harness/p5emu_a64_harness")
  add_custom_command(
    OUTPUT "${P5EMU_A64_HARNESS_PATH}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/aarch64-harness"
    COMMAND ${CMAKE_COMMAND} -E env
            "P5EMU_A64_CROSS_CC=${P5EMU_A64_CROSS_CC}"
            bash "${CMAKE_SOURCE_DIR}/scripts/build_aarch64_harness.sh"
                 "${CMAKE_BINARY_DIR}/aarch64-harness"
    DEPENDS "${CMAKE_SOURCE_DIR}/aarch64/harness/p5emu_a64_harness.c"
            "${CMAKE_SOURCE_DIR}/core/include/p5emu/cpu/Arm64Abi.h"
    COMMENT "Cross-compiling the AArch64 execution harness")
  add_custom_target(p5emu_aarch64_harness ALL
                    DEPENDS "${P5EMU_A64_HARNESS_PATH}")
  add_dependencies(p5emu_tests p5emu_aarch64_harness)
  target_compile_definitions(p5emu_tests PRIVATE
      P5EMU_A64_HARNESS_DEFAULT="${P5EMU_A64_HARNESS_PATH}")
endif()

target_include_directories(p5emu_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(p5emu_tests PRIVATE p5emu_core p5emu_compiler_flags p5emu_sanitizers)

p5emu_apply_target_flags(p5emu_tests)

# --- Phase 0 suites (unchanged) ---
add_test(NAME p5emu.core            COMMAND p5emu_tests core.)
add_test(NAME p5emu.version         COMMAND p5emu_tests Version.)
add_test(NAME p5emu.cpu.contracts   COMMAND p5emu_tests CpuContracts.)

# --- Phase 1 suites ---
add_test(NAME p5emu.memory.port     COMMAND p5emu_tests GuestMemory.)
add_test(NAME p5emu.ir.validate     COMMAND p5emu_tests IrValidate.)
add_test(NAME p5emu.ir.build        COMMAND p5emu_tests IrBuild.)
add_test(NAME p5emu.decoder         COMMAND p5emu_tests Decoder.)
add_test(NAME p5emu.decoder.roundtrip COMMAND p5emu_tests DecoderRoundTrip.)
add_test(NAME p5emu.decoder.malformed COMMAND p5emu_tests DecoderMalformed.)
add_test(NAME p5emu.interpreter     COMMAND p5emu_tests Interpreter.)
add_test(NAME p5emu.interpreter.flags COMMAND p5emu_tests Flags.)
add_test(NAME p5emu.interpreter.controlflow COMMAND p5emu_tests ControlFlow.)
add_test(NAME p5emu.interpreter.memory COMMAND p5emu_tests MemorySemantics.)
add_test(NAME p5emu.differential     COMMAND p5emu_tests Differential.)
add_test(NAME p5emu.differential.gate COMMAND p5emu_tests DifferentialGate.)
add_test(NAME p5emu.endtoend        COMMAND p5emu_tests EndToEnd.)
add_test(NAME p5emu.provenance      COMMAND p5emu_tests Provenance.)

# --- Phase 2 suites ---
add_test(NAME p5emu.arm64.emitter   COMMAND p5emu_tests Arm64Emitter.)
add_test(NAME p5emu.arm64.codegen   COMMAND p5emu_tests Arm64Codegen.)
add_test(NAME p5emu.arm64.differential COMMAND p5emu_tests Arm64Differential.)
add_test(NAME p5emu.arm64.gate      COMMAND p5emu_tests Arm64Gate.)
add_test(NAME p5emu.arm64.pipeline  COMMAND p5emu_tests Arm64Pipeline.)
add_test(NAME p5emu.arm64.property  COMMAND p5emu_tests Arm64Property.)
add_test(NAME p5emu.arm64.debug     COMMAND p5emu_tests Arm64Debug.)

# --- Phase 3 suites ---
add_test(NAME p5emu.widths          COMMAND p5emu_tests Arm64Widths.)
add_test(NAME p5emu.partialreg      COMMAND p5emu_tests Arm64PartialReg.)
add_test(NAME p5emu.shifts          COMMAND p5emu_tests Arm64Shifts.)
add_test(NAME p5emu.memwidth        COMMAND p5emu_tests Arm64MemWidth.)
add_test(NAME p5emu.edge            COMMAND p5emu_tests Arm64Edge.)
add_test(NAME p5emu.controlflow     COMMAND p5emu_tests Arm64ControlFlow.)
add_test(NAME p5emu.neggate         COMMAND p5emu_tests Arm64NegGate.)
add_test(NAME p5emu.random          COMMAND p5emu_tests Arm64Random.)

# --- Phase 4 suites ---
add_test(NAME p5emu.loader.elf      COMMAND p5emu_tests LoaderElf.)
add_test(NAME p5emu.loader.ps5      COMMAND p5emu_tests LoaderPs5.)
add_test(NAME p5emu.loader.plan     COMMAND p5emu_tests LoaderPlan.)
add_test(NAME p5emu.loader.apply    COMMAND p5emu_tests LoaderApply.)
add_test(NAME p5emu.loader.e2e      COMMAND p5emu_tests LoaderE2E.)
add_test(NAME p5emu.loader.malformed COMMAND p5emu_tests LoaderMalformed.)
add_test(NAME p5emu.loader.fuzz     COMMAND p5emu_tests LoaderFuzz.)

# --- Phase 5 suites ---
add_test(NAME p5emu.reloc.parse     COMMAND p5emu_tests RelocParse.)
add_test(NAME p5emu.reloc.apply     COMMAND p5emu_tests RelocApply.)
add_test(NAME p5emu.reloc.property  COMMAND p5emu_tests RelocProperty.)
add_test(NAME p5emu.import.model    COMMAND p5emu_tests ImportModel.)
add_test(NAME p5emu.reloc.e2e       COMMAND p5emu_tests RelocE2E.)
add_test(NAME p5emu.reloc.negative  COMMAND p5emu_tests RelocNegative.)
add_test(NAME p5emu.reloc.fuzz      COMMAND p5emu_tests RelocFuzz.)

# --- Phase 6 suites ---
add_test(NAME p5emu.hle.registry    COMMAND p5emu_tests HleRegistry.)
add_test(NAME p5emu.hle.abi         COMMAND p5emu_tests HleAbi.)
add_test(NAME p5emu.hle.synthetic   COMMAND p5emu_tests HleSynthetic.)
add_test(NAME p5emu.hle.e2e         COMMAND p5emu_tests HleE2E.)
add_test(NAME p5emu.hle.differential COMMAND p5emu_tests HleDifferential.)
add_test(NAME p5emu.hle.negative    COMMAND p5emu_tests HleNegative.)
add_test(NAME p5emu.hle.fuzz        COMMAND p5emu_tests HleFuzz.)
add_test(NAME p5emu.hle.property    COMMAND p5emu_tests HleProperty.)
add_test(NAME p5emu.hle.transactional COMMAND p5emu_tests HleTransactional.)

# --- Phase 7 suites ---
add_test(NAME p5emu.runtime.addressspace COMMAND p5emu_tests RuntimeAddressSpace.)
add_test(NAME p5emu.runtime.stack    COMMAND p5emu_tests RuntimeStack.)
add_test(NAME p5emu.runtime.startup  COMMAND p5emu_tests RuntimeStartup.)
add_test(NAME p5emu.runtime.graph    COMMAND p5emu_tests RuntimeGraph.)
add_test(NAME p5emu.runtime.bias     COMMAND p5emu_tests RuntimeBias.)
add_test(NAME p5emu.runtime.lifecycle COMMAND p5emu_tests RuntimeLifecycle.)
add_test(NAME p5emu.runtime.resolve  COMMAND p5emu_tests RuntimeResolve.)
add_test(NAME p5emu.runtime.merge    COMMAND p5emu_tests RuntimeMerge.)
add_test(NAME p5emu.runtime.multimodule COMMAND p5emu_tests RuntimeMultiModule.)
add_test(NAME p5emu.runtime.hlestack COMMAND p5emu_tests RuntimeHleStackArgs.)
add_test(NAME p5emu.runtime.negative COMMAND p5emu_tests RuntimeNegative.)
add_test(NAME p5emu.runtime.readiness COMMAND p5emu_tests RuntimeReadiness.)
add_test(NAME p5emu.runtime.fuzz     COMMAND p5emu_tests RuntimeFuzz.)
add_test(NAME p5emu.runtime.property COMMAND p5emu_tests RuntimeProperty.)

# --- Phase 8 suites ---
add_test(NAME p5emu.runtimeprofile.classify COMMAND p5emu_tests RuntimeProfileClassify.)
add_test(NAME p5emu.runtimeprofile.startup COMMAND p5emu_tests RuntimeStartupFields.)
add_test(NAME p5emu.runtimeprofile.materialize COMMAND p5emu_tests RuntimeStartupMaterialize.)
add_test(NAME p5emu.runtimeprofile.tls COMMAND p5emu_tests RuntimeTlsGap.)
add_test(NAME p5emu.runtimeprofile.loadbias COMMAND p5emu_tests RuntimeLoadBiasEvidence.)
add_test(NAME p5emu.runtimeprofile.metadata COMMAND p5emu_tests RuntimeModuleMetadata.)
add_test(NAME p5emu.runtimeprofile.initorder COMMAND p5emu_tests RuntimeInitOrder.)
add_test(NAME p5emu.runtimeprofile.e2e COMMAND p5emu_tests RuntimeProfileE2E.)
add_test(NAME p5emu.runtimeprofile.negative COMMAND p5emu_tests RuntimeProfileNegative.)
add_test(NAME p5emu.runtimeprofile.property COMMAND p5emu_tests RuntimeProfileProperty.)

# --- Phase 9 suites ---
add_test(NAME p5emu.sce.tags       COMMAND p5emu_tests SceTagRegistry.)
add_test(NAME p5emu.sce.metadata   COMMAND p5emu_tests SceMetadataBuild.)
add_test(NAME p5emu.sce.nid        COMMAND p5emu_tests SceNidRepresentation.)
add_test(NAME p5emu.sce.library    COMMAND p5emu_tests SceLibraryIdentity.)
add_test(NAME p5emu.sce.negative   COMMAND p5emu_tests SceMetadataNegative.)
add_test(NAME p5emu.sce.property   COMMAND p5emu_tests SceMetadataProperty.)
add_test(NAME p5emu.sce.e2e        COMMAND p5emu_tests SceDynamicLinkingE2E.)

# Suite inventory, for the Phase 1 gate report.
set(P5EMU_TEST_SUITES
  "p5emu.core;core_test.cpp: Core (logging, status, version)"
  "p5emu.version;Version.: Version identity"
  "p5emu.cpu.contracts;cpu_contracts_test.cpp: machine state and naming contracts"
  "p5emu.memory.port;guest_memory_test.cpp: GuestMemoryPort contract"
  "p5emu.ir.validate;ir_test.cpp: ValidateIr positive and negative"
  "p5emu.ir.build;ir_test.cpp: IR construction and dump determinism"
  "p5emu.decoder;decoder_test.cpp: x86-64 decode, field-level"
  "p5emu.decoder.roundtrip;decoder_test.cpp: decode/re-encode round trip"
  "p5emu.decoder.malformed;decoder_test.cpp: malformed and truncated input"
  "p5emu.interpreter;interpreter_test.cpp: reference execution"
  "p5emu.interpreter.flags;interpreter_test.cpp: x86 flag semantics"
  "p5emu.interpreter.controlflow;interpreter_test.cpp: branches and terminators"
  "p5emu.interpreter.memory;interpreter_test.cpp: load/store semantics"
  "p5emu.differential;differential_test.cpp: backend agreement"
  "p5emu.differential.gate;differential_test.cpp: harness catches divergence"
  "p5emu.endtoend;end_to_end_test.cpp: bytes -> decode -> IR -> result"
  "p5emu.provenance;provenance_test.cpp: expected-value provenance"
  "p5emu.arm64.emitter;arm64_test.cpp: AArch64 instruction encodings"
  "p5emu.arm64.codegen;arm64_test.cpp: IR -> AArch64 codegen and W^X buffer"
  "p5emu.arm64.differential;arm64_test.cpp: interpreter vs AArch64 (independent execution)"
  "p5emu.arm64.gate;arm64_test.cpp: injected codegen defect is caught"
  "p5emu.arm64.pipeline;arm64_test.cpp: x86 bytes -> IR -> AArch64 execution"
  "p5emu.arm64.property;arm64_test.cpp: seeded random programs agree"
  "p5emu.arm64.debug;arm64_test.cpp: emit-code dump for disassembly"
  "p5emu.widths;widths_test.cpp: 8/16/32/64-bit ALU and flags"
  "p5emu.partialreg;widths_test.cpp: AL/AX/EAX/RAX write rules"
  "p5emu.shifts;widths_test.cpp: SHL/SHR/SAR, counts, flags"
  "p5emu.memwidth;widths_test.cpp: 1/2/4/8-byte memory access"
  "p5emu.edge;widths_test.cpp: carry/borrow/overflow boundaries"
  "p5emu.controlflow;widths_test.cpp: condition codes, nesting, loops"
  "p5emu.neggate;widths_test.cpp: injected width/shift defects are caught"
  "p5emu.random;widths_test.cpp: batched randomized differential (2500 cases)"
  "p5emu.loader.elf;loader_test.cpp: generic ELF64 parse and malformed rejection"
  "p5emu.loader.ps5;loader_test.cpp: PS5 dialect, dynamic tags, imports"
  "p5emu.loader.plan;loader_test.cpp: LoadPlan validation, BSS, permissions"
  "p5emu.loader.apply;loader_test.cpp: transactional apply"
  "p5emu.loader.e2e;loader_test.cpp: fixture -> loader -> guest memory -> CPU"
  "p5emu.loader.malformed;loader_fuzz_test.cpp: malformed ELF categories"
  "p5emu.loader.fuzz;loader_fuzz_test.cpp: deterministic mutation campaign"
  "p5emu.reloc.parse;reloc_test.cpp: RELA parsing and REL rejection"
  "p5emu.reloc.apply;reloc_test.cpp: relocation formulas and load bias"
  "p5emu.reloc.property;reloc_test.cpp: relocation property oracle"
  "p5emu.import.model;reloc_test.cpp: import requirement and resolver statuses"
  "p5emu.reloc.e2e;reloc_test.cpp: ELF -> reloc -> import -> CPU"
  "p5emu.reloc.negative;reloc_test.cpp: injected relocation/import defects caught"
  "p5emu.reloc.fuzz;reloc_fuzz_test.cpp: relocation mutation campaign"
  "p5emu.hle.registry;hle_test.cpp: HLE registry, trampolines, ambiguity"
  "p5emu.hle.abi;hle_test.cpp: guest ABI and checked guest memory"
  "p5emu.hle.synthetic;hle_test.cpp: synthetic HLE functions"
  "p5emu.hle.e2e;hle_test.cpp: ELF -> reloc -> HLE -> CPU"
  "p5emu.hle.differential;hle_test.cpp: interpreter vs AArch64 across HLE"
  "p5emu.hle.negative;hle_test.cpp: injected HLE/ABI defects caught"
  "p5emu.hle.fuzz;hle_fuzz_test.cpp: HLE dispatch/identity malformed campaign"
  "p5emu.hle.property;hle_fuzz_test.cpp: HLE arithmetic property cases"
  "p5emu.runtime.addressspace;runtime_test.cpp: multi-region address space"
  "p5emu.runtime.stack;runtime_test.cpp: guest stack, push/pop, alignment"
  "p5emu.runtime.startup;runtime_test.cpp: initial process stack (argc/argv/envp/auxv)"
  "p5emu.runtime.graph;runtime_test.cpp: dependency graph ordering"
  "p5emu.runtime.bias;runtime_test.cpp: load-bias allocator"
  "p5emu.runtime.lifecycle;runtime_test.cpp: module state transitions"
  "p5emu.runtime.resolve;runtime_test.cpp: guest-first then HLE identity resolution"
  "p5emu.runtime.merge;runtime_test.cpp: IR merge and guest-call linking"
  "p5emu.runtime.multimodule;runtime_test.cpp: guest->guest->HLE E2E (interpreter + AArch64)"
  "p5emu.runtime.hlestack;runtime_test.cpp: stack-argument HLE ABI"
  "p5emu.runtime.negative;runtime_test.cpp: injected runtime defects caught"
  "p5emu.runtime.readiness;runtime_test.cpp: unresolved imports block readiness"
  "p5emu.runtime.fuzz;runtime_fuzz_test.cpp: multi-module load campaign (6000 cases)"
  "p5emu.runtime.property;runtime_fuzz_test.cpp: graph/bias property cases (2000 cases)"
  "p5emu.runtimeprofile.classify;runtime_profile_test.cpp: generic-vs-PS5 profile selection"
  "p5emu.runtimeprofile.startup;runtime_profile_test.cpp: structured startup state known/unknown"
  "p5emu.runtimeprofile.materialize;runtime_profile_test.cpp: explicit refusal, no fabrication"
  "p5emu.runtimeprofile.tls;runtime_profile_test.cpp: TLS/thread-pointer gap fails explicitly"
  "p5emu.runtimeprofile.loadbias;runtime_profile_test.cpp: synthetic load-bias is not PS5 ASLR"
  "p5emu.runtimeprofile.metadata;runtime_profile_test.cpp: evidence-bounded module metadata"
  "p5emu.runtimeprofile.initorder;runtime_profile_test.cpp: synthetic vs PS5-unknown init order"
  "p5emu.runtimeprofile.e2e;runtime_profile_test.cpp: loader->startup->CPU (interpreter + AArch64)"
  "p5emu.runtimeprofile.negative;runtime_profile_test.cpp: UNKNOWN/evidence boundary proofs"
  "p5emu.runtimeprofile.property;runtime_profile_test.cpp: randomized evidence-boundary campaign"
  "p5emu.sce.tags;sce_metadata_test.cpp: tag number vs semantics knowledge"
  "p5emu.sce.metadata;sce_metadata_test.cpp: dynamic table -> typed SCE metadata"
  "p5emu.sce.nid;sce_metadata_test.cpp: NID representation, not generation"
  "p5emu.sce.library;sce_metadata_test.cpp: opaque library identity preservation"
  "p5emu.sce.negative;sce_metadata_test.cpp: SCE evidence-boundary negative proofs"
  "p5emu.sce.property;sce_metadata_test.cpp: randomized classification campaign (2000 cases)"
  "p5emu.sce.e2e;sce_metadata_test.cpp: metadata + linker + runtime + CPU (interpreter + AArch64)"
)
