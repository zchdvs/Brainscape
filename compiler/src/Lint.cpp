#include "Lint.h"

#include <algorithm>

#include "Text.h"
#include "brainscape/ModeEval.h"
#include "brainscape/ParamDisplay.h"
#include "brainscape/Tempo.h"

namespace bsc {

using namespace brainscape;

namespace {

constexpr uint32_t kFirstMacro = static_cast<uint32_t>(ParamId::MacroActivity);

std::string Quoted(std::string_view s) { return "`" + std::string(s) + "`"; }

const char* RowName(uint32_t id) {
  const ParamDescriptor* d = FindParam(static_cast<ParamId>(id));
  return d != nullptr && d->name != nullptr ? d->name : "?";
}

std::string Display(uint32_t id, uint32_t bits) {
  char buf[48];
  FormatPlain(static_cast<ParamId>(id), FloatOf(bits), buf, sizeof buf);
  return std::string(buf);
}

Location At(const Document& d, const std::string& key, const std::string& fallback) {
  const Location* l = d.Where(key);
  return l != nullptr ? *l : Location{fallback, 0, 0};
}

// The leaf's values: the stored one and both ends of every macro target on it.
void Reach(const Document& d, uint32_t leafId, uint32_t* lowest, uint32_t* highest) {
  uint32_t          lo = d.LeafBits(leafId), hi = lo;
  const MacroTable& t = d.state->mode.macros;
  for (uint32_t i = 0; i < t.targetCount; ++i) {
    if (t.targets[i].param != leafId) continue;
    for (const uint32_t b : {BitsOf(t.targets[i].lo), BitsOf(t.targets[i].hi)}) {
      if (LessBits(b, lo)) lo = b;
      if (LessBits(hi, b)) hi = b;
    }
  }
  *lowest  = lo;
  *highest = hi;
}

// Reach, and both ends of every expression assignment on the leaf too: the values the controls
// reach (the expression pedal moves a leaf as a macro does, clock.md §6.5).
void ReachAll(const Document& d, uint32_t leafId, uint32_t* lowest, uint32_t* highest) {
  Reach(d, leafId, lowest, highest);
  const ControlState& c = d.state->control;
  for (uint32_t i = 0; i < c.exprCount && i < kMaxExpressions; ++i) {
    if (c.expressions[i].target != leafId) continue;
    for (const uint32_t b : {BitsOf(c.expressions[i].lo), BitsOf(c.expressions[i].hi)}) {
      if (LessBits(b, *lowest)) *lowest = b;
      if (LessBits(*highest, b)) *highest = b;
    }
  }
}

// A counting leaf's integer as the engine reads it, RoundHalfAwayI32 of the canonical value
// (mode-compiler.md §3.7), here from the bits of a value in [0, 2^23): floor(v + 1/2) is
// floor((floor(2v) + 1) / 2). Negative values, NaN and below 1/2 read 0.
uint32_t CountOf(uint32_t bits) {
  if ((bits >> 31) != 0u) return 0u;
  const uint32_t exponent = (bits >> 23) & 0xFFu;
  if (exponent < 126u) return 0u;  // below 1/2
  if (exponent >= 149u) return 0x7FFFFFFFu;  // 2^22 or more: never a code
  const uint32_t mant = (bits & 0x7FFFFFu) | 0x800000u;  // v = mant · 2^(exponent - 150)
  return ((mant >> (149u - exponent)) + 1u) >> 1u;
}

// The tempo a preset stores, in BPM to a tenth, for messages: 6·10^7 / µs, rounded.
std::string Bpm(uint32_t usPerQuarter) {
  const uint64_t tenths = (600000000ull + usPerQuarter / 2u) / usPerQuarter;
  return Dec(static_cast<uint32_t>(tenths / 10u)) + "." + Dec(static_cast<uint32_t>(tenths % 10u));
}

// A synced field's effective value (FormatSyncedTime) at a tempo and Subdiv, at 48 kHz.
std::string Synced(tempo::SyncTarget target, uint32_t code, uint32_t ns, uint8_t subdiv,
                   uint8_t timeMode) {
  char buf[96];
  FormatSyncedTime(target, static_cast<uint8_t>(code), ns, subdiv, timeMode, 48000u, buf,
                   sizeof buf);
  return std::string(buf);
}

bool Detached(const Document& d, uint32_t leafId) {
  return std::find(d.detached.begin(), d.detached.end(), leafId) != d.detached.end();
}

// Lowercase ASCII words; any other ASCII character separates them, and bytes of multi-byte
// UTF-8 characters belong to the word they are in.
std::vector<std::string> Words(std::string_view s) {
  std::vector<std::string> out;
  std::string              word;
  for (const char ch : s) {
    const auto c = static_cast<uint8_t>(ch);
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c >= 0x80u) {
      word.push_back(ch);
    } else if (c >= 'A' && c <= 'Z') {
      word.push_back(static_cast<char>(c - 'A' + 'a'));
    } else if (!word.empty()) {
      out.push_back(word);
      word.clear();
    }
  }
  if (!word.empty()) out.push_back(word);
  return out;
}

// The first denylisted mark in `s`, or "".
std::string Mark(std::string_view s) {
  const std::vector<std::string> words = Words(s);
  for (const std::string& mark : Denylist()) {
    const std::vector<std::string> m = Words(mark);
    for (size_t i = 0; i + m.size() <= words.size(); ++i) {
      if (std::equal(m.begin(), m.end(), words.begin() + static_cast<std::ptrdiff_t>(i)))
        return mark;
    }
  }
  return std::string();
}

// Exact comparisons of binary32 distances: each value as an integer scaled by 2^149 in 320-bit
// two's complement.
struct Wide {
  uint64_t w[5] = {0, 0, 0, 0, 0};
};

Wide Scaled(uint32_t bits) {
  Wide           r;
  const uint32_t exponent = (bits >> 23) & 0xFFu;
  const uint64_t mant     = (bits & 0x7FFFFFu) | (exponent != 0u ? 0x800000u : 0u);
  const uint32_t shift    = exponent != 0u ? exponent - 1u : 0u;  // value * 2^149
  const uint32_t word = shift / 64u, bit = shift % 64u;
  r.w[word] = mant << bit;
  if (bit != 0u && word + 1u < 5u) r.w[word + 1] = mant >> (64u - bit);
  if ((bits & 0x80000000u) != 0u) {  // negate
    uint64_t carry = 1;
    for (uint64_t& x : r.w) {
      x     = ~x + carry;
      carry = (carry != 0u && x == 0u) ? 1u : 0u;
    }
  }
  return r;
}

Wide Add(const Wide& a, const Wide& b) {
  Wide     r;
  uint64_t carry = 0;
  for (int i = 0; i < 5; ++i) {
    const uint64_t s = a.w[i] + b.w[i];
    const uint64_t c = s < a.w[i] ? 1u : 0u;
    r.w[i]           = s + carry;
    carry            = c | (r.w[i] < s ? 1u : 0u);
  }
  return r;
}

Wide Negate(const Wide& a) {
  Wide     r;
  uint64_t carry = 1;
  for (int i = 0; i < 5; ++i) {
    r.w[i] = ~a.w[i] + carry;
    carry  = (carry != 0u && r.w[i] == 0u) ? 1u : 0u;
  }
  return r;
}

int Sign(const Wide& a) {
  if ((a.w[4] >> 63) != 0u) return -1;
  for (const uint64_t x : a.w) {
    if (x != 0u) return 1;
  }
  return 0;
}

// |t - v| for binary32 values, exactly.
Wide Distance(uint32_t t, uint32_t v) {
  const Wide d = Add(Scaled(t), Negate(Scaled(v)));
  return Sign(d) < 0 ? Negate(d) : d;
}

// sign(|t - a| - |t - b|): negative when a lands nearer t, zero on a tie.
int Nearer(uint32_t t, uint32_t a, uint32_t b) {
  return Sign(Add(Distance(t, a), Negate(Distance(t, b))));
}

// Whether `frames` at 48 kHz, frames / 48 ms, is below the binary32 `ms`, exactly: frames · 2^149
// against 48 · ms · 2^149 (48 = 32 + 16).
bool FramesBelowMs(uint32_t frames, uint32_t msBits) {
  Wide x16 = Scaled(msBits);
  for (int i = 0; i < 4; ++i) x16 = Add(x16, x16);
  const Wide x48 = Add(x16, Add(x16, x16));
  return Sign(Add(Scaled(tempo::IntegerValueBits(frames)), Negate(x48))) < 0;
}

uint32_t ValueAt(const ModeBlob& mode, uint32_t macroId, uint32_t index, uint32_t positionBits) {
  PresetLeaf   out[kMaxMacroTargets];
  const size_t n =
      EvalMacro(mode, static_cast<ParamId>(macroId), FloatOf(positionBits), out, kMaxMacroTargets);
  return index < n ? BitsOf(out[index].value) : 0u;
}

// Canonical positions in [0, 1] in order: +0, then the normal floats up to 1 (a subnormal
// position canonicalizes to +0).
constexpr uint32_t kPositions = 0x3F800000u - 0x00800000u + 2u;
uint32_t           PositionBits(uint32_t k) { return k == 0u ? 0u : 0x00800000u + (k - 1u); }

}  // namespace

