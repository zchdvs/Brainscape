#!/bin/bash
# Mutation fuzz of the package decoder/validator under ASan+UBSan (GCC).
cd /w
{
echo "=== $(g++ --version | head -1)"
g++ -std=c++17 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -I/b/dsp/include blob_compile.cpp blob_runtime.cpp -o /tmp/blob_asan || exit 1
/tmp/blob_asan hashes
time /tmp/blob_asan fuzz ${1:-3000000}
echo "exit status $?"
} > /w/out_fuzz_gcc.txt 2>&1
echo done fuzz gcc
