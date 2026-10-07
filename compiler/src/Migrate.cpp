#include "Migrate.h"

#include <cstring>

#include "Text.h"

namespace bsc {

using namespace brainscape;

void MigrateByName(json::Value* /*root*/, uint32_t /*fromVersion*/) {
  // Schema 1 is the first schema: no key has been renamed or moved yet. A later schema adds
  // its renames here, keyed by the version that made them (§2.2, per-key defaulting covers
  // additions).
}

namespace {

uint32_t Rd32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 |
         static_cast<uint32_t>(p[2]) << 16 | static_cast<uint32_t>(p[3]) << 24;
}

// SetParam's canonical value (determinism profile §3.7), on the bit pattern: NaN and
// infinities to the minimum, -0 and subnormals to +0, then the clamp.
uint32_t Canonical(const ParamDescriptor& d, uint32_t b) {
  if ((b & 0x7F800000u) == 0x7F800000u) return BitsOf(d.min);
  if ((b & 0x7F800000u) == 0u) b = 0u;
  if (LessBits(b, BitsOf(d.min))) return BitsOf(d.min);
  if (LessBits(BitsOf(d.max), b)) return BitsOf(d.max);
  return b;
}

}  // namespace

bool MigrateSession(const uint8_t* bytes, size_t length, const std::string& id,
                    const std::string& name, Document* out, std::vector<Finding>* findings) {
  const auto note = [&](bool error, const std::string& message) {
    if (findings != nullptr) findings->push_back(Finding{"M1", error, Location{}, message});
    return false;
  };
  if (length < 12u || std::memcmp(bytes, "BSWS", 4) != 0) return note(true, "not a BSWS session");
  if (Rd32(bytes + 4) != 1u)
    return note(true, "BSWS version " + Dec(Rd32(bytes + 4)) + ": only v1 migrates");
  const uint32_t n = Rd32(bytes + 8);
  if (n > (length - 12u) / 8u) return note(true, "truncated session");
  Document d;
  d.id   = id;
  d.name = name;
  uint32_t bits[kNumParams + 1];
  for (uint32_t i = 1; i <= kNumParams; ++i)
    bits[i] = BitsOf(FindParam(static_cast<ParamId>(i))->def);
  bool onset = false, mark = false;
  for (uint32_t i = 0; i < n; ++i) {
    const uint32_t         leafId = Rd32(bytes + 12 + 8 * i);
    const uint32_t         value  = Rd32(bytes + 16 + 8 * i);
    const ParamDescriptor* row    = FindParam(static_cast<ParamId>(leafId));
    if (row == nullptr || row->kind != ParamKind::Leaf) {
      note(false, "id " + Dec(leafId) + " is not a leaf of this build; dropped");
      continue;
    }
    const uint32_t c = Canonical(*row, value);
    if (leafId == static_cast<uint32_t>(ParamId::OnsetTrigger)) {
      onset = !LessBits(c, 0x3F000000u);  // >= 0.5
    } else if (leafId == static_cast<uint32_t>(ParamId::PositionSource)) {
      mark = !LessBits(c, 0x3F000000u);
    } else {
      bits[leafId] = c;
    }
  }
  ModeBlob& m = d.state->mode;
  if (onset) m.schedule.sources = static_cast<uint8_t>(m.schedule.sources | kSourceOnset);
  if (mark) m.layers[0].source = PositionSource::Mark;
  m.features = RequiredModeFeatures(m);
  // The leaves: every Leaf row of a present element (§6.2), 27 and 28 from the structure.
  PresetState& s = *d.state;
  s.leafCount    = 0;
  for (const ParamDescriptor& row : kParamTable) {
    const auto leafId = static_cast<uint32_t>(row.id);
    if (row.kind != ParamKind::Leaf || !ElementPresent(m, leafId)) continue;
    const uint32_t b         = StructureLeaf(leafId) ? StructureLeafBits(m, leafId) : bits[leafId];
    s.leaves[s.leafCount].id = leafId;
    std::memcpy(&s.leaves[s.leafCount].value, &b, sizeof b);
    ++s.leafCount;
  }
  for (uint32_t k = 0; k < s.control.macroCount; ++k)
    d.omittedPositions.push_back(s.control.positions[k].macroId);
  if (bits[static_cast<uint32_t>(ParamId::WetTrimDb)] != 0u) {
    note(false, "wet_trim_db is " + NumberText(bits[static_cast<uint32_t>(ParamId::WetTrimDb)]) +
                    " dB: from sound revision 2 it trims the wet signal only, so the session "
                    "will sound different");
  }
  if (onset || mark) {
    note(false, std::string("the session uses ") + (onset ? "the onset trigger" : "") +
                    (onset && mark ? " and " : "") + (mark ? "mark positioning" : "") +
                    ", which this build's compiler accepts from sound revision 2");
  }
  *out = std::move(d);
  return true;
}

}  // namespace bsc
