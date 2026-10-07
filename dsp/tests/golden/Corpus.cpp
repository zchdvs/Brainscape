#include "Corpus.h"

#include <algorithm>

#include "brainscape/ModeEval.h"

// Parameter values are float literals, or an integer converted and then put through
// at most ONE float operation: a single IEEE operation rounds the same everywhere
// and cannot be contracted, so the harness's own flags never change a script.
//
// Structure (the onset source, mark positioning, macros, CTRL) comes only from committed
// packages (mode-compiler.md §4.4, §10.3): presets/NAME.json, compiled by bspc to NAME.bsp,
// which the harness decodes. A preset with a package names it; its leaves are the document's.
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

// A preset that starts from a committed package (presets/PACKAGE.json): its leaves, mode, CTRL.
PresetCase PackagePreset(const char* name, const char* package) {
  PresetCase p;
  p.name    = name;
  p.package = package;
  return p;
}

// The contract-#1 freeze case of profile §5.7 (pitch 7 st, reverse 0.3, spray 50 ms,
// POS_MARK, onset trigger): freeze engages at an odd frame with onsets still arriving,
// so marks newer than the pin position grains (mechanism D1).
PresetCase FreezeMarks() {
  // presets/freeze_marks.json: delay 100 ms, mix 1, size 60 ms, overlap 0.55, spray 50 ms,
  // +7 st, spread 20 c, reverse 0.3, jitter 1, sensitivity 0.8, onset and mark.
  PresetCase p = PackagePreset("freeze_marks", "freeze_marks");
  p.script.Freeze(S(4) + 1, true);
  p.script.Freeze(S(11) + 37, false);
  p.require = {{C::Onsets, 10},       {C::FrozenOnsets, 3},  {C::FrozenFrames, S(6)},
               {C::FreezeEngages, 1}, {C::OffGridEvents, 2}};
  p.ablate  = {Feature::Freeze, Feature::MarkPosition, Feature::Reverse};
  return p;
}

// Every parameter moved every 250 ms at frames off the 48-frame grid, with freeze
// toggles and footswitch triggers among them (grown from the prototype battery). Until sound
// revision 2 it toggled rows 27 and 28 too; now that they are structure, every other step is a
// Spillover load between two modes (presets/mode_onset_marks.json, onset and marks, and the
// default mode), in both switch styles, before the step's leaves are set again. A load turns
// freeze off (profile §5.10), so none lands while a freeze is held (steps k % 17 = 6 to 8): each
// freeze holds its second, as before revision 2.
PresetCase AutomationOffGrid(int64_t frames) {
  PresetCase p = Preset("automation_offgrid", {});
  Script&    s = p.script;
  int64_t    k = 0;
  for (int64_t base = 0; base < frames; base += S(1) / 4, ++k) {
    const int64_t f      = base + 1 + (k * 37) % 47;  // never a multiple of 48
    const bool    frozen = k % 17 >= 6 && k % 17 <= 8;  // engaged at 5, released at 9
    if (k > 0 && k % 2 == 0 && !frozen) {  // the mode changes with (k / 2) % 2
      // FastCut and Trails each into both modes: FastCut at k = 2, 10, 16, 18, 26, ...
      const SwitchStyle style = k % 8 == 0 || k % 8 == 2 ? SwitchStyle::FastCut : SwitchStyle::Trails;
      if ((k / 2) % 2 == 1) {
        s.SpilloverPackage(f, "mode_onset_marks", style);
      } else {
        s.Spillover(f, {}, style);
      }
    }
    s.Param(f, P::WetTrimDb, I2F((k * 7) % 49 - 24));
    s.Param(f, P::Feedback, I2F(k % 12) * 0.1f);
    s.Param(f, P::FilterCutoffHz, I2F(100 + 397 * (k % 51)));
    s.Param(f, P::FilterRes, I2F(k % 11) * 0.1f);
    s.Param(f, P::FilterMorph, I2F(k % 13) * 0.25f);
    s.Param(f, P::Overlap, I2F(k % 7) / 7.0f);
    s.Param(f, P::GrainSizeMs, I2F(5 + 37 * (k % 13)));
    s.Param(f, P::TransposeSt, I2F((k * 5) % 49 - 24));
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
    if (k % 17 == 5) s.Freeze(f, true);
    if (k % 17 == 9) s.Freeze(f, false);
    if (k % 6 == 3) s.Trigger(f + 5);
  }
  p.require   = {{C::Events, 1000},       {C::OffGridEvents, 1000}, {C::Triggers, 5},
                 {C::FreezeEngages, 3},   {C::FrozenFrames, S(2)},  {C::FrozenOnsets, 3},
                 {C::Onsets, 5},          {C::Loads, 18},           {C::ModeSwitches, 17}};
  p.ablate    = {Feature::ModeSwitch, Feature::FastCut};
  p.invariant = {Invariance::HostileFpEnv};
  return p;
}

// Spillover loads as stamped events at odd frames (profile §5.10, §5.11): history, grains,
// marks, pending triggers and the scheduler phase carry over each load, the random-number
// epoch restarts at it, and events after a load at its frame apply after it. It starts from
// Strum-style marks and onset triggering (presets/strum_chain.json: delay 250 ms, feedback 0.5,
// size 80 ms, overlap 0.6, jitter 0.5, spray 30 ms, reverse 0.2, sensitivity 0.6, onset and
// mark), switches to the default mode with the first load and back with the last.
PresetCase SpilloverChain() {
  PresetCase p = PackagePreset("spillover_chain", "strum_chain");
  Script&    s = p.script;
  // Into a reverse loop above unity feedback, so the trails are loud when the next load lands.
  s.Spillover(S(2) + 4321,
      {{P::DelayMs, 700.0f}, {P::Feedback, 1.05f}, {P::GrainSizeMs, 120.0f}, {P::Overlap, 0.3f},
       {P::Jitter, 0.0f}, {P::SprayMs, 0.0f}, {P::TransposeSt, -12.0f}, {P::ReverseProb, 0.5f},
       {P::DelayMix, 0.4f}, {P::DelayTimeMs, 333.0f}, {P::DelayFb, 0.5f}});
  s.Trigger(S(5) + 1);
  s.Spillover(S(5) + 1,
      {{P::GrainSizeMs, 5.0f}, {P::Overlap, 1.0f}, {P::Jitter, 1.0f}, {P::SprayMs, 200.0f},
       {P::TransposeSt, 7.0f}, {P::PanSpread, 1.0f}, {P::ReverbMix, 0.5f}, {P::ReverbTime, 0.8f},
       {P::Feedback, 0.3f}});
  s.Param(S(5) + 1, P::Mix, 0.8f);
  s.Param(S(5) + 1, P::FilterCutoffHz, 1500.0f);
  s.SpilloverPackage(S(8) + 77, "strum_chain");
  s.Trigger(S(8) + 77);
  p.require   = {{C::Loads, 3, 3},         {C::Triggers, 2, 2}, {C::Events, 7},
                 {C::OffGridEvents, 7},    {C::Onsets, 10},     {C::FbAbove1Frames, S(2)},
                 {C::ModeSwitches, 2, 2}};
  p.ablate    = {Feature::Spillover};
  p.invariant = {Invariance::HostileFpEnv};
  return p;
}

