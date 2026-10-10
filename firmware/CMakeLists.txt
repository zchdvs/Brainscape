# Daisy Seed Rev7 bring-up firmware (firmware/README.md; docs/STATUS.md merged step 5;
# determinism-profile.md §8.4 step 13; companion-app.md §8 phase D). Three images on one
# platform layer, all linking the dsp/ archive exactly as CI builds and audits it:
#
#   brainscape_parity       Image A: the golden corpus rendered on silicon, hashes streamed
#                           over USB serial (tools/hil/parity_check.py)
#   brainscape_bench        Image B: the DWT measurement pass (tools/hil/bench_report.py)
#   brainscape_bench_hooks  Image B on the engine built with the FP guard's test hooks
#                           (flushing forced on, exception flags collected): the FZ = 0/1
#                           A/B of profile §4.2
#   brainscape_live         Image C: the engine in the audio callback (tools/hil/console.py)
#
# Configure with the pinned toolchain and the tests off:
#   cmake -B build/fw -G "Unix Makefiles" -DCMAKE_TOOLCHAIN_FILE=tools/cmake/arm-none-eabi-toolchain.cmake
#         -DCMAKE_BUILD_TYPE=Release -DBRAINSCAPE_BUILD_TESTS=OFF -DBRAINSCAPE_BUILD_FIRMWARE=ON
#         -DBRAINSCAPE_WERROR=ON
if(BRAINSCAPE_BUILD_TESTS)
  message(FATAL_ERROR "BRAINSCAPE_BUILD_FIRMWARE needs BRAINSCAPE_BUILD_TESTS=OFF (the tests are host-only)")
endif()
if(CMAKE_VERSION VERSION_LESS 3.24)
  # libDaisy v9.0.0's CMakeLists.txt sets policy CMP0135, which CMake 3.24 introduced.
  message(FATAL_ERROR "BRAINSCAPE_BUILD_FIRMWARE needs CMake 3.24 or later (libDaisy v9.0.0), "
                      "found ${CMAKE_VERSION}")
endif()
if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR NOT CMAKE_SYSTEM_PROCESSOR STREQUAL "arm")
  message(FATAL_ERROR "BRAINSCAPE_BUILD_FIRMWARE needs the arm-none-eabi toolchain file "
                      "(-DCMAKE_TOOLCHAIN_FILE=tools/cmake/arm-none-eabi-toolchain.cmake)")
endif()
# The engine archive the images link must be the one CI audits and renders under qemu-arm
# (parity.yml, parity-m7; firmware.yml compares the two archives byte for byte): a Release
# build with the pinned compiler, or the images test something else.
if(NOT CMAKE_BUILD_TYPE STREQUAL "Release")
  message(FATAL_ERROR "brainscape firmware: CMAKE_BUILD_TYPE is '${CMAKE_BUILD_TYPE}', not Release: "
                      "the engine archive would differ from the audited one")
endif()
if(NOT CMAKE_CXX_COMPILER_VERSION VERSION_EQUAL 10.3.1)
  message(FATAL_ERROR "brainscape firmware: arm-none-eabi-g++ ${CMAKE_CXX_COMPILER_VERSION}, not the "
                      "pinned 10.3-2021.10 (determinism-profile.md §6.8)")
endif()

# Where the images run (firmware/README.md, "App type"). BOOT_QSPI, the default: execute in
# place from the 8 MiB QSPI flash, flashed through the Daisy bootloader (grain-engine.md §7
# plans the pedal this way); the images are 150-210 KiB, past the 128 KiB internal flash.
# BOOT_NONE: the internal flash, no bootloader, for images that fit it. One setting for
# every image, since libDaisy's startup code depends on it.
set(BRAINSCAPE_FIRMWARE_APP_TYPE "BOOT_QSPI" CACHE STRING
    "Daisy app type of the firmware images: BOOT_NONE (internal flash) or BOOT_QSPI (Daisy bootloader)")
set_property(CACHE BRAINSCAPE_FIRMWARE_APP_TYPE PROPERTY STRINGS BOOT_NONE BOOT_QSPI)
if(BRAINSCAPE_FIRMWARE_APP_TYPE STREQUAL "BOOT_NONE")
  set(BRAINSCAPE_LD_TEXT FLASH)
  set(BRAINSCAPE_LD_DATA SRAM)
  set(BRAINSCAPE_LD_AXI_LENGTH 512K)
  set(BRAINSCAPE_LD_QSPI_ORIGIN 0x90000000)
  set(BRAINSCAPE_LD_QSPI_LENGTH 8M)
  set(BRAINSCAPE_FLASH_ADDRESS 0x08000000)
