include(${CMAKE_CURRENT_LIST_DIR}/../cmake/BrainscapeFpProfile.cmake)

# Deterministic archives (docs/design/determinism-profile.md §6.2): one archive links
# into the firmware and both oracles, so it carries no timestamps or owner ids.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND NOT APPLE AND NOT MSVC)
  set(CMAKE_CXX_ARCHIVE_CREATE "<CMAKE_AR> qcD <TARGET> <LINK_FLAGS> <OBJECTS>")
  set(CMAKE_CXX_ARCHIVE_APPEND "<CMAKE_AR> qD <TARGET> <LINK_FLAGS> <OBJECTS>")
  set(CMAKE_CXX_ARCHIVE_FINISH "<CMAKE_RANLIB> -D <TARGET>")
endif()

add_library(brainscape_dsp STATIC
  src/DetMath.cpp
  src/Engine.cpp
  src/EventQueue.cpp
  src/Granular.cpp
  src/InputCondition.cpp
  src/OnsetDetector.cpp
  src/ParamDisplay.cpp
  src/PostChain.cpp
  src/SoundRevision.cpp
  src/TestSignal.cpp
)
target_include_directories(brainscape_dsp
  PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)  # floating-point bodies stay private (§3.5)
target_compile_features(brainscape_dsp PUBLIC cxx_std_17)
# PUBLIC: every translation unit that includes engine headers compiles under the profile.
target_link_libraries(brainscape_dsp PUBLIC brainscape::fp_profile)

# The toolchain ID's floating-point flags (docs/design/determinism-profile.md §5.12), for
# brainscape::BuildToolchain(): the profile's flags, then the floating-point and target
# flags of CMAKE_CXX_FLAGS and the configuration's flags, and the SHA-256 of that text.
function(_brainscape_fp_flags_define config condition)
  get_target_property(words brainscape_fp_profile INTERFACE_COMPILE_OPTIONS)
  string(TOUPPER "${config}" cfg)
  separate_arguments(flags NATIVE_COMMAND "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${cfg}}")
  foreach(w IN LISTS flags)
    if(w MATCHES "^([-/](fp|arch):|/clang:|-f(no-)?(fast-math|fp-|math-errno|unsafe-math|associative-math|reciprocal-math|finite-math|signed-zeros|trapping-math|rounding-math|honor-|approx-func|denormal-fp-math|single-precision-constant|excess-precision)|-m(arch|cpu|tune|fpu|float-abi|fpmath|thumb|avx|fma|sse|daz-ftz|32|64))")
      list(APPEND words "${w}")
    endif()
  endforeach()
  list(JOIN words " " text)
  string(SHA256 hash "${text}")
  string(SUBSTRING "${hash}" 0 16 hash)
  target_compile_definitions(brainscape_dsp PRIVATE
    "$<${condition}:BRAINSCAPE_FP_FLAGS=\"${text}\">"
    "$<${condition}:BRAINSCAPE_FP_FLAGS_HASH=\"${hash}\">")
endfunction()
if(CMAKE_CONFIGURATION_TYPES)
  foreach(c IN LISTS CMAKE_CONFIGURATION_TYPES)
    _brainscape_fp_flags_define("${c}" "$<CONFIG:${c}>")
  endforeach()
else()
  _brainscape_fp_flags_define("${CMAKE_BUILD_TYPE}" 1)
endif()

# Mirror libDaisy's constraints on the host build so violations surface immediately
# (docs/research/vst-and-shared-dsp.md rec #3): no exceptions, no RTTI in dsp/.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(brainscape_dsp PRIVATE -fno-exceptions -fno-rtti -Wall -Wextra)
  if(BRAINSCAPE_WERROR)
    target_compile_options(brainscape_dsp PRIVATE -Werror)
  endif()
elseif(MSVC)
  # RTTI and exceptions off, as above. Under CMake's default /EHsc the out-of-line
  # constructors get unwind tables and pull in the C++ EH runtime, which the symbol
  # audit rejects (determinism-profile.md §6.3).
  target_compile_options(brainscape_dsp PRIVATE /EHs-c- /GR- /W4)
  if(BRAINSCAPE_WERROR)
    target_compile_options(brainscape_dsp PRIVATE /WX)
  endif()
endif()

# The test-only negative control (cmake/BrainscapeFpProfile.cmake) says so whenever the
# engine is rebuilt, not only at configure time, where --log-level=ERROR hides warnings.
if(BRAINSCAPE_FP_NEGATIVE_CONTROL)
  add_custom_command(TARGET brainscape_dsp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E echo
            "brainscape_dsp: NEGATIVE CONTROL BUILD (BRAINSCAPE_FP_NEGATIVE_CONTROL): floating-point contraction ON, not profile-conforming, never ship it"
    VERBATIM)
endif()

# The golden-hash harness cross-built for the Cortex-M7 with the firmware flags and run
# under qemu-arm user mode (docs/design/determinism-profile.md §6.2, the M7 leg). Needs
# the arm-none-eabi toolchain file; the host harness builds with the tests.
option(BRAINSCAPE_BUILD_M7_ORACLE "Build the golden harness as a Cortex-M7 ELF for qemu-arm" OFF)

# The engine built again with the FP guard's test hooks (determinism profile §6.4):
# flushing forced on inside the guard, and the exception flags each call raised. A
# separate library, so no shipping build carries the hooks' global state; the host tests
# and the forced-flush harness, the M7's included, link it.
if(BRAINSCAPE_BUILD_TESTS OR BRAINSCAPE_BUILD_M7_ORACLE)
  get_target_property(_bs_dsp_sources brainscape_dsp SOURCES)
  add_library(brainscape_dsp_fpenv_hooks STATIC ${_bs_dsp_sources})
  target_include_directories(brainscape_dsp_fpenv_hooks
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_compile_features(brainscape_dsp_fpenv_hooks PUBLIC cxx_std_17)
  target_link_libraries(brainscape_dsp_fpenv_hooks PUBLIC brainscape::fp_profile)
  target_compile_options(brainscape_dsp_fpenv_hooks
    PRIVATE $<TARGET_PROPERTY:brainscape_dsp,COMPILE_OPTIONS>)
  target_compile_definitions(brainscape_dsp_fpenv_hooks
    PRIVATE $<TARGET_PROPERTY:brainscape_dsp,COMPILE_DEFINITIONS>)
  target_compile_definitions(brainscape_dsp_fpenv_hooks PUBLIC BRAINSCAPE_FPENV_TEST_HOOKS=1)
endif()

if(BRAINSCAPE_BUILD_TESTS)
  add_subdirectory(tests)
endif()

if(BRAINSCAPE_BUILD_M7_ORACLE)
  if(BRAINSCAPE_BUILD_TESTS)
    message(FATAL_ERROR "BRAINSCAPE_BUILD_M7_ORACLE needs BRAINSCAPE_BUILD_TESTS=OFF (the tests are host-only)")
  endif()
  add_subdirectory(tests/golden)
endif()