// Restart mid-render with the parameters kept (profile §5.8): the engine is frozen, holds
// marks and a Spillover epoch, has two triggers still due and feedback ringing through
// every post stage, and must come back exactly as a fresh engine with those values.
PresetCase RestartKeptParams() {
  // presets/restart_kept.json: delay 180 ms, feedback 0.8, size 70 ms, overlap 0.7, spray 40 ms,
  // +5 st, reverse 0.3, jitter 0.6, onset and mark, post delay 0.3 at 420 ms with 0.6, reverb
  // 0.3 at 0.7; restart_kept_loaded.json the same at -7 st, 40 ms and feedback 0.9.
  PresetCase p = PackagePreset("restart_kept_params", "restart_kept");
  Script&    s = p.script;
  s.Param(S(1) + 17, P::DelayMs, 333.0f);
  s.Freeze(S(2) + 101, true);
  s.SpilloverPackage(S(3) + 55, "restart_kept_loaded");
  s.Param(S(3) + 2000, P::Mix, 0.7f);
  s.Freeze(S(4) + 7, true);
  for (int i = 0; i < 3; ++i) s.Trigger(S(6) + 9);  // one per frame from S(6) + 9
  s.Restart(S(6) + 10);
  s.Param(S(6) + 10, P::TransposeSt, 12.0f);  // frame 0 of the restarted timeline
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
  // presets/exact_load_marks.json: size 30 ms, overlap 0.9, jitter 1, spray 60 ms, +12 st,
  // spread 30 c, onset and mark, mod depth 0.5, a band-pass at 2.5 kHz.
  p.script.ExactLoadPackage(S(7) + 12345, "exact_load_marks");
  p.script.Param(S(7) + 12345, P::Feedback, 0.6f);
  p.require   = {{C::Restarts, 1, 1}, {C::FbAbove1Frames, S(7)}, {C::Onsets, 10}};
  p.ablate    = {Feature::Restart};
  p.invariant = {Invariance::RestartTail};
  return p;
}

// ── Sound revision 2: macros, expression, mode switches, the wet kill (mode-compiler.md
// §10.3). Every event is off the 48-frame grid: bases are multiples of 48 plus 1-47.

// Every macro swept from 0 to 1 and back, one move every 104 blocks (presets/macro_sweep.json:
// the six default macros and two auxiliary ones, with reversed ranges, curves below and above
// 1 and an in_range window), the Filter macro through the wet kill at 0; and a SetParam and a
// MacroMove on one leaf at one frame, in both orders: the later one wins (profile §5.11).
PresetCase MacroSweep() {
  PresetCase p = PackagePreset("macro_sweep", "macro_sweep");
  Script&    s = p.script;
  for (int64_t k = 0; k < 112; ++k) {
    const int64_t f     = S(1) / 2 + 4992 * k + 1 + (k * 29) % 47;
    const int64_t j     = k / 8;  // the macro's j-th move: up in sevenths, then down
    const auto    macro = static_cast<ParamId>(static_cast<uint32_t>(P::MacroActivity) + k % 8);
    s.Macro(f, macro, I2F(j <= 7 ? j : 14 - j) / 7.0f);
  }
  s.Param(S(3) + 777, P::Feedback, 0.8f);
  s.Macro(S(3) + 777, P::MacroRepeats, 0.25f);
  s.Macro(S(9) + 555, P::MacroRepeats, 0.9f);
  s.Param(S(9) + 555, P::Feedback, 0.1f);
  p.require   = {{C::MacroMoves, 114, 114}, {C::Events, 116}, {C::OffGridEvents, 116},
                 {C::KilledFrames, S(1) / 2}};
  p.ablate    = {Feature::Macro, Feature::Mode};
  p.invariant = {Invariance::HostileFpEnv};
  return p;
}

// The expression pedal on macros and on leaves (presets/expression.json: CTRL assigns
// macro.time over 0.1-0.9 at curve 2, post.reverb.mix over 0-0.8, layer0.window.sustain over
// 0.9-0.2 at curve 0.5 and macro.filter over 1-0.4): heel to toe and back twice, a move every 40
// blocks; a SetParam after a move at one frame, and one before.
PresetCase ExpressionPedal() {
  PresetCase p = PackagePreset("expression", "expression");
  Script&    s = p.script;
  for (int64_t k = 0; k < 300; ++k) {
    const int64_t f = S(1) / 2 + 1920 * k + 1 + (k * 13) % 47;
    const int64_t t = k % 150;
    if (k == 200) s.Param(f, P::ReverbMix, 0.9f);  // before the move: the move wins
    s.Expression(f, I2F(t <= 75 ? t : 150 - t) / 75.0f);
    if (k == 138) s.Param(f, P::ReverbMix, 0.05f);  // after the move: the SetParam wins
  }
  p.require   = {{C::ExpressionEvents, 300, 300}, {C::Events, 302}, {C::OffGridEvents, 302}};
  p.ablate    = {Feature::Macro, Feature::Mode};
  p.invariant = {Invariance::HostileFpEnv};
  return p;
}

