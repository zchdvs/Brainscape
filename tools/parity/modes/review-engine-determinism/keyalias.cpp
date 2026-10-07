// Probe: design §7.5 R8 key extension, "XORs ext << 56 into the 64-bit key before the fold".
// DrawKey copied from dsp/src/detail/GrainMath.h:44-48 (r1), extended as the design states.
#include <cstdint>
#include <cstdio>
static uint32_t Hash32(uint32_t x){x+=0x9E3779B9u;x^=x>>16;x*=0x21F0AAADu;x^=x>>15;x*=0x735A2D97u;x^=x>>15;return x;}
static uint32_t DrawKeyExt(int64_t abs, uint32_t purpose, uint64_t ext){
  uint64_t k64 = (uint64_t)abs * 8u + (purpose & 7u);
  k64 ^= ext << 56;
  return (uint32_t)k64 ^ (uint32_t)(k64 >> 32);
}
int main(){
  // ext packing: bit0 layer, bits1-3 ordinal, bits4-5 purpose>>3 (any packing gives the same class of alias)
  uint64_t collisions[3] = {0,0,0}; uint64_t tested = 0;
  for (int64_t f = 0; f < (1<<29) - (1<<26); f += 4093) {
    for (uint32_t p = 0; p < 8; ++p) {
      ++tested;
      // layer 1 vs layer 0 at f ^ 2^21
      if (DrawKeyExt(f,p,1) == DrawKeyExt(f ^ (int64_t(1)<<21),p,0)) ++collisions[0];
      // ordinal 1 vs ordinal 0 at f ^ 2^22
      if (DrawKeyExt(f,p,2) == DrawKeyExt(f ^ (int64_t(1)<<22),p,0)) ++collisions[1];
      // purpose 8+p vs purpose p at f ^ 2^25
      if (DrawKeyExt(f,p,16) == DrawKeyExt(f ^ (int64_t(1)<<25),p,0)) ++collisions[2];
    }
  }
  std::printf("tested %llu (frame,purpose) pairs below 2^29 frames\n",(unsigned long long)tested);
  std::printf("layer1(f)        == layer0(f ^ 2^21 = +-43.69 s): %llu\n",(unsigned long long)collisions[0]);
  std::printf("ordinal1(f)      == ordinal0(f ^ 2^22 = +-87.38 s): %llu\n",(unsigned long long)collisions[1]);
  std::printf("purpose(8+p)(f)  == purpose p(f ^ 2^25 = +-11.65 min): %llu\n",(unsigned long long)collisions[2]);
  // same-frame distinctness (the property the design claims) still holds:
  uint64_t same=0; for (int64_t f=0; f<(1<<22); f+=7) for (uint32_t p=0;p<8;++p) for(uint64_t e=1;e<64;++e) if(DrawKeyExt(f,p,e)==DrawKeyExt(f,p,0)) ++same;
  std::printf("same-frame collisions ext!=0 vs ext==0: %llu\n",(unsigned long long)same);
  // r1 keys unchanged with ext == 0
  std::printf("example: RandUnit-key layer1 f=1000 %08x, layer0 f=1000^2^21 %08x\n", Hash32(DrawKeyExt(1000,1,1)), Hash32(DrawKeyExt(1000 ^ (1<<21),1,0)));
}
