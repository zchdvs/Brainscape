#pragma once
#include <cstdint>
#include <cstring>
#include <vector>

#include "WideInt.h"
#include "brainscape/Tempo.h"
#include "detail/Tempo.h"

// The reference model of the tempo core (docs/design/clock.md §8.2: "each rule is tested against a
// frame-by-frame reference in exact rational arithmetic"), written from the design's text and not
// from Tempo.cpp:
//
//   * the phasor is a closed form: boundary k lies at the exact rational frame
//     T(k) = sA + (k·P − c) / K, re-anchored only when an event changes it, so (tick, acc) at any
//     frame are derived by exact 128-bit floor division instead of TempoCore's running
//     Euclidean advance;
//   * the grid fires frame by frame, either literally (every frame of a span) or, between events,
//     at each position's closed-form first frame ⌈T(k)⌉;
//   * the follower keeps its window as a plain list of (label, frame) pairs with absolute labels,
//     relabelled pair by pair, and computes the sums, D, A, B, P_fit, residuals and placements
//     directly in 128-bit arithmetic on every tick (TempoCore keeps them incrementally, re-bases
//     them and stores 32-bit differences);
//   * gaps apply eagerly, at their deadline frame (TempoCore applies them lazily, before the next
//     event);
//   * rounding is spelled out by exact 128-bit division (TempoCore calls IntMath).
//
// The integer rules themselves (the tie directions, the thresholds, the order of the steps) are
// the design's, so the two agree only where both follow it. A Perturbation switches one rule to a
// plausible wrong reading, to prove the comparisons can fail.
namespace brainscape::testing {

enum class Perturb : uint8_t {
  None,
  NoCatchUp,          // draft v1's grid: no catch-up at span starts (finding E1)
  RescaleTruncates,   // acc′ = floor(acc·P′/P) instead of rounding half up
  TapMeanFloors,      // P = floor((Σ << 32) / n)
  NoDownbeat,         // the first apply after an arm re-phases to the nearest beat only
  StopKeepsArmed,     // draft v1's Stop leaves an armed Start in place (E10)
  DropoutFloors,      // m = floor(gap·K / P_fit) − 1 instead of rounding half up
  GapOnlyBeforeTempo, // draft v1's gap: only before tempo events (E2)
  OutlierBound,       // the outlier test's 10 ms bound at 11 ms
  SubdivCatchesUp,    // the build before note 25: a finer grid fires its last passed position
  NoHold,             // the build before note 26: the old grid fires while a transport is armed
  LocateArmedOnly,    // the build before note 27: Locate under ClockRunning keeps the continue
                      // position
};

// One event of a stream: an engine event of types 6-10 or above (an unknown), a non-tempo event
// (Other, which runs the gap rule only), a Spillover load, or a Probe (a read of Info() that
// changes nothing).
struct StreamEvent {
  enum class Kind : uint8_t { Tempo, Unknown, Other, Spillover, Probe };
  int64_t  frame = 0;
  Kind     kind  = Kind::Tempo;
  uint8_t  type  = 0;
  uint32_t id    = 0;
  uint32_t value = 0;  // binary32 bits
  // Spillover:
  uint32_t us = 500000;
  uint8_t  timeMode = 0, subdiv = 0;
  bool     recall = false;
};

class TempoRef {
 public:
  explicit TempoRef(uint32_t rate, Perturb perturb = Perturb::None) : R_(rate), perturb_(perturb) {
    pMin_ = PFromNs(tempo::kMinNsPerQuarter);
    pMax_ = PFromNs(tempo::kMaxNsPerQuarter);
    Restart();
    stats_ = TempoStats();
  }

  void SetStored(uint32_t us, uint8_t tm, uint8_t sd) {
    if (us < 200000 || us > 3000000 || tm > 2 || sd > 5) {
      us = 500000;
      tm = 0;
      sd = 0;
    }
    storedUs_ = us;
    storedTm_ = tm;
    storedSd_ = sd;
  }

