#!/bin/bash
cd "$(dirname "$0")"
QEMU_CPU=cortex-m7 ./bin/arm_det_nc.elf ../out/in48.f32 arm_det_nc_rz --rz | grep -v input
