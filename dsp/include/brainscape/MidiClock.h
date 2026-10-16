#pragma once
#include <cstdint>

#include "brainscape/FpProfile.h"
#include "brainscape/Tempo.h"

namespace brainscape {

// One clock event as the translator makes it: a ClockTick or a Transport (docs/design/clock.md
// §4.1), with its value as binary32 bits (+0, or a Locate's position).
struct MidiClockEvent {
  uint8_t  type      = 0;  // tempo::kEventClockTick or tempo::kEventTransport
  uint32_t id        = 0;
  uint32_t valueBits = 0;
};

// MIDI bytes to events, one translator for every producer (§4.3): the pedal's control loop and
// the plugin's Standalone feed it the bytes they receive, in order, so identical bytes give
// identical events. Integer-only, used by producers and never by Process. It runs ahead of
// libDaisy's parser, which mis-handles Song Position (LD src/hid/midi_parser.cpp:83-86,
// :106-109), and keeps no running status of its own:
//
//   F8          ClockTick
//   FA          Transport Start, position 0, AtNextTick
//   FB          Transport Continue, AtNextTick
//   FC          Transport Stop, with AtNextTick marking MIDI's kind (Tempo.h)
//   F2 lsb msb  Transport Locate, position 6·(lsb + 128·msb), AtNextTick
//   others      nothing: Forward, for the other parsers (F9, FD, FE, FF and every non-real-time
//               byte)
//
// Real-time bytes (F8-FF) may arrive anywhere, inside another message or SysEx, and leave the F2
// state alone; any other status byte aborts it.
//
// It also follows the master's song position, for the re-assert after an Exact load (§2.5) and
// nothing else: FA sets the position to 0 and running; FB sets running, keeping the position; F2
// sets the position; FC clears running; an F8 while running adds one (modulo
// tempo::kPositionModulus, so the position always fits a Transport event). Until the first FA or
// F2 the position is unknown. Reset (after a UART restart, which may have lost bytes) forgets the
// position, the running state and the F2 state.
class MidiClockParser {
 public:
  enum class Result : uint8_t {
    Forward  = 0,  // not a clock byte: pass it on to other parsing
    Consumed = 1,  // part of a Song Position (F2 or its data), no event yet
    Event    = 2,  // *event holds the clock event this byte completed
  };
  Result Feed(uint8_t byte, MidiClockEvent* event) noexcept;
  void   Reset() noexcept;

  bool     PositionKnown() const noexcept { return positionKnown_; }
  bool     Running() const noexcept { return running_; }
  // The position the next F8 will carry, in 24-ppqn ticks; meaningful when PositionKnown().
  uint32_t NextTickPosition() const noexcept { return position_; }

 private:
  uint8_t  f2State_       = 0;  // 0 idle, 1 awaiting the LSB, 2 awaiting the MSB
  uint8_t  lsb_           = 0;
  bool     positionKnown_ = false;
  bool     running_       = false;
  uint32_t position_      = 0;
};

}  // namespace brainscape