  void Restart() {
    P_ = Pc_ = PFromNs(storedUs_ * 1000u);
    timeMode_ = storedTm_;
    subdiv_   = storedSd_;
    frame_    = 0;
    SetPhasor(0, -1, static_cast<int64_t>(P_));
    lastFired_ = lastGridFrame_ = -1;
    pcSerial_  = 0;
    pcChange_  = 0;
    source_    = 0;
    masterStopped_ = resumeRunning_ = running_ = armed_ = false;
    armedKind_ = 0;
    armedPosition_ = continuePosition_ = 0;
    haveTap_ = false;
    lastTap_ = 0;
    intervals_.clear();
    window_.clear();
    ring_.clear();
    haveLabel_ = haveTickRef_ = false;
    lastLabel_ = lastTickFrame_ = 0;
    outlierRun_ = 0;
    outlierSign_ = 0;
    driftRun_ = 0;
    earlyArmed_ = false;
  }

  // --- conversions, by exact 128-bit division --------------------------------------------------
  uint64_t PFromNs(uint32_t ns) const {
    // round-half-up(ns·R·2^32 / 10^9)
    return RoundHalfUp(W(static_cast<int64_t>(ns)) * W(R_) * W(int64_t{1} << 32), W(1000000000))
        .ToU64();
  }
  static I128 RoundHalfUp(I128 num, I128 den) {  // den > 0
    return I128::FloorDiv(num + num + den, den + den);
  }
  static I128 RoundHalfAway(I128 num, I128 den) {  // den != 0
    const bool neg = num.Negative() != den.Negative();
    const I128 an = num.Negative() ? -num : num, ad = den.Negative() ? -den : den;
    const I128 m = RoundHalfUp(an, ad);
    return neg ? -m : m;
  }

  // --- the closed-form phasor ------------------------------------------------------------------
  void SetPhasor(int64_t s, int64_t tick, int64_t acc) {
    sA_ = s;
    c_  = W(tick) * WU(P_) + W(acc);
  }
  // (tick, acc) at frame s: tick is the greatest k with T(k) < s.
  void TickAcc(int64_t s, int64_t* tick, int64_t* acc) const {
    const I128 x = W(s - sA_) * K() + c_;
    const I128 t = I128::CeilDiv(x, WU(P_)) - W(1);
    *tick = t.ToI64();
    *acc  = (x - t * WU(P_)).ToI64();
  }
  int64_t FirstFrame(int64_t k) const {  // ⌈T(k)⌉
    return sA_ + I128::CeilDiv(W(k) * WU(P_) - c_, K()).ToI64();
  }
  static I128 K() { return W(int64_t{24} << 32); }

  uint32_t Grid() const { return tempo::SubdivTicks(timeMode_ == 2 ? 0 : subdiv_); }

  // Fires the grid over [t0, t1), t0 being just after the events at t0 (§6.3's frame-by-frame
  // rule): at each frame t, the largest grid position whose boundary lies before t fires at t if
  // it has not fired; then the position whose boundary lies exactly at t. Literal: every frame;
  // otherwise between events in closed form.
  void Fire(int64_t t0, int64_t t1, std::vector<GridHit>* hits, bool literal) {
    const int64_t G = Grid();
    if (armed_ && perturb_ != Perturb::NoHold) {
      // §3.4 as amended (clock.md §11.12, note 26): while a MIDI transport waits for its tick the
      // grid is held. Nothing fires; every grid position whose first frame ⌈T(k)⌉ lies before t1
      // counts as fired.
      if (t1 <= t0) return;
      int64_t tick, acc;
      TickAcc(t1, &tick, &acc);
      int64_t last = tick;  // the greatest k with T(k) < t1; its first frame may be t1 itself
      if (FirstFrame(last) >= t1) --last;
      const int64_t g = I128::FloorDiv(W(last), W(G)).ToI64() * G;
      if (g > lastFired_) lastFired_ = g;
      return;
    }
    if (literal) {
      for (int64_t t = t0; t < t1; ++t) {
        int64_t tick, acc;
        TickAcc(t, &tick, &acc);
        // (a) the largest grid position whose boundary lies before t (draft v1 had no such
        // step: at one-frame spans it fired only boundaries lying exactly on a frame).
        if (perturb_ != Perturb::NoCatchUp) {
          const int64_t g = I128::FloorDiv(W(tick), W(G)).ToI64() * G;
          if (g > lastFired_) Hit(hits, t, g);
        }
        // (b) the position whose boundary lies exactly at t: tick + 1 with acc = P.
        if (acc == static_cast<int64_t>(P_) &&
            I128::FloorDiv(W(tick + 1), W(G)).ToI64() * G == tick + 1 && tick + 1 > lastFired_)
          Hit(hits, t, tick + 1);
      }
      return;
    }
    if (t1 <= t0) return;
    int64_t tick, acc;
    TickAcc(t0, &tick, &acc);
    if (perturb_ != Perturb::NoCatchUp) {
      const int64_t g = I128::FloorDiv(W(tick), W(G)).ToI64() * G;
      if (g > lastFired_) Hit(hits, t0, g);
    }
    int64_t k = (I128::FloorDiv(W(tick > lastFired_ ? tick : lastFired_), W(G)).ToI64() + 1) * G;
    for (;;) {
      const int64_t f = FirstFrame(k);
      if (f >= t1) break;
      Hit(hits, f, k);
      k += G;
    }
  }
  void Hit(std::vector<GridHit>* hits, int64_t f, int64_t k) {
    GridHit h;
    h.frame    = f;
    h.position = k;
    hits->push_back(h);
    lastFired_     = k;
    lastGridFrame_ = f;
  }

