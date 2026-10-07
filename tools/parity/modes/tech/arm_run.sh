#!/bin/bash
# Compile-only Cortex-M7 checks with the pinned arm-none-eabi 10.3-2021.10 (same as CI).
A=/opt/gcc-arm-none-eabi-10.3-2021.10/bin/arm-none-eabi-
W=/w; B=/b
cd $W
FLAGS="-mcpu=cortex-m7 -mthumb -mfpu=fpv5-d16 -mfloat-abi=hard -fno-fast-math -ffp-contract=off -fno-math-errno -fno-exceptions -fno-rtti -O2 -std=c++17"
FPRE='\sv(add|sub|mul|nmul|div|sqrt|cvt[a-z]*|cmp|cmpe|fm[as]|fnm[as]|ml[as]|nml[as]|abs|neg|sel|rint[a-z]*|max|min)[a-z0-9]*\.f(32|64)'
{
echo "=== $(${A}g++ --version | head -1)"
echo "--- bsnum (in-house number code) for the M7"
${A}g++ $FLAGS -c bsnum_arm.cpp -o /tmp/bsnum_arm.o && echo compiled
echo "FP arithmetic/convert instructions: $(${A}objdump -d /tmp/bsnum_arm.o | grep -cE "$FPRE")"
echo "all v* FP-register instructions: $(${A}objdump -d /tmp/bsnum_arm.o | grep -E '\sv[a-z]+' | awk '{print $3}' | sort | uniq -c | tr '\n' ' ')"
echo "undefined symbols: $(${A}nm -u /tmp/bsnum_arm.o | awk '{print $2}' | tr '\n' ' ')"
${A}size /tmp/bsnum_arm.o | tail -1
echo "--- blob runtime (decoder/validator/SHA-256) for the M7"
${A}g++ $FLAGS -I$B/dsp/include -c blob_runtime.cpp -o /tmp/blob_rt_arm.o && echo "compiled (static_assert layout of DecodedPackage holds)"
echo "FP arithmetic/convert instructions: $(${A}objdump -d /tmp/blob_rt_arm.o | grep -cE "$FPRE")"
echo "undefined symbols: $(${A}nm -u /tmp/blob_rt_arm.o | awk '{print $2}' | tr '\n' ' ')"
${A}size /tmp/blob_rt_arm.o | tail -1
echo "--- hazard sizes on the M7 (enum bool size_t long Naive alignof offsetof(f) long-double wchar_t enum:u8)"
${A}g++ $FLAGS -S hazard.cpp -o /tmp/hazard_arm.s && grep -A12 '^kSizes:' /tmp/hazard_arm.s | grep -E '\.word' | awk '{print $2}' | tr '\n' ' '; echo
echo "--- macro curve cost (DetMath PowF per target vs table+lerp), static instruction counts"
${A}g++ $FLAGS -I$B/dsp/src -I$B/dsp/include -c macro_cost.cpp -o /tmp/macro_arm.o
${A}g++ $FLAGS -I$B/dsp/src -I$B/dsp/include -c $B/dsp/src/DetMath.cpp -o /tmp/detmath_arm.o
for f in EvalTarget EvalTable FanOut; do
  n=$(${A}objdump -d --no-show-raw-insn /tmp/macro_arm.o | awk -v f="$f" '/^[0-9a-f]+ <.*>:$/{p=index($0,f)>0} p && /^ +[0-9a-f]+:/{c++} END{print c}')
  echo "  $f: $n instructions"
done
for f in PowF Log2D LogD Exp2D ExpD; do
  n=$(${A}objdump -d --no-show-raw-insn /tmp/detmath_arm.o | ${A}c++filt | awk -v f="detmath::$f(" '/^[0-9a-f]+ <.*>:$/{p=index($0,f)>0} p && /^ +[0-9a-f]+:/{c++} END{print c}')
  echo "  detmath::$f: $n instructions"
done
echo "  ExpKernel (anonymous):"; ${A}objdump -d --no-show-raw-insn /tmp/detmath_arm.o | ${A}c++filt | awk '/^[0-9a-f]+ <.*>:$/{name=$0} /^ +[0-9a-f]+:/{c[name]++} END{for (n in c) print "   ", c[n], n}' | sort -k2 | head -30
echo "  vdiv/vsqrt in PowF path: $(${A}objdump -d /tmp/detmath_arm.o | grep -cE 'vdiv|vsqrt')"
} > $W/out_arm.txt 2>&1
echo done arm
