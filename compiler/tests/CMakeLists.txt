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

# The compiler's tests (§10.1, §10.2): the JSON grammar suite, schema 1's rules E1-E12 and the
# canonical form, packages, decompile, verify and diff, lint L1-L9 and derive, and the property
# and reader-fuzz digests every host must reproduce. They reach dsp/src's blob/Blob.h for the
# decoder and validator with every feature (the vocabulary of later waves).
add_executable(bsc_tests
  ../../dsp/tests/test_main.cpp
  test_compile.cpp
  test_json.cpp
  test_lint.cpp
  test_property.cpp
)
target_link_libraries(bsc_tests PRIVATE brainscape_compiler)
target_include_directories(bsc_tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../../dsp/tests/vendor
  ${CMAKE_CURRENT_SOURCE_DIR}/../../dsp/src)
if(MSVC)
  target_compile_options(bsc_tests PRIVATE /EHsc /bigobj)
endif()
add_test(NAME compiler_unit COMMAND bsc_tests)

# bspc-roundtrip over the committed example documents (§8.3): each compiles, is canonical and
# stamped, decompiles to itself, rebuilds without its JSON section and recompiles to the same
# bytes; the sorted manifest of package, sound and control hashes must equal the committed one
# on every host. The examples commit no package: the manifest holds their hashes (the golden
# corpus's packages are committed, dsp/tests/golden/presets/).
file(GLOB _bsc_examples RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}/data CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/data/*.json)
list(SORT _bsc_examples)
add_test(NAME compiler_roundtrip
         COMMAND bspc roundtrip --expect MANIFEST ${_bsc_examples}
         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/data)

# bspc's command line (§8.2): unknown options refused without touching a file, the authoring
# sequence's messages, manifest differences by document, and non-ANSI file names on Windows.
add_test(NAME compiler_bspc_cli
         COMMAND ${CMAKE_COMMAND} -DBSPC=$<TARGET_FILE:bspc> -DDATA=${CMAKE_CURRENT_SOURCE_DIR}/data
                 -DWORK=${CMAKE_CURRENT_BINARY_DIR}/bspc_cli
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/bspc_cli.cmake)
