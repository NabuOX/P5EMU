cmake_minimum_required(VERSION 3.20)

# P5EMU is intentionally portable: no Xcode, no MSVC-only, no /dev access in core.
# Apple/iOS support is added later via apps/ios/ WITHOUT introducing Xcode
# requirements into this file or into core/.
project(P5EMU
  VERSION 0.0.0
  DESCRIPTION "Independent PlayStation 5 emulator architecture (Phase 0 skeleton)"
  LANGUAGES C CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")

set(CMAKE_C_STANDARD 20)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

include(P5EMUCompilerSetup)
include(P5EMUSanitizers)

# Phase 0 intentionally has no third-party dependencies. Nothing is fetched at
# configure time: the build must work on an air-gapped machine.

option(P5EMU_BUILD_TESTS    "Build P5EMU tests"       ON)
option(P5EMU_BUILD_CLI      "Build P5EMU CLI tool"   ON)
option(P5EMU_WERROR         "Treat warnings as errors" OFF)

add_subdirectory(core)

if(P5EMU_BUILD_CLI)
  add_subdirectory(frontend/cli)
endif()

if(P5EMU_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

message(STATUS "P5EMU ${PROJECT_VERSION}")
message(STATUS "  build type      : ${CMAKE_BUILD_TYPE}")
message(STATUS "  host arch       : ${CMAKE_SYSTEM_PROCESSOR}")
message(STATUS "  compiler        : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  tests           : ${P5EMU_BUILD_TESTS}")
message(STATUS "  sanitizers      : ${P5EMU_SANITIZER_DESCRIPTION}")
