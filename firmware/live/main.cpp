// Image C, LIVE AUDIO (firmware/README.md): the engine in libDaisy's audio callback at 48 kHz
// in 48-frame blocks on the Seed's own codec (Rev7: PCM3060, line level, pins 16/17 in and
// 18/19 out), the pedal's configuration (maxBlockSize 48, the 2^22 ring), with a few presets
// from the golden corpus and a text protocol over USB serial (tools/hil/console.py). Every
// change reaches the engine as a frame-stamped event through its EventQueue, stamped at the
// next block boundary (determinism-profile.md §5.11), as the pedal's control loop will.
//
// Sound revision 2 (mode-compiler.md §7): the onset trigger and mark positioning are mode
// structure, not parameters (rows 27 and 28 are retired), so `onset` and `marks` load a
// PresetState with another package's mode (live/LivePresets.h); the macros and the
// expression pedal are their own events.
//
// Commands, one per line:
//   info | list | params | get
//   preset N [exact|cut]  load preset N: a Spillover event (trails kept, real-time), with
//                         "cut" a FastCut one (the old grains fade out over 128 frames), or
//                         with "exact" an Exact load (Restart: the output is muted while it runs)
//   set NAME VALUE        a parameter event; NAME is the descriptor name (layer0.size_ms),
//                         its ParamId (GrainSizeMs) or a short alias (size); the effect volume
//                         (volume) is a device setting, kept by every load
//   onset on|off          the mode's onset source / layer 0 at marks: a Spillover load of the
//   marks on|off          current leaves with the matching structure (also "set onset|marks V",
//                         V >= 0.5 on, as sessions migrate rows 27 and 28)
//   macro NAME POS        a macro move (activity repeats shape time space filter aux1 aux2), 0-1
//   expression POS        the expression pedal, 0-1 (does nothing without assignments)
//   freeze on|off | trigger
//   stats [reset]         CPU load: DWT cycles of Engine::Process per callback, mean and peak
//   dfu                   reboot into the bootloader
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#include "JsonLine.h"
#include "brainscape/Engine.h"
#include "brainscape/EventQueue.h"
#include "brainscape/ModeEval.h"
#include "brainscape/Params.h"
#include "live/LivePresets.h"
#include "platform/Placement.h"
#include "platform/Platform.h"
#include "platform/SeedHw.h"
#include "platform/UsbSerial.h"

using namespace brainscape;
using brainscape::fw::Serial;
using brainscape::fw::UsbSerial;
using brainscape::golden::JsonObj;
using brainscape::golden::JsonString;
using brainscape::golden::JsonUInt;
namespace live = brainscape::fw::live;

