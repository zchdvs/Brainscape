#include <charconv>
#include <cstddef>
// Does GCC 10.3 libstdc++ (arm-none-eabi) provide floating-point to_chars/from_chars?
size_t fmt(char* b, size_t n, float x) { auto r = std::to_chars(b, b + n, x); return r.ptr - b; }
float parse(const char* b, size_t n) { float x = 0; std::from_chars(b, b + n, x); return x; }
