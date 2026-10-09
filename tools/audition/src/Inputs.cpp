#include "Inputs.h"

#include <algorithm>
#include <memory>
#include <vector>

namespace bsa {

namespace ts = brainscape::testsignal;

namespace {

// The vector's first `frames` frames (its notes inside `signalFrames`, silence after).
Stereo Generate(Vector v, uint32_t signalFrames, uint32_t frames) {
  const uint32_t        count = ts::BuildVector(v, signalFrames, nullptr, 0);
  std::vector<ts::Note> notes(count);
  ts::BuildVector(v, signalFrames, notes.data(), count);
  Stereo out;
  out.l.resize(frames);
  out.r.resize(frames);
  auto gen = std::make_unique<ts::Generator>();  // 64 KiB of string state: never a stack local
  gen->Start(notes.data(), count);
  for (uint32_t pos = 0; pos < frames;) {
    const uint32_t n = std::min<uint32_t>(4096, frames - pos);
    gen->Render(out.l.data() + pos, out.r.data() + pos, n);
    pos += n;
  }
  return out;
}

std::string Describe(Vector v) {
  return std::string("test signal: ") + ts::VectorName(v) + ", generator v" +
         std::to_string(ts::kVersion);
}

}  // namespace

Input VectorInput(Vector v, uint32_t signalFrames, uint32_t tailFrames) {
  Input in;
  in.name         = ts::VectorName(v);
  in.audio        = Generate(v, signalFrames, signalFrames + tailFrames);
  in.signalFrames = signalFrames;
  in.description  = Describe(v) + ", " + std::to_string(signalFrames) + " frames and " +
                   std::to_string(tailFrames) + " silent";
  return in;
}

Input LoopedInput(Vector v, uint32_t frames, uint32_t signalFrames, uint32_t tailFrames) {
  const Stereo one = Generate(v, signalFrames, signalFrames);
  Input        in;
  in.name = ts::VectorName(v);
  in.audio.l.resize(static_cast<size_t>(frames) + tailFrames, 0.f);
  in.audio.r.resize(static_cast<size_t>(frames) + tailFrames, 0.f);
  for (uint32_t i = 0; i < frames; ++i) {
    in.audio.l[i] = one.l[i % signalFrames];
    in.audio.r[i] = one.r[i % signalFrames];
  }
  in.signalFrames = frames;
  in.description  = Describe(v) + ", " + std::to_string(signalFrames) + " frames looped to " +
                   std::to_string(frames) + ", " + std::to_string(tailFrames) + " silent";
  return in;
}

bool VectorByName(const std::string& name, Vector* out) {
  for (const Vector v : kVectors) {
    if (name == ts::VectorName(v)) {
      *out = v;
      return true;
    }
  }
  return false;
}

}  // namespace bsa
