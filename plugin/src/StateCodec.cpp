#include "StateCodec.h"

#include <algorithm>
#include <cstring>
#include <utility>

#include "brainscape/ParamDisplay.h"

namespace brainscape::plugin {

namespace {

constexpr uint8_t kMagic[4] = {'B', 'S', 'W', 'S'};

enum SettingKey : uint32_t {
  kInputMode = 1, kInputGainDb = 2, kOutputGainDb = 3, kRestartOnStart = 4, kEffectVolumeDb = 5
};

// The blocks after the settings (StateCodec.h): tags as four ASCII bytes, read little-endian.
constexpr uint32_t Tag(char a, char b, char c, char d) {
  return static_cast<uint32_t>(static_cast<uint8_t>(a)) | static_cast<uint32_t>(static_cast<uint8_t>(b)) << 8 |
         static_cast<uint32_t>(static_cast<uint8_t>(c)) << 16 | static_cast<uint32_t>(static_cast<uint8_t>(d)) << 24;
}
constexpr uint32_t kTagFactoryMode = Tag('F', 'M', 'O', 'D');
constexpr uint32_t kMaxIdBytes     = 48;  // a document id (Preset.h kMaxIdBytes)

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

void PutPadding(std::vector<uint8_t>& out, size_t n) {
  while (n % 4u != 0u) {
    out.push_back(0u);
    ++n;
  }
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
  bool Bytes(size_t n, const uint8_t** at) {
    if (left < n) return false;
    *at = p;
    p += n;
    left -= n;
    return true;
  }
};

size_t Padded(size_t n) { return (n + 3u) & ~static_cast<size_t>(3u); }

// An FMOD payload (StateCodec.h); false when it does not read whole.
bool ReadFactoryMode(const uint8_t* payload, size_t length, WrapperFactoryMode& out) {
  Reader             r{payload, length};
  WrapperFactoryMode m;
  uint32_t           idLength = 0;
  const uint8_t*     at       = nullptr;
  if (!r.U32(idLength) || idLength == 0u || idLength > kMaxIdBytes || !r.Bytes(Padded(idLength), &at)) return false;
  for (uint32_t i = 0; i < idLength; ++i) {
    const uint8_t c = at[i];
    const bool    ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
    if (!ok) return false;
  }
  for (size_t i = idLength; i < Padded(idLength); ++i) {
    if (at[i] != 0u) return false;
  }
  m.id.assign(reinterpret_cast<const char*>(at), idLength);
  if (!r.Bytes(sizeof m.packageHash, &at)) return false;
  std::memcpy(m.packageHash, at, sizeof m.packageHash);
  uint32_t count = 0;
  if (!r.U32(count) || count > kMaxMacros) return false;
  for (uint32_t k = 0; k < count; ++k) {
    uint32_t id = 0, bits = 0;
    if (!r.U32(id) || !r.U32(bits)) return false;
    // A macro row's position, canonical; an entry of another row is dropped.
    if (id < static_cast<uint32_t>(ParamId::MacroActivity) || id > static_cast<uint32_t>(ParamId::MacroAux2)) continue;
    m.macroIds[m.macroCount]  = id;
    m.positions[m.macroCount] = Canonicalize(static_cast<ParamId>(id), FromBits(bits));
    ++m.macroCount;
  }
  if (r.left != 0u) return false;
  out = std::move(m);
  return true;
}

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
  if (state.hasFactory) {
    const WrapperFactoryMode& m      = state.factory;
    const size_t              idSize = std::min<size_t>(m.id.size(), kMaxIdBytes);
    const uint32_t            count  = std::min<uint32_t>(m.macroCount, kMaxMacros);
    PutU32(out, kTagFactoryMode);
    PutU32(out, static_cast<uint32_t>(4u + Padded(idSize) + sizeof m.packageHash + 4u + 8u * count));
    PutU32(out, static_cast<uint32_t>(idSize));
    out.insert(out.end(), m.id.begin(), m.id.begin() + static_cast<std::ptrdiff_t>(idSize));
    PutPadding(out, idSize);
    out.insert(out.end(), m.packageHash, m.packageHash + sizeof m.packageHash);
    PutU32(out, count);
    for (uint32_t k = 0; k < count; ++k) {
      PutU32(out, m.macroIds[k]);
      PutU32(out, Bits(Canonicalize(static_cast<ParamId>(m.macroIds[k]), m.positions[k])));
    }
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
  // The blocks. What does not read whole is ignored, as readers before blocks ignored it.
  while (r.left > 0u) {
    uint32_t       tag = 0, length = 0;
    const uint8_t* payload = nullptr;
    if (!r.U32(tag) || !r.U32(length) || length > r.left || !r.Bytes(Padded(length), &payload)) {
      s.unreadTail = true;
      break;
    }
    if (tag == kTagFactoryMode && !s.hasFactory) {
      if (ReadFactoryMode(payload, length, s.factory)) {
        s.hasFactory = true;
      } else {
        s.factory    = WrapperFactoryMode{};
        s.unreadTail = true;
      }
    }
  }
  out = std::move(s);
  return true;
}

}  // namespace brainscape::plugin
