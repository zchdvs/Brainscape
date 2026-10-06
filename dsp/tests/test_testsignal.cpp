#include <cstring>
#include <vector>

#include "brainscape/TestSignal.h"
#include "catch.hpp"

using namespace brainscape::testsignal;

namespace {

std::vector<Note> Score(Vector v, uint32_t activeFrames) {
  std::vector<Note> notes(BuildVector(v, activeFrames, nullptr, 0));
  REQUIRE(BuildVector(v, activeFrames, notes.data(), static_cast<uint32_t>(notes.size())) ==
          notes.size());
  return notes;
}

struct Stream {
  std::vector<int32_t> l, r;
};

// Renders `frames` frames in blocks cycling through `blocks`, clamped to what remains.
Stream RenderQ23(const std::vector<Note>& notes, uint32_t frames,
                 const std::vector<uint32_t>& blocks) {
  Generator* gen = new Generator();
  gen->Start(notes.data(), static_cast<uint32_t>(notes.size()));
  Stream s;
  s.l.resize(frames);
  s.r.resize(frames);
  for (uint32_t pos = 0, i = 0; pos < frames; ++i) {
    const uint32_t want = blocks[i % blocks.size()];
    const uint32_t n    = want < frames - pos ? want : frames - pos;
    gen->RenderQ23(s.l.data() + pos, s.r.data() + pos, n);
    pos += n;
  }
  delete gen;
  return s;
}

// FNV-1a 64 over the little-endian bytes of each frame's L and R samples.
uint64_t Fnv(const Stream& s) {
  uint64_t h = 0xCBF29CE484222325ull;
  for (size_t i = 0; i < s.l.size(); ++i) {
    for (const int32_t q : {s.l[i], s.r[i]}) {
      const auto u = static_cast<uint32_t>(q);
      for (int b = 0; b < 4; ++b) {
        h ^= static_cast<uint8_t>(u >> (8 * b));
        h *= 0x100000001B3ull;
      }
    }
  }
  return h;
}

const Vector kAllVectors[] = {Vector::Plucks,      Vector::Strums,     Vector::SoftNotes,
                              Vector::OnsetBursts, Vector::Saturation, Vector::Silence};

}  // namespace

TEST_CASE("SplitMix32 is the reference SplitMix32 stream, random access") {
  // State 0's first outputs from the stepping form: state += 0x9E3779B9; fmix32(state).
  REQUIRE(SplitMix32(0u, 0u) == 0x92CA2F0Eu);
  REQUIRE(SplitMix32(0u, 1u) == 0x3CD6E3F3u);
  REQUIRE(SplitMix32(0u, 2u) == 0x1B147DCCu);
  REQUIRE(SplitMix32(0u, 3u) == 0x4C081DBFu);
  REQUIRE(SplitMix32(0x12345678u, 0u) == 0x047660EAu);
  REQUIRE(SplitMix32(0xFFFFFFFFu, 0xFFFFFFFFu) == 0x81F16F39u);
  uint32_t state = 0xDEADBEEFu;
  for (uint32_t n = 0; n < 1000; ++n) {
    state += 0x9E3779B9u;
    REQUIRE(SplitMix32(0xDEADBEEFu, n) == Mix32(state));
  }
  static_assert(SplitMix32(0u, 0u) == 0x92CA2F0Eu, "usable in constant expressions");
}

TEST_CASE("Noise24 covers the signed 24-bit range") {
  int32_t lo = 0, hi = 0;
  int64_t sum = 0;
  for (uint32_t n = 0; n < 200000; ++n) {
    const int32_t q = Noise24(0x51u, n);
    REQUIRE(q >= kQ23Min);
    REQUIRE(q <= kQ23Max);
    lo = q < lo ? q : lo;
    hi = q > hi ? q : hi;
    sum += q;
  }
  REQUIRE(lo < -8300000);
  REQUIRE(hi > 8300000);
  REQUIRE(sum / 200000 > -40000);  // mean within 0.5 % of full scale
  REQUIRE(sum / 200000 < 40000);
}

TEST_CASE("FloorShift is floor division by a power of two") {
  for (int64_t v = -70000; v <= 70000; v += 7) {
    for (unsigned s = 0; s < 20; ++s) {
      const int64_t d     = int64_t{1} << s;
      const int64_t floor = (v - (((v % d) + d) % d)) / d;
      REQUIRE(FloorShift(v, s) == floor);
    }
  }
  REQUIRE(FloorShift(-1, 30) == -1);
  REQUIRE(FloorShift(INT64_MIN + 1, 62) == -2);
}

TEST_CASE("Q23ToFloat is exact on the whole 24-bit grid") {
  uint32_t wrong = 0;
  for (int32_t q = kQ23Min; q <= kQ23Max; ++q) {
    // x * 2^23 is exact too, so the round trip must give q back.
    if (static_cast<int32_t>(Q23ToFloat(q) * 8388608.0f) != q) ++wrong;
  }
  REQUIRE(wrong == 0u);
  REQUIRE(Q23ToFloat(kQ23Min) == -1.0f);
  REQUIRE(Q23ToFloat(kQ23Max) == 1.0f - 0x1p-23f);
  REQUIRE(Q23ToFloat(1) == 0x1p-23f);
  uint32_t bits;
  const float zero = Q23ToFloat(0);
  std::memcpy(&bits, &zero, 4);
  REQUIRE(bits == 0u);  // +0, never -0
}

