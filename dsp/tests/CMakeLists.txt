# Host-only test harness (docs/research/vst-and-shared-dsp.md rec #7): never built
# under the arm-none-eabi toolchain. Tests keep exceptions on (Catch2 needs them);
# the dsp/ library itself is compiled without.
add_executable(brainscape_tests
  test_main.cpp
  test_engine.cpp
  test_detmath.cpp
)
target_link_libraries(brainscape_tests PRIVATE brainscape_dsp)
# The engine's private headers: the unit tests exercise its internals directly.
target_include_directories(brainscape_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/vendor
                                                    ${CMAKE_CURRENT_SOURCE_DIR}/../src)
if(MSVC)
  target_compile_options(brainscape_tests PRIVATE /EHsc /bigobj)
endif()
add_test(NAME dsp_unit COMMAND brainscape_tests)

# Offline render tool: fast-iteration audio out of the same engine the pedal runs.
add_executable(brainscape_render render_main.cpp)
target_link_libraries(brainscape_render PRIVATE brainscape_dsp)

# Self-test of the configure-time forbidden-flag check (§6.3), with this build's
# generator and compiler.
get_filename_component(_bs_dsp_dir .. ABSOLUTE BASE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(_bs_probe_args "-DGENERATOR=${CMAKE_GENERATOR}" "-DPLATFORM=${CMAKE_GENERATOR_PLATFORM}"
                   "-DTOOLSET=${CMAKE_GENERATOR_TOOLSET}")
if(NOT CMAKE_GENERATOR MATCHES "^(Visual Studio|Xcode)")
  list(APPEND _bs_probe_args "-DCXX=${CMAKE_CXX_COMPILER}" "-DMAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}")
endif()
if(CMAKE_TOOLCHAIN_FILE)
  list(APPEND _bs_probe_args "-DTOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}")
endif()
add_test(NAME dsp_fp_profile_check
         COMMAND ${CMAKE_COMMAND} ${_bs_probe_args}
                 -DPROBE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/fpcheck -DDSP_DIR=${_bs_dsp_dir}
                 -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/fpcheck
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/FpProfileCheck.cmake)
