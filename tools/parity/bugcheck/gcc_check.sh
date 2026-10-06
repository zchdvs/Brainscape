#!/bin/sh
# Build each dsp/ tree with the CI's GCC flags (-Wall -Wextra -Werror, no exceptions/RTTI
# for the library), then run the full unit suite and the block-split harness.
set -e
cd /w
g++ --version | head -1
for t in dsp_fix dsp_fixB dsp_regress; do
  echo "##### $t"
  out=/tmp/$t; mkdir -p $out
  for f in $t/src/*.cpp; do
    g++ -std=c++17 -O2 -fno-exceptions -fno-rtti -Wall -Wextra -Werror -I$t/include -c $f -o $out/$(basename $f .cpp).o
  done
  ar rcs $out/libdsp.a $out/*.o
  g++ -std=c++17 -O2 -I$t/include -I$t/tests/vendor $t/tests/test_main.cpp $t/tests/test_engine.cpp $out/libdsp.a -o $out/tests
  $out/tests | tail -2 || true
  g++ -std=c++17 -O2 -I$t/include bugcheck.cpp $out/libdsp.a -o $out/bugcheck
  $out/bugcheck all | grep -E "broken cases"
  $out/bugcheck matrix | grep -E "broken cases"
done