  // --- events ----------------------------------------------------------------------------------
  // The gap at its deadline: applied by the driver at lastTickFrame + R, before anything at or
  // after that frame (eager).
  bool GapDue(int64_t* deadline) const {
    if (!haveTickRef_) return false;
    *deadline = lastTickFrame_ + R_;
    return true;
  }
  void Gap() {
    window_.clear();
    ring_.clear();
    haveTickRef_ = false;
    lastTickFrame_ = 0;
    outlierRun_ = 0;
    outlierSign_ = 0;
    driftRun_ = 0;
    earlyArmed_ = false;
    ++stats_.gaps;
    if (source_ != 0) {
      ++stats_.losses;
      if (source_ == 2) {
        resumeRunning_ = true;
        running_ = false;
      }
      source_ = 0;
    }
  }

  // §6.3 as amended (clock.md §11.12, note 25): a change of the grid G starts the new grid at its
  // next position. The new grid's positions whose first frames lie before this frame count as
  // fired; one whose first frame is this frame still fires by the catch-up.
  void GridChanged(int64_t oldG) {
    const int64_t G = Grid();
    if (G == oldG || perturb_ == Perturb::SubdivCatchesUp) return;
    int64_t tick, acc;
    TickAcc(frame_, &tick, &acc);
    const int64_t g = I128::FloorDiv(W(tick), W(G)).ToI64() * G;
    if (g > lastFired_ && FirstFrame(g) < frame_) lastFired_ = g;
  }

  void Event(const StreamEvent& e) {
    frame_ = e.frame;
    switch (e.kind) {
      case StreamEvent::Kind::Other:
      case StreamEvent::Kind::Probe:
        return;
      case StreamEvent::Kind::Unknown:
        ++stats_.unknownEvents;
        return;
      case StreamEvent::Kind::Spillover: {
        const int64_t oldG = Grid();
        SetStored(e.us, e.timeMode, e.subdiv);
        timeMode_ = storedTm_;
        subdiv_   = storedSd_;
        if (e.recall && source_ == 0) InternalTempo(PFromNs(storedUs_ * 1000u));
        GridChanged(oldG);  // after the recalled tempo, as a Tempo event and then the grid
        return;
      }
      case StreamEvent::Kind::Tempo:
        break;
    }
    uint32_t position = 0;
    if (!Valid(e.type, e.id, e.value, &position)) {
      if (e.type >= 6 && e.type <= 10) ++stats_.invalidEvents;
      return;
    }
    switch (e.type) {
      case 6: Tap(); break;
      case 7:
        if (source_ != 0) {
          ++stats_.tempoIgnored;
        } else {
          ++stats_.tempoEvents;
          InternalTempo(PFromNs(e.id));
        }
        break;
      case 8: Tick(); break;
      case 9: Transport(e.id, position); break;
      case 10: {
        ++stats_.subdivEvents;
        const int64_t oldG = Grid();
        if (((e.id >> 8) & 0xFF) == 1) timeMode_ = static_cast<uint8_t>(e.id & 0xFF);
        else subdiv_ = static_cast<uint8_t>(e.id & 0xFF);
        GridChanged(oldG);
        break;
      }
      default: break;
    }
  }

