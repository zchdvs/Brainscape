#!/bin/bash
# usage: arm_build.sh <tag> <tree> <gcc-bin-dir> <extra flags...>
# Firmware flags (task brief / tools/cmake toolchain) + extras. Produces:
#   arm/<tag>/obj/*.o      engine objects (for disassembly / nm)
#   arm/<tag>/battery.elf  semihosting battery runnable under qemu-arm -cpu cortex-m7
set -e
W=$(cd "$(dirname "$0")" && pwd)
tag=$1; tree=$2; bin=$3; shift 3
CXX="$bin/arm-none-eabi-g++"
FW="-mcpu=cortex-m7 -mthumb -mfpu=fpv5-d16 -mfloat-abi=hard -O3 -std=gnu++17 -fno-exceptions -fno-rtti"
out=$W/arm/$tag
mkdir -p $out/obj
for s in Engine Granular OnsetDetector PostChain; do
  "$CXX" $FW "$@" -I$W/$tree/include -c $W/$tree/src/$s.cpp -o $out/obj/$s.o
done
"$CXX" $FW "$@" -I$W/$tree/include -I$W/harness -c $W/harness/battery.cpp -o $out/obj/battery_main.o
"$CXX" $FW "$@" --specs=rdimon.specs -Wl,-Ttext-segment=0x400000 \
  $out/obj/battery_main.o $out/obj/Engine.o $out/obj/Granular.o $out/obj/OnsetDetector.o $out/obj/PostChain.o \
  -o $out/battery.elf
echo "$tag built ($("$CXX" --version | head -1))"
