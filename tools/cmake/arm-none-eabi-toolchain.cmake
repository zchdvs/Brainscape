# Cross-compile toolchain for the Daisy Seed's Cortex-M7 (hard-float, matching
# libDaisy's own flags). Used by the CI compile-only job to catch dsp/ portability
# breaks (libm calls in the hot loop, soft-float macro traps, size_t narrowing)
# long before firmware bring-up. Usage:
#   cmake -B build-arm -DCMAKE_TOOLCHAIN_FILE=tools/cmake/arm-none-eabi-toolchain.cmake \
#         -DBRAINSCAPE_BUILD_TESTS=OFF
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(CMAKE_C_COMPILER arm-none-eabi-gcc)
set(CMAKE_CXX_COMPILER arm-none-eabi-g++)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(BRAINSCAPE_M7_FLAGS "-mcpu=cortex-m7 -mthumb -mfpu=fpv5-d16 -mfloat-abi=hard")
# Determinism profile (docs/design/determinism-profile.md §3.2): firmware code outside
# dsp/ gets the profile too. Without -ffp-contract=off this toolchain fused 152
# multiply-adds in dsp/.
set(BRAINSCAPE_M7_FP_FLAGS "-ffp-contract=off -fno-math-errno")
set(CMAKE_C_FLAGS_INIT   "${BRAINSCAPE_M7_FLAGS} ${BRAINSCAPE_M7_FP_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${BRAINSCAPE_M7_FLAGS} ${BRAINSCAPE_M7_FP_FLAGS} -fno-exceptions -fno-rtti")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
