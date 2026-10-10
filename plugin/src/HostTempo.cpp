#include "HostTempo.h"

#include <cmath>

namespace brainscape::plugin {

namespace {

uint32_t AbsDiff(uint32_t a, uint32_t b) noexcept { return a > b ? a - b : b - a; }

// The integer nearest a canonical row value in [0, max], ties away from zero: comparisons of
// the value with half-integers, which every FP environment answers alike (they are exact).
uint32_t NearestStep(float plain, uint32_t max) noexcept {
  uint32_t n = 0;
  while (n < max && plain >= static_cast<float>(n) + 0.5f) ++n;
  return n;
}

}  // namespace

TempoEvent TempoEventOf(uint32_t nsPerQuarter) noexcept {
  TempoEvent e;
  e.type = Engine::EventType::Tempo;
  e.id   = nsPerQuarter;
  return e;
}

TempoEvent TransportEventOf(tempo::TransportKind kind, bool atNextTick, uint32_t position,
                            uint32_t offset) noexcept {
  TempoEvent e;
  e.type = Engine::EventType::Transport;
  // Start and Locate carry their position; a host-style Start or Locate its anchor's offset.
  const bool positioned = kind == tempo::TransportKind::Start || kind == tempo::TransportKind::Locate;
  e.id        = tempo::TransportId(kind, atNextTick, atNextTick || !positioned ? 0u : offset);
  e.valueBits = positioned ? tempo::IntegerValueBits(position) : 0u;
  return e;
}

TempoEvent SubdivisionEventOf(tempo::SubdivField field, uint8_t code) noexcept {
  TempoEvent e;
  e.type = Engine::EventType::Subdivision;
  e.id   = tempo::SubdivisionId(field, code);
  return e;
}

uint32_t SubdivPositionOf(float plain) noexcept { return NearestStep(plain, 5u); }

uint8_t TimeModeOf(float plain) noexcept { return static_cast<uint8_t>(NearestStep(plain, 2u)); }

void HostFollower::Reset() noexcept {
  following_  = false;
  wasPlaying_ = false;
  haveGrid_   = false;
  clamped_    = false;
  octaves_    = 0;
  sentNs_     = 0;
  gridX_      = 0.0;
}

size_t HostFollower::Block(const HostTransport& t, bool follow, bool startEdge, int frames,
                           uint32_t rate, TempoEvent* out) noexcept {
  const tempo::HostTempo h =
      follow && t.hasBpm ? tempo::HostTempoFromBpm(t.bpm) : tempo::HostTempo{};
  const uint32_t ns = h.nsPerQuarter;
  if (ns == 0u) {
    // Internal, or a host that reports no tempo (the Standalone, some hosts): §10.1's Internal.
    // What was sent is forgotten, so following again sends the tempo and anchors.
    Reset();
    return 0;
  }
  octaves_ = h.octaves;
  clamped_ = h.clamped;
  // The host's position in the engine's quarters, ppq·2^-octaves: exact, at most four halvings or
  // doublings of a value HostAnchor bounds by 2^40.
  double ppq = t.hasPpq ? t.ppq : 0.0;
  for (int32_t k = 0; k < h.octaves; ++k) ppq *= 0.5;
  for (int32_t k = 0; k > h.octaves; --k) ppq *= 2.0;
  size_t     n     = 0;
  const bool start = t.playing && (startEdge || !following_);
  if (start || sentNs_ == 0u || AbsDiff(ns, sentNs_) >= kTempoHysteresisNs) {
    out[n++] = TempoEventOf(ns);
    sentNs_  = ns;
  }
  const double x = 24.0 * ppq;
  if (start) {
    uint32_t position = 0, offset = 0;
    if (!t.hasPpq || !tempo::HostAnchor(ppq, ns, rate, &position, &offset)) position = offset = 0;
    out[n++]  = TransportEventOf(tempo::TransportKind::Start, false, position, offset);
    haveGrid_ = t.hasPpq;
    gridX_    = x;
  } else if (t.playing && t.hasPpq && haveGrid_ && !h.clamped && std::fabs(x - gridX_) > 0.5) {
    uint32_t position = 0, offset = 0;
    if (tempo::HostAnchor(ppq, ns, rate, &position, &offset)) {
      out[n++] = TransportEventOf(tempo::TransportKind::Locate, false, position, offset);
      gridX_   = x;
    }
  } else if (!t.playing && wasPlaying_) {
    out[n++] = TransportEventOf(tempo::TransportKind::Stop, false);
  }
  // The engine's grid at the next block's first frame: this block's frames at the tempo it plays,
  // sentNs_, at the engine's integer rate, 24·10^9 / (ns·R) ticks a frame.
  haveGrid_ = haveGrid_ && t.playing && t.hasPpq && frames > 0 && rate > 0u;
  if (haveGrid_) {
    gridX_ += static_cast<double>(frames) * 24.0e9 /
              (static_cast<double>(sentNs_) * static_cast<double>(rate));
  }
  wasPlaying_ = t.playing;
  following_  = true;
  return n;
}

}  // namespace brainscape::plugin