// Mode changes are Spillover loads (mode-compiler.md §7.3), here with long dense grains (about
// 33 voices, so past the 8 Hermite ones): from onsets and marks at unity (presets/
// switch_marks.json) to the default mode at -5 st with Trails (switch_live.json), to onsets
// alone at +7 st with FastCut (switch_onset.json), back to marks with FastCut 77 frames later,
// inside the first fade (each load fades its own grains), the same mode again with another CTRL
// and FastCut (switch_marks_ctrl.json: no switch, but a cut, and the pedal assigned), onsets
// with Trails, and the default mode with FastCut and a footswitch trigger at its frame. So the
// FastCuts fade unity, Hermite and linear grains alike. Each switch package has its own macro
// table and CTRL (aux1 only in switch_live, aux2 and another Repeats range only in
// switch_onset), and macro and expression moves follow the loads, some at a load's own frame
// after it: each evaluates on the mode and CTRL just loaded (§3.4, §7.3 step 3).
PresetCase ModeSwitchChain() {
  PresetCase p = PackagePreset("mode_switch", "switch_marks");
  Script&    s = p.script;
  s.Macro(S(1) + 4321, P::MacroSpace, 0.4f);
  s.SpilloverPackage(S(2) + 101, "switch_live", SwitchStyle::Trails);
  s.Macro(S(2) + 101, P::MacroAux1, 0.8f);  // at the load's frame, after it
  s.Expression(S(3) + 1234, 0.7f);
  s.SpilloverPackage(S(4) + 333, "switch_onset", SwitchStyle::FastCut);
  s.Expression(S(4) + 333, 0.25f);
  s.Macro(S(4) + 400, P::MacroRepeats, 0.9f);
  s.SpilloverPackage(S(4) + 410, "switch_marks", SwitchStyle::FastCut);
  s.Macro(S(5) + 2000, P::MacroAux2, 0.5f);  // switch_marks has no aux2: nothing moves
  s.SpilloverPackage(S(6) + 5, "switch_marks_ctrl", SwitchStyle::FastCut);
  s.Expression(S(7) + 777, 0.5f);
  s.SpilloverPackage(S(8) + 999, "switch_onset", SwitchStyle::Trails);
  s.Macro(S(8) + 999, P::MacroAux2, 0.7f);
  s.Expression(S(9) + 3333, 0.9f);
  s.SpilloverPackage(S(10) + 4, "switch_live", SwitchStyle::FastCut);
  s.Trigger(S(10) + 4);
  s.Expression(S(10) + 4, 0.4f);
  s.Macro(S(11) + 555, P::MacroAux1, 0.3f);
  p.require   = {{C::Loads, 6, 6},          {C::ModeSwitches, 5, 5}, {C::Triggers, 1, 1},
                 {C::MacroMoves, 6, 6},     {C::ExpressionEvents, 5, 5},
                 {C::Onsets, 10},           {C::OffGridEvents, 18}};
  p.ablate    = {Feature::ModeSwitch,   Feature::FastCut,      Feature::Spillover,
                 Feature::MarkPosition, Feature::OnsetTrigger, Feature::Macro};
  p.invariant = {Invariance::HostileFpEnv};
  return p;
}

// The wet kill (mode-compiler.md §7.2): into and out of the 40 Hz minimum by SetParam and by
// the Filter macro (the default mode's, 40 Hz-20 kHz), lone trim and effect-volume changes
// while killed (they stay muted), and the effect volume, a device setting, scaling the wet
// only. A high-pass, so 41 Hz, one hertz above the kill, passes nearly everything. The kills by
// the macro and by the second SetParam run at mix 1, where a killed wet path leaves the output
// exactly ±0 under a sounding input (mutedFrames): the output shows the kill, not only the
// script's cutoff target (killedFrames).
PresetCase WetKillCase() {
  PresetCase p = Preset("wet_kill",
      {{P::Mix, 0.5f}, {P::FilterMorph, 2.0f}, {P::FilterCutoffHz, 800.0f}, {P::Feedback, 0.3f},
       {P::DelayMs, 200.0f}, {P::Jitter, 0.3f}, {P::ReverbMix, 0.3f}});
  Script& s = p.script;
  s.Param(S(1) + 13, P::FilterCutoffHz, 40.0f);
  s.Param(S(2) + 29, P::WetTrimDb, -6.0f);
  s.Param(S(3) + 7, P::FilterCutoffHz, 41.0f);
  s.Macro(S(5) + 5, P::MacroFilter, 0.0f);
  s.Param(S(5) + 5, P::Mix, 1.0f);
  s.Param(S(6) + 77, P::WetTrimDb, 3.0f);
  s.Macro(S(7) + 9, P::MacroFilter, 1.0f);
  s.Param(S(7) + 9, P::Mix, 0.5f);
  s.Param(S(8) + 1, P::EffectVolumeDb, -12.0f);
  s.Param(S(9) + 33, P::FilterCutoffHz, 40.0f);
  s.Param(S(9) + 33, P::Mix, 1.0f);
  s.Param(S(10) + 3, P::EffectVolumeDb, 0.0f);
  s.Param(S(11) + 11, P::FilterCutoffHz, 20000.0f);
  s.Param(S(11) + 11, P::Mix, 0.5f);
  p.require   = {{C::KilledFrames, S(5)}, {C::MutedFrames, S(2)}, {C::MacroMoves, 2, 2},
                 {C::Events, 14},         {C::OffGridEvents, 14}};
  p.ablate    = {Feature::WetKill, Feature::Macro};
  p.invariant = {Invariance::AmongEdits, Invariance::HostileFpEnv};
  return p;
}

// One lone change per Leaf row, and one to the effect volume (mode-compiler.md §7.2, §10.3), each
// at its own frame in a busy state where every change is audible (presets/lone_busy.json: every
// post stage engaged, onsets firing grains among about eight periodic voices). AmongEdits
// renders each among edits that rebuild every other domain: R1 routes a change by its domain.
PresetCase LoneChanges() {
  PresetCase                           p = PackagePreset("lone_changes", "lone_busy");
  const std::pair<ParamId, float> kTo[] = {
      {P::DelayMs, 400.0f},      {P::Mix, 0.3f},            {P::Feedback, 0.9f},
      {P::WetTrimDb, -9.0f},     {P::GrainSizeMs, 25.0f},   {P::Overlap, 0.8f},
      {P::SprayMs, 150.0f},      {P::TransposeSt, 7.0f},    {P::SpreadCents, 80.0f},
      {P::ReverseProb, 1.0f},    {P::Jitter, 1.0f},         {P::WindowSustain, 1.0f},
      {P::WindowSkew, 0.0f},     {P::WindowSmooth, 0.0f},   {P::PanSpread, 1.0f},
      {P::ModRateHz, 7.0f},      {P::ModDepth, 1.0f},       {P::DelayTimeMs, 400.0f},
      {P::DelayFb, 0.9f},        {P::DelayMix, 1.0f},       {P::ReverbTime, 1.0f},
      {P::ReverbMix, 1.0f},      {P::FilterCutoffHz, 400.0f}, {P::FilterRes, 1.0f},
      {P::FilterMorph, 2.0f},    {P::TriggerSens, 1.0f},    {P::EffectVolumeDb, -9.0f}};
  int64_t k = 0;
  for (const auto& c : kTo) {
    p.script.Param(S(1) / 2 + 20160 * k + 1 + (k * 7) % 47, c.first, c.second);
    ++k;
  }
  p.require   = {{C::Events, 27, 27}, {C::OffGridEvents, 27}, {C::Onsets, 10}};
  p.invariant = {Invariance::AmongEdits};
  return p;
}

