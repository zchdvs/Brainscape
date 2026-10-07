#pragma once
#include <cstddef>
#include <cstdint>

#include "brainscape/FpProfile.h"
#include "brainscape/Mode.h"
#include "brainscape/PresetState.h"

namespace brainscape {

// The .bsp preset package (docs/design/mode-compiler.md §5, §6; companion-app.md §6.3): a
// 128-byte header, then sections {u32 tag, u32 length, payload, zero padding to 4}, at most
// 16,384 bytes in all; integers little-endian, floats as raw binary32 bits, every field written
// one by one. Known sections come in the order STAT, MODE, CTRL, META, JSON (STAT and MODE
// required); a section this build does not know may follow MODE anywhere and is carried
// verbatim.
//
//   offset  field                          offset  field
//   0       magic "BSPK"                   20      total_bytes (u32)
//   4       package_format (u16) = 1       24      section_count (u32)
//   6       flags (u16): FACTORY, JSON_STALE  28    reserved (u32) = 0
//   8       sound_rev (u32), never 0       32      sound_hash   = SHA-256(u32 len(STAT) || STAT
//   12      blob_format (u32) = 1                                  || u32 len(MODE) || MODE)
//   16      schema_version (u32) >= 1      64      control_hash = SHA-256(u32 len(CTRL) || CTRL)
//                                          96      package_hash = SHA-256(package, this zeroed)
//
// STAT: u32 n (<= 128), n x {u32 id, u32 bits} with ascending ids, then the performance state
//       {u8 reverse, time_mode, subdiv, tempo_source; u32 us_per_quarter}.
// MODE: u32 features, u32 chunk_count, then chunks {u32 tag, u32 length, payload} in the order
//       SCHD, LAYR, PSET, STEP, MODS, ROUT, LINK, DUCK, MACR, each at most once: SCHD, LAYR and
//       MACR always, the others exactly when used (Mode.h), at most 4 KiB.
// CTRL: u8 macro_count, u8 expr_count, u16 0; macro_count x {u32 macro_id, u32 position bits}
//       by ascending id; expr_count x {u32 target; f32 lo, hi, curve}.
// META: length-prefixed UTF-8 (str = u16 length, bytes): str id, str name, str family,
//       str author, str description; u8 tag count, that many str tags; u8 display-name count,
//       that many {u32 macro_id, str display_name} by ascending id.
// JSON: the canonical source document (§6.4), opaque here.
//
// Everything here is integer-only, allocates nothing and calls no library function but memcpy
// and memset, so the firmware links it (§5.2) and no FP environment can change a verdict:
// floats are compared as the integers their bit patterns order like.

inline constexpr uint32_t kPackageHeaderBytes = 128;
inline constexpr uint32_t kMaxPackageBytes    = 16384;  // E12
inline constexpr uint32_t kMaxModeBytes       = 4096;   // E12
inline constexpr uint16_t kPackageFormat      = 1;
inline constexpr uint32_t kBlobFormat         = 1;
inline constexpr uint32_t kSchemaVersion      = 1;  // the newest preset-document schema
inline constexpr uint16_t kPackageFlagFactory   = 1u << 0;
inline constexpr uint16_t kPackageFlagJsonStale = 1u << 1;  // pedal-side edits (companion §6.9)
inline constexpr uint16_t kPackageFlagsKnown    = kPackageFlagFactory | kPackageFlagJsonStale;

// Section and chunk tags: four ASCII bytes, read as a little-endian u32.
constexpr uint32_t PackageTag(char a, char b, char c, char d) noexcept {
  return static_cast<uint32_t>(static_cast<uint8_t>(a)) |
         static_cast<uint32_t>(static_cast<uint8_t>(b)) << 8 |
         static_cast<uint32_t>(static_cast<uint8_t>(c)) << 16 |
         static_cast<uint32_t>(static_cast<uint8_t>(d)) << 24;
}
inline constexpr uint32_t kTagStat = PackageTag('S', 'T', 'A', 'T');
inline constexpr uint32_t kTagMode = PackageTag('M', 'O', 'D', 'E');
inline constexpr uint32_t kTagCtrl = PackageTag('C', 'T', 'R', 'L');
inline constexpr uint32_t kTagMeta = PackageTag('M', 'E', 'T', 'A');
inline constexpr uint32_t kTagJson = PackageTag('J', 'S', 'O', 'N');
inline constexpr uint32_t kChunkSchd = PackageTag('S', 'C', 'H', 'D');
inline constexpr uint32_t kChunkLayr = PackageTag('L', 'A', 'Y', 'R');
inline constexpr uint32_t kChunkPset = PackageTag('P', 'S', 'E', 'T');
inline constexpr uint32_t kChunkStep = PackageTag('S', 'T', 'E', 'P');
inline constexpr uint32_t kChunkMods = PackageTag('M', 'O', 'D', 'S');
inline constexpr uint32_t kChunkRout = PackageTag('R', 'O', 'U', 'T');
inline constexpr uint32_t kChunkLink = PackageTag('L', 'I', 'N', 'K');
inline constexpr uint32_t kChunkDuck = PackageTag('D', 'U', 'C', 'K');
inline constexpr uint32_t kChunkMacr = PackageTag('M', 'A', 'C', 'R');

// META's limits (§2.2; the free-text fields are this format's).
inline constexpr uint32_t kMaxIdBytes          = 48;   // [a-z0-9._-], 1-48
inline constexpr uint32_t kMaxNameBytes        = 32;   // 1-32
inline constexpr uint32_t kMaxAuthorBytes      = 64;   // 0-64
inline constexpr uint32_t kMaxDescriptionBytes = 512;  // 0-512
inline constexpr uint32_t kMaxTags             = 8;
inline constexpr uint32_t kMaxTagBytes         = 32;  // 1-32
inline constexpr uint32_t kMaxDisplayNameBytes = 16;  // 1-16
enum class PresetFamily : uint8_t { None, Recall, Reverie, Misfire, Echoic };

// Why a package or a state was refused: one code per rule (§5.3). `detail` names the culprit
// where there is one: a tag, feature bits, a parameter or macro id, an index.
enum class PresetError : uint8_t {
  None = 0,
  // Header and container
  Truncated,          // shorter than the header
  TooLarge,           // over 16,384 bytes
  Magic,
  PackageFormat,      // detail: the format
  HeaderFlags,        // detail: the unknown bits
  TotalBytes,         // total_bytes is not the length
  HeaderReserved,
  BlobFormat,         // detail: the format
  SchemaVersion,      // 0
  SoundRevision,      // sound_rev 0
  PackageHash,
  SectionBounds,      // a section past the end, or bytes after the last
  SectionPadding,     // a nonzero padding byte
  SectionOrder,       // STAT, MODE first; CTRL, META, JSON in order. detail: the tag
  SectionDuplicate,   // detail: the tag
  SectionMissing,     // detail: the tag
  SoundHash,
  ControlHash,
  // STAT
  StatLength,
  StatCount,          // over 128 leaves
  StatOrder,          // ids not strictly ascending. detail: the id
  StatValue,          // NaN, infinite, -0 or subnormal. detail: the id
  Performance,        // a performance field outside its enumeration or range
  StatPadding,        // a nonzero leaf slot past the count (a state built in memory). detail:
                      // the slot
  // MODE
  ModeLength,
  ModeTooLarge,       // over 4 KiB
  ChunkOrder,         // a known chunk out of order or repeated. detail: the tag
  ChunkMissing,       // SCHD, LAYR or MACR absent. detail: the tag
  ChunkLength,        // detail: the tag
  ChunkCount,         // chunk_count is not the number of chunks
  ChunkDefault,       // a present optional chunk equal to its absent default. detail: the tag
  ModePadding,        // a nonzero pad, or a nonzero entry past a count
  ModeCount,          // a count outside its cap
  ModeEnum,           // an enumeration value outside the vocabulary
  ModeValue,          // a float NaN, infinite, -0 or subnormal
  ModeRange,          // a value outside its range
  MacroLayout,        // macro ids not ascending within 69-76, or `first`/`target_count` wrong
  FeatureMismatch,    // `features` is not what the content requires. detail: the difference
  UnsupportedFeature, // detail: the unknown chunk tag, or the feature bits this build lacks
  UnsupportedTarget,  // a macro target (ValidateMode) or expression target (CTRL) on a Reserved
                      // row: a later wave's ID, newer content rather than a corrupt one.
                      // detail: the id
  // CTRL
  CtrlLength,
  CtrlPositions,      // not one position per defined macro, by ascending id. detail: the id
  CtrlValue,          // a position or float not canonical, or outside its range
  CtrlCount,          // over 4 assignments
  CtrlPadding,
  // META
  MetaLength,
  MetaText,           // invalid UTF-8, a control character, or a length outside its limits
  MetaFamily,
  MetaDisplayName,    // for an undefined macro, or not by ascending id. detail: the id
  MetaDuplicate,      // a repeated tag
  // Semantic (ValidateMode, E8-E11)
  TargetNotLeaf,      // E8: a macro target that is not a Leaf row (nor a Reserved one,
                      // UnsupportedTarget). detail: the id
  TargetMix,          // E8: global.mix as a macro target
  TargetAbsent,       // E8: a target in an absent element. detail: the id
  TargetDuplicate,    // E8: a leaf targeted twice by one macro. detail: the id
  TargetRange,        // E9: a range end outside the target's range. detail: the id
  ShapeEmpty,         // E10: a defined shape macro without targets
  StepRatioIndex,     // E8: ratio_idx at or past the pitch set's count
  RouteEndpoint,      // E8: a route or link endpoint that does not exist
  LayerBudget,        // E11: slot shares over 1, or voice_count maxima over 64
  AbsentLeaf,         // a leaf of an absent element away from its default. detail: the id
  ExpressionTarget,   // CTRL: not a Leaf or Macro row (nor a Reserved one, UnsupportedTarget;
                      // or, ValidateMode, a leaf of an absent element). detail: the id
  ExpressionRange,    // CTRL: lo or hi outside the target's range. detail: the id
  kCount
};
const char* PresetErrorName(PresetError error) noexcept;

struct PresetDiagnostic {
  PresetError error  = PresetError::None;
  uint32_t    detail = 0;
};

// Where a section's payload lies in the package (length 0 and offset 0 when absent).
struct SectionSpan {
  uint32_t offset = 0;
  uint32_t length = 0;
};

// The header and the section map of a decoded package.
struct PackageInfo {
  uint16_t    packageFormat = 0;
  uint16_t    flags         = 0;
  uint32_t    soundRev      = 0;
  uint32_t    blobFormat    = 0;
  uint32_t    schemaVersion = 0;
  uint32_t    totalBytes    = 0;
  uint32_t    sectionCount  = 0;
  Digest32    soundHash;
  Digest32    controlHash;
  Digest32    packageHash;
  SectionSpan stat, mode, ctrl, meta, json;
  uint32_t    unknownSections = 0;  // carried, not read
};

// META, decoded as views into the package bytes (offset and length), so names need no copy.
struct PresetMeta {
  SectionSpan  id, name, author, description;
  PresetFamily family           = PresetFamily::None;
  uint8_t      tagCount         = 0;
  uint8_t      displayNameCount = 0;
  uint8_t      pad              = 0;
  SectionSpan  tags[kMaxTags];
  uint32_t     displayMacro[kMaxMacros] = {};  // ascending macro ids
  SectionSpan  displayName[kMaxMacros];
};

// Decodes and structurally validates a package (§5.3): the header, the sections and their
// order, the three hashes, STAT, MODE (chunk order and lengths, enumerations, counts, ranges,
// canonical floats, zero padding and zero entries past every count, macro layout, `features`
// equal to what the content requires and supported by this build), CTRL (positions against
// MACR, expression targets that are Leaf or Macro rows with both ends in range), and META
// (display names against MACR). It sets out->mode.modeHash to the SHA-256 of the MODE
// payload. Out of range is not invalid for a leaf: a known leaf outside this build's range
// decodes, and LoadPreset canonicalizes it and reports the load inexact. Semantic rules are
// ValidateMode's. `out` must not be null; on failure *out is unspecified. Returns true when the
// package decoded.
bool DecodePreset(const void* bytes, size_t length, PresetState* out,
                  PresetDiagnostic* diagnostic = nullptr, PackageInfo* info = nullptr,
                  PresetMeta* meta = nullptr) noexcept;

// Every check DecodePreset makes on the decoded structure (so a state built in memory is held
// to the same rules), then the semantic ones: E8-E11 (macro and expression targets, the shape
// macro, step ratio indices, route endpoints, the layer budget) and that the leaves of absent
// elements hold their defaults. Integer-only; no sample rate, since the engine runs at 48 kHz.
// LoadPreset runs it on every load (§7.3, sound revision 2), the compiler on every package.
bool ValidateMode(const PresetState& state, PresetDiagnostic* diagnostic = nullptr) noexcept;

// ── Encoding (compiler, pedal-side edits, tests) ──────────────────────────────────────────
// Writes a section payload for a state that passes DecodePreset's structural rules (features
// this build lacks included); false, with the diagnostic, if it does not or `capacity` is too
// small. A state has one encoding, so decoding then encoding gives back the same bytes.
bool EncodeStat(const PresetState& state, uint8_t* out, size_t capacity, uint32_t* written,
                PresetDiagnostic* diagnostic = nullptr) noexcept;
bool EncodeMode(const ModeBlob& mode, uint8_t* out, size_t capacity, uint32_t* written,
                PresetDiagnostic* diagnostic = nullptr) noexcept;
// CTRL's payload; nothing (0 bytes) when control.present is 0.
bool EncodeControl(const ModeBlob& mode, const ControlState& control, uint8_t* out,
                   size_t capacity, uint32_t* written,
                   PresetDiagnostic* diagnostic = nullptr) noexcept;

// META's fields as text (pointer and length, not NUL-terminated).
struct MetaText {
  const char* data   = nullptr;
  uint32_t    length = 0;
};
struct MetaContent {
  MetaText     id, name, author, description;
  PresetFamily family           = PresetFamily::None;
  uint8_t      tagCount         = 0;
  uint8_t      displayNameCount = 0;
  MetaText     tags[kMaxTags];
  uint32_t     displayMacro[kMaxMacros] = {};
  MetaText     displayName[kMaxMacros];
};
// META's payload under DecodePreset's rules, the display names checked against `mode`.
bool EncodeMeta(const MetaContent& meta, const ModeBlob& mode, uint8_t* out, size_t capacity,
                uint32_t* written, PresetDiagnostic* diagnostic = nullptr) noexcept;

// What a new package carries besides the state.
struct PackageContent {
  uint16_t       flags         = 0;
  uint32_t       schemaVersion = kSchemaVersion;
  const uint8_t* meta          = nullptr;  // META's payload, or none
  uint32_t       metaLength    = 0;
  const uint8_t* json          = nullptr;  // the JSON section, or none
  uint32_t       jsonLength    = 0;
};
// A complete package for `state` (its soundRev, which must not be 0, goes in the header):
// STAT, MODE, CTRL when present, META, JSON, every hash. Returns the package's length, or 0
// with the diagnostic.
size_t EncodePackage(const PresetState& state, const PackageContent& content, uint8_t* out,
                     size_t capacity, PresetDiagnostic* diagnostic = nullptr) noexcept;

// `state` written into the container of `original`, a package DecodePreset accepts: its
// flags (with `addFlags` set, such as JSON_STALE after a pedal-side edit, companion §6.9),
// schema version, META, JSON and unknown sections are carried byte for byte and in their
// order, and STAT, MODE, CTRL (inserted after MODE, or dropped, as control.present says),
// sound_rev and the hashes come from `state`. META's display names are checked against the
// state's macros. Re-encoding a decoded package gives back its bytes. Returns the length, or 0.
size_t ReencodePackage(const PresetState& state, const uint8_t* original, size_t originalLength,
                       uint16_t addFlags, uint8_t* out, size_t capacity,
                       PresetDiagnostic* diagnostic = nullptr) noexcept;

// The package's hashes over encoded payloads (§6.3).
void SoundHash(const uint8_t* stat, uint32_t statLength, const uint8_t* mode, uint32_t modeLength,
               Digest32* out) noexcept;
void ControlHash(const uint8_t* ctrl, uint32_t ctrlLength, Digest32* out) noexcept;
// SHA-256 of `mode` encoded: what DecodePreset stores in modeHash. False if it cannot encode.
bool ComputeModeHash(const ModeBlob& mode, Digest32* out) noexcept;

}  // namespace brainscape
