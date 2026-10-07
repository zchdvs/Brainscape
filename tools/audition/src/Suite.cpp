#include "Suite.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <map>
#include <mutex>
#include <tuple>

#include "JsonWriter.h"
#include "Wav.h"
#include "brainscape/Params.h"
#include "brainscape/SoundRevision.h"

namespace bsa {

using brainscape::Engine;
using brainscape::ParamId;
using brainscape::PresetState;
using EventType = Engine::EventType;

namespace {

// The thresholds of §11.3's pre-screen table.
constexpr double kPeakStoredDbfs = -1.0;  // Peak: at stored positions
constexpr double kPeakMovedDbfs  = 0.0;   //       during sweeps and S11
constexpr double kLevelLu        = 2.0;   // Level: wet at Mix 1 within ±2 LU of the dry
constexpr double kEngagedLowLu   = -1.0;  // Engaged: no more than 1 LU below bypass
constexpr double kEngagedHighLu  = 4.0;   //          and no more than 4 LU above it
constexpr double kSweepLu        = 3.0;   // Sweeps: Activity, Shape and Time within ±3 LU
constexpr double kRepeatsMaxLu   = 10.0;  // Repeats: at most +10 LU at maximum
constexpr double kFallbackLu     = 12.0;  // Fallback: SoftNotes within 12 dB of Plucks
constexpr double kClickRatio     = 4.0;   // Clicks: no step above 4x the static render's largest
// The tooling's own tolerances, where the design says "non-decreasing" or "measurably" of
// measurements that fluctuate with the input (README.md, "Readings").
constexpr double kRisingTolLu    = 1.0;   // the Repeats sweep's rising leg, per 1 s window step
constexpr double kRungTolLu      = 0.5;   // S7's levels, rung to rung
constexpr double kRungTolSeconds = 0.25;  // S7's tails, rung to rung
constexpr double kShapeBrightPct = 5.0;   // Response: Shape moves the brightness by 5 %
constexpr double kShapeEnvDb     = 1.0;   //   or the envelope's variation by 1 dB

const char* MacroShort(ParamId m) {
  switch (m) {
    case ParamId::MacroActivity: return "activity";
    case ParamId::MacroRepeats: return "repeats";
    case ParamId::MacroShape: return "shape";
    case ParamId::MacroTime: return "time";
    case ParamId::MacroSpace: return "space";
    case ParamId::MacroFilter: return "filter";
    case ParamId::MacroAux1: return "aux1";
    case ParamId::MacroAux2: return "aux2";
    default: return "?";
  }
}

const char* RoleName(Role r) {
  switch (r) {
    case Role::Engaged: return "engaged";
    case Role::Wet: return "wet";
    case Role::SweepRef: return "sweep-reference";
    case Role::Sweep: return "sweep";
    case Role::RepeatsRung: return "repeats-rung";
    case Role::Freeze: return "freeze";
    case Role::Triggers: return "triggers";
    case Role::Load: return "load";
    case Role::Combination: return "combination";
    case Role::Determinism: return "determinism";
  }
  return "?";
}

std::string Fmt(const char* f, double v) {
  char buf[64];
  std::snprintf(buf, sizeof buf, f, v);
  return buf;
}
std::string Lu(double v) { return Fmt("%+.1f", Round1(v)); }
std::string Db(double v) { return v <= kSilentDb ? std::string("silent") : Fmt("%.1f", Round1(v)); }

std::string PosText(float p) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.3g", static_cast<double>(p));
  return buf;
}

// The sweep of §11.3: from the stored position to 0, to 1 and back, over 16 s at constant
// speed, one move per 48-frame block.
std::vector<ScriptEvent> SweepEvents(ParamId macro, float stored) {
  std::vector<ScriptEvent> ev;
  const double             p = stored;
  const double             a = 8.0 * p, b = a + 8.0;  // the seconds it reaches 0 and 1
  for (uint32_t frame = 0; frame < kSweepFrames; frame += kPedalBlock) {
    const double t   = static_cast<double>(frame) / kRate;
    double       pos = t < a ? p - t / 8.0 : (t < b ? (t - a) / 8.0 : 1.0 - (t - b) / 8.0);
    pos              = std::min(1.0, std::max(0.0, pos));
    ev.push_back({frame, EventType::MacroMove, static_cast<uint32_t>(macro), static_cast<float>(pos), 0});
  }
  return ev;
}

Planned Base(const char* script, std::string name, Role role, Vector v) {
  Planned p;
  p.script = script;
  p.name   = std::move(name);
  p.role   = role;
  p.vector = v;
  return p;
}

const Rendered* Find(const std::vector<Rendered>& rs, const std::string& name) {
  for (const Rendered& r : rs) {
    if (r.plan.name == name) return &r;
  }
  return nullptr;
}

struct Verdicts {
  bool                     fail = false;
  std::vector<std::string> parts;
  void Add(bool ok, std::string what) {
    fail = fail || !ok;
    parts.push_back(std::move(what) + (ok ? "" : " (FAIL)"));
  }
  std::string Detail() const {
    std::string s;
    for (const std::string& p : parts) s += (s.empty() ? "" : "; ") + p;
    return s;
  }
};

// Many renders under one check: how many, which failed (and why), and the worst value.
struct Tally {
  size_t                   n = 0;
  std::vector<std::string> failed;
  double                   worst = -std::numeric_limits<double>::infinity();
  std::string              worstName;
  uint64_t                 over = 0;
  void Add(const Rendered& r, bool ok, double value, const std::string& why = std::string()) {
    ++n;
    if (!ok) failed.push_back(r.plan.name + why);
    if (value > worst) {
      worst     = value;
      worstName = r.plan.name;
    }
  }
  std::string Failures(size_t shown = 6) const {
    std::string s;
    for (size_t i = 0; i < failed.size() && i < shown; ++i) s += (i ? ", " : "") + failed[i];
    if (failed.size() > shown) s += " and " + std::to_string(failed.size() - shown) + " more";
    return s;
  }
};

double TailKey(const Metrics& m) {
  return m.tailFinite ? m.tailSeconds : std::numeric_limits<double>::infinity();
}

Check Make(const char* name, const Verdicts& v, const char* noneVerdict = "n/a",
           const char* noneDetail = "nothing to judge") {
  if (v.parts.empty()) return {name, noneVerdict, noneDetail};
  return {name, v.fail ? "FAIL" : "pass", v.Detail()};
}

std::vector<Vector> ClassInputs(InputClass c) {
  if (c == InputClass::Pad) return {Vector::SoftNotes};
  return {Vector::Plucks, Vector::Strums};
}

}  // namespace

