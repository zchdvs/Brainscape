add_library(brainscape_dsp STATIC
  src/Engine.cpp
  src/Granular.cpp
  src/OnsetDetector.cpp
  src/PostChain.cpp
  src/TestSignal.cpp
)
target_include_directories(brainscape_dsp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(brainscape_dsp PUBLIC cxx_std_17)

# Mirror libDaisy's constraints on the host build so violations surface immediately
# (docs/research/vst-and-shared-dsp.md rec #3): no exceptions, no RTTI in dsp/.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(brainscape_dsp PRIVATE -fno-exceptions -fno-rtti -Wall -Wextra)
  if(BRAINSCAPE_WERROR)
    target_compile_options(brainscape_dsp PRIVATE -Werror)
  endif()
elseif(MSVC)
  # MSVC: RTTI off; exception flags stay at the toolchain default for the static lib —
  # the code uses neither, and the GCC/Clang CI jobs enforce -fno-exceptions.
  target_compile_options(brainscape_dsp PRIVATE /GR- /W4)
  if(BRAINSCAPE_WERROR)
    target_compile_options(brainscape_dsp PRIVATE /WX)
  endif()
endif()

if(BRAINSCAPE_BUILD_TESTS)
  add_subdirectory(tests)
endif()

# The golden-hash harness cross-built for the Cortex-M7 with the firmware flags and run
# under qemu-arm user mode (docs/design/determinism-profile.md §6.2, the M7 leg). Needs
# the arm-none-eabi toolchain file; the host harness builds with the tests.
option(BRAINSCAPE_BUILD_M7_ORACLE "Build the golden harness as a Cortex-M7 ELF for qemu-arm" OFF)
if(BRAINSCAPE_BUILD_M7_ORACLE)
  if(BRAINSCAPE_BUILD_TESTS)
    message(FATAL_ERROR "BRAINSCAPE_BUILD_M7_ORACLE needs BRAINSCAPE_BUILD_TESTS=OFF (the tests are host-only)")
  endif()
  add_subdirectory(tests/golden)
endif()
