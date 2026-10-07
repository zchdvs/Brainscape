#!/bin/bash
# llvm-mca (LLVM 14's Cortex-M7 scheduling model) on the path of one macro target:
# EvalTarget (PowF inlined) + LogD + Exp2D bodies as clang emits them, concatenated (MCA
# ignores control flow: every instruction of every body counts once, untaken branches
# included, so this over-counts slightly). vcmp #0 forms are rewritten to a register
# compare (MCA 14 lacks them). Compared with the table+lerp alternative.
cd /w
F="--target=thumbv7em-none-eabihf -mcpu=cortex-m7 -mfpu=fpv5-d16 -mfloat-abi=hard -O2 -std=c++17 -fno-fast-math -ffp-contract=off -fno-math-errno -fno-exceptions -fno-rtti -DNDEBUG -ffreestanding -mllvm -ifcvt-limit=0"
G=/opt/gcc-arm-none-eabi-10.3-2021.10/arm-none-eabi/include
body() { sed -n "/^$1:/,/\.fnend/p" /tmp/mc.s | grep -E $'^\t[a-z]' | grep -vE $'^\t(bl|it[te]*)\t' | sed -E 's/(vcmp(e)?\.f(32|64)\s+([sd])([0-9]+)), #0(\.0)?/\1, \431/'; }
{
clang++-14 $F -DFLATTEN -I/b/dsp/src -I/b/dsp/include -isystem $G/c++/10.3.1 -isystem $G/c++/10.3.1/arm-none-eabi/thumb/v7e-m+dp/hard -isystem $G -S macro_cost.cpp -o /tmp/mc.s
{ body _ZN5probe10EvalTargetERKNS_6TargetEf; body _ZN10brainscape7detmath4LogDEd; body _ZN10brainscape7detmath5Exp2DEd; } > /tmp/target.s
body _ZN5probe9EvalTableEPKff > /tmp/table.s
for f in target table; do
  echo "=== $f: $(wc -l < /tmp/$f.s) instructions (f64 ops $(grep -cE '\.f64' /tmp/$f.s), f32 ops $(grep -cE '\.f32' /tmp/$f.s), vdiv $(grep -c vdiv /tmp/$f.s))"
  llvm-mca-14 -mtriple=thumbv7em-none-eabihf -mcpu=cortex-m7 -iterations=1 /tmp/$f.s 2>&1 | grep -E '^(error|note|Iterations|Instructions|Total Cycles)' | sed 's/^/  1 iter: /'
  llvm-mca-14 -mtriple=thumbv7em-none-eabihf -mcpu=cortex-m7 -iterations=100 /tmp/$f.s 2>&1 | grep -E '^(Total Cycles)' | sed 's/^/  100 iters: /'
done
} > /w/out_mca.txt 2>&1
echo done mca
