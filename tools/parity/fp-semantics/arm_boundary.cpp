// Hardware-in-the-loop / QEMU check of the FZ tininess boundary on the Cortex-M7.
// Expected per Arm ARM (tininess BEFORE rounding): fz=1 -> 0x00000000 (UFC set), fz=0 -> 0x00800000.
// x86 FTZ|DAZ returns 0x00800000 for the same operands (fpprobe [1]).
#include <cstdint>
#include <cstring>
extern "C" uint32_t BoundaryMul(int fz, uint32_t* fpscrOut) {
  uint32_t saved, v;
  __asm__ __volatile__("vmrs %0, fpscr" : "=r"(saved));
  v = fz ? (1u << 24) : 0u;  // full word: RN, DN=0, flags cleared
  __asm__ __volatile__("vmsr fpscr, %0" ::"r"(v) : "memory");
  volatile uint32_t ua = 0x3F7FFFFEu, ub = 0x00800001u;
  float a, b; uint32_t x = ua, y = ub;
  std::memcpy(&a, &x, 4); std::memcpy(&b, &y, 4);
  float r;
  __asm__ __volatile__("vmul.f32 %0, %1, %2" : "=t"(r) : "t"(a), "t"(b));
  __asm__ __volatile__("vmrs %0, fpscr" : "=r"(*fpscrOut));
  __asm__ __volatile__("vmsr fpscr, %0" ::"r"(saved) : "memory");
  uint32_t out; std::memcpy(&out, &r, 4);
  return out;
}
