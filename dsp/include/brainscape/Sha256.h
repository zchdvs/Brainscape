#pragma once
#include <cstddef>
#include <cstdint>

#include "brainscape/FpProfile.h"

namespace brainscape {

// FIPS 180-4 SHA-256 (docs/design/mode-compiler.md §6.3): integer-only and fed bytes, so the
// digest of a byte stream is the same on every target. The package's sound_hash,
// control_hash, package_hash and modeHash are SHA-256, and the firmware links this digest
// core with the decoder (dsp/src/blob/). Hex formatting stays with the tests
// (dsp/tests/golden/Sha256.h), which hash renders with it.
class Sha256Hasher {
 public:
  static constexpr size_t kDigestBytes = 32;

  Sha256Hasher() noexcept { Reset(); }

  void Reset() noexcept;
  void Update(const void* data, size_t length) noexcept;
  // Writes the digest of everything fed since the last Reset, then resets for reuse.
  void Final(uint8_t digest[kDigestBytes]) noexcept;

  // The digest of one buffer.
  static void Digest(const void* data, size_t length, uint8_t digest[kDigestBytes]) noexcept;

 private:
  void Block(const uint8_t* block) noexcept;

  uint32_t h_[8];
  uint8_t  buffer_[64];
  uint64_t bytes_;
  uint32_t used_;
};

}  // namespace brainscape
