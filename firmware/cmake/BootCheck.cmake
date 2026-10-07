# Checks one firmware image (firmware/README.md, "Memory map"): nothing that runs before the
# preinit hook has filled ITCM may call into ITCM. That is the startup code (libDaisy's
# Reset_Handler, newlib's __libc_init_array), the preinit hook and what it calls, and the
# SysTick path, which the Daisy bootloader leaves running from the image's first
# instruction. ITCM holds the engine's code, libgcc's helpers and the firmware's memcpy,
# memmove and memset (platform/MemFunctions.c) in an ITCM image; a direct call into it, or a
# veneer (how a QSPI caller reaches ITCM), fails the build.
#   cmake -DELF=... -DOBJDUMP=arm-none-eabi-objdump -P BootCheck.cmake
cmake_minimum_required(VERSION 3.22)
set(functions
  Reset_Handler
  __libc_init_array
  BrainscapePreinit
  _ZN10brainscape2fw13SetBootFpWordEv
  _ZN10brainscape2fw19PrepareFaultVectorsEv
  _ZN10brainscape2fw15UseFaultVectorsEb
  _ZN10brainscape2fw16ImageVectorTableEv
  SysTick_Handler
  HAL_IncTick
  HAL_SYSTICK_IRQHandler
  HAL_SYSTICK_Callback)
set(problems "")
set(checked 0)
foreach(fn IN LISTS functions)
  execute_process(COMMAND "${OBJDUMP}" -d --no-show-raw-insn "--disassemble=${fn}" "${ELF}"
                  OUTPUT_VARIABLE dis RESULT_VARIABLE rc ERROR_QUIET)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "BootCheck: ${OBJDUMP} failed on ${ELF}")
  endif()
  if(NOT dis MATCHES "<${fn}>:")
    continue()  # not linked into this image (inlined, or unused)
  endif()
  math(EXPR checked "${checked} + 1")
  string(REPLACE "\n" ";" lines "${dis}")
  foreach(line IN LISTS lines)
    if(line MATCHES "[ \t](bl|blx|b|b\\.w|b\\.n)[ \t]+([0-9a-f]+) <([^>]+)>")
      set(target "${CMAKE_MATCH_2}")
      set(name "${CMAKE_MATCH_3}")
      string(LENGTH "${target}" len)
      # Addresses below 0x00010000 are ITCM.
      if(name MATCHES "_veneer$" OR (len LESS_EQUAL 4) OR target MATCHES "^0000[0-9a-f][0-9a-f][0-9a-f][0-9a-f]$")
        string(APPEND problems "  ${fn} calls ${name} at 0x${target}\n")
      endif()
    endif()
  endforeach()
endforeach()
if(checked LESS 3)
  message(FATAL_ERROR "BootCheck: found only ${checked} of the boot-path functions in ${ELF}")
endif()
if(NOT problems STREQUAL "")
  message(FATAL_ERROR "BootCheck: ${ELF} calls into ITCM before the preinit hook fills it:\n${problems}")
endif()
get_filename_component(name "${ELF}" NAME)
message(STATUS "BootCheck: ${name}: ${checked} boot-path functions call nothing in ITCM")
