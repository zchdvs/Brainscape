// The plugin host's tempo and position as events (docs/design/clock.md §4.4, §10.4): the two
// producer functions every plugin build turns a host block's reading into, NsPerQuarterFromBpm and
// HostAnchor, against numbers worked by hand and against an exact rational reading of the design's
// formulas; the Subdiv knob's positions and codes; and the anchor's events in the tempo core, so a
// host-style Start fires its first grid position at the anchor and nothing before it.
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include "FpEnvTestUtil.h"
#include "WideInt.h"
#include "brainscape/Tempo.h"
#include "catch.hpp"
#include "detail/Tempo.h"

using namespace brainscape;
using namespace brainscape::testing;
using tempo::HostAnchor;
using tempo::NsPerQuarterFromBpm;
using tempo::TransportKind;

namespace {

uint32_t Xorshift(uint32_t& s) {
  s ^= s << 13;
  s ^= s >> 17;
  s ^= s << 5;
  return s;
}

// HostAnchor's answer, or {UINT32_MAX, UINT32_MAX} when it refuses.
struct Anchor {
  uint32_t position = UINT32_MAX;
  uint32_t offset   = UINT32_MAX;
};
Anchor AnchorOf(double ppq, uint32_t ns, uint32_t rate = 48000) {
  Anchor a;
  if (!HostAnchor(ppq, ns, rate, &a.position, &a.offset)) return {};
  return a;
}

}  // namespace

TEST_CASE("Host tempo: ns per quarter from the host's BPM, rounded and clamped (§4.4)", "[tempo][host]") {
  REQUIRE(NsPerQuarterFromBpm(120.0) == 500000000u);
  REQUIRE(NsPerQuarterFromBpm(140.0) == 428571429u);    // 428,571,428.57
  REQUIRE(NsPerQuarterFromBpm(137.5) == 436363636u);    // 436,363,636.36
  REQUIRE(NsPerQuarterFromBpm(128.0) == 468750000u);    // exact
  REQUIRE(NsPerQuarterFromBpm(93.75) == 640000000u);    // exact
  REQUIRE(NsPerQuarterFromBpm(20.0) == 3000000000u);    // the range's ends
  REQUIRE(NsPerQuarterFromBpm(300.0) == 200000000u);
  // Clamped to the tempo range: a host faster than 300 BPM or slower than 20.
  REQUIRE(NsPerQuarterFromBpm(300.0001) == 200000000u);
  REQUIRE(NsPerQuarterFromBpm(999.0) == 200000000u);
  REQUIRE(NsPerQuarterFromBpm(19.999) == 3000000000u);
  REQUIRE(NsPerQuarterFromBpm(1.0e-300) == 3000000000u);
  REQUIRE(NsPerQuarterFromBpm(std::numeric_limits<double>::denorm_min()) == 3000000000u);  // +inf
  // No event for a tempo that is not one.
  REQUIRE(NsPerQuarterFromBpm(0.0) == 0u);
  REQUIRE(NsPerQuarterFromBpm(-0.0) == 0u);
  REQUIRE(NsPerQuarterFromBpm(-120.0) == 0u);
  REQUIRE(NsPerQuarterFromBpm(std::numeric_limits<double>::infinity()) == 0u);
  REQUIRE(NsPerQuarterFromBpm(-std::numeric_limits<double>::infinity()) == 0u);
  REQUIRE(NsPerQuarterFromBpm(std::numeric_limits<double>::quiet_NaN()) == 0u);

  // Every result is the integer nearest 6·10^10 / bpm: within half a nanosecond of the quotient,
  // checked exactly in integers as |2·ns·bpm − 1.2·10^11| ≤ bpm on bpm = m / 2^20.
  uint32_t seed = 0x5eed1u;
  for (int i = 0; i < 200000; ++i) {
    const uint32_t m   = 20u * (1u << 20) + Xorshift(seed) % (280u * (1u << 20));  // [20, 300) BPM
    const double   bpm = static_cast<double>(m) / static_cast<double>(1u << 20);
    const uint32_t ns  = NsPerQuarterFromBpm(bpm);
    REQUIRE(ns >= tempo::kMinNsPerQuarter);
    REQUIRE(ns <= tempo::kMaxNsPerQuarter);
    // 2·ns·m − 1.2·10^11·2^20 against m, in 128 bits' worth of unsigned arithmetic split in two.
    const unsigned long long lhs = 2ull * ns;  // < 2^33
    // lhs·m < 2^62 and 1.2e11·2^20 < 2^57: both fit uint64_t.
    const unsigned long long a = lhs * m;
    const unsigned long long b = 120000000000ull * (1ull << 20);
    const unsigned long long diff = a > b ? a - b : b - a;
    INFO("bpm " << bpm << " ns " << ns);
    REQUIRE(diff <= m);
  }
}

