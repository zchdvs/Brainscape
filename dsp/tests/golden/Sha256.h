#pragma once
#include <cstdint>
#include <cstdio>
#include <string>

#include "brainscape/Sha256.h"

// The tests' SHA-256: the engine library's digest core (brainscape/Sha256.h, dsp/src/blob/;
// docs/design/mode-compiler.md §6.3) plus the hex formatting the harnesses print and compare,
// which needs snprintf and std::string and so stays out of the firmware archive.
namespace brainscape::golden {

class Sha256 : public brainscape::Sha256Hasher {
 public:
  // Finishes the digest as 64 lowercase hex digits and resets for reuse.
  std::string Hex() {
    uint8_t digest[kDigestBytes];
    Final(digest);
    return ToHex(digest, kDigestBytes);
  }

  static std::string ToHex(const uint8_t* bytes, size_t length) {
    std::string out(2 * length, '0');
    for (size_t i = 0; i < length; ++i) {
      char two[3];
      std::snprintf(two, sizeof two, "%02x", static_cast<unsigned>(bytes[i]));
      out[2 * i]     = two[0];
      out[2 * i + 1] = two[1];
    }
    return out;
  }
};

}  // namespace brainscape::golden
