# brainscape_compiler (docs/design/mode-compiler.md §8.1): the desktop-only preset compiler,
# namespace bsc, built for hosts only and never part of the sound (it is gated by its committed
# outputs and the package rule, §8.3, not by kSoundRevision): the exact number code (§6.5), the
# strict JSON reader and canonical writer, the schema (one table, Schema.cpp), Compile,
# Decompile, Format, Migrate, Pack, Lint and Derive. Integer-only: where lint and derive need
# floating point they call guarded dsp/ functions (EvalMacro, NearGuardMs, FormatPlain).
# Linking brainscape_dsp (descriptors, the decoder and validator, the encoder, kSoundRevision)
# puts it under the configure-time floating-point flag check.
add_library(brainscape_compiler STATIC
  src/Compile.cpp
  src/Document.cpp
  src/Json.cpp
  src/Lint.cpp
  src/Migrate.cpp
  src/Number.cpp
  src/Schema.cpp
  src/Text.cpp
)
target_include_directories(brainscape_compiler PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(brainscape_compiler PUBLIC cxx_std_17)
target_link_libraries(brainscape_compiler PUBLIC brainscape_dsp)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(brainscape_compiler PRIVATE -Wall -Wextra)
  if(BRAINSCAPE_WERROR)
    target_compile_options(brainscape_compiler PRIVATE -Werror)
  endif()
elseif(MSVC)
  target_compile_options(brainscape_compiler PRIVATE /W4 /EHsc)
  if(BRAINSCAPE_WERROR)
    target_compile_options(brainscape_compiler PRIVATE /WX)
  endif()
endif()

if(BRAINSCAPE_BUILD_TESTS)
  add_subdirectory(tests)
endif()