TEST_CASE("Host tempo: a host tempo outside the range folds by octaves into it (§4.4, §11.16)",
          "[tempo][host]") {
  using tempo::HostTempoFromBpm;
  // In range: NsPerQuarterFromBpm's ns, no fold.
  for (const double bpm : {20.0, 93.75, 120.0, 137.5, 140.0, 300.0}) {
    const tempo::HostTempo h = HostTempoFromBpm(bpm);
    REQUIRE(h.nsPerQuarter == NsPerQuarterFromBpm(bpm));
    REQUIRE(h.octaves == 0);
    REQUIRE_FALSE(h.clamped);
  }
  // Faster than 300 BPM: halved until it fits, one engine quarter every 2^octaves host beats.
  REQUIRE(HostTempoFromBpm(400.0).nsPerQuarter == 300000000u);  // 200 BPM
  REQUIRE(HostTempoFromBpm(400.0).octaves == 1);
  REQUIRE(HostTempoFromBpm(600.0).nsPerQuarter == 200000000u);  // 300 BPM, the range's end
  REQUIRE(HostTempoFromBpm(600.0).octaves == 1);
  REQUIRE(HostTempoFromBpm(999.0).nsPerQuarter == 240240240u);  // 249.75 BPM
  REQUIRE(HostTempoFromBpm(999.0).octaves == 2);
  REQUIRE(HostTempoFromBpm(4800.0).octaves == 4);
  REQUIRE_FALSE(HostTempoFromBpm(4800.0).clamped);
  // Slower than 20 BPM: doubled, 2^-octaves engine quarters a host beat.
  REQUIRE(HostTempoFromBpm(15.0).nsPerQuarter == 2000000000u);  // 30 BPM
  REQUIRE(HostTempoFromBpm(15.0).octaves == -1);
  REQUIRE(HostTempoFromBpm(19.999).octaves == -1);
  REQUIRE(HostTempoFromBpm(1.25).octaves == -4);
  REQUIRE(HostTempoFromBpm(1.25).nsPerQuarter == 3000000000u);
  REQUIRE_FALSE(HostTempoFromBpm(1.25).clamped);
  // Out of four octaves' reach: clamped as NsPerQuarterFromBpm clamps.
  REQUIRE(HostTempoFromBpm(1.0).clamped);
  REQUIRE(HostTempoFromBpm(1.0).nsPerQuarter == 3000000000u);
  REQUIRE(HostTempoFromBpm(5000.0).clamped);
  REQUIRE(HostTempoFromBpm(5000.0).nsPerQuarter == 200000000u);
  REQUIRE(HostTempoFromBpm(std::numeric_limits<double>::denorm_min()).clamped);
  // No tempo, no event.
  REQUIRE(HostTempoFromBpm(0.0).nsPerQuarter == 0u);
  REQUIRE(HostTempoFromBpm(-400.0).nsPerQuarter == 0u);
  REQUIRE(HostTempoFromBpm(std::numeric_limits<double>::quiet_NaN()).nsPerQuarter == 0u);
  // The fold is exact: a host at 2^j times an in-range tempo plays the tempo the fewest halvings
  // (or doublings) bring into the range, NsPerQuarterFromBpm's ns for it.
  uint32_t seed = 0xf01dU;
  for (int i = 0; i < 20000; ++i) {
    const uint32_t m   = 20u * (1u << 20) + Xorshift(seed) % (280u * (1u << 20));
    const double   bpm = static_cast<double>(m) / static_cast<double>(1u << 20);
    for (int j = 1; j <= 4; ++j) {
      INFO("bpm " << bpm << " j " << j);
      const double           up = bpm * static_cast<double>(1 << j);
      const tempo::HostTempo hu = HostTempoFromBpm(up);
      double                 f  = up;
      int32_t                k  = 0;
      while (f > 300.0) {
        f *= 0.5;
        ++k;
      }
      REQUIRE(hu.octaves == k);
      REQUIRE(hu.nsPerQuarter == NsPerQuarterFromBpm(f));
      REQUIRE_FALSE(hu.clamped);
      const double           down = bpm / static_cast<double>(1 << j);
      const tempo::HostTempo hd   = HostTempoFromBpm(down);
      f                           = down;
      k                           = 0;
      while (f < 20.0) {
        f *= 2.0;
        --k;
      }
      REQUIRE(hd.octaves == k);
      REQUIRE(hd.nsPerQuarter == NsPerQuarterFromBpm(f));
      REQUIRE_FALSE(hd.clamped);
    }
  }
}

