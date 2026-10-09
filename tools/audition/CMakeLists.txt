# The audition tooling (docs/design/mode-compiler.md §8.1, §11.3): the shared offline render, the
# scripts S0-S11, the pre-screen's metrics and the recipes, for `bspc render` and the app. Hosts
# only and no JUCE (companion §3.5); namespace bsa, outside brainscape:: (companion §3.4). It
# links brainscape_dsp, so the engine's floating-point profile and the configure-time flag check
# reach it; the metrics' own binary64 arithmetic changes no render.
add_library(brainscape_audition STATIC
  src/Hash.cpp
  src/Inputs.cpp
  src/JsonWriter.cpp
  src/Metrics.cpp
  src/Recipe.cpp
  src/Render.cpp
  src/Suite.cpp
  src/Wav.cpp
)
target_include_directories(brainscape_audition PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(brainscape_audition PUBLIC cxx_std_17)
target_link_libraries(brainscape_audition PUBLIC brainscape_dsp)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(brainscape_audition PRIVATE -Wall -Wextra)
  if(BRAINSCAPE_WERROR)
    target_compile_options(brainscape_audition PRIVATE -Werror)
  endif()
  # GCC before 9 keeps std::filesystem in its own library.
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9)
    target_link_libraries(brainscape_audition PUBLIC stdc++fs)
  endif()
elseif(MSVC)
  target_compile_options(brainscape_audition PRIVATE /W4 /EHsc)
  target_compile_definitions(brainscape_audition PRIVATE _CRT_SECURE_NO_WARNINGS)  # std::fopen
  if(BRAINSCAPE_WERROR)
    target_compile_options(brainscape_audition PRIVATE /WX)
  endif()
endif()

if(BRAINSCAPE_BUILD_TESTS)
  add_executable(bsa_tests ../../dsp/tests/test_main.cpp tests/test_audition.cpp)
  target_link_libraries(bsa_tests PRIVATE brainscape_audition)
  target_include_directories(bsa_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../dsp/tests/vendor)
  target_compile_definitions(bsa_tests PRIVATE
    BSA_CORPUS_PRESETS="${CMAKE_CURRENT_SOURCE_DIR}/../../dsp/tests/golden/presets")
  if(MSVC)
    target_compile_options(bsa_tests PRIVATE /EHsc /bigobj)
    target_compile_definitions(bsa_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
  endif()
  add_test(NAME audition_unit COMMAND bsa_tests)

  # `bspc render` end to end (tools/bspc), and the ratings log's carry-forward (ratings.py), over
  # the compiler's example documents.
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    add_test(NAME audition_ratings
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ratings.py)
    add_test(NAME audition_bspc_render
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_render_cli.py
                     --bspc $<TARGET_FILE:bspc>
                     --data ${CMAKE_CURRENT_SOURCE_DIR}/../../compiler/tests/data
                     --work ${CMAKE_CURRENT_BINARY_DIR}/render_cli)
  endif()
endif()
