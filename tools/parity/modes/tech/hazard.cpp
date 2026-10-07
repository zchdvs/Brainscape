// PROBE: why nothing is memcpy'd from a struct: sizes that differ between x64 and the M7.
#include <cstddef>
#include <cstdint>
enum Plain { A, B };                // no fixed underlying type
enum class Fixed : uint8_t { X };
struct Naive { bool on; Plain kind; size_t count; long big; float f; void* p; };
extern "C" const unsigned kSizes[] = {sizeof(Plain), sizeof(bool), sizeof(size_t), sizeof(long),
                                      sizeof(Naive), alignof(Naive), offsetof(Naive, f),
                                      sizeof(long double), sizeof(wchar_t), sizeof(Fixed)};
#if defined(HOST_MAIN)
#include <cstdio>
int main() {
  const char* n[] = {"enum", "bool", "size_t", "long", "Naive", "alignof(Naive)", "offsetof(Naive,f)", "long double", "wchar_t", "enum:uint8_t"};
  for (int i = 0; i < 10; ++i) std::printf("%s=%u ", n[i], kSizes[i]);
  std::printf("\n");
}
#endif