TEST_CASE("Host tempo: the conversions own the FP control word", "[tempo][host][fpenv]") {
  // A host thread in round-toward-zero with FTZ and DAZ gets the same answers.
  const double bpms[] = {137.5, 140.0, 97.3, 123.456789};
  const double ppqs[] = {3.37, 0.1, 1234.567891, -0.51};
  for (double bpm : bpms) {
    const uint32_t want = NsPerQuarterFromBpm(bpm);
    uint32_t       got  = 0;
    {
      const HostileFpScope hostile;
      got = NsPerQuarterFromBpm(bpm);
    }
    REQUIRE(got == want);
    tempo::HostTempo folded;
    {
      const HostileFpScope hostile;
      folded = tempo::HostTempoFromBpm(bpm * 4.0);
    }
    REQUIRE(folded.nsPerQuarter == tempo::HostTempoFromBpm(bpm * 4.0).nsPerQuarter);
    for (double ppq : ppqs) {
      const Anchor a = AnchorOf(ppq, want);
      Anchor       h;
      bool         ok = false;
      {
        const HostileFpScope hostile;
        ok = HostAnchor(ppq, want, 48000, &h.position, &h.offset);
      }
      REQUIRE(ok);
      REQUIRE(h.position == a.position);
      REQUIRE(h.offset == a.offset);
    }
  }
}

