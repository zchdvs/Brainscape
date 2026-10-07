cmake_minimum_required(VERSION 3.22)  # JUCE 9.0.3's minimum (docs/design/companion-app.md §3.2)
project(brainscape LANGUAGES CXX)

# Polyglot monorepo superbuild (docs/research/vst-and-shared-dsp.md, recommended layout):
# dsp/ always builds; firmware/ and plugin/ are opt-in so a contributor configuring
# only the piece they work on never needs arm-none-eabi-gcc or a plugin framework.
option(BRAINSCAPE_BUILD_TESTS    "Build the dsp/ unit tests and offline render tool" ON)
option(BRAINSCAPE_BUILD_FIRMWARE "Build the Daisy Seed firmware (arm-none-eabi toolchain)" OFF)
option(BRAINSCAPE_BUILD_PLUGIN   "Build the desktop plugin and companion app (fetches JUCE)" OFF)
option(BRAINSCAPE_WERROR         "Treat warnings as errors (on in CI)" OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The floating-point build profile (docs/design/determinism-profile.md §3): defines
# brainscape::fp_profile, which brainscape_dsp links PUBLIC, the test-only
# -DBRAINSCAPE_FP_NEGATIVE_CONTROL=ON escape (never cached, and refused together with
# the plugin or firmware options above), and the configure-time check that rejects
# forbidden FP and LTO settings on every target that compiles or links the engine, the
# plugin's JUCE targets included. dsp/ includes it as well (include_guard), for projects
# that add dsp/ on its own.
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/BrainscapeFpProfile.cmake)

if(BRAINSCAPE_BUILD_TESTS)
  enable_testing()
endif()

if(BRAINSCAPE_BUILD_PLUGIN)
  # Before add_subdirectory(dsp), and in plugin builds only (companion §3.2): the engine
  # must link into a shared object, and a second Brainscape build loaded in the same DAW
  # must not bind to this one's symbols. Never set on brainscape_dsp itself: in the
  # firmware archive these add GOT relocations (determinism profile §3.5).
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
  set(CMAKE_C_VISIBILITY_PRESET hidden)
  set(CMAKE_CXX_VISIBILITY_PRESET hidden)
  set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
endif()

add_subdirectory(dsp)

# The desktop-only preset compiler (docs/design/mode-compiler.md §8.1): hosts only, never in a
# cross build for the pedal.
if(NOT CMAKE_CROSSCOMPILING)
  add_subdirectory(compiler)
endif()

if(BRAINSCAPE_BUILD_FIRMWARE)
  message(STATUS "brainscape: firmware/ build not yet implemented — lands with hardware bring-up")
endif()

if(BRAINSCAPE_BUILD_PLUGIN)
  # JUCE reads the enabled languages before its project() call and compiles C sources;
  # enabling C here keeps every other configure C++-only.
  enable_language(C)
  # Pinned archive (companion §3.2); FETCHCONTENT_SOURCE_DIR_JUCE serves offline builds.
  # Tag 9.0.3 = commit be29c81492b6151c8ea8d14c840e1311963b3a83.
  include(FetchContent)
  if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)  # extracted files get the extraction time
  endif()
  FetchContent_Declare(JUCE
    URL      https://github.com/juce-framework/JUCE/archive/refs/tags/9.0.3.tar.gz
    URL_HASH SHA256=a81e5508b8a0efa483917794ebeaff56aed3075730c405a947734413f40c1aba
  )
  FetchContent_MakeAvailable(JUCE)
  add_subdirectory(plugin)
endif()
