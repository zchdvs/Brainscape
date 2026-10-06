#!/bin/bash
cd "$(dirname "$0")"
for cpu in cortex-m7 cortex-m4 cortex-a15 max; do
  s=$(date +%s.%N)
  out=$(QEMU_CPU=$cpu ./bin/arm_det_nc.elf ../out/in48.f32 cpu_$cpu 2>&1 | grep -v input | sed -E 's/.*\] ([a-z]+) .*fnv=([0-9a-f]+)/\1=\2/' | tr '\n' ' ')
  e=$(date +%s.%N)
  echo "QEMU_CPU=$cpu wall=$(python3 -c "print(round($e-$s,1))")s :: $out"
done
# M-profile ISA check: an fpv5 (double-precision) build must UNDEF on cortex-m4 (single-precision FPv4-SP)