TEST_CASE("Host anchor: the first tick at or after the block's first frame (§4.1, §4.4)", "[tempo][host]") {
  // §8.4's bounce: 137.5 BPM from ppq 3.37. x = 80.88 ticks, so tick 81 is 0.12 of a tick in, and a
  // tick is 872.73 frames at 48 kHz: round(104.727) = 105.
  const uint32_t ns1375 = NsPerQuarterFromBpm(137.5);
  Anchor         a      = AnchorOf(3.37, ns1375);
  REQUIRE(a.position == 81u);
  REQUIRE(a.offset == 105u);
  // At 120 BPM a tick is exactly 1,000 frames at 48 kHz.
  a = AnchorOf(0.0, 500000000u);
  REQUIRE(a.position == 0u);
  REQUIRE(a.offset == 0u);
  a = AnchorOf(1.25, 500000000u);  // x = 30: on a tick
  REQUIRE(a.position == 30u);
  REQUIRE(a.offset == 0u);
  a = AnchorOf(1.26, 500000000u);  // x = 30.24: tick 31, 0.76 of a tick on
  REQUIRE(a.position == 31u);
  REQUIRE(a.offset == 760u);
  a = AnchorOf(0.5 / 24.0, 500000000u);  // half a tick: 500 frames
  REQUIRE(a.position == 1u);
  REQUIRE(a.offset == 500u);
  // At 96 kHz the same anchor is twice the frames; at 44.1 kHz a tick is 918.75 frames, so
  // round(0.76 · 918.75) = 698.
  REQUIRE(AnchorOf(1.26, 500000000u, 96000).offset == 1520u);
  REQUIRE(AnchorOf(1.26, 500000000u, 44100).offset == 698u);

  // Within 10^-9 of a tick counts as the tick, either side: a host's ppq a rounding away from a
  // beat anchors on the beat, never a whole tick later.
  a = AnchorOf(1.0 + 1.0e-12, 500000000u);
  REQUIRE(a.position == 24u);
  REQUIRE(a.offset == 0u);
  a = AnchorOf(1.0 - 1.0e-12, 500000000u);
  REQUIRE(a.position == 24u);
  REQUIRE(a.offset == 0u);
  // Past the tolerance it is the next tick: x = 24 + 2.4·10^-8 anchors tick 25 a whole tick on.
  a = AnchorOf(1.0 + 1.0e-9, 500000000u);
  REQUIRE(a.position == 25u);
  REQUIRE(a.offset == 1000u);

  // Pre-roll wraps by floor-mod to the same grid phase: ppq −0.51 is x = −12.24, tick −12 is 0.24
  // of a tick on, at position 6,291,456 − 12.
  a = AnchorOf(-0.51, 500000000u);
  REQUIRE(a.position == tempo::kPositionModulus - 12u);
  REQUIRE(a.offset == 240u);
  a = AnchorOf(-0.5, 500000000u);
  REQUIRE(a.position == tempo::kPositionModulus - 12u);
  REQUIRE(a.offset == 0u);
  // And past the range's end: 262,144.5 quarters is tick 6,291,468, position 12.
  a = AnchorOf(262144.5, 500000000u);
  REQUIRE(a.position == 12u);
  REQUIRE(a.offset == 0u);
  // Every position is a multiple of every grid away from the unreduced tick, so the grid phase is
  // the host's: 96 divides the modulus.
  REQUIRE(tempo::kPositionModulus % 96u == 0u);

  // The largest offset is one tick rounded to a frame: 48,000 frames at 20 BPM and 384 kHz.
  a = AnchorOf(1.0e-8 / 24.0, tempo::kMaxNsPerQuarter, 384000);
  REQUIRE(a.position == 1u);
  REQUIRE(a.offset <= 48000u);
  REQUIRE(a.offset >= 47999u);

  // Refused, with nothing written: what no host block can anchor.
  uint32_t position = 7u, offset = 9u;
  REQUIRE_FALSE(HostAnchor(std::numeric_limits<double>::quiet_NaN(), 500000000u, 48000, &position, &offset));
  REQUIRE_FALSE(HostAnchor(std::numeric_limits<double>::infinity(), 500000000u, 48000, &position, &offset));
  REQUIRE_FALSE(HostAnchor(1.0995116277760e12, 500000000u, 48000, &position, &offset));  // 2^40
  REQUIRE_FALSE(HostAnchor(-1.0995116277760e12, 500000000u, 48000, &position, &offset));
  REQUIRE_FALSE(HostAnchor(1.0, tempo::kMinNsPerQuarter - 1u, 48000, &position, &offset));
  REQUIRE_FALSE(HostAnchor(1.0, tempo::kMaxNsPerQuarter + 1u, 48000, &position, &offset));
  REQUIRE_FALSE(HostAnchor(1.0, 500000000u, 7999, &position, &offset));
  REQUIRE_FALSE(HostAnchor(1.0, 500000000u, 384001, &position, &offset));
  REQUIRE_FALSE(HostAnchor(1.0, 500000000u, 48000, nullptr, &offset));
  REQUIRE(position == 7u);
  REQUIRE(offset == 9u);
  REQUIRE(HostAnchor(1.0995116277759e12, 500000000u, 48000, &position, &offset));  // just inside
}

