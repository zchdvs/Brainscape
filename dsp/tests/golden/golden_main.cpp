// Golden-hash harness (docs/design/determinism-profile.md §6.1). Renders the corpus
// through the engine, prints a SHA-256 per (vector, preset) with coverage counters,
// and checks every preset exercised what it claims to.
//
//   brainscape_golden [--mode report|check|mint] [--golden FILE] [--report FILE]
//                     [--block N | --pattern A,B,... | --random-blocks SEED] [--ring LOG2]
//                     [--only NAME,...] [--delivery engine|split] [--fresh-engine]
//                     [--fp-env clean|hostile] [--ablate | --no-ablate] [--quick]
//                     [--tag TAG] [--note KEY=VALUE] [--wav-dir DIR] [--list]
//
// report (default) never compares against anything it must match: it fails only when
//        a render cannot run, a preset's coverage is missing or an invariance check
//        changes its output (exit 1).
// check  also requires every hash, per-second hash and counter, each vector's input
//        hash and ring sizes, and the header's revision, versions and engine
//        configuration to equal the golden file for this build's kSoundRevision (exit 2
//        on any difference).
// mint   writes the golden file from a canonical run (2^22 ring, 48-frame grid, events
//        through the engine's transport, one restarted engine, a clean FP environment,
//        whole corpus, ablations on). It refuses while kSoundRevision is 0: nothing is
//        minted until the engine stops changing (profile §8.4 step 10).
// --random-blocks cycles 257 block sizes of 1-512 frames drawn from the integer-only
// SplitMix32 stream at SEED, so every target derives the same pattern (profile §6.4).
// --delivery split applies events through SetParam, SetFreeze, Trigger and LoadPreset
// (Spillover) with blocks split at their frames instead of as stamped events (profile
// §5.11); --fresh-engine Inits an engine per render instead of restarting one (§5.8);
// --fp-env hostile renders from a calling thread whose control word is FTZ|DAZ (Arm
// FZ|DN) with round toward zero (§6.4). None may change a hash. Each preset's
// invariance checks (Corpus.h) run in every mode.
// With a golden file for this revision, report and check write a WAV of every preset
// that misses it into --wav-dir (profile §6.1: WAV files only on mismatch).
// --note records a fact about the build that the binary cannot see (the archive's
// SHA-256, the emulator version) in the report's build.notes; repeatable.
// A negative-control build (-DBRAINSCAPE_FP_NEGATIVE_CONTROL=ON: contraction on, test
// only) is not profile-conforming: it reports, marks the report, and refuses check and
// mint (exit 3).
// brainscape_golden_flush is this harness on the engine built with the guard's test hooks
// (dsp/tests/CMakeLists.txt). Its --force-flush-control renders every preset again with
// flushing forced on inside the guard (profile §6.4): that must reproduce every vector but
// a subnormal-input one, which it must change, or the corpus cannot see a flushing FP
// environment. It refuses mint.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if !defined(__arm__)
#include <chrono>
#endif
#if defined(_MSC_VER)
#include <crtdbg.h>
#endif

#include "../WavWriter.h"
#include "Corpus.h"
#include "Json.h"
#include "Render.h"
#include "brainscape/SoundRevision.h"
#include "brainscape/TestSignal.h"
#if defined(BRAINSCAPE_FPENV_TEST_HOOKS)
#include "detail/FpEnvGuard.h"
#endif

using namespace brainscape;
using namespace brainscape::golden;

namespace {

#ifndef BRAINSCAPE_GOLDEN_FILE
#define BRAINSCAPE_GOLDEN_FILE "golden.json"
#endif

#if defined(BRAINSCAPE_FP_NEGATIVE_CONTROL)
constexpr bool kNegativeControl = true;
#else
constexpr bool kNegativeControl = false;
#endif

#if defined(BRAINSCAPE_FPENV_TEST_HOOKS)
constexpr bool kTestHooks = true;
void SetForceFlush(bool on) { brainscape::detail::fpenv_test::forceFlush = on; }
#else
constexpr bool kTestHooks = false;
void SetForceFlush(bool) {}
#endif

constexpr const char* kReportFormat = "brainscape-golden-report/1";
constexpr const char* kGoldenFormat = "brainscape-golden/1";
constexpr const char* kUsage =
    "usage: brainscape_golden [--mode report|check|mint] [--golden FILE] [--report FILE]\n"
    "       [--block N | --pattern A,B,... | --random-blocks SEED] [--ring LOG2]\n"
    "       [--only VECTOR[/PRESET],...] [--delivery engine|split] [--fresh-engine]\n"
    "       [--fp-env clean|hostile] [--ablate | --no-ablate] [--quick] [--tag TAG]\n"
    "       [--note KEY=VALUE] [--wav-dir DIR] [--list]\n"
    "       [--force-flush-control (brainscape_golden_flush only)]\n";

struct Options {
  std::string              mode   = "report";
  std::string              golden = BRAINSCAPE_GOLDEN_FILE;
  std::string              report;
  std::string              tag = "local";
  std::string              wavDir;
  std::vector<uint32_t>    pattern{48};
  int64_t                  blockSeed = -1;  // --random-blocks
  uint32_t                 ring = 22;
  std::vector<std::string> only;
  std::vector<std::pair<std::string, std::string>> notes;
  Delivery                 delivery    = Delivery::Engine;
  bool                     freshEngine = false;
  FpEnv                    fpEnv       = FpEnv::Clean;
  bool                     ablate      = true;
  bool                     quick       = false;
  bool                     list        = false;
  bool                     flushControl = false;

