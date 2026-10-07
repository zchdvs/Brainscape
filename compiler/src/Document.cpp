#include "Document.h"

#include <cstring>

#include "Number.h"
#include "Text.h"

namespace bsc {

using namespace brainscape;

// ── Findings ─────────────────────────────────────────────────────────────────────────────

std::string Describe(const Finding& f, std::string_view file) {
  std::string out(file);
  if (f.at.line != 0) out += ":" + Dec(f.at.line) + ":" + Dec(f.at.column);
  out += f.error ? ": error " : ": warning ";
  out += f.code;
  if (!f.at.pointer.empty()) out += " at " + f.at.pointer;
  out += ": " + f.message;
  return out;
}

bool HasErrors(const std::vector<Finding>& findings) {
  for (const Finding& f : findings) {
    if (f.error) return true;
  }
  return false;
}

// ── Floats as bits ───────────────────────────────────────────────────────────────────────

uint32_t BitsOf(const float& f) {
  uint32_t u;
  std::memcpy(&u, &f, sizeof u);
  return u;
}

float FloatOf(uint32_t bits) {
  float f;
  std::memcpy(&f, &bits, sizeof f);
  return f;
}

uint32_t OrderKey(uint32_t b) { return (b & 0x80000000u) != 0u ? ~b : (b | 0x80000000u); }
bool     LessBits(uint32_t a, uint32_t b) { return OrderKey(a) < OrderKey(b); }

std::string NumberText(uint32_t bits) {
  char         buf[kMaxNumberText];
  const size_t n = WriteNumberBits(bits, buf);
  return std::string(buf, n);
}

// ── The document ─────────────────────────────────────────────────────────────────────────

Document::Document() : state(std::make_unique<PresetState>()) {}

Document::Document(const Document& o)
    : schemaVersion(o.schemaVersion),
      id(o.id),
      name(o.name),
      family(o.family),
      stamped(o.stamped),
      soundRev(o.soundRev),
      soundHash(o.soundHash),
      author(o.author),
      description(o.description),
      tags(o.tags),
      state(std::make_unique<PresetState>(*o.state)),
      hasRatioGen(o.hasRatioGen),
      ratioGen(o.ratioGen),
      detached(o.detached),
      where(o.where),
      notes(o.notes),
      omittedPositions(o.omittedPositions) {
  for (uint32_t k = 0; k < kMaxMacros; ++k) displayName[k] = o.displayName[k];
}

Document& Document::operator=(const Document& o) {
  if (this != &o) {
    Document copy(o);
    *this = std::move(copy);
  }
  return *this;
}

uint32_t Document::LeafBits(uint32_t leafId) const {
  for (uint32_t i = 0; i < state->leafCount && i < PresetState::kMaxLeaves; ++i) {
    if (state->leaves[i].id == leafId) return BitsOf(state->leaves[i].value);
  }
  const ParamDescriptor* d = FindParam(static_cast<ParamId>(leafId));
  return d != nullptr ? BitsOf(d->def) : 0u;
}

void Document::SetLeafBits(uint32_t leafId, uint32_t bits) {
  for (uint32_t i = 0; i < state->leafCount && i < PresetState::kMaxLeaves; ++i) {
    if (state->leaves[i].id == leafId) std::memcpy(&state->leaves[i].value, &bits, sizeof bits);
  }
}

const Location* Document::Where(const std::string& key) const {
  const auto it = where.find(key);
  return it != where.end() ? &it->second : nullptr;
}

// ── Schema facts ─────────────────────────────────────────────────────────────────────────

namespace {

bool HasOp(const ModeLayer& layer, ModifierOp op) {
  return layer.modifier[0] == op || layer.modifier[1] == op;
}

bool StartsWith(std::string_view s, std::string_view prefix) {
  return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

}  // namespace

// The rule of dsp/src/blob/Mode.cpp's ElementPresent (lane B), which ValidateMode applies;
// compiler_unit checks that the two agree for every row and element.
bool ElementPresent(const ModeBlob& mode, uint32_t leafId) {
  const bool twoLayers = mode.schedule.layerCount >= 2u;
  switch (static_cast<ParamId>(leafId)) {
    case ParamId::SvfCutoffHz:
    case ParamId::SvfRes:
      return HasOp(mode.layers[0], ModifierOp::Svf);
    case ParamId::CrushBits:
    case ParamId::CrushDownsample:
      return HasOp(mode.layers[0], ModifierOp::Crush);
    case ParamId::L1SvfCutoffHz:
    case ParamId::L1SvfRes:
      return twoLayers && HasOp(mode.layers[1], ModifierOp::Svf);
    case ParamId::L1CrushBits:
    case ParamId::L1CrushDownsample:
      return twoLayers && HasOp(mode.layers[1], ModifierOp::Crush);
    case ParamId::LayerMix:
      return twoLayers;
    case ParamId::StepCount:
      return mode.steps.countMax > 0u;
    case ParamId::Modulator0RateHz:
    case ParamId::Modulator0Depth:
      return mode.modulators[0].type != ModulatorType::None;
    case ParamId::Modulator1RateHz:
    case ParamId::Modulator1Depth:
      return mode.modulators[1].type != ModulatorType::None;
    default:
      break;
  }
  if (leafId >= static_cast<uint32_t>(ParamId::L1DelayMs) &&
      leafId <= static_cast<uint32_t>(ParamId::L1CrushDownsample)) {
    return twoLayers;
  }
  return true;
}

bool StructureLeaf(uint32_t leafId) {
  return leafId == static_cast<uint32_t>(ParamId::OnsetTrigger) ||
         leafId == static_cast<uint32_t>(ParamId::PositionSource);
}

uint32_t StructureLeafBits(const ModeBlob& mode, uint32_t leafId) {
  constexpr uint32_t kOne = 0x3F800000u;
  if (leafId == static_cast<uint32_t>(ParamId::OnsetTrigger)) {
    return (mode.schedule.sources & kSourceOnset) != 0u ? kOne : 0u;
  }
  if (leafId == static_cast<uint32_t>(ParamId::PositionSource)) {
    return mode.layers[0].source == PositionSource::Mark ? kOne : 0u;
  }
  return 0u;
}

bool IsSchemaLeaf(uint32_t leafId) {
  // A Leaf row, or a Reserved row that a later wave makes a Leaf: IDs 29-68 (design §4.2). The
  // Reserved rows from 69 on become performance and device rows (perf.reverse, perf.loop_level,
  // global.trigger_offset), which no preset document holds.
  const ParamDescriptor* d = FindParam(static_cast<ParamId>(leafId));
  return d != nullptr && d->name != nullptr && !StructureLeaf(leafId) &&
         (d->kind == ParamKind::Leaf || (d->kind == ParamKind::Reserved &&
                                         leafId < static_cast<uint32_t>(ParamId::MacroActivity)));
}

const ParamDescriptor* SchemaLeaf(std::string_view name) {
  for (const ParamDescriptor& d : kParamTable) {
    if (d.name != nullptr && name == d.name) {
      return IsSchemaLeaf(static_cast<uint32_t>(d.id)) ? &d : nullptr;
    }
  }
  return nullptr;
}

std::string LeafPath(std::string_view name) {
  for (const char* prefix : {"layer", "modulator"}) {
    const std::string_view p(prefix);
    if (StartsWith(name, p) && name.size() > p.size() + 1 && name[p.size()] >= '0' &&
        name[p.size()] <= '9' && name[p.size() + 1] == '.') {
      return std::string(p) + "s[" + std::string(1, name[p.size()]) + "]" +
             std::string(name.substr(p.size() + 1));
    }
  }
  return std::string(name);
}

std::string LeafPointer(std::string_view name) {
  std::string out = "/";
  for (const char c : LeafPath(name)) {
    if (c == '.' || c == '[') {
      out.push_back('/');
    } else if (c != ']') {
      out.push_back(c);
    }
  }
  return out;
}

const char* LeafWave(uint32_t leafId) {
  switch (static_cast<ParamId>(leafId)) {
    case ParamId::Repeat:
    case ParamId::DecayMs:
    case ParamId::VoiceCount:
    case ParamId::Intermittency:
    case ParamId::BurstCount:
    case ParamId::BurstSpacingMs:
      return "W1";
    case ParamId::StepCount:
    case ParamId::DelaySync:
      return "W2";
    default:
      return "W3";
  }
}

namespace {
const char* const  kMacroNames[kMaxMacros] = {"activity", "repeats", "shape", "time",
                                              "space",    "filter",  "aux1",  "aux2"};
constexpr uint32_t kFirstMacro             = static_cast<uint32_t>(ParamId::MacroActivity);
}  // namespace

const char* MacroName(uint32_t macroId) {
  return macroId >= kFirstMacro && macroId < kFirstMacro + kMaxMacros
             ? kMacroNames[macroId - kFirstMacro]
             : nullptr;
}

uint32_t MacroIdByName(std::string_view name) {
  for (uint32_t k = 0; k < kMaxMacros; ++k) {
    if (name == kMacroNames[k]) return kFirstMacro + k;
  }
  return 0;
}

}  // namespace bsc
