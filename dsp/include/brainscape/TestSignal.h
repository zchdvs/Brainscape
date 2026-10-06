#pragma once
#include <cstdint>

// Shared deterministic test-signal generator (docs/design/determinism-profile.md
// §5.13). CI's golden-hash harness, the firmware render mode and the app's parity
// check all derive the same input bits from it, so nothing is transferred.
//
// Every sample is computed in integers as a signed 24-bit value q (the codec grid
// of profile §2.2) and handed to the engine as exactly q * 2^-23: the int -> float
// conversion and the power-of-two scale are exact, so no compiler flag, FP mode or
// libm can change an input bit. Integer arithmetic only; right shifts of negative
// values go through FloorShift (implementation-defined in C++17 otherwise).
namespace brainscape::testsignal {

// Bump whenever ANY sample of ANY note or vector changes. Golden files record it per
// vector (profile §6.1), so a generator change reads as a version mismatch, never as
// an engine sound change.
inline constexpr uint32_t kVersion = 1;

inline constexpr uint32_t kSampleRate = 48000;  // the only rate vectors are defined at
inline constexpr int32_t  kQ23Max     = (1 << 23) - 1;
inline constexpr int32_t  kQ23Min     = -(1 << 23);

// SplitMix32's output function (MurmurHash3's 32-bit finalizer).
constexpr uint32_t Mix32(uint32_t z) noexcept {
  z = (z ^ (z >> 16)) * 0x85EBCA6Bu;
  z = (z ^ (z >> 13)) * 0xC2B2AE35u;
  return z ^ (z >> 16);
}
// Draw n (0-based) of the SplitMix32 stream whose state starts at `seed`. Random
// access: a noise sample depends only on (seed, n), never on how frames are blocked.
constexpr uint32_t SplitMix32(uint32_t seed, uint32_t n) noexcept {
  return Mix32(seed + (n + 1u) * 0x9E3779B9u);
}
// Uniform signed 24-bit noise in [kQ23Min, kQ23Max] (the draw's top 24 bits).
constexpr int32_t Noise24(uint32_t seed, uint32_t n) noexcept {
  return static_cast<int32_t>(SplitMix32(seed, n) >> 8) - (1 << 23);
}
// floor(v / 2^s): an arithmetic shift with defined behaviour for negative v.
constexpr int64_t FloorShift(int64_t v, unsigned s) noexcept {
  return v >= 0 ? v >> s : -((-(v + 1)) >> s) - 1;
}
constexpr int32_t SaturateQ23(int64_t v) noexcept {
  return v > kQ23Max ? kQ23Max : (v < kQ23Min ? kQ23Min : static_cast<int32_t>(v));
}

// q * 2^-23, exactly. Out of line: public headers carry no floating-point bodies
// (profile §3.5).
float Q23ToFloat(int32_t q) noexcept;

enum class Kind : uint8_t {
  Noise,  // SplitMix32 noise burst
  Pluck,  // Karplus-Strong string excited by a SplitMix32 burst
  Tone,   // integer phase accumulator through a fixed-point parabolic sine
};

// One sounding event. Q-format fields are integers; nothing here is a float.
struct Note {
  Kind     kind     = Kind::Noise;
  uint32_t start    = 0;       // first frame
  uint32_t length   = 0;       // frames the note sounds; its voice frees afterwards
  int32_t  level    = 0;       // peak amplitude in Q23 (may exceed full scale: the mix saturates)
  uint32_t pitch    = 0;       // Pluck: period in frames [2, kMaxPeriod]; Tone: frequency in mHz
  uint32_t seed     = 0;       // noise key (Noise samples, Pluck excitation)
  uint32_t attack   = 0;       // frames of linear fade-in
  uint32_t release  = 0;       // frames of linear fade-out ending at start + length
  uint16_t panL     = 32768;   // channel gains, Q15 (32768 = unity)
  uint16_t panR     = 32768;
};

// Streams a score: notes sorted by start, mixed in int64 and saturated to Q23.
class Generator {
 public:
  static constexpr uint32_t kMaxVoices = 16;    // a 17th overlapping note steals the oldest
  static constexpr uint32_t kMaxPeriod = 1024;  // lowest pluck: 46.9 Hz

  // Rewinds to frame 0. `notes` must stay alive and be sorted by start.
  void Start(const Note* notes, uint32_t count) noexcept;
  // The next n frames; any block split yields the same samples.
  void RenderQ23(int32_t* left, int32_t* right, uint32_t n) noexcept;
  void Render(float* left, float* right, uint32_t n) noexcept;
  uint64_t Frame() const noexcept { return frame_; }

 private:
  struct Voice {
    const Note* note   = nullptr;  // null = free
    uint32_t    age    = 0;        // frames since start
    uint32_t    phase  = 0;        // Tone: phase accumulator and increment
    uint32_t    inc    = 0;
    uint32_t    period = 0;        // Pluck: string length and read index into ks
    uint32_t    pos    = 0;
    int32_t     ks[kMaxPeriod];
  };
  void Begin(Voice& v, const Note& n) noexcept;
  int32_t Next(Voice& v) noexcept;  // mono Q23 sample, pre-pan

  const Note* notes_   = nullptr;
  uint32_t    count_   = 0;
  uint32_t    next_    = 0;  // index of the next note to start
  uint64_t    frame_   = 0;
  Voice       voices_[kMaxVoices];
};

// Standard vectors, defined once here so CI, the firmware render mode and the app
// render the same scores. Each writes min(count, capacity) notes and returns count;
// notes start and end inside [0, activeFrames), so frames after it are silent.
enum class Vector : uint8_t {
  Plucks,       // single plucks every 0.5 s across the guitar range, alternating pan
  Strums,       // six-string chords strummed every 2 s (12 ms per string)
  SoftNotes,    // overlapping tones with 60 ms attacks: few or no onsets
  OnsetBursts,  // 12-20 ms noise bursts 90-150 ms apart: dense onsets
  Saturation,   // chords and noise at about 4x full scale, clipped by the mix
  Silence,      // no notes
};
const char* VectorName(Vector) noexcept;
uint32_t    BuildVector(Vector, uint32_t activeFrames, Note* notes, uint32_t capacity) noexcept;

}  // namespace brainscape::testsignal