  bool Canonical() const {
    return pattern.size() == 1 && pattern[0] == 48 && ring == 22 && only.empty() && !quick &&
           delivery == Delivery::Engine && !freshEngine && fpEnv == FpEnv::Clean && !flushControl;
  }
};

const char* DeliveryName(Delivery d) {
  return d == Delivery::Engine ? "engine-events" : "split-at-event-frames";
}

const char* FpEnvName(FpEnv e) { return e == FpEnv::Clean ? "clean" : "hostile"; }

// Identifies the build for triage only (profile §5.12): compiler, version, target.
std::string Toolchain() {
  std::string s;
#if defined(__clang__)
  s = std::string("clang ") + __clang_version__;
#elif defined(__GNUC__)
  s = std::string("gcc ") + __VERSION__;
#elif defined(_MSC_VER)
  s = "msvc " + std::to_string(_MSC_FULL_VER);
#else
  s = "unknown";
#endif
#if defined(__x86_64__) || defined(_M_X64)
  s += " x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
  s += " aarch64";
#elif defined(__arm__)
  s += " arm";
#if defined(__ARM_ARCH_7EM__)
  s += " armv7e-m";
#endif
#endif
#if defined(__FP_FAST_FMAF) || defined(__FMA__)
  s += " fma-capable";
#endif
#if defined(__FAST_MATH__)
  s += " fast-math";
#endif
#if defined(_M_FP_FAST)
  s += " /fp:fast";
#elif defined(_M_FP_CONTRACT)
  s += " /fp:contract";
#endif
  if (kNegativeControl) s += " NEGATIVE-CONTROL(contraction on)";
  return s;
}

std::vector<std::string> Split(const char* s) {
  std::vector<std::string> out;
  std::string              cur;
  for (; *s != '\0'; ++s) {
    if (*s == ',') {
      out.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(*s);
    }
  }
  out.push_back(cur);
  return out;
}

uint32_t Count(const char* s) { return static_cast<uint32_t>(std::atoi(s)); }

std::vector<uint32_t> RandomBlocks(uint32_t seed) {
  std::vector<uint32_t> out;
  for (uint32_t n = 0; n < 257; ++n) out.push_back(1u + testsignal::SplitMix32(seed, n) % 512u);
  return out;
}

bool Parse(int argc, char** argv, Options* o) {
  for (int i = 1; i < argc; ++i) {
    const char* a    = argv[i];
    const bool  more = i + 1 < argc;
    if (!std::strcmp(a, "--mode") && more) o->mode = argv[++i];
    else if (!std::strcmp(a, "--golden") && more) o->golden = argv[++i];
    else if (!std::strcmp(a, "--report") && more) o->report = argv[++i];
    else if (!std::strcmp(a, "--tag") && more) o->tag = argv[++i];
    else if (!std::strcmp(a, "--wav-dir") && more) o->wavDir = argv[++i];
    else if (!std::strcmp(a, "--ring") && more) o->ring = Count(argv[++i]);
    else if (!std::strcmp(a, "--only") && more) o->only = Split(argv[++i]);
    else if (!std::strcmp(a, "--delivery") && more && !std::strcmp(argv[i + 1], "engine")) {
      o->delivery = Delivery::Engine;
      ++i;
    } else if (!std::strcmp(a, "--delivery") && more && !std::strcmp(argv[i + 1], "split")) {
      o->delivery = Delivery::Split;
      ++i;
    } else if (!std::strcmp(a, "--fresh-engine")) o->freshEngine = true;
    else if (!std::strcmp(a, "--fp-env") && more && !std::strcmp(argv[i + 1], "clean")) {
      o->fpEnv = FpEnv::Clean;
      ++i;
    } else if (!std::strcmp(a, "--fp-env") && more && !std::strcmp(argv[i + 1], "hostile")) {
      o->fpEnv = FpEnv::Hostile;
      ++i;
    }
    else if (!std::strcmp(a, "--note") && more && std::strchr(argv[i + 1], '=') != nullptr) {
      const std::string kv = argv[++i];
      o->notes.emplace_back(kv.substr(0, kv.find('=')), kv.substr(kv.find('=') + 1));
    } else if ((!std::strcmp(a, "--block") || !std::strcmp(a, "--pattern")) && more) {
      o->pattern.clear();
      o->blockSeed = -1;
      for (const std::string& b : Split(argv[++i])) {
        o->pattern.push_back(Count(b.c_str()));
      }
    } else if (!std::strcmp(a, "--random-blocks") && more) {
      const auto seed = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
      o->blockSeed    = seed;
      o->pattern      = RandomBlocks(seed);
    } else if (!std::strcmp(a, "--ablate")) o->ablate = true;
    else if (!std::strcmp(a, "--no-ablate")) o->ablate = false;
    else if (!std::strcmp(a, "--quick")) o->quick = true;
    else if (!std::strcmp(a, "--list")) o->list = true;
    else if (!std::strcmp(a, "--force-flush-control") && kTestHooks) o->flushControl = true;
    else {
      std::fprintf(stderr, "unknown or incomplete argument: %s\n", a);
      return false;
    }
  }
  for (uint32_t b : o->pattern) {
    if (b < 1 || b > 512) {
      std::fprintf(stderr, "block sizes must be 1..512\n");
      return false;
    }
  }
  if (o->ring < 8 || o->ring > 26) {
    std::fprintf(stderr, "--ring must be 8..26\n");
    return false;
  }
  return o->mode == "report" || o->mode == "check" || o->mode == "mint";
}

bool Selected(const Options& o, const VectorCase& v, const PresetCase& p) {
  if (o.quick && v.longRender) return false;
  if (o.only.empty()) return true;
  for (const std::string& name : o.only) {
    if (name == v.name || name == std::string(v.name) + "/" + p.name) return true;
  }
  return false;
}

std::string Short(const std::string& hash) { return hash.substr(0, 16); }
int64_t     Get(const RenderOutput& r, Counter c) { return r.counters[static_cast<size_t>(c)]; }

// First second whose hashes differ, or -1 when every second matches.
int64_t FirstDiff(const std::vector<std::string>& a, const std::vector<std::string>& b) {
  const size_t n = a.size() < b.size() ? a.size() : b.size();
  for (size_t i = 0; i < n; ++i) {
    if (a[i] != b[i]) return static_cast<int64_t>(i);
  }
  return a.size() == b.size() ? -1 : static_cast<int64_t>(n);
}

// The earliest second an ablation may first differ in: before it, the feature had
// nothing to act on, so an earlier difference means the ablation changed more than
// the feature.
int64_t NotBeforeSecond(const PresetCase& p, Feature f, const RenderOutput& r) {
  auto firstEvent = [&](EventType t) {
    for (const Event& e : p.script.Events()) {
      if (e.type == t) return e.frame / 48000;
    }
    return int64_t{0};
  };
  switch (f) {
    case Feature::MarkPosition:
    case Feature::OnsetTrigger: return r.firstOnsetBlock >= 0 ? r.firstOnsetBlock / 48000 : 0;
    case Feature::Freeze: return firstEvent(EventType::Freeze);
    case Feature::Triggers: return firstEvent(EventType::Trigger);
    case Feature::RingLength: return r.ringReachFrame / 48000;
    case Feature::Spillover: return firstEvent(EventType::SpilloverLoad);
    case Feature::Restart:
      return p.script.Restarts().empty() ? 0 : p.script.Restarts().front().frame / 48000;
    default: return 0;
  }
}

bool SameCounters(const RenderOutput& a, const RenderOutput& b) {
  for (size_t i = 0; i < static_cast<size_t>(Counter::kCount); ++i) {
    if (a.counters[i] != b.counters[i]) return false;
  }
  return true;
}

// What an invariance render changed: the output, the counters, or both.
std::string InvarianceDiff(const RenderOutput& ref, const RenderOutput& got) {
  std::string d;
  if (got.hash != ref.hash || got.secondHashes != ref.secondHashes) {
    d += "output from s" + std::to_string(FirstDiff(ref.secondHashes, got.secondHashes));
  }
  for (size_t i = 0; i < static_cast<size_t>(Counter::kCount); ++i) {
    if (got.counters[i] == ref.counters[i]) continue;
    d += (d.empty() ? "" : ", ") + std::string(CounterName(static_cast<Counter>(i))) + " " +
         std::to_string(ref.counters[i]) + " -> " + std::to_string(got.counters[i]);
  }
  return d;
}

std::string StrOf(const Json& j, const char* key) {
  const Json* v = j.Find(key);
  return v != nullptr ? v->string : std::string();
}

const Json* Named(const Json* list, const std::string& name) {
  if (list == nullptr) return nullptr;
  for (const Json& c : list->items) {
    const Json* n = c.Find("name");
    if (n != nullptr && n->string == name) return &c;
  }
  return nullptr;
}

Json ReportHeader(const Options& o, uint32_t historyFrames) {
  Json report = Json::Obj();
  report.Set("format", Json::Str(kReportFormat));
  report.Set("soundRevision", Json::Int(kSoundRevision));
  report.Set("generatorVersion", Json::Int(testsignal::kVersion));
  report.Set("corpusVersion", Json::Int(kCorpusVersion));
  report.Set("sampleRate", Json::Int(48000));
  report.Set("stereoInput", Json::Bool(true));
  report.Set("ditherRingWrite", Json::Bool(true));
  report.Set("historyFrames", Json::Int(historyFrames));
  Json build = Json::Obj();
  build.Set("tag", Json::Str(o.tag));
  build.Set("toolchain", Json::Str(Toolchain()));
  const ToolchainId& id = BuildToolchain();  // the engine library's own (profile §5.12)
  Json engineToolchain  = Json::Obj();
  engineToolchain.Set("compiler", Json::Str(id.compiler));
  engineToolchain.Set("version", Json::Str(id.version));
  engineToolchain.Set("target", Json::Str(id.target));
  engineToolchain.Set("fpFlags", Json::Str(id.fpFlags));
  engineToolchain.Set("fpFlagsHash", Json::Str(id.fpFlagsHash));
  build.Set("engineToolchain", engineToolchain);
  Json pattern = Json::Arr();
  for (uint32_t b : o.pattern) pattern.Push(Json::Int(b));
  build.Set("blockPattern", pattern);
  if (o.blockSeed >= 0) build.Set("blockSeed", Json::Int(o.blockSeed));
  build.Set("delivery", Json::Str(DeliveryName(o.delivery)));
  build.Set("start", Json::Str(o.freshEngine ? "init" : "restart"));
  build.Set("fpEnv", Json::Str(FpEnvName(o.fpEnv)));
  build.Set("ablations", Json::Bool(o.ablate));
  build.Set("canonical", Json::Bool(o.Canonical() && !kNegativeControl));
  build.Set("negativeControl", Json::Bool(kNegativeControl));
  build.Set("forcedFlushControl", Json::Bool(o.flushControl));
  Json notes = Json::Obj();
  for (const auto& kv : o.notes) notes.Set(kv.first, Json::Str(kv.second));
  build.Set("notes", notes);
  report.Set("build", build);
  report.Set("vectors", Json::Arr());
  return report;
}

// Renders one preset, checks its requirements and ablations, prints its line and
// returns its report entry; *ok is false when coverage is missing.
Json RunPreset(Renderer& renderer, const Options& o, const VectorCase& v,
               const std::vector<testsignal::Note>& notes, const PresetCase& p,
               const RenderOutput& r, bool* ok) {
  Json e = Json::Obj();
  e.Set("name", Json::Str(p.name));
  e.Set("hash", Json::Str(r.hash));
  Json secs = Json::Arr();
  for (const std::string& h : r.secondHashes) secs.Push(Json::Str(h));
  e.Set("secondHashes", secs);
  Json counters = Json::Obj();
  for (size_t i = 0; i < static_cast<size_t>(Counter::kCount); ++i) {
    counters.Set(CounterName(static_cast<Counter>(i)), Json::Int(r.counters[i]));
  }
  e.Set("counters", counters);

  std::vector<std::string> failures;
  for (const Requirement& q : p.require) {
    const int64_t got = Get(r, q.counter);
    if (got < q.min) {
      failures.push_back(std::string(CounterName(q.counter)) + "=" + std::to_string(got) +
                         " < " + std::to_string(q.min));
    } else if (got > q.max) {
      failures.push_back(std::string(CounterName(q.counter)) + "=" + std::to_string(got) +
                         " > " + std::to_string(q.max));
    }
  }
  Json ablations = Json::Arr();
  for (const Feature f : o.ablate ? p.ablate : std::vector<Feature>{}) {
    const std::string name = std::string("ablation ") + FeatureName(f);
    RenderOutput      ra;
    bool              rendered;
    if (f == Feature::RingLength) {
      RenderConfig rc = renderer.Config();
      ++rc.historyLog2;
      Renderer doubled(rc);
      rendered = doubled.ok() && doubled.Render(v, notes, p, &ra);
    } else {
      rendered = renderer.Render(v, notes, Ablate(p, f), &ra);
    }
    if (!rendered) {
      failures.push_back(name + " did not render");
      continue;
    }
    const bool    changed   = ra.hash != r.hash;
    const int64_t first     = FirstDiff(r.secondHashes, ra.secondHashes);
    const int64_t notBefore = NotBeforeSecond(p, f, r);
    Json          a         = Json::Obj();
    a.Set("feature", Json::Str(FeatureName(f)));
    a.Set("changed", Json::Bool(changed));
    a.Set("firstDiffSecond", Json::Int(first));
    a.Set("notBeforeSecond", Json::Int(notBefore));
    a.Set("ok", Json::Bool(changed && first >= notBefore));
    ablations.Push(a);
    if (!changed) {
      failures.push_back(name + " changed nothing");
    } else if (first < notBefore) {
      failures.push_back(name + " first differs at s" + std::to_string(first) + ", before s" +
                         std::to_string(notBefore));
    }
  }
  e.Set("ablations", ablations);

  // Invariances must reproduce the render (profile §6.4).
  Json invariances = Json::Arr();
  for (const Invariance inv : p.invariant) {
    const std::string name = std::string("invariance ") + InvarianceName(inv);
    RenderOutput      ri;
    bool              rendered = false, same = false;
    Json              a = Json::Obj();
    a.Set("check", Json::Str(InvarianceName(inv)));
    if (inv == Invariance::HostileFpEnv) {
      RenderConfig rc = renderer.Config();
      rc.fpEnv        = rc.fpEnv == FpEnv::Clean ? FpEnv::Hostile : FpEnv::Clean;
      rc.freshEngine  = true;  // Init too, in the other environment
      Renderer other(rc);
      rendered = other.ok() && other.Render(v, notes, p, &ri);
      same     = rendered && ri.hash == r.hash && ri.secondHashes == r.secondHashes &&
             SameCounters(ri, r);
      a.Set("fpEnv", Json::Str(FpEnvName(rc.fpEnv)));
      a.Set("firstDiffSecond", Json::Int(rendered ? FirstDiff(r.secondHashes, ri.secondHashes) : -1));
    } else if (!p.script.Restarts().empty()) {
      int64_t          from = 0;
      const PresetCase tail = TailAfterRestart(p, &from);
      rendered = renderer.Render(v, notes, tail, &ri, nullptr, from);
      same     = rendered && r.restartFrame == from && ri.hash == r.restartHash;
      a.Set("fromFrame", Json::Int(from));
    }
    a.Set("ok", Json::Bool(same));
    invariances.Push(a);
    if (!rendered) {
      failures.push_back(name + " did not render");
    } else if (!same) {
      const std::string diff =
          inv == Invariance::HostileFpEnv ? InvarianceDiff(r, ri) : "output from the restart";
      failures.push_back(name + " changed " + diff);
    }
  }
  e.Set("invariances", invariances);

  // The forced-flush control (profile §6.4), on a fresh engine like the hostile check.
  if (o.flushControl) {
    RenderConfig rc = renderer.Config();
    rc.freshEngine  = true;
    Renderer     forced(rc);
    RenderOutput rf;
    SetForceFlush(true);
    const bool rendered = forced.ok() && forced.Render(v, notes, p, &rf);
    SetForceFlush(false);
    const bool    changed = rendered && rf.hash != r.hash;
    const int64_t first   = rendered ? FirstDiff(r.secondHashes, rf.secondHashes) : -1;
    Json          f       = Json::Obj();
    f.Set("expectChange", Json::Bool(v.subnormalInput));
    f.Set("changed", Json::Bool(changed));
    f.Set("firstDiffSecond", Json::Int(first));
    f.Set("ok", Json::Bool(rendered && changed == v.subnormalInput));
    e.Set("forcedFlush", f);
    if (!rendered) {
      failures.push_back("forced flush did not render");
    } else if (changed && !v.subnormalInput) {
      failures.push_back("forced flush changed the output from s" + std::to_string(first) +
                         ": a golden vector must reproduce it");
    } else if (!changed && v.subnormalInput) {
      failures.push_back("forced flush changed nothing: the vector cannot see flushing");
    }
  }

  Json cov = Json::Obj();
  cov.Set("ok", Json::Bool(failures.empty()));
  Json list = Json::Arr();
  for (const std::string& f : failures) list.Push(Json::Str(f));
  cov.Set("failures", list);
  e.Set("coverage", cov);

  const bool  ablated = o.ablate && !p.ablate.empty();
  const char* status  = !failures.empty()                ? "COVERAGE FAILED"
                        : ablated && !p.invariant.empty() ? "coverage ok (ablations, invariances ok)"
                        : ablated                         ? "coverage ok (ablations ok)"
                        : !p.invariant.empty()            ? "coverage ok (invariances ok)"
                                                          : "coverage ok";
  std::printf("  %-24s %s  onsets=%lld frozen=%lld events=%lld offgrid=%lld tail=%lld  %s\n",
              p.name, Short(r.hash).c_str(), static_cast<long long>(Get(r, Counter::Onsets)),
              static_cast<long long>(Get(r, Counter::FrozenFrames)),
              static_cast<long long>(Get(r, Counter::Events)),
              static_cast<long long>(Get(r, Counter::OffGridEvents)),
              static_cast<long long>(Get(r, Counter::TailActiveFrames)), status);
  for (const std::string& f : failures) std::printf("      - %s\n", f.c_str());
  std::fflush(stdout);
  *ok = failures.empty();
  return e;
}

// The golden file is the report minus what describes one run: the build, and each
// preset's coverage, ablation and invariance results.
Json GoldenFrom(const Json& report) {
  Json g = Json::Obj();
  for (const auto& m : report.members) {
    if (m.first == "build") continue;
    if (m.first != "vectors") {
      g.Set(m.first, m.second);
      continue;
    }
    Json vectors = Json::Arr();
    for (const Json& v : m.second.items) {
      Json vo = Json::Obj();
      for (const auto& vm : v.members) {
        if (vm.first != "presets") {
          vo.Set(vm.first, vm.second);
          continue;
        }
        Json presets = Json::Arr();
        for (const Json& p : vm.second.items) {
          Json po = Json::Obj();
          for (const auto& pm : p.members) {
            if (pm.first != "coverage" && pm.first != "ablations" && pm.first != "invariances" &&
                pm.first != "forcedFlush") {
              po.Set(pm.first, pm.second);
            }
          }
          presets.Push(po);
        }
        vo.Set("presets", presets);
      }
      vectors.Push(vo);
    }
    g.Set("vectors", vectors);
  }
  g.Set("format", Json::Str(kGoldenFormat));
  return g;
}

bool SameInts(const Json* a, const Json* b) {
  if (a == nullptr || b == nullptr || a->items.size() != b->items.size()) return false;
  for (size_t i = 0; i < a->items.size(); ++i) {
    if (a->items[i].integer != b->items[i].integer) return false;
  }
  return true;
}

std::vector<std::string> Strings(const Json* list) {
  std::vector<std::string> out;
  if (list != nullptr) {
    for (const Json& s : list->items) out.push_back(s.string);
  }
  return out;
}

// Compares a run against a golden file; returns the differences, empty when equal.
// A run of the whole corpus must also have rendered everything the golden file has.
std::vector<std::string> Compare(const Json& golden, const Json& run, bool wholeCorpus) {
  std::vector<std::string> diffs;
  for (const char* key : {"soundRevision", "generatorVersion", "corpusVersion", "sampleRate",
                          "historyFrames"}) {
    const Json* g = golden.Find(key);
    const Json* r = run.Find(key);
    if (g == nullptr || r == nullptr || g->integer != r->integer) {
      diffs.push_back(std::string(key) + " differs");
    }
  }
  for (const char* key : {"stereoInput", "ditherRingWrite"}) {
    const Json* g = golden.Find(key);
    const Json* r = run.Find(key);
    if (g == nullptr || r == nullptr || g->boolean != r->boolean) {
      diffs.push_back(std::string(key) + " differs");
    }
  }
  if (!diffs.empty()) return diffs;

  const Json* gv = golden.Find("vectors");
  const Json* rv = run.Find("vectors");
  if (gv == nullptr || rv == nullptr) return {"no vectors"};
  for (const Json& v : rv->items) {
    const std::string name = StrOf(v, "name");
    const Json*       g    = Named(gv, name);
    if (g == nullptr) {
      diffs.push_back(name + ": not in the golden file");
      continue;
    }
    for (const char* key : {"generatorVersion", "frames"}) {
      if (g->Find(key) == nullptr || g->Find(key)->integer != v.Find(key)->integer) {
        diffs.push_back(name + ": " + key + " differs");
      }
    }
    if (StrOf(*g, "inputHash") != StrOf(v, "inputHash")) {
      diffs.push_back(name + ": inputHash differs (generator)");
    }
    if (!SameInts(g->Find("ringSizes"), v.Find("ringSizes"))) {
      diffs.push_back(name + ": ringSizes differ");
    }
    for (const Json& p : v.Find("presets")->items) {
      const std::string pname = name + "/" + StrOf(p, "name");
      const Json*       gp    = Named(g->Find("presets"), StrOf(p, "name"));
      if (gp == nullptr) {
        diffs.push_back(pname + ": not in the golden file");
        continue;
      }
      const int64_t second =
          FirstDiff(Strings(gp->Find("secondHashes")), Strings(p.Find("secondHashes")));
      if (StrOf(*gp, "hash") != StrOf(p, "hash")) {
        diffs.push_back(pname + ": hash differs from second " + std::to_string(second));
      } else if (second >= 0) {
        diffs.push_back(pname + ": per-second hashes differ from second " +
                        std::to_string(second));
      }
      const Json* gc = gp->Find("counters");
      const Json* rc = p.Find("counters");
      for (const auto& m : rc->members) {
        const Json* want = gc != nullptr ? gc->Find(m.first) : nullptr;
        if (want == nullptr || want->integer != m.second.integer) {
          diffs.push_back(pname + ": counter " + m.first + " = " +
                          std::to_string(m.second.integer) +
                          (want != nullptr ? ", golden " + std::to_string(want->integer)
                                           : ", not in golden"));
        }
      }
      if (gc != nullptr) {
        for (const auto& m : gc->members) {
          if (rc->Find(m.first) == nullptr) {
            diffs.push_back(pname + ": counter " + m.first + " is in golden but not counted");
          }
        }
      }
    }
  }
  if (wholeCorpus) {
    for (const Json& g : gv->items) {
      const std::string vname = StrOf(g, "name");
      const Json*       v     = Named(rv, vname);
      const Json*       gps   = g.Find("presets");
      if (gps == nullptr) continue;
      for (const Json& p : gps->items) {
        const std::string pname = StrOf(p, "name");
        if (v == nullptr || Named(v->Find("presets"), pname) == nullptr) {
          diffs.push_back(vname + "/" + pname + ": in the golden file but not rendered");
        }
      }
    }
  }
  return diffs;
}

// Re-renders every preset whose hash misses the golden file and writes it as a WAV.
void WriteMismatchWavs(Renderer& renderer, const std::vector<VectorCase>& corpus,
                       const Json& golden, const Json& report, const std::string& dir) {
  for (const VectorCase& v : corpus) {
    const Json* gv = Named(golden.Find("vectors"), v.name);
    const Json* rv = Named(report.Find("vectors"), v.name);
    if (gv == nullptr || rv == nullptr) continue;
    const std::vector<testsignal::Note> notes = VectorNotes(v);
    for (const PresetCase& p : v.presets) {
      const Json* gp = Named(gv->Find("presets"), p.name);
      const Json* rp = Named(rv->Find("presets"), p.name);
      if (gp == nullptr || rp == nullptr || StrOf(*gp, "hash") == StrOf(*rp, "hash")) continue;
      RenderOutput      r;
      Capture           cap;
      const std::string path = dir + "/" + v.name + "__" + p.name + ".wav";
      if (renderer.Render(v, notes, p, &r, &cap) &&
          tools::WriteWavFloat32Stereo(path.c_str(), cap.l.data(), cap.r.data(), cap.l.size(),
                                       48000)) {
        std::printf("# golden: wrote %s\n", path.c_str());
      }
    }
  }
}

bool ReadFile(const std::string& path, std::string* out) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (f == nullptr) return false;
  char   buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out->append(buf, n);
  std::fclose(f);
  return true;
}

