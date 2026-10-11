# Host-only test harness (docs/research/vst-and-shared-dsp.md rec #7): never built
# under the arm-none-eabi toolchain. Tests keep exceptions on (Catch2 needs them);
# the dsp/ library itself is compiled without.
add_executable(brainscape_tests
  test_main.cpp
  test_blob.cpp
  blob/BlobSamples.cpp
  blob/Fixtures.cpp
  test_engine.cpp
  test_state.cpp
  test_sync.cpp
  test_boundary.cpp
  test_clock.cpp
  test_detmath.cpp
  test_host_tempo.cpp
  test_intmath.cpp
  test_midiclock.cpp
  test_mode_eval.cpp
  test_modes.cpp
  test_onset.cpp
  test_param_display.cpp
  test_params.cpp
  test_tempo.cpp
  test_tempo_rules.cpp
  test_testsignal.cpp
  test_wave1.cpp
)
target_link_libraries(brainscape_tests PRIVATE brainscape_dsp)
# The engine's private headers: the unit tests exercise its internals directly.
target_include_directories(brainscape_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/vendor
                                                    ${CMAKE_CURRENT_SOURCE_DIR}/../src)
# The frozen package fixtures (docs/design/mode-compiler.md §10.3), read by test_blob.cpp.
target_compile_definitions(brainscape_tests PRIVATE
  BRAINSCAPE_FROZEN_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/golden/presets/frozen")
# The factory packages and their manifest (UsesTempo on each, docs/design/clock.md §6.6, §8.2).
target_compile_definitions(brainscape_tests PRIVATE
  BRAINSCAPE_FACTORY_PACKAGES="${CMAKE_CURRENT_SOURCE_DIR}/../../firmware/factory")
if(MSVC)
  target_compile_options(brainscape_tests PRIVATE /EHsc /bigobj)
endif()
# The toolchain-ID test compares BuildToolchain() with the build's own flags, read here and
# not through dsp/CMakeLists.txt's filter.
function(_brainscape_test_flags_define config condition)
  string(TOUPPER "${config}" cfg)
  string(REGEX REPLACE "[\"\\\\]" "" flags "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${cfg}}")
  target_compile_definitions(brainscape_tests PRIVATE
    "$<${condition}:BRAINSCAPE_TEST_BUILD_FLAGS=\"${flags}\">")
endfunction()
if(CMAKE_CONFIGURATION_TYPES)
  foreach(c IN LISTS CMAKE_CONFIGURATION_TYPES)
    _brainscape_test_flags_define("${c}" "$<CONFIG:${c}>")
  endforeach()
else()
  _brainscape_test_flags_define("${CMAKE_BUILD_TYPE}" 1)
endif()
add_test(NAME dsp_unit COMMAND brainscape_tests)

# The forced-flush tests, on the engine built with the guard's test hooks
# (dsp/CMakeLists.txt).
get_filename_component(_bs_dsp_dir .. ABSOLUTE BASE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
add_executable(brainscape_fpenv_tests test_main.cpp test_flush.cpp)
target_link_libraries(brainscape_fpenv_tests PRIVATE brainscape_dsp_fpenv_hooks)
target_include_directories(brainscape_fpenv_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/vendor
                                                          ${_bs_dsp_dir}/src)
if(MSVC)
  target_compile_options(brainscape_fpenv_tests PRIVATE /EHsc)
endif()
add_test(NAME dsp_fpenv_forced_flush COMMAND brainscape_fpenv_tests)

# The firmware's engine arenas against PlanMemory (firmware/platform/Placement.h), on every
# host build: the images check the same at boot, on hardware.
add_executable(firmware_arena_plan ${CMAKE_CURRENT_SOURCE_DIR}/../../firmware/tests/arena_plan.cpp)
target_link_libraries(firmware_arena_plan PRIVATE brainscape_dsp)
target_include_directories(firmware_arena_plan PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../firmware)
add_test(NAME firmware_arena_plan COMMAND firmware_arena_plan)

# The live image's presets and mode structures (firmware/live/LivePresets.h) from the packages
# compiled in, as the image builds them at boot: each loads exactly, and so does each with
# every structure the onset and marks commands select.
add_executable(firmware_live_presets
  ${CMAKE_CURRENT_SOURCE_DIR}/../../firmware/tests/live_presets.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../../firmware/live/LivePresets.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/golden/Corpus.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/golden/EventScript.cpp)
target_link_libraries(firmware_live_presets PRIVATE brainscape_dsp brainscape_golden_packages)
target_include_directories(firmware_live_presets PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../firmware
                                                         ${CMAKE_CURRENT_SOURCE_DIR}/golden)
add_test(NAME firmware_live_presets COMMAND firmware_live_presets)

# The firmware's event contract (firmware/platform/EventBudget.h, cpu-budget.md §4.3 step 8):
# its rules, and a random same-frame stream rendered as staged and coalesced, bit for bit.
add_executable(firmware_event_budget
  ${CMAKE_CURRENT_SOURCE_DIR}/../../firmware/tests/event_budget.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../../firmware/platform/EventBudget.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/golden/EventScript.cpp)
target_link_libraries(firmware_event_budget PRIVATE brainscape_dsp brainscape_golden_packages)
target_include_directories(firmware_event_budget PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../firmware
                                                          ${CMAKE_CURRENT_SOURCE_DIR}/golden)
add_test(NAME firmware_event_budget COMMAND firmware_event_budget)

# The bench images' configurations (firmware/bench/Configs.h) on the host: the output hashes the
# Seed must match (cpu-budget.md §8.3, G1) and the shadow ledger's worst block. The ctest checks
# that every configuration builds, loads exactly and renders.
set(_bs_fw ${CMAKE_CURRENT_SOURCE_DIR}/../../firmware)
set(_bs_murmuration ${CMAKE_CURRENT_BINARY_DIR}/FactoryMurmuration.cpp)
add_custom_command(OUTPUT ${_bs_murmuration}
  COMMAND ${CMAKE_COMMAND} -DIN=${_bs_fw}/factory/murmuration.bsp -DOUT=${_bs_murmuration}
          -DSYMBOL=kFactoryMurmuration -DHEADER=bench/FactoryPackages.h
          -DNAMESPACE=brainscape::fw::bench -P ${_bs_fw}/cmake/EmbedFile.cmake
  DEPENDS ${_bs_fw}/factory/murmuration.bsp ${_bs_fw}/cmake/EmbedFile.cmake
  COMMENT "Embedding firmware/factory/murmuration.bsp"
  VERBATIM)
add_executable(brainscape_bench_configs
  ${_bs_fw}/tests/bench_configs.cpp
  ${_bs_fw}/bench/Configs.cpp
  ${_bs_murmuration}
  ${CMAKE_CURRENT_SOURCE_DIR}/golden/Corpus.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/golden/EventScript.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/golden/Render.cpp)
target_link_libraries(brainscape_bench_configs PRIVATE brainscape_dsp brainscape_golden_packages)
target_include_directories(brainscape_bench_configs PRIVATE ${_bs_fw} ${CMAKE_CURRENT_SOURCE_DIR}/golden
                                                            ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_definitions(brainscape_bench_configs PRIVATE
  BRAINSCAPE_GOLDEN_FILE="${CMAKE_CURRENT_SOURCE_DIR}/golden/golden.json"
  BRAINSCAPE_GOLDEN_PRESETS="${CMAKE_CURRENT_SOURCE_DIR}/golden/presets")
add_test(NAME firmware_bench_configs COMMAND brainscape_bench_configs --check)

# The bench image's micros of the engine's internals (firmware/bench/EngineMicro.h), run once with
# a stand-in clock: their setups, before a Seed runs them.
add_executable(firmware_engine_micro
  ${_bs_fw}/tests/engine_micro.cpp
  ${_bs_fw}/bench/EngineMicro.cpp)
target_link_libraries(firmware_engine_micro PRIVATE brainscape_dsp)
target_include_directories(firmware_engine_micro PRIVATE ${_bs_fw} ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME firmware_engine_micro COMMAND firmware_engine_micro)

# Offline render tool: fast-iteration audio out of the same engine the pedal runs.
add_executable(brainscape_render render_main.cpp)
target_link_libraries(brainscape_render PRIVATE brainscape_dsp)

# Undefined-symbol audit of the engine archive (determinism profile §6.3), and its
# negative control (§6.4): a library left on libm must fail it.
if(MSVC)
  get_filename_component(_bs_cl_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
  find_program(BRAINSCAPE_DUMPBIN dumpbin HINTS "${_bs_cl_dir}")
  set(_bs_audit_tool "${BRAINSCAPE_DUMPBIN}")
  set(_bs_audit_flavor msvc)
elseif(APPLE)
  set(_bs_audit_tool "${CMAKE_NM}")
  set(_bs_audit_flavor apple)
else()
  set(_bs_audit_tool "${CMAKE_NM}")
  set(_bs_audit_flavor gnu)
endif()
set(_bs_flags "${CMAKE_CXX_FLAGS}")
foreach(c IN LISTS CMAKE_CONFIGURATION_TYPES CMAKE_BUILD_TYPE)
  string(TOUPPER "${c}" c)
  string(APPEND _bs_flags " ${CMAKE_CXX_FLAGS_${c}}")
endforeach()
set(_bs_sanitizers OFF)
if(_bs_flags MATCHES "-fsanitize=")
  set(_bs_sanitizers ON)
endif()
set(_bs_audit ${CMAKE_COMMAND} "-DTOOL=${_bs_audit_tool}" -DFLAVOR=${_bs_audit_flavor}
    "-DDEBUG=$<NOT:$<CONFIG:Release,RelWithDebInfo,MinSizeRel>>" "-DSANITIZERS=${_bs_sanitizers}")
set(_bs_audit_script ${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/BrainscapeSymbolAudit.cmake)

add_test(NAME dsp_symbol_audit
         COMMAND ${_bs_audit} -DLIB=$<TARGET_FILE:brainscape_dsp> -P ${_bs_audit_script})

add_library(brainscape_audit_control STATIC audit_control.cpp)
add_test(NAME dsp_symbol_audit_control
         COMMAND ${_bs_audit} -DLIB=$<TARGET_FILE:brainscape_audit_control> -P ${_bs_audit_script})
set_tests_properties(dsp_symbol_audit_control PROPERTIES
                     PASS_REGULAR_EXPRESSION "rejected: [^\n]*exp2")

# Self-test of the configure-time forbidden-flag check (§6.3), with this build's
# generator and compiler.
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
                 -DWORK_DIR=${CMAKE_BINARY_DIR}/fpcheck
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/FpProfileCheck.cmake)

add_subdirectory(golden)
add_subdirectory(blob)
add_subdirectory(tempo)
