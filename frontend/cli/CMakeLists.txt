# P5EMU CLI. Developer tool only: no game support, no loader, no compatibility.

add_executable(p5emu-cli main.cpp)

target_link_libraries(p5emu-cli PRIVATE p5emu_core p5emu_compiler_flags p5emu_sanitizers)

p5emu_apply_target_flags(p5emu-cli)
