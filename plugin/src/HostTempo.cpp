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
  following_      = false;
  wasPlaying_     = false;
  havePrediction_ = false;
  sentNs_         = 0;
  predicted_      = 0.0;
}

size_t HostFollower::Block(const HostTransport& t, bool follow, bool startEdge, int frames,
                           uint32_t rate, double hostRate, TempoEvent* out) noexcept {
  const uint32_t ns = follow && t.hasBpm ? tempo::NsPerQuarterFromBpm(t.bpm) : 0u;
  if (ns == 0u) {
    // Internal, or a host that reports no tempo (the Standalone, some hosts): §10.1's Internal.
    // What was sent is forgotten, so following again sends the tempo and anchors.
    Reset();
    return 0;
  }
  size_t     n     = 0;
  const bool start = t.playing && (startEdge || !following_);
  if (start || sentNs_ == 0u || AbsDiff(ns, sentNs_) >= kTempoHysteresisNs) {
    out[n++] = TempoEventOf(ns);
    sentNs_  = ns;
  }
  const double x = t.hasPpq ? 24.0 * t.ppq : 0.0;
  if (start) {
    uint32_t position = 0, offset = 0;
    if (!t.hasPpq || !tempo::HostAnchor(t.ppq, ns, rate, &position, &offset)) position = offset = 0;
    out[n++] = TransportEventOf(tempo::TransportKind::Start, false, position, offset);
  } else if (t.playing && t.hasPpq && havePrediction_ && std::fabs(x - predicted_) > 0.5) {
    uint32_t position = 0, offset = 0;
    if (tempo::HostAnchor(t.ppq, ns, rate, &position, &offset)) {
      out[n++] = TransportEventOf(tempo::TransportKind::Locate, false, position, offset);
    }
  } else if (!t.playing && wasPlaying_) {
    out[n++] = TransportEventOf(tempo::TransportKind::Stop, false);
  }
  // This block's x advanced by its frames at its tempo: where the next block should start.
  havePrediction_ = t.playing && t.hasPpq && frames > 0 && hostRate > 0.0;
  if (havePrediction_) predicted_ = x + static_cast<double>(frames) * (t.bpm * 24.0 / (60.0 * hostRate));
  wasPlaying_ = t.playing;
  following_  = true;
  return n;
}

}  // namespace brainscape::plugin
