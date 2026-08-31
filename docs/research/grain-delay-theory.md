# Granular Synthesis and Grain Delay: Theory, Algorithms, and Open Implementations

> Research reference for **Brainscape** — an open-source granular delay pedal (Daisy Seed firmware + VST) built on a shared platform-agnostic C++ DSP core.
> Status: **IN PROGRESS** — written incrementally; sections still filling in.

## Summary

- **Grain delay ≠ granular synthesis.** A grain delay writes live input into a circular buffer and schedules grains from the *recent past* relative to a moving write head; a classic granulator reads a *static* buffer at an absolute position. The write head moving during playback is the entire source of a grain delay's difficulty and its character.
- Granular synthesis descends from **Dennis Gabor's 1947 "acoustical quanta"**, was made compositional by **Iannis Xenakis** (~1960, *Analogique A-B*, 1959), first computer-implemented by **Curtis Roads (1974)**, and made real-time by **Barry Truax (1986)** on the DMX-1000 ([Wikipedia](https://en.wikipedia.org/wiki/Granular_synthesis)). Roads' **Microsound** (MIT Press, 2001) is the canonical reference and explicitly the design brief behind Csound's `partikkel`.
- **Mutable Instruments Clouds is the single best open reference** for an embedded grain delay: MIT-licensed, STM32F4, `kMaxNumGrains = 64`, 32 kHz internal rate, 16-bit or 8-bit µ-law buffer, and a *quality-tiering* scheme that renders only the top 25% of grains at full quality.
- **Mutable Instruments Beads is NOT open source.** It is absent from `pichenettes/eurorack`; Émilie Gillet said she'd release it only under conditions that have not occurred. Do not plan around porting Beads.
- **DaisySP's `GranularPlayer` is not usable** for this project: it is static-buffer only, exactly two overlapping grains, and a fixed 256-point cosine envelope. Brainscape needs its own grain engine. DaisySP's `DelayLine` (linear + Hermite cubic reads) is a reasonable primitive, but its `max_size` template parameter forces static allocation, which conflicts with SDRAM placement patterns.
- **Grain quality tiering is the key CPU lever** — Clouds' trick of giving only some grains cubic interpolation / stereo and downgrading the rest is what makes 40-60 concurrent grains fit on a much weaker chip than the Daisy's.
- **Gain normalization must be `1/sqrt(N)`**, not `1/N` — Clouds uses `fast_rsqrt_carmack(num_grains_ - 1.0f)` smoothed by a one-pole. Overlapping grains from a common source are partially correlated, so power-summing is closer to right than amplitude-summing.
- **Windows: Hann is the safe default, Tukey is the useful morph.** Hann is COLA-compliant at 50% overlap; a Tukey window with α→0 becomes rectangular (clicks, but maximum "solidity"), α→1 becomes Hann. Clouds implements exactly this morph cheaply — a triangle ramp blended toward a 4096-entry LUT window.
- **The Daisy Seed's 64 MB SDRAM is the reason a long grain delay is possible at all.** At 48 kHz stereo float, 64 MB is ~116 seconds; at 16-bit stereo it's ~349 seconds. Internal SRAM (512 KB AXI) holds only ~1.4 s of 16-bit stereo.
- **SDRAM on the Daisy is MPU-configured as cacheable/bufferable** (libDaisy `system.cpp`, region 1 at `0xC0000000`, 64 MB, `MPU_ACCESS_CACHEABLE`). Cortex-M7 D-cache lines are 32 bytes, so *sequential* reads inside a grain are cheap after the first miss; the cost is one miss per grain per cache line, and many simultaneous grains at scattered positions thrash a small D-cache.
- SDRAM is 100 MHz × 32-bit ≈ **400 MB/s theoretical**, but random-access latency (row activate + CAS) dominates for scattered grain reads — budget by *cache misses*, not by bandwidth.
- Prefer **16-bit fixed-point storage in SDRAM** over float: it halves memory traffic and doubles the samples per cache line, at a noise floor that is inaudible under a granular texture.
- **Feedback with granular in the loop is unconditionally unstable unless tamed** — Clouds puts an SVF (feedback filter) plus highpass/lowpass in the chain and applies soft limiting. Any grain-delay feedback path needs DC blocking, band-limiting, and a saturating nonlinearity.
- Best studyable open implementations, verified licenses: **Clouds (MIT)**, **Argotlunar (GPL v2)**, **EmissionControl2 (source on GitHub, uses AlloLib)**, **SuperCollider `GrainIn`/`GrainBuf` (GPL)**, **Csound `partikkel` (LGPL)**. GPL sources are *study-only* if Brainscape ships permissive — read the architecture, do not copy code.

---

## 1. Brief history

| Year | Who | What |
|---|---|---|
| 1946/47 | **Dennis Gabor** | "Theory of communication" / "Acoustical quanta and the theory of hearing" — sound represented as a series of elementary *grains*, each carrying both temporal and frequency information. Gabor built a mechanical machine to granulate sound and alter duration without shifting pitch. |
| 1959–60 | **Iannis Xenakis** | Applied Gabor's quanta compositionally; *Analogique A-B* (1959) realized with analog tone generators and tape splicing. Held that "all sound, even continuous musical variation, is conceived as an assemblage of a large number of elementary sounds." Formalized in *Formalized Music*. |
| 1974 | **Curtis Roads** | First computer implementation of granular synthesis. |
| 1986 | **Barry Truax** | First *real-time* granular synthesis, on the DMX-1000 signal processing computer — the ancestor of every live granulator including this one. |
| 2001 | **Curtis Roads** | *Microsound* (MIT Press) — the definitive taxonomy of time scales and grain techniques. Microsound sits below ~100 ms and above ~10 ms, straddling the audio (20 Hz–20 kHz) and infrasonic (<20 Hz) ranges. |

Grain durations are conventionally **~1 to 100 ms**. Below ~1–2 ms the "grain" degenerates into a click/impulse and the window's own spectrum dominates; above ~100 ms the ear stops fusing grains into a texture and starts hearing discrete echoes — which is exactly the boundary a *grain delay* wants to sit on and cross.

**Roads' emission taxonomy** (adopted by nearly every implementation):
- **Synchronous** — grains emitted at strictly periodic intervals. Produces a pitched artifact at the emission rate (the "grain tone"), which becomes a musical feature above ~20 Hz emission rate.
- **Quasi-synchronous** — periodic with randomized jitter on the interval. Smears the grain tone into a band.
- **Asynchronous / cloud** — grain onsets drawn from a stochastic process (Poisson-like). No grain tone; produces clouds and textures.

A good grain delay should expose the **synchronous ↔ asynchronous** axis as a single "randomness/jitter" control, because it is the difference between a rhythmic stutter delay and a diffuse cloud.

---

## 2. Grain delay vs. granular synthesis of a static buffer

This is the central architectural distinction for Brainscape.

**Static-buffer granulation** (SuperCollider `GrainBuf`, EmissionControl2, most samplers):
- The buffer is fixed. `position` is an absolute normalized location in a file.
- Grains can read anywhere, in any direction, at any rate, forever.
- No producer/consumer race — the buffer never changes underneath a playing grain.

**Grain delay / delay-line granulation** (Clouds in granular mode, Argotlunar, SuperCollider `GrainIn`, this project):
- A **circular buffer** is continuously written by the live input at exactly 1 sample per sample.
- Grains are scheduled at a **delay offset behind the write head**, not at an absolute position.
- The write head is a moving cliff: any grain whose read pointer crosses it hears the oldest audio in the buffer spliced against the newest — a hard discontinuity.

