A=/opt/gcc-arm-none-eabi-10.3-2021.10/bin/arm-none-eabi-
for o in /tmp/bsnum_arm.o /tmp/blob_rt_arm.o; do :; done
cd /w
FLAGS="-mcpu=cortex-m7 -mthumb -mfpu=fpv5-d16 -mfloat-abi=hard -fno-fast-math -ffp-contract=off -fno-math-errno -fno-exceptions -fno-rtti -O2 -std=c++17"
${A}g++ $FLAGS -c bsnum_arm.cpp -o /tmp/bsnum_arm.o
${A}g++ $FLAGS -I/b/dsp/include -c blob_runtime.cpp -o /tmp/blob_rt_arm.o
for o in /tmp/bsnum_arm.o /tmp/blob_rt_arm.o; do
  echo "$o FP-register instructions by mnemonic:"
  ${A}objdump -d --no-show-raw-insn $o | grep -E '^\s+[0-9a-f]+:\s+v[a-z]' | awk '{print $2}' | sort | uniq -c | tr '\n' ' '; echo
  ${A}objdump -d --no-show-raw-insn $o | grep -E '^\s+[0-9a-f]+:\s+v[a-z]' | grep -vE 'vmov|vldr|vstr|vpush|vpop' | head
done