  // §4.2, from the table.
  static bool Valid(uint8_t type, uint32_t id, uint32_t value, uint32_t* position) {
    *position = 0;
    switch (type) {
      case 6:
      case 8: return id == 0 && value == 0;
      case 7: return id >= 200000000u && id <= 3000000000u && value == 0;
      case 9: {
        const uint32_t kind = id & 3u, atNext = (id >> 8) & 1u, offset = id >> 16;
        if ((id & 0xFCu) != 0 || (id & 0xFE00u) != 0) return false;
        if (offset != 0 && (atNext != 0 || kind == 0)) return false;
        if (kind == 1 || kind == 3) {
          // An integer in [0, 6,291,456) as an exact binary32: decoded here through the float.
          float f;
          static_assert(sizeof f == 4, "binary32");
          std::memcpy(&f, &value, 4);
          if ((value >> 31) != 0) return false;  // −0 and negatives
          if (!(f >= 0.0f && f < 6291456.0f)) return false;
          const uint32_t n = static_cast<uint32_t>(f);
          if (static_cast<float>(n) != f) return false;
          if (value != 0 && ((value >> 23) & 0xFFu) == 0) return false;  // subnormals
          *position = n;
          return true;
        }
        return value == 0;
      }
      case 10: {
        if ((id >> 16) != 0 || value != 0) return false;
        const uint32_t code = id & 0xFF, field = (id >> 8) & 0xFF;
        return (field == 0 && code <= 5) || (field == 1 && code <= 2);
      }
      default: return false;
    }
  }

  void SetPcRef(uint64_t pc, bool drift) {
    if (pc == Pc_) return;
    const uint64_t diff = pc > Pc_ ? pc - Pc_ : Pc_ - pc;
    // A Jump: more than Pc >> 5, which is floor(Pc / 32).
    uint8_t c;
    if (WU(diff) > I128::FloorDiv(WU(Pc_), W(32))) {
      c = 1;
      ++stats_.jumps;
    } else if (drift) {
      c = 2;
      ++stats_.slews;
    } else {
      c = 3;
    }
    Pc_ = pc;
    pcChange_ = c;
    ++pcSerial_;
  }

  void InternalTempo(uint64_t np) {
    if (np != P_) {
      int64_t tick, acc;
      TickAcc(frame_, &tick, &acc);
      I128 a2 = perturb_ == Perturb::RescaleTruncates
                    ? I128::FloorDiv(W(acc) * WU(np), WU(P_))
                    : RoundHalfUp(W(acc) * WU(np), WU(P_));
      if (a2 < W(1)) a2 = W(1);
      P_ = np;
      SetPhasor(frame_, tick, a2.ToI64());
    }
    SetPcRef(np, false);
  }

  int64_t Nearest(int64_t tick, int64_t acc) const {
    return W(2) * W(acc) >= WU(P_) ? tick + 1 : tick;
  }
  void Relabel(int64_t delta) {
    for (auto& t : window_) t.label += delta;
    for (auto& t : ring_) t.label += delta;
    lastLabel_ += delta;
  }

