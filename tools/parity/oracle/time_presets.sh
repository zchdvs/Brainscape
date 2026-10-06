#!/bin/bash
cd "$(dirname "$0")"
for v in arm_orig arm_orig_nc arm_det; do
  for p in default clean shimmer syncpitch strum selfosc post freeze; do
    s=$(date +%s.%N); ./bin/$v.elf ../out/in48.f32 $v --only $p > /dev/null; e=$(date +%s.%N)
    printf "%-12s %-9s %6.2fs\n" $v $p $(python3 -c "print($e-$s)")
  done
done
