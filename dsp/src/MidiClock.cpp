#include "brainscape/MidiClock.h"

// Integer-only (docs/design/clock.md §4.3): producer code, never called by Process, kept out of
// the pedal's ITCM (§9.6; firmware/CMakeLists.txt).
namespace brainscape {
namespace {

MidiClockEvent Transport(tempo::TransportKind kind, uint32_t position) noexcept {
  MidiClockEvent e;
  e.type      = tempo::kEventTransport;
  e.id        = tempo::TransportId(kind, /*atNextTick=*/true);
  e.valueBits = tempo::IntegerValueBits(position);
  return e;
}

}  // namespace

MidiClockParser::Result MidiClockParser::Feed(uint8_t byte, MidiClockEvent* event) noexcept {
  if (byte >= 0xF8) {  // real time: anywhere, the F2 state untouched
    switch (byte) {
      case 0xF8:
        if (running_ && positionKnown_) position_ = (position_ + 1u) % tempo::kPositionModulus;
        *event      = MidiClockEvent();
        event->type = tempo::kEventClockTick;
        return Result::Event;
      case 0xFA:
        position_      = 0;
        positionKnown_ = true;
        running_       = true;
        *event         = Transport(tempo::TransportKind::Start, 0);
        return Result::Event;
      case 0xFB:
        running_ = true;
        *event   = Transport(tempo::TransportKind::Continue, 0);
        return Result::Event;
      case 0xFC:
        running_ = false;
        *event   = Transport(tempo::TransportKind::Stop, 0);
        return Result::Event;
      default:  // F9, FD, FE (Active Sensing), FF
        return Result::Forward;
    }
  }
  if (byte >= 0x80) {  // any other status byte aborts the F2 state
    if (byte == 0xF2) {
      f2State_ = 1;
      return Result::Consumed;
    }
    f2State_ = 0;
    return Result::Forward;
  }
  // A data byte.
  if (f2State_ == 1) {
    lsb_     = byte;
    f2State_ = 2;
    return Result::Consumed;
  }
  if (f2State_ == 2) {
    f2State_ = 0;
    // At most 16,383 sixteenths, 98,298 ticks: inside the position range.
    position_      = 6u * (static_cast<uint32_t>(lsb_) + 128u * static_cast<uint32_t>(byte));
    positionKnown_ = true;
    *event         = Transport(tempo::TransportKind::Locate, position_);
    return Result::Event;
  }
  return Result::Forward;
}

void MidiClockParser::Reset() noexcept {
  f2State_       = 0;
  lsb_           = 0;
  positionKnown_ = false;
  running_       = false;
  position_      = 0;
}

}  // namespace brainscape
