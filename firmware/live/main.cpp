// Image C, LIVE AUDIO (firmware/README.md): the engine in libDaisy's audio callback at 48 kHz
// in 48-frame blocks on the Seed's own codec (Rev7: PCM3060, line level, pins 16/17 in and
// 18/19 out), the pedal's configuration (maxBlockSize 48, the 2^22 ring), with a few presets
// from the golden corpus and a text protocol over USB serial (tools/hil/console.py). Every
// change reaches the engine as a frame-stamped event through its EventQueue, stamped at the
// next block boundary (determinism-profile.md §5.11), as the pedal's control loop will.
//
// Commands, one per line:
//   info | list | params | get
//   preset N [exact]      load preset N: a Spillover event (trails kept, real-time), or with
//                         "exact" an Exact load (Restart: the output is muted while it runs)
//   set NAME VALUE        a parameter event; NAME is the descriptor name (layer0.size_ms),
//                         its ParamId (GrainSizeMs) or a short alias (size)
//   freeze on|off | trigger
//   stats [reset]         CPU load: DWT cycles of Engine::Process per callback, mean and peak
//   dfu                   reboot into the bootloader
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#include "Corpus.h"
#include "EventScript.h"
#include "JsonLine.h"
#include "brainscape/Engine.h"
#include "brainscape/EventQueue.h"
#include "brainscape/Params.h"
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

namespace {

constexpr uint32_t kBlock = 48;

// Musically useful corpus presets (dsp/tests/golden/Corpus.cpp), parameters only: the
// corpus scripts drive the renders, not the live engine.
struct LiveSlot {
  const char* label;
  const char* vector;
  const char* preset;
};
constexpr LiveSlot kSlots[] = {
    {"default", "plucks_12s", "default"},
    {"clean delay", "plucks_12s", "clean_delay"},
    {"strum marks", "plucks_12s", "strum_marks"},
    {"pitch, reverse, spray", "plucks_12s", "pitch_reverse_spray"},
    {"freeze marks (all wet)", "strums_16s", "freeze_marks"},
    {"ambient tail (feedback, delay, reverb, filter)", "strums_tail_123s", "tail_post_fb"},
    {"octave shimmer (onset grains)", "strums_freeze_70s", "freeze_long"},
    {"glitch (1 ms grains)", "onset_bursts_6s", "dense_1ms"},
};
constexpr uint32_t kNumSlots = sizeof kSlots / sizeof kSlots[0];

PresetState g_presets[kNumSlots];  // complete presets, built from the corpus at boot
PresetState g_staged[2];           // Spillover payloads, valid until their event retires
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

struct Alias {
  const char* name;
  ParamId     id;
};
constexpr Alias kAliases[] = {
    {"delay", ParamId::DelayMs},          {"mix", ParamId::Mix},
    {"feedback", ParamId::Feedback},      {"out", ParamId::OutTrimDb},
    {"size", ParamId::GrainSizeMs},       {"density", ParamId::Overlap},
    {"overlap", ParamId::Overlap},        {"spray", ParamId::SprayMs},
    {"pitch", ParamId::PitchSt},          {"spread", ParamId::SpreadCents},
    {"reverse", ParamId::ReverseProb},    {"jitter", ParamId::Jitter},
    {"sustain", ParamId::WindowSustain},  {"skew", ParamId::WindowSkew},
    {"smooth", ParamId::WindowSmooth},    {"pan", ParamId::PanSpread},
    {"modrate", ParamId::ModRateHz},      {"moddepth", ParamId::ModDepth},
    {"delaytime", ParamId::DelayTimeMs},  {"delayfb", ParamId::DelayFb},
    {"delaymix", ParamId::DelayMix},      {"reverbtime", ParamId::ReverbTime},
    {"reverbmix", ParamId::ReverbMix},    {"cutoff", ParamId::FilterCutoffHz},
    {"res", ParamId::FilterRes},          {"morph", ParamId::FilterMorph},
    {"sens", ParamId::TriggerSens},       {"onset", ParamId::OnsetTrigger},
    {"marks", ParamId::PositionSource},
};
// ParamId identifiers, in id order (Params.h).
constexpr const char* kIdNames[] = {
    "DelayMs",       "Mix",         "Feedback",      "OutTrimDb",     "GrainSizeMs",
    "Overlap",       "SprayMs",     "PitchSt",       "SpreadCents",   "ReverseProb",
    "Jitter",        "WindowSustain", "WindowSkew",  "WindowSmooth",  "PanSpread",
    "ModRateHz",     "ModDepth",    "DelayTimeMs",   "DelayFb",       "DelayMix",
    "ReverbTime",    "ReverbMix",   "FilterCutoffHz", "FilterRes",    "FilterMorph",
    "TriggerSens",   "OnsetTrigger", "PositionSource"};
static_assert(sizeof kIdNames / sizeof kIdNames[0] == kNumParams, "one name per parameter");

const ParamDescriptor* FindByName(const std::string& name) {
  const std::string n = Lower(name);
  for (size_t i = 0; i < kNumParams; ++i) {
    if (n == Lower(kParamTable[i].name) || n == Lower(kIdNames[i])) return &kParamTable[i];
  }
  for (const Alias& a : kAliases) {
    if (n == a.name) return FindParam(a.id);
  }
  return nullptr;
}

std::string ParamJson(const ParamDescriptor& d, bool withRange) {
  JsonObj o;
  o.Str("name", d.name).Str("id", kIdNames[static_cast<uint32_t>(d.id) - 1u]);
  o.Raw("value", FloatText(g_engine->GetParam(d.id)));
  if (withRange) {
    o.Raw("min", FloatText(d.min)).Raw("max", FloatText(d.max)).Raw("default", FloatText(d.def));
    o.Str("unit", d.unit);
  }
  return o.Done();
}

bool BuildPresets() {
  const std::vector<golden::VectorCase> corpus = golden::BuildCorpus();
  for (uint32_t s = 0; s < kNumSlots; ++s) {
    const golden::PresetCase* found = nullptr;
    for (const golden::VectorCase& v : corpus) {
      if (std::strcmp(v.name, kSlots[s].vector) != 0) continue;
      for (const golden::PresetCase& p : v.presets) {
        if (std::strcmp(p.name, kSlots[s].preset) == 0) found = &p;
      }
    }
    if (found == nullptr) return false;
    g_presets[s] = *golden::CompletePreset(found->params);
    if (!CheckPreset(g_presets[s])) return false;
  }
  return true;
}

bool g_frozen = false;  // what the producer last asked for (every load turns it off)

std::string Hello() {
  return fw::HelloJson("live", "\"target\":" + JsonString(BRAINSCAPE_FW_IMAGE) +
                                   ",\"engineCode\":" + JsonString(BRAINSCAPE_FW_ENGINE_CODE) +
                                   ",\"maxBlockSize\":" + JsonUInt(kBlock) +
                                   ",\"preset\":" + golden::JsonInt(g_current));
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
            .UInt("queueRefused", g_queue.ConsumeRefused())
            .UInt("droppedSerialBytes", Serial().DroppedBytes())
            .Int("preset", g_current)
            .Bool("frozen", g_frozen)
            .Done());
  if (reset) g_statReset.store(true, std::memory_order_relaxed);
}

