// The live image's test signals (Tone.h; firmware/DAC_TEST.md).
#include "live/Tone.h"

namespace brainscape::fw::tone {

namespace {

// 10^(-a/20), a = 0..120, and 10^(-b/2000), b = 0..99: binary64 rounded once from the exact
// value (computed in 80-digit decimal, written as hex-float literals so no compiler parses a
// decimal). firmware_tone checks the codes they give at all 12,001 levels.
constexpr double kWholeDb[121] = {
    0x1.0000000000000p+0, 0x1.c8520affa0a4bp-1, 0x1.96b230bcdc434p-1, 0x1.6a77deae8ab8ap-1,
    0x1.430cd74f6d478p-1, 0x1.1feb33c1c381ep-1, 0x1.009b9cf334252p-1, 0x1.c9676c6fcaf20p-2,
    0x1.97a967f7524b3p-2, 0x1.6b54337bc3b65p-2, 0x1.43d136248490fp-2, 0x1.209a37aef4450p-2,
    0x1.0137987dd704cp-2, 0x1.ca7d767c030d5p-3, 0x1.98a13577c93c0p-3, 0x1.6c310e3769f3fp-3,
    0x1.44960c576b375p-3, 0x1.2149a5fed24d9p-3, 0x1.01d3f2d9684d0p-3, 0x1.cb94298ac6d24p-4,
    0x1.999999999999ap-4, 0x1.6d0e6f32e6ea2p-4, 0x1.455b5a30b035dp-4, 0x1.21f97ef20893bp-4,
    0x1.0270ac3f8a9fap-4, 0x1.ccab8602d2697p-5, 0x1.9a9294b8536eap-5, 0x1.6dec56bfd58e7p-5,
    0x1.46211ff90ea29p-5, 0x1.22a9c2c9695eap-5, 0x1.030dc4ea03a72p-5, 0x1.cdc38c4b206e7p-6,
    0x1.9b8c272fbe6e0p-6, 0x1.6ecac53002711p-6, 0x1.46e75df96dc99p-6, 0x1.235a71c5ee5ccp-6,
    0x1.03ab3d12bc2c4p-6, 0x1.cedc3ccaea15cp-7, 0x1.9c86515bda14cp-7, 0x1.6fa9bad56bdb6p-7,
    0x1.47ae147ae147bp-7, 0x1.240b8c28b8bb5p-7, 0x1.044914f3c02b1p-7, 0x1.cff597e9a752cp-8,
    0x1.9d811398ddcc3p-8, 0x1.7089380241edfp-8, 0x1.487543c6a9255p-8, 0x1.24bd1233113ecp-8,
    0x1.04e74cc73ee87p-8, 0x1.d10f9e0f0efddp-9, 0x1.9e7c6e43390b7p-9, 0x1.71693d08e6becp-9,
    0x1.493cec2631f1ap-9, 0x1.256f0426685a7p-9, 0x1.0585e4c78b07bp-9, 0x1.d22a4fa316facp-10,
    0x1.9f7861b7937a0p-10, 0x1.7249ca3bee77dp-10, 0x1.4a050de314dd7p-10, 0x1.2621624456492p-10,
    0x1.0624dd2f1a9fcp-10, 0x1.d345ad0df45eep-11, 0x1.a074ee52cd11bp-11, 0x1.732adfee1f756p-11,
    0x1.4acda94717d68p-11, 0x1.26d42cce9b24cp-11, 0x1.06c4363887511p-11, 0x1.d461b6b81b979p-12,
    0x1.a1721471fe40bp-12, 0x1.740c7e727264bp-12, 0x1.4b96be9c2da2cp-12, 0x1.278764071eff0p-12,
    0x1.0763f01e8e5aep-12, 0x1.d57e6d0a4090cp-13, 0x1.a26fd472780c4p-13, 0x1.74eea61c12623p-13,
    0x1.4c604e2c75fb3p-13, 0x1.283b082ff1f97p-13, 0x1.08040b1c10b12p-13, 0x1.d69bd06d56db6p-14,
    0x1.a36e2eb1c432dp-14, 0x1.75d1573e5d18bp-14, 0x1.4d2a58423da7cp-14, 0x1.28ef198b4c5dep-14,
    0x1.08a4876c13120p-14, 0x1.d7b9e14a91d46p-15, 0x1.a46d238da54e8p-15, 0x1.76b4922ce2dfbp-15,
    0x1.4df4dd27fe9a3p-15, 0x1.29a3985b8eb6fp-15, 0x1.09456549be1bdp-15, 0x1.d8d8a00b64cb3p-16,
    0x1.a56cb36416f7dp-16, 0x1.7798573b66da3p-16, 0x1.4ebfdd286009dp-16, 0x1.2a5884e341e83p-16,
    0x1.09e6a4f05e629p-16, 0x1.d9f80d198328bp-17, 0x1.a66cde934de83p-17, 0x1.787ca6bddf15fp-17,
    0x1.4f8b588e368f1p-17, 0x1.2b0ddf6517470p-17, 0x1.0a88469b64863p-17, 0x1.db1828dee0964p-18,
    0x1.a76da579b81cdp-18, 0x1.7961810874a9fp-18, 0x1.50574fa4843ecp-18, 0x1.2bc3a823e8b2fp-18,
    0x1.0b2a4a8665482p-18, 0x1.dc38f3c5b124bp-19, 0x1.a86f0875fcf94p-19, 0x1.7a46e66f83d5cp-19,
    0x1.5123c2b678c64p-19, 0x1.2c79df62b8ae9p-19, 0x1.0bccb0ed19a17p-19, 0x1.dd5a6e3869738p-20,
    0x1.a97107e6fd6a8p-20, 0x1.7b2cd7479c209p-20, 0x1.51f0b20f71869p-20, 0x1.2d308564b277fp-20,
    0x1.0c6f7a0b5ed8dp-20,
};
constexpr double kHundredthDb[100] = {
    0x1.0000000000000p+0, 0x1.ff692f4303d67p-1, 0x1.fed28af2ab826p-1, 0x1.fe3c1301e1137p-1,
    0x1.fda5c7639273cp-1, 0x1.fd0fa80ab1673p-1, 0x1.fc79b4ea338a2p-1, 0x1.fbe3edf512504p-1,
    0x1.fb4e531e4b038p-1, 0x1.fab8e458dec2fp-1, 0x1.fa23a197d2819p-1, 0x1.f98e8ace2f054p-1,
    0x1.f8f99fef00e58p-1, 0x1.f864e0ed588a8p-1, 0x1.f7d04dbc4a2bfp-1, 0x1.f73be64eedcfcp-1,
    0x1.f6a7aa985f491p-1, 0x1.f6139a8bbe374p-1, 0x1.f57fb61c2e04ap-1, 0x1.f4ebfd3cd5e53p-1,
    0x1.f4586fe0e0d60p-1, 0x1.f3c50dfb7d9b9p-1, 0x1.f331d77fdec0ep-1, 0x1.f29ecc613a967p-1,
    0x1.f20bec92cb311p-1, 0x1.f1793807ce68bp-1, 0x1.f0e6aeb385d77p-1, 0x1.f054508936d84p-1,
    0x1.efc21d7c2a861p-1, 0x1.ef30157fadba8p-1, 0x1.ee9e3887110cfp-1, 0x1.ee0c8685a8d15p-1,
    0x1.ed7aff6ecd16dp-1, 0x1.ece9a335d9a75p-1, 0x1.ec5871ce2e05cp-1, 0x1.ebc76b2b2d6d3p-1,
    0x1.eb368f403ecfep-1, 0x1.eaa5de00ccd60p-1, 0x1.ea15576045dcbp-1, 0x1.e984fb521bf4bp-1,
    0x1.e8f4c9c9c4e1bp-1, 0x1.e864c2baba18cp-1, 0x1.e7d4e61878bf9p-1, 0x1.e74533d681ab6p-1,
    0x1.e6b5abe8595f9p-1, 0x1.e6264e41880cfp-1, 0x1.e5971ad599906p-1, 0x1.e50811981d71fp-1,
    0x1.e479327ca6e3bp-1, 0x1.e3ea7d76ccc0ap-1, 0x1.e35bf27a298b9p-1, 0x1.e2cd917a5b6e3p-1,
    0x1.e23f5a6b0437ep-1, 0x1.e1b14d3fc95cap-1, 0x1.e12369ec53f40p-1, 0x1.e095b06450b7fp-1,
    0x1.e008209b70041p-1, 0x1.df7aba8565d43p-1, 0x1.deed7e15e9c35p-1, 0x1.de606b40b70adp-1,
    0x1.ddd381f98c813p-1, 0x1.dd46c2342c990p-1, 0x1.dcba2be45d5fep-1, 0x1.dc2dbefde87d4p-1,
    0x1.dba17b749b31bp-1, 0x1.db15613c46556p-1, 0x1.da897048be578p-1, 0x1.d9fda88ddb3ccp-1,
    0x1.d97209ff789eap-1, 0x1.d8e6949175aa4p-1, 0x1.d85b4837b51f4p-1, 0x1.d7d024e61d4ecp-1,
    0x1.d7452a90981a6p-1, 0x1.d6ba592b12f32p-1, 0x1.d62fb0a97ed86p-1, 0x1.d5a530ffd056dp-1,
    0x1.d51ada21ff877p-1, 0x1.d490ac04080e5p-1, 0x1.d406a699e919ep-1, 0x1.d37cc9d7a561ap-1,
    0x1.d2f315b143251p-1, 0x1.d2698a1acc2adp-1, 0x1.d1e027084dbf9p-1, 0x1.d156ec6dd8b4fp-1,
    0x1.d0cdda3f81607p-1, 0x1.d044f0715f9a8p-1, 0x1.cfbc2ef78ebd8p-1, 0x1.cf3395c62da4ap-1,
    0x1.ceab24d15eaabp-1, 0x1.ce22dc0d47a99p-1, 0x1.cd9abb6e11f8bp-1, 0x1.cd12c2e7ea6c4p-1,
    0x1.cc8af26f01542p-1, 0x1.cc0349f78a7adp-1, 0x1.cb7bc975bd248p-1, 0x1.caf470ddd40e1p-1,
    0x1.ca6d40240d6bdp-1, 0x1.c9e6373caae8ap-1, 0x1.c95f561bf1a52p-1, 0x1.c8d89cb62a365p-1,
};

}  // namespace

float CodeToFloat(int32_t code) noexcept {
  // |code| <= 2^24 converts exactly, and the power-of-two scale is exact.
  return static_cast<float>(code) * 0x1p-23f;
}

int32_t LevelCode(int32_t centiDb) noexcept {
  if (centiDb >= 0) return kFullScale;
  if (centiDb < kMinCentiDb) centiDb = kMinCentiDb;
  // 10^(c / 2000) = 10^(-a/20) * 10^(-b/2000) for -c = 100 a + b: one IEEE product of two
  // tabled constants, then an exact power-of-two scale and an exact rounding, so every target
  // gives the same code. The product is within 2 ulp of the exact level, 2^23 times that is
  // under 1e-8 of a code, and no level lies within 1e-5 of a rounding tie (firmware_tone
  // counts them): the code is the exact level rounded to nearest. No engine code is called,
  // so the live image's ITCM keeps the layout it has without the tone (Tone.h).
  const auto   n = static_cast<uint32_t>(-centiDb);
  const double x = kWholeDb[n / 100u] * kHundredthDb[n % 100u] * 0x1p23;  // in [8, 2^23)
  const auto   t = static_cast<int32_t>(x);
  return x - static_cast<double>(t) >= 0.5 ? t + 1 : t;  // x - t is exact
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