const std::vector<std::string>& Denylist() {
  // Record §2.2: Hologram's marks and effect names, and other makers' product names.
  static const std::vector<std::string> kList = {
      "hologram", "microcosm", "chroma console", "infinite jets",  "mosaic",       "seq",
      "glide",    "haze",      "tunnel",         "strum",          "blocks",       "interrupt",
      "arp",      "pattern",   "warp",           "dream sequence", "hold sampler", "phrase looper",
      "mood",     "blooper",   "habit",          "onward",         "particle",     "tensor",
      "clouds",   "beads",     "lore",           "fathom",         "fable",        "ricochet",
      "drift"};
  return kList;
}

uint32_t SolvePosition(const ModeBlob& mode, uint32_t macroId, uint32_t index, uint32_t leafBits,
                       uint32_t current) {
  const MacroTable& t  = mode.macros;
  uint32_t          lo = 0, hi = 0;
  for (uint32_t k = 0; k < t.macroCount; ++k) {
    if (t.macros[k].id != macroId || index >= t.macros[k].count) continue;
    lo = BitsOf(t.targets[t.macros[k].first + index].lo);
    hi = BitsOf(t.targets[t.macros[k].first + index].hi);
  }
  const bool rising = !LessBits(hi, lo);
  const auto value  = [&](uint32_t k) {
    return ValueAt(mode, macroId, index, PositionBits(k));
  };
  const uint32_t top = value(kPositions - 1u);
  // The first position whose value reaches `aim` (non-decreasing when rising), for an aim the
  // top position reaches.
  const auto firstReaching = [&](uint32_t aim) {
    uint32_t first = 0, last = kPositions - 1u;  // the answer is in [first, last]
    while (first < last) {
      const uint32_t mid = first + (last - first) / 2u;
      const uint32_t v   = value(mid);
      if (rising ? !LessBits(v, aim) : !LessBits(aim, v)) {
        last = mid;
      } else {
        first = mid + 1u;
      }
    }
    return first;
  };
  // Past the top, every position that gives the top value lands as near as any: aim at it.
  const bool     past  = rising ? LessBits(top, leafBits) : LessBits(leafBits, top);
  const uint32_t aim   = past ? top : leafBits;
  const uint32_t first = firstReaching(aim);
  uint32_t       best  = first;
  if (first > 0u) {
    // value(first - 1) misses the aim and value(first) reaches it: the nearer of the two values,
    // the lower position's on a tie, and the first position that gives it.
    const uint32_t a = value(first - 1u);
    if (Nearer(aim, a, value(first)) <= 0) best = firstReaching(a);
  }
  // The stored position stays when it lands as near the leaf as the answer (no churn).
  const bool canonical = current == 0u || (current >= 0x00800000u && current <= 0x3F800000u);
  if (canonical) {
    const uint32_t v = ValueAt(mode, macroId, index, current);
    if (Nearer(leafBits, v, value(best)) <= 0) return current;
  }
  return PositionBits(best);
}

