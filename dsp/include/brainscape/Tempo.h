#pragma once
#include <cstdint>

#include "brainscape/FpProfile.h"

// The tempo core's public vocabulary (docs/design/clock.md §2.6, §4.1, §4.2): the payloads of
// events 6-10, what the engine reports about tempo, and its counters. Integer-only: an event's
// value is handled here as its binary32 bit pattern, so producers build positions and the engine
// reads them without a floating-point operation (every position is an exact binary32, §4.1).
//
// The numbers are permanent once landed (mode-compiler.md §7.4). Engine::EventType gains them
// with the sound revision that wires the tempo core into the engine (§11.3); until then nothing
// in the engine reads this header.
namespace brainscape::tempo {

// Event numbers (§4.1). 11, GlobalReverse, is reserved for the rest of W2.
inline constexpr uint8_t kEventTap         = 6;   // id 0; value +0
inline constexpr uint8_t kEventTempo       = 7;   // id: ns per quarter; value +0
inline constexpr uint8_t kEventClockTick   = 8;   // id 0; value +0
inline constexpr uint8_t kEventTransport   = 9;   // id: TransportId(); value: a position or +0
inline constexpr uint8_t kEventSubdivision = 10;  // id: SubdivisionId(); value +0
inline constexpr uint8_t kEventLast        = 10;  // types above it are unknown (§4.2)

// The tempo range (§2.1): 20-300 BPM, PresetState's µs per quarter times 1,000.
inline constexpr uint32_t kMinNsPerQuarter = 200000000u;   // 300 BPM
inline constexpr uint32_t kMaxNsPerQuarter = 3000000000u;  // 20 BPM

// Transport (event 9): id bits 0-1 the kind, bit 8 AtNextTick, bits 16-31 the host anchor's
// frame offset (Start, Continue and Locate without AtNextTick only), every other bit 0 (§4.1).
// AtNextTick marks the MIDI kind (§3.4): Start, Continue and Locate then apply at the next
// ClockTick, and a Stop at its own frame with MIDI's rules (it cancels an armed transport and
// reverts a clock source to Internal); without it (the plugin host's kind) a Stop only stops the
// transport. MidiClockParser sets it on all four, FC included (as-built note, clock.md §11.10).
enum class TransportKind : uint8_t { Stop = 0, Start = 1, Continue = 2, Locate = 3 };
inline constexpr uint32_t kTransportKindMask     = 0x3u;
inline constexpr uint32_t kTransportAtNextTick   = 1u << 8;
inline constexpr uint32_t kTransportOffsetShift  = 16;
inline constexpr uint32_t kTransportReservedMask = 0x0000FEFCu;  // bits 2-7 and 9-15
// Positions are 24-ppqn ticks in [0, kPositionModulus): 96 × 2^16, a multiple of every grid and
// of a 16-step pattern of whole notes, and below 2^24, so every position is an exact binary32.
inline constexpr uint32_t kPositionModulus = 6291456u;

constexpr uint32_t TransportId(TransportKind kind, bool atNextTick, uint32_t offset = 0) noexcept {
  return static_cast<uint32_t>(kind) | (atNextTick ? kTransportAtNextTick : 0u) |
         ((offset & 0xFFFFu) << kTransportOffsetShift);
}

// Subdivision (event 10): id bits 0-7 the code, bits 8-15 the field, every other bit 0 (§4.1).
enum class SubdivField : uint8_t { Subdivision = 0, TimeMode = 1 };
// §5.1's codes: 0 TAP (×1, the default and neutral setting), 1 ×1/4, 2 ×1/2, 3 ×2, 4 ×4, 5 ×8.
inline constexpr uint8_t kSubdivCodes   = 6;
inline constexpr uint8_t kSubdivTap     = 0;
// §2.4's time modes: 0 Free (the default), 1 Subdiv, 2 Tempo (which forces TAP).
inline constexpr uint8_t kTimeModeCodes = 3;
inline constexpr uint8_t kTimeModeTempo = 2;

constexpr uint32_t SubdivisionId(SubdivField field, uint8_t code) noexcept {
  return static_cast<uint32_t>(code) | (static_cast<uint32_t>(field) << 8);
}

// The grid of each Subdiv code in 24-ppqn ticks (§5.1), also the s of every synced duration
// (§2.3). A switch, not a table: an inline table is a COMDAT section the firmware's ITCM check
// would have to place (firmware/cmake/ItcmCheck.cmake).
constexpr uint32_t SubdivTicks(uint8_t code) noexcept {
  switch (code) {
    case 1: return 96;  // ×1/4: whole notes
    case 2: return 48;  // ×1/2: halves
    case 3: return 12;  // ×2: eighths
    case 4: return 6;   // ×4: sixteenths
    case 5: return 3;   // ×8: thirty-seconds
    default: return 24;  // TAP: quarters
  }
}

// The exact binary32 bit pattern of an integer n < 2^24, built in integers (+0 for 0): what a
// producer stores as a Transport position's value.
constexpr uint32_t IntegerValueBits(uint32_t n) noexcept {
  if (n == 0) return 0;
  uint32_t e = 0;  // floor(log2 n)
  while (e < 31 && (n >> (e + 1)) != 0) ++e;
  const uint32_t mantissa = (e <= 23 ? (n << (23 - e)) : (n >> (e - 23))) & 0x7FFFFFu;
  return ((127u + e) << 23) | mantissa;
}

// A Transport position from its value's bits: true when they are the exact binary32 of an integer
// in [0, kPositionModulus). −0, fractions, subnormals, infinities and NaNs are refused: §4.2
// ignores and counts such an event, never clamps or rounds it.
constexpr bool DecodePositionBits(uint32_t bits, uint32_t* position) noexcept {
  if (bits == 0) {
    *position = 0;
    return true;
  }
  if ((bits >> 31) != 0) return false;
  const uint32_t exponent = (bits >> 23) & 0xFFu;
  if (exponent < 127 || exponent >= 150) return false;  // below 1, or at least 2^23
  const uint32_t mantissa = (bits & 0x7FFFFFu) | 0x800000u;
  const uint32_t shift    = 150u - exponent;  // 1-23 fraction bits
  if ((mantissa & ((1u << shift) - 1u)) != 0) return false;
  const uint32_t value = mantissa >> shift;
  if (value >= kPositionModulus) return false;
  *position = value;
  return true;
}

// The sources (§3.1). The plugin host is not one: its events play as Internal.
enum class ClockSource : uint8_t { Internal = 0, ClockFree = 1, ClockRunning = 2 };

// The class of a change of the committed tempo Pc (§7.1), from engine state alone: Jump above
// Pc >> 5, Drift a deadband commit (rule 3) within it, Step any other change within it.
enum class PcChange : uint8_t { None = 0, Jump = 1, Drift = 2, Step = 3 };

}  // namespace brainscape::tempo