bool WriteFile(const std::string& path, const std::string& text) {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (f == nullptr) return false;
  const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
  return std::fclose(f) == 0 && ok;
}

}  // namespace

int main(int argc, char** argv) {
#if defined(_MSC_VER)
  // MSVC's debug CRT answers a failed assert or abort() with a dialog, which hangs an
  // unattended run (CI, ctest); report on stderr and exit instead.
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
  for (int type : {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT}) {
    _CrtSetReportMode(type, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(type, _CRTDBG_FILE_STDERR);
  }
#endif
  Options o;
  if (argc == 2 && (!std::strcmp(argv[1], "--help") || !std::strcmp(argv[1], "-h"))) {
    std::fputs(kUsage, stdout);
    return 0;
  }
  if (!Parse(argc, argv, &o)) {
    std::fputs(kUsage, stderr);
    return 3;
  }
  const std::vector<VectorCase> corpus = BuildCorpus();
  // Presets and events hold canonical values, as packages do (profile §3.7): a value
  // SetParam would store differently is a corpus bug, which no leg could see from its
  // hashes alone.
  int nonCanonical = 0;
  for (const VectorCase& v : corpus) {
    for (const PresetCase& p : v.presets) {
      auto check = [&](ParamId id, float value) {
        const float canonical = Canonicalize(id, value);
        if (std::memcmp(&canonical, &value, sizeof value) != 0) {
          std::fprintf(stderr, "%s/%s: parameter %u value %a is not canonical (%a)\n", v.name,
                       p.name, static_cast<unsigned>(id), static_cast<double>(value),
                       static_cast<double>(canonical));
          ++nonCanonical;
        }
      };
      for (const auto& kv : p.params) check(kv.first, kv.second);
      for (const Event& e : p.script.Events()) {
        if (e.type == EventType::SetParam) check(static_cast<ParamId>(e.id), e.value);
      }
    }
  }
  if (nonCanonical > 0) return 1;
  if (o.list) {
    for (const VectorCase& v : corpus) {
      std::printf("%s (%s, %u s%s)\n", v.name, testsignal::VectorName(v.source),
                  static_cast<unsigned>(v.frames / 48000u), v.longRender ? ", long" : "");
      for (const PresetCase& p : v.presets) std::printf("  %s/%s\n", v.name, p.name);
    }
    return 0;
  }
  if (kNegativeControl) {
    std::fprintf(stderr,
                 "*** NEGATIVE CONTROL BUILD (BRAINSCAPE_FP_NEGATIVE_CONTROL): contraction is ON. "
                 "Not profile-conforming; its hashes must differ from every conforming build. "
                 "Report only. ***\n");
    if (o.mode != "report") {
      std::fprintf(stderr, "%s refused: a negative-control build only reports\n", o.mode.c_str());
      return 3;
    }
  }
  if (o.mode == "mint" && kTestHooks) {
    std::fprintf(stderr, "mint refused: this harness runs the engine built with test hooks\n");
    return 3;
  }
  if (o.mode == "mint" && kSoundRevision == 0) {
    std::fprintf(stderr, "mint refused: kSoundRevision is 0, so there is no revision to mint\n");
    return 3;
  }
  if (o.mode == "mint" && (!o.Canonical() || !o.ablate)) {
    std::fprintf(stderr,
                 "mint refused: needs the whole corpus at --block 48 --ring 22 with ablations, "
                 "engine delivery, a restarted engine and a clean FP environment\n");
    return 3;
  }

  std::string pattern;
  for (uint32_t b : o.pattern) pattern += (pattern.empty() ? "" : ",") + std::to_string(b);
  if (o.blockSeed >= 0) {
    pattern = "random 1-512 (seed " + std::to_string(o.blockSeed) + ", " +
              std::to_string(o.pattern.size()) + " sizes)";
  }
  std::printf("# brainscape golden: soundRevision %u, generator v%u, corpus v%u, ring 2^%u, "
              "blocks %s, events %s, %s, %s FP environment\n# toolchain: %s (fp flags %s)\n",
              static_cast<unsigned>(kSoundRevision), static_cast<unsigned>(testsignal::kVersion),
              static_cast<unsigned>(kCorpusVersion), static_cast<unsigned>(o.ring),
              pattern.c_str(), DeliveryName(o.delivery),
              o.freshEngine ? "an engine Init'd per render" : "one engine restarted per render",
              FpEnvName(o.fpEnv), Toolchain().c_str(), BuildToolchain().fpFlagsHash);

  RenderConfig rc;
  rc.blockPattern = o.pattern;
  rc.historyLog2  = o.ring;
  rc.delivery     = o.delivery;
  rc.freshEngine  = o.freshEngine;
  rc.fpEnv        = o.fpEnv;
  Renderer renderer(rc);
  if (!renderer.ok()) {
    std::fprintf(stderr, "cannot allocate the engine arenas\n");
    return 3;
  }
#if !defined(__arm__)
  const auto t0 = std::chrono::steady_clock::now();
#endif

  Json report  = ReportHeader(o, renderer.HistoryFrames());
  Json vectors = Json::Arr();
  int  coverageFailures = 0, renderFailures = 0, presets = 0;
  for (const VectorCase& v : corpus) {
    bool any = false;
    for (const PresetCase& p : v.presets) any = any || Selected(o, v, p);
    if (!any) continue;

    const std::vector<testsignal::Note> notes = VectorNotes(v);
    const std::string                   input = InputHash(v, notes);
    std::printf("%-22s input %s  %u notes, %u frames\n", v.name, Short(input).c_str(),
                static_cast<unsigned>(notes.size()), static_cast<unsigned>(v.frames));
    Json vj = Json::Obj();
    vj.Set("name", Json::Str(v.name));
    vj.Set("source", Json::Str(testsignal::VectorName(v.source)));
    vj.Set("generatorVersion", Json::Int(testsignal::kVersion));
    vj.Set("frames", Json::Int(v.frames));
    vj.Set("inputHash", Json::Str(input));
    Json rings = Json::Arr();
    rings.Push(Json::Int(int64_t{1} << 22));  // only the canonical ring is proven (profile §6.4)
    vj.Set("ringSizes", rings);

    Json pj = Json::Arr();
    for (const PresetCase& p : v.presets) {
      if (!Selected(o, v, p)) continue;
      ++presets;
      RenderOutput r;
      if (!renderer.Render(v, notes, p, &r)) {
        std::printf("  %-24s RENDER FAILED\n", p.name);
        ++renderFailures;
        continue;
      }
      bool ok = true;
      pj.Push(RunPreset(renderer, o, v, notes, p, r, &ok));
      if (!ok) ++coverageFailures;
    }
    vj.Set("presets", pj);
    vectors.Push(vj);
  }
  report.Set("vectors", vectors);

#if !defined(__arm__)
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::printf("# %d presets rendered in %.1f s\n", presets, secs);
#endif
  if (!o.report.empty() && !WriteFile(o.report, ToText(report))) {
    std::fprintf(stderr, "cannot write %s\n", o.report.c_str());
    return 3;
  }
  const int status = renderFailures > 0 || coverageFailures > 0 ? 1 : 0;
  if (coverageFailures > 0) {
    std::printf("# coverage FAILED for %d preset(s): the corpus does not exercise them\n",
                coverageFailures);
  }

  if (o.mode == "mint") {
    if (status != 0) {
      std::fprintf(stderr, "mint refused: the run has failures\n");
      return status;
    }
    if (!WriteFile(o.golden, ToText(GoldenFrom(report)))) {
      std::fprintf(stderr, "cannot write %s\n", o.golden.c_str());
      return 3;
    }
    std::printf("# minted %s for sound revision %u\n", o.golden.c_str(),
                static_cast<unsigned>(kSoundRevision));
    return 0;
  }

  const int   missing = o.mode == "check" ? 2 : status;
  std::string text, err;
  Json        golden;
  if (!ReadFile(o.golden, &text)) {
    std::printf("# no golden file at %s\n", o.golden.c_str());
    return missing;
  }
  if (!FromText(text, &golden, &err) || StrOf(golden, "format") != kGoldenFormat) {
    std::printf("# golden file %s is unreadable %s\n", o.golden.c_str(), err.c_str());
    return missing;
  }
  const Json* rev = golden.Find("soundRevision");
  if (rev == nullptr || rev->integer != static_cast<int64_t>(kSoundRevision)) {
    std::printf("# golden file is for sound revision %lld, this build is %u: nothing to compare\n",
                rev != nullptr ? static_cast<long long>(rev->integer) : -1LL,
                static_cast<unsigned>(kSoundRevision));
    return missing;
  }
  const std::vector<std::string> diffs = Compare(golden, report, o.Canonical());
  for (const std::string& d : diffs) std::printf("# golden: %s\n", d.c_str());
  if (!o.wavDir.empty()) WriteMismatchWavs(renderer, corpus, golden, report, o.wavDir);
  std::printf("# golden: %s\n", diffs.empty() ? "every rendered preset matches" : "MISMATCH");
  if (o.mode == "check" && !diffs.empty()) return 2;
  return status;
}