void Derive(Document* doc, bool solve, std::vector<std::string>* log) {
  PresetState&      s        = *doc->state;
  const MacroTable& t        = s.mode.macros;
  const auto        position = [&](uint32_t macroId) -> float* {
    for (uint32_t k = 0; k < s.control.macroCount; ++k) {
      if (s.control.positions[k].macroId == macroId) return &s.control.positions[k].position;
    }
    return nullptr;
  };
  if (solve) {
    for (uint32_t k = 0; k < t.macroCount; ++k) {
      const MacroDef& md = t.macros[k];
      float*          p  = position(md.id);
      if (p == nullptr) continue;
      for (uint32_t i = 0; i < md.count; ++i) {
        const MacroTarget& target = t.targets[md.first + i];
        if (Detached(*doc, target.param) || BitsOf(target.lo) == BitsOf(target.hi)) continue;
        const uint32_t before = BitsOf(*p);
        const uint32_t solved =
            SolvePosition(s.mode, md.id, i, doc->LeafBits(target.param), before);
        *p                    = FloatOf(solved);
        if (log != nullptr && solved != before) {
          log->push_back(std::string("controls.macro_positions.") + MacroName(md.id) + ": " +
                         NumberText(before) + " -> " + NumberText(solved) + " (from " +
                         RowName(target.param) + ")");
        }
        break;
      }
    }
    doc->omittedPositions.clear();
  }
  // Each leaf's net change, logged once, naming the macro that wrote it last.
  struct Change {
    uint32_t leaf, before, macro, position;
  };
  std::vector<Change> changes;
  for (uint32_t k = 0; k < t.macroCount; ++k) {
    const MacroDef& md = t.macros[k];
    const float*    p  = position(md.id);
    if (p == nullptr) continue;
    PresetLeaf   out[kMaxMacroTargets];
    const size_t n = EvalMacro(s.mode, static_cast<ParamId>(md.id), *p, out, kMaxMacroTargets);
    for (size_t i = 0; i < n; ++i) {
      if (Detached(*doc, out[i].id) || !ElementPresent(s.mode, out[i].id)) continue;
      const auto at = std::find_if(changes.begin(), changes.end(),
                                   [&](const Change& c) { return c.leaf == out[i].id; });
      if (at == changes.end()) {
        changes.push_back(Change{out[i].id, doc->LeafBits(out[i].id), md.id, BitsOf(*p)});
      } else {
        at->macro    = md.id;
        at->position = BitsOf(*p);
      }
      doc->SetLeafBits(out[i].id, BitsOf(out[i].value));
    }
  }
  if (log == nullptr) return;
  for (const Change& c : changes) {
    const uint32_t after = doc->LeafBits(c.leaf);
    if (after == c.before) continue;
    log->push_back(std::string(RowName(c.leaf)) + ": " + NumberText(c.before) + " -> " +
                   NumberText(after) + " (" + MacroName(c.macro) + " at " +
                   NumberText(c.position) + ")");
  }
}