elseif(BRAINSCAPE_FIRMWARE_APP_TYPE STREQUAL "BOOT_QSPI")
  set(BRAINSCAPE_LD_TEXT QSPIFLASH)
  set(BRAINSCAPE_LD_DATA SRAM)
  set(BRAINSCAPE_LD_AXI_LENGTH 512K)
  set(BRAINSCAPE_LD_QSPI_ORIGIN 0x90040000)  # the Daisy bootloader keeps the first 256 KiB
  set(BRAINSCAPE_LD_QSPI_LENGTH 7936K)
  set(BRAINSCAPE_FLASH_ADDRESS 0x90040000)
else()
  message(FATAL_ERROR "BRAINSCAPE_FIRMWARE_APP_TYPE must be BOOT_NONE or BOOT_QSPI, not "
                      "'${BRAINSCAPE_FIRMWARE_APP_TYPE}' (BOOT_SRAM would put the code in the AXI "
                      "SRAM the Warm arena needs)")
endif()

# The heap is the SDRAM after the Bulk arena (about 46 MiB): only the golden harness's
# containers live there (the corpus, its scripts, the test-signal generator), never the
# engine's state or anything the audio callback touches. The parity stream peaks near
# 250 KiB of it (measured under qemu-arm), more than AXI SRAM has to spare. Nothing may
# allocate before BoardInit has brought the SDRAM up; the firmware's _sbrk refuses to.
set(BRAINSCAPE_LD_HEAP SDRAM)

include(cmake/LibDaisy.cmake)

# libDaisy's headers as system headers for Brainscape's sources, so -Wall -Wextra -Werror
# judge Brainscape's code only (GCC: -isystem overrides -I for the same directory).
set(_bs_daisy_includes "")
foreach(t daisy CMSIS_CORE CMSIS_DEVICE_H7 STM32H7XX_HAL_DRIVER STM32_USB_DEVICE_LIBRARY
          STM32_USB_HOST_LIBRARY FatFs)
  get_target_property(dirs ${t} INTERFACE_INCLUDE_DIRECTORIES)
  if(dirs)
    list(APPEND _bs_daisy_includes ${dirs})
  endif()
endforeach()
target_include_directories(brainscape_daisy SYSTEM INTERFACE ${_bs_daisy_includes})

# Build identity: the commit (refreshed at every build), and the SHA-256 of the engine
# archive the images link, which must equal the archive the parity-m7 CI job audits and runs
# under qemu-arm.
find_package(Git QUIET)
set(_bs_generated ${CMAKE_CURRENT_BINARY_DIR}/generated)
add_custom_target(brainscape_fw_gitinfo
  COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -DOUT=${_bs_generated}/GitInfo.h
          -DGIT=${GIT_EXECUTABLE} -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/GitInfo.cmake
  BYPRODUCTS ${_bs_generated}/GitInfo.h
  COMMENT "Recording the source commit"
  VERBATIM)
configure_file(platform/BuildInfo.cpp.in ${_bs_generated}/BuildInfo.cpp @ONLY)
# engine-archives.sha256, beside the images, is what tools/hil/parity_check.py and
# bench_report.py --expect-archive compare the device's hello line with.
add_custom_command(
  OUTPUT ${_bs_generated}/EngineArchiveSha256.h ${CMAKE_CURRENT_BINARY_DIR}/engine-archives.sha256
  COMMAND ${CMAKE_COMMAND} -DARCHIVE=$<TARGET_FILE:brainscape_dsp>
          -DHOOKS_ARCHIVE=$<TARGET_FILE:brainscape_dsp_fpenv_hooks>
          -DOUT=${_bs_generated}/EngineArchiveSha256.h
          -DOUT_LIST=${CMAKE_CURRENT_BINARY_DIR}/engine-archives.sha256
          -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/ArchiveSha256.cmake
  DEPENDS brainscape_dsp brainscape_dsp_fpenv_hooks $<TARGET_FILE:brainscape_dsp>
          $<TARGET_FILE:brainscape_dsp_fpenv_hooks> ${CMAKE_CURRENT_SOURCE_DIR}/cmake/ArchiveSha256.cmake
  COMMENT "Hashing the engine archives the images link"
  VERBATIM)

# Flags for Brainscape's own firmware sources (libDaisy keeps its own).
function(_brainscape_fw_flags target)
  target_compile_options(${target} PRIVATE -Wall -Wextra
    $<$<COMPILE_LANGUAGE:CXX>:-Wno-psabi> $<$<BOOL:${BRAINSCAPE_WERROR}>:-Werror>)
  target_compile_features(${target} PRIVATE cxx_std_17)
