# Undefined-symbol audit of the engine archive (docs/design/determinism-profile.md §6.3).
# dsp/ may reference nothing from a runtime library beyond the allow list for its
# toolchain, and never libm, so a call site swapped back to libm fails here even where
# libm happens to round like DetMath (§6.4). A symbol counts when some object in the
# archive references it and none defines it. Script mode, so the Cortex-M7 leg can run
# it on the cross-built archive:
#
#   cmake -DLIB=<archive> -DTOOL=<nm, llvm-nm or dumpbin> -DFLAVOR=<gnu|apple|arm|msvc>
#         [-DDEBUG=ON] [-DSANITIZERS=ON] -P cmake/BrainscapeSymbolAudit.cmake
#
# DEBUG allows the assertion and runtime-check hooks of a build without NDEBUG;
# SANITIZERS allows the sanitizer runtimes' entry points.
cmake_minimum_required(VERSION 3.21)

foreach(v LIB TOOL FLAVOR)
  if("${${v}}" STREQUAL "")
    message(FATAL_ERROR "symbol audit: -D${v}=... is required")
  endif()
endforeach()

# §6.3's lists. Additions, none of them math: stack-protector hooks (the counterpart of
# MSVC's security cookie, on by default in distribution GCC), fortified memory calls,
# Darwin's memset-to-zero and stack-probe calls, and in MSVC Debug builds the /RTC1
# checks and the debug CRT behind the STL's own assertions.
set(allow "memset|memmove|memcpy")
if(FLAVOR STREQUAL "msvc")
  string(APPEND allow "|_fltused|__security_cookie|__security_check_cookie|__GSHandlerCheck|__ImageBase")
  set(allow_debug "_wassert|_RTC_[A-Za-z]+|_CrtDbgReport|_invalid_parameter")
elseif(FLAVOR STREQUAL "arm")
  # 64-bit integer helpers and integer<->float conversions; soft-float arithmetic
  # (__aeabi_dadd and the like) would mean wrong FPU flags.
  string(APPEND allow
    "|__aeabi_(u?idiv|u?idivmod|u?ldivmod|llsl|llsr|lasr|lmul|u?lcmp|[df]2u?lz|u?l2[df]|mem(cpy|move|set|clr)[48]?)")
  set(allow_debug "__assert_func")
elseif(FLAVOR STREQUAL "gnu")
  # GCC on AArch64 calls libgcc's outline-atomics helpers (integer atomics only).
  string(APPEND allow "|__stack_chk_fail|__stack_chk_guard|__(memset|memmove|memcpy)_chk")
  string(APPEND allow "|__aarch64_(cas|swp|ldadd|ldclr|ldeor|ldset)(1|2|4|8|16)_(relax|acq|rel|acq_rel|sync)")
  set(allow_debug "__assert_fail")
elseif(FLAVOR STREQUAL "apple")
  string(APPEND allow "|__stack_chk_fail|__stack_chk_guard|__chkstk_darwin|bzero|__bzero")
  set(allow_debug "__assert_rtn")
else()
  message(FATAL_ERROR "symbol audit: unknown FLAVOR '${FLAVOR}' (gnu, apple, arm or msvc)")
endif()
if(DEBUG)
  string(APPEND allow "|${allow_debug}")
endif()
if(SANITIZERS)
  string(APPEND allow "|__(asan|ubsan|lsan|sanitizer)_[A-Za-z0-9_]+")
endif()

get_filename_component(tool_name "${TOOL}" NAME_WE)
if(tool_name MATCHES "^dumpbin$")
  set(cmd "${TOOL}" /nologo /symbols "${LIB}")
else()
  set(cmd "${TOOL}" -P "${LIB}")
endif()
execute_process(COMMAND ${cmd} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "symbol audit: '${cmd}' failed (${rc}):\n${err}")
endif()
# Keep the output a well-formed CMake list: brackets group and semicolons split.
string(REPLACE ";" "," out "${out}")
string(REPLACE "[" "(" out "${out}")
string(REPLACE "]" ")" out "${out}")
string(REPLACE "\n" ";" lines "${out}")

set(undefined "")
set(defined "")
foreach(line IN LISTS lines)
  if(tool_name MATCHES "^dumpbin$")
    # 00A 00000000 UNDEF  notype ()    External     | memset
    if(NOT line MATCHES "^[0-9A-F]+ [0-9A-F]+ ([A-Z0-9]+) [^|]* External +\\| ([^ ]+)")
      continue()
    endif()
    set(name "${CMAKE_MATCH_2}")
    set(undef "${CMAKE_MATCH_1}")
    string(COMPARE EQUAL "${undef}" "UNDEF" undef)
  else()
    # POSIX format: name type [value size]; w and v are weak undefined.
    if(NOT line MATCHES "^([^ ]+) ([A-Za-z])( |$)")
      continue()
    endif()
    set(name "${CMAKE_MATCH_1}")
    string(REGEX MATCH "^[Uwv]$" undef "${CMAKE_MATCH_2}")
  endif()
  # string(REGEX REPLACE "^_" ...) would strip every leading underscore (CMake re-anchors
  # after each match), turning Darwin's ___stack_chk_fail into stack_chk_fail.
  if(name MATCHES "^__imp_(.+)$")  # MSVC dllimport thunk
    set(name "${CMAKE_MATCH_1}")
  endif()
  if(FLAVOR STREQUAL "apple" AND name MATCHES "^_(.+)$")
    set(name "${CMAKE_MATCH_1}")
  endif()
  if(undef)
    list(APPEND undefined "${name}")
  else()
    list(APPEND defined "${name}")
  endif()
endforeach()
if(NOT defined)
  message(FATAL_ERROR "symbol audit: no symbols read from ${LIB}; '${cmd}' output not understood")
endif()

list(REMOVE_DUPLICATES undefined)
list(REMOVE_ITEM undefined ${defined})
set(rejected "")
foreach(name IN LISTS undefined)
  if(NOT name MATCHES "^(${allow})$")
    list(APPEND rejected "${name}")
  endif()
endforeach()
list(JOIN undefined " " undefined)
if(rejected)
  list(JOIN rejected " " rejected)
  message(FATAL_ERROR "symbol audit: ${LIB} references symbols outside the ${FLAVOR} allow "
                      "list\n  rejected: ${rejected}\n  unresolved: ${undefined}")
endif()
message(STATUS "symbol audit: ${LIB} references only allowed symbols: ${undefined}")
