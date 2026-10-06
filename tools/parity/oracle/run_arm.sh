#!/bin/bash
# Run every ARM oracle variant under qemu-arm (user mode, via binfmt) on the 10 s input.
cd "$(dirname "$0")"
for v in arm_orig arm_orig_nc arm_det arm_det_nc arm_det_nc_nofz; do
  start=$(date +%s.%N)
  ./bin/$v.elf ../out/in48.f32 $v --out out > logs/$v.txt 2>&1
  end=$(date +%s.%N)
  echo "$v wall=$(echo "$end - $start" | bc 2>/dev/null || python3 -c "print($end-$start)")s" >> logs/$v.txt
done
cat logs/arm_*.txt
