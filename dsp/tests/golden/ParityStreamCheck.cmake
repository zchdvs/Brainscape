# ctest's golden_parity_stream_*: runs brainscape_parity_stream (the firmware parity image's
# stream, on the host) and checks its output with tools/hil/parity_check.py, the tool the
# owner runs against the Daisy Seed (firmware/README.md).
#   cmake -DSTREAM=... -DMAX_BLOCK=... -DTAG=... -DPYTHON=... -DCHECK=... -DGOLDEN=... -DOUT=...
#         -P this
foreach(v STREAM MAX_BLOCK TAG PYTHON CHECK GOLDEN OUT)
  if(NOT DEFINED ${v})
    message(FATAL_ERROR "ParityStreamCheck.cmake: ${v} is not set")
  endif()
endforeach()
set(args --quick --max-block ${MAX_BLOCK} --tag ${TAG})
execute_process(COMMAND ${STREAM} ${args} --out ${OUT} RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "brainscape_parity_stream ${args} exited with ${rc}")
endif()
execute_process(COMMAND ${PYTHON} ${CHECK} --log ${OUT} --golden ${GOLDEN} RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "parity_check.py --log ${OUT} exited with ${rc}")
endif()
