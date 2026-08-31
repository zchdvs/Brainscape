cmake_minimum_required(VERSION 3.21)
project(brainscape LANGUAGES CXX)

# Polyglot monorepo superbuild (docs/research/vst-and-shared-dsp.md, recommended layout):
# dsp/ always builds; firmware/ and plugin/ are opt-in so a contributor configuring
# only the piece they work on never needs arm-none-eabi-gcc or a plugin framework.
option(BRAINSCAPE_BUILD_TESTS    "Build the dsp/ unit tests and offline render tool" ON)
option(BRAINSCAPE_BUILD_FIRMWARE "Build the Daisy Seed firmware (arm-none-eabi toolchain)" OFF)
option(BRAINSCAPE_BUILD_PLUGIN   "Build the desktop plugin" OFF)
option(BRAINSCAPE_WERROR         "Treat warnings as errors (on in CI)" OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(BRAINSCAPE_BUILD_TESTS)
  enable_testing()
endif()

add_subdirectory(dsp)

if(BRAINSCAPE_BUILD_FIRMWARE)
  message(STATUS "brainscape: firmware/ build not yet implemented — lands with hardware bring-up")
endif()
if(BRAINSCAPE_BUILD_PLUGIN)
  message(STATUS "brainscape: plugin/ build not yet implemented")
endif()