  // §3.2
  void Tap() {
    ++stats_.taps;
    if (source_ == 2) {
      ++stats_.tapsIgnored;
      return;
    }
    int64_t tick, acc;
    TickAcc(frame_, &tick, &acc);
    if (source_ == 1) {
      ++stats_.tapPhases;
      const int64_t kn = Nearest(tick, acc);
      const int64_t target = I128::FloorDiv(W(kn + 48), W(96)).ToI64() * 96;
      const int64_t delta = target - kn;
      Relabel(delta);
      SetPhasor(frame_, tick + delta, acc);
      lastFired_ += delta;
      return;
    }
    if (!haveTap_) {
      haveTap_ = true;
      lastTap_ = frame_;
      intervals_.clear();
      return;
    }
    const int64_t I = frame_ - lastTap_;
    // I < R/5, exactly: 5I < R.
    if (W(I) * W(5) < W(R_)) {
      ++stats_.tapsIgnored;
      return;
    }
    int64_t sum = 0;
    for (int64_t v : intervals_) sum += v;
    const int64_t n = static_cast<int64_t>(intervals_.size());
    // A pause: I > 3R, or I ≥ 1.75 × the mean (I·n ≥ 1.75·Σ).
    if (I > 3 * static_cast<int64_t>(R_) || (n > 0 && W(4) * W(n) * W(I) >= W(7) * W(sum))) {
      lastTap_ = frame_;
      intervals_.clear();
      return;
    }
    const bool first = n == 0;
    // More than 40 % from the mean: |I − Σ/n| > 0.4·Σ/n.
    I128 dev = W(n) * W(I) - W(sum);
    if (dev.Negative()) dev = -dev;
    if (n > 0 && W(5) * dev > W(2) * W(sum)) {
      intervals_.assign(1, I);
    } else {
      intervals_.push_back(I);
      if (intervals_.size() > 4) intervals_.erase(intervals_.begin());
    }
    lastTap_ = frame_;
    // Apply: the beat in the old units.
    const int64_t b = I128::FloorDiv(W(tick), W(24)).ToI64();
    const I128 x = W(tick - 24 * b) * WU(P_) + W(acc);
    int64_t B = W(2) * x >= W(24) * WU(P_) ? 24 * (b + 1) : 24 * b;
    if (first && perturb_ != Perturb::NoDownbeat) {
      while (I128::FloorDiv(W(B - 24), W(96)) * W(96) != W(B - 24)) ++B;  // B ≡ 24 (mod 96)
    }
    int64_t s2 = 0;
    for (int64_t v : intervals_) s2 += v;
    const int64_t n2 = static_cast<int64_t>(intervals_.size());
    I128 np = perturb_ == Perturb::TapMeanFloors
                  ? I128::FloorDiv(W(s2) * W(int64_t{1} << 32), W(n2))
                  : RoundHalfUp(W(s2) * W(int64_t{1} << 32), W(n2));
    uint64_t p = np.ToU64();
    if (p < pMin_) p = pMin_;
    if (p > pMax_) p = pMax_;
    P_ = p;
    SetPcRef(p, false);
    SetPhasor(frame_, B - 1, static_cast<int64_t>(p));
    if (lastFired_ < B - 1) lastFired_ = B - 1;
  }

  // §3.4
  void Transport(uint32_t id, uint32_t position) {
    const uint32_t kind = id & 3u;
    const bool atNext = ((id >> 8) & 1u) != 0;
    const uint32_t offset = id >> 16;
    if (atNext) {
      ++stats_.transports;
      if (kind == 0) {  // Stop
        if (armed_ && perturb_ != Perturb::StopKeepsArmed) {
          armed_ = false;
          ++stats_.transportsCancelled;
        }
        source_ = 0;
        if (haveLabel_) continuePosition_ = lastLabel_ + 1;
        masterStopped_ = true;
        resumeRunning_ = false;
        running_ = false;
        earlyArmed_ = false;
      } else if (kind == 3 && source_ != 2) {
        continuePosition_ = position;
      } else {
        // A Locate under ClockRunning is armed, and sets the continue position too (note 27).
        if (kind == 3 && perturb_ != Perturb::LocateArmedOnly) continuePosition_ = position;
        armed_ = true;
        armedKind_ = static_cast<uint8_t>(kind);
        armedPosition_ = position;
      }
      return;
    }
    if (source_ != 0) {
      ++stats_.transportsIgnored;
      return;
    }
    ++stats_.transports;
    if (kind == 0) {
      running_ = false;
      return;
    }
    const int64_t k = kind == 2 ? continuePosition_ : static_cast<int64_t>(position);
    // Boundary k at frame + offset: the design's loop, literally.
    int64_t tick = k - 1;
    I128 acc = WU(P_) - W(offset) * K();
    while (acc <= W(0)) {
      acc = acc + WU(P_);
      --tick;
    }
    SetPhasor(frame_, tick, acc.ToI64());
    lastFired_ = k - 1;
    if (kind != 3) running_ = true;
  }

