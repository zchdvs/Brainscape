// The shared MIDI clock translator and the tempo events' payload helpers (docs/design/clock.md
// §4.1, §4.3, §8.2): real-time bytes inside a Note On and inside SysEx, Song Position after a
// Program Change and after Channel Pressure (libDaisy's failure cases), running status kept across
// F8, the master's position through FA, F8, FC, F2, FB and Reset(), and every position's exact
// binary32. Each behaviour check also runs on a perturbed translator, which it must catch.
#include <cstdint>
#include <cstring>
#include <vector>

#include "brainscape/MidiClock.h"
#include "brainscape/Tempo.h"
#include "catch.hpp"

using namespace brainscape;
using Result = MidiClockParser::Result;

namespace {

struct Out {
  std::vector<MidiClockEvent> events;
  std::vector<uint8_t>        forwarded;
  std::vector<Result>         results;
};

// A parser with libDaisy's flaw transplanted (a real-time byte aborts the F2 state) and an F8
// counted while stopped: the perturbed control the checks must catch.
class PerturbedParser {
 public:
  Result Feed(uint8_t byte, MidiClockEvent* e) {
    if (byte == 0xF8) {
      MidiClockEvent unused;
      inner_.Feed(0xF7, &unused);  // the flaw: EOX, a status byte, aborts the F2 state
      if (inner_.PositionKnown() && !inner_.Running()) ++extra_;  // counts while stopped
    }
    return inner_.Feed(byte, e);
  }
  uint32_t NextTickPosition() const { return inner_.NextTickPosition() + extra_; }

 private:
  MidiClockParser inner_;
  uint32_t        extra_ = 0;
};

template <class P>
Out Run(P& parser, const std::vector<uint8_t>& bytes) {
  Out o;
  for (uint8_t b : bytes) {
    MidiClockEvent e;
    const Result r = parser.Feed(b, &e);
    o.results.push_back(r);
    if (r == Result::Event) o.events.push_back(e);
    if (r == Result::Forward) o.forwarded.push_back(b);
  }
  return o;
}

bool IsTick(const MidiClockEvent& e) {
  return e.type == tempo::kEventClockTick && e.id == 0 && e.valueBits == 0;
}
bool IsTransport(const MidiClockEvent& e, tempo::TransportKind kind, uint32_t position) {
  return e.type == tempo::kEventTransport && e.id == tempo::TransportId(kind, true) &&
         e.valueBits == tempo::IntegerValueBits(position);
}

// The checks, as functions of the parser type, so the perturbed control runs the same ones.
template <class P>
bool SongPositionSurvivesRealTime() {
  P p;
  const Out o = Run(p, {0xF2, 0x10, 0xF8, 0x20});
  return o.events.size() == 2 && IsTick(o.events[0]) &&
         IsTransport(o.events[1], tempo::TransportKind::Locate, 6u * (0x10u + 128u * 0x20u));
}

}  // namespace

TEST_CASE("MidiClockParser: the byte table", "[midiclock]") {
  MidiClockParser p;
  const Out o = Run(p, {0xF8, 0xFA, 0xFB, 0xFC, 0xF2, 0x05, 0x01, 0xF9, 0xFD, 0xFE, 0xFF});
  REQUIRE(o.events.size() == 5);
  REQUIRE(IsTick(o.events[0]));
  REQUIRE(IsTransport(o.events[1], tempo::TransportKind::Start, 0));
  REQUIRE(IsTransport(o.events[2], tempo::TransportKind::Continue, 0));
  REQUIRE(IsTransport(o.events[3], tempo::TransportKind::Stop, 0));
  REQUIRE(IsTransport(o.events[4], tempo::TransportKind::Locate, 6u * (5u + 128u)));
  REQUIRE(o.forwarded == std::vector<uint8_t>{0xF9, 0xFD, 0xFE, 0xFF});
  REQUIRE(o.results[4] == Result::Consumed);
  REQUIRE(o.results[5] == Result::Consumed);
  // Every byte, alone, from idle: only F8, FA, FB and FC make events; F2 is consumed; the rest
  // are forwarded.
  for (int b = 0; b < 256; ++b) {
    MidiClockParser q;
    MidiClockEvent  e;
    const Result    r = q.Feed(static_cast<uint8_t>(b), &e);
    INFO("byte " << b);
    if (b == 0xF8 || b == 0xFA || b == 0xFB || b == 0xFC) REQUIRE(r == Result::Event);
    else if (b == 0xF2) REQUIRE(r == Result::Consumed);
    else REQUIRE(r == Result::Forward);
  }
}

