#include <cstdint>
#include <cstdio>
#include <cstring>
float EngineSide(float, float, float);
float PluginSide(float, float, float);
int main() {
  volatile float a = 1.0f + 1.0f / 8388608.0f, b = a, c = -(1.0f + 2.0f / 8388608.0f);
  float e = EngineSide(a, b, c), p = PluginSide(a, b, c);
  uint32_t ue, up; std::memcpy(&ue, &e, 4); std::memcpy(&up, &p, 4);
  std::printf("engine-TU call -> 0x%08X (%s)   plugin-TU call -> 0x%08X\n", ue, ue ? "FUSED copy" : "unfused copy", up);
}