TEST_CASE("Host anchor: random positions against an exact reading of §4.4", "[tempo][host]") {
  // ppq = m / 2^24 is exact in binary64, and so is x = 24·ppq = 3m / 2^21, so the design's formula
  // reads in integers: k = ⌈3m / 2^21⌉ (no other x lies within 10^-9 of an integer: the nearest
  // non-integer is 2^-21 ≈ 4.8·10^-7 away), and the offset rounds the exact quotient
  // (k·2^21 − 3m)·ns·rate / (2^21·24·10^9). HostAnchor evaluates it in binary64, whose roundings
  // may decide a quotient within 10^-6 of a half either way; every other one must agree.
  uint32_t seed      = 0xabcdefu;
  int      compared  = 0;
  const I128 den     = I128::FromU64((1ull << 21) * 24ull * 1000000000ull);
  for (int i = 0; i < 100000; ++i) {
    // m in about ±24 quarters' worth of ticks either side of 0 (pre-roll included).
    const int64_t  m    = static_cast<int64_t>(Xorshift(seed) % 805306368u) - 402653184;
    const uint32_t ns   = tempo::kMinNsPerQuarter +
                        Xorshift(seed) % (tempo::kMaxNsPerQuarter - tempo::kMinNsPerQuarter + 1u);
    const uint32_t rate = (Xorshift(seed) & 1u) != 0u ? 48000u : 44100u;
    const double   ppq  = static_cast<double>(m) / 16777216.0;
    const Anchor   a    = AnchorOf(ppq, ns, rate);
    REQUIRE(a.position != UINT32_MAX);
    I128 q, r;
    I128::DivModFloor(I128::FromI64(3 * m), I128::FromI64(1 << 21), &q, &r);
    const int64_t k    = r.IsZero() ? q.ToI64() : q.ToI64() + 1;
    const int64_t rest = r.IsZero() ? 0 : (1 << 21) - r.ToI64();  // (k − x)·2^21, in [0, 2^21)
    const int64_t want = ((k % 6291456) + 6291456) % 6291456;
    INFO("ppq " << ppq << " ns " << ns << " rate " << rate);
    REQUIRE(a.position == static_cast<uint32_t>(want));
    I128 oq, orem;
    I128::DivModFloor(I128::FromI64(rest) * I128::FromU64(ns) * I128::FromU64(rate), den, &oq, &orem);
    const I128 twice = orem + orem;
    const I128 dist  = twice > den ? twice - den : den - twice;  // |2·remainder − den|
    if (dist * I128::FromU64(1000000) > den) {                  // not within 10^-6 of a half
      const int64_t rounded = oq.ToI64() + (twice >= den ? 1 : 0);
      REQUIRE(a.offset == static_cast<uint32_t>(rounded));
      ++compared;
    }
    // At most one tick, rounded to a frame.
    I128 tq, tr;
    I128::DivModFloor(I128::FromU64(ns) * I128::FromU64(rate), I128::FromU64(24000000000ull), &tq, &tr);
    REQUIRE(static_cast<int64_t>(a.offset) <= tq.ToI64() + 1);
  }
  REQUIRE_FALSE(I128::Overflow());
  REQUIRE(compared > 99000);
}

