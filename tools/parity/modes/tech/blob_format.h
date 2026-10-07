#pragma once
// PROBE (scratch): a .bsp package sketch, blob_format 1. Byte layout is the contract;
// no struct is ever memcpy'd to or from bytes. Integers little-endian, floats as raw
// binary32 bits, every section 4-byte aligned with zero padding.
//
//   header (96 B)
//     0  magic 'B''S''P''K'      4  u16 package_format   6  u16 flags
//     8  u32 sound_rev          12  u32 blob_format     16  u32 schema_version
//    20  u32 total_bytes        24  u32 section_count   28  u32 reserved (0)
//    32  u8[32] sound_hash  = SHA-256(STAT payload || MODE payload)
//    64  u8[32] package_hash = SHA-256(whole package with these 32 bytes zeroed)
//   sections: u32 tag (fourcc LE), u32 length, payload, zero pad to 4
//     STAT  u32 n, n x {u32 id, u32 bits} strictly ascending ids; then perf state:
//           u8 global_reverse, u8 time_mode, u8 subdiv, u8 tempo_source, u32 us_per_quarter
//     MODE  u8 layer_count, u8 pitch_count, u8 macro_count, u8 target_count,
//           u8 sched_sources, u8 subdiv, u16 burst_count, u32 burst_spacing_ms(f32)
//           2 x layer {u8 pos_source, u8 size_law, u8 select, u8 quantize, u16 scale_mask,
//                      u8 pitch_first, u8 pitch_n, u8 mod_op[2], u8 mod_band[2],
//                      f32 glide_st_start, f32 glide_st_end, f32 glide_curve}
//           16 x pitch {f32 st, f32 weight}
//           8 x macro {u8 id, u8 first, u8 n, u8 0}
//           32 x target {u32 param, f32 lo, f32 hi, f32 in_lo, f32 in_hi, f32 curve}
//           (fixed capacity: unused slots zero, so the length is a constant)
//     JSON  canonical source text (UTF-8), META, CTRL: opaque to this probe
#include <cstddef>
#include <cstdint>

namespace bsp {

constexpr uint32_t kHeaderBytes   = 96;
constexpr uint16_t kPackageFormat = 1;
constexpr uint32_t kBlobFormat    = 1;
constexpr uint32_t kMaxLayers = 2, kMaxPitch = 16, kMaxMacros = 8, kMaxTargets = 32, kMaxLeaves = 128;
constexpr uint32_t kLayerBytes  = 24;
constexpr uint32_t kModeBytes   = 12 + kMaxLayers * kLayerBytes + kMaxPitch * 8 + kMaxMacros * 4 + kMaxTargets * 24;

constexpr uint32_t Tag(char a, char b, char c, char d) {
  return uint32_t(uint8_t(a)) | uint32_t(uint8_t(b)) << 8 | uint32_t(uint8_t(c)) << 16 | uint32_t(uint8_t(d)) << 24;
}
constexpr uint32_t kTagStat = Tag('S', 'T', 'A', 'T'), kTagMode = Tag('M', 'O', 'D', 'E'),
                   kTagJson = Tag('J', 'S', 'O', 'N'), kTagMeta = Tag('M', 'E', 'T', 'A');

// The decoded, in-memory form used by dsp/: fixed-width members only, so its layout is the
// same on x64 and the M7 (checked by static_assert), but it is never serialized.
struct Leaf { uint32_t id; float value; };
struct Layer {
  uint8_t posSource, sizeLaw, select, quantize;
  uint16_t scaleMask;
  uint8_t pitchFirst, pitchN, modOp[2], modBand[2];
  float glideStStart, glideStEnd, glideCurve;
};
struct Pitch { float st, weight; };
struct Macro { uint8_t id, first, n, pad; };
struct Target { uint32_t param; float lo, hi, inLo, inHi, curve; };
struct DecodedPackage {
  uint32_t soundRev, blobFormat, schemaVersion;
  uint8_t  soundHash[32];
  uint32_t leafCount;
  Leaf     leaves[kMaxLeaves];
  uint8_t  globalReverse, timeMode, subdiv, tempoSource;
  uint32_t usPerQuarter;
  uint8_t  layerCount, pitchCount, macroCount, targetCount, schedSources, modeSubdiv;
  uint16_t burstCount;
  float    burstSpacingMs;
  Layer    layers[kMaxLayers];
  Pitch    pitch[kMaxPitch];
  Macro    macros[kMaxMacros];
  Target   targets[kMaxTargets];
};
static_assert(sizeof(Layer) == 24, "layout");
static_assert(offsetof(DecodedPackage, leaves) == 48, "layout");
static_assert(offsetof(DecodedPackage, burstSpacingMs) == 1088, "layout");
static_assert(sizeof(DecodedPackage) == 2068, "layout");

enum class Error : uint8_t {
  None, Truncated, Magic, PackageFormat, BlobFormat, TotalBytes, SectionBounds, SectionDup,
  SectionMissing, PackageHash, SoundHash, StatCount, StatOrder, StatNonFinite, StatNonCanonical,
  PerfRange, ModeLength, ModeCount, ModeEnum, ModeIndex, ModeNonFinite, ModeRange, ModePadding,
  kCount
};
const char* ErrorName(Error) noexcept;

// dsp/ side: structural validation and decode; no allocation, no libc beyond memcpy.
// skipHashes is a fuzzing hook only (lets mutated packages reach the inner validators).
Error Decode(const uint8_t* bytes, size_t n, DecodedPackage* out, bool skipHashes = false) noexcept;

// SHA-256, integer-only (dsp/ needs it to verify package_hash on the pedal).
void Sha256(const uint8_t* const* parts, const size_t* lens, int count, uint8_t out[32]) noexcept;

}  // namespace bsp