  // The fit, directly from the window.
  struct Fit {
    bool valid = false;
    int64_t n = 0, sx = 0, sy = 0, sxx = 0, sxy = 0;
    I128 D, A, B;
    uint64_t pFit = 0;
    int64_t label0 = 0, frame0 = 0;
  };
  Fit FitNow() const {
    Fit f;
    f.n = static_cast<int64_t>(window_.size());
    if (f.n == 0) return f;
    f.label0 = window_.front().label;
    f.frame0 = window_.front().frame;
    for (const auto& t : window_) {
      const int64_t x = t.label - f.label0, y = t.frame - f.frame0;
      f.sx += x;
      f.sy += y;
      f.sxx += x * x;
      f.sxy += x * y;
    }
    if (f.n < 2) return f;
    const I128 N = W(f.n);
    f.D = N * W(f.sxx) - W(f.sx) * W(f.sx);
    f.A = N * W(f.sxy) - W(f.sx) * W(f.sy);
    f.B = W(f.sy) * W(f.sxx) - W(f.sx) * W(f.sxy);
    f.valid = f.D > W(0) && f.A > W(0);
    if (f.valid) {
      uint64_t p = RoundHalfUp(W(24) * f.A * W(int64_t{1} << 32), f.D).ToU64();
      if (p < pMin_) p = pMin_;
      if (p > pMax_) p = pMax_;
      f.pFit = p;
    }
    return f;
  }
  static I128 Rho(const Fit& f, int64_t label, int64_t frame) {
    return W(frame - f.frame0) * f.D - f.B - f.A * W(label - f.label0);
  }

