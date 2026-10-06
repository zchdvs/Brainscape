#!/bin/sh
cd /w
REX2='vfn?m(add|sub)[0-9]+(ss|ps)'
for v in "gnu17:-std=gnu++17" "iso17:-std=c++17" "on:-std=c++17 -ffp-contract=on" "off:-std=c++17 -ffp-contract=off" "pragma_stdc:-std=c++17 -DPRAGMA_STDC -Wall"; do
  n=${v%%:*}; f=${v#*:}; g++ -O3 -march=haswell $f -S contract.cpp -o asm/gcc14_$n.s 2>asm/gcc14_$n.err; echo "gcc14 x64 $n: $(./count.sh asm/gcc14_$n.s "$REX2") warn=$(grep -c warning asm/gcc14_$n.err)"; done
gcc --version | head -1
head -3 asm/gcc14_pragma_stdc.err
