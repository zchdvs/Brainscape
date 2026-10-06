#!/bin/bash
cd "$(dirname "$0")"
./bin/arm_orig.elf ../out/in48.f32 t --only clean; echo "rc=$?"
QEMU_STRACE=1 ./bin/arm_orig.elf ../out/in48.f32 t --only clean 2>&1 | head -30