  // §3.3
  void Tick() {
    ++stats_.ticks;
    const Fit before = FitNow();
    const bool wasEmpty = window_.empty();
    int64_t label;
    if (haveLabel_) {
      label = lastLabel_ + 1;
      if (before.valid && before.n >= 8 && haveTickRef_) {
        const int64_t gap = frame_ - lastTickFrame_;
        if (W(gap) * K() >= W(4) * WU(before.pFit) && W(10) * W(gap) >= W(R_)) {
          const int64_t m = (perturb_ == Perturb::DropoutFloors
                                 ? I128::FloorDiv(W(gap) * K(), WU(before.pFit))
                                 : RoundHalfUp(W(gap) * K(), WU(before.pFit)))
                                .ToI64() - 1;
          label += m;
          stats_.dropoutTicks += static_cast<uint64_t>(m);
        }
      }
    } else {
      int64_t tick, acc;
      TickAcc(frame_, &tick, &acc);
      label = Nearest(tick, acc);
    }
    bool fitted = true;
    if (before.valid && before.n >= 8) {
      I128 rho = Rho(before, label, frame_);
      const bool positive = rho > W(0);
      if (rho.Negative()) rho = -rho;
      // A quarter tick (4·|ρD| > A) and 10 ms (|ρD|/D > R/100) off the line; the perturbation
      // reads 11 ms.
      const bool farOff = perturb_ == Perturb::OutlierBound
                              ? W(1000) * rho > W(11) * W(R_) * before.D
                              : W(100) * rho > W(R_) * before.D;
      if (W(4) * rho > before.A && farOff) {
        fitted = false;
        ++stats_.tickOutliers;
        const int32_t sign = positive ? 1 : -1;
        if (outlierRun_ > 0 && sign == outlierSign_) {
          ++outlierRun_;
        } else {
          outlierRun_ = 1;
          outlierSign_ = sign;
        }
        driftRun_ = 0;
      }
    }
    if (fitted) {
      outlierRun_ = 0;
      outlierSign_ = 0;
    }
    ring_.push_back({label, frame_});
    if (ring_.size() > 6) ring_.erase(ring_.begin());
    lastLabel_ = label;
    haveLabel_ = true;
    lastTickFrame_ = frame_;
    haveTickRef_ = true;
    const uint32_t prevN = static_cast<uint32_t>(window_.size());
    if (outlierRun_ >= 6) {
      window_ = ring_;
      ++stats_.reacquires;
      outlierRun_ = 0;
      outlierSign_ = 0;
    } else if (fitted) {
      window_.push_back({label, frame_});
    }
    // Labels out of the last 96 leave.
    std::vector<Pair> kept;
    for (const auto& t : window_)
      if (t.label > label - 96) kept.push_back(t);
    window_.swap(kept);
    const Fit fit = FitNow();
    bool entered = false;
    if (armed_) {
      const int64_t p = armedKind_ == 2 ? continuePosition_ : armedPosition_;
      Relabel(p - label);
      label = p;
      lastFired_ = p - 1;
      masterStopped_ = false;
      resumeRunning_ = false;
      running_ = true;
      earlyArmed_ = wasEmpty;
      source_ = 2;
      armed_ = false;
      entered = true;
    } else if (source_ == 0 && !masterStopped_ && fit.n >= 24 && fit.valid) {
      int64_t tick, acc;
      TickAcc(frame_, &tick, &acc);
      const int64_t kn = Nearest(tick, acc);
      Relabel(kn - label);
      label = kn;
      if (resumeRunning_) {
        source_ = 2;
        running_ = true;
        ++stats_.resumes;
      } else {
        source_ = 1;
      }
      entered = true;
    }
    if (source_ == 0) return;
    const Fit f2 = FitNow();  // relabelled: the same numbers
    // Place the phasor on the line at this frame.
    if (!f2.valid) {
      SetPhasor(frame_, label - 1, static_cast<int64_t>(P_));
    } else {
      const I128 d = RoundHalfAway(K() * Rho(f2, label, frame_), f2.D);
      const I128 j = I128::CeilDiv(d, WU(f2.pFit)) - W(1);
      P_ = f2.pFit;
      SetPhasor(frame_, label + j.ToI64(), (d - j * WU(f2.pFit)).ToI64());
    }
    // Commit (§7.1).
    if (!f2.valid) return;
    const uint64_t pf = f2.pFit;
    const uint64_t diff = pf > Pc_ ? pf - Pc_ : Pc_ - pf;
    const uint32_t N = static_cast<uint32_t>(f2.n);
    if ((entered && N >= 24) || (prevN < 24 && N >= 24)) {
      ++stats_.commits;
      SetPcRef(pf, false);
      driftRun_ = 0;
      return;
    }
    if (earlyArmed_ && prevN < 12 && N >= 12) {
      earlyArmed_ = false;
      if (WU(diff) > I128::FloorDiv(WU(Pc_), W(16))) {  // Pc >> 4
        ++stats_.earlyCommits;
        SetPcRef(pf, false);
        driftRun_ = 0;
      }
      return;
    }
    if (N < 24) return;
    if (WU(diff) > I128::FloorDiv(WU(Pc_), W(512))) {
      ++stats_.commits;
      SetPcRef(pf, true);
      driftRun_ = 0;
      return;
    }
    if (!fitted) return;
    if (WU(diff) > I128::FloorDiv(WU(Pc_), W(4096))) {
      if (++driftRun_ >= 192) {
        ++stats_.commits;
        SetPcRef(pf, true);
        driftRun_ = 0;
      }
    } else {
      driftRun_ = 0;
    }
  }

  TempoInfo Info(int64_t at) const {
    // §2.6: the snapshot sees §3.5's gap without applying it. The driver applies gaps eagerly
    // only while an event is still to come, so a snapshot after the last event reads a copy that
    // applies the gap.
    int64_t deadline;
    if (GapDue(&deadline) && deadline <= at) {
      TempoRef copy = *this;
      copy.Gap();
      return copy.Info(at);
    }
    TempoInfo i;
    int64_t tick, acc;
    TickAcc(at, &tick, &acc);
    i.position = tick;
    i.nsPerQuarter = static_cast<uint32_t>(
        RoundHalfUp(WU(Pc_) * W(1000000000), W(R_) * W(int64_t{1} << 32)).ToU64());
    i.source = source_;
    i.timeMode = timeMode_;
    i.subdiv = subdiv_;
    i.flags = static_cast<uint8_t>((running_ ? kTempoFlagRunning : 0) |
                                   (window_.size() >= 24 ? kTempoFlagLocked : 0));
    i.lastGridFrame = lastGridFrame_;
    i.lastClockBirth = -1;
    return i;
  }

