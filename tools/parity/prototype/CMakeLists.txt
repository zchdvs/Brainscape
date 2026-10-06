cmake_minimum_required(VERSION 3.21)
project(determinism_profile LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

set(H ${CMAKE_CURRENT_SOURCE_DIR}/harness)
set(DET ${CMAKE_CURRENT_SOURCE_DIR}/dsp_det)
set(REF ${CMAKE_CURRENT_SOURCE_DIR}/dsp_ref)

# One battery executable per (source tree, flag set). The harness TU gets the same
# flags as the engine; it does no float work on the hashed path.
function(variant name tree)
  add_executable(battery_${name} ${H}/battery.cpp
    ${tree}/src/Engine.cpp ${tree}/src/Granular.cpp ${tree}/src/OnsetDetector.cpp ${tree}/src/PostChain.cpp)
  target_include_directories(battery_${name} PRIVATE ${tree}/include ${H})
  target_compile_options(battery_${name} PRIVATE /GR- /O2 ${ARGN})
endfunction()

# Positive set: in-tree math, contraction off (VS2022 default for /fp:precise).
variant(det_precise     ${DET} /fp:precise)
variant(det_strict      ${DET} /fp:strict)
variant(det_avx2        ${DET} /arch:AVX2 /fp:precise)
variant(det_avx2strict  ${DET} /arch:AVX2 /fp:strict)
# Negative controls: contraction / value-unsafe optimizations.
variant(det_contract    ${DET} /arch:AVX2 /fp:precise /fp:contract)
variant(det_fast        ${DET} /arch:AVX2 /fp:fast)
# Baseline: the current repo sources (UCRT libm).
variant(ref_precise     ${REF} /fp:precise)
variant(ref_avx2        ${REF} /arch:AVX2 /fp:precise)

add_executable(gen_input ${H}/gen_input.cpp)
add_executable(nullcmp ${H}/nullcmp.cpp)
add_executable(accuracy ${H}/accuracy.cpp)
target_include_directories(accuracy PRIVATE ${DET}/include)
target_compile_options(accuracy PRIVATE /O2 /fp:precise)

# Source-level enforcement test: bad flags + forced-include of FpProfile.h.
variant(det_contract_pragma ${DET} /arch:AVX2 /fp:precise /fp:contract /FI${DET}/include/brainscape/FpProfile.h)

# Option B: explicit source-placed FMAs (dsp_fma), contraction still off.
set(FMA ${CMAKE_CURRENT_SOURCE_DIR}/dsp_fma)
variant(fma_avx2 ${FMA} /arch:AVX2 /fp:precise)
variant(fma_sse  ${FMA} /fp:precise)
