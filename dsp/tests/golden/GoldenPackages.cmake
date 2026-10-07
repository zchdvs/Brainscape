# brainscape_golden_packages(<target>): a static library holding the corpus's committed
# packages (EmbeddedPackages.h), generated from presets/MANIFEST and the .bsp files at build
# time (EmbedPackages.cmake). Linking it defines BRAINSCAPE_GOLDEN_EMBEDDED_PACKAGES for the
# consumer's sources, so the golden harness's LoadPackage (EventScript.cpp) reads the table
# instead of presets/NAME.bsp: the Daisy Seed images have no file system (firmware/README.md),
# and brainscape_parity_stream renders as the parity image does.
include_guard(GLOBAL)
set(_bs_golden_dir ${CMAKE_CURRENT_LIST_DIR})

function(brainscape_golden_packages target)
  set(presets ${_bs_golden_dir}/presets)
  set(out ${CMAKE_CURRENT_BINARY_DIR}/${target}.cpp)
  # A package changes only with its MANIFEST line; a new one also reconfigures.
  file(GLOB bsps CONFIGURE_DEPENDS ${presets}/*.bsp)
  add_custom_command(OUTPUT ${out}
    COMMAND ${CMAKE_COMMAND} -DPRESETS=${presets} -DOUT=${out}
            -P ${_bs_golden_dir}/EmbedPackages.cmake
    DEPENDS ${presets}/MANIFEST ${bsps} ${_bs_golden_dir}/EmbedPackages.cmake
    COMMENT "Embedding the golden corpus's packages (presets/MANIFEST)"
    VERBATIM)
  add_library(${target} STATIC ${out})
  target_include_directories(${target} PUBLIC ${_bs_golden_dir})
  target_compile_features(${target} PUBLIC cxx_std_17)
  target_compile_definitions(${target} INTERFACE BRAINSCAPE_GOLDEN_EMBEDDED_PACKAGES)
endfunction()
