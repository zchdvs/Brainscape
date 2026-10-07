# libDaisy, pinned (firmware/README.md, "Dependencies"). Fetched into the build tree at
# configure time and never vendored: it bundles ST code under SLA0044
# (docs/design/companion-app.md §7.2), and these prototype images are not distributed.
#
# Tag v9.0.0 = commit 08087203debc646d3710ec8caa35bac6cf20ab5e (github.com/daisyaudio/libDaisy,
# formerly electro-smith/libDaisy). GitHub's archive of a commit carries no submodules, so the
# four libDaisy builds from are fetched at the commits that libDaisy commit records for them
# (its gitlinks), each by archive SHA-256. libDaisy's other submodules (CMSIS-DSP, googletest)
# are not used by its library build and are not fetched.
#
# Offline or air-gapped builds: FETCHCONTENT_SOURCE_DIR_LIBDAISY=<a libDaisy checkout at that
# commit with those submodules initialized> skips every download below.
include_guard(GLOBAL)
include(FetchContent)
if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)  # extracted files get the extraction time
endif()

set(BRAINSCAPE_LIBDAISY_TAG    "v9.0.0")
set(BRAINSCAPE_LIBDAISY_COMMIT "08087203debc646d3710ec8caa35bac6cf20ab5e")

# A source directory with no CMakeLists.txt, so FetchContent_MakeAvailable only populates:
# libDaisy is added below, once its submodule directories are filled.
set(_bs_no_cmake "brainscape-populate-only")

FetchContent_Declare(libdaisy
  URL      https://github.com/daisyaudio/libDaisy/archive/${BRAINSCAPE_LIBDAISY_COMMIT}.tar.gz
  URL_HASH SHA256=3c45bc7738394e5d94e4f809d42148629358158f84ee3b610b1d26a7864d0cea
  SOURCE_SUBDIR ${_bs_no_cmake}
)
FetchContent_MakeAvailable(libdaisy)

if(NOT FETCHCONTENT_SOURCE_DIR_LIBDAISY)
  # name | path in libDaisy | repository | gitlink commit at v9.0.0 | archive SHA-256
  set(_bs_libdaisy_submodules
    "cmsis5|Drivers/CMSIS_5|ARM-software/CMSIS_5|2b7495b8535bdcb306dac29b9ded4cfb679d7e5c|9f7f0080795b1635b82d9f7b084d37011866cd78365749997391ba0f9602f9fe"
    "cmsisdevh7|Drivers/CMSIS-Device/ST/STM32H7xx|STMicroelectronics/cmsis_device_h7|6dac8c24d7b38ab20806d27dd7d8285a6433b8f7|7479ca90f6d90c5f108d48211c189fd0daf7cda1dbf537362adf57cd693907c9"
    "halh7|Drivers/STM32H7xx_HAL_Driver|STMicroelectronics/stm32h7xx_hal_driver|404a70d4c19c124636c78801ff7c0dbc87bc4256|f0bb4062e3bd09a98b7a3e2047e1816328157490662ba2a7630ee22c048ff28a"
    "usbdevice|Middlewares/ST/STM32_USB_Device_Library|STMicroelectronics/stm32_mw_usb_device|7b5e6886d2f11ad15d2b46364e8a96984358f639|3cf84090a8179dd24c4c0389e2caa26dc2c026f4ffe6eb6ae69ca23e83924ffc"
  )
  foreach(entry IN LISTS _bs_libdaisy_submodules)
    string(REPLACE "|" ";" f "${entry}")
    list(GET f 0 name)
    list(GET f 1 path)
    list(GET f 2 repo)
    list(GET f 3 commit)
    list(GET f 4 sha)
    FetchContent_Declare(libdaisy_${name}
      URL      https://github.com/${repo}/archive/${commit}.tar.gz
      URL_HASH SHA256=${sha}
      SOURCE_DIR ${libdaisy_SOURCE_DIR}/${path}
      SOURCE_SUBDIR ${_bs_no_cmake}
    )
    FetchContent_MakeAvailable(libdaisy_${name})
  endforeach()
endif()

foreach(probe IN ITEMS
    Drivers/CMSIS_5/CMSIS/Core/Include/core_cm7.h
    Drivers/CMSIS-Device/ST/STM32H7xx/Include/stm32h750xx.h
    Drivers/STM32H7xx_HAL_Driver/Inc/stm32h7xx_hal.h
    Middlewares/ST/STM32_USB_Device_Library/Core/Inc/usbd_core.h)
  if(NOT EXISTS "${libdaisy_SOURCE_DIR}/${probe}")
    message(FATAL_ERROR "libDaisy ${BRAINSCAPE_LIBDAISY_TAG}: ${probe} is missing under "
                        "${libdaisy_SOURCE_DIR} (an incomplete FETCHCONTENT_SOURCE_DIR_LIBDAISY?)")
  endif()
endforeach()

# libDaisy's own toolchain file (cmake/toolchains/stm32h750xx.cmake) sets these as directory
# options. Under Brainscape's toolchain file they are set on firmware/ (this directory and
# libDaisy, added from it); dsp/ is a sibling directory, so the engine archive never sees
# them. The CPU, FPU and floating-point profile flags come from the toolchain file.
set(BRAINSCAPE_DAISY_DEFINITIONS CORE_CM7 STM32H750xx STM32H750IB ARM_MATH_CM7 HSE_VALUE=16000000)
if(NOT BRAINSCAPE_FIRMWARE_APP_TYPE STREQUAL "BOOT_NONE")
  # A bootloaded application: the Daisy bootloader has already run SystemInit (FPU access,
  # clocks reset, VTOR), so libDaisy's startup code skips it (core/startup_stm32h750xx.c).
  list(APPEND BRAINSCAPE_DAISY_DEFINITIONS BOOT_APP)
endif()
add_compile_definitions(${BRAINSCAPE_DAISY_DEFINITIONS})
add_compile_options(-ffunction-sections -fdata-sections)
add_subdirectory(${libdaisy_SOURCE_DIR} ${libdaisy_BINARY_DIR} EXCLUDE_FROM_ALL)
# libDaisy's Reset_Handler copies .data and zeroes .bss in plain loops, which GCC turns into
# calls to memcpy and memset. An ITCM image keeps the firmware's memcpy and memset in ITCM
# (platform/MemFunctions.c), which holds nothing until the preinit hook has copied it in, so
# the startup file is built with the loops kept as loops (checked from the disassembly as
# every image links, firmware/cmake/BootCheck.cmake).
set_source_files_properties(${libdaisy_SOURCE_DIR}/core/startup_stm32h750xx.c
  DIRECTORY ${libdaisy_SOURCE_DIR}
  PROPERTIES COMPILE_OPTIONS -fno-tree-loop-distribute-patterns)

# Everything a firmware translation unit that includes libDaisy headers needs.
add_library(brainscape_daisy INTERFACE)
target_link_libraries(brainscape_daisy INTERFACE daisy)
target_compile_definitions(brainscape_daisy INTERFACE
  BRAINSCAPE_LIBDAISY_VERSION="${BRAINSCAPE_LIBDAISY_TAG} ${BRAINSCAPE_LIBDAISY_COMMIT}")