double DryLevel(Vector v) {
  static std::mutex            mutex;
  static std::map<int, double> cache;
  const std::lock_guard<std::mutex> lock(mutex);
  const auto                   it = cache.find(static_cast<int>(v));
  if (it != cache.end()) return it->second;
  const Input  in = VectorInput(v);
  const double l  = Loudness(Conditioned(in.audio, InputMode::Stereo)).Integrated(0, in.signalFrames);
  cache[static_cast<int>(v)] = l;
  return l;
}

const char* InputClassName(InputClass c) { return c == InputClass::Pad ? "pad" : "attack"; }
Vector      ClassVector(InputClass c) { return c == InputClass::Pad ? Vector::SoftNotes : Vector::Plucks; }

bool OnsetMode(const PresetState& s) {
  return (s.mode.schedule.sources & brainscape::kSourceOnset) != 0 ||
         s.mode.layers[0].source == brainscape::PositionSource::Mark;
}

std::string PresetDir(const Preset& p) { return p.identity.package ? p.identity.id : std::string("leaf-only"); }

bool SuiteResult::Failed() const {
  if (!ok) return true;
  for (const Check& c : checks) {
    if (c.verdict == "FAIL") return true;
  }
  return false;
}

std::vector<Planned> Plan(const Preset& preset, const std::vector<const Preset*>& set,
                          const std::vector<std::string>& scripts, std::vector<std::string>* skipped) {
  auto wants = [&](const char* s) {
    return std::find(scripts.begin(), scripts.end(), s) != scripts.end();
  };
  auto skip = [&](std::string why) {
    if (skipped != nullptr) skipped->push_back(std::move(why));
  };
  const PresetState& st  = *preset.state;
  const Vector       cls = ClassVector(preset.declare.inputClass);
  const std::string  cn  = VectorName(cls);
  std::vector<Planned> out;

  if (wants("S0")) {
    for (const Vector v : kVectors) {
      out.push_back(Base("S0", std::string("S0.engaged.") + VectorName(v), Role::Engaged, v));
    }
    for (const Vector v : {Vector::Plucks, Vector::Strums, Vector::SoftNotes}) {
      Planned p = Base("S0", std::string("S0.wet.") + VectorName(v), Role::Wet, v);
      p.leaves  = {{ParamId::Mix, 1.0f}};
      p.variant = "global.mix at 1: the wet alone";
      out.push_back(p);
    }
    // Determinism (§11.3, "two renders and two block patterns give one hash").
    Planned again  = Base("S0", "S0.engaged." + cn + "@again", Role::Determinism, cls);
    again.repeatOf = "S0.engaged." + cn;
    again.writeWav = false;
    out.push_back(again);
    Planned mixed      = again;
    mixed.name         = "S0.engaged." + cn + "@mixed-blocks";
    mixed.blockPattern = kMixedPattern;
    out.push_back(mixed);
  }

  bool anySweep = false;
  for (size_t i = 0; i < 6; ++i) anySweep = anySweep || wants(kScriptNames[i + 1]);
  if (anySweep) {
    Planned ref = Base("S1", "S1.reference." + cn, Role::SweepRef, cls);
    ref.looped       = true;
    ref.signalFrames = kSweepFrames;
    ref.tailFrames   = 0;
    out.push_back(ref);
    bool firstSweep = true;
    for (size_t i = 0; i < 6; ++i) {
      const char* script = kScriptNames[i + 1];
      if (!wants(script)) continue;
      const ParamId m      = kSweepMacros[i];
      const float   stored = StoredPosition(st, m);
      if (stored < 0) {
        skip(std::string(script) + ": macro." + MacroShort(m) + " is undefined in this mode");
        continue;
      }
      Planned s = ref;
      s.script  = script;
      s.name    = std::string(script) + "." + MacroShort(m) + "." + cn;
      s.role    = Role::Sweep;
      s.macro   = m;
      s.stored  = stored;
      s.events  = SweepEvents(m, stored);
      s.eventsDescription = std::string("macro.") + MacroShort(m) + " from its stored " +
                            PosText(stored) +
                            " to 0, to 1 and back over 16 s, one MacroMove per 48-frame block";
      out.push_back(s);
      if (firstSweep) {
        Planned d      = s;
        d.name         = s.name + "@512-blocks";
        d.role         = Role::Determinism;
        d.repeatOf     = s.name;
        d.blockPattern = {512};
        d.writeWav     = false;
        out.push_back(d);
        firstSweep = false;
      }
    }
  }

  if (wants("S7")) {
    if (!MacroDefined(st, ParamId::MacroRepeats)) {
      skip("S7: macro.repeats is undefined in this mode");
    } else {
      for (const float rung : kRepeatsRungs) {
        Planned p    = Base("S7", "S7.repeats-" + PosText(rung) + "." + cn, Role::RepeatsRung, cls);
        p.tailFrames = kRepeatsTail;
        p.positions  = {{ParamId::MacroRepeats, rung}};
        p.position   = rung;
        p.variant    = "macro.repeats stored at " + PosText(rung);
        out.push_back(p);
      }
    }
  }

  if (wants("S8")) {
    Planned p = Base("S8", "S8.freeze." + cn, Role::Freeze, cls);
    p.events  = {{kFreezeOn, EventType::Freeze, 0, 1.f, 0}, {kFreezeOff, EventType::Freeze, 0, 0.f, 0}};
    p.eventsDescription = "freeze engaged at 4 s, released at 14 s";
    out.push_back(p);
  }

  if (wants("S9")) {
    Planned p = Base("S9", "S9.footswitch." + cn, Role::Triggers, cls);
    for (uint32_t k = 0; k < 16; ++k) {
      p.events.push_back({static_cast<int64_t>(kRate + kRate / 4 + k * (kRate / 2)), EventType::Trigger,
                          static_cast<uint32_t>(Engine::TriggerSource::Footswitch), 1.f, 0});
    }
    p.eventsDescription = "a footswitch trigger every 0.5 s from 1.25 s to 8.75 s";
    out.push_back(p);
  }

  if (wants("S10")) {
    const Preset* from = nullptr;
    for (size_t i = 0; i < set.size(); ++i) {
      if (set[i] == &preset && set.size() > 1) from = set[(i + set.size() - 1) % set.size()];
    }
    const std::string fromName = from != nullptr ? PresetDir(*from) : std::string("default");
    for (const brainscape::SwitchStyle style :
         {brainscape::SwitchStyle::Trails, brainscape::SwitchStyle::FastCut}) {
      const bool trails = style == brainscape::SwitchStyle::Trails;
      Planned p = Base("S10", std::string("S10.") + (trails ? "trails" : "fastcut") + ".from-" + fromName + "." + cn,
                       Role::Load, cls);
      p.from   = from;
      p.events = {{kLoadFrame, EventType::SpilloverLoad, static_cast<uint32_t>(style), 0.f, 0}};
      p.eventsDescription = std::string("loaded Exact: ") +
                            (from != nullptr ? from->identity.id : std::string("the default preset")) +
                            "; this preset as a Spillover load at 5 s with " + (trails ? "Trails" : "FastCut");
      out.push_back(p);
    }
  }

  if (wants("S11")) {
    for (const Preset* other : set) {
      if (other == &preset) continue;
      Planned p = Base("S11", "S11.at-" + PresetDir(*other) + "." + cn, Role::Combination, cls);
      for (uint32_t id = static_cast<uint32_t>(ParamId::MacroActivity);
           id <= static_cast<uint32_t>(ParamId::MacroAux2); ++id) {
        const auto  m   = static_cast<ParamId>(id);
        const float pos = StoredPosition(*other->state, m);
        if (pos >= 0 && MacroDefined(st, m)) p.positions.push_back({m, pos});
      }
      p.variant  = "the macro positions " + other->identity.id + " stores";
      p.writeWav = false;
      out.push_back(p);
    }
    const ParamId corners[4] = {ParamId::MacroActivity, ParamId::MacroRepeats, ParamId::MacroShape,
                                ParamId::MacroTime};
    const char    letters[4] = {'a', 'r', 's', 't'};
    for (uint32_t bits = 0; bits < 16; ++bits) {
      std::string tag;
      Planned     p;
      for (int k = 0; k < 4; ++k) {
        const float pos = (bits >> (3 - k)) & 1u ? 1.f : 0.f;
        tag += letters[k];
        tag += pos != 0.f ? '1' : '0';
        if (MacroDefined(st, corners[k])) p.positions.push_back({corners[k], pos});
      }
      Planned c   = Base("S11", "S11.corner-" + tag + "." + cn, Role::Combination, cls);
      c.positions = p.positions;
      c.variant   = "activity, repeats, shape and time at the corner " + tag;
      c.writeWav  = false;
      out.push_back(c);
    }
  }
  return out;
}

