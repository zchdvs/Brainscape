#pragma once
// The live image's test signals: the `tone` console command (firmware/DAC_TEST.md;
// docs/design/output-limiter.md §6.4, lane L1b, D10). While a tone plays, AudioCallback
// writes these samples straight into libDaisy's output buffers instead of running the engine;
// `tone off` hands the output back to the engine. Nothing here is in the engine or its
// archive, so it is not a sound revision, and none of it is placed in ITCM: the live image
// executes it in place from QSPI with the rest of the firmware.
//
// Every sample is an integer code q on the codec's 24-bit grid, handed out as the float
// q * 2^-23, which is exact (|q| <= 2^23), as the golden harness's test signals are
// (brainscape/TestSignal.h). libDaisy then converts with f2s24, which clamps to ±0.999985
// and multiplies by 2^23: the codec receives exactly q, except that a code beyond ±8,388,482
// (1.0 is 2^23) reaches it as ±8,388,482, the code the engine's own 1.0 gives
// (output-limiter.md §2.2). The arithmetic is integer: a quarter-wave table of the 1 kHz sine
// in Q30, one 32x32->64 product and a rounding shift per sample. The one transcendental, a
// level in dB, is converted once per step on the main thread from two tables of binary64
// powers of ten with one IEEE product and exact steps after it (LevelCode), so the host test
// (firmware/tests/tone.cpp) computes the same codes as the Seed, bit for bit. None of it calls
// the engine's code: an engine function referenced from here would pull its object out of the
// engine's archive earlier in the link and reorder the live image's ITCM. As it is, every ITCM
// function sits at the address it has without the tone; only the veneers through which ITCM
// calls the tempo core's QSPI code point to where that code moved.
//
// The signals, all identical on both channels, phase-locked to one 48-frame period (1 kHz at
// 48 kHz) that runs on from the start of the command:
//   Sine    1 kHz, sample peaks at the level (k = 12 and 36 of each period sit on the crest)
//   Isp     fs/4 (12 kHz) at 45 degrees: +P, +P, -P, -P, whose sample peaks are P but whose
//           reconstructed waveform peaks at P * sqrt(2), +3.01 dB: the inter-sample-peak test
//   Fs4     fs/4 at 0 degrees: 0, +P, 0, -P, whose true peak is its sample peak: Isp's control
//   Square  1 kHz, 24 frames at +P and 24 at -P
//   Silence zeros
// and the DAC test program (kTestProgram), which tools/hil/dac_test.py finds in a recording
// and measures: change it only with kProgramVersion and dac_test.py's PROGRAM together (the
// host test prints it for test_dac_test.py to compare).
#include <atomic>
#include <cstdint>

namespace brainscape::fw::tone {

inline constexpr uint32_t kProgramVersion = 1;
inline constexpr uint32_t kSampleRate     = 48000;
inline constexpr uint32_t kPeriod         = 48;       // frames: 1 kHz; fs/4 (4) divides it
inline constexpr int32_t  kFullScale      = 1 << 23;  // the code of 1.0
inline constexpr int32_t  kCodecMax       = 8388482;  // f2s24(1.0): the largest code the codec receives
inline constexpr int32_t  kMinCentiDb     = -12000;   // levels in hundredths of a dB, sample peak
inline constexpr int32_t  kMaxCentiDb     = 0;

enum class Wave : uint8_t { Silence, Sine, Isp, Fs4, Square };

// round(2^30 * sin(2 pi k / 48)), k = 0..12: the 1 kHz sine's quarter wave. Computed to 60
// digits; the nearest any entry comes to a rounding tie is 0.07 (k = 10), so the test's
// binary64 check of every entry is unambiguous.
inline constexpr int32_t kSineQ30[13] = {
    0,         140151432, 277904834, 410903207,  536870912,  653652607, 759250125,
    851856663, 929887697, 992008094, 1037154959, 1064555814, 1073741824};

// The 1 kHz sine's code at `phase` (0..47) for a crest code q in [0, 2^23]: round half up of
// q * sin on the positive half, mirrored, so the two halves are exact negatives.
constexpr int32_t SineCode(int32_t q, uint32_t phase) noexcept {
  const uint32_t half = phase < 24u ? phase : phase - 24u;
  const uint32_t k    = half <= 12u ? half : 24u - half;
  const int64_t  p    = static_cast<int64_t>(q) * kSineQ30[k];
  const auto     v    = static_cast<int32_t>((p + (int64_t{1} << 29)) >> 30);  // p >= 0
  return phase < 24u ? v : -v;
}

// The code of `wave` at `phase` (0..47) for the level code q in [0, 2^23].
constexpr int32_t Code(Wave wave, int32_t q, uint32_t phase) noexcept {
  switch (wave) {
    case Wave::Sine:
      return SineCode(q, phase);
    case Wave::Isp:
      return (phase & 2u) == 0u ? q : -q;
    case Wave::Fs4:
      return (phase & 1u) == 0u ? 0 : ((phase & 2u) == 0u ? q : -q);
    case Wave::Square:
      return phase < kPeriod / 2u ? q : -q;
    case Wave::Silence:
      break;
  }
  return 0;
}

// What the codec receives for a code: libDaisy's f2s24 of CodeToFloat(code).
constexpr int32_t CodecCode(int32_t code) noexcept {
  return code > kCodecMax ? kCodecMax : (code < -kCodecMax ? -kCodecMax : code);
}

// q * 2^-23, exactly.
float CodeToFloat(int32_t code) noexcept;

// The crest code of a level: round(2^23 * 10^(centiDb / 2000)), for centiDb in
// [kMinCentiDb, kMaxCentiDb]; 0 dB is 2^23 exactly.
int32_t LevelCode(int32_t centiDb) noexcept;

// A level as typed: an optional minus sign, digits, and at most two decimals ("-3.01", "0",
// "-20"), read exactly, within [kMinCentiDb, kMaxCentiDb]. A plus sign or a positive level is
// refused: the generator never asks the codec for more than full scale.
bool ParseCentiDb(const char* text, int32_t* centiDb) noexcept;

// A level as text with two decimals and a sign when it is not zero ("-3.01", "0.00", "+3.01").
// `out` holds at least 12 characters.
void CentiDbText(int32_t centiDb, char* out) noexcept;

const char* WaveName(Wave wave) noexcept;  // "silence", "sine", "isp", "fs4", "square"

// One stretch of a program at one level.
struct Step {
  Wave     wave    = Wave::Silence;
  int32_t  centiDb = 0;
  int32_t  q       = 0;  // LevelCode(centiDb), 0 for silence
  uint32_t frames  = 0;
};

inline constexpr uint32_t kMaxSteps = 96;

struct Program {
  Step     steps[kMaxSteps];
  uint32_t count = 0;
  bool     loop  = false;  // start again after the last step; otherwise silence after it

