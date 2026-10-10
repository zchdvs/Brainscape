#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "brainscape/Mode.h"
#include "brainscape/Params.h"

namespace brainscape::plugin {

enum class InputMode : uint32_t { Mono = 0, Stereo = 1 };  // Mono: R := L (companion §4.8)

// Where the engine's tempo comes from in a plugin (docs/design/clock.md §10.1, D9): the host's
// tempo and transport (the default), or the internal tempo alone (taps, typed tempos, the
// preset's). Under Host, a host that reports no tempo (the Standalone, some hosts) is Internal.
enum class TempoSource : uint32_t { Host = 0, Internal = 1 };

// Global wrapper settings, saved with the session but never part of a preset (§4.8, §6.9).
// The processor starts the Standalone in Mono (BrainscapeProcessor's constructor).
struct WrapperSettings {
  InputMode   inputMode      = InputMode::Stereo;
  float       inputGainDb    = 0.f;
  float       outputGainDb   = 0.f;
  bool        restartOnStart = false;  // "Restart on transport start" (§4.9), off by default
  // The tempo core's device settings (clock.md §10.1, §10.2, §10.4): the Tempo source; "Receive
  // MIDI clock" (on by default), which gates the MIDI clock bytes the Standalone's MIDI input
  // brings to the translator; and global.tempo_recall (row 85: Keep, or Preset), which the engine
  // reads at a Spillover load.
  TempoSource tempoSource       = TempoSource::Host;
  bool        receiveMidiClock  = true;
  bool        tempoRecallPreset = false;
};
inline constexpr float kWrapperGainRangeDb = 24.f;

// DAW session state (companion §6.9, before .bsp packages exist): every leaf as exact
// binary32 bits plus the wrapper settings. Little-endian fields written one by one, never
// a struct. Layout v1:
//   "BSWS"  u32 version=1  u32 n  n x {u32 ParamId, u32 bits}  u32 m  m x {u32 key, u32 bits}
// Readers skip setting keys they do not know, so a setting is added without a new version.
// Freeze is a performance state and is never stored (companion §6.2).
// The leaves are the Leaf rows (mode-compiler.md §4.1), by ordinal: sessions written before
// the ID table (IDs 1-28, all still Leaf rows) decode unchanged. An id that is not a Leaf
// row of this build (a Macro, Performance, Global, Reserved or Retired row, or one it lacks)
// is unknown. BSWS v2, a .bsp plus wrapper settings, replaces this layout and migrates v1
// sessions to preset documents (mode-compiler.md §4.4, §9.2).
//
// Blocks may follow the settings, {u32 tag, u32 length, payload, zero padding to 4}; a reader
// skips a tag it does not know, and readers written before blocks existed stop after the
// settings, so every v1 reader takes a session with blocks. One block so far:
//   "FMOD"  the factory mode the session played (FactoryModes.h, the editor's Modes menu):
//           u32 idLength (1-48), the document id's bytes, zero padding to 4; u8[32] its
//           package_hash; u32 m (<= 8), m x {u32 macro id, u32 position bits}, the macro mirrors
//           (CTRL's positions, pickup references, mode-compiler.md §3.5).
// Bytes after the settings that do not read as whole blocks are ignored, and said so
// (unreadTail), as readers before blocks ignored them; an FMOD block that does not read is
// ignored the same way. A session without one plays the default mode, as every v1 session did.
//
// Setting keys: 1 input mode, 2 input gain, 3 output gain, 4 restart on transport start, 5 the
// effect volume; and the tempo core's (clock.md §10.1, §10.4): 6 the Tempo source (0 Host,
// 1 Internal), 7 Receive MIDI clock, 8 global.tempo_recall (0 Keep, 1 Preset), 9 the last
// committed tempo in ns per quarter (the internal tempo the session keeps across relaunch, project
// reload and device changes; written only in range), 10 and 11 rows 83 and 84, perf.subdiv's
// position and perf.time_mode, as canonical binary32 (the live Subdiv and time mode the session's
// restart re-asserts). A session without 9-11 restores the preset's stored ones.
struct WrapperFactoryMode {
  std::string id;                          // "factory.lull"
  uint8_t     packageHash[32] = {};        // the package the session played
  uint32_t    macroCount      = 0;
  uint32_t    macroIds[kMaxMacros]  = {};  // Macro rows (69-76); the reader drops other ids
  float       positions[kMaxMacros] = {};  // canonicalized when written and read
};

struct WrapperState {
  float           plain[kNumLeafParams];
  WrapperSettings settings;
  // global.effect_volume_db (mode-compiler.md §3.8), a device setting outside presets: setting
  // key 5, absent from sessions written before it.
  float           effectVolumeDb  = 0.f;
  bool            hasEffectVolume = false;
  // The tempo core's performance (setting keys 9-11): the committed tempo, 0 when absent; and
  // rows 83 and 84, canonical, when hasPerformance.
  uint32_t        tempoNs        = 0;
  bool            hasPerformance = false;
  float           subdivPosition = 2.f;  // TAP
  float           timeMode       = 0.f;  // Free
  uint32_t        unknownIds = 0;  // ids in the blob this build lacks (ignored)
  uint32_t        missingIds = 0;  // ids this build has that the blob lacks (defaults)
  bool               hasFactory = false;  // an FMOD block was read (written when set)
  WrapperFactoryMode factory;
  bool               unreadTail = false;  // bytes after the settings were ignored (decoding)
};

inline constexpr uint32_t kStateFormatVersion = 1;

void EncodeState(const WrapperState& state, std::vector<uint8_t>& out);
// Complete-state semantics: ids absent from the blob load their defaults. Values are
// canonicalized, so canonical values round-trip bit for bit. Returns false (and leaves
// `out` untouched) for a malformed or newer blob.
bool DecodeState(const void* data, size_t bytes, WrapperState& out);

float CanonicalGainDb(float db) noexcept;

}  // namespace brainscape::plugin
