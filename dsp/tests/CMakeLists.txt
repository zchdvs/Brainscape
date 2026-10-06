# Host-only test harness (docs/research/vst-and-shared-dsp.md rec #7): never built
# under the arm-none-eabi toolchain. Tests keep exceptions on (Catch2 needs them);
# the dsp/ library itself is compiled without.
add_executable(brainscape_tests
  test_main.cpp
  test_engine.cpp
  test_param_display.cpp
)
target_link_libraries(brainscape_tests PRIVATE brainscape_dsp)
target_include_directories(brainscape_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/vendor)
if(MSVC)
  target_compile_options(brainscape_tests PRIVATE /EHsc /bigobj)
endif()
add_test(NAME dsp_unit COMMAND brainscape_tests)

# Offline render tool: fast-iteration audio out of the same engine the pedal runs.
add_executable(brainscape_render render_main.cpp)
target_link_libraries(brainscape_render PRIVATE brainscape_dsp)