// ── Sound revision 4: wave 1's trigger sources, bursts and intermittency (mode-compiler.md
// §7.5 R9). Events off the 48-frame grid.
using TS = Engine::TriggerSource;

// Onsets alone (presets/sources_onset.json: sources ["onset"]): no free-running births, so every
// grain is an onset's, and the footswitch and MIDI triggers the script sends are dropped.
PresetCase OnsetOnly() {
  PresetCase p = PackagePreset("onset_only", "sources_onset");
  p.script.Trigger(S(3) + 333, TS::Footswitch);
  p.script.Trigger(S(6) + 17, TS::MidiNote);
  p.require = {{C::Onsets, 10}, {C::Births, 10, 30}, {C::Triggers, 2, 2}, {C::OffGridEvents, 2}};
  p.ablate  = {Feature::Sources, Feature::OnsetTrigger};
  return p;
}

// Phasing copies of the newest onset (presets/burst_marks.json, the Strum B sketch: onsets alone
// on marks, a burst of 6 at spacing 0, so on consecutive frames, spray 4 ms).
PresetCase OnsetBurst() {
  PresetCase p = PackagePreset("onset_burst", "burst_marks");
  p.require    = {{C::Onsets, 10}, {C::BurstBirths, 50}};
  p.ablate     = {Feature::Burst, Feature::MarkPosition};
  return p;
}

// Spaced bursts from onsets and the footswitch, with intermittency skipping whole triggers
// (presets/burst_spaced.json: sources ["onset", "footswitch"], bursts of 4 every 120 ms,
// intermittency 0.3). MIDI triggers are dropped; a load of another mode (sources_onset) late in
// the render resets the bursts in progress.
PresetCase BurstSpaced() {
  PresetCase p = PackagePreset("burst_spaced", "burst_spaced");
  Script&    s = p.script;
  s.Trigger(S(1) + 77, TS::Footswitch);
  s.Trigger(S(2) + 33, TS::MidiNote);
  s.Trigger(S(3) + 501, TS::Footswitch);
  s.Trigger(S(5) + 13, TS::Footswitch);
  s.Trigger(S(6) + 47, TS::MidiNote);
  s.Trigger(S(7) + 999, TS::Footswitch);
  s.SpilloverPackage(S(9) + 123, "sources_onset");
  p.require   = {{C::Onsets, 10},      {C::Triggers, 6, 6}, {C::BurstBirths, 20},
                 {C::Skips, 3},        {C::Loads, 1, 1},    {C::ModeSwitches, 1, 1},
                 {C::OffGridEvents, 7}};
  p.ablate    = {Feature::Burst, Feature::Intermittency, Feature::Sources, Feature::Triggers};
  p.invariant = {Invariance::HostileFpEnv};
  return p;
}

// Intermittency on the free-running scheduler (the default mode): about half the periodic births
// skipped, each still consuming its interval.
PresetCase IntermittentCloud() {
  PresetCase p = Preset("intermittent_cloud",
      {{P::Intermittency, 0.5f}, {P::Overlap, 0.75f}, {P::Jitter, 0.6f}, {P::SprayMs, 80.0f},
       {P::GrainSizeMs, 70.0f}, {P::DelayMs, 220.0f}, {P::PanSpread, 0.8f}, {P::Feedback, 0.3f}});
  p.require = {{C::Skips, 500}, {C::Births, 500}};
  p.ablate  = {Feature::Intermittency};
  return p;
}

// MIDI notes trigger, the footswitch does not (presets/sources_midi.json: sources ["periodic",
// "midi_note"], one free-running voice, bursts of 3 every 40 ms); the burst count and spacing
// change alone mid-render, and AmongEdits renders each change among edits of every other domain.
PresetCase MidiGate() {
  PresetCase p = PackagePreset("midi_gate", "sources_midi");
  Script&    s = p.script;
  s.Trigger(S(1) + 5, TS::MidiNote);
  s.Trigger(S(2) + 77, TS::MidiNote);
  s.Trigger(S(3) + 17, TS::Footswitch);
  s.Trigger(S(4) + 301, TS::MidiNote);
  s.Trigger(S(5) + 3, TS::Footswitch);
  s.Param(S(5) + 1001, P::BurstCount, 5.0f);
  s.Trigger(S(6) + 11, TS::MidiNote);
  s.Param(S(6) + 333, P::BurstSpacingMs, 0.0f);
  s.Trigger(S(7) + 444, TS::Footswitch);
  s.Trigger(S(8) + 999, TS::MidiNote);
  p.require   = {{C::Triggers, 8, 8}, {C::BurstBirths, 10}, {C::OffGridEvents, 10}};
  p.ablate    = {Feature::Sources, Feature::Triggers, Feature::Burst};
  p.invariant = {Invariance::AmongEdits, Invariance::HostileFpEnv};
  return p;
}

// ── Sound revision 5: wave 1's pitch sets (mode-compiler.md §7.5 R10). ──────────────────────

// A cycled set (presets/pitchset_cycle.json: {0 twice, +12, -12} by `cycle`, the free-running
// scheduler at overlap 0.6) with the transpose leaf moved over it alone, and footswitch triggers
// taking their turn in the cycle.
PresetCase PitchCycle() {
  PresetCase p = PackagePreset("pitch_cycle", "pitchset_cycle");
  Script&    s = p.script;
  s.Param(S(3) + 211, P::TransposeSt, -5.0f);
  s.Trigger(S(5) + 7);
  s.Trigger(S(5) + 7);
  s.Param(S(7) + 97, P::TransposeSt, 7.0f);
  s.Param(S(9) + 1, P::TransposeSt, 0.0f);
  p.require   = {{C::Events, 5, 5}, {C::Triggers, 2, 2}, {C::OffGridEvents, 5}, {C::Births, 500}};
  p.ablate    = {Feature::PitchSet, Feature::Pitch, Feature::Triggers};
  p.invariant = {Invariance::AmongEdits, Invariance::HostileFpEnv};
  return p;
}