TEST_CASE("MidiClockParser: real-time bytes inside a Note On and inside SysEx", "[midiclock]") {
  MidiClockParser p;
  const Out o = Run(p, {0x90, 0x3C, 0xF8, 0x40, 0xF0, 0x7E, 0xF8, 0x01, 0xFA, 0x02, 0xF7});
  REQUIRE(o.events.size() == 3);
  REQUIRE(IsTick(o.events[0]));
  REQUIRE(IsTick(o.events[1]));
  REQUIRE(IsTransport(o.events[2], tempo::TransportKind::Start, 0));
  // The other parsers see the Note On and the SysEx whole, without the real-time bytes.
  REQUIRE(o.forwarded == std::vector<uint8_t>{0x90, 0x3C, 0x40, 0xF0, 0x7E, 0x01, 0x02, 0xF7});
}

TEST_CASE("MidiClockParser: Song Position after a Program Change and Channel Pressure",
          "[midiclock]") {
  MidiClockParser p;
  // libDaisy cuts these after one data byte (LD src/hid/midi_parser.cpp:83-86, :106-109).
  const Out o = Run(p, {0xC0, 0x05, 0xF2, 0x10, 0x20, 0xD3, 0x40, 0xF2, 0x7F, 0x7F});
  REQUIRE(o.events.size() == 2);
  REQUIRE(IsTransport(o.events[0], tempo::TransportKind::Locate, 6u * (0x10u + 128u * 0x20u)));
  REQUIRE(IsTransport(o.events[1], tempo::TransportKind::Locate, 98298u));  // 16,383 × 6
  REQUIRE(o.forwarded == std::vector<uint8_t>{0xC0, 0x05, 0xD3, 0x40});
  // A status byte aborts F2; its data bytes then go on.
  MidiClockParser q;
  const Out a = Run(q, {0xF2, 0x10, 0x90, 0x3C, 0x40, 0xF2, 0x01, 0xF7, 0x02});
  REQUIRE(a.events.empty());
  REQUIRE(a.forwarded == std::vector<uint8_t>{0x90, 0x3C, 0x40, 0xF7, 0x02});
  // A real-time byte does not.
  REQUIRE(SongPositionSurvivesRealTime<MidiClockParser>());
}

TEST_CASE("MidiClockParser: running status kept across F8", "[midiclock]") {
  MidiClockParser p;
  const std::vector<uint8_t> in = {0x90, 0x3C, 0x40, 0xF8, 0x3E, 0xF8, 0x41, 0x40, 0xFA,
                                   0x43, 0xFC, 0x40, 0xB0, 0x07, 0xF8, 0x64, 0x08, 0x10};
  const Out o = Run(p, in);
  std::vector<uint8_t> expect;
  for (uint8_t b : in)
    if (b != 0xF8 && b != 0xFA && b != 0xFC) expect.push_back(b);
  REQUIRE(o.forwarded == expect);
  REQUIRE(o.events.size() == 5);
}

