#include <cstdint>
extern "C" {
int16_t c_i16(float x) { return static_cast<int16_t>(x); }
uint32_t c_u32(float x) { return static_cast<uint32_t>(x); }
int32_t c_i32(float x) { return static_cast<int32_t>(x); }
int64_t c_i64(double x) { return static_cast<int64_t>(x); }
float c_u32f(uint32_t u) { return static_cast<float>(u); }
}