// A weighted random set (presets/pitchset_random.json: {0: 3, -12: 1, +7: 2} by `random`,
// periodic and onsets, reverse 0.2) switched to the cycled mode and back, Trails then FastCut:
// each switch resets the cycle, and the random draws restart with the load's epoch.
PresetCase PitchRandom() {
  PresetCase p = PackagePreset("pitch_random", "pitchset_random");
  Script&    s = p.script;
  s.SpilloverPackage(S(4) + 77, "pitchset_cycle");
  s.SpilloverPackage(S(7) + 501, "pitchset_random", SwitchStyle::FastCut);
  p.require = {{C::Loads, 2, 2}, {C::ModeSwitches, 2, 2}, {C::Onsets, 10}, {C::Births, 500}};
  p.ablate  = {Feature::PitchSet, Feature::PitchSelect, Feature::ModeSwitch, Feature::Reverse};
  return p;
}

// ── Sound revision 6: wave 1's repeat and decay (mode-compiler.md §7.5 R11). ────────────────

// Micro-loops (presets/repeat_loops.json: 4 passes of 70 ms over the set {0, +12} by cycle,
// decay 900 ms, periodic and onsets): the repeat and the decay moved alone, decay switched off,
// then one pass.
PresetCase RepeatLoops() {
  PresetCase p = PackagePreset("repeat_loops", "repeat_loops");
  Script&    s = p.script;
  s.Param(S(3) + 333, P::Repeat, 8.0f);
  s.Param(S(5) + 71, P::DecayMs, 0.0f);
  s.Param(S(7) + 5, P::DecayMs, 250.0f);
  s.Param(S(9) + 17, P::Repeat, 1.0f);
  p.require   = {{C::Events, 4, 4}, {C::OffGridEvents, 4}, {C::RepeatPasses, 500}, {C::Onsets, 10}};
  p.ablate    = {Feature::Repeat, Feature::Decay, Feature::PitchSet};
  p.invariant = {Invariance::AmongEdits, Invariance::HostileFpEnv};
  return p;
}

// The newest note, fading as it ages (presets/decay_marks.json: mark positioning, decay 700 ms,
// the Strum A sketch): grains read the newest onset's mark, quieter the older it is.
PresetCase DecayMarks() {
  PresetCase p = PackagePreset("decay_marks", "decay_marks");
  p.script.Trigger(S(10) + 777);  // two seconds after the last pluck: an old mark, faint
  p.require = {{C::Onsets, 10}, {C::Triggers, 1, 1}, {C::OffGridEvents, 1}};
  p.ablate  = {Feature::Decay, Feature::MarkPosition};
  return p;
}

// ── Sound revision 7: wave 1's voice count (mode-compiler.md §7.5 R12). ──────────────────────

// A dense cloud held to few voices (presets/voice_limit.json: overlap 0.9, voice_count 6, onset
// bursts of 5 stealing the oldest), the count moved alone to 2 and back to 64.
PresetCase VoiceLimit() {
  PresetCase p = PackagePreset("voice_limit", "voice_limit");
  Script&    s = p.script;
  s.Param(S(4) + 129, P::VoiceCount, 2.0f);
  s.Param(S(8) + 55, P::VoiceCount, 64.0f);
  p.require   = {{C::Events, 2, 2}, {C::OffGridEvents, 2}, {C::Steals, 20}, {C::Onsets, 10}};
  p.ablate    = {Feature::VoiceCount, Feature::Burst};
  p.invariant = {Invariance::AmongEdits, Invariance::HostileFpEnv};
  return p;
}

// One voice from onsets alone (presets/mono_stutter.json: bursts of 4 every 30 ms, voice_count
// 1, the Blocks sketch): every grain cuts the one before it, so each onset stutters.
PresetCase MonoStutter() {
  PresetCase p = PackagePreset("mono_stutter", "mono_stutter");
  p.require    = {{C::Onsets, 10}, {C::Steals, 40}, {C::BurstBirths, 40}};
  p.ablate     = {Feature::VoiceCount, Feature::Burst};
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
    case C::SubnormalOutFrames: return "subnormalOutFrames";
    case C::LastActiveFrame: return "lastActiveFrame";
    case C::LastNonzeroFrame: return "lastNonzeroFrame";
    case C::MacroMoves: return "macroMoves";
    case C::ExpressionEvents: return "expressionEvents";
    case C::ModeSwitches: return "modeSwitches";
    case C::KilledFrames: return "killedFrames";
    case C::MutedFrames: return "mutedFrames";
    case C::Births: return "births";
    case C::BurstBirths: return "burstBirths";
    case C::Skips: return "skips";
    case C::RepeatPasses: return "repeatPasses";
    case C::Steals: return "steals";
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
    case Feature::Mode: return "mode";
    case Feature::Macro: return "macro";
    case Feature::ModeSwitch: return "modeSwitch";
    case Feature::FastCut: return "fastCut";
    case Feature::WetKill: return "wetKill";
    case Feature::Sources: return "sources";
    case Feature::Burst: return "burst";
    case Feature::Intermittency: return "intermittency";
    case Feature::PitchSet: return "pitchSet";
    case Feature::PitchSelect: return "pitchSelect";
    case Feature::Repeat: return "repeat";
    case Feature::Decay: return "decay";
    case Feature::VoiceCount: return "voiceCount";
  }
  return "unknown";
}

