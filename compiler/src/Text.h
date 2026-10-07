#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// Integer text helpers for the compiler (docs/design/mode-compiler.md §8.3): decimal and hex
// written with integer arithmetic only, so no printf family, stream number formatting or
// std::to_string enters compiler/ (the compiler audit bans them) and no locale is consulted.
namespace bsc {

std::string Dec(uint64_t value);
std::string DecSigned(int64_t value);
// Lowercase hex, two digits per byte.
std::string Hex(const uint8_t* bytes, size_t length);
// 2 * length lowercase hex digits into `out`; false on any other character or length.
bool ParseHex(std::string_view text, uint8_t* out, size_t length);

}  // namespace bsc
