#pragma once
#include <cstddef>
#include <cstdint>

#include "brainscape/Engine.h"
#include "brainscape/Tempo.h"

// The plugin host's tempo and transport as engine events (docs/design/clock.md §4.4, §10.1): what
// the wrapper reads from the host's playhead once per host block, and the Tempo and Transport
// events it sends at the block's first frame while the Tempo source is Host and the host reports a
// tempo. JUCE-free, so the tests drive it block by block; every frame it computes comes from
// dsp/'s guarded conversions (NsPerQuarterFromBpm, HostAnchor), so every build sends the same.
namespace brainscape::plugin {

// One host block's playhead (JUCE's AudioPlayHead::PositionInfo, each field optional).
struct HostTransport {
  bool   playing = false;
  bool   hasBpm  = false;
  double bpm     = 0.0;
  bool   hasPpq  = false;
  double ppq     = 0.0;  // quarter notes at the block's first frame
};

// An event 6-10 as the engine takes it: its type, id and value bits (Tempo.h's payloads).
struct TempoEvent {
  Engine::EventType type      = Engine::EventType::Tempo;
  uint32_t          id        = 0;
  uint32_t          valueBits = 0;
};

TempoEvent TempoEventOf(uint32_t nsPerQuarter) noexcept;
TempoEvent TransportEventOf(tempo::TransportKind kind, bool atNextTick, uint32_t position = 0,
                            uint32_t offset = 0) noexcept;
TempoEvent SubdivisionEventOf(tempo::SubdivField field, uint8_t code) noexcept;

// Rows 83 and 84 (perf.subdiv, perf.time_mode, §10.4) hold canonical plain values, positions
// 0-5 and modes 0-2: the integer each rounds to (half away from zero, as the engine reads counting
// leaves), clamped to the row's range.
uint32_t SubdivPositionOf(float plain) noexcept;
uint8_t  TimeModeOf(float plain) noexcept;

// §4.4, one host block at a time. Following means the Tempo source is Host and the host reports
// a tempo; then, at the block's first frame:
//   1. a Tempo event when the host's tempo in ns differs from the last one sent by 1,000 or more
//      (1 µs per quarter: a host's rounding noise never moves the output), and always at a start;
//      a tempo outside 20-300 BPM is folded by octaves into it (HostTempoFromBpm) and the host's
//      position read in the folded quarters, so the grid stays on the host's beats;
//   2. at a transport start (CheckTransportStart's edge, or following that begins while the host
//      plays), after the Tempo, Transport Start carrying HostAnchor's position and frame offset,
//      or position 0 and offset 0 when the host reports no ppq;
//   3. while playing, Transport Locate, anchored as a Start, when the reported position is more
//      than half a tick from the engine's grid: the last anchor's position advanced by every block
//      since at the tempo the engine was sent, at its integer rate. That catches a loop wrap and a
//      jump, and the offset a tempo ramp or a tempo change inside a block leaves, since the engine
//      plays a block at the tempo of its first frame (clock.md §11.16). Not while the tempo is
//      clamped, whose grid cannot follow the host's beats;
//   4. Transport Stop at the first block that does not play.
// Host-style transports: no AtNextTick (Tempo.h). Reset() forgets what was sent, so the next block
// that follows sends its tempo and, playing, anchors with a Start: the re-asserts after every
// Exact load (§10.1).
class HostFollower {
 public:
  static constexpr size_t   kMaxEvents         = 2;  // a Tempo, then a Transport
  static constexpr uint32_t kTempoHysteresisNs = 1000;

  void Reset() noexcept;
  // The next block that follows sends the host's tempo again, keeping the transport's state: after
  // a Spillover load, which may have played its stored tempo (recall Preset).
  void ResendTempo() noexcept { sentNs_ = 0; }
  // `follow`: the Tempo source is Host. `startEdge`: a playing block after a non-playing one,
  // prepareToPlay or a switch to offline (the restart option's edge). `frames`: the block's
  // length; `rate`: the engine's integer rate, at which its grid advances.
  // Writes the block's events to `out` (kMaxEvents of room) and returns their count.
  size_t Block(const HostTransport& t, bool follow, bool startEdge, int frames, uint32_t rate,
               TempoEvent* out) noexcept;
  // The last block followed the host: taps and Tempo events from other producers are dropped
  // (§10.1), and the BPM panel says HOST.
  bool     Following() const noexcept { return following_; }
  uint32_t SentNs() const noexcept { return sentNs_; }
  // The fold of the host's tempo in the last block followed (HostTempoFromBpm): one engine quarter
  // is 2^Octaves() host beats. Clamped: the host's tempo is out of reach and the grid off its
  // beats.
  int32_t  Octaves() const noexcept { return octaves_; }
  bool     Clamped() const noexcept { return clamped_; }

 private:
  bool     following_  = false;
  bool     wasPlaying_ = false;
  bool     haveGrid_   = false;  // the engine's grid follows an anchor of this transport
  bool     clamped_    = false;
  int32_t  octaves_    = 0;
  uint32_t sentNs_     = 0;    // 0: nothing sent since the last Reset
  double   gridX_      = 0.0;  // the engine's grid at the next block's first frame, in ticks
};

}  // namespace brainscape::plugin
