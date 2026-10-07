#pragma once
#include <cstddef>
#include <cstdint>

// Exact binary32 text, in integers only (docs/design/mode-compiler.md §6.4, §6.5): the reader
// and the shortest-digit writer of canonical JSON, and the lenient reader of the plugin's typed
// text. No floating-point arithmetic, no allocation, no C library beyond memcpy, no table to
// generate and no third-party code (owner question Q1: no vendored fast_float), so every host
// reads and writes the same bits and characters. A 640-bit integer of 32-bit limbs carries the
// exact arithmetic.
namespace bsc {

enum class NumberStatus : uint8_t {
  Ok,
  Syntax,  // not a number of the grammar
  Range,   // the magnitude rounds past FLT_MAX: an error, never infinity
};

// RFC 8259's number grammar, -?(0|[1-9][0-9]*)(.[0-9]+)?([eE][+-]?[0-9]+)?, read as the
// correctly rounded binary32 (round half to even, at any number of digits; gradual underflow,
// so tiny values round to a subnormal or to a signed zero). With `used` null the whole text
// must be the number; otherwise the longest prefix that is one is read and *used is its length
// (a JSON tokenizer's call). `-0` reads as -0: canonicalization, not the reader, makes it +0.
NumberStatus ParseJsonNumber(const char* text, size_t length, float* out,
                             size_t* used = nullptr) noexcept;

// The plugin's typed text, looser than JSON with the same exact rounding (§6.5): an optional
// + or -, leading zeros, and a point with digits on either side or both ("05", "+25", ".5",
// "5.", "5.e3"), then an optional exponent. The whole text must match.
NumberStatus ParseTypedNumber(const char* text, size_t length, float* out) noexcept;

// The canonical text of a finite float (§6.4): the shortest decimal that reads back to the same
// binary32 under correct rounding, the closest such and the even digit on a tie (std::to_chars
// without precision), laid out by ECMAScript's Number::toString ("0.55", "375", "1e-7",
// "3.4028235e+38"); -0 is written "-0" so every float round-trips. One exception: 0x15AE43FD
// and 0x95AE43FD, whose shortest text read through binary64 rounds to the neighbouring float,
// are written with eight digits, "7.0385307e-26" and "-7.0385307e-26", which read back right
// both ways (record §2.3). Returns the length (at most kMaxNumberText, no terminator written);
// 0 for NaN and infinities, which canonical JSON never holds.
inline constexpr size_t kMaxNumberText = 32;
size_t WriteNumber(float value, char* out) noexcept;
size_t WriteNumberBits(uint32_t bits, char* out) noexcept;

// The pieces, for the tests: the shortest digits of a finite nonzero |value| (d1 d2 ... dn,
// value = 0.d1d2...dn * 10^(exponent + 1)), and their ECMAScript layout.
struct ShortestDigits {
  char digits[10] = {};
  int  count      = 0;
  int  exponent   = 0;  // the decimal exponent of the first digit
};
ShortestDigits Shortest(uint32_t bits) noexcept;
size_t         LayoutNumber(bool negative, const ShortestDigits& digits, char* out) noexcept;

}  // namespace bsc
