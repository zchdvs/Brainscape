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

// The Tempo knob in Tempo time mode (§6.4, D15): ns per quarter, exponential from 20 BPM at
// m = 0 to 300 BPM at m = 1, round(3·10^9 · 2^(−m·log2 15)) through DetMath's Exp2D, so equal
// turns make equal tempo ratios. m is canonicalized as a macro position is (NaN and ±inf to 0,
// clamped to [0, 1]). A producer function (the pedal's Time knob, a plugin's tempo control), never
// called by Process: an entry point that owns the FP control word, so every build turns a knob
// position into the same Tempo event. Always within the tempo range.
uint32_t TempoNsFromKnob(float m) noexcept;

// The Subdiv knob (row 83, perf.subdiv, §10.4): its positions 0-5 in the Microcosm's CC#5 order
// (×1/4, ×1/2, TAP, ×2, ×4, ×8) and §5.1's codes (0 TAP, 1 ×1/4, 2 ×1/2, 3 ×2, 4 ×4, 5 ×8), which
// Subdivision events carry. A position above 5 reads as 5, a code above 5 as TAP.
constexpr uint8_t SubdivCodeFromPosition(uint32_t position) noexcept {
  return position == 2u ? kSubdivTap
                        : static_cast<uint8_t>(position < 2u ? position + 1u : (position > 5u ? 5u : position));
}
constexpr uint32_t SubdivPositionFromCode(uint8_t code) noexcept {
  const uint32_t c = code;
  return c == kSubdivTap || c > 5u ? 2u : (c <= 2u ? c - 1u : c);
}

// The plugin host's tempo and position as events (§4.4): producer functions, as TempoNsFromKnob
// is, and entry points that own the FP control word, so every build turns a host block's reading
// into the same events and the same frames. Never called by Process.
//
// NsPerQuarterFromBpm: ns per quarter for a host tempo in BPM, round(6·10^10 / bpm) (ties away
// from zero), clamped to the tempo range [kMinNsPerQuarter, kMaxNsPerQuarter]; 0, no event, for a
// non-finite or non-positive bpm.
uint32_t NsPerQuarterFromBpm(double bpm) noexcept;

// HostAnchor: the anchor a host-style Start or Locate carries (§4.1, §4.4, D11) for a host block
// whose first frame is at quarter-note position `ppq`. With x = 24·ppq, the position is k = ⌈x⌉,
// the first tick at or after the block's first frame (an x within 10^-9 of an integer counts as
// that integer), reduced modulo kPositionModulus (floor-mod, so pre-roll wraps to the same grid
// phase); the offset is that tick's frame from the block's first frame at `ns` per quarter and the
// engine's integer rate `rate`, round((k − x)·ns·rate / (24·10^9)), evaluated in binary64 in that
// order, at most one tick rounded to a frame (48,000 frames at 20 BPM and 384 kHz, inside the
// event's 16 bits). False, with nothing written, for a non-finite ppq or one of 2^40 quarters or
// more either way, an ns outside the tempo range or a rate outside 8,000-384,000.
bool HostAnchor(double ppq, uint32_t ns, uint32_t rate, uint32_t* position, uint32_t* offset) noexcept;

// HostTempoFromBpm: what the plugin follows of a host's tempo (§4.4, clock.md §11.16): q =
// 6·10^10 / bpm ns per quarter, doubled while below kMinNsPerQuarter and halved while above
// kMaxNsPerQuarter, at most kMaxHostOctaves times, then rounded and clamped as NsPerQuarterFromBpm
// rounds and clamps it. octaves > 0 doubled it (a host faster than 300 BPM: one engine quarter is
// 2^octaves host beats), < 0 halved it (slower than 20 BPM: 2^-octaves engine quarters a beat), so
// every engine quarter lands on a host beat, or every host beat on one; the follower anchors at
// ppq·2^-octaves, exactly. `clamped` when the folds did not reach the range (a host below 1.25 or
// above 4,800 BPM): the grid then runs off the host's beats. nsPerQuarter 0, no event, for a
// non-finite or non-positive bpm.
inline constexpr int32_t kMaxHostOctaves = 4;
struct HostTempo {
  uint32_t nsPerQuarter = 0;
  int32_t  octaves      = 0;
  bool     clamped      = false;
};
HostTempo HostTempoFromBpm(double bpm) noexcept;

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
