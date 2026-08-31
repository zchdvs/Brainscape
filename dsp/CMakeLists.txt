add_library(brainscape_dsp STATIC
  src/Engine.cpp
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
