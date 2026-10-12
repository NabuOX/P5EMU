# P5EMU core.
#
# Phase 0 scope: foundation only.
#   - Version/identity
#   - Status (error) type
#   - Logging facility
#   - Architectural interfaces for the CPU execution model and guest memory
#
# Nothing here emulates anything. No PS5 behaviour is implemented or guessed.

add_library(p5emu_core STATIC
  src/Version.cpp
  src/Status.cpp
  src/Log.cpp
  src/CpuModel.cpp
  src/LinearGuestMemory.cpp
  src/Ir.cpp
  src/IrValidate.cpp
  src/IrBuilder.cpp
  src/X64Decoder.cpp
  src/X64Encode.cpp
  src/TranslateToIr.cpp
  src/ReferenceInterpreter.cpp
  src/Backend.cpp
  src/Differential.cpp
  src/Arm64Emitter.cpp
  src/Arm64CodeGen.cpp
  src/Arm64Backend.cpp
  src/Arm64Runner.cpp
  src/CodeBuffer.cpp
  src/MappedGuestMemory.cpp
  src/loader/Elf64.cpp
  src/loader/Ps5Executable.cpp
  src/loader/Reloc.cpp
  src/loader/Import.cpp
  src/loader/Loader.cpp
  src/hle/Hle.cpp
  src/runtime/Runtime.cpp
  src/runtime/RuntimeProfile.cpp
)

target_include_directories(p5emu_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

target_link_libraries(p5emu_core PUBLIC p5emu_compiler_flags)

p5emu_apply_target_flags(p5emu_core)
