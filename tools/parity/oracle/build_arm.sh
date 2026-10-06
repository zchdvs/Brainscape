#!/bin/bash
# Builds the oracle ELF for the Cortex-M7 with firmware compile flags, linked
# against arm_sys.c (Linux EABI syscall shim; libgloss-linux's SVC-immediate
# convention does not work under modern qemu-user) so qemu-arm user mode runs it:
#   QEMU_CPU=cortex-m7 ./bin/<variant>.elf <in_mono.f32> <tag> [--chunks]
set -e
export PATH="/c/Program Files (x86)/GNU Arm Embedded Toolchain/10 2021.10/bin:$PATH"
cd "$(dirname "$0")"
FW="-mcpu=cortex-m7 -mthumb -mfpu=fpv5-d16 -mfloat-abi=hard -O3 -std=gnu++17 -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections"
mkdir -p bin obj
build() { # name dspdir extra-flags...
  local name=$1 dsp=$2; shift 2
  mkdir -p obj/$name
  for s in Engine Granular OnsetDetector PostChain; do
    arm-none-eabi-g++ $FW "$@" -I$dsp/include -c $dsp/src/$s.cpp -o obj/$name/$s.o
  done
  arm-none-eabi-ar rcs obj/$name/libdsp.a obj/$name/Engine.o obj/$name/Granular.o obj/$name/OnsetDetector.o obj/$name/PostChain.o
  arm-none-eabi-g++ $FW -I$dsp/include -c oracle.cpp -o obj/$name/oracle.o
  arm-none-eabi-gcc -mcpu=cortex-m7 -mthumb -mfpu=fpv5-d16 -mfloat-abi=hard -O2 -c arm_sys.c -o obj/$name/arm_sys.o
  arm-none-eabi-g++ $FW -nostartfiles -static -Wl,--gc-sections obj/$name/oracle.o obj/$name/arm_sys.o obj/$name/libdsp.a -lm -o bin/$name.elf
  local fma=$(arm-none-eabi-objdump -d obj/$name/libdsp.a | grep -cE "\svfn?m[as]\.f(32|64)" || true)
  echo "$name: vfma/vfms/vfnm* in dsp lib = $fma"
}
build arm_orig      dsp_orig
build arm_orig_nc   dsp_orig -ffp-contract=off
build arm_det       dsp_det
build arm_det_nc    dsp_det  -ffp-contract=off
build arm_det_nc_nofz dsp_det -ffp-contract=off -DBRAINSCAPE_PROBE_NO_FZ -DBRAINSCAPE_ALLOW_NO_DENORMAL_GUARD
build arm_det_nc_Os dsp_det -ffp-contract=off -Os
build arm_det_iso dsp_det -std=c++17
build arm_fma_nc dsp_fma -ffp-contract=off
arm-none-eabi-size bin/*.elf
