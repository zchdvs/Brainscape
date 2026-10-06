# Self-test of the configure-time forbidden-flag check (docs/design/determinism-profile.md
# §3.2, §3.4, §6.3): the probe project (fpcheck/) must configure without its violations
# and fail with them, naming every route; the test-only negative-control escape must admit
# contraction only, refuse plugin and firmware builds, and not outlive its configure. Run
# by CTest in script mode with the parent build's generator and compiler.
cmake_minimum_required(VERSION 3.21)

# Never built, so the compiler check is skipped: its try-compile adds nothing and, under
# a deep build directory, overflows MSBuild's path limit. For the same reason every probe
# directory has a one- or two-letter name: MSBuild's compiler identification nests deep
# under each, and every character here comes off the build directory's path budget.
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

set(rejected "forbidden floating-point or LTO settings")
set(warning "BRAINSCAPE_FP_NEGATIVE_CONTROL=ON: floating-point contraction ON")
set(refused "BRAINSCAPE_FP_NEGATIVE_CONTROL is test-only and cannot build the plugin or the firmware")
# Every route except contraction; alternatives separated by |.
set(routes
  "(via via_juce_lto): '-GL'|(via via_juce_lto): '-flto'"
  "via_ipo INTERPROCEDURAL_OPTIMIZATION"
  "via_single_precision COMPILE_OPTIONS: '-fsingle-precision-constant'"
  "via_denormal_math COMPILE_OPTIONS: '-fdenormal-fp-math=preserve-sign'"
  "via_daz_ftz COMPILE_OPTIONS: '-mdaz-ftz'"
  "via_launcher CXX_COMPILER_LAUNCHER: '-fapprox-func'"
  "CMAKE_CXX_COMPILER_ARG1 (")
set(contraction_routes
  "via_source_options.cpp COMPILE_OPTIONS: '-ffp-contract=fast'"
  "src/PostChain.cpp COMPILE_FLAGS: '/fp:contract'")

# Configures the probe in ${WORK_DIR}/<dir> with extra arguments; sets rc, out, and flat:
# out with every run of whitespace made one space, since CMake wraps long messages.
macro(run_probe dir)
  execute_process(COMMAND "${CMAKE_COMMAND}" ${args} -B "${WORK_DIR}/${dir}" ${ARGN}
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
  string(REGEX REPLACE "[ \t\r\n]+" " " flat "${out}")
endmacro()

function(expect_configured what)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "fp profile check: ${what} failed to configure:\n${out}")
  endif()
endfunction()

# A configure that must fail has to fail for the expected reason, not, say, because a
# compiler could not be identified under a long path.
function(expect_failure what marker)
  if(rc EQUAL 0)
    message(FATAL_ERROR "fp profile check: ${what} configured")
  endif()
  string(FIND "${flat}" "${marker}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR
            "fp profile check: ${what}: the probe configure failed for another reason:\n${out}")
  endif()
endfunction()

function(expect_routes what)
  set(missed "")
  foreach(route IN LISTS ARGN)
    set(found FALSE)
    string(REPLACE "|" ";" alternatives "${route}")
    foreach(alt IN LISTS alternatives)
      string(FIND "${flat}" "${alt}" pos)
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
    message(FATAL_ERROR "fp profile check: ${what}: the check missed\n  ${missed}\nOutput:\n${out}")
  endif()
endfunction()

run_probe(c)
expect_configured("the clean probe")

run_probe(v -DBRAINSCAPE_FPCHECK_VIOLATIONS=ON)
expect_failure("the probe with forbidden settings" "${rejected}")
expect_routes("forbidden settings" ${routes} ${contraction_routes})

# The negative-control escape admits contraction, says so loudly, and is never cached.
run_probe(n -DBRAINSCAPE_FP_NEGATIVE_CONTROL=ON -DBRAINSCAPE_FPCHECK_CONTRACTION=ON)
expect_configured("the negative control with contraction")
string(FIND "${flat}" "${warning}" pos)
if(pos EQUAL -1)
  message(FATAL_ERROR "fp profile check: the negative control configured without its warning")
endif()
file(READ "${WORK_DIR}/n/CMakeCache.txt" cache)
string(FIND "${cache}" "BRAINSCAPE_FP_NEGATIVE_CONTROL" pos)
if(NOT pos EQUAL -1)
  message(FATAL_ERROR "fp profile check: the negative control was cached")
endif()
# The same directory reconfigured without the -D conforms again, and rejects contraction.
run_probe(n -DBRAINSCAPE_FPCHECK_CONTRACTION=ON)
expect_failure("a negative-control directory reconfigured without the escape" "${rejected}")
expect_routes("a negative-control directory reconfigured without the escape"
              "via_contraction COMPILE_OPTIONS: '-ffp-contract=fast'")
string(FIND "${flat}" "${warning}" pos)
if(NOT pos EQUAL -1)
  message(FATAL_ERROR "fp profile check: the negative control outlived its configure")
endif()

# It still rejects every other route.
run_probe(nv -DBRAINSCAPE_FP_NEGATIVE_CONTROL=ON -DBRAINSCAPE_FPCHECK_VIOLATIONS=ON)
expect_failure("the negative control with non-contraction settings" "${rejected}")
expect_routes("under the negative control" ${routes})
foreach(admitted "via_source_options.cpp COMPILE_OPTIONS" "src/PostChain.cpp COMPILE_FLAGS")
  string(FIND "${flat}" "${admitted}" pos)
  if(NOT pos EQUAL -1)
    message(FATAL_ERROR "fp profile check: under the negative control the check rejected "
                        "contraction (${admitted}):\n${out}")
  endif()
endforeach()

# And it never builds anything shipped.
foreach(shipped PLUGIN FIRMWARE)
  run_probe(r -DBRAINSCAPE_FP_NEGATIVE_CONTROL=ON -DBRAINSCAPE_BUILD_${shipped}=ON)
  expect_failure("the negative control with BRAINSCAPE_BUILD_${shipped}" "${refused}")
  file(REMOVE_RECURSE "${WORK_DIR}/r")
endforeach()

message(STATUS "fp profile check: every forbidden route was rejected; the negative control "
               "admits contraction only, is never cached and refuses plugin and firmware builds")
