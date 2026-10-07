#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "Render.h"

// Render hashes (determinism profile §6.1; companion §7.4): SHA-256 of the interleaved
// little-endian float32 frames, the golden harness's byte stream, whole and per 1 s segment (the
// last one short), so two renders' first differing second can be named.
namespace bsa {

struct RenderHashes {
  std::string              whole;    // 64 lowercase hex digits
  std::vector<std::string> seconds;  // one per 48,000 frames, the last one short
};

RenderHashes HashRender(const std::vector<float>& l, const std::vector<float>& r);
inline RenderHashes HashRender(const Stereo& s) { return HashRender(s.l, s.r); }

// SHA-256 of bytes, as lowercase hex.
std::string Sha256Hex(const void* data, size_t length);
std::string Sha256Hex(const std::string& text);
std::string Hex(const uint8_t* bytes, size_t length);

}  // namespace bsa
