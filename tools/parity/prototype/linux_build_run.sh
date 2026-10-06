#!/bin/sh
# usage: linux_build_run.sh <tag> <tree: dsp_det|dsp_ref> <cxx> <flags...>
# Builds the battery from /w/<tree> with the given compiler+flags, runs it on the
# stored input (30 s, 48-frame blocks) and writes /w/out/hash/<tag>.txt.
set -e
tag=$1; tree=$2; cxx=$3; shift 3
cd /w
mkdir -p bin out/hash out/dump
$cxx -std=c++17 -fno-exceptions -fno-rtti "$@" -I$tree/include -Iharness \
  harness/battery.cpp $tree/src/Engine.cpp $tree/src/Granular.cpp $tree/src/OnsetDetector.cpp \
  $tree/src/PostChain.cpp -o bin/battery_$tag
# fused-multiply-add instruction count in the engine objects (x86: vfmadd/vfmsub/vfnmadd/vfnmsub)
fma=$(objdump -d bin/battery_$tag | grep -cE "vf(n)?m(add|sub)[0-9]{3}s[sd]" || true)
dump=""
[ -n "$DUMP" ] && dump="--dump out/dump"
./bin/battery_$tag --in input/input.f32 --tag $tag --seconds 30 $dump > out/hash/$tag.txt
echo "$tag ($($cxx --version | head -1 | sed 's/ (.*)//'); flags: $*) fma_insns=$fma ALL=$(grep ALL out/hash/$tag.txt | awk '{print $2}')"
