# The tempo core on every leg (docs/design/clock.md §8.2): seeded streams through TempoCore and the
# reference model, and a committed digest of TempoCore's outputs that the host legs and the
# emulated Cortex-M7 must all reproduce. The M7 oracle build (BRAINSCAPE_BUILD_M7_ORACLE) links the
# firmware's archive into an ELF that qemu-arm user mode runs, as the golden harness and the
# package tool.
if(BRAINSCAPE_BUILD_M7_ORACLE)
  enable_language(C)
endif()

add_executable(brainscape_tempo_tool tempo_tool.cpp)
target_link_libraries(brainscape_tempo_tool PRIVATE brainscape_dsp)
target_include_directories(brainscape_tempo_tool PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..
                                                         ${CMAKE_CURRENT_SOURCE_DIR}/../../src)

if(BRAINSCAPE_BUILD_M7_ORACLE)
  target_sources(brainscape_tempo_tool PRIVATE ../golden/m7/syscalls.c)
  target_compile_options(brainscape_tempo_tool PRIVATE
    "$<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions;-fno-rtti;-Wno-psabi>")
  target_link_options(brainscape_tempo_tool PRIVATE
    -nostartfiles -static -Wl,-Ttext-segment=0x400000 -Wl,--gc-sections)
  set_target_properties(brainscape_tempo_tool PROPERTIES SUFFIX .elf)
else()
  if(MSVC)
    target_compile_options(brainscape_tempo_tool PRIVATE /EHsc)
  endif()
  add_test(NAME tempo_digest COMMAND brainscape_tempo_tool --check)
endif()
