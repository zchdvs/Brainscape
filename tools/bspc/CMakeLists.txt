# bspc (docs/design/mode-compiler.md §8.2): the preset compiler's command-line tool, a thin
# shell over brainscape_compiler. Hosts only.
add_executable(bspc main.cpp)
target_link_libraries(bspc PRIVATE brainscape_compiler)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(bspc PRIVATE -Wall -Wextra)
  if(BRAINSCAPE_WERROR)
    target_compile_options(bspc PRIVATE -Werror)
  endif()
elseif(MSVC)
  target_compile_options(bspc PRIVATE /W4 /EHsc)
  target_compile_definitions(bspc PRIVATE _CRT_SECURE_NO_WARNINGS)  # std::fopen
  # UTF-8 as the process code page: preset file names outside the ANSI code page open.
  target_sources(bspc PRIVATE bspc.manifest)
  if(BRAINSCAPE_WERROR)
    target_compile_options(bspc PRIVATE /WX)
  endif()
endif()