endfunction()

# The golden harness, shared with the host and qemu builds (dsp/tests/golden): the parity
# image renders the corpus with it, the live image takes its presets from it, the bench
# reuses its renderer, SHA-256 and test signal plumbing. Since sound revision 2 the corpus's
# structure comes from its committed packages (dsp/tests/golden/presets, mode-compiler.md
# §10.3), which the images carry compiled in (EmbeddedPackages.h; there is no file system):
# linking brainscape_fw_golden_packages makes the harness's LoadPackage read them.
set(_bs_golden ${CMAKE_SOURCE_DIR}/dsp/tests/golden)
include(${_bs_golden}/GoldenPackages.cmake)
brainscape_golden_packages(brainscape_fw_golden_packages)
_brainscape_fw_flags(brainscape_fw_golden_packages)
function(_brainscape_fw_golden name engine)
  add_library(${name} STATIC
    ${_bs_golden}/Corpus.cpp ${_bs_golden}/EventScript.cpp ${_bs_golden}/Render.cpp
    ${_bs_golden}/ParityStream.cpp)
  target_include_directories(${name} PUBLIC ${_bs_golden} ${CMAKE_SOURCE_DIR}/dsp/tests
                                     ${CMAKE_SOURCE_DIR}/dsp/src)
  target_link_libraries(${name} PUBLIC ${engine} brainscape_fw_golden_packages)
  _brainscape_fw_flags(${name})
endfunction()
_brainscape_fw_golden(brainscape_fw_golden brainscape_dsp)
_brainscape_fw_golden(brainscape_fw_golden_hooks brainscape_dsp_fpenv_hooks)

# The platform layer: board bring-up, the FP boot word, DWT, fault handling, USB serial,
# arena placement.
add_library(brainscape_fw_platform STATIC
  platform/Seed.cpp
  platform/Fault.cpp
  platform/UsbSerial.cpp
  platform/Placement.cpp
  platform/Syscalls.c
  ${_bs_generated}/BuildInfo.cpp
  ${_bs_generated}/EngineArchiveSha256.h)
# The preinit hook runs before the copy to ITCM, where an ITCM image keeps memcpy and memset
# (below): its loops must stay loops, not become calls to them.
set_source_files_properties(platform/Seed.cpp platform/Fault.cpp PROPERTIES
  COMPILE_OPTIONS -fno-tree-loop-distribute-patterns)
add_dependencies(brainscape_fw_platform brainscape_fw_gitinfo)
target_include_directories(brainscape_fw_platform
  PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR} ${_bs_golden} ${CMAKE_SOURCE_DIR}/dsp/include
  PRIVATE ${_bs_generated})
target_link_libraries(brainscape_fw_platform PUBLIC brainscape_daisy brainscape::fp_profile)
_brainscape_fw_flags(brainscape_fw_platform)

# memcpy, memmove and memset moving 32 bytes per iteration instead of newlib-nano's byte
# loops (platform/MemFunctions.c). An object library, so every image links the objects
# themselves and libc.a's members are never pulled in; -ffreestanding and
# -fno-tree-loop-distribute-patterns keep the compiler from turning the loops into calls to
# the functions they implement.
add_library(brainscape_fw_mem OBJECT platform/MemFunctions.c)
target_compile_options(brainscape_fw_mem PRIVATE -ffreestanding -fno-tree-loop-distribute-patterns)
_brainscape_fw_flags(brainscape_fw_mem)