void LoadPresetCommand(int32_t n, bool exact) {
  if (n < 0 || static_cast<uint32_t>(n) >= kNumSlots) {
    Error("no such preset");
    return;
  }
  if (exact) {
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
    g_frozen  = false;
    Reply(JsonObj()
              .Str("type", "preset")
              .Int("n", n)
              .Str("name", kSlots[n].label)
              .Str("load", "exact")
              .Bool("ok", ok)
              .UInt("cycles", cycles)
              .UInt("microseconds", cycles / (fw::SysClkHz() / 1000000u))
              .Done());
    return;
  }
  // A Spillover load as an event: its payload must stay valid until the event retires.
  uint32_t slot = g_nextStage;
  if (g_stagedUsed[slot] && !g_queue.Retired(g_stagedEvent[slot])) {
    slot ^= 1u;
    if (g_stagedUsed[slot] && !g_queue.Retired(g_stagedEvent[slot])) {
      Error("both staging slots are in flight; try again");
      return;
    }
  }
  g_staged[slot] = g_presets[n];
  uint32_t index = 0;
  if (!PushEvent(Engine::EventType::SpilloverLoad, 0, 0.f, &g_staged[slot], &index)) return;
  g_stagedUsed[slot]  = true;
  g_stagedEvent[slot] = index;
  g_nextStage         = slot ^ 1u;
  g_current           = n;
  g_frozen            = false;
  Reply(JsonObj()
            .Str("type", "preset")
            .Int("n", n)
            .Str("name", kSlots[n].label)
            .Str("load", "spillover")
            .Bool("ok", true)
            .Done());
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
  if (c == "info") {
    Reply(Hello());
  } else if (c == "list") {
    std::string list = "[";
    for (uint32_t s = 0; s < kNumSlots; ++s) {
      list += (s ? "," : "") + JsonObj()
                                   .UInt("n", s)
                                   .Str("name", kSlots[s].label)
                                   .Str("source", std::string(kSlots[s].vector) + "/" + kSlots[s].preset)
                                   .Done();
    }
    Reply(JsonObj().Str("type", "presets").Raw("presets", list + "]").Int("current", g_current).Done());
  } else if (c == "params" || c == "get") {
    for (size_t i = 0; i < kNumParams; ++i) {
      Reply("{\"type\":\"param\",\"param\":" + ParamJson(kParamTable[i], c == "params") + "}");
    }
  } else if (c == "preset" && w.size() >= 2) {
    LoadPresetCommand(std::atoi(w[1].c_str()), w.size() >= 3 && w[2] == "exact");
  } else if (c == "set" && w.size() == 3) {
    const ParamDescriptor* d = FindByName(w[1]);
    if (d == nullptr) {
      Error("unknown parameter: " + w[1] + " (try: params)");
      return;
    }
    char*       end   = nullptr;
    const float value = std::strtof(w[2].c_str(), &end);
    if (end == w[2].c_str() || *end != '\0') {
      Error("not a number: " + w[2]);
      return;
    }
    const float canonical = Canonicalize(d->id, value);
    if (PushEvent(Engine::EventType::SetParam, static_cast<uint32_t>(d->id), canonical, nullptr)) {
      uint32_t bits;
      std::memcpy(&bits, &canonical, sizeof bits);
      Reply(JsonObj()
                .Str("type", "set")
                .Str("name", d->name)
                .Raw("value", FloatText(canonical))
                .Hex("bits", bits)
                .Done());
    }
  } else if (c == "freeze" && w.size() == 2 && (w[1] == "on" || w[1] == "off")) {
    if (PushEvent(Engine::EventType::Freeze, 0, w[1] == "on" ? 1.f : 0.f, nullptr)) {
      g_frozen = w[1] == "on";
      Reply(JsonObj().Str("type", "freeze").Bool("on", w[1] == "on").Done());
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
  if (!BuildPresets()) fw::Fatal("a live preset is missing from the golden corpus");

  g_engine = new (placement.engine) Engine();
  if (!g_engine->Init(ec, placement.arenas)) fw::Fatal("Engine::Init refused the placement");
  if (!g_engine->LoadPreset(g_presets[0], LoadMode::Exact)) fw::Fatal("preset 0 did not load exactly");

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