TEST_CASE("MidiClockParser: the master's position", "[midiclock]") {
  MidiClockParser p;
  MidiClockEvent  e;
  REQUIRE_FALSE(p.PositionKnown());
  for (int i = 0; i < 5; ++i) p.Feed(0xF8, &e);  // clock before any transport: unknown
  REQUIRE_FALSE(p.PositionKnown());
  REQUIRE_FALSE(p.Running());
  p.Feed(0xFA, &e);
  REQUIRE(p.PositionKnown());
  REQUIRE(p.Running());
  REQUIRE(p.NextTickPosition() == 0);
  for (int i = 0; i < 3; ++i) p.Feed(0xF8, &e);
  REQUIRE(p.NextTickPosition() == 3);
  p.Feed(0xFC, &e);
  REQUIRE_FALSE(p.Running());
  p.Feed(0xF8, &e);  // clock while stopped does not advance the song position
  p.Feed(0xF8, &e);
  REQUIRE(p.NextTickPosition() == 3);
  REQUIRE(p.Feed(0xF2, &e) == Result::Consumed);
  REQUIRE(p.Feed(0x05, &e) == Result::Consumed);
  REQUIRE(p.Feed(0x00, &e) == Result::Event);
  REQUIRE(p.NextTickPosition() == 30);
  REQUIRE_FALSE(p.Running());
  p.Feed(0xFB, &e);
  REQUIRE(p.Running());
  REQUIRE(p.NextTickPosition() == 30);
  p.Feed(0xF8, &e);
  REQUIRE(p.NextTickPosition() == 31);
  p.Feed(0xF2, &e);  // a reset mid-F2 forgets the half message too
  p.Feed(0x01, &e);
  p.Reset();
  REQUIRE_FALSE(p.PositionKnown());
  REQUIRE_FALSE(p.Running());
  REQUIRE(p.Feed(0x02, &e) == Result::Forward);
  p.Feed(0xF8, &e);
  REQUIRE_FALSE(p.PositionKnown());
  // Continue without a known position stays unknown; F2 alone makes it known.
  p.Feed(0xFB, &e);
  REQUIRE_FALSE(p.PositionKnown());
  p.Feed(0xF2, &e);
  p.Feed(0x7F, &e);
  p.Feed(0x7F, &e);
  REQUIRE(p.PositionKnown());
  REQUIRE(p.NextTickPosition() == 98298u);
  // The position wraps at the event range, so a re-assert's Locate is always valid.
  for (uint32_t i = 0; i < tempo::kPositionModulus - 98298u; ++i) p.Feed(0xF8, &e);
  REQUIRE(p.NextTickPosition() == 0);
  p.Feed(0xF8, &e);
  REQUIRE(p.NextTickPosition() == 1);
}

TEST_CASE("MidiClockParser: a perturbed translator is caught", "[midiclock][control]") {
  REQUIRE_FALSE(SongPositionSurvivesRealTime<PerturbedParser>());
  PerturbedParser p;
  MidiClockEvent  e;
  p.Feed(0xFA, &e);
  p.Feed(0xFC, &e);
  p.Feed(0xF8, &e);
  // The real parser keeps 0 while stopped; the perturbed one counted the tick.
  REQUIRE(p.NextTickPosition() != 0);
}

TEST_CASE("Tempo payloads: positions as exact binary32 bits", "[midiclock][payload]") {
  // Every position: built in integers, equal to the float conversion, and decoded back.
  for (uint32_t n = 0; n < tempo::kPositionModulus; ++n) {
    const uint32_t bits = tempo::IntegerValueBits(n);
    const float    f    = static_cast<float>(n);  // exact: n < 2^24
    uint32_t       fb;
    std::memcpy(&fb, &f, sizeof fb);
    uint32_t back = 0xFFFFFFFFu;
    if (bits != fb || !tempo::DecodePositionBits(bits, &back) || back != n) {
      INFO("n=" << n);
      REQUIRE(bits == fb);
      REQUIRE(back == n);
    }
  }
  // Refused: −0, fractions, the modulus and above, subnormals, infinities and NaNs.
  auto bitsOf = [](float x) {
    uint32_t b;
    std::memcpy(&b, &x, sizeof b);
    return b;
  };
  const uint32_t refused[] = {0x80000000u,          bitsOf(0.5f),        bitsOf(1.5f),
                              bitsOf(-1.0f),        bitsOf(6291456.0f),  bitsOf(8388608.0f),
                              bitsOf(16777216.0f),  bitsOf(3.0e9f),      0x00000001u,
                              0x007FFFFFu,          0x7F800000u,         0xFF800000u,
                              0x7FC00000u,          bitsOf(6291455.5f)};
  for (uint32_t b : refused) {
    uint32_t pos = 12345;
    INFO("bits=" << b);
    REQUIRE_FALSE(tempo::DecodePositionBits(b, &pos));
    REQUIRE(pos == 12345);
  }
  // The ids.
  REQUIRE(tempo::TransportId(tempo::TransportKind::Locate, true) == 0x103u);
  REQUIRE(tempo::TransportId(tempo::TransportKind::Start, false, 0xFFFF) == 0xFFFF0001u);
  REQUIRE(tempo::SubdivisionId(tempo::SubdivField::TimeMode, 2) == 0x102u);
  REQUIRE((tempo::kTransportReservedMask & (tempo::kTransportKindMask | tempo::kTransportAtNextTick |
                                            0xFFFF0000u)) == 0);
  REQUIRE((tempo::kTransportReservedMask | tempo::kTransportKindMask | tempo::kTransportAtNextTick |
           0xFFFF0000u) == 0xFFFFFFFFu);
}
