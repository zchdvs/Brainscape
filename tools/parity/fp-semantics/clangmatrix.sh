#!/bin/sh
cd /w
REA='^[[:space:]]*(fn?m(add|sub)|fml[as])[[:space:]]'
REX='vfn?m(add|sub)[0-9]+ss|vfn?m(add|sub)[0-9]+ps'
REM='vf(n)?m[as]\.f(32|64)'
c() { tgt=$1; re=$2; name=$3; shift 3; clang++-14 --target=$tgt -O3 -S "$@" contract.cpp -o asm/clang14_$name.s 2>asm/clang14_$name.err; echo "clang14 $name: $(./count.sh asm/clang14_$name.s "$re") warn=$(grep -c warning asm/clang14_$name.err)"; }
c aarch64-linux-gnu "$REA" a64_default -std=c++17
c arm64-apple-macos11 "$REA" apple_default -std=c++17
c arm64-apple-macos11 "$REA" apple_off -std=c++17 -ffp-contract=off
c arm64-apple-macos11 "$REA" apple_fast -std=c++17 -ffp-contract=fast
c arm64-apple-macos11 "$REA" apple_pragma_stdc -std=c++17 -DPRAGMA_STDC
c arm64-apple-macos11 "$REA" apple_pragma_clang -std=c++17 -DPRAGMA_CLANG
c arm64-apple-macos11 "$REA" apple_fpmodel_strict -std=c++17 -ffp-model=strict
c thumbv7em-none-eabihf "$REM" m7_default -std=c++17 -mcpu=cortex-m7 -mfpu=fpv5-d16 -mfloat-abi=hard
c x86_64-linux-gnu "$REX" x64_default -std=c++17
c x86_64-linux-gnu "$REX" x64_fma -std=c++17 -mfma
c x86_64-linux-gnu "$REX" x64_haswell_off -std=c++17 -march=haswell -ffp-contract=off
echo "-- clang aarch64 k_reduce body ops:"; awk '/^k_reduce:/,/ret/' asm/clang14_a64_default.s | grep -E "fadd|faddp" | head -5
echo "-- gcc-11 x86 (-march=haswell):"
REX2='vfn?m(add|sub)[0-9]+(ss|ps)'
for v in "gnu17:-std=gnu++17" "iso17:-std=c++17" "on:-std=c++17 -ffp-contract=on" "off:-std=c++17 -ffp-contract=off" "pragma_stdc:-std=c++17 -DPRAGMA_STDC"; do
  n=${v%%:*}; f=${v#*:}; g++ -O3 -march=haswell $f -S contract.cpp -o asm/gcc11_$n.s 2>asm/gcc11_$n.err; echo "gcc11 x64 $n: $(./count.sh asm/gcc11_$n.s "$REX2") warn=$(grep -c warning asm/gcc11_$n.err)"; done
