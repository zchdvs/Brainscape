#!/bin/bash
# Coverage-guided libFuzzer run (clang 14) of the decoder/validator, ASan+UBSan.
cd /w
{
echo "=== $(clang++-14 --version | head -1)"
clang++-14 -std=c++17 -O2 -I/b/dsp/include blob_compile.cpp blob_runtime.cpp -o /tmp/blob_clang && rm -rf /tmp/corpus && mkdir -p /tmp/corpus && /tmp/blob_clang seeds /tmp/corpus && ls -l /tmp/corpus
clang++-14 -std=c++17 -O1 -g -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all -I/b/dsp/include blob_libfuzzer.cpp blob_runtime.cpp -o /tmp/fz || exit 1
/tmp/fz -max_total_time=${1:-120} -print_final_stats=1 -max_len=4096 /tmp/corpus 2>&1 | grep -E "^(#[0-9]+ +(DONE|INITED)|stat::|==|SUMMARY|Done)" | tail -20
echo "exit status ${PIPESTATUS[0]}"
} > /w/out_fuzz_clang.txt 2>&1
echo done fuzz clang
