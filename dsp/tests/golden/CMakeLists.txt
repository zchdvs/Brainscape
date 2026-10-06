# Golden-hash harness (docs/design/determinism-profile.md §6.1). One source for every
# leg: the host build registers it with ctest in report mode; the M7 oracle build
# (BRAINSCAPE_BUILD_M7_ORACLE) links the same brainscape_dsp archive the firmware
# links into an ELF that qemu-arm user mode runs.
add_executable(brainscape_golden
  golden_main.cpp
  Corpus.cpp
  EventScript.cpp
  Render.cpp
)
target_link_libraries(brainscape_golden PRIVATE brainscape_dsp)
# The engine's private FP-environment header, for the hostile-environment renders.
target_include_directories(brainscape_golden PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../src)
target_compile_definitions(brainscape_golden PRIVATE
  BRAINSCAPE_GOLDEN_FILE="${CMAKE_CURRENT_SOURCE_DIR}/golden.json")

if(BRAINSCAPE_BUILD_M7_ORACLE)
  enable_language(C)
  target_sources(brainscape_golden PRIVATE m7/syscalls.c)
  target_compile_options(brainscape_golden PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions;-fno-rtti;-Wno-psabi>")
  # qemu-user cannot map a segment at address 0, where the bare-metal default puts .text.
  target_link_options(brainscape_golden PRIVATE
    -nostartfiles -static -Wl,-Ttext-segment=0x400000 -Wl,--gc-sections)
  set_target_properties(brainscape_golden PROPERTIES SUFFIX .elf)
else()
  # Report mode: passes unless a render fails or a preset's coverage is missing. Hash
  # values gate nothing until a sound revision is minted (profile §8.4 step 10).
  # Debug builds skip the ablation renders (Release proves coverage) but keep every
  # vector, so assertions run over the whole corpus.
  add_test(NAME golden_report
    COMMAND brainscape_golden --mode report --tag ctest-$<CONFIG>
            --report ${CMAKE_CURRENT_BINARY_DIR}/golden-report-$<CONFIG>.json
            $<IF:$<CONFIG:Debug>,--no-ablate,--ablate>)
endif()