const char* InvarianceName(Invariance i) noexcept {
  switch (i) {
    case Invariance::HostileFpEnv: return "hostileFpEnv";
    case Invariance::RestartTail: return "restartTail";
    case Invariance::AmongEdits: return "amongEdits";
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
    for (StagedLoad& staged : out.script.MutableStaged()) set(staged.preset.params, id, value);
    drop([id](const Event& e) {
      return e.type == EventType::SetParam && e.id == static_cast<uint32_t>(id);
    });
  };
  // The cutoff at the kill (40 Hz, its minimum) one hertz above it, wherever the preset sets it.
  auto unkill = [&](ParamList& params) {
    for (auto& kv : params) {
      if (kv.first == P::FilterCutoffHz && kv.second == 40.0f) kv.second = 41.0f;
    }
  };
  switch (f) {
    case Feature::MarkPosition: out.strip |= kStripMark; break;
    case Feature::OnsetTrigger: out.strip |= kStripOnset; break;
    case Feature::Reverse: neutral(P::ReverseProb, 0.0f); break;
    case Feature::Pitch: neutral(P::TransposeSt, 0.0f); break;
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
    case Feature::Mode: out.strip |= kStripMode; break;
    case Feature::Macro:
      drop([](const Event& e) {
        return e.type == EventType::MacroMove || e.type == EventType::Expression;
      });
      break;
    case Feature::ModeSwitch: out.strip |= kKeepMode; break;
    case Feature::FastCut:
      for (StagedLoad& staged : out.script.MutableStaged()) staged.style = SwitchStyle::Trails;
      break;
    case Feature::WetKill:
      unkill(out.params);
      for (StagedLoad& staged : out.script.MutableStaged()) unkill(staged.preset.params);
      for (Event& e : events) {
        if (e.type == EventType::SetParam && e.id == static_cast<uint32_t>(P::FilterCutoffHz) &&
            e.value == 40.0f) {
          e.value = 41.0f;
        }
      }
      break;
    case Feature::Sources: out.strip |= kStripSources; break;
    case Feature::Burst: neutral(P::BurstCount, 1.0f); break;
    case Feature::Intermittency: neutral(P::Intermittency, 0.0f); break;
    case Feature::PitchSet: out.strip |= kStripPitchSet; break;
    case Feature::PitchSelect: out.strip |= kStripPitchSelect; break;
    case Feature::Repeat: neutral(P::Repeat, 1.0f); break;
    case Feature::Decay: neutral(P::DecayMs, 0.0f); break;
    case Feature::VoiceCount: neutral(P::VoiceCount, 64.0f); break;
  }
  return out;
}

PresetSource StateAt(const PresetCase& p, int64_t frame) {
  // The preset whose mode plays, decoded for macro and expression moves, and every leaf's value.
  const PresetSource                 start{p.package, p.params};
  std::unique_ptr<PresetState>       first = CompletePreset(start, p.strip);
  std::unique_ptr<PresetState>       loaded;
  const PresetState*                 active  = first.get();
  const char*                        package = p.package;
  float                              values[kNumLeafParams];
  auto take = [&values](const PresetState& preset) {
    for (uint32_t i = 0; i < preset.leafCount; ++i) {
      const size_t k = LeafIndex(preset.leaves[i].id);
      if (k < kNumLeafParams) values[k] = preset.leaves[i].value;
    }
  };
  auto load = [&](const StagedLoad& staged) {
    loaded  = CompletePreset(staged.preset, p.strip, (p.strip & kKeepMode) != 0 ? first.get() : nullptr);
    active  = loaded.get();
    package = staged.preset.package;
    if (active != nullptr) take(*active);
  };
  if (active == nullptr) return {};
  take(*active);
  const auto& events   = p.script.Events();
  const auto& restarts = p.script.Restarts();
  size_t      e = 0, r = 0;
  PresetLeaf  out[kMaxExpressions * kMaxMacroTargets];
  // In time order; a restart comes before the events stamped at its frame.
  for (;;) {
    if (active == nullptr) return {};
    const bool eventDue   = e < events.size() && events[e].frame < frame;
    const bool restartDue = r < restarts.size() && restarts[r].frame < frame;
    if (!eventDue && !restartDue) break;
    if (restartDue && (!eventDue || restarts[r].frame <= events[e].frame)) {
      if (restarts[r].load) load(p.script.Staged()[restarts[r].staged]);
      ++r;
      continue;
    }
    const Event& ev = events[e++];
    size_t       n  = 0;
    if (ev.type == EventType::SetParam && IsLeaf(ev.id)) {  // the engine stores no other kind
      values[LeafIndex(ev.id)] = Canonicalize(static_cast<ParamId>(ev.id), ev.value);
    } else if (ev.type == EventType::SpilloverLoad) {
      load(p.script.Staged()[ev.id]);
    } else if (ev.type == EventType::MacroMove) {
      n = EvalMacro(active->mode, static_cast<ParamId>(ev.id), ev.value, out, kMaxMacroTargets);
    } else if (ev.type == EventType::Expression) {
      n = EvalExpression(active->mode, active->control, ev.value, out,
                         kMaxExpressions * kMaxMacroTargets);
    }
    for (size_t i = 0; i < n; ++i) {
      if (IsLeaf(out[i].id)) values[LeafIndex(out[i].id)] = out[i].value;
    }
  }
  PresetSource state;
  state.package = package;
  for (size_t i = 0; i < kNumLeafParams; ++i) state.params.emplace_back(LeafId(i), values[i]);
  return state;
}

PresetCase TailAfterRestart(const PresetCase& p, int64_t* start) {
  const RestartPoint& last = p.script.Restarts().back();
  *start                   = last.frame;
  PresetCase tail;
  tail.name = p.name;
  const PresetSource state =
      last.load ? p.script.Staged()[last.staged].preset : StateAt(p, last.frame);
  tail.package                = state.package;
  tail.params                 = state.params;
  tail.strip                  = p.strip;
  tail.script.MutableStaged() = p.script.Staged();
  for (const Event& e : p.script.Events()) {
    if (e.frame < last.frame) continue;
    Event moved = e;
    moved.frame -= last.frame;
    tail.script.MutableEvents().push_back(moved);
  }
  return tail;
}

