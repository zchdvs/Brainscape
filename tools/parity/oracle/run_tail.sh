#!/bin/bash
cd "$(dirname "$0")"
for v in arm_det_nc arm_det_nc_nofz; do QEMU_CPU=cortex-m7 ./bin/$v.elf in48_tail60.f32 $v --chunks > logs/tail_$v.txt; grep -v "^  " logs/tail_$v.txt; done
