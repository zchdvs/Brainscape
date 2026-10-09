# golden_check_edits (dsp/tests/golden/CMakeLists.txt): the harness's check mode against
# copies of golden.json with one field edited, rendering one short preset. Each edit must
# exit 2 and name the difference; the copy rewritten without an edit must pass, so every
# failure comes from its edit.
#   cmake -DHARNESS=brainscape_golden -DGOLDEN=golden.json -DWORK_DIR=dir -P GoldenEdits.cmake
cmake_minimum_required(VERSION 3.22)

set(vector silence_4s)
set(preset default_silence)

file(READ "${GOLDEN}" golden)
function(_index out json name)
  string(JSON n LENGTH "${json}" ${ARGN})
  math(EXPR last "${n} - 1")
  foreach(i RANGE ${last})
    string(JSON got GET "${json}" ${ARGN} ${i} name)
    if(got STREQUAL name)
      set(${out} ${i} PARENT_SCOPE)
      return()
    endif()
  endforeach()
  message(FATAL_ERROR "${name} is not in ${GOLDEN}")
endfunction()
_index(v "${golden}" ${vector} vectors)
_index(p "${golden}" ${preset} vectors ${v} presets)
set(P vectors ${v} presets ${p})

file(MAKE_DIRECTORY "${WORK_DIR}")
set(failed "")
function(expect label want_rc want_text json)
  set(path "${WORK_DIR}/${label}.json")
  file(WRITE "${path}" "${json}")
  execute_process(
    COMMAND "${HARNESS}" --mode check --golden "${path}" --only ${vector}/${preset}
            --no-ablate --tag edits
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  string(FIND "${out}" "${want_text}" at)
  if(rc STREQUAL want_rc AND NOT at EQUAL -1)
    message(STATUS "${label}: exit ${rc}, \"${want_text}\"")
  else()
    message(STATUS "${label}: exit ${rc}, expected ${want_rc} with \"${want_text}\"\n${out}${err}")
    set(failed "${failed} ${label}" PARENT_SCOPE)
  endif()
endfunction()

string(JSON j SET "${golden}" generatorVersion 1)
expect(unedited 0 "# golden: every rendered preset matches" "${j}")

string(JSON j SET "${golden}" ${P} secondHashes 1
       "\"0000000000000000000000000000000000000000000000000000000000000000\"")
expect(second_hash 2 "${vector}/${preset}: per-second hashes differ from second 1" "${j}")

string(JSON seconds LENGTH "${golden}" ${P} secondHashes)
math(EXPR last "${seconds} - 1")
string(JSON j REMOVE "${golden}" ${P} secondHashes ${last})
expect(second_hashes_cut 2 "${vector}/${preset}: per-second hashes differ from second ${last}" "${j}")

string(JSON j SET "${golden}" vectors ${v} ringSizes "[2097152, 4194304]")
expect(ring_sizes 2 "${vector}: ringSizes differ" "${j}")

string(JSON j SET "${golden}" generatorVersion 99)
expect(generator_version 2 "generatorVersion differs" "${j}")

string(JSON frames GET "${golden}" ${P} counters frames)
math(EXPR frames "${frames} + 1")
string(JSON j SET "${golden}" ${P} counters frames ${frames})
expect(counter 2 "${vector}/${preset}: counter frames = " "${j}")

# A counter name the harness never counts (births became a real counter at sound revision 4).
string(JSON j SET "${golden}" ${P} counters notACounter 0)
expect(extra_counter 2 "${vector}/${preset}: counter notACounter is in golden but not counted" "${j}")

if(failed)
  message(FATAL_ERROR "check mode missed or misreported:${failed}")
endif()
