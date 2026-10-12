# Compiler setup for P5EMU.
#
# Rules enforced here:
#   * core/ must compile with Clang, GCC and (later) Apple Clang unchanged.
#   * No compiler-specific extension is enabled unconditionally.
#   * No target-architecture assumptions leak out of core/.

set(CMAKE_CXX_CLANG_TIDY "" CACHE STRING "clang-tidy binary (empty = disabled)")

add_library(p5emu_compiler_flags INTERFACE)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(p5emu_compiler_flags INTERFACE
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wconversion
    -Wsign-conversion
    -Wcast-qual
    -Wpointer-arith
    -Wnon-virtual-dtor
    -Wold-style-cast
  )
  if(P5EMU_WERROR)
    target_compile_options(p5emu_compiler_flags INTERFACE -Werror)
  endif()
elseif(MSVC)
  target_compile_options(p5emu_compiler_flags INTERFACE /W4 /permissive-)
  if(P5EMU_WERROR)
    target_compile_options(p5emu_compiler_flags INTERFACE /WX)
  endif()
endif()

# Large translations units (decoder tables, IR) benefit from optimisation even in
# debug builds. Enabled per-target, not globally, so tests stay debuggable.
function(p5emu_apply_target_flags target)
  target_compile_options(${target} PRIVATE $<$<CONFIG:Debug>:-O1>)
endfunction()