### 2.1 The moving-write-head problem

Three distinct hazards, all of which must be handled explicitly:

1. **Read overtaking write (grain too "recent").** A grain at delay offset `d` playing at rate `r > 1` closes on the write head at `(r - 1)` samples per sample. If `d` is small and grain duration `L` is long, the grain runs off the end of history. Guard: require `d >= L * max(0, r - 1) + margin`, or clamp the scheduled offset at schedule time.
2. **Write overtaking read (grain too "old").** A grain playing at `r < 1` (or reversed) recedes from the write head at `(1 - r)` samples per sample, and the write head laps the buffer every `bufferLength` samples. If `d + L*(1-r) > bufferLength`, the grain gets overwritten mid-flight. Guard: clamp `d` to `bufferLength - L*(1-r) - margin`.
3. **Feedback re-injection.** If the granular output is written back into the same buffer, a grain can read material it produced microseconds ago — a tight, unstable loop.

**Clouds' answer** is the cleanest: `ScheduleGrain()` computes the grain's start position **accounting for both the playback and recording head movement**, i.e. it converts the user's "position" into a buffer index using the *current* write pointer, and the grain then owns a fixed integer start sample plus a phase increment. The grain never re-queries the write head after it starts. This is the right model: **resolve position once at schedule time, then play open-loop.** It makes the grain voice trivially cheap and immune to write-head drift within its own lifetime.

### 2.2 Latency and "position" semantics

In a grain delay, `position`/`delay` is a *time-behind-now* control, and it should be exposed that way. Two useful mappings:

- **Free (musical time):** delay in ms or in tempo divisions, 0 → bufferLength.
- **Spray:** each grain's delay = `basePosition + rand(-spray, +spray)`. Spray is what turns a stutter into a cloud, and it is the single most important "granular-ness" control after density.

Note the minimum achievable latency: a grain of length `L` scheduled at `position = 0` must still read `L` samples of already-recorded audio, so the effective minimum latency of a grain delay is roughly one grain length (plus block size). There is no such thing as a zero-latency granulator with non-zero grain size.

---

## 3. Core algorithm

### 3.1 Circular buffer management

Design decisions, with recommendations:

- **Power-of-two length + mask.** `idx & (N-1)` beats `%` by a wide margin on Cortex-M7 and avoids the compiler emitting a division. Choose `N` as a power of two even if it wastes SDRAM — you have 64 MB.
- **Write direction.** DaisySP's `DelayLine` decrements the write pointer (`write_ptr_ = (write_ptr_ - 1 + max_size) % max_size`) so that `Read(delay)` indexes forward. Either convention works; pick one and document it. An *incrementing* write pointer with `read = (write - delay) & mask` is more natural for grain delay because "delay" and "age" are the same number.
- **Sample format.** Store `int16_t` (or Clouds-style 8-bit µ-law for a lo-fi mode) rather than `float`. Halves SDRAM traffic and doubles samples-per-cache-line. Clouds does exactly this: 16-bit PCM for hi-fi, 8-bit µ-law for lo-fi, with a `kDownsamplingFactor = 2` that also halves the *rate* in lo-fi mode — a second, orthogonal way to buy buffer time.
- **Guard region / wrap handling.** The cheapest interpolation-safe approach is to mask each of the interpolation taps individually (`(i+0)&mask`, `(i+1)&mask`, …). A duplicated guard region at the end of the buffer avoids per-tap masking but costs a second write per sample. On the Daisy, per-tap masking is cheaper than doubling write bandwidth.
- **Stereo layout: interleave.** Store L/R adjacent (`buf[2*i]`, `buf[2*i+1]`) so one cache line fetch serves both channels. This roughly halves cache misses for stereo grains versus two separate mono buffers. Clouds' `Grain::OverlapAdd` accumulates stereo contributions from interleaved reads for exactly this reason.

### 3.2 Grain scheduler

The scheduler decides *when* a new grain is born. Three mechanisms, ideally OR'd together (this is Clouds' design and it is worth copying wholesale):

```
seed = seed_probabilistic || seed_deterministic || seed_trigger
```

- **Deterministic (synchronous):** a phasor accumulates each sample; when `grain_rate_phasor_ >= space_between_grains`, emit and reset to 0. In Clouds: `space_between_grains = grain_size_hint_ / target_num_grains`.
- **Probabilistic (asynchronous):** each sample, `Random::GetFloat() < p && target_num_grains > num_grains_`. This is a Bernoulli approximation of a Poisson process; it self-limits because it stops firing once the live grain count reaches target.
- **External trigger:** for a footswitch/tap/MIDI-triggered grain burst.

**Density mapping.** Clouds cubes the overlap control: `overlap = overlap * overlap * overlap`, then `target_num_grains = max_num_grains_ * overlap`. Cubing is important — the perceptual difference between 1 and 4 grains is enormous while 40 vs 50 is nearly inaudible, so a linear density knob wastes most of its travel. Use a cubic or exponential law.

**Density vs. size coupling.** There are two ways to define density and they behave very differently:
- *Overlap-referenced* (Clouds): density = number of simultaneous grains. Grain rate is derived: `rate = density / grainSize`. Changing size does not change how "thick" it sounds — good default behavior.
- *Rate-referenced* (many plugins): density = grains per second, independent of size. Changing size then changes the overlap factor and thus the loudness and texture drastically.

**Recommendation: implement overlap-referenced density, and expose grain rate as a derived readout.** It decouples the two knobs perceptually, which is what makes Clouds feel playable.

### 3.3 Grain voices and polyphony

A grain voice should be a **POD struct with no dynamic allocation and no virtual calls**, living in a fixed-size array with an active/free flag. Recommended state, following Clouds' `Grain::Start()` signature (`pre_delay, buffer_size, start, width, phase_increment, window_shape, gain_l, gain_r, quality`):

```
struct Grain {
  bool     active;
  int32_t  pre_delay;      // samples to wait before this grain starts, within the block
  uint32_t phase;          // 16.16 fixed point read position within the grain
  int32_t  phase_increment; // 16.16; encodes pitch AND direction (negative = reverse)
  int32_t  first_sample;   // absolute (masked) buffer index where the grain begins
  uint32_t envelope_phase;
  float    envelope_phase_increment; // = 2.0f / width
  float    window_shape;
  float    gain_l, gain_r;
  uint8_t  quality;        // interpolation/stereo tier
};
```

Key architectural points:

- **`pre_delay` is essential and often omitted.** Grains are scheduled at sample resolution but rendered per block. Without a `pre_delay`, every grain in a block starts on the block boundary, which quantizes grain onsets to the block size (e.g. 48 samples = 1 ms at 48 kHz) and produces an audible periodic artifact at the block rate. Clouds handles this by consuming pre-delay at the top of the render loop:
  `while (pre_delay_ && size) { destination += 2; --size; --pre_delay_; }`
  **Copy this.** It is the difference between a granulator that sounds smooth and one with a buzz at 1 kHz.
