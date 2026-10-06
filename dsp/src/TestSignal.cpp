#include "brainscape/TestSignal.h"

namespace brainscape::testsignal {

namespace {

constexpr int64_t  kUnityQ16  = 65536;
constexpr int64_t  kPluckLoss = 65280;  // Q16 gain per string pass on top of the 2-tap average
constexpr uint32_t kFs        = 1u << 23;

// sin(2*pi*phase/2^32) in Q30 from the refined parabola y + 0.225*(y|y| - y) with
// y = 4x(1 - |x|); about 0.1 % harmonic distortion, which a test tone tolerates.
int64_t SineQ30(uint32_t phase) noexcept {
  const int64_t p  = phase < 0x80000000u ? static_cast<int64_t>(phase)
                                         : static_cast<int64_t>(phase) - (int64_t{1} << 32);
  const int64_t x  = FloorShift(p, 1);  // Q30, [-1, 1)
  const int64_t ax = x < 0 ? -x : x;
  const int64_t y  = FloorShift(4 * x * ((int64_t{1} << 30) - ax), 30);
  const int64_t ay = y < 0 ? -y : y;
  const int64_t y2 = FloorShift(y * ay, 30);
  return y + (225 * (y2 - y)) / 1000;  // truncating division, as specified
}

// Linear fade-in over `attack` frames and fade-out over the last `release`, Q16.
int64_t EnvelopeQ16(const Note& n, uint32_t age) noexcept {
  int64_t env = kUnityQ16;
  if (age < n.attack) env = kUnityQ16 * age / n.attack;
  const uint32_t remaining = n.length - age;  // >= 1 while the note sounds
  if (remaining <= n.release) {
    const int64_t r = kUnityQ16 * remaining / n.release;
    if (r < env) env = r;
  }
  return env;
}

uint32_t Clamp(uint32_t v, uint32_t lo, uint32_t hi) noexcept {
  return v < lo ? lo : (v > hi ? hi : v);
}

// Appends while there is room; always counts, so a null/short buffer sizes the score.
struct ScoreWriter {
  Note*    out;
  uint32_t capacity;
  uint32_t count = 0;
  void Add(const Note& n) noexcept {
    if (count < capacity) out[count] = n;
    ++count;
  }
};

constexpr uint32_t kPluckPeriods[] = {582, 436, 327, 245, 194, 146, 109, 97, 73, 65};

// Chord shapes as string periods, low to high: E major, A minor, G major.
constexpr uint32_t kChords[3][6] = {
    {582, 389, 291, 231, 194, 146},
    {582, 436, 291, 218, 183, 146},
    {490, 389, 327, 245, 194, 122},
};

constexpr uint32_t kScaleMilliHz[] = {196000, 220000, 246942, 293665, 329628, 391995, 440000};

void BuildPlucks(uint32_t active, ScoreWriter& w) noexcept {
  const uint32_t length = 21600;  // 0.45 s of a 0.5 s slot
  for (uint32_t k = 0, start = 1200; start + length <= active; ++k, start += 24000) {
    Note n;
    n.kind    = Kind::Pluck;
    n.start   = start;
    n.length  = length;
    n.level   = static_cast<int32_t>((3u << 20) + (SplitMix32(0xB5u, k) >> 11));  // 0.375-0.625 FS
    n.pitch   = kPluckPeriods[(k * 7u) % 10u];
    n.seed    = 0x1000u + k;
    n.release = 960;
    n.panL    = static_cast<uint16_t>((k & 1u) ? 32768u : 19661u);
    n.panR    = static_cast<uint16_t>((k & 1u) ? 19661u : 32768u);
    w.Add(n);
  }
}

void BuildStrums(uint32_t active, ScoreWriter& w) noexcept {
  const uint32_t stagger = 576, length = 86400;
  for (uint32_t k = 0, start = 2400; start + 5u * stagger + length <= active;
       ++k, start += 96000) {
    const uint32_t* chord = kChords[k % 3u];
    const bool      up    = (k & 1u) != 0;  // upstrokes run high to low
    for (uint32_t s = 0; s < 6; ++s) {
      const uint32_t str = up ? 5u - s : s;
      Note n;
      n.kind    = Kind::Pluck;
      n.start   = start + s * stagger;
      n.length  = length;
      n.level   = static_cast<int32_t>(kFs * 3u / 10u);
      n.pitch   = chord[str];
      n.seed    = 0x2000u + 8u * k + str;
      n.release = 2400;
      n.panL    = static_cast<uint16_t>(32768u - str * 3000u);
      n.panR    = static_cast<uint16_t>(17768u + str * 3000u);
      w.Add(n);
    }
  }
}

void BuildSoftNotes(uint32_t active, ScoreWriter& w) noexcept {
  const uint32_t length = 57600;  // 1.2 s every 0.6 s: always two notes overlapping
  for (uint32_t k = 0, start = 0; start + length <= active; ++k, start += 28800) {
    Note n;
    n.kind    = Kind::Tone;
    n.start   = start;
    n.length  = length;
    n.level   = static_cast<int32_t>(kFs / 4u);
    n.pitch   = kScaleMilliHz[(k * 3u) % 7u];
    n.attack  = 2880;
    n.release = 14400;
    n.panL    = static_cast<uint16_t>((k & 1u) ? 32768u : 24576u);
    n.panR    = static_cast<uint16_t>((k & 1u) ? 24576u : 32768u);
    w.Add(n);
  }
}

void BuildOnsetBursts(uint32_t active, ScoreWriter& w) noexcept {
  uint32_t start = 2400;
  for (uint32_t k = 0;; ++k) {
    Note n;
    n.kind    = Kind::Noise;
    n.start   = start;
    n.length  = 576u + SplitMix32(0x0Cu, k) % 384u;  // 12-20 ms
    if (start + n.length > active) break;
    n.level   = static_cast<int32_t>(kFs / 4u + SplitMix32(0x0Du, k) % (kFs / 2u));
    n.seed    = 0x5000u + k;
    n.attack  = 48;
    n.release = 96;
    const uint32_t pan = SplitMix32(0x0Eu, k) % 16384u;
    n.panL    = static_cast<uint16_t>(16384u + pan);
    n.panR    = static_cast<uint16_t>(32768u - pan);
    w.Add(n);
    start += 4320u + SplitMix32(0x0Bu, k) % 2880u;  // 90-150 ms apart
  }
}

void BuildSaturation(uint32_t active, ScoreWriter& w) noexcept {
  const uint32_t length = 38400;
  const uint32_t chord[3] = {110000, 164814, 220000};
  for (uint32_t k = 0, start = 960; start + length <= active; ++k, start += 48000) {
    Note burst;
    burst.kind    = Kind::Noise;
    burst.start   = start;
    burst.length  = 1440;
    burst.level   = static_cast<int32_t>(3u * kFs);
    burst.seed    = 0x6000u + k;
    burst.release = 240;
    w.Add(burst);
    for (uint32_t t = 0; t < 3; ++t) {
      Note n;
      n.kind    = Kind::Tone;
      n.start   = start;
      n.length  = length;
      n.level   = static_cast<int32_t>(kFs + kFs / 2u + kFs / 10u);  // 1.6 FS each
      n.pitch   = chord[t];
      n.attack  = 480;
      n.release = 2400;
      n.panL    = static_cast<uint16_t>(32768u - t * 6000u);
      n.panR    = static_cast<uint16_t>(20768u + t * 6000u);
      w.Add(n);
    }
  }
}

}  // namespace

float Q23ToFloat(int32_t q) noexcept { return static_cast<float>(q) * 0x1p-23f; }

void Generator::Start(const Note* notes, uint32_t count) noexcept {
  notes_ = notes;
  count_ = notes != nullptr ? count : 0u;
  next_  = 0;
  frame_ = 0;
  for (Voice& v : voices_) v.note = nullptr;
}

void Generator::Begin(Voice& v, const Note& n) noexcept {
  v.note  = &n;
  v.age   = 0;
  v.phase = 0;
  v.inc   = static_cast<uint32_t>((static_cast<uint64_t>(n.pitch) << 32) /
                                  (uint64_t{kSampleRate} * 1000u));
  v.period = Clamp(n.pitch, 2u, kMaxPeriod);
  v.pos    = 0;
  if (n.kind == Kind::Pluck) {
    for (uint32_t i = 0; i < v.period; ++i) {
      v.ks[i] = static_cast<int32_t>(
          FloorShift(static_cast<int64_t>(Noise24(n.seed, i)) * n.level, 23));
    }
  }
}

int32_t Generator::Next(Voice& v) noexcept {
  const Note& n = *v.note;
  int64_t     s = 0;
  switch (n.kind) {
    case Kind::Noise:
      s = FloorShift(static_cast<int64_t>(Noise24(n.seed, v.age)) * n.level, 23);
      break;
    case Kind::Pluck: {
      const int32_t  y  = v.ks[v.pos];
      const uint32_t nx = v.pos + 1u == v.period ? 0u : v.pos + 1u;
      v.ks[v.pos] = static_cast<int32_t>(
          FloorShift((static_cast<int64_t>(y) + v.ks[nx]) * kPluckLoss, 17));
      v.pos = nx;
      s     = y;
      break;
    }
    case Kind::Tone:
      s = FloorShift(SineQ30(v.phase) * n.level, 30);
      v.phase += v.inc;
      break;
  }
  return static_cast<int32_t>(FloorShift(s * EnvelopeQ16(n, v.age), 16));
}

void Generator::RenderQ23(int32_t* left, int32_t* right, uint32_t n) noexcept {
  for (uint32_t i = 0; i < n; ++i, ++frame_) {
    while (next_ < count_ && notes_[next_].start <= frame_) {
      const Note& note = notes_[next_++];
      if (note.length == 0) continue;
      Voice* slot = nullptr;
      for (Voice& v : voices_) {
        if (v.note == nullptr) { slot = &v; break; }
        if (slot == nullptr || v.age > slot->age) slot = &v;  // steal the oldest
      }
      Begin(*slot, note);
    }
    int64_t l = 0, r = 0;
    for (Voice& v : voices_) {
      if (v.note == nullptr) continue;
      const int64_t s = Next(v);
      l += FloorShift(s * v.note->panL, 15);
      r += FloorShift(s * v.note->panR, 15);
      if (++v.age >= v.note->length) v.note = nullptr;
    }
    left[i]  = SaturateQ23(l);
    right[i] = SaturateQ23(r);
  }
}

void Generator::Render(float* left, float* right, uint32_t n) noexcept {
  constexpr uint32_t kChunk = 128;
  int32_t ql[kChunk], qr[kChunk];
  for (uint32_t done = 0; done < n;) {
    const uint32_t m = n - done < kChunk ? n - done : kChunk;
    RenderQ23(ql, qr, m);
    for (uint32_t i = 0; i < m; ++i) {
      left[done + i]  = Q23ToFloat(ql[i]);
      right[done + i] = Q23ToFloat(qr[i]);
    }
    done += m;
  }
}

const char* VectorName(Vector v) noexcept {
  switch (v) {
    case Vector::Plucks: return "plucks";
    case Vector::Strums: return "strums";
    case Vector::SoftNotes: return "soft_notes";
    case Vector::OnsetBursts: return "onset_bursts";
    case Vector::Saturation: return "saturation";
    case Vector::Silence: return "silence";
  }
  return "unknown";
}

uint32_t BuildVector(Vector v, uint32_t activeFrames, Note* notes, uint32_t capacity) noexcept {
  ScoreWriter w{notes, notes != nullptr ? capacity : 0u, 0u};
  switch (v) {
    case Vector::Plucks: BuildPlucks(activeFrames, w); break;
    case Vector::Strums: BuildStrums(activeFrames, w); break;
    case Vector::SoftNotes: BuildSoftNotes(activeFrames, w); break;
    case Vector::OnsetBursts: BuildOnsetBursts(activeFrames, w); break;
    case Vector::Saturation: BuildSaturation(activeFrames, w); break;
    case Vector::Silence: break;
  }
  return w.count;
}

}  // namespace brainscape::testsignal
