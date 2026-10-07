#include "Recipe.h"

#include <cstdio>
#include <cstring>

#include "JsonWriter.h"
#include "brainscape/Params.h"
#include "brainscape/SoundRevision.h"
#include "brainscape/TestSignal.h"

namespace bsa {

using brainscape::Engine;

namespace {

constexpr size_t kInlineEvents = 64;  // longer scripts are named by their hash

void Identity(JsonWriter& w, const PresetIdentity& p) {
  if (!p.package) {
    w.Null();
    return;
  }
  w.BeginObject();
  w.Key("id").String(p.id);
  w.Key("name").String(p.name);
  w.Key("family").String(p.family);
  w.Key("source").String(p.source);
  w.Key("soundRev").Uint(p.soundRev);
  w.Key("soundHash").String(p.soundHash);
  w.Key("controlHash").String(p.controlHash);
  w.Key("packageHash").String(p.packageHash);
  w.EndObject();
}

const char* ParamName(uint32_t id) {
  const brainscape::ParamDescriptor* d = brainscape::FindParam(static_cast<brainscape::ParamId>(id));
  return d != nullptr && d->name != nullptr ? d->name : "";
}

void Put32(std::vector<uint8_t>& b, uint32_t v) {
  for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

}  // namespace

const char* EventTypeName(Engine::EventType t) {
  switch (t) {
    case Engine::EventType::SetParam: return "SetParam";
    case Engine::EventType::Freeze: return "Freeze";
    case Engine::EventType::Trigger: return "Trigger";
    case Engine::EventType::SpilloverLoad: return "SpilloverLoad";
    case Engine::EventType::MacroMove: return "MacroMove";
    case Engine::EventType::Expression: return "Expression";
  }
  return "unknown";
}

std::string EventsSha256(const std::vector<ScriptEvent>& events) {
  std::vector<uint8_t> b;
  b.reserve(events.size() * 24);
  for (const ScriptEvent& e : events) {
    const auto frame = static_cast<uint64_t>(e.frame);
    Put32(b, static_cast<uint32_t>(frame));
    Put32(b, static_cast<uint32_t>(frame >> 32));
    Put32(b, static_cast<uint32_t>(e.type));
    Put32(b, e.id);
    uint32_t bits;
    std::memcpy(&bits, &e.value, 4);
    Put32(b, bits);
    Put32(b, e.staged);
  }
  return Sha256Hex(b.data(), b.size());
}

std::string RecipeJson(const RecipeInput& in) {
  JsonWriter w;
  w.BeginObject();
  w.Key("format").String(kRecipeFormat);
  if (!in.script.empty()) w.Key("script").String(in.script);
  if (!in.name.empty()) w.Key("render").String(in.name);
  w.Key("soundRevision").Uint(brainscape::kSoundRevision);
  const brainscape::ToolchainId& tc = brainscape::BuildToolchain();
  w.Key("engineToolchain").BeginObject();
  w.Key("compiler").String(tc.compiler);
  w.Key("version").String(tc.version);
  w.Key("target").String(tc.target);
  w.Key("fpFlagsHash").String(tc.fpFlagsHash);
  w.EndObject();
  w.Key("sampleRate").Uint(kRate);
  w.Key("frames").Uint(in.result != nullptr ? in.result->out.Frames() : 0);
  std::string pattern = "[";
  for (size_t i = 0; i < in.blockPattern.size(); ++i) {
    pattern += (i == 0 ? "" : ", ") + std::to_string(in.blockPattern[i]);
  }
  w.Key("blockPattern").Raw(pattern + "]");
  w.Key("start").String("Init, then LoadPreset(Exact)");
  // The device settings every render starts from (mode-compiler.md §3.8, §9.2).
  w.Key("deviceSettings").BeginArray();
  for (const brainscape::ParamDescriptor& d : brainscape::kParamTable) {
    if (d.kind != brainscape::ParamKind::Global) continue;
    w.BeginObject();
    w.Key("id").Uint(static_cast<uint32_t>(d.id));
    w.Key("name").String(d.name);
    w.Key("value").Float(d.def);
    w.EndObject();
  }
  w.EndArray();

  w.Key("package");
  Identity(w, in.identity);
  if (!in.variant.empty()) w.Key("variant").String(in.variant);
  // The leaves as loaded, every stored one, with their exact bits.
  w.Key("preset").BeginArray();
  if (in.preset != nullptr) {
    for (uint32_t i = 0; i < in.preset->leafCount && i < brainscape::PresetState::kMaxLeaves; ++i) {
      const brainscape::PresetLeaf& leaf = in.preset->leaves[i];
      w.BeginObject();
      w.Key("id").Uint(leaf.id);
      w.Key("name").String(ParamName(leaf.id));
      w.Key("value").Float(leaf.value);
      w.Key("bits").String(BitsHex(leaf.value));
      w.EndObject();
    }
  }
  w.EndArray();
  w.Key("positions").BeginArray();
  if (in.preset != nullptr && in.preset->control.present != 0) {
    const brainscape::ControlState& c = in.preset->control;
    for (uint32_t i = 0; i < c.macroCount && i < brainscape::kMaxMacros; ++i) {
      w.BeginObject();
      w.Key("macro").String(ParamName(c.positions[i].macroId));
      w.Key("position").Float(c.positions[i].position);
      w.EndObject();
    }
  }
  w.EndArray();
  if (in.result != nullptr) {
    const brainscape::LoadReport& r = in.result->load;
    w.Key("load").BeginObject();
    w.Key("exact").Bool(r.exact);
    w.Key("unknownIds").Uint(r.unknownIds);
    w.Key("missingIds").Uint(r.missingIds);
    w.Key("changedValues").Uint(r.changedValues);
    w.Key("unsupported").Uint(r.unsupported);
    w.EndObject();
  }

  w.Key("input").BeginObject();
  w.Key("source").String(in.inputDescription);
  if (!in.inputVector.empty()) {
    w.Key("vector").String(in.inputVector);
    w.Key("generatorVersion").Uint(brainscape::testsignal::kVersion);
  }
  w.Key("signalFrames").Uint(in.inputSignalFrames);
  w.Key("sourceRate").Double(in.inputSourceRate, 1);
  w.Key("conversion").String(in.inputConverted ? "linear interpolation to 48 kHz" : "none");
  w.Key("conditioning").String("ConditionInput24");
  w.Key("mode").String(in.mode == InputMode::Mono ? "mono (R = L)" : "stereo");
  w.Key("sha256").String(in.conditionedInputSha256);
  w.EndObject();

  const std::vector<ScriptEvent> none;
  const std::vector<ScriptEvent>& events = in.events != nullptr ? *in.events : none;
  w.Key("events").BeginObject();
  w.Key("count").Uint(events.size());
  if (!in.eventsDescription.empty()) w.Key("description").String(in.eventsDescription);
  w.Key("sha256").String(EventsSha256(events));
  if (events.size() <= kInlineEvents) {
    w.Key("list").BeginArray();
    for (const ScriptEvent& e : events) {
      w.BeginObject();
      w.Key("frame").Int(e.frame);
      w.Key("type").String(EventTypeName(e.type));
      w.Key("id").Uint(e.id);
      w.Key("value").Float(e.value);
      if (e.type == Engine::EventType::SpilloverLoad) w.Key("staged").Uint(e.staged);
      w.EndObject();
    }
    w.EndArray();
  }
  w.EndObject();
  if (!in.staged.empty()) {
    w.Key("staged").BeginArray();
    for (const PresetIdentity& s : in.staged) Identity(w, s);
    w.EndArray();
  }

  w.Key("outputSha256").String(in.hashes.whole);
  w.Key("outputSegmentSha256").BeginArray();
  for (const std::string& h : in.hashes.seconds) w.String(h);
  w.EndArray();
  if (in.result != nullptr) w.Key("onsets").Uint(in.result->onsets);
  w.Key("wav");
  if (in.wavPath.empty()) {
    w.Null();
  } else {
    w.BeginObject();
    w.Key("path").String(in.wavPath);
    w.Key("bits").Uint(in.wavBits);
    w.Key("sha256").String(in.wavSha256);
    w.EndObject();
  }
  if (in.metrics != nullptr) {
    const Metrics& m = *in.metrics;
    w.Key("metrics").BeginObject();
    w.Key("span").Uint(m.span);
    w.Key("peakDbfs").Double(m.peakDbfs, 2);
    w.Key("overFullScale").Uint(m.overFull);
    w.Key("nonFinite").Uint(m.nonFinite);
    w.Key("subnormal").Uint(m.subnormal);
    w.Key("loudnessLufs").Double(m.loudness, 2);
    w.Key("tailSeconds").Double(m.tailSeconds, 3);
    w.Key("tailFinite").Bool(m.tailFinite);
    w.Key("maxStep").Double(m.maxStep, 6);
    w.Key("maxStepFrame").Uint(m.maxStepFrame);
    w.Key("brightnessHz").Double(m.features.brightnessHz, 1);
    w.Key("envelopeVarDb").Double(m.features.envelopeVarDb, 2);
    std::string st = "[";
    for (size_t i = 0; i < m.shortTerm.size(); ++i) {
      char buf[32];
      std::snprintf(buf, sizeof buf, "%s%.1f", i == 0 ? "" : ", ", m.shortTerm[i]);
      st += buf;
    }
    w.Key("shortTermLufs").Raw(st + "]");
    w.EndObject();
  }
  w.Key("identical").String(in.inputConverted
                                ? "on this machine only: the input was converted by platform code"
                                : "on every conforming build of this sound revision");
  w.EndObject();
  return w.Text();
}

}  // namespace bsa
