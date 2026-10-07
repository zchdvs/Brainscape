#pragma once
#include "detail/FpProfilePrivate.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "brainscape/Params.h"
#include "brainscape/Preset.h"

// Private helpers of the package code (docs/design/mode-compiler.md §5.2, §5.3). Nothing here
// does floating-point arithmetic or even moves a value through a floating-point register:
// floats are read and written as their bit patterns with memcpy, compared as the integers
// those patterns order like, and every limit is an integer constant. The M7 build of these
// objects carries no floating-point instruction (checked when lane B landed; record in
// docs/STATUS.md).
namespace brainscape::blob {

// ── Bytes ─────────────────────────────────────────────────────────────────────────────────
inline uint32_t Rd32(const uint8_t* p) noexcept {
  return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 |
         static_cast<uint32_t>(p[2]) << 16 | static_cast<uint32_t>(p[3]) << 24;
}
inline uint16_t Rd16(const uint8_t* p) noexcept {
  return static_cast<uint16_t>(static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8);
}
inline void Wr32(uint8_t* p, uint32_t v) noexcept {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
  p[2] = static_cast<uint8_t>(v >> 16);
  p[3] = static_cast<uint8_t>(v >> 24);
}
inline void Wr16(uint8_t* p, uint16_t v) noexcept {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
}
// Byte comparison as a loop: memcmp is not on the symbol audit's allowlist.
inline bool SameBytes(const void* a, const void* b, size_t n) noexcept {
  const auto* x = static_cast<const uint8_t*>(a);
  const auto* y = static_cast<const uint8_t*>(b);
  uint8_t     d = 0;
  for (size_t i = 0; i < n; ++i) d = static_cast<uint8_t>(d | (x[i] ^ y[i]));
  return d == 0;
}
inline bool AllZero(const void* a, size_t n) noexcept {
  const auto* x = static_cast<const uint8_t*>(a);
  uint8_t     d = 0;
  for (size_t i = 0; i < n; ++i) d = static_cast<uint8_t>(d | x[i]);
  return d == 0;
}

// ── Floats as bits ────────────────────────────────────────────────────────────────────────
// By reference, so the value is read from memory into an integer register.
inline uint32_t BitsOf(const float& f) noexcept {
  uint32_t u;
  std::memcpy(&u, &f, sizeof u);
  return u;
}
inline void SetBits(float* f, uint32_t u) noexcept { std::memcpy(f, &u, sizeof u); }

// What canonicalization leaves (determinism profile §3.7): finite, and neither -0 nor a
// subnormal.
inline bool Canonical(uint32_t b) noexcept {
  const uint32_t exponent = b & 0x7F800000u;
  if (exponent == 0x7F800000u) return false;
  if (exponent == 0u) return b == 0u;
  return true;
}
// An unsigned integer that orders finite binary32 values as their values do (+0 and -0 apart).
inline uint32_t OrderKey(uint32_t b) noexcept {
  return (b & 0x80000000u) != 0u ? ~b : (b | 0x80000000u);
}
inline bool InRange(uint32_t b, uint32_t lo, uint32_t hi) noexcept {
  return OrderKey(b) >= OrderKey(lo) && OrderKey(b) <= OrderKey(hi);
}
inline bool Less(uint32_t a, uint32_t b) noexcept { return OrderKey(a) < OrderKey(b); }

// The limits, as bit patterns (test_blob.cpp checks each against its float literal).
inline constexpr uint32_t kF0           = 0x00000000u;  // 0
inline constexpr uint32_t kFOne         = 0x3F800000u;  // 1
inline constexpr uint32_t kFMinusOne    = 0xBF800000u;  // -1
inline constexpr uint32_t kFCurveMin    = 0x3D800000u;  // 1/16
inline constexpr uint32_t kFCurveMax    = 0x41800000u;  // 16
inline constexpr uint32_t kFSt          = 0x41C00000u;  // 24
inline constexpr uint32_t kFMinusSt     = 0xC1C00000u;  // -24
inline constexpr uint32_t kFRearmMin    = 0x41200000u;  // 10
inline constexpr uint32_t kFRearmMax    = 0x469C4000u;  // 20000
inline constexpr uint32_t kFRearmDef    = 0x447A0000u;  // 1000
inline constexpr uint32_t kFPosSelMax   = 0x459C4000u;  // 5000
inline constexpr uint32_t kFAttackMax   = 0x459C4000u;  // 5000
inline constexpr uint32_t kFReleaseMax  = 0x469C4000u;  // 20000
inline constexpr uint32_t kFDuckAtkMin  = 0x3DCCCCCDu;  // 0.1
inline constexpr uint32_t kFDuckAtkMax  = 0x43FA0000u;  // 500
inline constexpr uint32_t kFDuckRelMin  = 0x3F800000u;  // 1
inline constexpr uint32_t kFDuckRelMax  = 0x459C4000u;  // 5000
inline constexpr uint32_t kFDuckAtkDef  = 0x40A00000u;  // 5
inline constexpr uint32_t kFDuckRelDef  = 0x42A00000u;  // 80

// ── Diagnostics ───────────────────────────────────────────────────────────────────────────
inline bool Fail(PresetDiagnostic* d, PresetError e, uint32_t detail = 0) noexcept {
  if (d != nullptr) {
    d->error  = e;
    d->detail = detail;
  }
  return false;
}

// ── Shared checks (Validate.cpp) ──────────────────────────────────────────────────────────
// The structural rules on a decoded state, in DecodePreset's order: STAT's leaves and the
// performance state, then MODE (with `supported` the feature bits accepted), then CTRL
// against MACR.
bool CheckStat(const PresetState& state, PresetDiagnostic* d) noexcept;
bool CheckMode(const ModeBlob& mode, uint32_t supported, PresetDiagnostic* d) noexcept;
bool CheckControl(const ModeBlob& mode, const ControlState& control,
                  PresetDiagnostic* d) noexcept;
// ValidateMode with another feature set: tests reach the vocabulary this build cannot play.
bool ValidateModeWith(const PresetState& state, uint32_t supported, PresetDiagnostic* d) noexcept;
// A load's step 0 (mode-compiler.md §7.3): ValidateMode's rules on the mode and CTRL, the
// structural and the semantic ones, without STAT's. The leaves are the load's step 2, which
// canonicalizes them and counts unknown, duplicate and changed ones instead of refusing them.
bool ValidateStructure(const PresetState& state, uint32_t supported, PresetDiagnostic* d) noexcept;
// Whether the element that holds leaf `id` exists in `mode` (§1.3): the second layer, a
// layer's SVF or crush, a modulator, the step table. Every other leaf's element always exists.
bool ElementPresent(const ModeBlob& mode, uint32_t id) noexcept;
// META's parser (Decode.cpp), shared with EncodeMeta: `meta` may be null.
bool ParseMeta(const uint8_t* p, uint32_t length, uint32_t base, const ModeBlob& mode,
               PresetMeta* meta, PresetDiagnostic* d) noexcept;
// DecodePreset with another feature set (tests and the fuzzers).
bool DecodePresetWith(const void* bytes, size_t length, PresetState* out, PresetDiagnostic* d,
                      PackageInfo* info, PresetMeta* meta, uint32_t supported) noexcept;
// Whether an optional chunk's content equals its absent default.
bool PitchSetsDefault(const ModeBlob& mode) noexcept;
bool DryDuckDefault(const DryDuck& duck) noexcept;

}  // namespace brainscape::blob