  TempoCore::State Capture() const {
    TempoCore::State s;
    s.frame = frame_;
    TickAcc(frame_, &s.tick, &s.acc);
    s.p = P_;
    s.pc = Pc_;
    s.lastFired = lastFired_;
    s.lastGridFrame = lastGridFrame_;
    s.lastClockBirth = -1;
    s.timeMode = timeMode_;
    s.subdiv = subdiv_;
    s.source = source_;
    s.storedUs = storedUs_;
    s.storedTimeMode = storedTm_;
    s.storedSubdiv = storedSd_;
    s.pcSerial = pcSerial_;
    s.pcChange = pcChange_;
    s.masterStopped = masterStopped_;
    s.resumeRunning = resumeRunning_;
    s.running = running_;
    s.armed = armed_;
    s.armedKind = armed_ ? armedKind_ : 0;
    s.armedPosition = armed_ ? armedPosition_ : 0;
    s.continuePosition = continuePosition_;
    s.haveTap = haveTap_;
    s.lastTap = haveTap_ ? lastTap_ : 0;
    s.tapN = static_cast<uint32_t>(intervals_.size());
    for (size_t i = 0; i < intervals_.size(); ++i) s.tapIntervals[i] = intervals_[i];
    s.winN = static_cast<uint32_t>(window_.size());
    for (size_t i = 0; i < window_.size(); ++i) {
      s.winLabel[i] = window_[i].label;
      s.winFrame[i] = window_[i].frame;
    }
    const Fit f = FitNow();
    s.sx = f.sx;
    s.sy = f.sy;
    s.sxx = f.sxx;
    s.sxy = f.sxy;
    s.fitValid = f.valid;
    s.d = f.n >= 2 ? f.D.ToI64() : 0;
    s.a = f.n >= 2 ? f.A.ToI64() : 0;
    s.b = f.n >= 2 ? f.B.ToI64() : 0;
    s.pFit = f.valid ? f.pFit : 0;
    s.ringN = static_cast<uint32_t>(ring_.size());
    for (size_t i = 0; i < ring_.size(); ++i) {
      s.ringLabel[i] = ring_[i].label;
      s.ringFrame[i] = ring_[i].frame;
    }
    s.haveLabel = haveLabel_;
    s.lastLabel = haveLabel_ ? lastLabel_ : 0;
    s.haveTickRef = haveTickRef_;
    s.lastTickFrame = haveTickRef_ ? lastTickFrame_ : 0;
    s.outlierRun = outlierRun_;
    s.outlierSign = outlierSign_;
    s.driftRun = driftRun_;
    s.earlyArmed = earlyArmed_;
    return s;
  }

  const TempoStats& Stats() const { return stats_; }
  Perturb perturbation() const { return perturb_; }

 private:
  struct Pair {
    int64_t label;
    int64_t frame;
  };

  uint32_t R_;
  Perturb  perturb_;
  uint64_t pMin_ = 0, pMax_ = 0;
  uint32_t storedUs_ = 500000;
  uint8_t  storedTm_ = 0, storedSd_ = 0;

  int64_t  frame_ = 0;
  int64_t  sA_ = 0;
  I128     c_;
  uint64_t P_ = 1, Pc_ = 1;
  int64_t  lastFired_ = -1, lastGridFrame_ = -1;
  uint32_t pcSerial_ = 0;
  uint8_t  pcChange_ = 0;
  uint8_t  timeMode_ = 0, subdiv_ = 0, source_ = 0;
  bool     masterStopped_ = false, resumeRunning_ = false, running_ = false, armed_ = false;
  uint8_t  armedKind_ = 0;
  int64_t  armedPosition_ = 0, continuePosition_ = 0;
  bool     haveTap_ = false;
  int64_t  lastTap_ = 0;
  std::vector<int64_t> intervals_;
  std::vector<Pair>    window_, ring_;
  bool     haveLabel_ = false, haveTickRef_ = false;
  int64_t  lastLabel_ = 0, lastTickFrame_ = 0;
  uint32_t outlierRun_ = 0;
  int32_t  outlierSign_ = 0;
  uint32_t driftRun_ = 0;
  bool     earlyArmed_ = false;
  TempoStats stats_;
};

}  // namespace brainscape::testing
