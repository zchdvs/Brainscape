# The package checks that run on every leg (docs/design/mode-compiler.md §10.2, §10.3): the
# deterministic mutation fuzzer, whose verdict digest the host and the emulated Cortex-M7 must
# both reproduce, and the frozen fixtures. The M7 oracle build (BRAINSCAPE_BUILD_M7_ORACLE)
# links the firmware's archive into an ELF that qemu-arm user mode runs, as the golden harness.
if(BRAINSCAPE_BUILD_M7_ORACLE)
  enable_language(C)
endif()

add_executable(brainscape_blob_tool blob_tool.cpp BlobSamples.cpp Fixtures.cpp)
target_link_libraries(brainscape_blob_tool PRIVATE brainscape_dsp)
target_include_directories(brainscape_blob_tool PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../src)
set(_bs_frozen ${CMAKE_CURRENT_SOURCE_DIR}/../golden/presets/frozen)

if(BRAINSCAPE_BUILD_M7_ORACLE)
  target_sources(brainscape_blob_tool PRIVATE ../golden/m7/syscalls.c)
  target_compile_options(brainscape_blob_tool PRIVATE
    "$<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions;-fno-rtti;-Wno-psabi>")
  target_link_options(brainscape_blob_tool PRIVATE
    -nostartfiles -static -Wl,-Ttext-segment=0x400000 -Wl,--gc-sections)
  set_target_properties(brainscape_blob_tool PROPERTIES SUFFIX .elf)
else()
  if(MSVC)
    target_compile_options(brainscape_blob_tool PRIVATE /EHsc)
  endif()
  add_test(NAME blob_fuzz COMMAND brainscape_blob_tool --fuzz)
  add_test(NAME blob_fixtures COMMAND brainscape_blob_tool --fixtures ${_bs_frozen})
endif()

# The libFuzzer harness, for Clang with -fsanitize=fuzzer (the Linux Clang leg; nightly with a
# kept corpus). Configure the whole build with the sanitizers in CMAKE_CXX_FLAGS
# (-fsanitize=fuzzer-no-link,address,undefined) so the engine archive is instrumented too.
option(BRAINSCAPE_BUILD_LIBFUZZER "Build the libFuzzer harness of the package decoder" OFF)
if(BRAINSCAPE_BUILD_LIBFUZZER AND NOT BRAINSCAPE_BUILD_M7_ORACLE)
  add_executable(brainscape_blob_libfuzzer blob_libfuzzer.cpp BlobSamples.cpp)
  target_link_libraries(brainscape_blob_libfuzzer PRIVATE brainscape_dsp)
  target_include_directories(brainscape_blob_libfuzzer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../src)
  target_compile_options(brainscape_blob_libfuzzer PRIVATE -fsanitize=fuzzer)
  target_link_options(brainscape_blob_libfuzzer PRIVATE -fsanitize=fuzzer)
endif()
