#include "Hash.h"

#include <cstring>

#include "brainscape/Sha256.h"

namespace bsa {

using brainscape::Sha256Hasher;

std::string Hex(const uint8_t* bytes, size_t length) {
  static const char kDigits[] = "0123456789abcdef";
  std::string       out(length * 2, '0');
  for (size_t i = 0; i < length; ++i) {
    out[2 * i]     = kDigits[bytes[i] >> 4];
    out[2 * i + 1] = kDigits[bytes[i] & 15];
  }
  return out;
}

std::string Sha256Hex(const void* data, size_t length) {
  uint8_t d[Sha256Hasher::kDigestBytes];
  Sha256Hasher::Digest(data, length, d);
  return Hex(d, sizeof d);
}

std::string Sha256Hex(const std::string& text) { return Sha256Hex(text.data(), text.size()); }

RenderHashes HashRender(const std::vector<float>& l, const std::vector<float>& r) {
  RenderHashes h;
  Sha256Hasher whole, second;
  uint8_t      digest[Sha256Hasher::kDigestBytes];
  uint8_t      buf[8 * 512];
  size_t       used = 0;
  auto         flush = [&] {
    whole.Update(buf, used);
    second.Update(buf, used);
    used = 0;
  };
  const size_t frames = l.size() < r.size() ? l.size() : r.size();
  for (size_t i = 0; i < frames; ++i) {
    uint32_t u[2];
    std::memcpy(&u[0], &l[i], 4);
    std::memcpy(&u[1], &r[i], 4);
    for (int c = 0; c < 2; ++c) {
      for (int b = 0; b < 4; ++b) buf[used++] = static_cast<uint8_t>(u[c] >> (8 * b));
    }
    if (used == sizeof buf) flush();
    if ((i + 1) % kRate == 0 || i + 1 == frames) {
      flush();
      second.Final(digest);
      h.seconds.push_back(Hex(digest, sizeof digest));
    }
  }
  flush();
  whole.Final(digest);
  h.whole = Hex(digest, sizeof digest);
  return h;
}

}  // namespace bsa
