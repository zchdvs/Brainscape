# Self-test of the configure-time forbidden-flag check (docs/design/determinism-profile.md
# §3.2, §3.4, §6.3): the probe project (fpcheck/) must configure without its violations
# and fail with them, naming every route. Run by CTest in script mode with the parent
# build's generator and compiler.
cmake_minimum_required(VERSION 3.21)

# Never built, so the compiler check is skipped: its try-compile adds nothing and, under
# a deep build directory, overflows MSBuild's path limit.
set(args -S "${PROBE_DIR}" -G "${GENERATOR}" "-DBRAINSCAPE_DSP_DIR=${DSP_DIR}"
         -DCMAKE_CXX_COMPILER_WORKS=ON)
if(PLATFORM)
  list(APPEND args -A "${PLATFORM}")
endif()
if(TOOLSET)
  list(APPEND args -T "${TOOLSET}")
endif()
if(CXX)
  list(APPEND args "-DCMAKE_CXX_COMPILER=${CXX}")
endif()
if(MAKE_PROGRAM)
  list(APPEND args "-DCMAKE_MAKE_PROGRAM=${MAKE_PROGRAM}")
endif()
if(TOOLCHAIN_FILE)
  list(APPEND args "-DCMAKE_TOOLCHAIN_FILE=${TOOLCHAIN_FILE}")
endif()
file(REMOVE_RECURSE "${WORK_DIR}")

execute_process(COMMAND "${CMAKE_COMMAND}" ${args} -B "${WORK_DIR}/clean"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "fp profile check: the clean probe failed to configure:\n${out}")
endif()

execute_process(COMMAND "${CMAKE_COMMAND}" ${args} -B "${WORK_DIR}/violations"
                        -DBRAINSCAPE_FPCHECK_VIOLATIONS=ON
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
if(rc EQUAL 0)
  message(FATAL_ERROR "fp profile check: the probe configured despite its forbidden settings")
endif()
set(missed "")
foreach(route "(via via_juce_lto): '-GL'|(via via_juce_lto): '-flto'"
              "via_ipo INTERPROCEDURAL_OPTIMIZATION"
              "via_single_precision COMPILE_OPTIONS: '-fsingle-precision-constant'"
              "via_source_options.cpp COMPILE_OPTIONS: '-ffp-contract=fast'"
              "src/PostChain.cpp COMPILE_FLAGS: '/fp:contract'")
  set(found FALSE)
  string(REPLACE "|" ";" alternatives "${route}")
  foreach(alt IN LISTS alternatives)
    string(FIND "${out}" "${alt}" pos)
    if(NOT pos EQUAL -1)
      set(found TRUE)
    endif()
  endforeach()
  if(NOT found)
    list(APPEND missed "${route}")
  endif()
endforeach()
if(missed)
  list(JOIN missed "\n  " missed)
  message(FATAL_ERROR "fp profile check: the check missed\n  ${missed}\nOutput:\n${out}")
endif()
message(STATUS "fp profile check: every forbidden route was rejected")
