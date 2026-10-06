import sys
p=sys.argv[1]+'/src/Granular.cpp'
s=open(p,encoding='utf-8',newline='').read()
def sub(a,b):
    global s
    assert s.count(a)==1,a[:50]; s=s.replace(a,b)
sub('#include <cmath>\n','#include <cmath>\n#include <cstdio>\n'
 'unsigned long long g_bcAheadReads = 0, g_bcAheadGrains = 0;\n'
 'static uint32_t g_bcRingStart = 0;\n'
 'static inline bool BcAhead(uint32_t tap, uint32_t live, uint32_t mask) {\n'
 '  const uint32_t a = (tap - live) & mask;\n  return a >= 1u && a <= 512u;\n}\n')
sub('''    if (s < endN) {
      uint64_t pos   = g.pos;''','''    if (s < endN) {
      {
        bool hit = false;
        uint64_t pp = g.pos;
        const int tlo = g.unity ? 0 : (g.tier == 0 ? -1 : 0);
        const int thi = g.unity ? 0 : (g.tier == 0 ? 2 : 1);
        for (uint32_t n = s; n < endN; ++n) {
          const uint32_t f = static_cast<uint32_t>(pp >> 32);
          const uint32_t live = (g_bcRingStart + n) & mask_;
          bool a = false;
          for (int t = tlo; t <= thi; ++t) a = a || BcAhead((f + static_cast<uint32_t>(t)) & mask_, live, mask_);
          if (a) { ++g_bcAheadReads; hit = true; }
          pp = static_cast<uint64_t>(static_cast<int64_t>(pp) + g.inc);
        }
        if (hit) ++g_bcAheadGrains;
      }
      uint64_t pos   = g.pos;''')
sub('''  for (uint32_t n = 0; n < numFrames; ++n) {
    wetL[n] = 0.f;''','''  g_bcRingStart = ringFrameAtBlockStart;
  for (uint32_t n = 0; n < numFrames; ++n) {
    wetL[n] = 0.f;''')
open(p,'w',encoding='utf-8',newline='').write(s)