std::vector<Check> PreScreen(const Preset& preset, const std::vector<Rendered>& rs) {
  std::vector<Check>   checks;
  const InputClass     ic      = preset.declare.inputClass;
  const Vector         cls     = ClassVector(ic);
  const std::string    cn      = VectorName(cls);
  const std::vector<Vector> inputs = ClassInputs(ic);

  {  // Renders: each rendered, and loaded exactly what the package stores.
    Verdicts v;
    size_t   n = 0;
    for (const Rendered& r : rs) {
      ++n;
      if (!r.ok) v.Add(false, r.plan.name + ": " + r.error);
      else if (!r.exact) v.Add(false, r.plan.name + ": loaded inexactly");
    }
    if (!v.fail) v.Add(true, std::to_string(n) + " renders, every load exact");
    checks.push_back(Make("Renders", v));
  }
  {  // Determinism: two renders and two block patterns give one hash.
    Verdicts v;
    for (const Rendered& r : rs) {
      if (r.plan.role != Role::Determinism) continue;
      const Rendered* a = Find(rs, r.plan.repeatOf);
      const bool same = a != nullptr && a->ok && r.ok && a->hashes.whole == r.hashes.whole;
      v.Add(same, r.plan.name + (same ? " = " : " != ") + r.plan.repeatOf);
    }
    checks.push_back(Make("Determinism", v));
  }
  {  // Numbers: no NaN or infinity, no subnormal sample, on any render.
    Verdicts finite, denormal;
    for (const Rendered& r : rs) {
      if (!r.ok) continue;
      if (r.metrics.nonFinite != 0) finite.Add(false, r.plan.name + ": " + std::to_string(r.metrics.nonFinite) + " non-finite samples");
      if (r.metrics.subnormal != 0) denormal.Add(false, r.plan.name + ": " + std::to_string(r.metrics.subnormal) + " subnormal samples");
    }
    if (!finite.fail) finite.Add(true, "every sample of every render finite");
    if (!denormal.fail) denormal.Add(true, "no subnormal sample in any render");
    checks.push_back(Make("Finite", finite));
    checks.push_back(Make("Denormals", denormal));
  }

  // S0 on the class inputs: Peak, Level, Engaged, Tail.
  Verdicts peak, level, engaged, tail;
  for (const Vector in : inputs) {
    const std::string vn  = VectorName(in);
    const Rendered*   e   = Find(rs, "S0.engaged." + vn);
    const Rendered*   w   = Find(rs, "S0.wet." + vn);
    const double      dry = DryLevel(in);
    if (e != nullptr && e->ok) {
      peak.Add(Round1(e->metrics.peakDbfs) <= kPeakStoredDbfs, vn + " " + Db(e->metrics.peakDbfs) + " dBFS");
      const double d = e->metrics.loudness - dry;
      engaged.Add(Round1(d) >= kEngagedLowLu && Round1(d) <= kEngagedHighLu, vn + " " + Lu(d) + " LU");
      const bool        finite = e->metrics.tailFinite;
      const std::string t      = vn + " " + TailText(e->metrics);
      if (finite || !preset.declare.selfOscillating) tail.Add(finite, t);
      else tail.parts.push_back(t + " (declared self-oscillating)");
    }
    if (w != nullptr && w->ok) {
      const double d = w->metrics.loudness - dry;
      level.Add(std::fabs(Round1(d)) <= kLevelLu, vn + " " + Lu(d) + " LU");
    }
  }
  checks.push_back(Make("Peak", peak));
  checks.push_back(Make("Level", level));
  checks.push_back(Make("Engaged", engaged));
  checks.push_back(Make("Tail", tail));

  {  // Fallback: an onset mode on SoftNotes within 12 dB of its Plucks level (wet against dry).
    Verdicts        v;
    const Rendered* wp = Find(rs, "S0.wet.plucks");
    const Rendered* ws = Find(rs, "S0.wet.soft_notes");
    if (!OnsetMode(*preset.state)) {
      checks.push_back({"Fallback", "n/a", "not an onset mode"});
    } else if (wp != nullptr && ws != nullptr && wp->ok && ws->ok) {
      const double dp = wp->metrics.loudness - DryLevel(Vector::Plucks);
      const double ds = ws->metrics.loudness - DryLevel(Vector::SoftNotes);
      const double gap = ds - dp;
      const bool   ok  = std::fabs(Round1(gap)) <= kFallbackLu;
      const std::string d = "wet-dry on soft_notes " + Lu(ds) + " against plucks " + Lu(dp) + " (" + Lu(gap) + " LU)";
      if (ok || !preset.declare.needsAttacks) checks.push_back({"Fallback", ok ? "pass" : "FAIL", d});
      else checks.push_back({"Fallback", "declared", d + "; documented as needing attacks"});
    } else {
      checks.push_back({"Fallback", "n/a", "needs S0"});
    }
  }

  // Sweeps, Repeats, Clicks: S1-S6 against the reference on the same looped input.
  const Rendered* ref = Find(rs, "S1.reference." + cn);
  Verdicts        sweeps, clicks, repeats;
  Tally           moved;  // Peak during sweeps and S11
  for (const Rendered& r : rs) {
    if (r.plan.role != Role::Sweep || !r.ok) continue;
    const std::string m = MacroShort(r.plan.macro);
    moved.Add(r, Round1(r.metrics.peakDbfs) <= kPeakMovedDbfs, r.metrics.peakDbfs);
    if (ref == nullptr || !ref->ok) continue;
    const double refStep = std::max(ref->metrics.maxStep, 1e-9);
    clicks.Add(r.metrics.maxStep <= kClickRatio * refStep,
               m + " " + Fmt("%.2fx", r.metrics.maxStep / refStep));
    // The level against the reference per 3 s window, where the reference sounds.
    std::vector<double> delta;
    double              lo = 1e9, hi = -1e9;
    for (size_t k = 0; k < r.metrics.shortTerm.size() && k < ref->metrics.shortTerm.size(); ++k) {
      if (ref->metrics.shortTerm[k] <= -70.0 || r.metrics.shortTerm[k] <= kSilentDb) {
        delta.push_back(std::numeric_limits<double>::quiet_NaN());
        continue;
      }
      const double d = r.metrics.shortTerm[k] - ref->metrics.shortTerm[k];
      delta.push_back(d);
      lo = std::min(lo, d);
      hi = std::max(hi, d);
    }
    const std::string range = lo <= hi ? Lu(lo) + ".." + Lu(hi) + " LU" : std::string("no level");
    if (r.plan.macro == ParamId::MacroActivity || r.plan.macro == ParamId::MacroShape ||
        r.plan.macro == ParamId::MacroTime) {
      sweeps.Add(lo <= hi && Round1(lo) >= -kSweepLu && Round1(hi) <= kSweepLu, m + " " + range);
    } else {
      sweeps.parts.push_back(m + " " + range + " (not judged)");
    }
    if (r.plan.macro == ParamId::MacroRepeats) {
      // The rising leg (0 to 1, from 8p s to 8p + 8 s): its windows' level does not fall.
      const double a    = 8.0 * r.plan.stored;
      bool         ok   = true;
      double       prev = std::numeric_limits<double>::quiet_NaN();
      size_t       n    = 0;
      for (size_t k = 0; k < delta.size(); ++k) {
        if (static_cast<double>(k) < a || static_cast<double>(k) + 3.0 > a + 8.0) continue;
        if (std::isnan(delta[k])) continue;
        if (!std::isnan(prev) && delta[k] < prev - kRisingTolLu) ok = false;
        prev = delta[k];
        ++n;
      }
      if (n >= 2) repeats.Add(ok, std::string("S2's rising leg ") + (ok ? "does not fall" : "falls"));
    }
  }
  checks.push_back(Make("Sweeps", sweeps));
  if (!clicks.parts.empty()) clicks.parts.front() = "largest step against the static: " + clicks.parts.front();
  checks.push_back(Make("Clicks", clicks));

  {  // S7: the Repeats ladder, level and tail non-decreasing, at most +10 LU at maximum.
    std::vector<const Rendered*> rungs;
    for (const Rendered& r : rs) {
      if (r.plan.role == Role::RepeatsRung && r.ok) rungs.push_back(&r);
    }
    if (rungs.size() >= 2) {
      bool        levelOk = true, tailOk = true;
      std::string lv, tv;
      for (size_t i = 0; i < rungs.size(); ++i) {
        const Metrics& m = rungs[i]->metrics;
        lv += (i ? " " : "") + Db(m.loudness);
        tv += (i ? " " : "") + (!m.tailFinite ? std::string("unending")
                                               : Fmt(m.tailEstimated ? "~%.0f" : "%.1f", m.tailSeconds));
        if (i > 0) {
          const Metrics& p = rungs[i - 1]->metrics;
          if (m.loudness < p.loudness - kRungTolLu) levelOk = false;
          if (!(TailKey(m) >= TailKey(p) - kRungTolSeconds)) tailOk = false;
        }
      }
      repeats.Add(levelOk, "S7 levels " + lv + " LUFS");
      repeats.Add(tailOk, "S7 tails " + tv + " s");
      const Rendered* stored = Find(rs, "S0.engaged." + cn);
      if (stored != nullptr && stored->ok) {
        const double d = rungs.back()->metrics.loudness - stored->metrics.loudness;
        repeats.Add(Round1(d) <= kRepeatsMaxLu, "at maximum " + Lu(d) + " LU over the stored position");
      }
    }
    checks.push_back(Make("Repeats", repeats));
  }

  {  // S11: Combinations hold Peak, Tail and Clicks; the worst cases are listed for listening.
    const Rendered* e = Find(rs, "S0.engaged." + cn);
    Tally           held;
    const Rendered *worstPeak = nullptr, *worstTail = nullptr, *worstStep = nullptr;
    for (const Rendered& r : rs) {
      if (r.plan.role != Role::Combination || !r.ok) continue;
      const Metrics& m = r.metrics;
      moved.Add(r, Round1(m.peakDbfs) <= kPeakMovedDbfs, m.peakDbfs);
      std::string why;
      if (Round1(m.peakDbfs) > kPeakMovedDbfs) why += " peak " + Db(m.peakDbfs) + " dBFS";
      if (!m.tailFinite && !preset.declare.selfOscillating) why += " unending tail";
      const double refStep = e != nullptr && e->ok ? std::max(e->metrics.maxStep, 1e-9) : 0.0;
      if (refStep > 0 && m.maxStep > kClickRatio * refStep) why += " step " + Fmt("%.1fx", m.maxStep / refStep);
      held.Add(r, why.empty(), 0.0, why);
      if (worstPeak == nullptr || m.peakDbfs > worstPeak->metrics.peakDbfs) worstPeak = &r;
      if (worstTail == nullptr || TailKey(m) > TailKey(worstTail->metrics)) worstTail = &r;
      if (worstStep == nullptr || m.maxStep > worstStep->metrics.maxStep) worstStep = &r;
    }
    if (held.n == 0) {
      checks.push_back({"Combinations", "n/a", "nothing to judge"});
    } else {
      std::string d = std::to_string(held.n - held.failed.size()) + " of " + std::to_string(held.n) + " hold";
      if (!held.failed.empty()) d += "; " + held.Failures();
      d += "; worst: peak " + worstPeak->plan.name + " " + Db(worstPeak->metrics.peakDbfs) + " dBFS, tail " +
           worstTail->plan.name + " " + TailText(worstTail->metrics) +
           ", step " + worstStep->plan.name + Fmt(" %.3f", worstStep->metrics.maxStep);
      checks.push_back({"Combinations", held.failed.empty() ? "pass" : "FAIL", d});
    }
  }
  if (moved.n == 0) {
    checks.push_back({"Peak (moved)", "n/a", "nothing to judge"});
  } else {
    std::string d = std::to_string(moved.n) + " sweep and S11 renders, highest " + Db(moved.worst) +
                    " dBFS (" + moved.worstName + ")";
    if (!moved.failed.empty()) d += "; over 0 dBFS: " + moved.Failures();
    checks.push_back({"Peak (moved)", moved.failed.empty() ? "pass" : "FAIL", d});
  }

  {  // Other peaks: reported, not judged (§11.3 judges the class inputs at stored positions, the
     // sweeps and S11): S0 on the other vectors and at Mix 1, and S7-S10.
    std::string d;
    auto        report = [&](const std::string& label, auto&& pick) {
      Tally t;
      for (const Rendered& r : rs) {
        if (r.ok && r.plan.role != Role::Determinism && pick(r)) {
          t.Add(r, true, r.metrics.peakDbfs);
          t.over += r.metrics.overFull;
        }
      }
      if (t.n == 0) return;
      d += (d.empty() ? "" : "; ") + label + " highest " + Db(t.worst) + " dBFS (" + t.worstName + ")";
      if (t.over != 0) d += ", " + std::to_string(t.over) + " samples over full scale";
    };
    report("S0", [&](const Rendered& r) {
      return r.plan.script == "S0" &&
             (r.plan.role != Role::Engaged ||
              std::find(inputs.begin(), inputs.end(), r.plan.vector) == inputs.end());
    });
    for (const char* script : {"S7", "S8", "S9", "S10"}) {
      report(script, [&](const Rendered& r) { return r.plan.script == script; });
    }
    checks.push_back({"Peak (other)", d.empty() ? "n/a" : "info", d.empty() ? "nothing to report" : d});
  }

  {
    const Rendered* e = Find(rs, "S0.engaged." + cn);
    std::string d = "voices and births per second are not observable through the engine's API";
    if (e != nullptr && e->ok) {
      d += "; onsets on " + cn + ": " +
           std::to_string(e->onsets) + Fmt(" (%.1f per second)", static_cast<double>(e->onsets) * kRate /
                                                                  static_cast<double>(std::max<size_t>(1, e->metrics.span)));
    }
    checks.push_back({"Load", "info", d});
  }
  return checks;
}

