#include "Text.h"

namespace bsc {

std::string Dec(uint64_t value) {
  char   buf[24];
  size_t n = 0;
  do {
    buf[n++] = static_cast<char>('0' + value % 10u);
    value /= 10u;
  } while (value != 0u);
  std::string out;
  out.reserve(n);
  while (n > 0) out.push_back(buf[--n]);
  return out;
}

std::string DecSigned(int64_t value) {
  if (value >= 0) return Dec(static_cast<uint64_t>(value));
  return "-" + Dec(~static_cast<uint64_t>(value) + 1u);
}

std::string Hex(const uint8_t* bytes, size_t length) {
  static const char kDigits[] = "0123456789abcdef";
  std::string       out;
  out.reserve(2 * length);
  for (size_t i = 0; i < length; ++i) {
    out.push_back(kDigits[bytes[i] >> 4]);
    out.push_back(kDigits[bytes[i] & 15u]);
  }
  return out;
}

bool ParseHex(std::string_view text, uint8_t* out, size_t length) {
  if (text.size() != 2 * length) return false;
  for (size_t i = 0; i < 2 * length; ++i) {
    const char c = text[i];
    uint8_t    d = 0;
    if (c >= '0' && c <= '9') {
      d = static_cast<uint8_t>(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      d = static_cast<uint8_t>(c - 'a' + 10);
    } else {
      return false;
    }
    if (i % 2u == 0u) {
      out[i / 2] = static_cast<uint8_t>(d << 4);
    } else {
      out[i / 2] = static_cast<uint8_t>(out[i / 2] | d);
    }
  }
  return true;
}

}  // namespace bsc