get_filename_component(_bs_tool_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
find_program(BRAINSCAPE_ARM_SIZE arm-none-eabi-size HINTS ${_bs_tool_dir} REQUIRED)
find_program(BRAINSCAPE_ARM_OBJCOPY arm-none-eabi-objcopy HINTS ${_bs_tool_dir} REQUIRED)
find_program(BRAINSCAPE_ARM_OBJDUMP arm-none-eabi-objdump HINTS ${_bs_tool_dir} REQUIRED)
find_program(BRAINSCAPE_ARM_READELF arm-none-eabi-readelf HINTS ${_bs_tool_dir} REQUIRED)

# One firmware image: an ELF, the .bin the web programmer flashes, a .hex, a map file, and
# a memory-region report. ENGINE_CODE ITCM places the engine archive's code and constants in
# ITCM (profile §7.1), XIP leaves them with the rest of the code. MIN_HEAP is what the
# image's heap region must hold (linker check).
function(brainscape_firmware_image name)
  cmake_parse_arguments(ARG "" "MIN_HEAP;ENGINE_CODE" "SOURCES;LIBS" ${ARGN})
  set(BRAINSCAPE_LD_MIN_HEAP ${ARG_MIN_HEAP})
  if(ARG_ENGINE_CODE STREQUAL "ITCM")
    set(BRAINSCAPE_LD_ENGINE_CODE "ITCM")
    # With the run-time helpers it calls (64-bit division and conversions) and the
    # firmware's memcpy, memmove and memset (platform/MemFunctions.c; libDaisy's startup code
    # is built not to call them before the copy to ITCM, cmake/LibDaisy.cmake). Not the
    # archive's members that never run in the audio path, which execute in place with the
    # rest of the code: the package decoder, encoder and their SHA-256 (a producer decodes a
    # package before it stages the state, mode-compiler.md §5; the golden harness hashes its
    # output with it) and the test-signal generator (the harness's input). With them the
    # archive no longer fits the 64 KiB at sound revision 2. The tempo core's integer code runs
    # at control rate and stays out too (docs/design/clock.md §9.6, D1 as amended 2026-10-09).
    set(_bs_not_itcm_members Decode.cpp.obj Encode.cpp.obj Sha256.cpp.obj TestSignal.cpp.obj
                             IntMath.cpp.obj MidiClock.cpp.obj Tempo.cpp.obj)
    set(_bs_not_itcm "")
    foreach(a libbrainscape_dsp.a libbrainscape_dsp_fpenv_hooks.a)
      foreach(m IN LISTS _bs_not_itcm_members)
        string(APPEND _bs_not_itcm " *${a}:${m}")
      endforeach()
    endforeach()
    # The engine's C++17 inline constexpr tables (Params.h, Mode.h) are COMDAT sections that
    # every object using them carries, and the linker keeps the first copy it reads: often a
    # golden-harness object's (Corpus.cpp, Render.cpp), which the per-archive patterns would
    # leave in QSPI. Named here without a file, the kept copy goes to ITCM whichever object
    # supplied it. cmake/ItcmCheck.cmake fails the build when a COMDAT section of the archive's
    # ITCM members (a new table, a template instantiation) lands anywhere else.
    set(_bs_engine_comdat
      .rodata._ZN10brainscape11kParamTableE
      .rodata._ZN10brainscape11kLeafParamsE
      .rodata._ZN10brainscape13params_detail12kLeafOrdinalE
      .rodata._ZN10brainscape16kDefaultModeHashE)
    list(JOIN _bs_engine_comdat " " _bs_engine_comdat)
    set(BRAINSCAPE_LD_ITCM_INPUTS "EXCLUDE_FILE(${_bs_not_itcm}) *libbrainscape_dsp.a:*(.text .text.* .rodata .rodata.*) EXCLUDE_FILE(${_bs_not_itcm}) *libbrainscape_dsp_fpenv_hooks.a:*(.text .text.* .rodata .rodata.*) *(${_bs_engine_comdat}) *libgcc.a:*(.text .text.*) *MemFunctions.c.o*(.text .text.*)")
    set(BRAINSCAPE_LD_ITCM_DISCARD "*libgcc.a:*(.ARM.exidx .ARM.exidx.*)")
  elseif(ARG_ENGINE_CODE STREQUAL "XIP")
    set(BRAINSCAPE_LD_ENGINE_CODE "with the rest of the code, ${BRAINSCAPE_LD_TEXT}")
    set(BRAINSCAPE_LD_ITCM_INPUTS "")
    set(BRAINSCAPE_LD_ITCM_DISCARD "")
  else()
    message(FATAL_ERROR "brainscape_firmware_image(${name}): ENGINE_CODE must be ITCM or XIP")
  endif()
  set(ld ${CMAKE_CURRENT_BINARY_DIR}/${name}.ld)
  configure_file(${CMAKE_CURRENT_SOURCE_DIR}/linker/seed_h750.ld.in ${ld} @ONLY)
  add_executable(${name} ${ARG_SOURCES})
  target_link_libraries(${name} PRIVATE brainscape_fw_mem ${ARG_LIBS} brainscape_fw_platform)
  _brainscape_fw_flags(${name})
  target_compile_definitions(${name} PRIVATE BRAINSCAPE_FW_IMAGE="${name}"
                                             BRAINSCAPE_FW_ENGINE_CODE="${ARG_ENGINE_CODE}")
  set_target_properties(${name} PROPERTIES SUFFIX .elf LINK_DEPENDS ${ld})
  target_link_options(${name} PRIVATE
    -T${ld} --specs=nano.specs --specs=nosys.specs
    LINKER:--gc-sections LINKER:--check-sections LINKER:--warn-common
    LINKER:-Map=${CMAKE_CURRENT_BINARY_DIR}/${name}.map LINKER:--print-memory-usage)
  add_custom_command(TARGET ${name} POST_BUILD
    COMMAND ${BRAINSCAPE_ARM_OBJCOPY} -O binary $<TARGET_FILE:${name}> ${CMAKE_CURRENT_BINARY_DIR}/${name}.bin
    COMMAND ${BRAINSCAPE_ARM_OBJCOPY} -O ihex $<TARGET_FILE:${name}> ${CMAKE_CURRENT_BINARY_DIR}/${name}.hex
    COMMAND ${CMAKE_COMMAND} -DELF=$<TARGET_FILE:${name}> -DBIN=${CMAKE_CURRENT_BINARY_DIR}/${name}.bin
            -DSIZE=${BRAINSCAPE_ARM_SIZE} -DAPP_TYPE=${BRAINSCAPE_FIRMWARE_APP_TYPE}
            -DFLASH_ADDRESS=${BRAINSCAPE_FLASH_ADDRESS} -DENGINE_CODE=${ARG_ENGINE_CODE}
            -DOUT=${CMAKE_CURRENT_BINARY_DIR}/${name}.size.txt
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/ImageReport.cmake
    COMMAND ${CMAKE_COMMAND} -DELF=$<TARGET_FILE:${name}> -DOBJDUMP=${BRAINSCAPE_ARM_OBJDUMP}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/BootCheck.cmake
    BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/${name}.bin ${CMAKE_CURRENT_BINARY_DIR}/${name}.hex
               ${CMAKE_CURRENT_BINARY_DIR}/${name}.map ${CMAKE_CURRENT_BINARY_DIR}/${name}.size.txt
    VERBATIM)
  if(ARG_ENGINE_CODE STREQUAL "ITCM")
    list(JOIN _bs_not_itcm_members "," _bs_not_itcm_arg)
    add_custom_command(TARGET ${name} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -DELF=$<TARGET_FILE:${name}> -DREADELF=${BRAINSCAPE_ARM_READELF}
              -DARCHIVE=$<TARGET_FILE:brainscape_dsp> -DHOOKS_ARCHIVE=$<TARGET_FILE:brainscape_dsp_fpenv_hooks>
              -DNOT_ITCM=${_bs_not_itcm_arg}
              -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/ItcmCheck.cmake
      VERBATIM)
  endif()
endfunction()

brainscape_firmware_image(brainscape_parity
  SOURCES parity/main.cpp
  LIBS brainscape_fw_golden
  ENGINE_CODE ITCM
  MIN_HEAP 4M)
# The bench in three builds: the shipping engine in ITCM (the numbers), the same engine
# executing in place from QSPI (the code-placement A/B of profile §7.1), and the engine
# built with the FP guard's test hooks (flushing forced on and exception flags collected:
# the FZ = 0/1 silent-tail A/B and flag census of profile §4.2).
brainscape_firmware_image(brainscape_bench
  SOURCES bench/main.cpp bench/Microbench.cpp
  LIBS brainscape_fw_golden
  ENGINE_CODE ITCM
  MIN_HEAP 4M)
brainscape_firmware_image(brainscape_bench_xip
  SOURCES bench/main.cpp bench/Microbench.cpp
  LIBS brainscape_fw_golden
  ENGINE_CODE XIP
  MIN_HEAP 4M)
brainscape_firmware_image(brainscape_bench_hooks
  SOURCES bench/main.cpp bench/Microbench.cpp
  LIBS brainscape_fw_golden_hooks
  ENGINE_CODE ITCM
  MIN_HEAP 4M)
brainscape_firmware_image(brainscape_live
  SOURCES live/main.cpp live/LivePresets.cpp
  LIBS brainscape_fw_golden
  ENGINE_CODE ITCM
  MIN_HEAP 4M)

add_custom_target(brainscape_firmware ALL
  DEPENDS brainscape_parity brainscape_bench brainscape_bench_xip brainscape_bench_hooks
          brainscape_live)

# The Daisy bootloader binary libDaisy ships, beside the images, for a BOOT_QSPI setup.
if(BRAINSCAPE_FIRMWARE_APP_TYPE STREQUAL "BOOT_QSPI")
  configure_file(${libdaisy_SOURCE_DIR}/core/dsy_bootloader_v6_4-intdfu-2000ms.bin
                 ${CMAKE_CURRENT_BINARY_DIR}/dsy_bootloader_v6_4-intdfu-2000ms.bin COPYONLY)
endif()