namespace {

struct InputKey {
  int      vector;
  bool     looped;
  uint32_t signal, tail;
  bool operator<(const InputKey& o) const {
    return std::tie(vector, looped, signal, tail) < std::tie(o.vector, o.looped, o.signal, o.tail);
  }
};

struct CachedInput {
  Input       input;
  std::string conditionedSha256;
};

const CachedInput& GetInput(std::map<InputKey, CachedInput>& cache, const Planned& p) {
  const InputKey key{static_cast<int>(p.vector), p.looped, p.signalFrames, p.tailFrames};
  auto           it = cache.find(key);
  if (it != cache.end()) return it->second;
  CachedInput c;
  c.input = p.looped ? LoopedInput(p.vector, p.signalFrames, kSignalFrames, p.tailFrames)
                     : VectorInput(p.vector, p.signalFrames, p.tailFrames);
  c.conditionedSha256 = HashRender(Conditioned(c.input.audio, InputMode::Stereo)).whole;
  return cache.emplace(key, std::move(c)).first->second;
}

std::shared_ptr<PresetState> Variant(const PresetState& base, const Planned& p) {
  auto s = std::make_shared<PresetState>(base);
  for (const auto& pos : p.positions) AtPosition(s.get(), pos.first, pos.second);
  for (const auto& leaf : p.leaves) SetLeaf(s.get(), leaf.first, leaf.second);
  return s;
}

const PresetState& DefaultPreset() {
  static const std::unique_ptr<PresetState> s = [] {
    float values[brainscape::kNumLeafParams];
    for (size_t i = 0; i < brainscape::kNumLeafParams; ++i) {
      values[i] = brainscape::FindParam(brainscape::LeafId(i))->def;
    }
    return LeafPreset(values);
  }();
  return *s;
}

// The onsets the engine's own detector hears in `audio`, per second: `audio` as the input of the
// default preset, since the detector listens to the input whatever the preset plays.
std::vector<uint32_t> HeardOnsets(Renderer& renderer, const Stereo& audio) {
  RenderRequest rq;
  rq.preset = &DefaultPreset();
  rq.input  = &audio;
  RenderResult r;
  return renderer.Render(rq, &r) ? r.onsetSeconds : std::vector<uint32_t>();
}

std::string JoinPath(const std::string& a, const std::string& b) {
  if (a.empty()) return b;
  const char last = a.back();
  return a + (last == '/' || last == '\\' ? "" : "/") + b;
}

void WriteIndex(const std::string& path, const Preset& preset, const SuiteResult& r, bool ranPreScreen) {
  JsonWriter w;
  w.BeginObject();
  w.Key("format").String("brainscape-audition-index/1");
  w.Key("soundRevision").Uint(brainscape::kSoundRevision);
  const PresetIdentity& id = preset.identity;
  w.Key("preset").BeginObject();
  w.Key("id").String(id.package ? id.id : std::string(""));
  w.Key("name").String(id.name);
  w.Key("family").String(id.family);
  w.Key("source").String(id.source);
  w.Key("soundRev").Uint(id.soundRev);
  w.Key("soundHash").String(id.soundHash);
  w.Key("controlHash").String(id.controlHash);
  w.Key("packageHash").String(id.packageHash);
  w.EndObject();
  w.Key("declare").BeginObject();
  w.Key("class").String(InputClassName(preset.declare.inputClass));
  w.Key("self_oscillating").Bool(preset.declare.selfOscillating);
  w.Key("needs_attacks").Bool(preset.declare.needsAttacks);
  w.EndObject();
  w.Key("onsetMode").Bool(OnsetMode(*preset.state));
  // Each script's hash: SHA-256 of "name hash\n" over its renders in order.
  w.Key("scripts").BeginObject();
  for (const char* s : kScriptNames) {
    std::string lines;
    size_t      n = 0;
    for (const Rendered& x : r.renders) {
      if (x.plan.script != s || x.plan.role == Role::Determinism) continue;
      lines += x.plan.name + " " + x.hashes.whole + "\n";
      ++n;
    }
    if (n == 0) continue;
    w.Key(s).BeginObject();
    w.Key("renders").Uint(n);
    w.Key("hash").String(Sha256Hex(lines));
    w.EndObject();
  }
  w.EndObject();
  w.Key("renders").BeginArray();
  for (const Rendered& x : r.renders) {
    w.BeginObject();
    w.Key("script").String(x.plan.script);
    w.Key("name").String(x.plan.name);
    w.Key("role").String(RoleName(x.plan.role));
    w.Key("ok").Bool(x.ok);
    if (!x.ok) w.Key("error").String(x.error);
    w.Key("hash").String(x.hashes.whole);
    w.Key("seconds").BeginArray();
    for (const std::string& h : x.hashes.seconds) w.String(h);
    w.EndArray();
    if (!x.wavPath.empty()) w.Key("wav").String(x.wavPath);
    if (!x.recipePath.empty()) w.Key("recipe").String(x.recipePath);
    w.Key("peakDbfs").Double(x.metrics.peakDbfs, 2);
    w.Key("loudnessLufs").Double(x.metrics.loudness, 2);
    w.Key("tailSeconds").Double(x.metrics.tailSeconds, 3);
    w.Key("tailFinite").Bool(x.metrics.tailFinite);
    w.Key("tailEstimated").Bool(x.metrics.tailEstimated);
    w.EndObject();
  }
  w.EndArray();
  w.Key("skipped").BeginArray();
  for (const std::string& s : r.skipped) w.String(s);
  w.EndArray();
  w.Key("prescreen").BeginObject();
  w.Key("ran").Bool(ranPreScreen);
  w.Key("failed").Bool(ranPreScreen && r.Failed());
  w.Key("checks").BeginArray();
  for (const Check& c : r.checks) {
    w.BeginObject();
    w.Key("name").String(c.name);
    w.Key("verdict").String(c.verdict);
    w.Key("detail").String(c.detail);
    w.EndObject();
  }
  w.EndArray();
  w.EndObject();
  w.EndObject();
  WriteText(path, w.Text());
}

}  // namespace