namespace {

constexpr uint32_t kBlock = 48;

PresetState g_presets[live::kNumSlots];          // complete presets, built from the corpus at boot
PresetState g_structures[live::kNumStructures];  // their mode and CTRL only
PresetState g_now;                // what the engine plays, as the producer sent it: the last
                                  // load, then every parameter, macro and expression move
PresetState g_staged[2];          // Spillover payloads, valid until their event retires
uint32_t    g_stagedEvent[2] = {0, 0};
bool        g_stagedUsed[2]  = {false, false};
uint32_t    g_nextStage      = 0;

Engine*                  g_engine = nullptr;
EventQueue               g_queue;
Engine::BlockEvent       g_blockEvents[EventQueue::kCapacity];
int64_t                  g_frame = 0;  // audio thread: the engine timeline's next frame
std::atomic<uint32_t>    g_blocks{0};  // published block count of the timeline
std::atomic<bool>        g_muted{false};
std::atomic<uint32_t>    g_mutedCallbacks{0};
uint32_t                 g_seq = 0;    // producer: sequence numbers of stamped events
int32_t                  g_current = 0;

// CPU meter (audio thread writes, main loop reads and resets).
std::atomic<uint32_t> g_statCount{0};
std::atomic<uint32_t> g_statPeak{0};
std::atomic<uint32_t> g_statOver{0};
volatile uint64_t     g_statSum = 0;
std::atomic<bool>     g_statReset{false};
std::atomic<uint32_t> g_onsets{0};

void AudioCallback(daisy::AudioHandle::InputBuffer in, daisy::AudioHandle::OutputBuffer out,
                   size_t size) {
  if (g_statReset.exchange(false, std::memory_order_relaxed)) {
    g_statCount.store(0, std::memory_order_relaxed);
    g_statPeak.store(0, std::memory_order_relaxed);
    g_statOver.store(0, std::memory_order_relaxed);
    g_statSum = 0;
  }
  if (g_muted.load(std::memory_order_acquire) || size == 0 || size > kBlock) {
    for (size_t i = 0; i < size; ++i) out[0][i] = out[1][i] = 0.f;
    g_mutedCallbacks.fetch_add(1, std::memory_order_release);
    return;
  }
  const uint32_t t0 = fw::Cycles();
  Engine::ProcessContext ctx;
  ctx.in        = in;
  ctx.out       = out;
  ctx.numFrames = static_cast<uint32_t>(size);
  ctx.events    = g_blockEvents;
  ctx.numEvents = g_queue.PopBlock(g_frame, ctx.numFrames, g_blockEvents, EventQueue::kCapacity);
  g_engine->Process(ctx);
  const uint32_t cycles = fw::Cycles() - t0;

  g_frame += static_cast<int64_t>(size);
  g_blocks.fetch_add(1, std::memory_order_release);
  g_statSum = g_statSum + cycles;
  g_statCount.fetch_add(1, std::memory_order_relaxed);
  if (cycles > g_statPeak.load(std::memory_order_relaxed)) g_statPeak.store(cycles, std::memory_order_relaxed);
  // The block's budget: its frames at 48 kHz on the 480 MHz core.
  if (cycles > static_cast<uint32_t>(size) * 10000u) g_statOver.fetch_add(1, std::memory_order_relaxed);
  const uint32_t onsets = g_engine->ConsumeOnsetCount();
  if (onsets != 0) {
    g_onsets.fetch_add(onsets, std::memory_order_relaxed);
    fw::PulseLed(60);
  }
}

// ---- Producer side (main loop) ---------------------------------------------------------

void Reply(const std::string& json) { Serial().WriteLine(json, UsbSerial::Mode::Drop); }
void Error(const std::string& m) { Reply(JsonObj().Str("type", "error").Str("message", m).Done()); }

// The next block boundary the audio thread has not reached: a late stamp is applied at the
// start of the block that takes it, never dropped.
int64_t NextBoundary() {
  return static_cast<int64_t>(g_blocks.load(std::memory_order_acquire) + 1u) * kBlock;
}

bool PushEvent(Engine::EventType type, uint32_t id, float value, const PresetState* preset,
               uint32_t* index = nullptr) {
  Engine::Event e;
  e.frame  = NextBoundary();
  e.seq    = g_seq++;
  e.type   = type;
  e.id     = id;
  e.value  = value;
  e.preset = preset;
  if (g_queue.Push(e, index)) return true;
  Error("the event queue refused the event (full, or out of order)");
  return false;
}

// Stops Process from running (the callback outputs silence) and waits until a whole callback
// has seen it, so the main loop may call the non-real-time API.
void Mute() {
  g_muted.store(true, std::memory_order_release);
  const uint32_t seen = g_mutedCallbacks.load(std::memory_order_acquire);
  const uint32_t t0   = daisy::System::GetNow();
  while (g_mutedCallbacks.load(std::memory_order_acquire) - seen < 2u) {
    if (daisy::System::GetNow() - t0 > 100u) break;  // audio not running: nothing to wait for
  }
}

void Unmute() { g_muted.store(false, std::memory_order_release); }

// A float as text without printf: sign, integer part and four decimals, plus its exact bits.
std::string FloatText(float v) {
  std::string s;
  if (v < 0.f) {
    s = "-";
    v = -v;
  }
  const auto whole = static_cast<uint64_t>(v);
  auto       frac  = static_cast<uint64_t>((v - static_cast<float>(whole)) * 10000.f + 0.5f);
  uint64_t   w     = whole;
  if (frac >= 10000u) {
    frac -= 10000u;
    ++w;
  }
  std::string f = JsonUInt(frac);
  while (f.size() < 4) f = "0" + f;
  return s + JsonUInt(w) + "." + f;
}

std::string Lower(std::string s) {
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return s;
}

// The rows `set` reaches: every Leaf row and the effect volume (a device setting, Global),
// each with its ParamId spelling (Params.h).
struct Settable {
  ParamId     id;
  const char* idName;
};
constexpr Settable kSettable[] = {
    {ParamId::DelayMs, "DelayMs"},
    {ParamId::Mix, "Mix"},
    {ParamId::Feedback, "Feedback"},
    {ParamId::WetTrimDb, "WetTrimDb"},
    {ParamId::GrainSizeMs, "GrainSizeMs"},
    {ParamId::Overlap, "Overlap"},
    {ParamId::SprayMs, "SprayMs"},
    {ParamId::TransposeSt, "TransposeSt"},
    {ParamId::SpreadCents, "SpreadCents"},
    {ParamId::ReverseProb, "ReverseProb"},
    {ParamId::Jitter, "Jitter"},
    {ParamId::WindowSustain, "WindowSustain"},
    {ParamId::WindowSkew, "WindowSkew"},
    {ParamId::WindowSmooth, "WindowSmooth"},
    {ParamId::PanSpread, "PanSpread"},
    {ParamId::ModRateHz, "ModRateHz"},
    {ParamId::ModDepth, "ModDepth"},
    {ParamId::DelayTimeMs, "DelayTimeMs"},
    {ParamId::DelayFb, "DelayFb"},
    {ParamId::DelayMix, "DelayMix"},
    {ParamId::ReverbTime, "ReverbTime"},
    {ParamId::ReverbMix, "ReverbMix"},
    {ParamId::FilterCutoffHz, "FilterCutoffHz"},
    {ParamId::FilterRes, "FilterRes"},
    {ParamId::FilterMorph, "FilterMorph"},
    {ParamId::TriggerSens, "TriggerSens"},
    {ParamId::Repeat, "Repeat"},
    {ParamId::DecayMs, "DecayMs"},
    {ParamId::VoiceCount, "VoiceCount"},
    {ParamId::Intermittency, "Intermittency"},
    {ParamId::BurstCount, "BurstCount"},
    {ParamId::BurstSpacingMs, "BurstSpacingMs"},
    {ParamId::EffectVolumeDb, "EffectVolumeDb"},
};
constexpr size_t kNumSettable = sizeof kSettable / sizeof kSettable[0];

// Every Leaf row has a name here, so a wave that adds leaves fails to build until it names
// them; every row here is one SetParam stores.
constexpr bool EveryLeafSettable() {
  for (size_t i = 0; i < kNumLeafParams; ++i) {
    bool found = false;
    for (const Settable& s : kSettable) found = found || s.id == LeafId(i);
    if (!found) return false;
  }
  for (const Settable& s : kSettable) {
    if (!IsLeaf(s.id) && s.id != ParamId::EffectVolumeDb) return false;
  }
  return true;
}
static_assert(EveryLeafSettable(), "one ParamId name per Leaf row (and the effect volume)");

struct Alias {
  const char* name;
  ParamId     id;
};
constexpr Alias kAliases[] = {
    {"delay", ParamId::DelayMs},          {"mix", ParamId::Mix},
    {"feedback", ParamId::Feedback},      {"trim", ParamId::WetTrimDb},
    {"out", ParamId::WetTrimDb},          {"size", ParamId::GrainSizeMs},
    {"density", ParamId::Overlap},        {"overlap", ParamId::Overlap},
    {"spray", ParamId::SprayMs},          {"pitch", ParamId::TransposeSt},
    {"transpose", ParamId::TransposeSt},  {"spread", ParamId::SpreadCents},
    {"reverse", ParamId::ReverseProb},    {"jitter", ParamId::Jitter},
    {"sustain", ParamId::WindowSustain},  {"skew", ParamId::WindowSkew},
    {"smooth", ParamId::WindowSmooth},    {"pan", ParamId::PanSpread},
    {"modrate", ParamId::ModRateHz},      {"moddepth", ParamId::ModDepth},
    {"delaytime", ParamId::DelayTimeMs},  {"delayfb", ParamId::DelayFb},
    {"delaymix", ParamId::DelayMix},      {"reverbtime", ParamId::ReverbTime},
    {"reverbmix", ParamId::ReverbMix},    {"cutoff", ParamId::FilterCutoffHz},
    {"res", ParamId::FilterRes},          {"morph", ParamId::FilterMorph},
    {"sens", ParamId::TriggerSens},       {"repeat", ParamId::Repeat},
    {"decay", ParamId::DecayMs},          {"voices", ParamId::VoiceCount},
    {"skip", ParamId::Intermittency},
    {"burst", ParamId::BurstCount},       {"spacing", ParamId::BurstSpacingMs},
    {"volume", ParamId::EffectVolumeDb},
};

const Settable* FindByName(const std::string& name) {
  const std::string n = Lower(name);
  for (const Settable& s : kSettable) {
    if (n == Lower(FindParam(s.id)->name) || n == Lower(s.idName)) return &s;
  }
  for (const Alias& a : kAliases) {
    if (n != a.name) continue;
    for (const Settable& s : kSettable) {
      if (s.id == a.id) return &s;
    }
  }
  return nullptr;
}

// A macro by its knob name (activity) or its row name (macro.activity).
const ParamDescriptor* FindMacro(const std::string& name) {
  const std::string n = Lower(name);
  size_t            count = 0;
  const ParamDescriptor* rows = Descriptors(&count);
  for (size_t i = 0; i < count; ++i) {
    const ParamDescriptor& d = rows[i];
    if (d.kind != ParamKind::Macro) continue;
    if (n == d.name || "macro." + n == d.name) return &d;
  }
  return nullptr;
}

std::string ParamJson(const Settable& s, bool withRange) {
  const ParamDescriptor& d = *FindParam(s.id);
  JsonObj                o;
  o.Str("name", d.name).Str("id", s.idName);
  o.Raw("value", FloatText(g_engine->GetParam(d.id)));
  if (withRange) {
    o.Raw("min", FloatText(d.min)).Raw("max", FloatText(d.max)).Raw("default", FloatText(d.def));
    o.Str("unit", d.unit);
  }
  return o.Done();
}

// The producer's copy of what the engine plays (g_now), as each event changes it.
void Mirror(const PresetLeaf* leaves, size_t count) {
  for (size_t k = 0; k < count; ++k) {
    for (uint32_t i = 0; i < g_now.leafCount; ++i) {
      if (g_now.leaves[i].id == leaves[k].id) {
        g_now.leaves[i].value = Canonicalize(static_cast<ParamId>(leaves[k].id), leaves[k].value);
      }
    }
  }
}

bool g_frozen = false;  // what the producer last asked for (every load turns it off)

std::string Hello() {
  return fw::HelloJson("live", "\"target\":" + JsonString(BRAINSCAPE_FW_IMAGE) +
                                   ",\"engineCode\":" + JsonString(BRAINSCAPE_FW_ENGINE_CODE) +
                                   ",\"maxBlockSize\":" + JsonUInt(kBlock) +
                                   ",\"preset\":" + golden::JsonInt(g_current) +
                                   ",\"onset\":" + (live::PlaysOnset(g_now.mode) ? "true" : "false") +
                                   ",\"marks\":" + (live::PlaysMarks(g_now.mode) ? "true" : "false"));
}

void Stats(bool reset) {
  // One consistent snapshot: the audio interrupt updates these together.
  __disable_irq();
  const uint32_t count = g_statCount.load(std::memory_order_relaxed);
  const uint64_t sum   = g_statSum;
  const uint32_t peak  = g_statPeak.load(std::memory_order_relaxed);
  __enable_irq();
  const uint64_t budget = static_cast<uint64_t>(fw::SysClkHz()) / 48000u * kBlock;  // cycles/block
  const uint64_t mean   = count != 0 ? sum / count : 0;
  Reply(JsonObj()
            .Str("type", "stats")
            .UInt("callbacks", count)
            .UInt("meanCycles", mean)
            .UInt("peakCycles", peak)
            .UInt("budgetCycles", budget)
            .UInt("meanPermille", budget != 0 ? mean * 1000u / budget : 0)
            .UInt("peakPermille", budget != 0 ? static_cast<uint64_t>(peak) * 1000u / budget : 0)
            .UInt("overBudget", g_statOver.load(std::memory_order_relaxed))
            .UInt("onsets", g_onsets.load(std::memory_order_relaxed))
            .UInt("modeSwitches", g_engine->ModeSwitches())
            .UInt("queueRefused", g_queue.ConsumeRefused())
            .UInt("droppedSerialBytes", Serial().DroppedBytes())
            .Int("preset", g_current)
            .Bool("onset", live::PlaysOnset(g_now.mode))
            .Bool("marks", live::PlaysMarks(g_now.mode))
            .Bool("frozen", g_frozen)
            .Done());
  if (reset) g_statReset.store(true, std::memory_order_relaxed);
}

// A Spillover load of `state` as an event (its payload must stay valid until the event
// retires: one of two staging slots); the event's id is the switch style.
bool StageSpillover(const PresetState& state, SwitchStyle style) {
  uint32_t slot = g_nextStage;
  if (g_stagedUsed[slot] && !g_queue.Retired(g_stagedEvent[slot])) {
    slot ^= 1u;
    if (g_stagedUsed[slot] && !g_queue.Retired(g_stagedEvent[slot])) {
      Error("both staging slots are in flight; try again");
      return false;
    }
  }
  g_staged[slot] = state;
  uint32_t index = 0;
  if (!PushEvent(Engine::EventType::SpilloverLoad, static_cast<uint32_t>(style), 0.f, &g_staged[slot],
                 &index)) {
    return false;
  }
  g_stagedUsed[slot]  = true;
  g_stagedEvent[slot] = index;
  g_nextStage         = slot ^ 1u;
  g_now               = state;
  g_frozen            = false;
  return true;
}

void LoadPresetCommand(int32_t n, const std::string& how) {
  if (n < 0 || static_cast<uint32_t>(n) >= live::kNumSlots) {
    Error("no such preset");
    return;
  }
  if (how == "exact") {
    Mute();
    const uint32_t t0 = fw::Cycles();
    const bool     ok = g_engine->LoadPreset(g_presets[n], LoadMode::Exact);
    const uint32_t cycles = fw::Cycles() - t0;
    g_queue.Clear();  // a new engine timeline: frame 0
    g_frame = 0;
    g_blocks.store(0, std::memory_order_release);
    g_seq = 0;
    g_stagedUsed[0] = g_stagedUsed[1] = false;
    Unmute();
    g_current = n;
    g_now     = g_presets[n];
    g_frozen  = false;
    Reply(JsonObj()
              .Str("type", "preset")
              .Int("n", n)
              .Str("name", live::kSlots[n].label)
              .Str("load", "exact")
              .Bool("ok", ok)
              .UInt("cycles", cycles)
              .UInt("microseconds", cycles / (fw::SysClkHz() / 1000000u))
              .Done());
    return;
  }
  if (!how.empty() && how != "cut") {
    Error("preset N [exact|cut]");
    return;
  }
  const SwitchStyle style = how == "cut" ? SwitchStyle::FastCut : SwitchStyle::Trails;
  if (!StageSpillover(g_presets[n], style)) return;
  g_current = n;
  Reply(JsonObj()
            .Str("type", "preset")
            .Int("n", n)
            .Str("name", live::kSlots[n].label)
            .Str("load", style == SwitchStyle::FastCut ? "spillover, fast cut" : "spillover")
            .Bool("ok", true)
            .Done());
}

// `onset` / `marks`: the current leaves with the structure that has the onset source and mark
// positioning asked for (live/LivePresets.h), as a Spillover load.
void StructureCommand(bool onset, bool marks) {
  const uint32_t   i = live::StructureIndex(onset, marks);
  const char*      package = live::kStructures[i].package;
  static PresetState next;  // 2.6 KiB: not on the main loop's stack
  next = g_now;
  live::WithStructure(&next, g_structures[i]);
  if (!StageSpillover(next, SwitchStyle::Trails)) return;
  Reply(JsonObj()
            .Str("type", "mode")
            .Bool("onset", onset)
            .Bool("marks", marks)
            .Str("from", package != nullptr ? std::string("package ") + package : "the default mode")
            .Str("load", "spillover")
            .Done());
}

bool OnOff(const std::string& w, bool* on) {
  if (w != "on" && w != "off") return false;
  *on = w == "on";
  return true;
}

bool ParseFloat(const std::string& text, float* value) {
  char* end = nullptr;
  *value    = std::strtof(text.c_str(), &end);
  if (end == text.c_str() || *end != '\0') {
    Error("not a number: " + text);
    return false;
  }
  return true;
}

std::vector<std::string> Words(const std::string& line) {
  std::vector<std::string> out;
  std::string              cur;
  for (const char c : line) {
    if (c == ' ' || c == '\t') {
      if (!cur.empty()) out.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

void Command(const std::vector<std::string>& w) {
  const std::string& c = w[0];
  bool               on = false;
  float              value = 0.f;
  if (c == "info") {
    Reply(Hello());
  } else if (c == "list") {
    std::string list = "[";
    for (uint32_t s = 0; s < live::kNumSlots; ++s) {
      list += (s ? "," : "") + JsonObj()
                                   .UInt("n", s)
                                   .Str("name", live::kSlots[s].label)
                                   .Str("source", std::string(live::kSlots[s].vector) + "/" +
                                                      live::kSlots[s].preset)
                                   .Bool("onset", live::PlaysOnset(g_presets[s].mode))
                                   .Bool("marks", live::PlaysMarks(g_presets[s].mode))
                                   .Done();
    }
    Reply(JsonObj().Str("type", "presets").Raw("presets", list + "]").Int("current", g_current).Done());
  } else if (c == "params" || c == "get") {
    for (size_t i = 0; i < kNumSettable; ++i) {
      Reply("{\"type\":\"param\",\"param\":" + ParamJson(kSettable[i], c == "params") + "}");
    }
  } else if (c == "preset" && (w.size() == 2 || w.size() == 3)) {
    LoadPresetCommand(std::atoi(w[1].c_str()), w.size() == 3 ? w[2] : std::string());
  } else if ((c == "onset" || c == "marks") && w.size() == 2 && OnOff(w[1], &on)) {
    StructureCommand(c == "onset" ? on : live::PlaysOnset(g_now.mode),
                     c == "marks" ? on : live::PlaysMarks(g_now.mode));
  } else if (c == "set" && w.size() == 3 && (Lower(w[1]) == "onset" || Lower(w[1]) == "marks")) {
    // Rows 27 and 28's old spelling: >= 0.5 on, as sessions migrate (mode-compiler.md §4.4).
    if (!ParseFloat(w[2], &value)) return;
    const bool want = value >= 0.5f;
    StructureCommand(Lower(w[1]) == "onset" ? want : live::PlaysOnset(g_now.mode),
                     Lower(w[1]) == "marks" ? want : live::PlaysMarks(g_now.mode));
  } else if (c == "set" && w.size() == 3) {
    const Settable* s = FindByName(w[1]);
    if (s == nullptr) {
      Error("unknown parameter: " + w[1] + " (try: params)");
      return;
    }
    if (!ParseFloat(w[2], &value)) return;
    const float canonical = Canonicalize(s->id, value);
    if (PushEvent(Engine::EventType::SetParam, static_cast<uint32_t>(s->id), canonical, nullptr)) {
      const PresetLeaf leaf{static_cast<uint32_t>(s->id), canonical};
      Mirror(&leaf, 1);
      uint32_t bits;
      std::memcpy(&bits, &canonical, sizeof bits);
      Reply(JsonObj()
                .Str("type", "set")
                .Str("name", FindParam(s->id)->name)
                .Raw("value", FloatText(canonical))
                .Hex("bits", bits)
                .Done());
    }
  } else if (c == "macro" && w.size() == 3) {
    const ParamDescriptor* m = FindMacro(w[1]);
    if (m == nullptr) {
      Error("unknown macro: " + w[1] + " (activity repeats shape time space filter aux1 aux2)");
      return;
    }
    if (!ParseFloat(w[2], &value)) return;
    const float position = Canonicalize(m->id, value);
    if (PushEvent(Engine::EventType::MacroMove, static_cast<uint32_t>(m->id), position, nullptr)) {
      // The leaves the engine sets at the event, from the mode it plays (ModeEval.h).
      PresetLeaf   out[kMaxMacroTargets];
      const size_t n = EvalMacro(g_now.mode, m->id, position, out, kMaxMacroTargets);
      Mirror(out, n);
      Reply(JsonObj()
                .Str("type", "macro")
                .Str("name", m->name)
                .Raw("position", FloatText(position))
                .UInt("leaves", n)
                .Done());
    }
  } else if (c == "expression" && w.size() == 2) {
    if (!ParseFloat(w[1], &value)) return;
    const float position = Canonicalize(ParamId::PerfExpression, value);
    if (PushEvent(Engine::EventType::Expression, 0, position, nullptr)) {
      PresetLeaf   out[kMaxExpressions * kMaxMacroTargets];
      const size_t n = EvalExpression(g_now.mode, g_now.control, position, out,
                                      kMaxExpressions * kMaxMacroTargets);
      Mirror(out, n);
      Reply(JsonObj()
                .Str("type", "expression")
                .Raw("position", FloatText(position))
                .UInt("leaves", n)
                .Done());
    }
  } else if (c == "freeze" && w.size() == 2 && OnOff(w[1], &on)) {
    if (PushEvent(Engine::EventType::Freeze, 0, on ? 1.f : 0.f, nullptr)) {
      g_frozen = on;
      Reply(JsonObj().Str("type", "freeze").Bool("on", on).Done());
    }
  } else if (c == "trigger") {
    if (PushEvent(Engine::EventType::Trigger,
                  static_cast<uint32_t>(Engine::TriggerSource::Footswitch), 1.f, nullptr)) {
      Reply(JsonObj().Str("type", "trigger").Done());
    }
  } else if (c == "stats") {
    Stats(w.size() >= 2 && w[1] == "reset");
  } else if (c == "dfu") {
    Mute();
    fw::RebootToBootloader();
  } else {
    Error("unknown command: " + c);
  }
}

}  // namespace

int main() {
  fw::BoardInit("live");
  const fw::EnginePlacement placement = fw::Placement();
  EngineConfig              ec;
  ec.maxBlockSize = kBlock;  // the pedal's engine; ring 2^22, 48 kHz, stereo input
  const char* why = nullptr;
  if (!fw::CheckPlacement(ec, placement, &why)) fw::Fatal(why);
  if (!live::BuildSlots(g_presets, &why)) fw::Fatal(why);
  if (!live::BuildStructures(g_structures, &why)) fw::Fatal(why);

  g_engine = new (placement.engine) Engine();
  if (!g_engine->Init(ec, placement.arenas)) fw::Fatal("Engine::Init refused the placement");
  if (!g_engine->LoadPreset(g_presets[0], LoadMode::Exact)) fw::Fatal("preset 0 did not load exactly");
  g_now = g_presets[0];

  daisy::DaisySeed& seed = fw::Seed();
  seed.SetAudioSampleRate(daisy::SaiHandle::Config::SampleRate::SAI_48KHZ);
  seed.SetAudioBlockSize(kBlock);
  seed.StartAudio(AudioCallback);
  fw::SetLedMode(fw::LedMode::Pulse);

  Reply(Hello());
  std::string line;
  for (;;) {
    Serial().Pump();
    if (!Serial().ReadLine(&line)) continue;
    const std::vector<std::string> w = Words(line);
    if (!w.empty()) Command(w);
  }
}