std::vector<Finding> Lint(const Document& d, const LintOptions& options) {
  std::vector<Finding> out;
  const auto add = [&](const char* code, bool factoryError, Location at, std::string message) {
    out.push_back(
        Finding{code, options.factory && factoryError, std::move(at), std::move(message)});
  };
  const PresetState& s = *d.state;
  const ModeBlob&    m = s.mode;
  const MacroTable&  t = m.macros;

  // L1: subnormals written (the reader noted them).
  for (const Finding& note : d.notes) out.push_back(note);

  // L2: the near guard for each pitch entry (engine §3).
  for (uint32_t n = 0; n < m.schedule.layerCount; ++n) {
    const bool     l1   = n == 1u;
    const uint32_t base = static_cast<uint32_t>(l1 ? ParamId::L1DelayMs : ParamId::DelayMs);
    const uint32_t size = static_cast<uint32_t>(l1 ? ParamId::L1GrainSizeMs : ParamId::GrainSizeMs);
    const uint32_t trans =
        static_cast<uint32_t>(l1 ? ParamId::L1TransposeSt : ParamId::TransposeSt);
    const uint32_t spread =
        static_cast<uint32_t>(l1 ? ParamId::L1SpreadCents : ParamId::SpreadCents);
    uint32_t baseLo, baseHi, sizeLo, sizeHi, transLo, transHi, spreadLo, spreadHi;
    Reach(d, base, &baseLo, &baseHi);
    Reach(d, size, &sizeLo, &sizeHi);
    Reach(d, trans, &transLo, &transHi);
    Reach(d, spread, &spreadLo, &spreadHi);
    // A synced base delay (clock.md §6.2, §11.1) at its shortest: the note value at 300 BPM under
    // ×8, the shortest tempo and the finest Subdiv the live controls reach, folded (§5.3).
    const uint32_t syncCode = m.layers[n].baseSync;
    const uint32_t syncLo =
        syncCode != 0u ? tempo::SyncedDuration(tempo::SyncTarget::BaseDelay,
                                               static_cast<uint8_t>(syncCode),
                                               tempo::kMinNsPerQuarter, 5, 0, 48000u)
                             .frames
                       : 0u;
    for (uint32_t e = 0; e < m.pitch[n].count; ++e) {
      const float    st = m.pitch[n].entries[e].st;
      const uint32_t guard =
          BitsOf(NearGuardMs(FloatOf(sizeHi), st, FloatOf(transHi), FloatOf(spreadHi)));
      if (syncCode != 0u) {
        if (FramesBelowMs(syncLo, guard)) {
          add("L2", false,
              At(d, "base_sync:" + Dec(n), "/layers/" + Dec(n) + "/position/base_sync"),
              "the synced base delay at its shortest, " +
                  Synced(tempo::SyncTarget::BaseDelay, syncCode, tempo::kMinNsPerQuarter, 5, 0) +
                  " (300 BPM, Subdiv \xC3\x97" "8), is below size_ms * (r - 1) = " +
                  NumberText(guard) + " ms for pitch entry " + NumberText(BitsOf(st)) +
                  " st at the largest size, transpose and spread reached, so the guard moves "
                  "those grains back");
        }
        continue;
      }
      if (LessBits(baseLo, guard)) {
        add("L2", false, At(d, "leaf:" + Dec(base), "/layers/" + Dec(n) + "/position/base_ms"),
            "the smallest base_ms reached, " + NumberText(baseLo) +
                " ms, is below size_ms * (r - 1) = " + NumberText(guard) + " ms for pitch entry " +
                NumberText(BitsOf(st)) +
                " st at the largest size, transpose and spread reached, so the guard moves those "
                "grains back");
      }
    }
  }

  // L3: activity, repeats and time do something.
  for (const ParamId macro : {ParamId::MacroActivity, ParamId::MacroRepeats, ParamId::MacroTime}) {
    for (uint32_t k = 0; k < t.macroCount; ++k) {
      if (t.macros[k].id != static_cast<uint32_t>(macro) || t.macros[k].count != 0u) continue;
      add("L3", false, At(d, "macro:" + Dec(t.macros[k].id), "/macros"),
          std::string("the ") + MacroName(t.macros[k].id) + " macro has no targets");
    }
  }

  // L4: targeted leaves derived from the stored positions (§3.5).
  for (uint32_t k = 0; k < t.macroCount; ++k) {
    const MacroDef& md       = t.macros[k];
    uint32_t        position = 0x3F000000u;
    for (uint32_t c = 0; c < s.control.macroCount; ++c) {
      if (s.control.positions[c].macroId == md.id)
        position = BitsOf(s.control.positions[c].position);
    }
    PresetLeaf   values[kMaxMacroTargets];
    const size_t n =
        EvalMacro(m, static_cast<ParamId>(md.id), FloatOf(position), values, kMaxMacroTargets);
    for (size_t i = 0; i < n; ++i) {
      const uint32_t leafId = values[i].id;
      if (Detached(d, leafId)) continue;
      const uint32_t stored  = d.LeafBits(leafId);
      const uint32_t derived = BitsOf(values[i].value);
      if (stored == derived) continue;
      const std::string a = Display(leafId, stored), b = Display(leafId, derived);
      if (a == b) continue;
      add("L4", true, At(d, "leaf:" + Dec(leafId), LeafPointer(RowName(leafId))),
          std::string(RowName(leafId)) + " is " + NumberText(stored) + " (" + a + ") but " +
              MacroName(md.id) + " at " + NumberText(position) + " gives " + NumberText(derived) +
              " (" + b + "); bspc derive rewrites it, or list it in editor.detached");
    }
  }
  for (const uint32_t macroId : d.omittedPositions) {
    add("L4", true, Location{"/controls/macro_positions", 0, 0},
        std::string("the position of ") + MacroName(macroId) + " is omitted (compiled as 0.5)");
  }

  // L5: something starts grains without a trigger. With no source at all nothing ever does
  // (the engine drops a footswitch or MIDI trigger whose source the mode does not list), so that
  // case is an error for --factory.
  const uint8_t sources = m.schedule.sources;
  if (sources == 0u) {
    add("L5", true, At(d, "sources", "/scheduler/sources"),
        "no source at all: this mode never plays a grain (it lists no periodic, clock, onset, "
        "footswitch or midi_note source)");
  } else if ((sources & (kSourcePeriodic | kSourceClock | kSourceOnset)) == 0u) {
    add("L5", false, At(d, "sources", "/scheduler/sources"),
        "no free-running source (periodic, clock) and no onset: silent until triggered");
  }

  // L6: mark positioning needs a decay.
  for (uint32_t n = 0; n < m.schedule.layerCount; ++n) {
    if (m.layers[n].source != PositionSource::Mark) continue;
    const uint32_t decay = static_cast<uint32_t>(n == 1u ? ParamId::L1DecayMs : ParamId::DecayMs);
    if (d.LeafBits(decay) == 0u) {
      add("L6", false, At(d, "source:" + Dec(n), "/layers/" + Dec(n) + "/position/source"),
          "mark positioning with decay_ms 0 holds the last note until the next onset");
    }
  }

  // L7: Shift secondaries are written directly by the panel (§3.1).
  const ParamId secondaries[] = {ParamId::ModDepth, ParamId::ModRateHz, ParamId::ReverbTime,
                                 ParamId::ReverbMode, ParamId::FilterRes};
  for (uint32_t k = 0; k < t.macroCount; ++k) {
    const MacroDef& md = t.macros[k];
    for (uint32_t i = 0; i < md.count; ++i) {
      const uint32_t param = t.targets[md.first + i].param;
      for (const ParamId sec : secondaries) {
        if (param != static_cast<uint32_t>(sec)) continue;
        add("L7", true, At(d, "target:" + Dec(md.id) + ":" + Dec(i), "/macros"),
            std::string(MacroName(md.id)) + " targets " + RowName(param) +
                ", a Shift secondary the panel writes directly");
      }
    }
  }

  // L8: the universal endpoints of Filter and Space (§3.1).
  for (uint32_t k = 0; k < t.macroCount; ++k) {
    const MacroDef& md = t.macros[k];
    const Location  at = At(d, "macro:" + Dec(md.id), "/macros");
    if (md.id == static_cast<uint32_t>(ParamId::MacroFilter)) {
      const ParamDescriptor& cutoff = *FindParam(ParamId::FilterCutoffHz);
      bool                   ok     = false;
      for (uint32_t i = 0; i < md.count; ++i) {
        const MacroTarget& target = t.targets[md.first + i];
        ok                        = ok ||
             (target.param == static_cast<uint32_t>(ParamId::FilterCutoffHz) &&
              BitsOf(target.lo) == BitsOf(cutoff.min) && BitsOf(target.hi) == BitsOf(cutoff.max));
      }
      if (!ok) {
        add("L8", true, at,
            "the filter macro must run post.filter.cutoff_hz from its minimum (40 Hz, the wet "
            "kill) at 0 to its maximum (20000 Hz, bypass) at 1");
      }
    }
    if (md.id == static_cast<uint32_t>(ParamId::MacroSpace)) {
      for (uint32_t i = 0; i < md.count; ++i) {
        const MacroTarget& target = t.targets[md.first + i];
        if ((target.param == static_cast<uint32_t>(ParamId::DelayMix) ||
             target.param == static_cast<uint32_t>(ParamId::ReverbMix)) &&
            BitsOf(target.lo) != 0u) {
          add("L8", true, At(d, "target:" + Dec(md.id) + ":" + Dec(i), "/macros"),
              std::string("the space macro adds wet at 0: ") + RowName(target.param) +
                  " starts at " + NumberText(BitsOf(target.lo)));
        }
      }
    }
  }

  // Synced times (clock.md §5.2, §6.2, §6.5; sound revision 9). The stored tempo and Subdiv as a
  // load plays them (§2.4: Tempo time mode forces TAP).
  const PerformanceState& perf    = s.performance;
  const uint32_t          ns      = perf.usPerQuarter * 1000u;
  const auto              subdiv  = static_cast<uint8_t>(perf.subdiv);
  const auto              tmode   = static_cast<uint8_t>(perf.timeMode);
  static const char* const kRates[] = {"TAP", "\xC3\x97" "1/4", "\xC3\x97" "1/2",
                                       "\xC3\x97" "2", "\xC3\x97" "4", "\xC3\x97" "8"};
  const std::string       storedAt =
      "at the stored " + Bpm(perf.usPerQuarter) + " BPM and " +
      kRates[tmode == tempo::kTimeModeTempo || subdiv >= tempo::kSubdivCodes ? 0u : subdiv];

  // L12 (§6.2, Q11, D6): a synced base delay with grain feedback, whose repeats fall a FIFO pass
  // (10.67 ms at 48 kHz) later per pass than the grid.
  uint32_t fbLo, fbHi;
  ReachAll(d, static_cast<uint32_t>(ParamId::Feedback), &fbLo, &fbHi);
  for (uint32_t n = 0; n < m.schedule.layerCount; ++n) {
    if (m.layers[n].baseSync == 0u || !LessBits(0u, fbHi)) continue;
    add("L12", false, At(d, "base_sync:" + Dec(n), "/layers/" + Dec(n) + "/position/base_sync"),
        "layer " + Dec(n) + " syncs its base delay with feedback.amount reaching " +
            NumberText(fbHi) +
            ": grain-feedback repeats fall 10.67 ms later per pass than the grid; tempo-exact "
            "repeats belong on the post delay (post.delay.sync with post.delay.fb)");
  }

  // L13 (§6.5): a macro target or expression assignment on a leaf a sync overrides wherever it
  // could move: post.delay.time_ms while row 63 is nonzero at every value the controls reach, a
  // layer's base_ms while its base_sync is not off (structure, which nothing moves).
  uint32_t syncLoBits, syncHiBits;
  ReachAll(d, static_cast<uint32_t>(ParamId::DelaySync), &syncLoBits, &syncHiBits);
  const auto overridden = [&](uint32_t leafId, const std::string& by) {
    for (uint32_t k = 0; k < t.macroCount; ++k) {
      const MacroDef& md = t.macros[k];
      for (uint32_t i = 0; i < md.count; ++i) {
        if (t.targets[md.first + i].param != leafId) continue;
        add("L13", false, At(d, "target:" + Dec(md.id) + ":" + Dec(i), "/macros"),
            std::string(MacroName(md.id)) + " targets " + RowName(leafId) + ", which " + by +
                " overrides at every position: the target does nothing");
      }
    }
    const ControlState& c = s.control;
    for (uint32_t i = 0; i < c.exprCount && i < kMaxExpressions; ++i) {
      if (c.expressions[i].target != leafId) continue;
      add("L13", false, At(d, "expression:" + Dec(i), "/controls/expression"),
          std::string("the expression pedal targets ") + RowName(leafId) + ", which " + by +
              " overrides at every position: the assignment does nothing");
    }
  };
  if (CountOf(syncLoBits) >= 1u) {
    overridden(static_cast<uint32_t>(ParamId::DelayTimeMs), "post.delay.sync");
  }
  for (uint32_t n = 0; n < m.schedule.layerCount; ++n) {
    if (m.layers[n].baseSync == 0u) continue;
    overridden(static_cast<uint32_t>(n == 1u ? ParamId::L1DelayMs : ParamId::DelayMs),
               "layer " + Dec(n) + "'s base_sync");
  }

  // L14 (§5.2, §5.3): a synced field whose reachable codes fold at the stored tempo and Subdiv,
  // where a sweep of the code is no longer monotone in duration.
  {
    const uint32_t lo = CountOf(syncLoBits) < 1u ? 1u : CountOf(syncLoBits);
    const uint32_t hi = CountOf(syncHiBits) > kMaxSyncDivision ? kMaxSyncDivision : CountOf(syncHiBits);
    std::string    folds;
    for (uint32_t code = lo; code <= hi; ++code) {
      const tempo::SyncedTime st = tempo::SyncedDuration(
          tempo::SyncTarget::PostDelay, static_cast<uint8_t>(code), ns, subdiv, tmode, 48000u);
      if (st.frames == 0u || st.octaves == 0) continue;
      folds += (folds.empty() ? "" : ", ") +
               Synced(tempo::SyncTarget::PostDelay, code, ns, subdiv, tmode);
    }
    if (!folds.empty()) {
      add("L14", false,
          At(d, "leaf:" + Dec(static_cast<uint32_t>(ParamId::DelaySync)), "/post/delay/sync"),
          "post.delay.sync's reachable codes fold " + storedAt + " (10 ms-4 s): " + folds +
              "; a sweep of the code is not monotone there");
    }
  }
  for (uint32_t n = 0; n < m.schedule.layerCount; ++n) {
    const uint32_t code = m.layers[n].baseSync;
    if (code == 0u) continue;
    const tempo::SyncedTime st = tempo::SyncedDuration(
        tempo::SyncTarget::BaseDelay, static_cast<uint8_t>(code), ns, subdiv, tmode, 48000u);
    if (st.frames == 0u || st.octaves == 0) continue;
    add("L14", false, At(d, "base_sync:" + Dec(n), "/layers/" + Dec(n) + "/position/base_sync"),
        "layer " + Dec(n) + "'s base_sync folds " + storedAt + " (1 ms-5 s): " +
            Synced(tempo::SyncTarget::BaseDelay, code, ns, subdiv, tmode));
  }

  // L9: other makers' marks in product strings.
  const auto check = [&](const std::string& text, const Location& at, const char* what) {
    const std::string mark = Mark(text);
    if (!mark.empty()) {
      add("L9", true, at,
          std::string(what) + " " + Quoted(text) + " holds " + Quoted(mark) +
              ", another maker's mark or product name (§11.2)");
    }
  };
  check(d.id, At(d, "id", "/id"), "id");
  check(d.name, At(d, "name", "/name"), "name");
  check(d.author, At(d, "author", "/meta/author"), "author");
  check(d.description, At(d, "description", "/meta/description"), "description");
  for (size_t i = 0; i < d.tags.size(); ++i) {
    check(d.tags[i], At(d, "tag:" + Dec(i), "/meta/tags/" + Dec(i)), "tag");
  }
  for (uint32_t k = 0; k < kMaxMacros; ++k) {
    if (d.displayName[k].empty()) continue;
    check(d.displayName[k], At(d, "display:" + Dec(kFirstMacro + k), "/macros"), "display name");
  }
  return out;
}

}  // namespace bsc