PresetCase AmongEdits(const PresetCase& p) {
  PresetCase out = p;
  out.require.clear();
  out.ablate.clear();
  out.invariant.clear();
  // A Leaf row of exactly `domain` other than `except`, or none.
  auto other = [](uint8_t domain, uint32_t except) -> const ParamDescriptor* {
    for (size_t i = 0; i < kNumLeafParams; ++i) {
      const ParamDescriptor* d = FindParam(LeafId(i));
      if (d->domain == domain && static_cast<uint32_t>(d->id) != except) return d;
    }
    return nullptr;
  };
  std::vector<Event> events;
  uint32_t           seq = 0;
  for (const Event& e : p.script.Events()) {
    Event copy = e;
    copy.seq   = seq++;
    events.push_back(copy);
    if (e.type != EventType::SetParam) continue;
    // The value each touched leaf holds after this event, so setting it back changes nothing.
    PresetCase upTo = p;
    upTo.script.MutableEvents().clear();
    for (const Event& before : p.script.Events()) {
      upTo.script.MutableEvents().push_back(before);
      if (&before == &e) break;
    }
    const PresetSource now = StateAt(upTo, e.frame + 1);
    for (const uint8_t domain : {kDomainGranular, kDomainPost, kDomainMix, kDomainFeedback,
                                 kDomainWet, kDomainDetector}) {
      const ParamDescriptor* d = other(domain, e.id);
      if (d == nullptr) continue;
      float was = d->def;
      for (const auto& kv : now.params) {
        if (kv.first == d->id) was = kv.second;
      }
      Event set = e;
      set.id    = static_cast<uint32_t>(d->id);
      set.value = was == d->min ? d->max : d->min;
      set.seq   = seq++;
      events.push_back(set);
      set.value = was;
      set.seq   = seq++;
      events.push_back(set);
    }
  }
  out.script.MutableEvents() = std::move(events);
  return out;
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
    // presets/strum_marks.json: onset and mark, sensitivity 0.6, overlap 0.3, size 150 ms,
    // jitter 0, spray 40 ms, feedback 0.2.
    PresetCase strum = PackagePreset("strum_marks", "strum_marks");
    strum.script.Trigger(S(2) + 101);
    strum.script.Trigger(S(5) + 7);
    strum.script.Trigger(S(8) + 333);
    strum.require = {{C::Onsets, 10}, {C::Triggers, 3}, {C::OffGridEvents, 3}};
    strum.ablate  = {Feature::MarkPosition, Feature::OnsetTrigger, Feature::Triggers};
    v.presets.push_back(strum);

    PresetCase heavy = Preset("pitch_reverse_spray",
        {{P::Jitter, 1.0f}, {P::SprayMs, 400.0f}, {P::ReverseProb, 0.5f}, {P::SpreadCents, 60.0f},
         {P::TransposeSt, 7.0f}, {P::Overlap, 0.85f}, {P::PanSpread, 1.0f}, {P::GrainSizeMs, 60.0f},
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
    farRevUp.params.emplace_back(P::TransposeSt, 24.0f);
    farRevUp.ablate = {Feature::Spray, Feature::Reverse, Feature::Pitch};
    v.presets.push_back(farRevUp);

    // Spray reflected off the near guard: a 500 ms grain at +24 st needs 1.5 s behind
    // the write head, so from a 1 ms base about 7 in 8 draws fold back into the bounds.
    PresetCase nearRail = Preset("near_rail_spray",
        {{P::DelayMs, 1.0f}, {P::SprayMs, 2000.0f}, {P::GrainSizeMs, 500.0f}, {P::TransposeSt, 24.0f}});
    nearRail.ablate = {Feature::Spray};
    v.presets.push_back(nearRail);
    corpus.push_back(std::move(v));
  }

  {  // Strummed chords: freeze with marks newer than the pin, and the pitch extremes.
    VectorCase v{"strums_16s", Vector::Strums, Frames(16), Frames(16), false, {}};
    v.presets.push_back(FreezeMarks());

    PresetCase pitch = Preset("pitch_extremes",
        {{P::TransposeSt, 24.0f}, {P::SpreadCents, 100.0f}, {P::Jitter, 0.3f}, {P::Feedback, 0.4f},
         {P::GrainSizeMs, 80.0f}});
    for (int64_t k = 1; k * 72000 < S(16); ++k) {
      pitch.script.Param(k * 72000 + 13, P::TransposeSt, (k & 1) ? -24.0f : 24.0f);
    }
    pitch.require = {{C::Events, 9}, {C::OffGridEvents, 9}};
    pitch.ablate  = {Feature::Pitch};
    v.presets.push_back(pitch);

    // Freeze is a level settled per frame (profile §5.11): a release and a re-engage at
    // one frame keep the pin, and a Spillover load's freeze-off is immediate, so a freeze
    // after it at its frame pins anew. Live positioning, so every pin reaches the output.
    PresetCase retoggle = Preset("freeze_retoggle_spill",
        {{P::DelayMs, 150.0f}, {P::Mix, 1.0f}, {P::GrainSizeMs, 40.0f}, {P::Overlap, 0.8f},
         {P::SprayMs, 10.0f}, {P::TransposeSt, 5.0f}, {P::Jitter, 0.5f}, {P::Feedback, 0.3f}});
    retoggle.script.Freeze(S(3) + 101, true);
    retoggle.script.Freeze(S(6) + 4999, false);
    retoggle.script.Freeze(S(6) + 4999, true);
    retoggle.script.Spillover(S(9) + 23,  // while frozen
        {{P::DelayMs, 333.0f}, {P::Mix, 1.0f}, {P::GrainSizeMs, 20.0f}, {P::Overlap, 1.0f},
         {P::TransposeSt, -7.0f}, {P::Feedback, 0.5f}, {P::ReverbMix, 0.4f}});
    retoggle.script.Freeze(S(10) + 77, true);
    retoggle.script.Spillover(S(12) + 5,
        {{P::DelayMs, 90.0f}, {P::Mix, 1.0f}, {P::GrainSizeMs, 60.0f}, {P::SprayMs, 0.0f},
         {P::TransposeSt, 12.0f}, {P::Feedback, 0.2f}});
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
    // presets/dense_1ms.json: size 1 ms, overlap 1, jitter 1, spray 5 ms, reverse 0.5, onset,
    // pan spread 1.
    PresetCase dense = PackagePreset("dense_1ms", "dense_1ms");
    dense.require = {{C::Onsets, 20}};
    dense.ablate  = {Feature::Reverse, Feature::OnsetTrigger};
    v.presets.push_back(dense);
    corpus.push_back(std::move(v));
  }

  {  // Soft notes: every post stage at its extremes, then swept.
    VectorCase v{"soft_notes_10s", Vector::SoftNotes, Frames(10), Frames(10), false, {}};
    // The cutoff 1 Hz above its minimum, which kills the wet since sound revision 2 (§7.6):
    // under the kill, the other post stages' ablations could change nothing.
    PresetCase postMax = Preset("post_max",
        {{P::Feedback, 0.3f}, {P::ModDepth, 1.0f}, {P::ModRateHz, 10.0f}, {P::DelayMix, 1.0f},
         {P::DelayFb, 0.9f}, {P::DelayTimeMs, 2000.0f}, {P::ReverbMix, 1.0f}, {P::ReverbTime, 1.0f},
         {P::FilterCutoffHz, 41.0f}, {P::FilterRes, 1.0f}, {P::FilterMorph, 2.0f}});
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
        Preset("hot_out", {{P::WetTrimDb, 24.0f}, {P::Mix, 1.0f}, {P::Feedback, 0.6f}});
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
        Preset("fb_decay", {{P::Feedback, 1.1f}, {P::TransposeSt, 5.0f}, {P::SprayMs, 30.0f}});
    const float kFb[] = {1.05f, 1.0f, 0.95f, 0.9f, 0.8f, 0.7f, 0.6f,
                         0.5f,  0.4f, 0.3f,  0.2f, 0.1f, 0.05f, 0.0f};
    int64_t k = 0;
    for (const float fb : kFb) decay.script.Param(S(4) + 24000 * k++ + 17, P::Feedback, fb);
    decay.require   = {{C::FbAbove1Frames, 1}, {C::Events, 14}, {C::TailActiveFrames, S(2)}};
    // A long decay to exact silence under the hostile rounding mode. It never passes through
    // the subnormal range (the int16 ring and the in-code flush of profile §4.3 end it), so
    // flushing cannot reach it; plucks_subnormal_6s covers that.
    decay.invariant = {Invariance::HostileFpEnv};
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
    // presets/freeze_long.json: +12 st, jitter 0.4, feedback 0.3, spread 15 c, spray 80 ms,
    // reverse 0.3, onset and mark; freeze_live_long.json the same on the live position.
    PresetCase marks = PackagePreset("freeze_long", "freeze_long");
    marks.script.Freeze(S(1) + 11, true);
    marks.require = {{C::FrozenOnsets, 20}, {C::FrozenFrames, S(68)}};
    marks.ablate  = {Feature::Freeze};
    v.presets.push_back(marks);

    // Live positioning: grains sit behind the anchor, so the re-anchor (mechanism D3)
    // moves them and the ring ablation first differs at the re-anchor second, 66.
    PresetCase live = PackagePreset("freeze_live_long", "freeze_live_long");
    live.script.Freeze(S(1) + 11, true);
    live.require = {{C::FrozenOnsets, 20}, {C::FrozenFrames, S(68)}, {C::FreezeEngages, 1}};
    live.ablate  = {Feature::Freeze, Feature::RingLength};
    v.presets.push_back(live);
    corpus.push_back(std::move(v));
  }

  {  // A mark aging past one ring with reverse grains on the far rail (mechanism D2).
    VectorCase v{"plucks_markage_92s", Vector::Plucks, Frames(2), Frames(92), true, {}};
    // presets/reverse_mark_aging.json: mark positioning, reverse 1, spray 0, jitter 0, size
    // 100 ms, overlap 0.4, mix 1.
    PresetCase age = PackagePreset("reverse_mark_aging", "reverse_mark_aging");
    age.require = {{C::Onsets, 1}, {C::TailActiveFrames, S(80)}};
    age.ablate  = {Feature::MarkPosition, Feature::RingLength};
    v.presets.push_back(age);
    // presets/repeat_mark_aging.json (sound revision 6): the same, with 16 passes of 500 ms, so
    // the far rail covers the whole life, 15 passes more: the aging mark reaches it about 79 s
    // in, 8.5 s before the 2^22 ring's staleness guard drops the mark; the doubled ring never
    // reaches it.
    PresetCase loops = PackagePreset("repeat_mark_aging", "repeat_mark_aging");
    loops.require    = {{C::Onsets, 1}, {C::TailActiveFrames, S(80)}, {C::RepeatPasses, 600}};
    loops.ablate     = {Feature::Repeat, Feature::MarkPosition, Feature::RingLength};
    v.presets.push_back(loops);
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

  {  // Subnormal input where the plucks are silent: every flush mode changes the output.
    VectorCase v{"plucks_subnormal_6s", Vector::Plucks, Frames(3), Frames(6), false, {}, true};
    // Mix 0 passes the dry signal through the mix arithmetic (dry * 1 * 1), so every
    // subnormal input sample is an output sample: DAZ or Arm FZ zeroes it on the way in,
    // FTZ on the way out.
    PresetCase dry = Preset("subnormal_dry", {{P::Mix, 0.0f}});
    dry.require   = {{C::SubnormalOutFrames, S(3)}};
    dry.invariant = {Invariance::HostileFpEnv};
    v.presets.push_back(dry);
    // Subnormal input through the ring write, the onset detector and a scaled dry path,
    // under a wet path that decays to exact zero. Mix 0.75: the dry at 0.5 under the Mix law
    // (sound revision 3), as at Mix 0.5 under the linear crossfade before it.
    PresetCase wet = Preset("subnormal_wet",
        {{P::Mix, 0.75f}, {P::WetTrimDb, -6.0f}, {P::Feedback, 0.5f}, {P::DelayMs, 250.0f}});
    wet.require   = {{C::SubnormalOutFrames, S(1)}, {C::Onsets, 4}};
    wet.invariant = {Invariance::HostileFpEnv};
    v.presets.push_back(wet);
    corpus.push_back(std::move(v));
  }

  {  // Sound revision 2's modes: macro and expression moves, mode switches, the wet kill.
    VectorCase v{"plucks_modes_14s", Vector::Plucks, Frames(12), Frames(14), false, {}};
    v.presets.push_back(MacroSweep());
    v.presets.push_back(ExpressionPedal());
    v.presets.push_back(ModeSwitchChain());
    v.presets.push_back(WetKillCase());
    v.presets.push_back(LoneChanges());
    corpus.push_back(std::move(v));
  }

  {  // Wave 1 (sound revision 4 on): trigger sources, bursts, intermittency.
    VectorCase v{"plucks_wave1_12s", Vector::Plucks, Frames(10), Frames(12), false, {}};
    v.presets.push_back(OnsetOnly());
    v.presets.push_back(OnsetBurst());
    v.presets.push_back(BurstSpaced());
    v.presets.push_back(IntermittentCloud());
    v.presets.push_back(MidiGate());
    v.presets.push_back(PitchCycle());   // sound revision 5 on: pitch sets
    v.presets.push_back(PitchRandom());
    v.presets.push_back(RepeatLoops());  // sound revision 6 on: repeat and decay
    v.presets.push_back(DecayMarks());
    v.presets.push_back(VoiceLimit());  // sound revision 7 on: voice count
    v.presets.push_back(MonoStutter());
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
