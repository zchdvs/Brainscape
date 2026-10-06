// Probe: the companion design's BrainscapeParam computes the normalised value in double
// (NormalizedFromPlain) but JUCE's AudioProcessorParameter::getValue()/setValue() carry it
// as float (juce_AudioProcessorParameter.h:113,129). Any host-originated set (automation,
// host undo, AU/CLAP/LV2 parameter replay) therefore restores
//   plain' = (float) PlainFromNormalized((float) NormalizedFromPlain(plain)).
// Linear taper, double arithmetic, no contraction (MSVC /fp:precise).
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <cstring>
struct R { const char* name; float s, e; };
static float n_of(float p, float s, float e) { double n = ((double)p - s) / ((double)e - s); return (float)n; }
static float p_of(float n, float s, float e) { double p = (double)s + ((double)e - s) * (double)n; return (float)p; }
int main() {
  const R rs[] = {{"[1,5000]",1,5000},{"[0,1]",0,1},{"[0,1.1]",0,1.1f},{"[-24,24]",-24,24},{"[1,500]",1,500},
                  {"[0,2000]",0,2000},{"[0,100]",0,100},{"[0.01,10]",0.01f,10},{"[10,2000]",10,2000},
                  {"[0,0.9]",0,0.9f},{"[40,20000]",40,20000},{"[0,3]",0,3}};
  printf("%-12s %12s %12s %8s | %14s %14s %8s\n","range","3dec_vals","3dec_fail","pct","all_floats","all_fail","pct");
  for (const R& r : rs) {
    // 3-decimal authored grid
    uint64_t n3 = 0, f3 = 0;
    const long lo = lround((double)r.s * 1000.0), hi = lround((double)r.e * 1000.0);
    for (long k = lo; k <= hi; ++k) {
      float p = (float)((double)k / 1000.0);
      if (p < r.s || p > r.e) continue;
      ++n3; if (p_of(n_of(p, r.s, r.e), r.s, r.e) != p) ++f3;
    }
    // every float in range
    uint64_t na = 0, fa = 0;
    auto run = [&](float a, float b) {   // a<=b, same sign, non-negative walk on bits
      uint32_t ua, ub; memcpy(&ua,&a,4); memcpy(&ub,&b,4);
      for (uint64_t u = ua; u <= ub; ++u) { uint32_t w=(uint32_t)u; float p; memcpy(&p,&w,4);
        ++na; if (p_of(n_of(p, r.s, r.e), r.s, r.e) != p) ++fa; }
    };
    if (r.s >= 0) run(r.s, r.e);
    else { run(0.0f, r.e); // positive half incl +0
           uint32_t ua, ub; float a = -0.0f, b = r.s; memcpy(&ua,&a,4); memcpy(&ub,&b,4);
           for (uint64_t u = ua + 1; u <= ub; ++u) { uint32_t w=(uint32_t)u; float p; memcpy(&p,&w,4);
             ++na; if (p_of(n_of(p, r.s, r.e), r.s, r.e) != p) ++fa; } }
    printf("%-12s %12llu %12llu %7.2f%% | %14llu %14llu %7.2f%%\n", r.name,
           (unsigned long long)n3,(unsigned long long)f3, n3?100.0*f3/n3:0.0,
           (unsigned long long)na,(unsigned long long)fa, na?100.0*fa/na:0.0);
  }
}
