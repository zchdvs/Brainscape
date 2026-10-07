// The Mix law re-measurement (docs/design/mode-compiler.md §7.1 R3b, §11.1, §11.3; record P1):
// the first factory set's static recipes (record §2.2, draft v2, at sound revision 3's
// vocabulary: the trims are wet_trim_db, onset and mark are mode structure) rendered over the
// curation probe's three scores (probe/probe.cpp: an 8-pluck phrase, a 4-string strum, a soft
// 3-tone pad) and the shared test-signal vectors Plucks, Strums and SoftNotes (10 s each), to
// measure how loud each recipe plays engaged against bypass under revision 2's linear crossfade
// and revision 3's Mix law.
//
// The wet path never reads Mix, so one engine render at Mix 1 is the wet signal, and the output
// at the stored Mix m is dry·gd(m) + wet·gw(m) for either law: revision 2's (1 − m, m) is
// composed from it, and revision 3's is rendered by the engine and checked against the same
// composition bit for bit (column "exact"). Levels: RMS in dB over the frames the input sounds
// (the record's method), and integrated K-weighted loudness (ITU-R BS.1770-4: the two-stage K
// filter at 48 kHz, 400 ms blocks with 75 % overlap, the -70 LKFS and -10 LU gates; computed here
// in binary64, outside dsp/) over the same frames. "Engaged" is the output at the stored Mix
// against the dry input, which is what bypass plays; the pre-screen (§11.3) passes it from -1 to
// +4 LU. "Level" is the wet at Mix 1 against the dry, which wet_trim_db sets to within ±2 LU.
//
//   cmake -S tools/parity/modes/mixlaw -B build/mixlaw && cmake --build build/mixlaw --config Release
//   build/mixlaw/Release/mixlaw_probe [recipe]
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "brainscape/Engine.h"
#include "brainscape/HostArenas.h"
#include "brainscape/Mode.h"
#include "brainscape/Preset.h"
#include "brainscape/PresetState.h"
#include "brainscape/SoundRevision.h"
#include "brainscape/TestSignal.h"

using namespace brainscape;
using P = ParamId;

