// The live image's test signals (Tone.h; firmware/DAC_TEST.md).
#include "live/Tone.h"

#include "detail/DetMath.h"

namespace brainscape::fw::tone {

float CodeToFloat(int32_t code) noexcept {
  // |code| <= 2^24 converts exactly, and the power-of-two scale is exact.
  return static_cast<float>(code) * 0x1p-23f;
}

int32_t LevelCode(int32_t centiDb) noexcept {
  if (centiDb >= 0) return kFullScale;
  if (centiDb < kMinCentiDb) centiDb = kMinCentiDb;
  // 10^(c / 2000) = 2^(c * log2(10) / 2000): an exact integer times a binary64 constant
  // (log2(10) / 2000 rounded once, as a hex-float literal), one rounding, then the engine's
  // exp2, identical on every target (determinism-profile.md §3.9).
  constexpr double kLog2TenOver2000 = 0x1.b369690dedc9dp-10;
  const double     t = static_cast<double>(centiDb) * kLog2TenOver2000;
  const double     g = detmath::Exp2D(t);
  return detmath::RoundHalfAwayI32(detmath::ScaleB(g, 23));
}

bool ParseCentiDb(const char* text, int32_t* centiDb) noexcept {
  if (text == nullptr || *text == '\0') return false;
  bool negative = false;
  if (*text == '-') {
    negative = true;
    ++text;
  }
  int32_t whole  = 0;
  int32_t digits = 0;
  for (; *text >= '0' && *text <= '9'; ++text) {
    if (++digits > 3) return false;  // beyond -999 dB
    whole = whole * 10 + (*text - '0');
  }
  int32_t hundredths = 0;
  int32_t decimals   = 0;
  if (*text == '.') {
    ++text;
    for (; *text >= '0' && *text <= '9'; ++text) {
      if (++decimals > 2) return false;
      hundredths = hundredths * 10 + (*text - '0');
    }
    if (decimals == 1) hundredths *= 10;
  }
  if (*text != '\0' || digits + decimals == 0) return false;
  int32_t value = whole * 100 + hundredths;
  if (negative) value = -value;
  if (value < kMinCentiDb || value > kMaxCentiDb) return false;
  *centiDb = value;
  return true;
}

void CentiDbText(int32_t centiDb, char* out) noexcept {
  char*    p = out;
  uint32_t m = 0;
  if (centiDb < 0) {
    *p++ = '-';
    m    = static_cast<uint32_t>(-static_cast<int64_t>(centiDb));
  } else {
    if (centiDb > 0) *p++ = '+';
    m = static_cast<uint32_t>(centiDb);
  }
  char     digits[10];
  uint32_t n     = 0;
  uint32_t whole = m / 100u;
  do {
    digits[n++] = static_cast<char>('0' + whole % 10u);
    whole /= 10u;
  } while (whole != 0u);
  while (n != 0u) *p++ = digits[--n];
  *p++ = '.';
  *p++ = static_cast<char>('0' + (m / 10u) % 10u);
  *p++ = static_cast<char>('0' + m % 10u);
  *p   = '\0';
}

const char* WaveName(Wave wave) noexcept {
  switch (wave) {
    case Wave::Sine:
      return "sine";
    case Wave::Isp:
      return "isp";
    case Wave::Fs4:
      return "fs4";
    case Wave::Square:
      return "square";
    case Wave::Silence:
      break;
  }
  return "silence";
}

uint64_t Program::Frames() const noexcept {
  uint64_t total = 0;
  for (uint32_t i = 0; i < count; ++i) total += steps[i].frames;
  return total;
}

namespace {

bool Append(Program* p, Wave wave, int32_t centiDb, uint32_t frames) noexcept {
  if (p->count == kMaxSteps || frames == 0u) return false;
  if (wave != Wave::Silence && (centiDb < kMinCentiDb || centiDb > kMaxCentiDb)) return false;
  Step& s   = p->steps[p->count++];
  s.wave    = wave;
  s.centiDb = wave == Wave::Silence ? 0 : centiDb;
  s.q       = wave == Wave::Silence ? 0 : LevelCode(centiDb);
  s.frames  = frames;
  return true;
}

bool AppendSegment(Program* p, const Segment& seg) noexcept {
  for (uint32_t i = 0; i < seg.steps; ++i) {
    const int32_t level = seg.startCentiDb + static_cast<int32_t>(i) * seg.stepCentiDb;
    if (!Append(p, seg.wave, level, seg.stepFrames)) return false;
  }
  return true;
}

bool Fail(Program* p) noexcept {
  p->count = 0;
  p->loop  = false;
  return false;
}

}  // namespace

bool BuildTest(Program* p) noexcept {
  p->count = 0;
  p->loop  = false;
  for (const Segment& seg : kTestProgram) {
    if (!AppendSegment(p, seg)) return Fail(p);
  }
  return true;
}

bool BuildSteady(Program* p, Wave wave, int32_t centiDb) noexcept {
  p->count = 0;
  p->loop  = true;
  return Append(p, wave, centiDb, kSampleRate) ? true : Fail(p);
}

bool BuildSweep(Program* p, Wave wave) noexcept {
  p->count = 0;
  p->loop  = true;
  if (wave != Wave::Sine && wave != Wave::Isp) return Fail(p);
  for (const Segment& seg : kTestProgram) {
    if (seg.wave != wave || seg.steps < 2u) continue;
    if (!AppendSegment(p, seg) || !Append(p, Wave::Silence, 0, kSampleRate)) return Fail(p);
    return true;
  }
  return Fail(p);
}

void Player::Start(const Program& program) noexcept {
  program_ = program;
  step_    = 0;
  inStep_  = 0;
  phase_   = 0;
  frames_.store(0, std::memory_order_relaxed);
  bool playable = program_.count != 0u && program_.count <= kMaxSteps;
  for (uint32_t i = 0; playable && i < program_.count; ++i) playable = program_.steps[i].frames != 0u;
  playing_.store(playable, std::memory_order_relaxed);
}

void Player::Fill(float* left, float* right, uint32_t frames) noexcept {
  bool done = !playing_.load(std::memory_order_relaxed);
  for (uint32_t i = 0; i < frames; ++i) {
    int32_t code = 0;
    if (!done) {
      const Step& s = program_.steps[step_];
      code          = Code(s.wave, s.q, phase_);
      if (++inStep_ == s.frames) {
        inStep_ = 0;
        if (++step_ == program_.count) {
          step_ = 0;
          done  = !program_.loop;
        }
      }
    }
    phase_        = phase_ + 1u == kPeriod ? 0u : phase_ + 1u;
    const float v = CodeToFloat(code);
    left[i]       = v;
    right[i]      = v;
  }
  playing_.store(!done, std::memory_order_relaxed);
  frames_.store(frames_.load(std::memory_order_relaxed) + frames, std::memory_order_relaxed);
}

}  // namespace brainscape::fw::tone
