#!/bin/bash
# usage: linux_run.sh TAG CXX [quick]
TAG=$1; CXX=$2; QUICK=${3:-}; EXTRA=${4:-}
W=/w; B=/b
cd $W
OUT=$W/out_$TAG.txt
{
echo "=== $TAG: $($CXX --version | head -1)"
$CXX -std=c++17 -O2 -pthread -Wall -Wextra $EXTRA numtest.cpp -o /tmp/numtest_$TAG || exit 1
/tmp/numtest_$TAG info
/tmp/numtest_$TAG edge
/tmp/numtest_$TAG speed
/tmp/numtest_$TAG halfway 1009
/tmp/numtest_$TAG random 20000000
$CXX -std=c++17 -O2 -Wall -Wextra -I$B/dsp/include blob_compile.cpp blob_runtime.cpp -o /tmp/blob_$TAG || exit 1
/tmp/blob_$TAG hashes
$CXX -std=c++17 -O2 -DHOST_MAIN hazard.cpp -o /tmp/hazard_$TAG && /tmp/hazard_$TAG
if [ "$QUICK" != quick ]; then /tmp/numtest_$TAG roundtrip 1; fi
} > $OUT 2>&1
echo done $TAG