namespace {

constexpr double   kSr     = 48000.0;
constexpr uint32_t kBlock  = 512;

struct Leaf {
  ParamId id;
  float   value;
};
struct Recipe {
  const char*       name;
  const char*       note;  // what the static recipe leaves out
  bool              onset, mark;
  std::vector<Leaf> leaves;
};

// Record §2.2's recipes as stored leaves (draft v2). The trims of Murmuration, Halation and
// Undertow were OutTrim at revision 1, which scaled the dry too; stored as ID 4 they scale the
// wet only since revision 2. Echolalia and Déjà Vu take the record's "about -3 dB once wet-only".
const std::vector<Recipe>& Recipes() {
  static const std::vector<Recipe> r = {
      {"Engram", "v2, on the post delay", false, false,
       {{P::DelayMs, 1}, {P::Mix, 0.35f}, {P::Feedback, 0}, {P::GrainSizeMs, 100}, {P::Overlap, 0},
        {P::SprayMs, 0}, {P::Jitter, 0}, {P::WindowSustain, 1}, {P::WindowSmooth, 0},
        {P::PanSpread, 0}, {P::ModDepth, 0.05f}, {P::ModRateHz, 0.6f}, {P::DelayTimeMs, 405},
        {P::DelayFb, 0.45f}, {P::DelayMix, 1}, {P::ReverbMix, 0.12f}, {P::ReverbTime, 0.4f}}},
      {"Callback", "", false, false,
       {{P::DelayMs, 250}, {P::Mix, 0.4f}, {P::Feedback, 0}, {P::GrainSizeMs, 100}, {P::Overlap, 0},
        {P::SprayMs, 0}, {P::Jitter, 0}, {P::WindowSustain, 1}, {P::WindowSmooth, 0},
        {P::PanSpread, 0}, {P::DelayTimeMs, 375}, {P::DelayFb, 0.5f}, {P::DelayMix, 0.55f},
        {P::ReverbMix, 0.12f}}},
      {"Retrograde", "", false, false,
       {{P::DelayMs, 40}, {P::Mix, 0.5f}, {P::Feedback, 0.35f}, {P::GrainSizeMs, 400},
        {P::Overlap, 0.32f}, {P::SprayMs, 0}, {P::Jitter, 0}, {P::ReverseProb, 1},
        {P::WindowSustain, 0.6f}, {P::WindowSkew, 0.5f}, {P::WindowSmooth, 1}, {P::PanSpread, 0.3f},
        {P::ReverbMix, 0.2f}}},
      {"Updraft", "", false, false,
       {{P::DelayMs, 400}, {P::Mix, 0.45f}, {P::Feedback, 0.55f}, {P::GrainSizeMs, 120},
        {P::Overlap, 0.45f}, {P::SprayMs, 8}, {P::Jitter, 0.15f}, {P::TransposeSt, 12},
        {P::SpreadCents, 6}, {P::WindowSustain, 0.35f}, {P::WindowSmooth, 0.9f},
        {P::PanSpread, 0.6f}, {P::ReverbMix, 0.3f}, {P::ReverbTime, 0.6f}}},
      {"Pinhole", "", false, false,
       {{P::DelayMs, 330}, {P::Mix, 0.5f}, {P::Feedback, 0.6f}, {P::GrainSizeMs, 100}, {P::Overlap, 0},
        {P::SprayMs, 0}, {P::Jitter, 0}, {P::WindowSustain, 1}, {P::WindowSmooth, 0},
        {P::PanSpread, 0}, {P::FilterMorph, 1}, {P::FilterCutoffHz, 900}, {P::FilterRes, 0.55f},
        {P::ReverbMix, 0.25f}}},
      {"Murmuration", "", false, false,
       {{P::DelayMs, 300}, {P::Mix, 0.55f}, {P::Feedback, 0.4f}, {P::GrainSizeMs, 140},
        {P::Overlap, 0.8f}, {P::SprayMs, 600}, {P::Jitter, 1}, {P::SpreadCents, 25},
        {P::ReverseProb, 0.35f}, {P::WindowSustain, 0.15f}, {P::WindowSmooth, 1}, {P::PanSpread, 1},
        {P::ModDepth, 0.15f}, {P::ReverbMix, 0.5f}, {P::ReverbTime, 0.7f}, {P::WetTrimDb, 2}}},
      {"Halation", "static: +12 st, not W1's set {0, +12}", false, false,
       {{P::DelayMs, 450}, {P::Mix, 0.5f}, {P::Feedback, 0.35f}, {P::GrainSizeMs, 180},
        {P::Overlap, 0.55f}, {P::SprayMs, 80}, {P::Jitter, 0.6f}, {P::TransposeSt, 12},
        {P::SpreadCents, 8}, {P::WindowSustain, 0.3f}, {P::WindowSmooth, 1}, {P::PanSpread, 0.8f},
        {P::ReverbMix, 0.4f}, {P::ReverbTime, 0.65f}, {P::WetTrimDb, 2}}},
      {"Undertow", "static: -12 st, not W1's set {0, -12}", false, false,
       {{P::DelayMs, 120}, {P::Mix, 0.5f}, {P::Feedback, 0.3f}, {P::GrainSizeMs, 220},
        {P::Overlap, 0.55f}, {P::SprayMs, 60}, {P::Jitter, 0.5f}, {P::TransposeSt, -12},
        {P::SpreadCents, 6}, {P::WindowSustain, 0.3f}, {P::WindowSmooth, 1}, {P::PanSpread, 0.7f},
        {P::ReverbMix, 0.3f}, {P::WetTrimDb, 1}}},
      {"Lull", "", false, false,
       {{P::DelayMs, 600}, {P::Mix, 0.55f}, {P::Feedback, 0.95f}, {P::GrainSizeMs, 450},
        {P::Overlap, 0.6f}, {P::SprayMs, 150}, {P::Jitter, 0.5f}, {P::SpreadCents, 10},
        {P::WindowSustain, 0.2f}, {P::WindowSmooth, 1}, {P::PanSpread, 0.9f}, {P::ModDepth, 0.2f},
        {P::ModRateHz, 0.2f}, {P::ReverbMix, 0.55f}, {P::ReverbTime, 0.85f}}},
      {"Echolalia", "static: no W1 decay_ms", true, true,
       {{P::TriggerSens, 0.65f}, {P::DelayMs, 250}, {P::Mix, 0.5f}, {P::Feedback, 0.15f},
        {P::GrainSizeMs, 180}, {P::Overlap, 0}, {P::SprayMs, 0}, {P::Jitter, 0},
        {P::WindowSustain, 0.6f}, {P::WindowSkew, 0.15f}, {P::WindowSmooth, 0.5f},
        {P::PanSpread, 0.3f}, {P::ReverbMix, 0.3f}, {P::WetTrimDb, -3}}},
      {"DejaVu", "static: no W1 decay_ms or burst", true, true,
       {{P::TriggerSens, 0.65f}, {P::DelayMs, 250}, {P::Mix, 0.5f}, {P::Feedback, 0.1f},
        {P::GrainSizeMs, 300}, {P::Overlap, 0.7f}, {P::SprayMs, 4}, {P::Jitter, 0.6f},
        {P::SpreadCents, 4}, {P::WindowSustain, 0.3f}, {P::WindowSmooth, 1}, {P::PanSpread, 0.9f},
        {P::ReverbMix, 0.4f}, {P::WetTrimDb, -3}}},
      {"Kaleido", "static: +12 st, no W1 repeat or set", false, true,
       {{P::TriggerSens, 0.65f}, {P::DelayMs, 300}, {P::Mix, 0.5f}, {P::Feedback, 0.3f},
        {P::GrainSizeMs, 240}, {P::Overlap, 0.4f}, {P::SprayMs, 40}, {P::Jitter, 0.35f},
        {P::TransposeSt, 12}, {P::WindowSustain, 0.5f}, {P::WindowSmooth, 0.8f},
        {P::PanSpread, 0.7f}, {P::ReverbMix, 0.35f}}},
      {"Refrain", "static: no W1 set, repeat or voice_count", false, false,
       {{P::DelayMs, 250}, {P::Mix, 0.5f}, {P::GrainSizeMs, 120}, {P::Overlap, 0.5f},
        {P::WindowSustain, 0.5f}, {P::WindowSmooth, 0.8f}, {P::PanSpread, 0.7f},
        {P::ReverbMix, 0.25f}}},
      {"Shards", "static: onset OR'd with the scheduler (W1: onset only)", true, true,
       {{P::GrainSizeMs, 70}, {P::Overlap, 0.45f}, {P::SprayMs, 400}, {P::Jitter, 1},
        {P::ReverseProb, 0.5f}, {P::TransposeSt, 12}, {P::WindowSustain, 0.7f},
        {P::WindowSkew, 0.1f}, {P::WindowSmooth, 0.2f}, {P::PanSpread, 1}, {P::DelayMix, 0.2f},
        {P::DelayTimeMs, 250}}},
      {"Afterimage", "reserve", false, false,
       {{P::DelayMs, 30}, {P::Mix, 0.45f}, {P::Feedback, 0.25f}, {P::GrainSizeMs, 80},
        {P::Overlap, 0.62f}, {P::SprayMs, 60}, {P::Jitter, 1}, {P::SpreadCents, 12},
        {P::WindowSustain, 0.2f}, {P::WindowSmooth, 1}, {P::PanSpread, 0.8f}, {P::ReverbMix, 0.35f}}},
      {"Runaway", "reserve", false, false,
       {{P::DelayMs, 500}, {P::Mix, 0.5f}, {P::Feedback, 1.05f}, {P::GrainSizeMs, 200},
        {P::Overlap, 0.5f}, {P::SprayMs, 30}, {P::Jitter, 0.4f}, {P::TransposeSt, 12},
        {P::WindowSustain, 0.3f}, {P::WindowSmooth, 1}, {P::PanSpread, 0.7f}, {P::ReverbMix, 0.6f},
        {P::ReverbTime, 0.9f}}},
  };
  return r;
}

struct Score {
  std::string                   name;
  std::vector<testsignal::Note> notes;
  uint32_t                      inputEnd = 0;  // frames the input sounds in: the measurement
  uint32_t                      frames   = 0;
};

testsignal::Note Pluck(uint32_t start, uint32_t len, double fHz, double lvl, uint32_t seed) {
  testsignal::Note n;
  n.kind    = testsignal::Kind::Pluck;
  n.start   = start;
  n.length  = len;
  n.level   = static_cast<int32_t>(lvl * 8388607.0);
  n.pitch   = static_cast<uint32_t>(kSr / fHz + 0.5);
  n.seed    = seed;
  n.release = 2400;
  return n;
}
testsignal::Note Tone(uint32_t start, uint32_t len, double fHz, double lvl) {
  testsignal::Note n;
  n.kind    = testsignal::Kind::Tone;
  n.start   = start;
  n.length  = len;
  n.level   = static_cast<int32_t>(lvl * 8388607.0);
  n.pitch   = static_cast<uint32_t>(fHz * 1000.0 + 0.5);
  n.attack  = 24000;
  n.release = 24000;
  return n;
}

// The curation probe's scores (probe/probe.cpp), then the shared vectors (TestSignal.h).
std::vector<Score> Scores() {
  std::vector<Score> s;
  {
    Score sc{"phrase", {}, 0, 12 * 48000};
    const double f[8] = {110, 146.8, 196, 220, 261.6, 329.6, 392, 440};
    for (int i = 0; i < 8; ++i) sc.notes.push_back(Pluck(12000 + i * 24000, 33600, f[i], 0.35, 100 + i));
    sc.inputEnd = 12000 + 7 * 24000 + 33600;
    s.push_back(sc);
  }
  {
    Score sc{"chord", {}, 0, 12 * 48000};
    const double f[4] = {146.8, 220, 293.7, 370};
    for (int i = 0; i < 4; ++i) sc.notes.push_back(Pluck(14400 + i * 1200, 144000, f[i], 0.25, 200 + i));
    sc.inputEnd = 14400 + 3 * 1200 + 144000;
    s.push_back(sc);
  }
  {
    Score sc{"pad", {}, 0, 12 * 48000};
    const double f[3] = {220, 277.18, 329.63};
    for (int i = 0; i < 3; ++i) sc.notes.push_back(Tone(12000, 192000, f[i], 0.15));
    sc.inputEnd = 12000 + 192000;
    s.push_back(sc);
  }
  for (const testsignal::Vector v :
       {testsignal::Vector::Plucks, testsignal::Vector::Strums, testsignal::Vector::SoftNotes}) {
    Score          sc{testsignal::VectorName(v), {}, 10 * 48000, 12 * 48000};
    const uint32_t n = testsignal::BuildVector(v, sc.inputEnd, nullptr, 0);
    sc.notes.resize(n);
    testsignal::BuildVector(v, sc.inputEnd, sc.notes.data(), n);
    s.push_back(sc);
  }
  return s;
}

struct Stereo {
  std::vector<float> l, r;
};

Stereo Input(const Score& sc) {
  testsignal::Generator g;
  g.Start(sc.notes.data(), static_cast<uint32_t>(sc.notes.size()));
  Stereo in{std::vector<float>(sc.frames), std::vector<float>(sc.frames)};
  g.Render(in.l.data(), in.r.data(), sc.frames);
  return in;
}

std::unique_ptr<PresetState> State(const Recipe& rc, float mix) {
  auto s = std::make_unique<PresetState>();
  for (size_t i = 0; i < kNumLeafParams; ++i) {
    s->leaves[i] = {static_cast<uint32_t>(LeafId(i)), FindParam(LeafId(i))->def};
  }
  s->leafCount = static_cast<uint32_t>(kNumLeafParams);
  for (const Leaf& kv : rc.leaves) s->leaves[LeafIndex(kv.id)].value = kv.value;
  s->leaves[LeafIndex(P::Mix)].value = mix;
  if (rc.onset) s->mode.schedule.sources = static_cast<uint8_t>(s->mode.schedule.sources | kSourceOnset);
  if (rc.mark) s->mode.layers[0].source = PositionSource::Mark;
  s->mode.features = RequiredModeFeatures(s->mode);
  ComputeModeHash(s->mode, &s->mode.modeHash);
  return s;
}

Stereo Render(Engine& eng, const PresetState& ps, const Stereo& in, bool* exact) {
  LoadReport rep;
  eng.LoadPreset(ps, LoadMode::Exact, &rep);
  *exact = rep.exact;
  const size_t frames = in.l.size();
  Stereo       out{std::vector<float>(frames), std::vector<float>(frames)};
  for (size_t pos = 0; pos < frames; pos += kBlock) {
    const auto   n       = static_cast<uint32_t>(frames - pos < kBlock ? frames - pos : kBlock);
    const float* ins[2]  = {in.l.data() + pos, in.r.data() + pos};
    float*       outs[2] = {out.l.data() + pos, out.r.data() + pos};
    Engine::ProcessContext ctx;
    ctx.in        = ins;
    ctx.out       = outs;
    ctx.numFrames = n;
    eng.Process(ctx);
  }
  return out;
}

double Db(double x) { return x > 1e-30 ? 10.0 * std::log10(x) : -300.0; }

// Mean square over both channels' frames in [0, end) (the record's RMS, as power).
double MeanSquare(const Stereo& s, uint32_t end) {
  double acc = 0;
  for (uint32_t i = 0; i < end; ++i) {
    acc += 0.5 * (double(s.l[i]) * s.l[i] + double(s.r[i]) * s.r[i]);
  }
  return acc / end;
}

// Integrated loudness, ITU-R BS.1770-4, of frames [0, end) (the K filter runs from frame 0).
double Lufs(const Stereo& s, uint32_t end) {
  struct Biquad {
    double b0, b1, b2, a1, a2, z1 = 0, z2 = 0;
    double Run(double x) {
      const double y = b0 * x + z1;
      z1             = b1 * x - a1 * y + z2;
      z2             = b2 * x - a2 * y;
      return y;
    }
  };
  std::vector<double> sq[2];
  for (int c = 0; c < 2; ++c) {
    Biquad shelf{1.53512485958697, -2.69169618940638, 1.19839281085285, -1.69065929318241,
                 0.73248077421585};
    Biquad hp{1.0, -2.0, 1.0, -1.99004745483398, 0.99007225036621};
    const std::vector<float>& x = c == 0 ? s.l : s.r;
    sq[c].resize(end);
    for (uint32_t i = 0; i < end; ++i) {
      const double y = hp.Run(shelf.Run(x[i]));
      sq[c][i]       = y * y;
    }
  }
  const uint32_t      block = 19200, hop = 4800;
  std::vector<double> z;
  for (uint32_t a = 0; a + block <= end; a += hop) {
    double ms = 0;
    for (int c = 0; c < 2; ++c) {
      double acc = 0;
      for (uint32_t i = a; i < a + block; ++i) acc += sq[c][i];
      ms += acc / block;
    }
    z.push_back(ms);
  }
  auto loud      = [](double ms) { return -0.691 + Db(ms); };
  auto gatedMean = [&](double gate) {
    double acc = 0;
    size_t n   = 0;
    for (double ms : z) {
      if (loud(ms) > -70.0 && loud(ms) > gate) {
        acc += ms;
        ++n;
      }
    }
    return n == 0 ? 0.0 : acc / double(n);
  };
  const double relative = loud(gatedMean(-70.0)) - 10.0;
  return loud(gatedMean(relative));
}

double Peak(const Stereo& s) {
  double pk = 0;
  for (size_t i = 0; i < s.l.size(); ++i) {
    pk = std::fmax(pk, std::fabs(s.l[i]));
    pk = std::fmax(pk, std::fabs(s.r[i]));
  }
  return 2.0 * Db(pk);  // dBFS: Db is 10·log10 of a power
}

// out = dry·gd + wet·gw, sample by sample in binary32 as the engine's mix does.
Stereo Compose(const Stereo& dry, const Stereo& wet, float gd, float gw) {
  Stereo o{std::vector<float>(dry.l.size()), std::vector<float>(dry.l.size())};
  for (size_t i = 0; i < dry.l.size(); ++i) {
    o.l[i] = dry.l[i] * gd + wet.l[i] * gw;
    o.r[i] = dry.r[i] * gd + wet.r[i] * gw;
  }
  return o;
}

const char* Verdict(double engaged) { return engaged >= -1.0 && engaged <= 4.0 ? "pass" : "FAIL"; }

}  // namespace

