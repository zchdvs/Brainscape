# Checks one ITCM image (firmware/README.md, "Memory map"): the engine's shared code and
# constants are in ITCM. A C++17 inline variable (kParamTable and the other tables of
# Params.h) or a template instantiation is a COMDAT section that every object using it
# carries, and the linker keeps the first copy it reads: in these images often a golden-harness
# object's (Corpus.cpp, Render.cpp), not the engine archive's, which the linker script's
# per-archive patterns would leave in QSPI beside the ITCM code that reads it. Every COMDAT
# code or constant section that an ITCM member of the engine archives defines must therefore
# keep its copy in ITCM (below 0x00010000), whichever object supplied it; a new one fails the
# build until it is named in brainscape_firmware_image's _bs_engine_comdat (CMakeLists.txt).
#
# It also checks the calls out of ITCM. A call from code in ITCM to code outside it (QSPI) goes
# through a linker veneer placed in ITCM beside the caller, so the image's ITCM veneers are exactly
# its calls out of ITCM. The audio path makes none per sample: the only ones allowed are the
# tempo core's control-rate entry points (docs/design/clock.md §9.6, §11.12), named in
# ITCM_VENEERS. Any other (a hot path calling a cold engine function, or an object kept out of
# ITCM) fails the build until it is moved, inlined, or named there deliberately.
#   cmake -DELF=... -DREADELF=arm-none-eabi-readelf -DARCHIVE=libbrainscape_dsp.a
#         -DHOOKS_ARCHIVE=libbrainscape_dsp_fpenv_hooks.a -DNOT_ITCM=Decode.cpp.obj,...
#         -DITCM_VENEERS=<mangled target>,... -P ItcmCheck.cmake
cmake_minimum_required(VERSION 3.22)
string(REPLACE "," ";" not_itcm "${NOT_ITCM}")
set(symbols "")
foreach(archive IN ITEMS "${ARCHIVE}" "${HOOKS_ARCHIVE}")
  execute_process(COMMAND "${READELF}" -SW "${archive}"
                  OUTPUT_VARIABLE out RESULT_VARIABLE rc ERROR_QUIET)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "ItcmCheck: ${READELF} failed on ${archive}")
  endif()
  # Brackets would stop CMake's list splitting at ';' (readelf prints "[Nr]" on every line).
  string(REPLACE "[" "(" out "${out}")
  string(REPLACE "]" ")" out "${out}")
  string(REPLACE ";" "," out "${out}")
  string(REPLACE "\n" ";" lines "${out}")
  set(member "")
  foreach(line IN LISTS lines)
    if(line MATCHES "^File: .*\\(([^()]+)\\)$")
      set(member "${CMAKE_MATCH_1}")
    elseif(line MATCHES "^ *\\( *[0-9]+\\) \\.(rodata|text)\\.([A-Za-z0-9_$.]+) +PROGBITS +[0-9a-f]+ +[0-9a-f]+ +[0-9a-f]+ +[0-9a-f]+ +[A-Z]*G[A-Z]* ")
      # GCC names a COMDAT section after its symbol: .rodata.<symbol>, .text.<symbol>.
      if(NOT member IN_LIST not_itcm)
        list(APPEND symbols "${CMAKE_MATCH_2}")
      endif()
    endif()
  endforeach()
endforeach()
list(REMOVE_DUPLICATES symbols)
list(LENGTH symbols defined)
if(defined EQUAL 0)
  message(FATAL_ERROR "ItcmCheck: no COMDAT section found in ${ARCHIVE}: the readelf parse is broken")
endif()

execute_process(COMMAND "${READELF}" -sW "${ELF}" OUTPUT_VARIABLE syms RESULT_VARIABLE rc ERROR_QUIET)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "ItcmCheck: ${READELF} failed on ${ELF}")
endif()
set(problems "")
set(linked 0)
foreach(sym IN LISTS symbols)
  string(REPLACE "." "\\." sym_re "${sym}")
  string(REPLACE "$" "\\$" sym_re "${sym_re}")
  # Num: Value Size Type Bind Vis Ndx Name, defined symbols only (a numeric Ndx).
  string(REGEX MATCHALL "[0-9]+: [0-9a-f]+ +[0-9a-fx]+ +[A-Z_]+ +[A-Z_]+ +[A-Z_]+ +[0-9]+ +${sym_re}\n"
         hits "${syms}")
  if(hits STREQUAL "")
    continue()  # not linked into this image (--gc-sections)
  endif()
  math(EXPR linked "${linked} + 1")
  foreach(hit IN LISTS hits)
    string(REGEX MATCH "^[0-9]+: ([0-9a-f]+)" _ "${hit}")
    set(addr "${CMAKE_MATCH_1}")
    math(EXPR value "0x${addr}")
    if(value GREATER_EQUAL 65536)  # ITCM is 0x00000000-0x0000FFFF
      string(APPEND problems "  ${sym} at 0x${addr}\n")
    endif()
  endforeach()
endforeach()
get_filename_component(name "${ELF}" NAME)
if(NOT problems STREQUAL "")
  message(FATAL_ERROR "ItcmCheck: ${name} keeps engine COMDAT sections outside ITCM (add them to "
                      "_bs_engine_comdat in firmware/CMakeLists.txt):\n${problems}")
endif()

# GNU ld names the stub of a long branch to `target` __<target>_veneer.
string(REPLACE "," ";" allowed "${ITCM_VENEERS}")
string(REGEX MATCHALL "[0-9]+: [0-9a-f]+ +[0-9]+ +[A-Z_]+ +[A-Z_]+ +[A-Z_]+ +[0-9A-Z_]+ +__[A-Za-z0-9_$.]+_veneer\n"
       veneers "${syms}")
set(calls 0)
set(unexpected "")
foreach(v IN LISTS veneers)
  string(REGEX MATCH "^[0-9]+: ([0-9a-f]+) .* __([A-Za-z0-9_$.]+)_veneer" _ "${v}")
  set(addr "${CMAKE_MATCH_1}")
  set(target "${CMAKE_MATCH_2}")
  math(EXPR value "0x${addr}")
  if(value GREATER_EQUAL 65536)
    continue()  # outside ITCM: a call into ITCM, or between QSPI and flash
  endif()
  math(EXPR calls "${calls} + 1")
  if(NOT target IN_LIST allowed)
    string(APPEND unexpected "  ${target} (veneer at 0x${addr})\n")
  endif()
endforeach()
if(NOT unexpected STREQUAL "")
  message(FATAL_ERROR "ItcmCheck: ${name} calls out of ITCM to targets not in ITCM_VENEERS "
                      "(firmware/CMakeLists.txt): keep the callee in ITCM or inline it, or name it "
                      "there if it runs at control rate only:\n${unexpected}")
endif()
message(STATUS "ItcmCheck: ${name}: ${linked} of the engine's ${defined} COMDAT sections linked, all in ITCM; "
               "${calls} calls out of ITCM, all allowed")