namespace brainscape {

// What displays, logs and producers read (§2.6): computed at the tempo core's current frame, the
// end of the last rendered span, with §3.5's gap predicate applied without mutating the state, so
// a display never shows a clock that has gone.
struct TempoInfo {
  int64_t  position       = -1;  // the phasor's tick at the last rendered frame
  uint32_t nsPerQuarter   = 0;   // the committed tempo, Pc, in ns per quarter
  uint8_t  source         = 0;   // tempo::ClockSource
  uint8_t  timeMode       = 0;   // as stored or last set (§2.4)
  uint8_t  subdiv         = 0;   // §5.1's code as stored or last set (not the effective one)
  uint8_t  flags          = 0;   // kTempoFlag* below
  int64_t  lastGridFrame  = -1;  // the frame the last grid position fired at: F(k), or the
                                 // event's frame for a catch-up; -1 before any (§6.3, §8.5)
  int64_t  lastClockBirth = -1;  // the frame the last CLOCK hit was born at, after jitter and any
                                 // deferral, or -1 (the engine's)
};
inline constexpr uint8_t kTempoFlagRunning = 1u << 0;  // the transport runs
inline constexpr uint8_t kTempoFlagLocked  = 1u << 1;  // the follower's window holds 24 or more

// Counts since Init, which Reset, Restart and loads keep (§2.6). The tempo core counts the tempo
// group; the engine counts the CLOCK births, the post chain the crossfades and folds (§5.3, §6.3,
// §7.3). transportsIgnored is an as-built addition (clock.md §11.10): §3.4 counts a host-style
// Transport ignored under clock, and §2.6's list had no counter for it.
struct TempoStats {
  uint64_t taps = 0;                 // valid Tap events
  uint64_t tapsIgnored = 0;          // of them, ignored: under ClockRunning, or a bounce
  uint64_t tapPhases = 0;            // of them, downbeat marks under ClockFree
  uint64_t tempoEvents = 0;          // Tempo events applied
  uint64_t tempoIgnored = 0;         // Tempo events ignored under clock
  uint64_t ticks = 0;                // valid ClockTick events
  uint64_t tickOutliers = 0;         // ticks excluded from the fit
  uint64_t reacquires = 0;           // windows replaced by the ring after six outliers
  uint64_t dropoutTicks = 0;         // ticks inferred lost
  uint64_t gaps = 0;                 // a second or more without a tick (§3.5)
  uint64_t losses = 0;               // gaps under a clock source
  uint64_t resumes = 0;              // implicit Continues after a loss under ClockRunning
  uint64_t transports = 0;           // valid Transport events applied, armed or stored
  uint64_t transportsCancelled = 0;  // armed Transports a Stop cancelled
  uint64_t transportsIgnored = 0;    // host-style Transports ignored under clock
  uint64_t subdivEvents = 0;         // valid Subdivision events
  uint64_t clockBirths = 0;          // the engine's
  uint64_t clockDeferred = 0;
  uint64_t clockDeferredFrames = 0;
  uint64_t clockDropped = 0;
  uint64_t commits = 0;              // §7.1 rules 1 and 3: Pc := P_fit under clock
  uint64_t earlyCommits = 0;         // §7.1 rule 2
  uint64_t jumps = 0;                // changes of Pc classed Jump
  uint64_t slews = 0;                // changes of Pc classed Drift
  uint64_t crossfades = 0;           // the post chain's (synced times)
  uint64_t folds = 0;
  uint64_t invalidEvents = 0;        // events 6-10 whose payload breaks §4.1
  uint64_t unknownEvents = 0;        // event types above 10
};

}  // namespace brainscape
