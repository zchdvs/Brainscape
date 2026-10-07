// The compiler audit's negative control (tools/ci/audit_compiler.py self-test): everything
// the audit bans from compiler/src, so its source ban and its import check must both reject
// this file. Build it with -O0 and no LTO, so the calls stay imports.
#include <cmath>
#include <cstdio>
#include <cstdlib>

float ReadFloat(const char* s) { return std::strtof(s, nullptr); }
double ReadDouble(const char* s) { return std::strtod(s, nullptr); }
float Curve(float x) { return std::pow(x, 1.5f) + std::sin(x); }
int WriteFloat(char* out, unsigned n, float x) { return std::snprintf(out, n, "%.9g", x); }