TEST_CASE("standard vectors match the independent reference implementation") {
  // FNV-1a 64 of the first 10 s of each vector (activeFrames = 10 s), computed by a
  // separate Python implementation of the generator spec. A change here means a
  // different input signal: bump kVersion.
  struct Kat {
    Vector   v;
    size_t   notes;
    uint64_t fnv;
  };
  const Kat kats[] = {
      {Vector::Plucks, 20, 0x65F20BC884FF4529ull},
      {Vector::Strums, 30, 0x885E42212E6BDDD2ull},
      {Vector::SoftNotes, 15, 0x9F565079F64ACFEAull},
      {Vector::OnsetBursts, 85, 0xB5CE56E4232F54EBull},
      {Vector::Saturation, 40, 0xC22ECEED2BFF228Eull},
      {Vector::Silence, 0, 0x8E8E7E9ABD9A0325ull},
  };
  STATIC_REQUIRE(kVersion == 1);
  for (const Kat& k : kats) {
    INFO(VectorName(k.v));
    const std::vector<Note> notes = Score(k.v, 480000);
    REQUIRE(notes.size() == k.notes);
    REQUIRE(Fnv(RenderQ23(notes, 480000, {512})) == k.fnv);
  }
}

TEST_CASE("rendering does not depend on the block split") {
  for (const Vector v : kAllVectors) {
    INFO(VectorName(v));
    const std::vector<Note> notes = Score(v, 192000);
    const Stream ref = RenderQ23(notes, 200000, {1});
    const std::vector<std::vector<uint32_t>> splits = {{7}, {48}, {512}, {48, 1, 127, 32}};
    for (const auto& blocks : splits) {
      const Stream s = RenderQ23(notes, 200000, blocks);
      REQUIRE(s.l == ref.l);
      REQUIRE(s.r == ref.r);
    }
  }
}

TEST_CASE("Start rewinds to an identical stream; float output is the Q23 stream scaled") {
  const std::vector<Note> notes = Score(Vector::Strums, 192000);
  Generator* gen = new Generator();
  std::vector<float> l1(100000), r1(100000), l2(100000), r2(100000);
  gen->Start(notes.data(), static_cast<uint32_t>(notes.size()));
  gen->Render(l1.data(), r1.data(), 100000);
  REQUIRE(gen->Frame() == 100000u);
  gen->Start(notes.data(), static_cast<uint32_t>(notes.size()));
  gen->Render(l2.data(), r2.data(), 100000);
  delete gen;
  REQUIRE(std::memcmp(l1.data(), l2.data(), l1.size() * sizeof(float)) == 0);
  REQUIRE(std::memcmp(r1.data(), r2.data(), r1.size() * sizeof(float)) == 0);
  const Stream q = RenderQ23(notes, 100000, {512});
  for (size_t i = 0; i < q.l.size(); ++i) {
    REQUIRE(l1[i] == Q23ToFloat(q.l[i]));
    REQUIRE(r1[i] == Q23ToFloat(q.r[i]));
  }
}

TEST_CASE("vectors stay inside their active span") {
  for (const Vector v : kAllVectors) {
    INFO(VectorName(v));
    const uint32_t active = 300000;
    const std::vector<Note> notes = Score(v, active);
    for (size_t i = 0; i < notes.size(); ++i) {
      REQUIRE(notes[i].start + notes[i].length <= active);
      if (i > 0) REQUIRE(notes[i - 1].start <= notes[i].start);
    }
    const Stream s = RenderQ23(notes, active + 9600, {512});
    for (uint32_t f = active; f < active + 9600; ++f) {
      REQUIRE(s.l[f] == 0);
      REQUIRE(s.r[f] == 0);
    }
  }
}

TEST_CASE("Saturation clips at full scale, Silence is exactly zero") {
  const Stream sat = RenderQ23(Score(Vector::Saturation, 96000), 96000, {48});
  uint32_t clipped = 0;
  for (size_t i = 0; i < sat.l.size(); ++i) {
    if (sat.l[i] == kQ23Max || sat.l[i] == kQ23Min) ++clipped;
  }
  REQUIRE(clipped > 10000u);
  const Stream quiet = RenderQ23(Score(Vector::Silence, 96000), 96000, {48});
  for (size_t i = 0; i < quiet.l.size(); ++i) {
    REQUIRE(quiet.l[i] == 0);
    REQUIRE(quiet.r[i] == 0);
  }
}

TEST_CASE("a seventeenth overlapping note steals the oldest voice") {
  std::vector<Note> notes;
  for (uint32_t i = 0; i < 17; ++i) {
    Note n;
    n.kind   = Kind::Tone;
    n.start  = i * 10;
    n.length = 48000;
    n.level  = 1 << 18;
    n.pitch  = 100000 + 10000 * i;
    notes.push_back(n);
  }
  std::vector<Note> without = notes;
  without[0].length = 160;  // ends exactly where the 17th note steals its voice
  const Stream a = RenderQ23(notes, 4800, {48});
  const Stream b = RenderQ23(without, 4800, {48});
  REQUIRE(a.l == b.l);
  REQUIRE(a.r == b.r);
}
