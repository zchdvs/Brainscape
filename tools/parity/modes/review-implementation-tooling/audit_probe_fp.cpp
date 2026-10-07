// A "compiler object" that formats and parses floats through the standard library.
#include <charconv>
#include <cstdio>
#include <string>
int WriteFloat(float x, char* b, int n) { auto r = std::to_chars(b, b + n, x); return int(r.ptr - b); }
float ReadFloat(const char* b, int n) { float v = 0; std::from_chars(b, b + n, v); return v; }
int PrintFloat(char* b, float x) { return std::snprintf(b, 32, "%g", (double)x); }
std::string StrFloat(float x) { return std::to_string(x); }
