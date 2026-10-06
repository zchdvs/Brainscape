#!/bin/bash
set -e
cd /w/oracle
g++ --version | head -1
mkdir -p bin_gcc
b() { name=$1; dsp=$2; shift 2
  g++ -std=c++17 -O2 -fno-rtti -fno-exceptions "$@" -I$dsp/include $dsp/src/*.cpp oracle.cpp -o bin_gcc/$name
  echo "$name fma-instrs: $(objdump -d bin_gcc/$name | grep -cE 'vfn?m(add|sub)[0-9]+[sp]s' || true)"
}
b gcc_orig      dsp_orig -ffp-contract=off
b gcc_det       dsp_det  -ffp-contract=off
b gcc_det_v3    dsp_det  -ffp-contract=off -march=x86-64-v3
b gcc_det_v3fma dsp_det  -march=x86-64-v3
b gcc_det_O3v3  dsp_det  -O3 -ffp-contract=off -march=x86-64-v3
b gcc_fma dsp_fma -ffp-contract=off
b gcc_fma_v3 dsp_fma -ffp-contract=off -march=x86-64-v3
for v in gcc_fma gcc_fma_v3; do ./bin_gcc/$v ../out/in48.f32 $v | grep -v input; done
