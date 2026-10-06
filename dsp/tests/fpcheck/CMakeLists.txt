# Probe project for the self-test of the forbidden-flag check (FpProfileCheck.cmake;
# docs/design/determinism-profile.md §6.3). Configured, never built: consumers of the
# engine, each given one forbidden setting by a different route.
cmake_minimum_required(VERSION 3.21)
project(brainscape_fpcheck LANGUAGES CXX)

add_subdirectory(${BRAINSCAPE_DSP_DIR} dsp)

function(consumer name)
  set(src "${CMAKE_CURRENT_BINARY_DIR}/${name}.cpp")
  file(WRITE "${src}" "#include \"brainscape/Engine.h\"\nint main() { return 0; }\n")
  add_executable(${name} "${src}")
  target_link_libraries(${name} PRIVATE brainscape_dsp)
endfunction()

consumer(clean_consumer)

if(BRAINSCAPE_FPCHECK_VIOLATIONS)
  # JUCE 9.0.3's juce_recommended_lto_flags (JUCEHelperTargets.cmake): MSVC's flag sits
  # between commas.
  add_library(lto_flags INTERFACE)
  target_compile_options(lto_flags INTERFACE
    $<$<CONFIG:Release>:$<IF:$<STREQUAL:"${CMAKE_CXX_COMPILER_FRONTEND_VARIANT}","MSVC">,-GL,-flto>>)
  consumer(via_juce_lto)
  target_link_libraries(via_juce_lto PRIVATE lto_flags)

  consumer(via_ipo)
  set_property(TARGET via_ipo PROPERTY INTERPROCEDURAL_OPTIMIZATION ON)

  consumer(via_single_precision)
  target_compile_options(via_single_precision PRIVATE -fsingle-precision-constant)

  consumer(via_source_options)
  set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/via_source_options.cpp"
                              PROPERTIES COMPILE_OPTIONS "-ffp-contract=fast")

  # A source of the engine itself, set from outside its directory.
  set_source_files_properties("${BRAINSCAPE_DSP_DIR}/src/PostChain.cpp"
                              TARGET_DIRECTORY brainscape_dsp PROPERTIES COMPILE_FLAGS "/fp:contract")
endif()