int main(int argc, char** argv) {
  const std::string only = argc > 1 ? argv[1] : "";
  EngineConfig      cfg;  // canonical: 48 kHz, 2^22 ring, stereo, dither on the ring write
  host::HeapArenas  arenas(PlanMemory(cfg));
  Engine            eng;
  if (!arenas.ok() || !eng.Init(cfg, arenas.get())) {
    std::fprintf(stderr, "init failed\n");
    return 1;
  }
  std::printf("# mixlaw_probe at sound revision %u; levels over the frames the input sounds\n",
              static_cast<unsigned>(kSoundRevision));
  std::printf("# %-11s %-10s %4s | %6s %6s %6s %6s | %6s %6s %-4s | %6s %6s %-4s %-4s | %6s %6s %6s | %s\n",
              "recipe", "input", "mix", "dryRMS", "W-D", "O2-D", "O3-D", "dryLU", "W-D", "lvl",
              "E2", "E3", "r2", "r3", "pkDry", "pk2", "pk3", "exact");
  const auto scores = Scores();
  std::vector<Stereo> inputs;
  for (const Score& sc : scores) inputs.push_back(Input(sc));
  int failures = 0;
  for (const Recipe& rc : Recipes()) {
    if (!only.empty() && only != rc.name) continue;
    float mix = 0.5f;
    for (const Leaf& kv : rc.leaves) {
      if (kv.id == P::Mix) mix = kv.value;
    }
    // The two laws' gains at the stored Mix: revision 2's crossfade and revision 3's law.
    const float g2d = 1.0f - mix, g2w = mix;
    const float g3d = mix <= 0.5f ? 1.0f : 2.0f * (1.0f - mix), g3w = mix >= 0.5f ? 1.0f : 2.0f * mix;
    for (size_t k = 0; k < scores.size(); ++k) {
      const Score&  sc = scores[k];
      const Stereo& in = inputs[k];
      bool          ex1 = false, exm = false;
      const Stereo  wet = Render(eng, *State(rc, 1.0f), in, &ex1);
      const Stereo  o3  = Render(eng, *State(rc, mix), in, &exm);
      const Stereo  o2  = Compose(in, wet, g2d, g2w);
      const Stereo  c3  = Compose(in, wet, g3d, g3w);
      size_t        differ = 0;  // frames whose bits differ (a -0 against a +0 included)
      for (size_t i = 0; i < in.l.size(); ++i) {
        differ += (std::memcmp(&o3.l[i], &c3.l[i], sizeof(float)) == 0 &&
                   std::memcmp(&o3.r[i], &c3.r[i], sizeof(float)) == 0)
                      ? 0u
                      : 1u;
      }
      if (!ex1 || !exm || differ != 0u) ++failures;
      const uint32_t end   = sc.inputEnd;
      const double   dry   = MeanSquare(in, end);
      const double   dryLu = Lufs(in, end);
      const double   wetLu = Lufs(wet, end), e2 = Lufs(o2, end) - dryLu, e3 = Lufs(o3, end) - dryLu;
      std::printf("%-13s %-10s %4.2f | %6.1f %+6.1f %+6.1f %+6.1f | %6.1f %+6.1f %-4s | %+6.1f %+6.1f %-4s %-4s | %+6.1f %+6.1f %+6.1f | %s\n",
                  rc.name, sc.name.c_str(), mix, Db(dry), Db(MeanSquare(wet, end)) - Db(dry),
                  Db(MeanSquare(o2, end)) - Db(dry), Db(MeanSquare(o3, end)) - Db(dry), dryLu,
                  wetLu - dryLu, std::fabs(wetLu - dryLu) <= 2.0 ? "ok" : "trim", e2, e3,
                  Verdict(e2), Verdict(e3), Peak(in), Peak(o2), Peak(o3),
                  differ == 0u && ex1 && exm ? "yes" : "NO");
    }
  }
  std::printf("# %s\n", failures == 0 ? "every revision-3 render is the law's composition, bit for bit"
                                      : "SOME RENDERS DIFFER FROM THE COMPOSITION OR LOADED INEXACT");
  return failures == 0 ? 0 : 1;
}