  uint64_t Frames() const noexcept;  // one pass
};

// A run of steps: `steps` levels from startCentiDb in stepCentiDb increments, each
// stepFrames long.
struct Segment {
  Wave     wave;
  int32_t  startCentiDb;
  int32_t  stepCentiDb;
  uint32_t steps;
  uint32_t stepFrames;
};

// The DAC test program, version kProgramVersion: 2,856,000 frames (59.5 s), then silence.
// tools/hil/dac_test.py's PROGRAM is the same list.
inline constexpr uint32_t kGap = kSampleRate / 2u;
inline constexpr Segment  kTestProgram[] = {
    {Wave::Silence, 0, 0, 1, kSampleRate},         // lead-in: 1 s of silence
    {Wave::Sine, -2000, 0, 1, 2u * kSampleRate},   // 1 kHz at -20 dBFS: the reference gain
    {Wave::Silence, 0, 0, 1, kGap},
    {Wave::Sine, -600, 0, 1, 2u * kSampleRate},
    {Wave::Silence, 0, 0, 1, kGap},
    {Wave::Sine, -100, 0, 1, 2u * kSampleRate},
    {Wave::Silence, 0, 0, 1, kGap},
    {Wave::Sine, 0, 0, 1, 2u * kSampleRate},       // sample peaks at 1.0: the 0 dBFS level
    {Wave::Silence, 0, 0, 1, kGap},
    {Wave::Isp, -1200, 0, 1, 2u * kSampleRate},    // fs/4 at 45 degrees, true peak -8.99 dBTP: the 12 kHz reference
    {Wave::Silence, 0, 0, 1, kGap},
    {Wave::Fs4, 0, 0, 1, 2u * kSampleRate},        // fs/4 at 0 degrees, sample and true peak 1.0: the control
    {Wave::Silence, 0, 0, 1, kGap},
    {Wave::Isp, -301, 0, 1, 2u * kSampleRate},     // true peak 0.00 dBTP
    {Wave::Silence, 0, 0, 1, kGap},
    {Wave::Isp, 0, 0, 1, 2u * kSampleRate},        // sample peaks at 1.0, true peak +3.01 dBTP: D10's test
    {Wave::Silence, 0, 0, 1, kGap},
    {Wave::Square, 0, 0, 1, 2u * kSampleRate},     // full-scale square
    {Wave::Silence, 0, 0, 1, kGap},
    {Wave::Silence, 0, 0, 1, 2u * kSampleRate},    // the noise floor
    {Wave::Silence, 0, 0, 1, kGap},
    {Wave::Sine, -4000, 100, 41, kGap},            // sweep: 1 kHz, -40 to 0 dBFS in 1 dB steps
    {Wave::Silence, 0, 0, 1, kGap},
    {Wave::Isp, -1200, 50, 25, kGap},              // sweep: fs/4 at 45 degrees, sample peaks -12 to 0 dBFS
};

// Programs for the commands. Each returns false (and leaves `p` empty) only on a level out of
// range or a wave the builder does not take.
bool BuildTest(Program* p) noexcept;
bool BuildSteady(Program* p, Wave wave, int32_t centiDb) noexcept;  // one level, until stopped
bool BuildSweep(Program* p, Wave wave) noexcept;  // Sine or Isp: kTestProgram's sweep, then 1 s of silence, repeating

// Plays a program. Start runs on the main thread while the audio callback does not call Fill
// (the live image mutes it first); Fill runs in the audio callback. Its output depends only on
// the program and the number of frames since Start, never on how they are blocked.
class Player {
 public:
  void Start(const Program& program) noexcept;
  void Fill(float* left, float* right, uint32_t frames) noexcept;

  uint32_t Frames() const noexcept { return frames_.load(std::memory_order_relaxed); }  // since Start, wrapping
  bool     Done() const noexcept { return !playing_.load(std::memory_order_relaxed); }  // ended, or never started
  const Program& program() const noexcept { return program_; }

 private:
  Program               program_;
  uint32_t              step_    = 0;
  uint32_t              inStep_  = 0;
  uint32_t              phase_   = 0;
  std::atomic<uint32_t> frames_{0};
  std::atomic<bool>     playing_{false};  // zero-initialized: the live image's player is in .bss
};

}  // namespace brainscape::fw::tone
