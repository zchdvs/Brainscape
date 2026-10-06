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
  src/Granular.cpp
  src/OnsetDetector.cpp
  src/PostChain.cpp
)
target_include_directories(brainscape_dsp
  PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)  # floating-point bodies stay private (§3.5)
target_compile_features(brainscape_dsp PUBLIC cxx_std_17)
# PUBLIC: every translation unit that includes engine headers compiles under the profile.
target_link_libraries(brainscape_dsp PUBLIC brainscape::fp_profile)

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

if(BRAINSCAPE_BUILD_TESTS)
  add_subdirectory(tests)
endif()
