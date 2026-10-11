// ctest firmware_tone: the live image's test signals (firmware/live/Tone.h), the `tone`
// command's arithmetic, on the host from the same source the Seed runs. The quarter-wave table
// against binary64 sin; every code of every wave against its definition; the exact float
// hand-off and libDaisy's f2s24 clamp; every level in [-120, 0] dB against binary64 pow; the
// level parser and printer; the DAC test program's steps; the player against a frame-by-frame
// reference model under many block splits, its loop and its silence after the end; and a digest
// of the whole program's codes, pinned with kProgramVersion, so a change to any sample of the
// program fails here until both are updated (with tools/hil/dac_test.py's PROGRAM).
//
//   firmware_tone                 run the checks
//   firmware_tone --program-json  print the program's steps and one period of codes per step,
//                                 for tools/hil/test_dac_test.py to compare with dac_test.py
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "live/Tone.h"

namespace {

using namespace brainscape::fw::tone;

int g_failures = 0;

void Check(bool ok, const char* what) {
  if (!ok) {
    std::printf("FAIL: %s\n", what);
    ++g_failures;
  }
}

constexpr double kTwoPi = 6.283185307179586476925286766559;

// libDaisy's f2s24 (src/daisy_core.h): clamp to +-FBIPMAX, scale by 2^23, truncate.
int32_t F2s24(float x) {
  const float fbipmax = 0.999985f;
  x                   = x <= -fbipmax ? -fbipmax : x;
  x                   = x >= fbipmax ? fbipmax : x;
  return static_cast<int32_t>(x * 8388608.0f);
}

// The code of frame n of a program, from its step table alone.
int32_t ReferenceCode(const Program& p, uint64_t n) {
  const uint64_t pass = p.Frames();
  if (!p.loop && n >= pass) return 0;
  uint64_t at = p.loop ? n % pass : n;
  for (uint32_t i = 0; i < p.count; ++i) {
    if (at < p.steps[i].frames) {
      return Code(p.steps[i].wave, p.steps[i].q, static_cast<uint32_t>(n % kPeriod));
    }
    at -= p.steps[i].frames;
  }
  return 0;
}

void TestSineTable() {
  for (uint32_t k = 0; k <= 12; ++k) {
    const double exact = std::ldexp(std::sin(kTwoPi * k / 48.0), 30);
    const double err   = std::fabs(static_cast<double>(kSineQ30[k]) - exact);
    Check(err <= 0.5, "kSineQ30 entry is sin(2 pi k / 48) * 2^30 rounded to nearest");
    Check(std::fabs(err - 0.5) > 1e-3, "kSineQ30 entry is not a near-tie");
  }
  Check(kSineQ30[4] == 1 << 29 && kSineQ30[12] == 1 << 30, "sin 30 and 90 degrees exact");
}

void TestCodes() {
  const int32_t levels[] = {kFullScale, kFullScale - 1, kCodecMax, LevelCode(-600), LevelCode(-2000), 12345, 1, 0};
  for (const int32_t q : levels) {
    for (uint32_t k = 0; k < kPeriod; ++k) {
      const int32_t c = SineCode(q, k);
      Check(SineCode(q, (k + 24u) % kPeriod) == -c, "sine halves are exact negatives");
      if (k <= 24u) Check(SineCode(q, 24u - k) == c, "sine positive half is symmetric");
      const double exact = static_cast<double>(q) * std::sin(kTwoPi * k / 48.0);
      Check(std::fabs(static_cast<double>(c) - exact) <= 0.5 + 0.004, "sine code within half a code (and the table's error)");
      Check(Code(Wave::Sine, q, k) == c, "Code(Sine) is SineCode");
      Check(Code(Wave::Silence, q, k) == 0, "silence is zero");
      const int32_t isp[4] = {q, q, -q, -q};
      const int32_t fs4[4] = {0, q, 0, -q};
      Check(Code(Wave::Isp, q, k) == isp[k % 4u], "isp is +P +P -P -P (fs/4 at 45 degrees)");
      Check(Code(Wave::Fs4, q, k) == fs4[k % 4u], "fs4 is 0 +P 0 -P (fs/4 at 0 degrees)");
      Check(Code(Wave::Square, q, k) == (k < 24u ? q : -q), "square is 24 frames +P, 24 -P");
    }
  }
  Check(SineCode(kFullScale, 12) == kFullScale && SineCode(kFullScale, 36) == -kFullScale,
        "the 0 dBFS sine's crest is 1.0");
  Check(SineCode(kFullScale, 0) == 0 && SineCode(kFullScale, 24) == 0, "the sine starts at zero");

  // The float hand-off is exact, and the codec gets the code, clamped at f2s24(1.0).
  const int32_t probes[] = {kFullScale, kFullScale - 1, kCodecMax + 1, kCodecMax, kCodecMax - 1, 4204263, 1, 0,
                            -1, -4204263, -kCodecMax, -kCodecMax - 1, -kFullScale};
  for (const int32_t c : probes) {
    const float f = CodeToFloat(c);
    Check(std::ldexp(static_cast<double>(f), 23) == static_cast<double>(c), "CodeToFloat is exact");
    Check(F2s24(f) == CodecCode(c), "libDaisy's f2s24 gives CodecCode");
  }
  Check(CodeToFloat(kFullScale) == 1.0f && CodeToFloat(-kFullScale) == -1.0f, "full scale is +-1.0f");
  Check(F2s24(1.0f) == 8388482, "f2s24(1.0) is 8,388,482 (output-limiter.md §6.4)");
  for (int32_t c = -kFullScale; c <= kFullScale; c += 4099) {
    Check(F2s24(CodeToFloat(c)) == CodecCode(c), "f2s24 round trip over the range");
  }
}

void TestLevels() {
  Check(LevelCode(0) == kFullScale, "0 dB is 2^23");
  Check(LevelCode(-2000) == 838861 && LevelCode(-600) == 4204263 && LevelCode(-100) == 7476355 &&
            LevelCode(-1200) == 2107123 && LevelCode(-301) == 5931846 && LevelCode(-4000) == 83886 &&
            LevelCode(kMinCentiDb) == 8,
        "the program's levels");
  int32_t  previous   = 0;
  uint32_t nearTies   = 0;
  uint32_t mismatches = 0;
  for (int32_t c = kMinCentiDb; c <= 0; ++c) {
    const int32_t q     = LevelCode(c);
    const double  exact = std::ldexp(std::pow(10.0, c / 2000.0), 23);
    const double  frac  = exact - std::floor(exact);
    if (std::fabs(frac - 0.5) < 1e-6) {
      ++nearTies;
      Check(std::fabs(q - exact) <= 0.5 + 1e-6, "level code at a near-tie is one of the two neighbours");
    } else if (q != static_cast<int32_t>(std::llround(exact))) {
      ++mismatches;
    }
    Check(q >= previous, "level codes never decrease");
    previous = q;
  }
  std::printf("levels: 12,001 codes from -120.00 to 0.00 dB, %u near-ties, %u not rounded to nearest\n",
              nearTies, mismatches);
  Check(mismatches == 0, "every level code is 2^23 * 10^(dB/20) rounded to nearest");
}

void TestParse() {
  struct Case {
    const char* text;
    bool        ok;
    int32_t     value;
  };
  const Case cases[] = {{"0", true, 0},         {"-0", true, 0},          {"-20", true, -2000},
                        {"-3.01", true, -301},  {"-6", true, -600},       {"-1", true, -100},
                        {"-.5", true, -50},     {"-0.5", true, -50},      {"-120", true, -12000},
                        {"-12.25", true, -1225}, {"0.00", true, 0},       {"-120.01", false, 0},
                        {"1", false, 0},        {"+1", false, 0},         {"0.5", false, 0},
                        {"-1.234", false, 0},   {"-", false, 0},          {".", false, 0},
                        {"", false, 0},         {"-6dB", false, 0},       {"-1e1", false, 0},
                        {"--1", false, 0},      {"-9999", false, 0},      {" -1", false, 0}};
  for (const Case& c : cases) {
    int32_t    v  = 7;
    const bool ok = ParseCentiDb(c.text, &v);
    if (ok != c.ok || (ok && v != c.value)) {
      std::printf("FAIL: ParseCentiDb(\"%s\") gave %d, %d\n", c.text, ok, v);
      ++g_failures;
    }
  }
  struct Text {
    int32_t     value;
    const char* text;
  };
  const Text texts[] = {{0, "0.00"}, {-301, "-3.01"}, {301, "+3.01"}, {-12000, "-120.00"}, {-5, "-0.05"}, {-50, "-0.50"}};
  for (const Text& t : texts) {
    char buf[12];
    CentiDbText(t.value, buf);
    if (std::strcmp(buf, t.text) != 0) {
      std::printf("FAIL: CentiDbText(%d) gave %s\n", t.value, buf);
      ++g_failures;
    }
  }
}

void TestPrograms() {
  auto p = std::make_unique<Program>();
  Check(BuildTest(p.get()) && !p->loop, "BuildTest");
  Check(p->count == 88u && p->Frames() == 2856000u, "the test program: 88 steps, 2,856,000 frames (59.5 s)");
  uint32_t i = 0;
  for (const Segment& seg : kTestProgram) {
    for (uint32_t s = 0; s < seg.steps; ++s, ++i) {
      const int32_t level = seg.wave == Wave::Silence ? 0 : seg.startCentiDb + static_cast<int32_t>(s) * seg.stepCentiDb;
      Check(i < p->count && p->steps[i].wave == seg.wave && p->steps[i].centiDb == level &&
                p->steps[i].q == (seg.wave == Wave::Silence ? 0 : LevelCode(level)) &&
                p->steps[i].frames == seg.stepFrames,
            "the test program's steps are kTestProgram's");
    }
  }
  // Every step's length is whole periods, so each tone starts on a period boundary.
  for (uint32_t k = 0; k < p->count; ++k) Check(p->steps[k].frames % kPeriod == 0u, "steps are whole periods");

  Check(BuildSteady(p.get(), Wave::Isp, -301) && p->loop && p->count == 1u && p->steps[0].q == 5931846,
        "BuildSteady");
  Check(!BuildSteady(p.get(), Wave::Sine, 1) && p->count == 0u, "BuildSteady refuses a level above 0 dB");
  Check(!BuildSteady(p.get(), Wave::Sine, kMinCentiDb - 1), "BuildSteady refuses a level below -120 dB");
  Check(BuildSweep(p.get(), Wave::Sine) && p->loop && p->count == 42u && p->steps[0].centiDb == -4000 &&
            p->steps[40].centiDb == 0 && p->steps[41].wave == Wave::Silence && p->steps[41].frames == kSampleRate,
        "BuildSweep(Sine): 41 steps and 1 s of silence");
  Check(BuildSweep(p.get(), Wave::Isp) && p->count == 26u && p->steps[0].centiDb == -1200 &&
            p->steps[24].centiDb == 0,
        "BuildSweep(Isp): 25 steps and 1 s of silence");
  Check(!BuildSweep(p.get(), Wave::Square) && p->count == 0u, "BuildSweep refuses other waves");
}

// Plays `p` for `total` frames in blocks cycling through `blocks`, checking every frame against
// the reference model and both channels against each other; returns a digest of the codes.
uint64_t Play(const Program& p, uint64_t total, const std::vector<uint32_t>& blocks, bool* ok) {
  auto player = std::make_unique<Player>();
  player->Start(p);
  std::vector<float> l(512), r(512);
  uint64_t           n      = 0;
  uint64_t           digest = 1469598103934665603ull;  // FNV-1a over the codes' bytes
  size_t             b      = 0;
  while (n < total) {
    uint32_t frames = blocks[b++ % blocks.size()];
    if (frames > total - n) frames = static_cast<uint32_t>(total - n);
    player->Fill(l.data(), r.data(), frames);
    for (uint32_t i = 0; i < frames; ++i, ++n) {
      const int32_t want = ReferenceCode(p, n);
      if (l[i] != CodeToFloat(want) || r[i] != l[i]) {
        if (*ok) std::printf("FAIL: frame %llu: %.9g / %.9g, expected code %d\n", static_cast<unsigned long long>(n),
                             static_cast<double>(l[i]), static_cast<double>(r[i]), want);
        *ok = false;
      }
      const auto u = static_cast<uint32_t>(want);
      for (int s = 0; s < 32; s += 8) {
        digest ^= (u >> s) & 0xFFu;
        digest *= 1099511628211ull;
      }
    }
  }
  if (player->Frames() != static_cast<uint32_t>(total)) *ok = false;
  if (!p.loop && total > p.Frames() && !player->Done()) *ok = false;
  if (p.loop && player->Done()) *ok = false;
  return digest;
}

void TestPlayer() {
  auto p = std::make_unique<Program>();
  BuildTest(p.get());
  const uint64_t total = p->Frames() + 4800u;  // and 0.1 s of the silence after it
  // The program's digest, pinned with kProgramVersion: a change to any code of the program must
  // bump the version and dac_test.py's PROGRAM with it.
  constexpr uint64_t kProgramDigest = 0x1421525b2f4912b3ULL;
  static_assert(kProgramVersion == 1, "re-pin kProgramDigest for the new program");
  uint64_t first = 0;
  const std::vector<std::vector<uint32_t>> splits = {{48}, {1}, {7, 13, 1, 48, 333}, {512}, {47, 49}};
  for (const auto& blocks : splits) {
    bool           ok     = true;
    const uint64_t digest = Play(*p, total, blocks, &ok);
    Check(ok, "the player matches the reference model, whatever the blocks");
    if (first == 0) first = digest;
    Check(digest == first, "the same digest under every block split");
  }
  std::printf("test program v%u: %llu frames, digest 0x%016llx\n", kProgramVersion,
              static_cast<unsigned long long>(p->Frames()), static_cast<unsigned long long>(first));
  Check(first == kProgramDigest, "the test program's digest is the pinned one");

  for (const Wave w : {Wave::Sine, Wave::Isp}) {
    BuildSweep(p.get(), w);
    bool ok = true;
    Play(*p, p->Frames() * 2u + 1000u, {48, 5}, &ok);
    Check(ok, "a sweep repeats, phase-continuous");
  }
  BuildSteady(p.get(), Wave::Sine, 0);
  bool ok = true;
  Play(*p, 3u * kSampleRate + 7u, {48}, &ok);
  Check(ok, "a steady tone plays until stopped");

  // A program that never started, or an empty one, plays silence.
  auto player = std::make_unique<Player>();
  float l[48], r[48];
  player->Fill(l, r, 48);
  bool silent = true;
  for (int i = 0; i < 48; ++i) silent = silent && l[i] == 0.0f && r[i] == 0.0f;
  Check(silent && player->Done(), "a player with no program is silent");
  Program empty;
  player->Start(empty);
  player->Fill(l, r, 48);
  Check(player->Done() && l[0] == 0.0f, "an empty program is silent");
}

void PrintProgramJson() {
  auto p = std::make_unique<Program>();
  BuildTest(p.get());
  std::printf("{\"version\":%u,\"sampleRate\":%u,\"period\":%u,\"codecMax\":%d,\"frames\":%llu,\"steps\":[",
              kProgramVersion, kSampleRate, kPeriod, kCodecMax, static_cast<unsigned long long>(p->Frames()));
  for (uint32_t i = 0; i < p->count; ++i) {
    const Step& s = p->steps[i];
    std::printf("%s{\"wave\":\"%s\",\"centiDb\":%d,\"q\":%d,\"frames\":%u,\"codes\":[", i ? "," : "",
                WaveName(s.wave), s.centiDb, s.q, s.frames);
    for (uint32_t k = 0; k < kPeriod; ++k) std::printf("%s%d", k ? "," : "", Code(s.wave, s.q, k));
    std::printf("]}");
  }
  std::printf("]}\n");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 2 && std::strcmp(argv[1], "--program-json") == 0) {
    PrintProgramJson();
    return 0;
  }
  TestSineTable();
  TestCodes();
  TestLevels();
  TestParse();
  TestPrograms();
  TestPlayer();
  std::printf("firmware_tone: %s\n", g_failures == 0 ? "ok" : "FAIL");
  return g_failures == 0 ? 0 : 1;
}
