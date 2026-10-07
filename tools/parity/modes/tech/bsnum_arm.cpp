// PROBE: the in-house number code as a firmware-flag object (compile-only on the M7).
#include "bsnum.h"
extern "C" int BsWrite(float x, char* buf) { return bsnum::Write(x, buf); }
extern "C" int BsParse(const char* s, unsigned n, float* out) {
  size_t used = 0;
  return static_cast<int>(bsnum::Parse(s, n, out, &used));
}
