#!/bin/bash
# Identify the qemu-arm build that binfmt_misc uses, by reading /proc/<pid>/exe of a running guest.
cd "$(dirname "$0")"
./bin/arm_orig.elf ../out/in48_60s.f32 ver --only shimmer >/dev/null 2>&1 &
PID=$!
sleep 1
ls -l /proc/$PID/exe 2>&1
cat /proc/$PID/exe 2>/dev/null | strings | grep -m3 -iE "QEMU emulator version|qemu-[0-9]|v[0-9]+\.[0-9]+\.[0-9]+" 
cat /proc/$PID/cmdline | tr '\0' ' '; echo
kill $PID 2>/dev/null
uname -r
