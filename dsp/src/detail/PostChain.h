#pragma once
#include "detail/FpProfilePrivate.h"

#include <cassert>
#include <cstdint>
#include <cstring>

#include "detail/DetMath.h"
#include "detail/FlushTiny.h"
#include "detail/Placement.h"
#include "detail/Smoother.h"

// Post chain (docs/design/grain-engine.md §2.6) and the fixed feedback taming
// chain (§2.3). Internal; free to change.
//
// Determinism rules (design §10 contracts #1/#7): every per-sample nonlinearity
// and oscillator here is in-tree arithmetic (parabolic sine, Padé tanh) or an
// IEEE-exact operation (sqrtf) — no libm anywhere. Control-rate coefficient
// math (DetMath sin/expm1) runs at Init or when a parameter changes.
namespace brainscape::detail {

// ── Small primitives ────────────────────────────────────────────────────────────

// Parabolic sine approximation of sin(2π·phase) on a [0,1) phase — deterministic,
// in-tree, plenty for LFO duty (peak error ~5.6%, odd-symmetric).
inline float CheapSine(float phase01) noexcept {
  float x = phase01 < 0.5f ? phase01 : phase01 - 1.0f;  // [-0.5, 0.5)
  return x * (8.0f - 16.0f * (x < 0.f ? -x : x));       // peaks exactly ±1 at ±0.25
}

// Padé tanh — bounded soft saturator (design §2.3). C1-continuous at the ±3 seam
// (value and derivative both match), exact 0 at 0.
inline float SoftSat(float x) noexcept {
  if (x > 3.0f) return 1.0f;
  if (x < -3.0f) return -1.0f;
  // Below 2^-63 the square is subnormal, and the flushed tamer state fed in here
  // reaches down to 1e-20 (determinism profile §4.3). 27 absorbs such a square, so
  // squaring 0 gives the same bits without the subnormal operation. Selecting on the
  // bits before the multiply keeps a compiler from speculating x * x.
  uint32_t u;
  std::memcpy(&u, &x, sizeof u);
  if ((u & 0x7FFFFFFFu) < 0x20000000u) u = 0u;
  float xs;
  std::memcpy(&xs, &u, sizeof u);
  const float x2 = xs * xs;
  return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

// Schroeder allpass over a caller-provided buffer slice. The stored state is flushed
// (determinism profile §4.3).
struct Allpass {
  float*   buf = nullptr;
  uint32_t len = 0, pos = 0;
  void Init(float* b, uint32_t n) noexcept {
    buf = b;
    len = n;
    pos = 0;
  }
  void Clear() noexcept {
    for (uint32_t i = 0; i < len; ++i) buf[i] = 0.f;
    pos = 0;
  }
  float Process(float x, float g) noexcept {
    const float v = buf[pos];
    const float y = v - g * x;
    float       w = x + g * y;
    FlushTiny(w);
    buf[pos] = w;
    if (++pos == len) pos = 0;
    return y;
  }
  float TapBack(uint32_t back) const noexcept {  // read inside the line, no state change
    uint32_t i = pos + len - back;
    if (i >= len) i -= len;
    return buf[i];
  }
};

// Plain delay line over a caller-provided slice, with an optional modulated
// (linearly interpolated) read for the reverb tank. Written values are flushed
// (determinism profile §4.3): the post delay and reverb tank recirculate.
struct DelaySlice {
  float*   buf = nullptr;
  uint32_t len = 0, pos = 0;
  void Init(float* b, uint32_t n) noexcept {
    buf = b;
    len = n;
    pos = 0;
  }
  void Clear() noexcept {
    for (uint32_t i = 0; i < len; ++i) buf[i] = 0.f;
    pos = 0;
  }
  void  Write(float x) noexcept {
    FlushTiny(x);
    buf[pos] = x;
    if (++pos == len) pos = 0;
  }
  float ReadBack(uint32_t back) const noexcept {  // back in [1, len]
    uint32_t i = pos + len - back;
    if (i >= len) i -= len;
    return buf[i];
  }
  float ReadBackLerp(float back) const noexcept {  // back in [1, len-1)
    assert(back >= 1.0f && back < static_cast<float>(len - 1u));  // profile §3.10: bounded excursion
    const auto  b0 = static_cast<uint32_t>(back);
    const float fr = back - static_cast<float>(b0);
    const float a  = ReadBack(b0);
    const float b  = ReadBack(b0 + 1u);
    return a + (b - a) * fr;
  }
};

// The post delay's stereo line over one caller-provided slice of 2 * len floats, the channels
// interleaved (left at 2i, right at 2i + 1): a frame's two samples share a cache line and one
// index computation, where the two DelaySlices it replaces were two streams 2 s apart in the
// Bulk arena. It holds the values two DelaySlices in lockstep held, flushed the same way.
struct StereoDelaySlice {
  float*   buf = nullptr;
  uint32_t len = 0, pos = 0;  // frames
  void Init(float* b, uint32_t n) noexcept {
    buf = b;
    len = n;
    pos = 0;
  }
  void Clear() noexcept {
    for (uint32_t i = 0; i < 2u * len; ++i) buf[i] = 0.f;
    pos = 0;
  }
  void Write(float l, float r) noexcept {
    FlushTiny(l);
    FlushTiny(r);
    buf[2u * pos]      = l;
    buf[2u * pos + 1u] = r;
    if (++pos == len) pos = 0;
  }
  const float* Frame(uint32_t back) const noexcept {  // back in [1, len]: {left, right}
    uint32_t i = pos + len - back;
    if (i >= len) i -= len;
    return buf + 2u * i;
  }
};

// The post delay's read head (determinism profile §5.6, companion §4.11). A time change
// glides the tap to its new integer target instead of splicing it, bending pitch like
// tape: two one-poles in cascade, a critically damped pair, keep the read speed continuous
// (a speed step is a kink in the waveform), and the speed cap holds a large change to
// 0.5x-1.5x playback instead of racing through the line. At rest the head sits exactly
// on the target and reads the integer tap, so static settings sound as they did before
// the glide existed.
//
// The head is base + frac with frac in [-0.5, 0.5): the fraction keeps full float
// precision anywhere in a 4 s line, a retarget never moves the head, and the head lands
// from either side without the stall a fraction just below 1 would hit.
//
// Since sound revision 9 (docs/design/clock.md §7.2) the pair has a second coefficient set, a
// slew with τ = 1 s per pole, which a synced target's Drift (a clock's deadband commit) selects
// (`slow`); every other retarget glides with the 50 ms pair, and a later retarget replaces the
// choice. The caller passes the chosen pair to Step. The slew's speed is capped at 2^-10 frames a
// frame, so a Drift bends the repeats at most 0.098 % (§7.4's 0.1 %) on any echo: its τ alone
// bends a change of Δ frames by Δ/(e·τ), past the budget on echoes longer than about 0.8 s.
struct TapGlide {
  static constexpr float kMaxSpeed  = 0.5f;      // frames of head movement per frame
  static constexpr float kSlewSpeed = 0x1p-10f;  // the slew's cap: 0.098 % of playback speed
  static constexpr float kSnap      = 0x1p-16f;  // frames from the target that count as on it

  uint32_t target = 2;    // frames behind the write head, in [2, len - 1]
  uint32_t base   = 2;    // the head: base + frac frames behind the write head
  float    frac   = 0.f;
  float    lead   = 0.f;  // the first pole's position minus target
  bool     moving = false;
  bool     slow   = false;  // the last retarget was a Drift: Step takes the slew's pair

  void Prime(uint32_t t) noexcept {
    target = base = t;
    frac = lead = 0.f;
    moving = false;
    slow   = false;
  }
  void Retarget(uint32_t t) noexcept {
    if (t == target) return;
    // Integers below 2^24, so the difference is exact; the first pole stays put.
    lead += static_cast<float>(target) - static_cast<float>(t);
    target = t;
    moving = true;
  }
  // One frame. coef is each pole's one-pole coefficient and keep is 1 - coef.
  void Step(float coef, float keep) noexcept {
    lead *= keep;
    FlushTiny(lead);
    const float pos = (static_cast<float>(base) - static_cast<float>(target)) + frac;
    float       speed = coef * (lead - pos);
    const float cap   = slow ? kSlewSpeed : kMaxSpeed;
    if (speed > cap) speed = cap;
    if (speed < -cap) speed = -cap;
    float f = frac + speed;  // in [-1, 1); the carries below are exact (Sterbenz)
    if (f >= 0.5f) {
      f -= 1.0f;
      ++base;
    } else if (f < -0.5f) {
      f += 1.0f;
      --base;
    }
    frac = f;
    if (base == target && detmath::Abs(frac) <= kSnap && detmath::Abs(lead) <= kSnap) {
      Prime(target);
    }
  }
  // Catmull-Rom (Granular's ReadHermite) toward the neighbour on frac's side; exactly the
  // integer tap at frac 0. A linear read low-passed the moving head by up to cos(pi f/fs),
  // -2 dB at 10 kHz, as a tremolo while frac cycles (review finding); this is -0.54 dB.
  // Both channels, each with the same arithmetic.
  void Read(const StereoDelaySlice& d, float* l, float* r) const noexcept {
    // Targets are in [2, len - 1] and the head stays between them, so every tap is a frame
    // of the line.
    assert(base >= 2u && base <= d.len - 1u);
    const float* f0 = d.Frame(base);
    if (frac == 0.0f) {
      *l = f0[0];
      *r = f0[1];
      return;
    }
    const bool     up  = frac > 0.0f;
    const uint32_t i1  = up ? base + 1u : base - 1u;
    const uint32_t im1 = up ? base - 1u : base + 1u;
    const uint32_t i2  = up ? base + 2u : base - 2u;
    assert(i2 >= 1u && i2 <= d.len);
    const float* fm1 = d.Frame(im1);
    const float* f1  = d.Frame(i1);
    const float* f2  = d.Frame(i2);
    const float  t   = detmath::Abs(frac);
    *l               = CatmullRom(fm1[0], f0[0], f1[0], f2[0], t);
    *r               = CatmullRom(fm1[1], f0[1], f1[1], f2[1], t);
  }

 private:
  static float CatmullRom(float xm1, float x0, float x1, float x2, float t) noexcept {
    const float c    = (x1 - xm1) * 0.5f;
    const float v    = x0 - x1;
    const float w    = c + v;
    const float a    = w + v + (x2 - x0) * 0.5f;
    const float bNeg = w + a;
    return (((a * t) - bNeg) * t + c) * t + x0;
  }
};

// ── Feedback taming chain (design §2.3; fixed order, not user-reorderable) ─────
// DC block -> HP ~100 Hz -> LP 8k->4k (feedback-dependent) -> soft saturation ->
// 2-stage allpass diffusion per channel. This is what makes feedback > 1.0 a
// bounded self-oscillation feature instead of a rail.
class FeedbackTamer {
 public:
  static uint32_t WarmFloats(double sampleRate) noexcept;
  void Init(float* warm, double sampleRate) noexcept;
  void Reset() noexcept;
  void ClearDiffusers() noexcept;  // the allpass buffers: Init and Restart, never Reset
  // Per-block coefficient update (control rate; fbAmount sets the LP corner).
  void SetFeedback(float fbAmount, double sampleRate) noexcept;
  void ProcessSample(float& l, float& r) noexcept;

 private:
  float   dcL_ = 0.f, dcR_ = 0.f;      // DC-blocker LP state
  float   hpL_ = 0.f, hpR_ = 0.f;      // 100 Hz HP (via one-pole LP state)
  float   lpL_ = 0.f, lpR_ = 0.f;      // damping LP state
  float   dcCoef_ = 0.f, hpCoef_ = 0.f, lpCoef_ = 1.f;
  Allpass apL_[2]{}, apR_[2]{};
};

// ── Post chain: ordered, bypassable stage list (design §2.6) ───────────────────

enum class Stage : uint8_t { Mod = 0, Delay = 1, Reverb = 2, Filter = 3 };
inline constexpr uint32_t kPostStages = 4;

struct PostParams {
  float modRateHz    = 0.4f;
  float modDepth     = 0.0f;   // drives excursion AND a wet mix capped at 0.5 so a
                               // dry term always survives (chorus, not vibrato);
                               // 0 = exactly transparent
  float delayFrames  = 16800;  // post.delay.time_ms in frames; the tap glides to it (TapGlide)
  // Synced times (sound revision 9, docs/design/clock.md §6.1, §7): with post.delay.sync nonzero,
  // the target in exact integer frames (§2.3, folded into 10 ms to tempo::PostSyncMaxFrames, a
  // little over 4 s, §5.3), which replaces delayFrames; 0 when unsynced. The engine raises
  // delayJump for a change of the target the head must crossfade to (a Jump of the committed
  // tempo, or a discrete change: the code, the effective Subdiv, the fold, sync on or off), and
  // sets delaySlow when the target's last change was a Drift, which the head slews to (τ = 1 s, at
  // most 2^-10 frames a frame); any other change glides.
  uint32_t delaySyncFrames = 0;
  uint32_t delayJump       = 0;
  bool     delaySlow       = false;
  float delayFb      = 0.3f;
  float delayMix     = 0.0f;   // equal-power; 0 = exactly transparent
  float reverbTime   = 0.5f;
  float reverbMix    = 0.0f;   // equal-power; 0 = exactly transparent
  float filterCutoff = 20000.f;
  float filterRes    = 0.1f;
  float filterMorph  = 0.0f;   // 0..3 continuous LP -> BP -> HP -> Notch (equal-power)
  bool  filterBypass = true;   // cutoff at descriptor max ramps the insert mix to an
                               // EXACT bypass (design §2.6: click-free crossfade,
                               // and the null contracts' transparency)
};

class PostChain {
 public:
  // A jump of the post delay's target (docs/design/clock.md §7.3): the outgoing head and the
  // incoming one, primed on the new target, mixed over this many frames with gains
  // (1024 - n) / 1024 and n / 1024 (n = 1-1024), exact multiples of 2^-10. 21.3 ms at 48 kHz.
  static constexpr uint32_t kXfadeFrames = 1024;

  // Warm arena: mod lines + reverb tank. Bulk arena: the stereo post-delay buffer.
  static uint32_t WarmFloats(double sampleRate) noexcept;
  static uint32_t BulkFloats(double sampleRate) noexcept;

  void Init(float* warm, float* bulk, double sampleRate) noexcept;
  // RT-safe: clears small filter/LFO state and primes smoothers from the given
  // params. Does NOT memset the delay/reverb buffers (750 KiB of SDRAM — review
  // finding: that made Engine::Reset miss 2-4 audio deadlines); stale tails are
  // masked by the mix ramps and fully cleared by the non-RT ClearBuffers().
  void Reset(const PostParams& p) noexcept;
  void ClearBuffers() noexcept;  // non-RT: memsets mod/delay/reverb storage
  // In-place over the block. Stage mixes are smoothed per sample internally; a
  // stage whose mix target and smoothed value are both exactly 0 is skipped
  // (its buffers freeze while bypassed — normal bypass semantics).
  void Process(const PostParams& p, uint32_t numFrames, float* l, float* r) noexcept;

  // Crossfades started since Init (Engine::TempoCounts' crossfades, clock.md §7.3).
  uint64_t Crossfades() const noexcept { return crossfades_; }

 private:
  void     UpdateFilterCoefs(float cutoff, float res, float morph) noexcept;  // control rate
  // The head's target: the synced frames when the engine gives them, else post.delay.time_ms's,
  // each clamped into the line.
  uint32_t DelayTarget(const PostParams& p) const noexcept;
  // A crossfade from the head as it is to a head primed on `tap` (§7.3).
  void StartFade(uint32_t tap) noexcept {
    pdOut_ = pdTap_;
    pdTap_.Prime(tap);
    pdFadePos_ = 0;
    pdFading_  = true;
    ++crossfades_;
  }
  // One frame of a head: its glide or slew step when it moves, then its read.
  void HeadFrame(TapGlide& head, float* l, float* r) noexcept {
    if (head.moving) {
      head.Step(head.slow ? pdSlewCoef_ : pdGlideCoef_, head.slow ? pdSlewKeep_ : pdGlideKeep_);
      head.Read(pd_, l, r);
    } else {
      const float* f = pd_.Frame(head.base);
      *l             = f[0];
      *r             = f[1];
    }
  }
  // One frame of a crossfade (§7.3), out of line: it runs for 1,024 frames a jump, and the head
  // frame it repeats is the per-sample loop's, which stays inline.
  BRAINSCAPE_NOINLINE void FadeFrame(float* tapL, float* tapR) noexcept;

  double sr_ = 48000.0;

  // Mod (stereo chorus): one LFO, quadrature R, modulated tap; wet capped at 0.5.
  DelaySlice modL_{}, modR_{};
  float      lfoPhase_  = 0.f;
  float      modCenter_ = 0.f, modMaxExc_ = 0.f;  // frames, fixed at Init
  Smoother   modDepthSm_{};

  // Post delay (stereo, Bulk) with damped, DC-blocked regeneration. The line holds
  // tempo::PostSyncMaxFrames(R) + 2 frames since sound revision 9 (clock.md §5.3): a synced target
  // reaches 4·R and 2^-7 of it, post.delay.time_ms keeps its clamp at round(2·R) - 1, and a read
  // `back` frames behind the write head returns the same frame in the longer line.
  StereoDelaySlice pd_{};
  TapGlide   pdTap_{};  // the head; during a crossfade the incoming one
  TapGlide   pdOut_{};  // during a crossfade, the outgoing head, still reading and gliding
  uint32_t   pdTimeMax_ = 2;      // post.delay.time_ms's clamp: round(2·R) - 1, fixed at Init
  uint32_t   pdJumpSeen_ = 0;     // PostParams::delayJump as last seen
  uint32_t   pdFadePos_  = 0;     // frames of the crossfade done, 1-kXfadeFrames
  uint32_t   pdPendingTap_ = 0;   // the latest jump's target while a fade runs
  bool       pdFading_  = false;
  bool       pdPending_ = false;  // a jump waits for the fade in progress to end
  uint64_t   crossfades_ = 0;
  float      pdGlideCoef_ = 1.f, pdGlideKeep_ = 0.f;  // fixed at Init
  float      pdSlewCoef_ = 1.f, pdSlewKeep_ = 0.f;    // fixed at Init: τ = 1 s (clock.md §7.2)
  float      pdLpL_ = 0.f, pdLpR_ = 0.f;  // loop damping LP state
  float      pdDcL_ = 0.f, pdDcR_ = 0.f;  // loop DC-blocker state
  float      pdLpCoef_ = 1.f, pdDcCoef_ = 0.f;
  Smoother   delayMixSm_{};

  // Reverb: Clouds-style Dattorro/Griesinger — 4 input diffusers, figure-8 tank
  // (both halves damped), slow LFO on the long delays, multi-tap wet outputs
  // (raw line-end outputs were two discrete slaps with 107 ms of silence first —
  // review finding).
  float      rvBandwidth_ = 0.f;
  Allpass    rvAp_[4]{};
  Allpass    rvDap_[4]{};
  DelaySlice rvDel_[2]{};
  float      rvLp_[2]     = {0.f, 0.f};
  float      rvLfoPhase_  = 0.f;
  float      rvDamp_ = 0.f, rvBw_ = 0.f, rvExc_ = 0.f, rvLfoInc_ = 0.f;  // fixed at Init
  float      rvDl0_ = 0.f, rvDl1_ = 0.f;                                  // fixed at Init
  uint32_t   rvTapL_[3] = {0, 0, 0}, rvTapR_[3] = {0, 0, 0};              // fixed at Init
  Smoother   reverbMixSm_{};

  // Filter: stereo double-sampled SVF, equal-power morph, smoothed insert mix.
  float    svfFreq_ = 0.f, svfDamp_ = 0.f;
  float    svfWa_ = 1.f, svfWb_ = 0.f;  // equal-power morph weights (control rate)
  uint32_t svfSeg_ = 0;
  float    svfCutoffCached_ = -1.f, svfResCached_ = -1.f, svfMorphCached_ = -1.f;
  float    svfL_[2] = {0.f, 0.f};
  float    svfR_[2] = {0.f, 0.f};
  Smoother filterMixSm_{};
  bool     svfCleared_ = true;

  Stage order_[kPostStages] = {Stage::Mod, Stage::Delay, Stage::Reverb, Stage::Filter};
};

}  // namespace brainscape::detail
