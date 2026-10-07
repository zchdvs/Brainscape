# Memory-region use of one firmware image (firmware/README.md, "Sizes"): the allocated
# sections of the ELF summed per STM32H750 region, the .bin the web programmer flashes, and
# whether the code fits where the app type puts it. Written to OUT and printed.
#   cmake -DELF=... -DBIN=... -DSIZE=arm-none-eabi-size -DAPP_TYPE=... -DFLASH_ADDRESS=...
#         -DENGINE_CODE=ITCM|XIP -DOUT=... -P ImageReport.cmake
cmake_minimum_required(VERSION 3.22)
execute_process(COMMAND "${SIZE}" -A -d "${ELF}" OUTPUT_VARIABLE table RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "${SIZE} -A failed on ${ELF}")
endif()
# name | first address | size in bytes
set(regions
  "ITCM|0|65536"
  "FLASH|134217728|131072"
  "DTCM|536870912|131072"
  "AXI_SRAM|603979776|524288"
  "D2_SRAM|805306368|294912"
  "D3_SRAM|939524096|65536"
  "BACKUP_SRAM|947912704|4096"
  "QSPI|2415919104|8388608"
  "SDRAM|3221225472|67108864")
foreach(r IN LISTS regions)
  string(REPLACE "|" ";" f "${r}")
  list(GET f 0 n)
  set(used_${n} 0)
endforeach()
string(REPLACE "\n" ";" lines "${table}")
foreach(line IN LISTS lines)
  if(NOT line MATCHES "^([.][^ ]+) +([0-9]+) +([0-9]+)")
    continue()
  endif()
  set(sec "${CMAKE_MATCH_1}")
  set(bytes "${CMAKE_MATCH_2}")
  set(addr "${CMAKE_MATCH_3}")
  if(bytes EQUAL 0 OR sec MATCHES "^[.](ARM[.]attributes|comment|debug|stab)")
    continue()
  endif()
  foreach(r IN LISTS regions)
    string(REPLACE "|" ";" f "${r}")
    list(GET f 0 n)
    list(GET f 1 base)
    list(GET f 2 len)
    math(EXPR top "${base} + ${len}")
    # Addresses above 2^31 overflow CMake's signed 64-bit math nowhere; compare as integers.
    if(addr GREATER_EQUAL base AND addr LESS top)
      math(EXPR used_${n} "${used_${n}} + ${bytes}")
    endif()
  endforeach()
endforeach()
file(SIZE "${BIN}" bin_bytes)
get_filename_component(name "${ELF}" NAME_WE)
set(report "${name} (${APP_TYPE}, engine code ${ENGINE_CODE}): ${name}.bin is ${bin_bytes} bytes, flashed at ${FLASH_ADDRESS}\n")
foreach(r IN LISTS regions)
  string(REPLACE "|" ";" f "${r}")
  list(GET f 0 n)
  list(GET f 2 len)
  if(used_${n} EQUAL 0)
    continue()
  endif()
  math(EXPR pct "(${used_${n}} * 1000 + ${len} / 2) / ${len}")
  math(EXPR whole "${pct} / 10")
  math(EXPR frac "${pct} % 10")
  math(EXPR kib "(${used_${n}} + 512) / 1024")
  string(APPEND report "  ${n}: ${used_${n}} bytes (${kib} KiB, ${whole}.${frac}% of the region)\n")
endforeach()
if(APP_TYPE STREQUAL "BOOT_NONE" AND bin_bytes GREATER 131072)
  string(APPEND report "  ERROR: the image does not fit the 128 KiB internal flash\n")
endif()
file(WRITE "${OUT}" "${report}")
message(STATUS "${report}")
if(report MATCHES "ERROR")
  message(FATAL_ERROR "${name} does not fit its memory")
endif()
