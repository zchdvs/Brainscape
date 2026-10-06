#!/bin/sh
# LTO with mixed per-TU flags: engine TUs -ffp-contract=off, "wrapper" TU (harness) and
# link step at the compiler default (contraction allowed), FMA ISA enabled everywhere.
# usage: lto_test.sh <tag> <cxx> <engine-extra-flags>
set -e
tag=$1; cxx=$2; shift 2
cd /w; mkdir -p bin/lto_$tag
B="-std=c++17 -O3 -march=x86-64-v3 -flto -fno-exceptions -fno-rtti"
for s in Engine Granular OnsetDetector PostChain; do $cxx $B -ffp-contract=off "$@" -Idsp_det/include -c dsp_det/src/$s.cpp -o bin/lto_$tag/$s.o; done
$cxx $B -Idsp_det/include -Iharness -c harness/battery.cpp -o bin/lto_$tag/main.o
$cxx $B bin/lto_$tag/*.o -o bin/battery_lto_$tag
fma=$(objdump -d bin/battery_lto_$tag | grep -cE "vf(n)?m(add|sub)[0-9]{3}s[sd]" || true)
./bin/battery_lto_$tag --in input/input.f32 --tag lto_$tag --seconds 30 > out/hash/lto_$tag.txt
echo "lto_$tag fma_insns=$fma $(grep ALL out/hash/lto_$tag.txt)"
