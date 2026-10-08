#include "Wav.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace bsa {

namespace {

void Put16(std::vector<uint8_t>& b, uint32_t v) {
  b.push_back(static_cast<uint8_t>(v));
  b.push_back(static_cast<uint8_t>(v >> 8));
}
void Put32(std::vector<uint8_t>& b, uint32_t v) {
  Put16(b, v & 0xFFFFu);
  Put16(b, v >> 16);
}
void PutTag(std::vector<uint8_t>& b, const char* tag) { b.insert(b.end(), tag, tag + 4); }

std::vector<uint8_t> Header(uint16_t format, uint16_t bits, size_t frames) {
  const auto           dataBytes = static_cast<uint32_t>(frames * 2u * (bits / 8u));
  std::vector<uint8_t> b;
  b.reserve(44 + dataBytes);
  PutTag(b, "RIFF");
  Put32(b, 36u + dataBytes);
  PutTag(b, "WAVE");
  PutTag(b, "fmt ");
  Put32(b, 16);
  Put16(b, format);  // 1 PCM, 3 IEEE float
  Put16(b, 2);
  Put32(b, kRate);
  Put32(b, kRate * 2u * (bits / 8u));
  Put16(b, static_cast<uint16_t>(2u * (bits / 8u)));
  Put16(b, bits);
  PutTag(b, "data");
  Put32(b, dataBytes);
  return b;
}

uint32_t Get32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 |
         static_cast<uint32_t>(p[2]) << 16 | static_cast<uint32_t>(p[3]) << 24;
}
uint16_t Get16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }

}  // namespace

int16_t ToPcm16(float x) {
  if (std::isnan(x)) return 0;
  if (x >= 1.0f) return 32767;
  if (x <= -1.0f) return -32768;
  const double v = static_cast<double>(x) * 32768.0;  // exact: a power-of-two scale
  const double r = v >= 0.0 ? std::floor(v + 0.5) : std::ceil(v - 0.5);  // exact below 2^52
  if (r > 32767.0) return 32767;
  if (r < -32768.0) return -32768;
  return static_cast<int16_t>(r);
}

std::vector<uint8_t> WavPcm16(const Stereo& s) {
  std::vector<uint8_t> b = Header(1, 16, s.Frames());
  for (size_t i = 0; i < s.Frames(); ++i) {
    Put16(b, static_cast<uint16_t>(ToPcm16(s.l[i])));
    Put16(b, static_cast<uint16_t>(ToPcm16(s.r[i])));
  }
  return b;
}

std::vector<uint8_t> WavFloat32(const Stereo& s) {
  std::vector<uint8_t> b = Header(3, 32, s.Frames());
  for (size_t i = 0; i < s.Frames(); ++i) {
    uint32_t u[2];
    std::memcpy(&u[0], &s.l[i], 4);
    std::memcpy(&u[1], &s.r[i], 4);
    Put32(b, u[0]);
    Put32(b, u[1]);
  }
  return b;
}

bool WriteFile(const std::string& path, const std::vector<uint8_t>& bytes) {
  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (f == nullptr) return false;
  const bool ok = bytes.empty() || std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
  return std::fclose(f) == 0 && ok;
}

bool WriteText(const std::string& path, const std::string& text) {
  return WriteFile(path, std::vector<uint8_t>(text.begin(), text.end()));
}

bool ReadWav(const std::vector<uint8_t>& b, Stereo* out, uint32_t* rate, uint16_t* bits) {
  if (b.size() < 44 || std::memcmp(b.data(), "RIFF", 4) != 0 ||
      std::memcmp(b.data() + 8, "WAVE", 4) != 0 || std::memcmp(b.data() + 12, "fmt ", 4) != 0 ||
      std::memcmp(b.data() + 36, "data", 4) != 0) {
    return false;
  }
  const uint16_t format   = Get16(b.data() + 20);
  const uint16_t channels = Get16(b.data() + 22);
  *rate                   = Get32(b.data() + 24);
  *bits                   = Get16(b.data() + 34);
  const uint32_t bytes    = Get32(b.data() + 40);
  if (channels != 2 || bytes > b.size() - 44 ||
      !((format == 1 && *bits == 16) || (format == 3 && *bits == 32))) {
    return false;
  }
  const size_t frames = bytes / (2u * (*bits / 8u));
  out->l.resize(frames);
  out->r.resize(frames);
  const uint8_t* p = b.data() + 44;
  for (size_t i = 0; i < frames; ++i) {
    for (int c = 0; c < 2; ++c) {
      float v;
      if (*bits == 16) {
        v = static_cast<float>(static_cast<int16_t>(Get16(p))) / 32768.0f;
        p += 2;
      } else {
        const uint32_t u = Get32(p);
        std::memcpy(&v, &u, 4);
        p += 4;
      }
      (c == 0 ? out->l : out->r)[i] = v;
    }
  }
  return true;
}

}  // namespace bsa