- **16.16 fixed-point phase.** `sample_index = first_sample + (phase >> 16)`, fraction = `phase & 65535`. On Cortex-M7 with an FPU, float phase is also fine, but fixed-point avoids float→int conversion per sample and keeps precision constant across a long buffer (a float32 phase loses sub-sample precision past ~2^24 samples ≈ 5.8 minutes — relevant with 64 MB!). **Use fixed-point or a split integer+fractional representation, not a bare float, for absolute buffer positions.**
- **Voice stealing.** Clouds simply doesn't fire a new grain if `num_grains_ >= target_num_grains`. Csound `partikkel` instead deletes the *oldest* grain when `imax_grains` is exceeded. Not firing is cheaper and click-free; stealing gives tighter rhythmic response to triggers. **Recommendation: don't-fire for the free-running scheduler, oldest-steal for explicit triggers.**
- **Quality tiering.** Clouds' `num_midfi_grains_ = 3 * max_num_grains / 4` means the newest/lowest-priority grains render at reduced quality. This is *the* CPU lever for an embedded granulator: full-quality (cubic interp, per-grain stereo) for the first N grains, linear-interp / mono-summed for the rest. The degradation is inaudible because low-priority grains are, by construction, buried in a dense texture.

### 3.4 Window / envelope functions

The window is applied per grain per sample. It must go to exactly zero at both ends or you get a click at every grain boundary — and at 40 grains × 30/s that's 1200 clicks/second, i.e. noise.

| Window | Shape | Spectral cost | Character | COLA |
|---|---|---|---|---|
| **Rectangular / boxcar** | flat | worst — sinc sidelobes, −13 dB first lobe | maximum transient/"solidity", audible clicks | at any integer overlap, but clicks |
| **Trapezoid** | linear attack, flat sustain, linear decay | moderate | preserves body of the grain, mild clicks; the "workhorse" | yes if attack+decay ≤ hop |
| **Triangle (Bartlett)** | ramp up/down | moderate | soft, no clicks, thin | yes at 50% |
| **Hann (raised cosine)** | 0.5(1 − cos(2πn/N)) | good — −31 dB first lobe, 18 dB/oct rolloff | smooth, "correct" default | **yes, exactly, at 50% overlap** |
| **Tukey (α)** | Hann tapers + flat top | tunable | **the morph knob**: α=0 → rectangular, α=1 → Hann | yes for many α/hop combos |
| **Gaussian** | exp(−x²) | best sidelobes, never exactly zero | very smooth, needs truncation | approximately |

