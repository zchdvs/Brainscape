# Golden-hash harness (docs/design/determinism-profile.md §6.1). One source for every
# leg: the host build registers it with ctest in check mode (report mode in a
# negative-control build); the M7 oracle build (BRAINSCAPE_BUILD_M7_ORACLE) links the same
# brainscape_dsp archive the firmware links into an ELF that qemu-arm user mode runs.
# brainscape_golden_flush is the harness again on the engine built with the guard's test
# hooks, for the forced-flush control (profile §6.4), on the host and the M7 alike.
if(BRAINSCAPE_BUILD_M7_ORACLE)
  enable_language(C)
endif()

function(_brainscape_golden_harness name engine main)
  add_executable(${name} ${main} Corpus.cpp EventScript.cpp Render.cpp ParityStream.cpp)
  target_link_libraries(${name} PRIVATE ${engine})
  # The engine's private FP-environment header, for the hostile-environment renders.
  target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../src)
  target_compile_definitions(${name} PRIVATE
    BRAINSCAPE_GOLDEN_FILE="${CMAKE_CURRENT_SOURCE_DIR}/golden.json")
  if(BRAINSCAPE_BUILD_M7_ORACLE)
    target_sources(${name} PRIVATE m7/syscalls.c)
    target_compile_options(${name} PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions;-fno-rtti;-Wno-psabi>")
    # qemu-user cannot map a segment at address 0, where the bare-metal default puts .text.
    target_link_options(${name} PRIVATE
      -nostartfiles -static -Wl,-Ttext-segment=0x400000 -Wl,--gc-sections)
    set_target_properties(${name} PROPERTIES SUFFIX .elf)
  endif()
endfunction()

_brainscape_golden_harness(brainscape_golden brainscape_dsp golden_main.cpp)
_brainscape_golden_harness(brainscape_golden_flush brainscape_dsp_fpenv_hooks golden_main.cpp)
# The firmware parity image's stream (ParityStream.h) from the same harness, printed on the
# host and under qemu-arm for tools/hil/parity_check.py (firmware/README.md).
_brainscape_golden_harness(brainscape_parity_stream brainscape_dsp parity_stream_main.cpp)

if(NOT BRAINSCAPE_BUILD_M7_ORACLE)
  # Check mode (profile §6.1): every hash, per-second hash and counter must equal
  # golden.json for this build's kSoundRevision, and a render failure or missing coverage
  # fails too. A negative-control build cannot check (the harness refuses), so it
  # reports. Debug builds skip the ablation renders (Release proves coverage) but keep
  # every vector, so assertions run over the whole corpus.
  if(BRAINSCAPE_FP_NEGATIVE_CONTROL)
    set(golden_mode report)
  else()
    set(golden_mode check)
  endif()
  add_test(NAME golden_check
    COMMAND brainscape_golden --mode ${golden_mode} --tag ctest-$<CONFIG>
            --report ${CMAKE_CURRENT_BINARY_DIR}/golden-report-$<CONFIG>.json
            $<IF:$<CONFIG:Debug>,--no-ablate,--ablate>)

  # Check mode against edited copies of golden.json: each edit must fail it.
  if(NOT BRAINSCAPE_FP_NEGATIVE_CONTROL)
    add_test(NAME golden_check_edits
      COMMAND ${CMAKE_COMMAND} -DHARNESS=$<TARGET_FILE:brainscape_golden>
              -DGOLDEN=${CMAKE_CURRENT_SOURCE_DIR}/golden.json
              -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/edits-$<CONFIG>
              -P ${CMAKE_CURRENT_SOURCE_DIR}/GoldenEdits.cmake)
  endif()

  # The forced-flush control (profile §6.4): the harness again, on the engine built with
  # the guard's test hooks, checks the golden file and renders every preset with flushing
  # forced on. Every vector must reproduce but the subnormal-input one, which must change.
  # Debug skips the long vectors.
  add_test(NAME golden_forced_flush
    COMMAND brainscape_golden_flush --mode ${golden_mode} --force-flush-control --no-ablate
            --tag ctest-flush-$<CONFIG>
            --report ${CMAKE_CURRENT_BINARY_DIR}/golden-flush-$<CONFIG>.json
            $<IF:$<CONFIG:Debug>,--quick,--no-ablate>)

  # The parity stream through tools/hil/parity_check.py (firmware/README.md): the short
  # vectors in the harness's configuration and in the pedal's (maxBlockSize 48), which must
  # give the same bits.
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND AND NOT BRAINSCAPE_FP_NEGATIVE_CONTROL)
    foreach(maxblock 512 48)
      add_test(NAME golden_parity_stream_mb${maxblock}
        COMMAND ${CMAKE_COMMAND} -DSTREAM=$<TARGET_FILE:brainscape_parity_stream>
                -DMAX_BLOCK=${maxblock} -DTAG=ctest-$<CONFIG>
                -DPYTHON=${Python3_EXECUTABLE}
                -DCHECK=${CMAKE_CURRENT_SOURCE_DIR}/../../../tools/hil/parity_check.py
                -DGOLDEN=${CMAKE_CURRENT_SOURCE_DIR}/golden.json
                -DOUT=${CMAKE_CURRENT_BINARY_DIR}/parity-stream-mb${maxblock}-$<CONFIG>.jsonl
                -P ${CMAKE_CURRENT_SOURCE_DIR}/ParityStreamCheck.cmake)
    endforeach()
  endif()
endif()
