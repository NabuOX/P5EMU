# Sanitizer support. Sanitizers are a debugging tool, never a correctness claim.

set(P5EMU_SANITIZER "none" CACHE STRING "Sanitizer to enable: none|address|undefined|thread")
set_property(CACHE P5EMU_SANITIZER PROPERTY STRINGS none address undefined thread)

add_library(p5emu_sanitizers INTERFACE)

if(P5EMU_SANITIZER STREQUAL "address")
  target_compile_options(p5emu_sanitizers INTERFACE -fsanitize=address -fno-omit-frame-pointer -g)
  target_link_options(p5emu_sanitizers INTERFACE -fsanitize=address)
  set(P5EMU_SANITIZER_DESCRIPTION "AddressSanitizer")
elseif(P5EMU_SANITIZER STREQUAL "undefined")
  target_compile_options(p5emu_sanitizers INTERFACE -fsanitize=undefined -fno-omit-frame-pointer -g)
  target_link_options(p5emu_sanitizers INTERFACE -fsanitize=undefined)
  set(P5EMU_SANITIZER_DESCRIPTION "UndefinedBehaviorSanitizer")
elseif(P5EMU_SANITIZER STREQUAL "thread")
  target_compile_options(p5emu_sanitizers INTERFACE -fsanitize=thread -fno-omit-frame-pointer -g)
  target_link_options(p5emu_sanitizers INTERFACE -fsanitize=thread)
  set(P5EMU_SANITIZER_DESCRIPTION "ThreadSanitizer")
else()
  set(P5EMU_SANITIZER_DESCRIPTION "none")
endif()
