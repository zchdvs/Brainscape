#!/bin/bash
cd "$(dirname "$0")"
s=$(date +%s.%N)
./bin/arm_det_nc.elf ../out/in48_60s.f32 arm_det_nc_60 --chunks > logs/arm_det_nc_60.txt
e=$(date +%s.%N); echo "arm_det_nc 60s x 8 presets wall=$(python3 -c "print(round($e-$s,1))")s" | tee -a logs/arm_det_nc_60.txt
for v in arm_det_nc_Os arm_det_iso; do ./bin/$v.elf ../out/in48.f32 $v > logs/$v.txt; done
./bin/arm_det_nc.elf ../out/in48.f32 arm_det_nc_b37 --block 37 > logs/arm_det_nc_b37.txt
./bin/arm_det_nc.elf ../out/in48.f32 arm_det_nc_b512 --block 512 > logs/arm_det_nc_b512.txt
./bin/arm_det_nc.elf ../out/in48.f32 arm_det_nc_h21 --hist 21 > logs/arm_det_nc_h21.txt
cat logs/arm_det_nc_Os.txt logs/arm_det_iso.txt logs/arm_det_nc_b37.txt logs/arm_det_nc_b512.txt logs/arm_det_nc_h21.txt | grep -v input
