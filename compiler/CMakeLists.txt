# brainscape_compiler (docs/design/mode-compiler.md §8.1): the desktop-only preset compiler,
# namespace bsc, built for hosts only and never part of the sound (it is gated by its committed
# outputs and the package rule, §8.3, not by kSoundRevision). Today it holds the exact binary32
# number code (§6.5), which lane B landed ahead of the rest of lane A: the JSON reader and
# writer, the schema, Compile, Decompile, Format, Lint, Derive and bspc follow.
add_library(brainscape_compiler STATIC src/Number.cpp)
target_include_directories(brainscape_compiler PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(brainscape_compiler PUBLIC cxx_std_17)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(brainscape_compiler PRIVATE -Wall -Wextra)
  if(BRAINSCAPE_WERROR)
    target_compile_options(brainscape_compiler PRIVATE -Werror)
  endif()
elseif(MSVC)
  target_compile_options(brainscape_compiler PRIVATE /W4)
  if(BRAINSCAPE_WERROR)
    target_compile_options(brainscape_compiler PRIVATE /WX)
  endif()
endif()

if(BRAINSCAPE_BUILD_TESTS)
  add_subdirectory(tests)
endif()