Verified facts: the Hann window satisfies COLA with a hop of 0.5 (adding delayed windows with 50% overlap yields a constant); the Tukey window has a flat top, reaches zero at the endpoints, with a sinusoidal transition parameterized by α, equal to rectangular at α=0 and Hann at α=1 ([CCRMA COLA examples](https://ccrma.stanford.edu/~jos/sasp/COLA_Examples.html), [SciPy `check_COLA`](https://docs.scipy.org/doc/scipy/reference/generated/scipy.signal.check_COLA.html), [DSP.jl windows](https://docs.juliadsp.org/stable/windows/)).

**Three implementation strategies:**

1. **LUT + linear interpolation (Clouds).** Envelope phase runs 0→2 with `envelope_phase_increment_ = 2.0f / width`. A triangle is produced by folding: `gain = phase >= 1.0f ? 2.0f - gain : gain`. That triangle value then *indexes* a 4096-entry window LUT, and the two are crossfaded:
   ```
   window = Interpolate(lut_window, gain, 4096.0f);
   gain += smoothness * (window - gain);
   ```
   This is elegant: `smoothness = 0` gives a pure triangle, `smoothness = 1` gives the LUT shape (Hann-like), and intermediate values morph continuously. One LUT, one lerp, one mul — very cheap. Combined with Clouds' `window_gain = 1.0f + 2.0f * window_shape` it also compensates the loudness change as the window narrows.
2. **Closed-form Tukey.** Compute attack/decay ramps in a fixed fraction of the grain. Csound `partikkel` parameterizes this as `ksustain_amount` and `ka_d_ratio`: with both at 0.5, attack takes 25% of grain duration, sustain 50%, decay 25%. This is the most *musically legible* parameterization — one knob for "how much flat top", one for "attack vs decay skew" (which gives you reverse-sounding grains without reversing playback).
3. **Envelope as a buffer (SuperCollider).** `GrainBuf`/`GrainIn` take `envbufnum`; `-1` selects a built-in Hann. Maximum flexibility, more memory traffic.

**Recommendation for Brainscape: strategy 1 for the DSP core, exposed as a Tukey-like `shape` parameter, plus a separate attack/decay `skew`.** The skew control is under-served in existing pedals and is cheap: it only changes where the fold point sits in the 0→2 phase.

**EmissionControl2** offers a different morph worth noting: a single normalized parameter where "0 to 0.5 interpolates between expo and tukey and 0.5 to 1 interpolated between tukey and reverse expo" — i.e. one knob sweeping percussive-attack → symmetric → reverse-swell. Musically excellent, and it costs nothing extra.

### 3.5 Parameters

The canonical grain-delay parameter set, with recommended ranges and laws:

| Parameter | Range | Law | Notes |
|---|---|---|---|
| **Size** | 1 ms – 500 ms | exponential | Below ~5 ms the window dominates and it becomes a formant/tone generator. Above ~200 ms grains stop fusing. |
| **Density / overlap** | 1 – 32 grains | cubic | Overlap-referenced (see 3.2). |
| **Position / delay** | 0 – bufferLength | linear or tempo-divided | Time behind the write head. |
| **Spray** | 0 – bufferLength | exponential | Random offset added per grain. The "granular" knob. |
| **Pitch** | −24 … +24 st | quantizable | Per grain, resolved at schedule time. |
| **Pitch spread** | 0 – 12 st | linear | Random per-grain detune; small amounts (±10 cents) thicken enormously. |
| **Reverse probability** | 0 – 100% | linear | Per-grain coin flip, not a global switch. |
| **Pan / stereo spread** | 0 – 1 | linear | `pan = 0.5 + spread * (rand() - 0.5)` (Clouds). |
| **Jitter / randomness** | 0 – 1 | linear | Crossfades scheduler from deterministic to probabilistic. |
| **Feedback** | 0 – ~1.1 | linear | Must be band-limited and saturated. |
| **Mix** | 0 – 100% | equal-power or linear | See 3.10. |

**Correlation/linking is a differentiator.** Argotlunar explicitly supports parameter correlation — e.g. linking filter cutoff to pitch, or grain duration to stereo pan. This is cheap to implement (a small modulation matrix resolved at schedule time) and gives an enormous increase in perceived musicality per unit of DSP.

### 3.6 Overlap-add and gain normalization

Every active grain writes into a shared output accumulator:

```
for each grain g:
   for n in 0..blockSize:
      s = interpolate(buffer, g.readPos)
      e = window(g.envPhase)
      outL[n] += s * e * g.gainL
      outR[n] += s * e * g.gainR
      advance g
```

**The loudness problem.** N overlapping grains from the same source are neither fully correlated (which would sum as N) nor fully independent (which would sum as √N). Empirically √N is the better model for a granulator because spray/pitch/reverse decorrelate the grains.

Clouds' normalization, verified from source:
```
float gain_normalization = num_grains_ > 2.0f ? fast_rsqrt_carmack(num_grains_ - 1.0f) : 1.0f;
```
smoothed with a one-pole at coefficient 0.01. Two details matter:
- The `- 1.0f` and the `> 2.0f` guard mean **one grain is unity gain** — no gain change at low density, which is what you want when the user is using it as a simple delay.
- **The one-pole smoothing is mandatory.** `num_grains_` is an integer that jumps; applying `1/√N` unsmoothed causes an audible step every time a grain is born or dies. A 0.01 coefficient at block rate is a time constant of ~100 blocks.

Additionally Clouds applies `window_gain = 1.0f + 2.0f * parameters.granular.window_shape` (with `CONSTRAIN`) — compensating for the fact that a Hann-ish window has ~0.5 average value while a rectangular one has 1.0, so morphing the window otherwise changes level by 6 dB.

### 3.7 Pitch shifting, reverse, and time-stretching

All three fall out of one mechanism: **the phase increment**.

- **Pitch shift** = `phase_increment = 65536 * SemitonesToRatio(semitones)` in 16.16. Clouds: `float pitch_ratio = SemitonesToRatio(pitch);`. Because each grain is short and windowed, resampling produces pitch shift *without* the buffer running out — the grain simply covers a different amount of source material. This is classic granular pitch shifting: it's a time-domain resample plus re-triggering, so it has no FFT latency but does have a characteristic "grain warble" whose rate is the emission rate.
- **Reverse** = `phase_increment < 0`, starting at the *end* of the grain's source region. This is cleaner than reversing the buffer. Note the guard math from §2.1 flips sign for reverse grains: a reversed grain moves *backwards* through history, i.e. *away* from the write head, so it's actually the safer direction.
- **Time-stretch** = keep `phase_increment = 1.0` but advance the *scheduling* position slower than real time. Clouds separates this into a distinct mode (`PLAYBACK_MODE_STRETCH`, backed by a `WSOLASamplePlayer`) because good time-stretching wants pitch-synchronous overlap-add (WSOLA finds the best-correlated splice point), which is a fundamentally different algorithm from free granulation. **Recommendation: don't try to make one engine do both well.** Offer granular stretch (cheap, characterful) and, if you want clean stretch later, add a separate WSOLA mode.

**Important interaction:** pitch and grain-size are not independent in a delay. A pitched-up grain consumes source faster, so it needs `size * ratio` samples of history. Either (a) fix the *output* duration and vary how much source is consumed (standard; pitch changes the "zoom" on the source), or (b) fix source length and vary output duration (grain rate changes with pitch — usually undesirable). Use (a).

### 3.8 Interpolation quality

Reading at a fractional position requires interpolation. Options in ascending cost:

| Method | Taps | Cost | Quality |
|---|---|---|---|
| **Truncation/nearest** | 1 | ~0 | Unusable — aliasing/zipper noise |
| **Linear** | 2 | 1 mul, 2 add | −6 dB/oct image rejection; audible dulling on pitch-down, aliasing on pitch-up. Acceptable *inside a granular texture*. |
| **Cubic Hermite (Catmull-Rom)** | 4 | ~5 mul | Big improvement; DaisySP's `DelayLine::ReadHermite` implements `(((a * f) - b_neg) * f + c) * f + x0`. **The right default for full-quality grains.** |
| **Windowed sinc / polyphase FIR** | 8–32 | expensive | Overkill for a granulator; the window transients dominate the error budget anyway. |

**Key insight for the CPU budget:** interpolation error inside a grain is masked by (a) the grain envelope's own spectral splatter and (b) the density of the texture. So **tier it**: cubic for the first ~8 grains, linear for the rest. This is precisely what Clouds' `GrainQuality` enum buys.

One caveat: **linear interpolation is a lowpass whose cutoff depends on the fractional phase**, so a grain playing at a rate very near 1.0 will slowly wobble in brightness as `frac` drifts. If a grain is at exactly rate 1.0 (very common — it's the "clean delay" case), **special-case it to an integer copy loop.** That path is both faster and bit-exact, and it makes a grain delay at unity pitch sound as clean as an ordinary delay.

### 3.9 Stereo strategies

Options, roughly in increasing sophistication:

1. **Mono buffer, stereo pan per grain.** Cheapest. `pan = 0.5f + parameters.stereo_spread * (Random::GetFloat() - 0.5f)` (Clouds), then equal-power gains `gain_l = cos(pan*π/2)`, `gain_r = sin(pan*π/2)`. Produces a wide, diffuse image from a mono source. Ideal for a guitar pedal fed by a mono pickup.
2. **True stereo buffer, correlated grains.** Grain reads both channels at the same position; pan becomes a rotation/width control. Preserves an incoming stereo image (important if Brainscape sits after another stereo pedal).
3. **True stereo buffer, decorrelated grains.** Left and right grains scheduled independently. Maximum width, but destroys mono compatibility and phantom-centre imaging — dangerous for a pedal that may be summed to mono at a PA.
4. **Haas/offset trick.** Same grain, small inter-channel delay. Cheap width, but mono-sum comb filtering.

**Recommendation:** true stereo interleaved buffer (option 2) as the substrate, with a **stereo spread** control that moves from correlated (option 2) toward per-grain random pan (option 1). Check mono compatibility at every spread setting. Clouds' `quality` bitmask encodes exactly this choice — **bit 0: channel count (mono/stereo), bit 1: fidelity level** — trading stereo for buffer time, which is a good user-facing tradeoff to offer.

### 3.10 Feedback routing

Feedback with a granulator in the loop is the hardest stability problem in the design, because the granulator is **not** a passive element: pitch shifting moves energy into new bands, and overlap-add can have gain > 1 transiently even when the normalization is nominally correct.

**Topology choice.** Two options:

- **(A) Feedback of wet into the record buffer.** `write(input + feedback * granularOut)`. Grains eat their own output. Produces the classic Microcosm/Clouds "growing cloud" — pitch shifting compounds, so with `pitch = +5 st` and feedback you get an ascending shepherd-like cascade. Very musical, very unstable.
- **(B) Feedback around the whole block into the input.** Same math, different insertion point; behaves nearly identically for a delay.
- **(C) Separate feedback delay line, granulator in parallel.** Stable and boring. Avoid as the only option.

**Recommendation: (A), with a mandatory taming chain.** Clouds' post-processing chain is the model — the `granular_processor` instantiates a **Diffuser (4 allpass stages), Reverb, PitchShifter, and three SVF filters (feedback, highpass, lowpass)**, with **feedback amount as one of four parameters under the BLEND knob** (dry/wet balance, random panning amount, feedback amount, reverb amount).

Concretely, the feedback path should contain, in order:
1. **DC blocker** — pitch-shifted and windowed grains accumulate DC; without this, feedback integrates it into a rail.
2. **Highpass ~80–120 Hz** — prevents low-frequency buildup, which is what actually blows up a granular feedback loop (energy piles up where the ear is least sensitive but the DAC is not).
3. **Lowpass ~4–8 kHz, feedback-dependent** — models tape/BBD and stops pitch-up feedback from screaming.
4. **Soft saturation (tanh or a cheap polynomial)** — bounds the loop gain without hard clipping. This is what lets you offer feedback > 1.0 (self-oscillation) as a *feature* rather than a fault.
5. **Optional: allpass diffusion** — Clouds' 4-stage diffuser smears transients so repeats blur rather than stack. Enormously improves the sound of high feedback.

**Stability rule of thumb:** the loop gain at *any* frequency must be < 1 for non-oscillating behavior. With granular pitch shift in the loop, energy migrates, so per-band analysis doesn't apply cleanly — the saturator is doing the real work. Test with a full-scale impulse, feedback at max, pitch at ±12 st, and confirm the output settles to a bounded limit cycle rather than a rail.

### 3.11 Wet/dry and level compensation

- **Dry path should be delay-matched or not at all.** For a guitar pedal, keep the dry path *analog or single-sample* — do not run it through the same block delay as the wet, or you get comb filtering when blended. Many pedals get this wrong.
- **Equal-power crossfade** (`cos/sin`) sounds more consistent than linear for uncorrelated wet, but for a delay where wet is a *delayed copy* of dry, **linear** is often better because it avoids a +3 dB bump at centre. Offer linear by default.
- **Kill-dry** must be available for parallel/loop use.
- **Output makeup:** the `1/√N` normalization plus `window_gain` compensation should already leave the wet path level-consistent; verify with a sine sweep at density 1 and density max and confirm < 1 dB variation.

---

## 4. Open implementations to study

### 4.1 Mutable Instruments Clouds — **MIT license**, the primary reference

Repo: `github.com/pichenettes/eurorack`, directory `clouds/`. **README license statement: "Code (AVR projects): GPL3.0. Code (STM32F projects): MIT license. Hardware: cc-by-sa-3.0. By: Emilie Gillet."** Clouds is an STM32F project → **MIT**. `granular_processor.h` carries an MIT header, © 2014 Emilie Gillet.

**Architecture (verified from source):**

- `PlaybackMode` enum: `PLAYBACK_MODE_GRANULAR`, `PLAYBACK_MODE_STRETCH`, `PLAYBACK_MODE_LOOPING_DELAY`, `PLAYBACK_MODE_SPECTRAL` — four engines behind one interface:
  - `GranularSamplePlayer` (the grain delay)
  - `WSOLASamplePlayer` (time stretch)
  - `LoopingSamplePlayer`
  - `PhaseVocoder` (spectral)
- Internal sample rate **32000 Hz**; `kDownsamplingFactor = 2` gives a 16 kHz lo-fi mode.
- Buffer resolution 16-bit PCM (hi-fi) or 8-bit µ-law (lo-fi).
- **Two buffers**: a large one in SDRAM and a small one in CCM/fast RAM. (Clouds has only 64 KB of SRAM plus a small external chip — the Daisy's 64 MB removes this entire constraint.)
- `int16_t tail_buffer_[2][256]` for overlap handling across block boundaries.
- **Quality setting is a bitmask: bit 0 = channel count (mono/stereo), bit 1 = fidelity.** Mono + lo-fi = 4× the buffer time of stereo + hi-fi.
- Effects chain: Diffuser, Reverb, PitchShifter, three SVFs (feedback / highpass / lowpass).
- **BLEND knob multiplexes four post-processing parameters**: dry/wet balance, random panning amount, feedback amount, reverb amount ([Clouds manual](https://pichenettes.github.io/mutable-instruments-documentation/modules/clouds/manual/)).
- Manual states **40–60 concurrent grains** in practice, against `kMaxNumGrains = 64`.
- Grain envelopes "continuously variable between boxcar, triangle, and Hann."

**Grain engine specifics** (from `granular_sample_player.h` / `grain.h`):
- `Init()` builds grain objects, sets `max_num_grains_` and `num_midfi_grains_ = 3 * max_num_grains / 4`.
- `overlap = overlap^3`; `target_num_grains = max_num_grains_ * overlap`; `space_between_grains = grain_size_hint_ / target_num_grains`.
- Triple triggering (probabilistic / deterministic phasor / external), OR'd.
- `ScheduleGrain()` resolves position, pitch ratio, and pan gains once, accounting for both playback and record head movement.
- `grain_size = Interpolate(lut_grain_size, parameters.size, 256.0f)` — grain size comes from a 256-entry LUT, i.e. an arbitrary (exponential) mapping rather than a formula. Cheap and tunable by ear.
- `pan = 0.5f + parameters.stereo_spread * (Random::GetFloat() - 0.5f)`.
- `Grain::Start(pre_delay, buffer_size, start, width, phase_increment, window_shape, gain_l, gain_r, recommended_quality)`.
- Envelope: `envelope_phase_increment_ = 2.0f / width`, triangle by folding at 1.0, morphed via `gain += smoothness * (window - gain)` where `window = Interpolate(lut_window, gain, 4096.0f)`.
- Playback phase in 16.16: `sample_index = first_sample + (phase >> 16)`, `frac = phase & 65535`.
- `pre_delay` consumed at the top of the render loop.
- `OverlapAdd()` is a *template* method — templated on quality/channel-count so the compiler generates specialized inner loops with no runtime branching. **This is a technique worth copying**: template the grain render on `<Quality, Stereo, Interpolation>` and dispatch once per grain, not per sample.

**Ports worth examining:** `hfl1967/cumuloid` (Clouds → Daisy Petal) and `erwincoumans/DaisyCloudSeed` (Clouds → Daisy Seed, larger buffers). These are direct evidence that a Clouds-class engine fits comfortably on the Daisy.

### 4.2 Mutable Instruments Beads — **NOT open source**

Verified: there is **no `beads` directory** in `pichenettes/eurorack` (directories present: blades, blinds, braids, branches, clouds, ears, edges, elements, frames, grids, kinks, links, marbles, peaks, plaits, rings, ripples, shades, shelves, stages, streams, tides, tides2, veils, volts, warps, yarns). Community discussion ([MW thread "Will Beads source code ever be released?"](https://www.modwiggler.com/forum/viewtopic.php?t=281283), [MI forum "Beads github code"](https://forum.mutable-instruments.net/t/beads-github-code/20206)) indicates Émilie Gillet said she would release it when she stopped providing support, but it has not been released. **Treat Beads as inspiration only, from the manual and from listening.** Its notable ideas that *are* documented — the "SEED"/trigger-per-grain philosophy, the built-in stereo, the different TIME/SHAPE parameterization — can be reimplemented from first principles.

### 4.3 DaisySP granular modules — **MIT, but not sufficient**

`electro-smith/DaisySP` (MIT; usable in closed-source and commercial projects). Relevant headers, verified from `Source/daisysp.h`:

- **`Sampling/granularplayer.h`** — Verified limitations: **static buffer only** (`Init()` takes "pointer to the sample to be played" and element count — no live input path); **exactly two grain streams** (`idx_`/`idx2_`, two phasors, `sig_`/`sig2_`); **fixed 256-entry cosine envelope** `float cosEnv_[256]`; parameters are speed, transposition (cents), grain size (ms, min 1). This is a granular *sample player*, essentially a crossfade-looping pitch shifter. **Not a granulator and not usable for Brainscape.**
- **`Noise/grainlet.h`** — a *Grainlet oscillator* (a Braids/Plaits-derived synthesis model), unrelated to granular delay despite the name.
- **`Effects/pitchshifter.h`** — a time-domain (crossfading delay-line) pitch shifter, "based on 'Pitch Shifting' from ucsd.edu", author shensley. Useful as a reference for the two-tap crossfade approach, but Brainscape's grain engine subsumes it.
- **`Utility/delayline.h`** — `template <typename T, size_t max_size>`. Write decrements: `line_[write_ptr_] = sample; write_ptr_ = (write_ptr_ - 1 + max_size) % max_size;`. `Read()` does linear interp: `T a = line_[(write_ptr_ + delay_) % max_size]; T b = line_[(write_ptr_ + delay_ + 1) % max_size]; return a + (b - a) * frac_;`. `ReadHermite()` uses 4 taps with `(((a * f) - b_neg) * f + c) * f + x0`. Storage is `T line_[max_size]` as a member — **static, sized at compile time**, so placing it in SDRAM means declaring the whole object `DSY_SDRAM_BSS`.
  - **Note the `%` operator**: `max_size` is a template constant, so if it's a power of two the compiler will turn `%` into a mask. If it isn't, you get a real division in the audio inner loop. Always use power-of-two sizes with `DelayLine`.
- **`Utility/looper.h`** — buffer-based looping utility, closer to what a grain delay needs at the storage layer.

**Conclusion: Brainscape must write its own grain engine.** DaisySP is useful for filters (`svf.h`, `onepole.h`), `dcblock.h`, `crossfade.h`, `limiter.h`, and as a style reference — not for granulation.

### 4.4 Argotlunar — **GPL v2**, the closest prior art to Brainscape's plugin side

- Author Michael Ourednik; site `mourednik.github.io/argotlunar`, source at `github.com/mourednik/argotlunar`. **License: GPL v2.**
- Self-described as a **"real-time delay-line granulator"** — i.e. exactly a grain delay, not a sample granulator. VST/AU, Windows/macOS/Linux.
- Per-grain randomization of **amplitude, panning, duration, delay, pitch, glissando, filter, and envelope**. Output of all grains is mixed and can be **fed back into the main input**.
- Time parameters host-tempo syncable; pitch parameters quantizable to chords/scales.
- **Parameter correlation** — e.g. filter cutoff linked to pitch, or grain duration linked to stereo pan. This is the standout feature.
- Two per-grain features Clouds lacks and Brainscape should consider: **per-grain glissando** (phase increment ramps during the grain — a cheap way to get chirps/doppler) and **per-grain filter** (a one-pole or SVF per grain, randomized).

**Licensing caution:** GPL v2. Study the architecture, do not copy code into a permissively-licensed core.

### 4.5 EmissionControl2 — research-grade, source on GitHub

- `github.com/EmissionControl2/EmissionControl2`, from UCSB CREATE (Curtis Roads' institution). C++ on the **AlloLib** library, CMake ≥ 3.13.
- Scale: granulation of multiple sound files simultaneously (up to 1 GB), **up to 2048 simultaneous grains** (hardware-limited), synchronous and asynchronous emission, **intermittency control**, per-grain signal processing, and a **modulation matrix with six LFOs** over all granulation parameters.
- Architecture (verified from `ecSource/include/emissionControl.h`):
  - `class Grain : public al::SynthVoice` — grain as a *voice* in a polyphonic synth framework, with `onProcess()` at sample rate and `configureGrain()` for setup. (Note: `SynthVoice` implies virtual dispatch — fine on desktop, **wrong for Cortex-M7**; use templates/POD instead.)
  - `voiceScheduler` — audio-rate trigger generator; `trigger()` returns true when a voice should sound, driven by frequency + randomization params.
  - `grainEnvelope` — three interpolated shapes (exponential, reverse-exponential, Tukey) morphed by one normalized parameter: 0→0.5 blends expo↔Tukey, 0.5→1 blends Tukey↔reverse-expo.
  - `ecModulator` — LFOs (sine, square, ramp, noise) with bipolar/unipolar-positive/unipolar-negative polarity modes.
  - `ecParameter` — parameter class with built-in internal + external modulation, range binding, and preset handling.
  - `grainParameters` struct bundles transposition, filter, resonance, duration, envelope type, pan, volume, and modulation depths — i.e. **all per-grain state resolved into one struct at schedule time**. Same pattern as Clouds' `Grain::Start()`.
- **Two ideas to steal:** (1) the single-knob expo↔Tukey↔reverse-expo envelope morph; (2) **intermittency** — a probability that a scheduled grain is simply *skipped*, which creates rhythmic gaps and is far more musical than just lowering density.

### 4.6 SuperCollider `GrainBuf` / `GrainIn` — GPL, but the cleanest API to learn from

- `GrainIn.ar(numChannels, trigger, dur, in, pan, envbufnum, maxGrains, mul, add)` — **granulates an input signal**; this is the grain-delay-shaped UGen.
- `GrainBuf.ar(numChannels, trigger, dur, sndbuf, rate, pos, interp, pan, envbufnum, maxGrains, mul, add)` — static buffer version.
- Two design decisions worth adopting:
  1. **"All args except numChannels and trigger are polled at grain creation time."** This is the same *resolve-once-at-schedule* rule as Clouds, stated as API contract. It is the single most important architectural rule for a grain engine — it makes grains independent, cheap, and glitch-free under parameter modulation.
  2. **`maxGrains` (default 512) is fixed at UGen init and cannot be modified.** Allocation is static. Same conclusion for embedded.
- `envbufnum = -1` selects a **built-in Hann envelope**; otherwise any buffer is the window. `interp` selects interpolation order (default 2 = linear; 4 = cubic).

### 4.7 Csound `partikkel` — LGPL, the most complete parameterization

- "A granular synthesizer with 'per grain' control over many of its parameters," explicitly **"conceived after reading Curtis Roads' book Microsound, and the goal was to create an opcode capable of all time-domain varieties of granular synthesis described in this book."** It is by common acknowledgement among Csound's most complex opcodes.
- Notable specifics:
  - **`imax_grains`** = max grains per k-period; over-estimating "should not affect performance," and exceeding it deletes the **oldest** grains. (Contrast Clouds' don't-fire policy.)
  - **Envelope parameterization: `ksustain_amount` + `ka_d_ratio`.** With both at 0.5: attack = 25% of duration, sustain = 50%, decay = 25%. **This is the best envelope UI in the survey** — two intuitive knobs spanning rectangular↔trapezoid↔triangle and attack-heavy↔decay-heavy.
  - **`ipanlaws`** = an f-table describing the panning curve for fractional channel-mask values, mixing a grain between two neighbouring outputs by the fractional value. Generalizes stereo pan to arbitrary channel counts.
- Also in Csound: `granule`, `grain`, `partikkelsync`, `syncgrain`, `fog`. The [Csound granular chapter](https://csound.com/manual/siggen/granular/) is a good compact taxonomy.

### 4.8 Delay-line granulation as a named technique

The literature term is **"Delay Line Granular Synthesis"**: a delay line stores samples from a real-time input stream, and each grain reads from the delay line with a potentially different delay time and playback rate. **GrainProc** ([NIME 2013](https://www.nime.org/proceedings/2013/nime2013_99.pdf)) is a good academic treatment framed as "granular versions of common delay-line based effects, such as delay, reverb, and chorus" — which is a precise description of Brainscape's design space.

Related: feeding the granular *output* back into the recording table alongside the input is a documented technique for **granular reverb** ([Ervik & Brandtsegg, Csound Conference](https://www.incontri.hmtm-hannover.de/fileadmin/www.incontri/Csound_Conference/Ervik_Brandtsegg2.pdf)).

### 4.9 Other implementations (survey in progress)

- **VCV Rack** granular modules — see §4.9 additions below.
- **Max/gen~ and Faust** granular examples.
- **`mdeGranular~`** (Michael Edwards) — a Max/MSP + Pd granular external with published source, notable for a clean live-input granulation design.

---

## 5. Practical budgets on the Daisy Seed

### 5.1 Hardware facts (verified)

- **MCU:** STM32H750IB, ARM Cortex-M7 @ **480 MHz**, with FPU and DSP extensions.
- **Internal memory** ([Daisy memory page](https://daisy.audio/pages/memory-what-is-the-difference)):

  | Region | Size | Notes |
  |---|---|---|
  | FLASH (internal) | 128 KB | program |
  | ITCMRAM | 64 KB | zero-wait instruction RAM |
  | DTCMRAM | 128 KB | zero-wait data RAM — "internal" option for relocating small buffers like wavetables |
  | SRAM (AXI, `0x24000000`) | 512 KB | default variable storage |
  | RAM_D2 | 288 KB (256 KB + 32 KB DMA) | |
  | RAM_D3 | 64 KB | |
  | SDRAM (`0xC0000000`) | **64 MB** | external, 65MB-model Seed |
  | QSPI FLASH | 8 MB | |

- Daisy docs recommend internal memory for "oscillators, utilities, modulation, non time-based effects" and **external SDRAM for "samplers, delays, reverbs, loopers, granulators."**
- **SDRAM is cacheable.** libDaisy `src/sys/system.cpp` `ConfigureMpu()` sets MPU region 1: `BaseAddress = 0xC0000000`, `Size = MPU_REGION_SIZE_64MB`, `IsCacheable = MPU_ACCESS_CACHEABLE`, `IsBufferable = MPU_ACCESS_BUFFERABLE`, `IsShareable = MPU_ACCESS_NOT_SHAREABLE`, `TypeExtField = MPU_TEX_LEVEL0`.
- SDRAM part is an Alliance Memory mobile SDRAM (**AS4C16M32MSA-6BIN**, 512 Mbit, 32-bit wide, 166 MHz-rated part, 5.4 ns access), **run at 100 MHz with 32-bit accesses** per community reports → **~400 MB/s theoretical peak**. *(Bus width and 100 MHz clock: reported in Daisy community/forum posts — treat the exact clock as **unverified** pending confirmation from the schematic or `dev/sdram.c`.)*
- **Bootloader caveat:** the bootloader needs 32 KB at the end of the SRAM region, so SRAM-resident programs are capped at ~480 KB.
- Usage: declare SDRAM buffers with `DSY_SDRAM_BSS` (`__attribute__((section(".sdram_bss")))`). **The SDRAM is not zeroed at startup** — "initial condition is undefined" — so you must clear/fill it from an `Init()`-callable function, *not* a constructor. libDaisy's own docs say SDRAM "operates in much the same way as normal memory, just a little bit slower. For most things this won't be noticeable."

### 5.2 Memory budget: how much delay time?

At 48 kHz, per second of audio:

| Format | Mono | Stereo |
|---|---|---|
| `float` (4 B) | 192 KB/s | 384 KB/s |
| `int16_t` (2 B) | 96 KB/s | 192 KB/s |
| 8-bit µ-law (1 B) | 48 KB/s | 96 KB/s |

Therefore in **64 MB (67.1 MB decimal / 64 MiB = 67,108,864 B)**:

| Format | Mono | Stereo |
|---|---|---|
| `float` | ~349 s (5.8 min) | ~175 s (2.9 min) |
| `int16_t` | ~699 s (11.7 min) | ~349 s (5.8 min) |
| 8-bit µ-law | ~1398 s (23 min) | ~699 s (11.7 min) |

And in **512 KB of AXI SRAM**: stereo `int16_t` gives only **~2.7 s**. That is the whole argument for SDRAM. A pedal that wants a 30-second granular buffer physically cannot do it in internal RAM.

**Practical recommendation:** allocate a power-of-two stereo `int16_t` buffer. `2^23` frames = 8,388,608 frames = **174.8 s stereo** and consumes 32 MiB, leaving 32 MiB free for a second buffer (looper, freeze snapshot, reverb tank). Or `2^22` frames = 87.4 s for 16 MiB, which is more than any pedal needs and leaves 48 MiB of headroom.

### 5.3 CPU budget: cost of N overlapping grains

**The per-sample, per-grain inner loop** (full quality, stereo out, cubic interp, mono interleaved-stereo source) is roughly:

| Operation | Approx. cost |
|---|---|
| Advance 16.16 phase, extract index + frac | 3 int ops |
| Convert frac to float | 1 int→float + 1 mul |
| 4 buffer loads (cubic) | **4 loads — the dominant cost if they miss cache** |
| int16→float conversion ×4 | 4 ops |
| Cubic Hermite evaluate | ~5 mul, ~5 add (fits FMA) |
| Envelope: advance phase, fold, LUT lerp | ~6 ops + 2 loads (LUT is in fast RAM) |
| Multiply by env, by gainL/gainR, accumulate | 4 mul, 2 add |
| **Total** | **~30–40 operations, of which ~6 are memory loads** |

The Cortex-M7 is dual-issue, superscalar, with a 6-stage pipeline and single-cycle single-precision FMA. **In cache and with no stalls**, this loop plausibly retires in **~15–25 cycles per grain per sample**.

Budget at 48 kHz on 480 MHz: **10,000 cycles per sample frame** total.

| Grains | Cycles/sample (@20 cyc/grain, cache-hot) | % of budget |
|---|---|---|
| 8 | 160 | 1.6% |
| 16 | 320 | 3.2% |
| 32 | 640 | 6.4% |
| 64 | 1,280 | 12.8% |

**This looks trivially affordable — and that number is a lie, because of cache.** The real limit is memory, not arithmetic.

### 5.4 The real constraint: SDRAM latency and D-cache behavior

The STM32H7's Cortex-M7 has a **16 KB L1 data cache with 32-byte cache lines** (standard for STM32H7; *verify against the reference manual*). Implications for a grain delay:

- **32 bytes = 8 stereo `int16_t` frames = 16 mono `int16_t` samples.** A grain playing forward at rate 1.0 through a stereo int16 buffer therefore incurs **one cache miss every 8 samples**. With `float` stereo it's one miss every 4 samples — **twice the miss rate**. This is the single strongest argument for 16-bit storage.
- A cache miss to SDRAM costs on the order of **~20–60 cycles** (row activate + CAS + burst fill, at 100 MHz SDRAM = 4.8 CPU cycles per SDRAM cycle). Call it **~40 cycles** as a working estimate. *(Unverified — needs measurement on hardware.)*
- **Working set:** N grains at N scattered buffer positions = N independent streams. Each stream needs at least one resident cache line, and prefetching/streaming behavior degrades badly once N × (lines in flight) exceeds the cache's associativity per set. With 16 KB / 32 B = **512 cache lines** total, 64 streams is fine in raw capacity but can conflict badly if grain positions alias to the same cache sets (which power-of-two buffer strides make *more* likely, not less).

**Revised budget with misses:**

| Grains | Misses/sample (stereo int16, rate 1.0) | Cycles from misses (@40) | Arith cycles | Total | % of 10,000 |
|---|---|---|---|---|---|
| 8 | 1.0 | 40 | 160 | 200 | 2% |
| 16 | 2.0 | 80 | 320 | 400 | 4% |
| 32 | 4.0 | 160 | 640 | 800 | 8% |
| 64 | 8.0 | 320 | 1,280 | 1,600 | 16% |

Even pessimistically doubling this, **32–64 grains at 48 kHz is comfortably achievable on the Daisy Seed**, with room for filters, reverb, and UI. This is consistent with Clouds achieving 40–60 grains on a **168 MHz STM32F4 with no FPU-heavy budget and far less RAM**, and with the existence of working Clouds ports to Daisy (`DaisyCloudSeed`, `cumuloid`).

**Mitigations to apply anyway:**
1. **16-bit storage** (2× fewer misses than float).
2. **Interleaved stereo** (1 stream instead of 2 per grain).
3. **Render per-grain over the whole block, not per-sample over all grains.** Iterate `for each grain { for each sample in block }`, accumulating into a block-sized float scratch buffer in **DTCM**. This gives each grain a contiguous run of buffer reads — maximum spatial locality — instead of interleaving 32 scattered streams sample by sample. **This is the single biggest optimization available and it is exactly what Clouds' `OverlapAdd(destination, size)` structure does.**
4. **Keep the accumulator, LUTs, and grain structs in DTCM/internal SRAM.** Only the sample buffer belongs in SDRAM.
5. **Avoid power-of-two grain-position strides where possible** — or accept it; measure before optimizing.
6. **Consider a small per-grain prefetch** (`__builtin_prefetch` / `PLD`) one cache line ahead. Cortex-M7 supports `PLD`.

### 5.5 Block size

Larger blocks amortize per-block overhead (scheduler, parameter smoothing, function call, template dispatch) and dramatically improve cache locality under the per-grain-outer-loop scheme. But block size sets latency and quantizes grain onsets (mitigated by `pre_delay`).

- 48 samples @ 48 kHz = 1.0 ms — Daisy default-ish, good latency.
- 128 samples = 2.7 ms — better for cache, still fine for a pedal (total round-trip with codec ≈ 5–6 ms).

**Recommendation: 48–64 samples for the pedal build, with `pre_delay` implemented so onset resolution is 1 sample regardless.** Make block size a compile-time constant so loops unroll.

---

## Recommendations for Brainscape

*(preliminary — will be expanded)*

1. **Write your own grain engine.** DaisySP's `GranularPlayer` is 2 grains and static-buffer; it cannot be adapted. Model the architecture on Clouds (MIT, so you may also *use* its code if you attribute).
2. **Adopt the "resolve everything at schedule time" contract** — the one rule shared by Clouds, SuperCollider, and EmissionControl2. Grains never re-read global parameters after birth.
3. **Grain struct = POD, fixed array, no virtuals, no allocation.** 64 slots. Template the render function on quality/stereo/interpolation and dispatch once per grain per block.
4. **Render per-grain over the block into a DTCM float accumulator**, not per-sample over all grains.
5. **Implement `pre_delay`.** Sample-accurate grain onsets inside a block. Non-negotiable.
6. **16.16 fixed-point buffer phase**, not float — float32 loses sub-sample precision on a multi-minute SDRAM buffer.
7. **Stereo interleaved `int16_t` SDRAM buffer, power-of-two frames** (suggest `2^22` = 87 s, 16 MiB, leaving headroom).
8. **Quality tiering:** cubic interpolation + per-grain stereo for the first ~8 grains; linear + cheaper pan for the rest.
9. **Special-case rate == 1.0** to an integer copy path so unity-pitch grains are bit-exact — makes the pedal usable as a clean delay.
10. **Density = overlap-referenced with a cubic law**, grain rate derived. Expose jitter as the synchronous↔asynchronous morph.
11. **Envelope: Clouds' triangle-morphed-to-LUT** for cheapness, exposed with `partikkel`-style `sustain_amount` + `attack_decay_ratio`, and optionally EmissionControl2's expo↔Tukey↔reverse-expo single-knob morph.
12. **`1/√(N-1)` gain normalization, one-pole smoothed**, plus window-shape gain compensation.
13. **Feedback: granular output into the record buffer**, with DC block → highpass → feedback-dependent lowpass → soft saturation → optional allpass diffusion. Allow feedback > 1 as a designed self-oscillation feature.
14. **Add EmissionControl2's "intermittency"** (probability a scheduled grain is skipped) and **Argotlunar's parameter correlation matrix** — both cheap, both strong differentiators over Microcosm.
15. **Keep the dry path out of the block-delayed wet path** to avoid comb filtering on blend.
16. **Licensing:** Clouds MIT is safe to vendor with attribution. **Argotlunar (GPL v2), SuperCollider (GPL), Csound (LGPL) are study-only** if Brainscape ships under a permissive license.

---

## Sources

- Mutable Instruments eurorack source — https://github.com/pichenettes/eurorack
- Clouds manual — https://pichenettes.github.io/mutable-instruments-documentation/modules/clouds/manual/
- Clouds open source page — https://pichenettes.github.io/mutable-instruments-documentation/modules/clouds/open_source/
- Beads source status (MW) — https://www.modwiggler.com/forum/viewtopic.php?t=281283
- Beads github code (MI forum) — https://forum.mutable-instruments.net/t/beads-github-code/20206
- DaisySP — https://github.com/electro-smith/DaisySP
- DaisySP docs — https://electro-smith.github.io/DaisySP/index.html
- DaisySP `pitchshifter.h` — https://github.com/electro-smith/DaisySP/blob/master/Source/Effects/pitchshifter.h
- libDaisy external SDRAM guide — https://electro-smith.github.io/libDaisy/md_doc_2md_2__a6___getting-_started-_external-_s_d_r_a_m.html
- Daisy memory regions — https://daisy.audio/pages/memory-what-is-the-difference
- Daisy Seed datasheet — https://daisy.nyc3.cdn.digitaloceanspaces.com/products/seed/Daisy_Seed_datasheet.pdf
- DaisyCloudSeed (Clouds on Daisy) — https://github.com/erwincoumans/DaisyCloudSeed
- cumuloid (Clouds on Daisy Petal) — https://github.com/hfl1967/cumuloid
- Argotlunar — https://mourednik.github.io/argotlunar/
- Argotlunar source — https://github.com/mourednik/argotlunar
- EmissionControl2 — https://github.com/EmissionControl2/EmissionControl2
- EmissionControl2 announcement (UCSB) — https://music.ucsb.edu/news/latest-news/center-research-electronic-art-technology-create-releases-emissioncontrol2
- SuperCollider GrainIn — https://doc.sccode.org/Classes/GrainIn.html
- SuperCollider GrainBuf — https://doc.sccode.org/Classes/GrainBuf.html
- Csound partikkel — https://csound.com/manual/opcodes/partikkel/
- Csound granular opcodes overview — https://csound.com/manual/siggen/granular/
- Wikipedia: Granular synthesis — https://en.wikipedia.org/wiki/Granular_synthesis
- Curtis Roads, *Microsound* (MIT Press) — https://mitpress.mit.edu/9780262681544/microsound/
- GrainProc (NIME 2013) — https://www.nime.org/proceedings/2013/nime2013_99.pdf
- Granular reverb (Ervik & Brandtsegg) — https://www.incontri.hmtm-hannover.de/fileadmin/www.incontri/Csound_Conference/Ervik_Brandtsegg2.pdf
- COLA examples (Julius Smith, CCRMA) — https://ccrma.stanford.edu/~jos/sasp/COLA_Examples.html
- SciPy `check_COLA` — https://docs.scipy.org/doc/scipy/reference/generated/scipy.signal.check_COLA.html
- ST AN4839: L1 cache on STM32F7/H7 — https://www.st.com/resource/en/application_note/dm00272913-level-1-cache-on-stm32f7-series-and-stm32h7-series-stmicroelectronics.pdf
- mdeGranular~ — https://michael-edwards.org/software/mdegranular/mdegranular.shtml

## Open questions

- Exact Daisy SDRAM clock and CAS latency (need `libDaisy/src/per/sdram.c` or the schematic) — **unverified**.
- Measured cache-miss penalty for SDRAM on the Daisy — **unverified**, needs a hardware benchmark.
- Confirm STM32H750 D-cache size (16 KB assumed) and line size (32 B assumed) against RM0433.
- Hologram Microcosm's actual algorithm family (not documented publicly) — needs listening analysis.
- VCV Rack granular modules survey — pending.
- Faust / gen~ granular reference implementations — pending.
