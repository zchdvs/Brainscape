#include "Corpus.h"

#include <algorithm>

// Parameter values are float literals, or an integer converted and then put through
// at most ONE float operation: a single IEEE operation rounds the same everywhere
// and cannot be contracted, so the harness's own flags never change a script.
namespace brainscape::golden {

namespace {

using testsignal::Vector;
using C = Counter;
using P = ParamId;

constexpr int64_t  S(int64_t seconds) { return seconds * 48000; }
constexpr uint32_t Frames(uint32_t seconds) { return seconds * 48000u; }
float              I2F(int64_t v) { return static_cast<float>(v); }

PresetCase Preset(const char* name, std::vector<std::pair<ParamId, float>> params) {
  PresetCase p;
  p.name   = name;
  p.params = std::move(params);
  return p;
}

// The contract-#1 freeze case of profile §5.7 (pitch 7 st, reverse 0.3, spray 50 ms,
// POS_MARK, onset trigger): freeze engages at an odd frame with onsets still arriving,
// so marks newer than the pin position grains (mechanism D1).
PresetCase FreezeMarks() {
  PresetCase p = Preset("freeze_marks",
      {{P::DelayMs, 100.0f}, {P::Mix, 1.0f}, {P::GrainSizeMs, 60.0f}, {P::Overlap, 0.55f},
       {P::SprayMs, 50.0f}, {P::PitchSt, 7.0f}, {P::SpreadCents, 20.0f}, {P::ReverseProb, 0.3f},
       {P::Jitter, 1.0f}, {P::TriggerSens, 0.8f}, {P::OnsetTrigger, 1.0f},
       {P::PositionSource, 1.0f}});
  p.script.Freeze(S(4) + 1, true);
  p.script.Freeze(S(11) + 37, false);
  p.require = {{C::Onsets, 10},       {C::FrozenOnsets, 3},  {C::FrozenFrames, S(6)},
               {C::FreezeEngages, 1}, {C::OffGridEvents, 2}};
  p.ablate  = {Feature::Freeze, Feature::MarkPosition, Feature::Reverse};
  return p;
}

// Every parameter moved every 250 ms at frames off the 48-frame grid, with freeze
// toggles and footswitch triggers among them (grown from the prototype battery).
PresetCase AutomationOffGrid(int64_t frames) {
  PresetCase p = Preset("automation_offgrid", {});
  Script&    s = p.script;
  int64_t    k = 0;
  for (int64_t base = 0; base < frames; base += S(1) / 4, ++k) {
    const int64_t f = base + 1 + (k * 37) % 47;  // never a multiple of 48
    s.Param(f, P::OutTrimDb, I2F((k * 7) % 49 - 24));
    s.Param(f, P::Feedback, I2F(k % 12) * 0.1f);
    s.Param(f, P::FilterCutoffHz, I2F(100 + 397 * (k % 51)));
    s.Param(f, P::FilterRes, I2F(k % 11) * 0.1f);
    s.Param(f, P::FilterMorph, I2F(k % 13) * 0.25f);
    s.Param(f, P::Overlap, I2F(k % 7) / 7.0f);
    s.Param(f, P::GrainSizeMs, I2F(5 + 37 * (k % 13)));
    s.Param(f, P::PitchSt, I2F((k * 5) % 49 - 24));
    s.Param(f, P::SpreadCents, I2F((k * 3) % 101));
    s.Param(f, P::Jitter, I2F(k % 5) * 0.25f);
    s.Param(f, P::SprayMs, I2F(25 * (k % 9)));
    s.Param(f, P::DelayMs, I2F(20 + 113 * (k % 40)));
    s.Param(f, P::ReverseProb, I2F(k % 4) * 0.25f);
    s.Param(f, P::WindowSustain, I2F(k % 6) * 0.2f);
    s.Param(f, P::WindowSkew, I2F((k * 3) % 5) * 0.25f);
    s.Param(f, P::WindowSmooth, I2F(k % 3) * 0.5f);
    s.Param(f, P::PanSpread, I2F(k % 5) * 0.25f);
    s.Param(f, P::ModRateHz, I2F(1 + k % 8) * 0.5f);
    s.Param(f, P::ModDepth, I2F(k % 3) * 0.4f);
    s.Param(f, P::DelayTimeMs, I2F(50 + 97 * (k % 20)));
    s.Param(f, P::DelayFb, I2F(k % 10) / 10.0f);  // 9 * 0.1f lands 1 ULP above the 0.9 max
    s.Param(f, P::DelayMix, I2F(k % 3) * 0.5f);
    s.Param(f, P::ReverbTime, I2F(k % 5) * 0.25f);
    s.Param(f, P::ReverbMix, I2F((k + 1) % 3) * 0.5f);
    s.Param(f, P::TriggerSens, I2F(k % 11) * 0.1f);
    s.Param(f, P::Mix, I2F(k % 5) * 0.25f);
    s.Param(f, P::OnsetTrigger, I2F(k % 2));
    s.Param(f, P::PositionSource, I2F((k / 2) % 2));
    if (k % 17 == 5) s.Freeze(f, true);
    if (k % 17 == 9) s.Freeze(f, false);
    if (k % 6 == 3) s.Trigger(f + 5);
  }
  p.require   = {{C::Events, 1000},     {C::OffGridEvents, 1000}, {C::Triggers, 5},
                 {C::FreezeEngages, 2}, {C::Onsets, 5}};
  p.invariant = {Invariance::HostileFpEnv};
  return p;
}

// Strum-style marks and onset triggering, the starting point of the load chain.
const ParamList kStrumMarks = {
    {P::DelayMs, 250.0f}, {P::Feedback, 0.5f},     {P::GrainSizeMs, 80.0f},   {P::Overlap, 0.6f},
    {P::Jitter, 0.5f},    {P::SprayMs, 30.0f},     {P::ReverseProb, 0.2f},    {P::OnsetTrigger, 1.0f},
    {P::PositionSource, 1.0f}, {P::TriggerSens, 0.6f}};

// Spillover loads as stamped events at odd frames (profile §5.10, §5.11): history, grains,
// marks, pending triggers and the scheduler phase carry over each load, the random-number
// epoch restarts at it, and events after a load at its frame apply after it.
PresetCase SpilloverChain() {
  PresetCase p = Preset("spillover_chain", kStrumMarks);
  Script&    s = p.script;
  // Into a reverse loop above unity feedback, so the trails are loud when the next load lands.
  s.Spillover(S(2) + 4321,
      {{P::DelayMs, 700.0f}, {P::Feedback, 1.05f}, {P::GrainSizeMs, 120.0f}, {P::Overlap, 0.3f},
       {P::Jitter, 0.0f}, {P::SprayMs, 0.0f}, {P::PitchSt, -12.0f}, {P::ReverseProb, 0.5f},
       {P::DelayMix, 0.4f}, {P::DelayTimeMs, 333.0f}, {P::DelayFb, 0.5f}});
  s.Trigger(S(5) + 1);
  s.Spillover(S(5) + 1,
      {{P::GrainSizeMs, 5.0f}, {P::Overlap, 1.0f}, {P::Jitter, 1.0f}, {P::SprayMs, 200.0f},
       {P::PitchSt, 7.0f}, {P::PanSpread, 1.0f}, {P::ReverbMix, 0.5f}, {P::ReverbTime, 0.8f},
       {P::Feedback, 0.3f}});
  s.Param(S(5) + 1, P::Mix, 0.8f);
  s.Param(S(5) + 1, P::FilterCutoffHz, 1500.0f);
  s.Spillover(S(8) + 77, kStrumMarks);
  s.Trigger(S(8) + 77);
  p.require   = {{C::Loads, 3, 3},         {C::Triggers, 2, 2}, {C::Events, 7},
                 {C::OffGridEvents, 7},    {C::Onsets, 10},     {C::FbAbove1Frames, S(2)}};
  p.ablate    = {Feature::Spillover};
  p.invariant = {Invariance::HostileFpEnv};
  return p;
}

// Restart mid-render with the parameters kept (profile §5.8): the engine is frozen, holds
// marks and a Spillover epoch, has two triggers still due and feedback ringing through
// every post stage, and must come back exactly as a fresh engine with those values.
PresetCase RestartKeptParams() {
  const ParamList base = {
      {P::DelayMs, 180.0f},     {P::Feedback, 0.8f},       {P::GrainSizeMs, 70.0f},
      {P::Overlap, 0.7f},       {P::SprayMs, 40.0f},       {P::PitchSt, 5.0f},
      {P::ReverseProb, 0.3f},   {P::Jitter, 0.6f},         {P::OnsetTrigger, 1.0f},
      {P::PositionSource, 1.0f}, {P::DelayMix, 0.3f},      {P::DelayTimeMs, 420.0f},
      {P::DelayFb, 0.6f},       {P::ReverbMix, 0.3f},      {P::ReverbTime, 0.7f}};
  ParamList loaded = base;
  loaded.emplace_back(P::PitchSt, -7.0f);
  loaded.emplace_back(P::GrainSizeMs, 40.0f);
  loaded.emplace_back(P::Feedback, 0.9f);
  PresetCase p = Preset("restart_kept_params", base);
  Script&    s = p.script;
  s.Param(S(1) + 17, P::DelayMs, 333.0f);
  s.Freeze(S(2) + 101, true);
  s.Spillover(S(3) + 55, loaded);
  s.Param(S(3) + 2000, P::Mix, 0.7f);
  s.Freeze(S(4) + 7, true);
  for (int i = 0; i < 3; ++i) s.Trigger(S(6) + 9);  // one per frame from S(6) + 9
  s.Restart(S(6) + 10);
  s.Param(S(6) + 10, P::PitchSt, 12.0f);  // frame 0 of the restarted timeline
  s.Freeze(S(9) + 3, true);
  s.Trigger(S(10) + 5);
  p.require   = {{C::Restarts, 1, 1},     {C::Loads, 1, 1},  {C::Triggers, 4, 4},
                 {C::FreezeEngages, 3},   {C::Onsets, 10},   {C::OffGridEvents, 10}};
  p.ablate    = {Feature::Restart};
  p.invariant = {Invariance::RestartTail, Invariance::HostileFpEnv};
  return p;
}

// An Exact load mid-render (profile §5.10): it cuts a self-oscillating loop and starts a
// different preset from the exact-restart state.
PresetCase ExactLoadMid() {
  PresetCase p = Preset("exact_load_mid",
      {{P::Feedback, 1.1f}, {P::ReverseProb, 1.0f}, {P::DelayMs, 300.0f}, {P::GrainSizeMs, 100.0f},
       {P::Overlap, 0.25f}, {P::SprayMs, 0.0f}, {P::Jitter, 0.0f}, {P::WindowSustain, 1.0f},
       {P::WindowSmooth, 0.0f}, {P::PanSpread, 0.0f}});
  p.script.ExactLoad(S(7) + 12345,
      {{P::GrainSizeMs, 30.0f}, {P::Overlap, 0.9f}, {P::Jitter, 1.0f}, {P::SprayMs, 60.0f},
       {P::PitchSt, 12.0f}, {P::SpreadCents, 30.0f}, {P::OnsetTrigger, 1.0f},
       {P::PositionSource, 1.0f}, {P::ModDepth, 0.5f}, {P::FilterCutoffHz, 2500.0f},
       {P::FilterMorph, 1.0f}});
  p.script.Param(S(7) + 12345, P::Feedback, 0.6f);
  p.require   = {{C::Restarts, 1, 1}, {C::FbAbove1Frames, S(7)}, {C::Onsets, 10}};
  p.ablate    = {Feature::Restart};
  p.invariant = {Invariance::RestartTail};
  return p;
}

}  // namespace

const char* CounterName(Counter c) noexcept {
  switch (c) {
    case C::Frames: return "frames";
    case C::Events: return "events";
    case C::OffGridEvents: return "offGridEvents";
    case C::Triggers: return "triggers";
    case C::Loads: return "loads";
    case C::Restarts: return "restarts";
    case C::Onsets: return "onsets";
    case C::FrozenOnsets: return "frozenOnsets";
    case C::FrozenFrames: return "frozenFrames";
    case C::FreezeEngages: return "freezeEngages";
    case C::FbAbove1Frames: return "fbAbove1Frames";
    case C::InClipFrames: return "inClipFrames";
    case C::SilentInFrames: return "silentInFrames";
    case C::OutActiveFrames: return "outActiveFrames";
    case C::TailActiveFrames: return "tailActiveFrames";
    case C::LastActiveFrame: return "lastActiveFrame";
    case C::LastNonzeroFrame: return "lastNonzeroFrame";
    case C::kCount: break;
  }
  return "unknown";
}

const char* FeatureName(Feature f) noexcept {
  switch (f) {
    case Feature::MarkPosition: return "markPosition";
    case Feature::OnsetTrigger: return "onsetTrigger";
    case Feature::Reverse: return "reverse";
    case Feature::Pitch: return "pitch";
    case Feature::Spray: return "spray";
    case Feature::Feedback: return "feedback";
    case Feature::PostMod: return "postMod";
    case Feature::PostDelay: return "postDelay";
    case Feature::PostReverb: return "postReverb";
    case Feature::PostFilter: return "postFilter";
    case Feature::Freeze: return "freeze";
    case Feature::Triggers: return "triggers";
    case Feature::RingLength: return "ringLength";
    case Feature::Spillover: return "spillover";
    case Feature::Restart: return "restart";
  }
  return "unknown";
}

const char* InvarianceName(Invariance i) noexcept {
  switch (i) {
    case Invariance::HostileFpEnv: return "hostileFpEnv";
    case Invariance::RestartTail: return "restartTail";
  }
  return "unknown";
}

PresetCase Ablate(const PresetCase& in, Feature f) {
  PresetCase out = in;
  out.require.clear();
  out.ablate.clear();
  out.invariant.clear();
  auto& events = out.script.MutableEvents();
  auto  drop   = [&](auto pred) {
    events.erase(std::remove_if(events.begin(), events.end(), pred), events.end());
  };
  auto set = [](ParamList& params, ParamId id, float value) {
    bool found = false;
    for (auto& p : params) {
      if (p.first == id) { p.second = value; found = true; }
    }
    if (!found) params.emplace_back(id, value);
  };
  auto neutral = [&](ParamId id, float value) {
    set(out.params, id, value);
    for (ParamList& staged : out.script.MutableStaged()) set(staged, id, value);
    drop([id](const Event& e) {
      return e.type == EventType::SetParam && e.id == static_cast<uint32_t>(id);
    });
  };
  switch (f) {
    case Feature::MarkPosition: neutral(P::PositionSource, 0.0f); break;
    case Feature::OnsetTrigger: neutral(P::OnsetTrigger, 0.0f); break;
    case Feature::Reverse: neutral(P::ReverseProb, 0.0f); break;
    case Feature::Pitch: neutral(P::PitchSt, 0.0f); break;
    case Feature::Spray: neutral(P::SprayMs, 0.0f); break;
    case Feature::Feedback: neutral(P::Feedback, 0.0f); break;
    case Feature::PostMod: neutral(P::ModDepth, 0.0f); break;
    case Feature::PostDelay: neutral(P::DelayMix, 0.0f); break;
    case Feature::PostReverb: neutral(P::ReverbMix, 0.0f); break;
    case Feature::PostFilter: neutral(P::FilterCutoffHz, 20000.0f); break;  // exact bypass
    case Feature::Freeze:
      drop([](const Event& e) { return e.type == EventType::Freeze; });
      break;
    case Feature::Triggers:
      drop([](const Event& e) { return e.type == EventType::Trigger; });
      break;
    case Feature::RingLength: break;  // same preset; the harness doubles the ring
    case Feature::Spillover:
      drop([](const Event& e) { return e.type == EventType::SpilloverLoad; });
      break;
    case Feature::Restart: out.script.MutableRestarts().clear(); break;
  }
  return out;
}

ParamList ParamsAt(const PresetCase& p, int64_t frame) {
  float values[kNumParams];
  auto  load = [&values](const ParamList& params) {
    const std::unique_ptr<PresetState> preset = CompletePreset(params);
    for (size_t i = 0; i < kNumParams; ++i) values[i] = preset->leaves[i].value;
  };
  load(p.params);
  const auto& events   = p.script.Events();
  const auto& restarts = p.script.Restarts();
  size_t      e = 0, r = 0;
  // In time order; a restart comes before the events stamped at its frame.
  for (;;) {
    const bool eventDue   = e < events.size() && events[e].frame < frame;
    const bool restartDue = r < restarts.size() && restarts[r].frame < frame;
    if (!eventDue && !restartDue) break;
    if (restartDue && (!eventDue || restarts[r].frame <= events[e].frame)) {
      if (restarts[r].load) load(p.script.Staged()[restarts[r].staged]);
      ++r;
      continue;
    }
    const Event& ev = events[e++];
    if (ev.type == EventType::SetParam) {
      values[ev.id - 1u] = Canonicalize(static_cast<ParamId>(ev.id), ev.value);
    } else if (ev.type == EventType::SpilloverLoad) {
      load(p.script.Staged()[ev.id]);
    }
  }
  ParamList out;
  for (size_t i = 0; i < kNumParams; ++i) out.emplace_back(kParamTable[i].id, values[i]);
  return out;
}

PresetCase TailAfterRestart(const PresetCase& p, int64_t* start) {
  const RestartPoint& last = p.script.Restarts().back();
  *start                   = last.frame;
  PresetCase tail;
  tail.name   = p.name;
  tail.params = last.load ? p.script.Staged()[last.staged] : ParamsAt(p, last.frame);
  tail.script.MutableStaged() = p.script.Staged();
  for (const Event& e : p.script.Events()) {
    if (e.frame < last.frame) continue;
    Event moved = e;
    moved.frame -= last.frame;
    tail.script.MutableEvents().push_back(moved);
  }
  return tail;
}

// Not yet in the corpus: profile §6.1's committed guitar DI recordings, which enter
// through ConditionInput24 (§3.7).
std::vector<VectorCase> BuildCorpus() {
  std::vector<VectorCase> corpus;

  {  // Plucks: onsets on every note.
    VectorCase v{"plucks_12s", Vector::Plucks, Frames(10), Frames(12), false, {}};
    PresetCase def = Preset("default", {});  // engine defaults: jitter 0.2, spray 20 ms
    def.require    = {{C::Onsets, 10}};
    def.ablate     = {Feature::Spray};
    v.presets.push_back(def);

    // The fixed-schedule control: one unity voice, no randomness, feedback through the tamer.
    PresetCase clean = Preset("clean_delay",
        {{P::DelayMs, 300.0f}, {P::Feedback, 0.5f}, {P::GrainSizeMs, 100.0f}, {P::Overlap, 0.0f},
         {P::SprayMs, 0.0f}, {P::Jitter, 0.0f}, {P::WindowSustain, 1.0f}, {P::WindowSmooth, 0.0f},
         {P::PanSpread, 0.0f}});
    clean.ablate = {Feature::Feedback};
    v.presets.push_back(clean);

    // Strum: onset trigger + POS_MARK + footswitch triggers off the grid. Spray >= 30 ms
    // already saturates the normalization's decorrelation term, so the markPosition
    // ablation can only differ through mark-positioned births.
    PresetCase strum = Preset("strum_marks",
        {{P::OnsetTrigger, 1.0f}, {P::PositionSource, 1.0f}, {P::TriggerSens, 0.6f},
         {P::Overlap, 0.3f}, {P::GrainSizeMs, 150.0f}, {P::Jitter, 0.0f}, {P::SprayMs, 40.0f},
         {P::Feedback, 0.2f}});
    strum.script.Trigger(S(2) + 101);
    strum.script.Trigger(S(5) + 7);
    strum.script.Trigger(S(8) + 333);
    strum.require = {{C::Onsets, 10}, {C::Triggers, 3}, {C::OffGridEvents, 3}};
    strum.ablate  = {Feature::MarkPosition, Feature::OnsetTrigger, Feature::Triggers};
    v.presets.push_back(strum);

    PresetCase heavy = Preset("pitch_reverse_spray",
        {{P::Jitter, 1.0f}, {P::SprayMs, 400.0f}, {P::ReverseProb, 0.5f}, {P::SpreadCents, 60.0f},
         {P::PitchSt, 7.0f}, {P::Overlap, 0.85f}, {P::PanSpread, 1.0f}, {P::GrainSizeMs, 60.0f},
         {P::WindowSustain, 0.1f}, {P::WindowSkew, 0.2f}, {P::WindowSmooth, 1.0f}});
    heavy.ablate = {Feature::Pitch, Feature::Reverse, Feature::Spray};
    v.presets.push_back(heavy);

    // The position limits: maximum delay, spray and grain size, then with reverse
    // and +24 st on top.
    const std::vector<std::pair<ParamId, float>> kFar = {
        {P::DelayMs, 5000.0f}, {P::SprayMs, 2000.0f}, {P::GrainSizeMs, 500.0f}};
    PresetCase farPlain = Preset("max_delay_spray", kFar);
    farPlain.ablate     = {Feature::Spray};
    v.presets.push_back(farPlain);

    PresetCase farRevUp = Preset("max_delay_spray_rev_up24", kFar);
    farRevUp.params.emplace_back(P::ReverseProb, 0.5f);
    farRevUp.params.emplace_back(P::PitchSt, 24.0f);
    farRevUp.ablate = {Feature::Spray, Feature::Reverse, Feature::Pitch};
    v.presets.push_back(farRevUp);

    // Spray reflected off the near guard: a 500 ms grain at +24 st needs 1.5 s behind
    // the write head, so from a 1 ms base about 7 in 8 draws fold back into the bounds.
    PresetCase nearRail = Preset("near_rail_spray",
        {{P::DelayMs, 1.0f}, {P::SprayMs, 2000.0f}, {P::GrainSizeMs, 500.0f}, {P::PitchSt, 24.0f}});
    nearRail.ablate = {Feature::Spray};
    v.presets.push_back(nearRail);
    corpus.push_back(std::move(v));
  }

  {  // Strummed chords: freeze with marks newer than the pin, and the pitch extremes.
    VectorCase v{"strums_16s", Vector::Strums, Frames(16), Frames(16), false, {}};
    v.presets.push_back(FreezeMarks());

    PresetCase pitch = Preset("pitch_extremes",
        {{P::PitchSt, 24.0f}, {P::SpreadCents, 100.0f}, {P::Jitter, 0.3f}, {P::Feedback, 0.4f},
         {P::GrainSizeMs, 80.0f}});
    for (int64_t k = 1; k * 72000 < S(16); ++k) {
      pitch.script.Param(k * 72000 + 13, P::PitchSt, (k & 1) ? -24.0f : 24.0f);
    }
    pitch.require = {{C::Events, 9}, {C::OffGridEvents, 9}};
    pitch.ablate  = {Feature::Pitch};
    v.presets.push_back(pitch);

    // Freeze is a level settled per frame (profile §5.11): a release and a re-engage at
    // one frame keep the pin, and a Spillover load's freeze-off is immediate, so a freeze
    // after it at its frame pins anew. Live positioning, so every pin reaches the output.
    PresetCase retoggle = Preset("freeze_retoggle_spill",
        {{P::DelayMs, 150.0f}, {P::Mix, 1.0f}, {P::GrainSizeMs, 40.0f}, {P::Overlap, 0.8f},
         {P::SprayMs, 10.0f}, {P::PitchSt, 5.0f}, {P::Jitter, 0.5f}, {P::Feedback, 0.3f}});
    retoggle.script.Freeze(S(3) + 101, true);
    retoggle.script.Freeze(S(6) + 4999, false);
    retoggle.script.Freeze(S(6) + 4999, true);
    retoggle.script.Spillover(S(9) + 23,  // while frozen
        {{P::DelayMs, 333.0f}, {P::Mix, 1.0f}, {P::GrainSizeMs, 20.0f}, {P::Overlap, 1.0f},
         {P::PitchSt, -7.0f}, {P::Feedback, 0.5f}, {P::ReverbMix, 0.4f}});
    retoggle.script.Freeze(S(10) + 77, true);
    retoggle.script.Spillover(S(12) + 5,
        {{P::DelayMs, 90.0f}, {P::Mix, 1.0f}, {P::GrainSizeMs, 60.0f}, {P::SprayMs, 0.0f},
         {P::PitchSt, 12.0f}, {P::Feedback, 0.2f}});
    retoggle.script.Freeze(S(12) + 5, true);
    retoggle.script.Freeze(S(14) + 31, false);
    retoggle.require   = {{C::Events, 8},          {C::OffGridEvents, 8}, {C::FreezeEngages, 3, 3},
                          {C::FrozenFrames, S(9)}, {C::FrozenOnsets, 3},  {C::Loads, 2, 2}};
    retoggle.ablate    = {Feature::Freeze};
    retoggle.invariant = {Invariance::HostileFpEnv};
    v.presets.push_back(retoggle);
    corpus.push_back(std::move(v));
  }

  {  // Dense onsets driving 1 ms grains at the maximum birth rate.
    VectorCase v{"onset_bursts_6s", Vector::OnsetBursts, Frames(6), Frames(6), false, {}};
    PresetCase dense = Preset("dense_1ms",
        {{P::GrainSizeMs, 1.0f}, {P::Overlap, 1.0f}, {P::Jitter, 1.0f}, {P::SprayMs, 5.0f},
         {P::ReverseProb, 0.5f}, {P::OnsetTrigger, 1.0f}, {P::PanSpread, 1.0f}});
    dense.require = {{C::Onsets, 20}};
    dense.ablate  = {Feature::Reverse, Feature::OnsetTrigger};
    v.presets.push_back(dense);
    corpus.push_back(std::move(v));
  }

  {  // Soft notes: every post stage at its extremes, then swept.
    VectorCase v{"soft_notes_10s", Vector::SoftNotes, Frames(10), Frames(10), false, {}};
    PresetCase postMax = Preset("post_max",
        {{P::Feedback, 0.3f}, {P::ModDepth, 1.0f}, {P::ModRateHz, 10.0f}, {P::DelayMix, 1.0f},
         {P::DelayFb, 0.9f}, {P::DelayTimeMs, 2000.0f}, {P::ReverbMix, 1.0f}, {P::ReverbTime, 1.0f},
         {P::FilterCutoffHz, 40.0f}, {P::FilterRes, 1.0f}, {P::FilterMorph, 2.0f}});
    postMax.ablate    = {Feature::PostMod, Feature::PostDelay, Feature::PostReverb,
                         Feature::PostFilter};
    postMax.invariant = {Invariance::HostileFpEnv};
    v.presets.push_back(postMax);

    PresetCase sweep = Preset("post_sweep",
        {{P::ModDepth, 0.5f}, {P::ModRateHz, 0.01f}, {P::DelayMix, 0.5f}, {P::DelayFb, 0.6f},
         {P::DelayTimeMs, 10.0f}, {P::ReverbMix, 0.5f}, {P::ReverbTime, 0.0f},
         {P::FilterCutoffHz, 2000.0f}, {P::FilterRes, 0.5f}, {P::FilterMorph, 0.0f}});
    for (int64_t k = 1; k * 12000 < S(10); ++k) {
      const int64_t f = k * 12000 + 5;
      sweep.script.Param(f, P::FilterMorph, I2F(k % 13) * 0.25f);
      sweep.script.Param(f, P::FilterCutoffHz, I2F(60 + 487 * (k % 29)));
      sweep.script.Param(f, P::DelayTimeMs, I2F(10 + 97 * (k % 20)));
    }
    sweep.require = {{C::Events, 100}, {C::OffGridEvents, 100}};
    sweep.ablate  = {Feature::PostFilter};
    v.presets.push_back(sweep);

    v.presets.push_back(Preset("clean_fixed",
        {{P::DelayMs, 120.0f}, {P::GrainSizeMs, 50.0f}, {P::Overlap, 0.25f}, {P::SprayMs, 0.0f},
         {P::Jitter, 0.0f}, {P::WindowSustain, 1.0f}, {P::WindowSmooth, 0.0f},
         {P::PanSpread, 0.0f}}));
    corpus.push_back(std::move(v));
  }

  {  // Full-scale saturation, into and out of the engine.
    VectorCase v{"saturation_6s", Vector::Saturation, Frames(6), Frames(6), false, {}};
    PresetCase hot =
        Preset("hot_out", {{P::OutTrimDb, 24.0f}, {P::Mix, 1.0f}, {P::Feedback, 0.6f}});
    hot.require = {{C::InClipFrames, 1000}};
    v.presets.push_back(hot);
    corpus.push_back(std::move(v));
  }

  {  // Feedback above 1: self-oscillation past the input, and feedback decaying to zero.
    VectorCase v{"plucks_selfosc_20s", Vector::Plucks, Frames(4), Frames(20), false, {}};
    // Grain clouds decay even at feedback 1.1 (decorrelated taps lose loop gain), so
    // the self-oscillating loop is a single abutting reverse voice: it grows until the
    // tamer's saturator holds it, long after the input stops.
    PresetCase osc = Preset("selfosc",
        {{P::Feedback, 1.1f}, {P::ReverseProb, 1.0f}, {P::DelayMs, 300.0f},
         {P::GrainSizeMs, 100.0f}, {P::Overlap, 0.25f}, {P::SprayMs, 0.0f}, {P::Jitter, 0.0f},
         {P::WindowSustain, 1.0f}, {P::WindowSmooth, 0.0f}, {P::PanSpread, 0.0f}});
    osc.require = {{C::FbAbove1Frames, S(20)}, {C::TailActiveFrames, S(15)}};
    osc.ablate  = {Feature::Feedback, Feature::Reverse};
    v.presets.push_back(osc);

    PresetCase decay =
        Preset("fb_decay", {{P::Feedback, 1.1f}, {P::PitchSt, 5.0f}, {P::SprayMs, 30.0f}});
    const float kFb[] = {1.05f, 1.0f, 0.95f, 0.9f, 0.8f, 0.7f, 0.6f,
                         0.5f,  0.4f, 0.3f,  0.2f, 0.1f, 0.05f, 0.0f};
    int64_t k = 0;
    for (const float fb : kFb) decay.script.Param(S(4) + 24000 * k++ + 17, P::Feedback, fb);
    decay.require   = {{C::FbAbove1Frames, 1}, {C::Events, 14}, {C::TailActiveFrames, S(2)}};
    decay.invariant = {Invariance::HostileFpEnv};  // decays through the subnormal range to 0
    v.presets.push_back(decay);
    corpus.push_back(std::move(v));
  }

  {  // Two minutes of silent tail through feedback and every post stage (profile §4.2).
    VectorCase v{"strums_tail_123s", Vector::Strums, Frames(3), Frames(123), true, {}};
    PresetCase tail = Preset("tail_post_fb",
        {{P::Feedback, 0.9f}, {P::DelayMix, 0.5f}, {P::DelayFb, 0.6f}, {P::DelayTimeMs, 350.0f},
         {P::ReverbMix, 0.6f}, {P::ReverbTime, 0.9f}, {P::FilterCutoffHz, 3000.0f},
         {P::FilterRes, 0.3f}, {P::FilterMorph, 0.5f}, {P::ModDepth, 0.3f}, {P::ModRateHz, 0.7f}});
    tail.require = {{C::SilentInFrames, S(120)}, {C::TailActiveFrames, S(1)}};
    v.presets.push_back(tail);
    corpus.push_back(std::move(v));
  }

  {  // Freezes held 69 s, past 3/4 of the 2^22 ring (65.5 s), with onsets throughout.
    VectorCase v{"strums_freeze_70s", Vector::Strums, Frames(70), Frames(70), true, {}};
    // Mark positioning: marks newer than the pin through a long hold (mechanism D1).
    // A grain starts at the mark whatever the anchor (Granular.cpp:82, :114), so the
    // re-anchor cannot reach this output; freeze_live_long covers it.
    PresetCase marks = Preset("freeze_long",
        {{P::PitchSt, 12.0f}, {P::Jitter, 0.4f}, {P::Feedback, 0.3f}, {P::SpreadCents, 15.0f},
         {P::SprayMs, 80.0f}, {P::ReverseProb, 0.3f}, {P::OnsetTrigger, 1.0f},
         {P::PositionSource, 1.0f}});
    marks.script.Freeze(S(1) + 11, true);
    marks.require = {{C::FrozenOnsets, 20}, {C::FrozenFrames, S(68)}};
    marks.ablate  = {Feature::Freeze};
    v.presets.push_back(marks);

    // Live positioning: grains sit behind the anchor, so the re-anchor (mechanism D3)
    // moves them and the ring ablation first differs at the re-anchor second, 66.
    PresetCase live = Preset("freeze_live_long",
        {{P::PitchSt, 12.0f}, {P::Jitter, 0.4f}, {P::Feedback, 0.3f}, {P::SpreadCents, 15.0f},
         {P::SprayMs, 80.0f}, {P::ReverseProb, 0.3f}, {P::OnsetTrigger, 1.0f},
         {P::PositionSource, 0.0f}});
    live.script.Freeze(S(1) + 11, true);
    live.require = {{C::FrozenOnsets, 20}, {C::FrozenFrames, S(68)}, {C::FreezeEngages, 1}};
    live.ablate  = {Feature::Freeze, Feature::RingLength};
    v.presets.push_back(live);
    corpus.push_back(std::move(v));
  }

  {  // A mark aging past one ring with reverse grains on the far rail (mechanism D2).
    VectorCase v{"plucks_markage_92s", Vector::Plucks, Frames(2), Frames(92), true, {}};
    PresetCase age = Preset("reverse_mark_aging",
        {{P::PositionSource, 1.0f}, {P::ReverseProb, 1.0f}, {P::SprayMs, 0.0f}, {P::Jitter, 0.0f},
         {P::GrainSizeMs, 100.0f}, {P::Overlap, 0.4f}, {P::Mix, 1.0f}});
    age.require = {{C::Onsets, 1}, {C::TailActiveFrames, S(80)}};
    age.ablate  = {Feature::MarkPosition, Feature::RingLength};
    v.presets.push_back(age);
    corpus.push_back(std::move(v));
  }

  {  // Dense automation off the pedal's block grid.
    VectorCase v{"plucks_automation_12s", Vector::Plucks, Frames(12), Frames(12), false, {}};
    v.presets.push_back(AutomationOffGrid(S(12)));
    corpus.push_back(std::move(v));
  }

  {  // The state API mid-render: Spillover loads, a Restart and an Exact load.
    VectorCase v{"plucks_state_14s", Vector::Plucks, Frames(12), Frames(14), false, {}};
    v.presets.push_back(SpilloverChain());
    v.presets.push_back(RestartKeptParams());
    v.presets.push_back(ExactLoadMid());
    corpus.push_back(std::move(v));
  }

  {  // Silence in, exact silence out.
    VectorCase v{"silence_4s", Vector::Silence, 0, Frames(4), false, {}};
    PresetCase quiet = Preset("default_silence", {});
    quiet.require    = {{C::OutActiveFrames, 0, 0}, {C::LastNonzeroFrame, -1, -1}};
    v.presets.push_back(quiet);
    corpus.push_back(std::move(v));
  }

  return corpus;
}

}  // namespace brainscape::golden
