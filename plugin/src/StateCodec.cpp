#include "StateCodec.h"

#include <cstring>

#include "brainscape/ParamDisplay.h"

namespace brainscape::plugin {

namespace {

constexpr uint8_t kMagic[4] = {'B', 'S', 'W', 'S'};

enum SettingKey : uint32_t {
  kInputMode = 1, kInputGainDb = 2, kOutputGainDb = 3, kRestartOnStart = 4, kEffectVolumeDb = 5
};

uint32_t Bits(float v) {
  uint32_t u = 0;
  std::memcpy(&u, &v, sizeof u);
  return u;
}

float FromBits(uint32_t u) {
  float v = 0.f;
  std::memcpy(&v, &u, sizeof v);
  return v;
}

void PutU32(std::vector<uint8_t>& out, uint32_t v) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

struct Reader {
  const uint8_t* p;
  size_t         left;
  bool U32(uint32_t& v) {
    if (left < 4) return false;
    v = static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 |
        static_cast<uint32_t>(p[2]) << 16 | static_cast<uint32_t>(p[3]) << 24;
    p += 4;
    left -= 4;
    return true;
  }
};

}  // namespace

float CanonicalGainDb(float db) noexcept {
  const uint32_t exponent = Bits(db) & 0x7F800000u;
  if (exponent == 0x7F800000u || exponent == 0u) return 0.f;
  if (db < -kWrapperGainRangeDb) return -kWrapperGainRangeDb;
  if (db > kWrapperGainRangeDb) return kWrapperGainRangeDb;
  return db;
}

void EncodeState(const WrapperState& state, std::vector<uint8_t>& out) {
  out.clear();
  out.insert(out.end(), kMagic, kMagic + 4);
  PutU32(out, kStateFormatVersion);
  PutU32(out, static_cast<uint32_t>(kNumLeafParams));
  for (size_t i = 0; i < kNumLeafParams; ++i) {
    PutU32(out, static_cast<uint32_t>(LeafId(i)));
    PutU32(out, Bits(state.plain[i]));
  }
  PutU32(out, state.hasEffectVolume ? 5u : 4u);
  PutU32(out, kInputMode);
  PutU32(out, static_cast<uint32_t>(state.settings.inputMode));
  PutU32(out, kInputGainDb);
  PutU32(out, Bits(state.settings.inputGainDb));
  PutU32(out, kOutputGainDb);
  PutU32(out, Bits(state.settings.outputGainDb));
  PutU32(out, kRestartOnStart);
  PutU32(out, state.settings.restartOnStart ? 1u : 0u);
  if (state.hasEffectVolume) {
    PutU32(out, kEffectVolumeDb);
    PutU32(out, Bits(Canonicalize(ParamId::EffectVolumeDb, state.effectVolumeDb)));
  }
}

bool DecodeState(const void* data, size_t bytes, WrapperState& out) {
  if (data == nullptr || bytes < 8) return false;
  const auto* p = static_cast<const uint8_t*>(data);
  if (std::memcmp(p, kMagic, 4) != 0) return false;
  Reader   r{p + 4, bytes - 4};
  uint32_t version = 0, count = 0;
  if (!r.U32(version) || version != kStateFormatVersion) return false;
  if (!r.U32(count) || count > r.left / 8) return false;

  WrapperState s{};
  bool         seen[kNumLeafParams] = {};
  for (size_t i = 0; i < kNumLeafParams; ++i) s.plain[i] = Canonicalize(LeafId(i), FindParam(LeafId(i))->def);
  for (uint32_t k = 0; k < count; ++k) {
    uint32_t id = 0, bits = 0;
    if (!r.U32(id) || !r.U32(bits)) return false;
    const size_t i = LeafIndex(id);
    if (i == kNumLeafParams) {
      ++s.unknownIds;
      continue;
    }
    s.plain[i] = Canonicalize(static_cast<ParamId>(id), FromBits(bits));
    seen[i]    = true;
  }
  for (bool b : seen) s.missingIds += b ? 0u : 1u;

  uint32_t settings = 0;
  if (!r.U32(settings) || settings > r.left / 8) return false;
  for (uint32_t k = 0; k < settings; ++k) {
    uint32_t key = 0, bits = 0;
    if (!r.U32(key) || !r.U32(bits)) return false;
    if (key == kInputMode) {
      s.settings.inputMode = bits == 1u ? InputMode::Stereo : InputMode::Mono;
    } else if (key == kInputGainDb) {
      s.settings.inputGainDb = CanonicalGainDb(FromBits(bits));
    } else if (key == kOutputGainDb) {
      s.settings.outputGainDb = CanonicalGainDb(FromBits(bits));
    } else if (key == kRestartOnStart) {
      s.settings.restartOnStart = bits == 1u;
    } else if (key == kEffectVolumeDb) {
      s.effectVolumeDb  = Canonicalize(ParamId::EffectVolumeDb, FromBits(bits));
      s.hasEffectVolume = true;
    }
  }
  out = s;
  return true;
}

}  // namespace brainscape::plugin
