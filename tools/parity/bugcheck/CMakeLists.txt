cmake_minimum_required(VERSION 3.21)
project(brainscape_bugcheck LANGUAGES CXX)

# Which dsp/ tree to build against: the repo (default, read-only use) or a scratch copy.
set(BUGCHECK_DSP_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../dsp" CACHE PATH "dsp/ source tree")
option(BRAINSCAPE_BUILD_TESTS "Build the dsp/ unit tests from the chosen tree" OFF)
option(BRAINSCAPE_WERROR "Warnings as errors" OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(BRAINSCAPE_BUILD_TESTS)
  enable_testing()
endif()

add_subdirectory(${BUGCHECK_DSP_DIR} ${CMAKE_BINARY_DIR}/dsp_build)

add_executable(bugcheck bugcheck.cpp)
target_link_libraries(bugcheck PRIVATE brainscape_dsp)
if(MSVC)
  target_compile_options(bugcheck PRIVATE /EHsc)
endif()

option(BUGCHECK_PROBE "Harness reads the instrumented dsp_probe counters" OFF)
if(BUGCHECK_PROBE)
  target_compile_definitions(bugcheck PRIVATE BUGCHECK_PROBE=1)
endif()

add_executable(suitecheck suitecheck.cpp)
target_link_libraries(suitecheck PRIVATE brainscape_dsp)