TEST_CASE("Subdiv knob: positions in CC#5's order and §5.1's codes", "[tempo][host]") {
  using tempo::SubdivCodeFromPosition;
  using tempo::SubdivPositionFromCode;
  // ×1/4, ×1/2, TAP, ×2, ×4, ×8.
  const uint8_t codes[] = {1, 2, 0, 3, 4, 5};
  for (uint32_t p = 0; p < 6; ++p) {
    REQUIRE(SubdivCodeFromPosition(p) == codes[p]);
    REQUIRE(SubdivPositionFromCode(codes[p]) == p);
  }
  REQUIRE(SubdivCodeFromPosition(6) == 5u);
  REQUIRE(SubdivCodeFromPosition(UINT32_MAX) == 5u);
  REQUIRE(SubdivPositionFromCode(6) == 2u);
  REQUIRE(SubdivPositionFromCode(255) == 2u);
  // The grids the knob walks, coarse to fine: whole notes to thirty-seconds.
  const uint32_t grids[] = {96, 48, 24, 12, 6, 3};
  for (uint32_t p = 0; p < 6; ++p) REQUIRE(tempo::SubdivTicks(SubdivCodeFromPosition(p)) == grids[p]);
}

TEST_CASE("Host anchor: a host-style Start fires its downbeat at the anchor and nothing before",
          "[tempo][host]") {
  // §4.1, E9: the Start applies at the host block's first frame f and places boundary p at f + o,
  // so the grid Init or Restart placed at frame 0 fires nothing between f and the anchor. Blocks of
  // every size, the Start at the first frame of a block that begins mid-tick.
  for (const double bpm : {137.5, 140.0, 97.0}) {
    for (const double ppq : {3.37, 0.0, 7.999, 12.5}) {
      const uint32_t ns = NsPerQuarterFromBpm(bpm);
      Anchor         a  = AnchorOf(ppq, ns);
      REQUIRE(a.position != UINT32_MAX);
      for (const int64_t block : {int64_t{37}, int64_t{64}, int64_t{441}, int64_t{512}}) {
        INFO("bpm " << bpm << " ppq " << ppq << " block " << block);
        TempoCore c;
        c.Init(48000);
        c.SetStoredPerformance(500000, 0, 0);
        c.Restart();
        // The host's events at frame 0 (the restart's first block): Tempo, then Start.
        c.BeforeEvent(0);
        REQUIRE(c.ApplyEvent(0, tempo::kEventTempo, ns, 0u));
        c.BeforeEvent(0);
        REQUIRE(c.ApplyEvent(0, tempo::kEventTransport,
                             tempo::TransportId(TransportKind::Start, false, a.offset),
                             tempo::IntegerValueBits(a.position)));
        std::vector<GridHit> hits;
        GridHit              buf[TempoCore::kMaxClockPerSpan];
        for (int64_t s = 0; s < 4 * 48000; s += block) {
          const uint32_t n = c.GridFrames(s + block, buf, TempoCore::kMaxClockPerSpan);
          for (uint32_t i = 0; i < n; ++i) hits.push_back(buf[i]);
        }
        REQUIRE_FALSE(hits.empty());
        // The first hit is the first grid position (quarters: TAP) at or after the anchor, at its
        // own first frame; nothing fires before the anchor.
        REQUIRE(hits.front().frame >= static_cast<int64_t>(a.offset));
        const int64_t first = (static_cast<int64_t>(a.position) + 23) / 24 * 24;
        REQUIRE(hits.front().position == first);
        if (first == static_cast<int64_t>(a.position)) REQUIRE(hits.front().frame == a.offset);
        // And every hit lands where the host's own beat is, to a frame: beat b at ppq b, so
        // (b − ppq)·60/bpm·48,000 frames after the block's first frame.
        for (const GridHit& h : hits) {
          const double beat   = static_cast<double>(h.position) / 24.0;
          const double ideal  = (beat - ppq) * 60.0 / bpm * 48000.0;
          INFO("position " << h.position << " frame " << h.frame << " ideal " << ideal);
          REQUIRE(std::fabs(static_cast<double>(h.frame) - ideal) <= 1.5);
        }
      }
    }
  }
}