SuiteResult RunSuite(Renderer& renderer, const Preset& preset, const std::vector<const Preset*>& set,
                     const SuiteOptions& options) {
  SuiteResult result;
  result.ok = true;
  const std::vector<Planned> plans = Plan(preset, set, options.scripts, &result.skipped);
  const std::string          dir   = JoinPath(options.outDir, PresetDir(preset));
  std::error_code            ec;
  std::filesystem::create_directories(std::filesystem::u8path(dir), ec);

  std::map<InputKey, CachedInput> inputs;
  auto render = [&](const Planned& p, Rendered* rd, RenderResult* rr) {
    const CachedInput&           in      = GetInput(inputs, p);
    std::shared_ptr<PresetState> variant = Variant(*preset.state, p);
    RenderRequest                rq;
    rq.input        = &in.input.audio;
    rq.events       = p.events;
    rq.blockPattern = p.blockPattern;
    if (p.role == Role::Load) {
      rq.preset = p.from != nullptr ? p.from->state.get() : &DefaultPreset();
      rq.staged = {variant.get()};
    } else {
      rq.preset = variant.get();
    }
    rd->plan  = p;
    rd->ok    = renderer.Render(rq, rr);
    rd->error = rr->error;
    rd->exact = rr->load.exact;
    if (p.role == Role::Load && rd->ok) {  // the staged preset must load exactly too
      brainscape::LoadReport check;
      rd->exact = rd->exact && brainscape::CheckPreset(*variant, &check);
    }
    rd->onsets = rr->onsets;
    if (!rd->ok) return variant;
    rd->hashes  = HashRender(rr->out);
    rd->metrics = Measure(rr->out, in.input.signalFrames);
    return variant;
  };

  auto writeFiles = [&](const Planned& p, Rendered* rd, const RenderResult& rr,
                        const PresetState& loaded, bool wav) {
    const CachedInput& in = GetInput(inputs, p);
    RecipeInput        ri;
    ri.script                 = p.script;
    ri.name                   = p.name;
    ri.preset                 = &loaded;
    ri.identity               = p.role == Role::Load ? (p.from != nullptr ? p.from->identity : PresetIdentity{})
                                                     : preset.identity;
    ri.variant                = p.variant;
    ri.inputDescription       = in.input.description;
    ri.inputVector            = in.input.name;
    ri.inputSignalFrames      = in.input.signalFrames;
    ri.conditionedInputSha256 = in.conditionedSha256;
    ri.blockPattern           = p.blockPattern;
    ri.events                 = &p.events;
    ri.eventsDescription      = p.eventsDescription;
    if (p.role == Role::Load) ri.staged = {preset.identity};
    ri.result  = &rr;
    ri.hashes  = rd->hashes;
    ri.metrics = &rd->metrics;
    if (wav) {
      const std::vector<uint8_t> bytes = WavPcm16(rr.out);
      rd->wavPath                      = p.name + ".wav";
      if (WriteFile(JoinPath(dir, rd->wavPath), bytes)) {
        ri.wavPath   = rd->wavPath;
        ri.wavBits   = 16;
        ri.wavSha256 = Sha256Hex(bytes.data(), bytes.size());
      } else {
        rd->wavPath.clear();
        result.ok = false;
        rd->error = "could not write " + JoinPath(dir, p.name + ".wav");
      }
    }
    rd->recipePath = p.name + ".json";
    if (!WriteText(JoinPath(dir, rd->recipePath), RecipeJson(ri))) {
      result.ok = false;
      rd->error = "could not write " + JoinPath(dir, rd->recipePath);
    }
  };

  // Response features need the audio of the sweeps and their reference, kept per sweep.
  std::vector<Check>    response;
  Stereo                refAudio;
  std::vector<uint32_t> refHeard;  // the reference's heard onsets, measured once
  for (const Planned& p : plans) {
    Rendered     rd;
    RenderResult rr;
    const std::shared_ptr<PresetState> loaded = render(p, &rd, &rr);
    if (!rd.ok) result.ok = false;
    if (rd.ok && p.role != Role::Determinism) {
      const PresetState& asLoaded =
          p.role == Role::Load ? (p.from != nullptr ? *p.from->state : DefaultPreset()) : *loaded;
      writeFiles(p, &rd, rr, asLoaded, options.wav && p.writeWav);
    }
    if (rd.ok && p.role == Role::SweepRef) refAudio = rr.out;
    if (rd.ok && p.role == Role::Sweep && options.metrics && !refAudio.l.empty() &&
        (p.macro == ParamId::MacroShape || p.macro == ParamId::MacroActivity)) {
      // Response: features near position 0 and near 1, each against the reference at the same
      // time, so the looped input's own changes cancel.
      auto at = [&](double centre) {
        const double s = std::max(0.0, centre - 1.0), e = std::min(16.0, centre + 1.0);
        const auto   f = static_cast<size_t>(s * kRate), t = static_cast<size_t>(e * kRate);
        const SpanFeatures a = Features(rr.out, f, t), b = Features(refAudio, f, t);
        return std::make_pair(a.brightnessHz / std::max(b.brightnessHz, 1e-9), a.envelopeVarDb - b.envelopeVarDb);
      };
      const auto   z = at(8.0 * p.stored), o = at(8.0 * p.stored + 8.0);
      const double bright = 100.0 * (o.first / std::max(z.first, 1e-9) - 1.0);
      const double env    = o.second - z.second;
      const std::string d = MacroShort(p.macro) + std::string(" 0 to 1: brightness ") + Fmt("%+.1f %%", bright) +
                            ", envelope variation " + Fmt("%+.2f dB", env);
      if (p.macro == ParamId::MacroShape) {
        const bool ok = std::fabs(bright) >= kShapeBrightPct || std::fabs(env) >= kShapeEnvDb;
        response.push_back({"Response (Shape)", ok ? "pass" : "FAIL", d});
      } else {
        // Event density as the pedal's own onset detector hears the output, per second of the
        // rising leg (0 to 1), beside the reference's: the engine reports no voices or births.
        if (refHeard.empty()) refHeard = HeardOnsets(renderer, refAudio);
        const std::vector<uint32_t> heard = HeardOnsets(renderer, rr.out);
        const auto first = static_cast<size_t>(std::ceil(8.0 * p.stored));
        std::string seq, refSeq;
        for (size_t k = first; k < first + 8 && k < heard.size() && k < refHeard.size(); ++k) {
          seq += (seq.empty() ? "" : " ") + std::to_string(heard[k]);
          refSeq += (refSeq.empty() ? "" : " ") + std::to_string(refHeard[k]);
        }
        response.push_back({"Response (Activity)", "info",
                            d + "; onsets the detector hears per second from 0 to 1: " + seq +
                                " (the stored position: " + refSeq + ")"});
      }
    }
    result.renders.push_back(std::move(rd));
  }

  // S11's worst cases, written for listening: re-rendered (renders are deterministic).
  if (options.wav && options.worstWavs > 0) {
    std::vector<size_t> combos;
    for (size_t i = 0; i < result.renders.size(); ++i) {
      if (result.renders[i].plan.role == Role::Combination && result.renders[i].ok) combos.push_back(i);
    }
    std::vector<size_t> pick;
    auto byPeak = combos, byTail = combos, byStep = combos;
    auto tailKey = [&](size_t i) {
      const Metrics& m = result.renders[i].metrics;
      return m.tailFinite ? m.tailSeconds : std::numeric_limits<double>::infinity();
    };
    std::stable_sort(byPeak.begin(), byPeak.end(), [&](size_t a, size_t b) {
      return result.renders[a].metrics.peakDbfs > result.renders[b].metrics.peakDbfs;
    });
    std::stable_sort(byTail.begin(), byTail.end(), [&](size_t a, size_t b) { return tailKey(a) > tailKey(b); });
    std::stable_sort(byStep.begin(), byStep.end(), [&](size_t a, size_t b) {
      return result.renders[a].metrics.maxStep > result.renders[b].metrics.maxStep;
    });
    for (const auto* list : {&byPeak, &byTail, &byStep}) {
      for (size_t k = 0; k < list->size() && k < options.worstWavs; ++k) {
        if (std::find(pick.begin(), pick.end(), (*list)[k]) == pick.end()) pick.push_back((*list)[k]);
      }
    }
    std::sort(pick.begin(), pick.end());
    for (const size_t i : pick) {
      Rendered     again;
      RenderResult rr;
      const std::shared_ptr<PresetState> loaded = render(result.renders[i].plan, &again, &rr);
      if (!again.ok || again.hashes.whole != result.renders[i].hashes.whole) {
        result.ok                = false;
        result.renders[i].error  = "the worst-case re-render differed";
        continue;
      }
      writeFiles(result.renders[i].plan, &result.renders[i], rr, *loaded, true);
    }
  }

  if (options.metrics) {
    result.checks = PreScreen(preset, result.renders);
    for (Check& c : response) result.checks.push_back(c);
  }
  WriteIndex(JoinPath(dir, "audition.json"), preset, result, options.metrics);
  if (options.metrics) WriteText(JoinPath(dir, "prescreen.txt"), Summary(preset, result));
  return result;
}

std::string Summary(const Preset& preset, const SuiteResult& r) {
  std::string s = (preset.identity.package ? preset.identity.id : std::string("leaf-only preset")) +
                  " (" + InputClassName(preset.declare.inputClass) +
                  (preset.declare.selfOscillating ? ", self-oscillating" : "") +
                  (preset.declare.needsAttacks ? ", needs attacks" : "") + "): " +
                  std::to_string(r.renders.size()) + " renders\n";
  for (const std::string& k : r.skipped) s += "  skipped: " + k + "\n";
  for (const Check& c : r.checks) {
    char head[48];
    std::snprintf(head, sizeof head, "  %-20s %-8s ", c.name.c_str(), c.verdict.c_str());
    s += head + c.detail + "\n";
  }
  return s;
}

}  // namespace bsa
