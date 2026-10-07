# The number code's tests (docs/design/mode-compiler.md §10.2): unit cases, and the hashed sets
# every leg runs per pull request (halfway strings at stride 1009, 20 million random strings,
# a strided round trip), each hashed over fixed index chunks combined in order, so no hash
# depends on the thread count. The exhaustive round trip (bsc_number_check --suite exhaustive,
# about 4.3e9 floats) is nightly.
add_executable(bsc_number_tests ../../dsp/tests/test_main.cpp test_number.cpp)
target_link_libraries(bsc_number_tests PRIVATE brainscape_compiler)
target_include_directories(bsc_number_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../dsp/tests/vendor)
add_test(NAME compiler_number_unit COMMAND bsc_number_tests)

find_package(Threads REQUIRED)
add_executable(bsc_number_check number_check.cpp)
target_link_libraries(bsc_number_check PRIVATE brainscape_compiler Threads::Threads)
add_test(NAME compiler_number_hashes COMMAND bsc_number_check --suite pr)
if(MSVC)
  target_compile_options(bsc_number_tests PRIVATE /EHsc)
  target_compile_options(bsc_number_check PRIVATE /EHsc)
endif()

# The compiler's tests (§10.1, §10.2): so far the JSON grammar suite.
add_executable(bsc_tests
  ../../dsp/tests/test_main.cpp
  test_json.cpp
)
target_link_libraries(bsc_tests PRIVATE brainscape_compiler)
target_include_directories(bsc_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../dsp/tests/vendor)
if(MSVC)
  target_compile_options(bsc_tests PRIVATE /EHsc /bigobj)
endif()
add_test(NAME compiler_unit COMMAND bsc_tests)
