# Output limiter: a safety ceiling at full scale

> The design pass for the output safety limiter. The audition page asked "At extreme knob corners,
> a sustained chord into an attack mode can reach +2.5 dBFS and clip the codec. Add an output
> safety limiter?", and the owner answered yes, as a sound revision of its own
> (`firmware/factory/AUDITION.md:43`, 2026-10-08). On 2026-10-09 the owner asked for the design to
> start now. This document specifies, precisely enough to build and test, a limiter that is
> inside the engine, so the pedal and the plugin play the same samples. It covers the
> algorithm, its arithmetic, where it sits, its control and display, its cost and code
> placement, its effect on the goldens and the audition, and its sound revision and order
> against CLOCK's tempo core and the cost governor.
>
> It extends [grain-engine.md](grain-engine.md) ("engine §N"),
> [determinism-profile.md](determinism-profile.md) ("profile §N"),
> [mode-compiler.md](mode-compiler.md) ("compiler §N"), [companion-app.md](companion-app.md)
> ("companion §N"), [cpu-budget.md](cpu-budget.md) ("budget §N") and [clock.md](clock.md)
> ("clock §N").
>
> The evidence comes from two sources, summarised in §12:
> - three research lanes: practice in comparable products, the DSP options with a prototype,
>   and the tree's constraints;
> - a prototype of this document's limiter, patched into `main` and built two ways. MSVC ran the
>   unit tests and the golden harness under eight deliveries. arm-none-eabi-gcc 10.3 built the
>   live image, whose map gives the ITCM figures.
>
> The lanes' and the prototype's code, scripts and logs are scratch work and are not committed.
> Code is cited as `path:line` at `main` `011b294` (sound revision 7). "LD" is the pinned
> libDaisy v9.0.0.
>
> Status: **draft for the owner's decisions** (2026-10-09). Each of the ten decisions in §11.2
> has a recommended answer. Nothing is built.

**Evidence labels.**

- **Measured:** run on the host (MSVC, `/fp:precise`), or bytes from a firmware build's map.
- **Model:** an estimate from the disassembly and the Seed's measured instruction latencies.
  Treat it as ±25 %.
- **Calculated:** derived from measured numbers.
- **Reported:** taken from a research lane's sources and not re-checked here.

---

## Summary

- **What.** A zero-latency peak limiter, linked across the two channels. It is the last step of
  the engine's output pass (Pass 3c), in `dsp/`, so the pedal and the plugin play the same
  limited samples.
- **When it acts.** Only when a sample of the mix would pass full scale. The threshold and the
  ceiling are both exactly 1.0: a hard knee at 0 dBFS. While no sample passes 1.0, the
  limiter's output is the input's bits, so a render that never clips keeps its hash.
  - 37 of the 45 golden renders keep their hashes (measured).
  - All 45 are identical under eight block, delivery and floating-point variants (measured).
  - The unit suite passes unchanged, apart from one click test whose renders run 0.9–3.2 dB
    over full scale (§9.4).
  - No factory preset changes at its stored positions.
- **How it acts.**
  - **Attack:** instant.
  - **Hold:** 10 ms, restarted while the over keeps coming back within 0.25 dB.
  - **Release:** 40 dB/s, ending at exactly 1.0f.
  - **Safety:** a gain floor of 2⁻²⁴ and a final clamp.
  - **No look-ahead.** The dry is never delayed, and `LatencySamples()` stays 0.
- **The dry signal.** The ceiling of each channel is the larger of 1.0 and the dry term's own
  level.
  - On the pedal this is always 1.0.
  - In the plugin, Mix 0 stays the input on any input, and a dry hotter than 0 dBFS is never
    pulled below its own level.
- **Arithmetic.**
  - Only +, −, ×, ÷, compares and bit operations.
  - One division per limited frame on the pedal.
  - No transcendental per sample. The release step is computed once, at `Init`.
  - No flush site is needed.
- **Cost.**
  - About 15–25 cycles per frame while idle and at most about 110 while limiting, so at most
    about 5.3k cycles per 48-frame block (1.1 %). This is a model, charged to the governor's
    static reserve S.
  - The live image grows by 864 bytes of ITCM (measured), from 3,208 bytes spare to 2,344.
- **Control.** None: it is always on, with no parameter row and no device setting. The plugin
  shows a limiter lamp and the gain reduction. The pedal's console reports the counts.
- **Revision.** Sound revision 8, landing before the cost governor and CLOCK's tempo core,
  which become 9 and 10 in landing order. It should land before the owner's knob-rating rows are
  written, so no rating needs a re-listen.
- **Work.** About 6–8.5 engineer-days in six lanes (§11.1).

---

## 1. Scope

### 1.1 The request, and what this pass delivers

The engine forms its output as dry·gd + wet·g·gw, where gd and gw are the Mix law's gains and g
is the wet gain. Its only conditioning is a saturation at ±`FLT_MAX` (`dsp/src/Engine.cpp:1231-1260`,
`:193`). So any over is clipped by whatever comes after the engine:

- **The pedal.** The codec's conversion clamps every over to just under full scale.
- **The plugin.** The host receives values above 1.0.

The pre-screen measured how far over the factory set goes (`firmware/factory/AUDITION.md:43`;
`docs/STATUS.md:160`):

- **Stored positions on the class inputs:** never above −1 dBFS.
- **Attack modes' sweeps and S11 corners on SoftNotes:** up to +2.5 dBFS (Echolalia +2.5,
  Updraft +2.4, Callback +1.7, Déjà Vu +1.7, Kaleido +1.3, Pinhole +1.2, Refrain +1.0,
  Retrograde +0.8, Engram +0.7).
- **S0 on the Saturation vector:** +5.5 to +10.7 dBFS on all 18 modes.
- **Lull's S10 and Runaway's S8:** just over 0 dBFS.

This pass designs:

1. the algorithm and its constants (§4);
2. its arithmetic under the determinism profile (§5);
3. where it sits, and why the plugin gets it too (§6);
4. its control and display (§7);
5. its cost, how the governor charges it, and where its code lives (§8);
6. its effect on the goldens, the tests and the audition (§9);
7. its sound revision and order (§10);
8. the plan and the decisions (§11).

### 1.2 Goals

1. **A safety ceiling.** No sample leaves the engine over full scale on the pedal, for any preset,
   knob position, event stream or input the codec can deliver.
2. **Nothing changes where nothing clipped.** While no sample of the mix exceeds 1.0, the output
   is today's output, bit for bit: every golden render and factory render that never passes full
   scale keeps its hash.
3. **One sound.** The plugin plays exactly what the pedal plays, within the parity contract
   (profile §2).
4. **Sample-identical** on MSVC, GCC, Clang and the M7, for any block split, delivery and
   caller floating-point environment.
5. **Bounded and charged.** Its worst cost per block is a constant the governor can reserve.
6. **Quiet when it acts.** It acts with a gain, not a waveshape. Overs of a few dB on sustained
   material become a level dip. The waveform bends only at the sample where a new maximum
   arrives (§4.6).

### 1.3 Non-goals

| Not a goal | Why, and where it goes |
|---|---|
| A creative compressor, a "glue" or a loudness maximiser | It never acts below full scale. Character belongs to modes; a drive or compressor would be a stage of its own, with its own design |
| A fix for hot inputs | The Saturation vector's +5.5 to +10.7 dBFS is an input-staging problem. It belongs to the input level, an Instrument/Line switch or an input AGC (the practice lane, §12). The limiter keeps those renders from clipping, but by ducking the whole mix |
| Protection of the feedback loop | The tamer's saturator already bounds it (engine §2.3, `dsp/src/detail/PostChain.h:32`). The limiter is outside the loop |
| True-peak (inter-sample) limiting | It needs a 4× oversampled side chain, which is BS.1770-4 Annex 2 practice, and a ceiling below 1.0, which changes the full-scale null (§3.4). Deferred until the Rev7's DAC behaviour is measured (D10) |
| The plugin's output trim and the pedal's analog output level | Both act after the engine, outside parity, at the user's choice (§6.3) |
| A limiter in the looper | The looper has its own design |

### 1.4 Terms

| Term | Meaning |
|---|---|
| s | The mix sample per channel: today's `oL`, `oR` after the ±`FLT_MAX` saturation (`Engine.cpp:1250-1258`) |
| a | The dry term of the mix per channel, `dry·gd` |
| p | max(\|s_L\|, \|s_R\|): the linked detector |
| C | The ceiling, 1.0f. Each channel's ceiling is c = max(C, \|a\|), which is C on the pedal |
| T | The threshold. In this design T = C |
| r | The gain the frame requires: 1 while p ≤ C, else min over channels of c/\|s\|, floored |
| G | The limiter's gain state, in [2⁻²⁴, 1]. "Idle" means G = 1 with no sample over C |
| Demand | A frame with p > C (equivalently r < 1) |
| H | The hold, in frames: 480 at 48 kHz |
| k | The per-frame release factor: 40 dB/s, about 1.0000960 at 48 kHz |
| Over | A sample of s with \|s\| > 1.0 |

### 1.5 Principles

1. **Engage only where clipping would occur.** Full scale is the one threshold at which "nothing
   changes where nothing clipped" holds by construction (§3.4).
2. **Gain, not shape.** The output is the input times a slowly varying gain. The only waveform
   bending is at the sample where a new maximum arrives (§4.6).
3. **Zero latency.** Instant attack and no look-ahead: the dry path stays undelayed
   (`dsp/include/brainscape/Engine.h:289-290`; grain-delay theory §3.11).
4. **Exact unity, by comparison, not convergence.** The idle path returns before any
   arithmetic, and the release is capped at exactly 1.0f.
5. **In the engine, after everything that sounds.** It is the last step of Pass 3c, outside the
   feedback loop, so it never changes the engine's state.

---

## 2. The problem in numbers

### 2.1 How the output is formed

Pass 3c (`Engine.cpp:1231-1261`) runs per sample, after the post chain.

1. It reads the smoothed Mix through the Mix law, dry = min(1, 2(1 − m)) and wet = min(1, 2m)
   (`dsp/src/detail/MixLaw.h:35-39`).
2. It reads the smoothed wet gain (`Engine.cpp:162-168`). That gain is either exactly 0 under
   the cutoff kill, or `Exp2F` of the mode's trim plus the player's effect volume.
3. It forms `dry·mix.dry + wet·g·mix.wet`. This two-multiply form keeps the endpoints exact.
4. It saturates at ±`FLT_MAX` with one-sided compares, so NaN still reaches the Debug assertion
   (`:1252-1258`).
5. It stores the result.

Reachable levels:

- The wet trim spans −24 to +24 dB (`Params.h:189`) and the effect volume −24 to +12 dB
  (`Params.h:267`), so the wet can be raised by up to 36 dB.
- At Mix 0.5 both gains are 1, so a full-scale input reaches +6 dBFS before any wet gain.
- The feedback tap is written in Pass 3a (`Engine.cpp:1209-1225`), before Pass 3c, so nothing
  in Pass 3c reaches the engine's state.

### 2.2 How it reaches the codec and the host

**The pedal.**
- `AudioCallback` hands libDaisy's buffers to `Process` and adds nothing
  (`firmware/live/main.cpp:100-106`).
- LD converts with `f2s24(out × output_adjust_)`, where `output_adjust_` is 1 because
  `postgain` is 1 (LD `src/hid/audio.cpp:447-453`, `:121`).
- `f2s24` clamps to ±`FBIPMAX` = ±0.999985, multiplies by 2²³ and truncates, so its largest code
  is 8,388,482 (LD `src/daisy_core.h:33-34`, `:146-151`; profile §2.2).
- Today, then, the pedal hard-clips every over, about 0.00013 dB under full scale.

**The plugin.**
- `RenderChunk` runs the engine, then applies the wrapper's output level, then meters. It never
  clamps (`plugin/src/PluginProcessor.cpp:1061-1108`).
- A DAW therefore receives the overs as floats.

**So the two outputs already differ above full scale.** Once the engine stops at 1.0, the codec
clamp only ever touches samples in (0.999985, 1.0]. That is a 125-code band, 1.3·10⁻⁴ dB. It
lies outside the parity contract, whose boundary is the engine's float output (profile §2.2).

### 2.3 How far over it goes: the revision-7 goldens

Each golden render at revision 7 was dumped and passed through the limiter of §4 (measured).
Because the limiter is outside the feedback loop, applying it to the dumped output equals
building it into the engine, and the golden harness agrees (§9.1). Eight renders cross full
scale:

| Preset | Peak today | Frames over today | Frames limited | Engagements | Lowest gain | Frames at the ceiling |
|---|---|---|---|---|---|---|
| `saturation_6s/hot_out` | +27.73 dBFS | 259,739 | 275,993 | 1 | −27.73 dB | 1,524 |
| `plucks_automation_12s/automation_offgrid` | +11.41 | 5,752 | 68,147 | 6 | −11.41 | 79 |
| `plucks_state_14s/spillover_chain` | +1.77 | 3 | 2,605 | 1 | −1.77 | 2 |
| `plucks_state_14s/exact_load_mid` | +1.32 | 8 | 5,770 | 4 | −1.32 | 5 |
| `plucks_wave1_12s/midi_gate` | +1.22 | 8 | 2,930 | 2 | −1.22 | 3 |
| `plucks_12s/strum_marks` | +0.40 | 3 | 1,293 | 1 | −0.40 | 2 |
| `soft_notes_10s/post_max` | +0.37 | 14 | 939 | 1 | −0.37 | 8 |
| `plucks_modes_14s/lone_changes` | +0.08 | 1 | 577 | 1 | −0.08 | 1 |

The other 37 peak at or below −0.60 dBFS. In order: `selfosc` −0.60, `onset_burst` −0.84,
`dense_1ms` −1.24, `freeze_long` −1.46, `mono_stutter` −1.66, `restart_kept_params` −1.70, and
the rest at or below −2.14. `default_silence` is silent.

### 2.4 Contracts the limiter must keep

| Contract | Where | What a limiter must do |
|---|---|---|
| Mix 0 plays the dry input bit for bit, up to the sign of a zero, on hostile input up to ±`FLT_MAX` | `MixLaw.h:15-27`; `dsp/tests/test_modes.cpp:506-558` | Leave a mix with no wet term untouched at any level |
| Every Mix plays dry·LawDry + wet·LawWet bit for bit | `test_modes.cpp:469-504` | Not engage on that test's renders (§9.4) |
| Contract #2: the unity-rate Tu path nulls to the bit, including a 1.0 impulse at Mix 1 | engine §10; `dsp/tests/test_engine.cpp:320-347` | Leave a sample of exactly 1.0 untouched |
| The trim and effect volume never scale the dry | `Engine.cpp:1236-1238`; `test_modes.cpp:356-368` | Do nothing at Mix 0 under any trim |
| The dry is never delayed; the plugin reports 0 latency | `Engine.h:289-290`; engine §2 item 5 | No look-ahead |
| Block-split invariance; no per-block decision | engine §10 #1; profile §5.7 | Per-sample state only |
| Finite in, finite out; NaN reaches the Debug check | profile §3.7; `Engine.cpp:1252-1272` | One-sided compares and a bounded gain |
| Recursive state is flushed per sample | profile §4.3 | No state that decays toward 0, or a flush site |
| The plugin's live dry passes above 0 dBFS unclipped | profile §3.7; companion §4.8 | A dry hotter than 0 dBFS is never pulled under its own level |
| `Reset` and `Restart` give the exact-restart state; Spillover keeps smoothers | `Engine.h:97-112`; `Engine.cpp:667-705`, `:754-770` | Prime on `Reset`; carry over on Spillover |

---

## 3. Options considered

### 3.1 What comparable products do (reported)

| Product | Output protection | Control |
|---|---|---|
| Strymon BigSky MX | An "adaptive limiter" for reverb build-up from hot input, added in firmware v2.02 (July 2024). No details are published | none found |
| Strymon TimeLine/BigSky, Chase Bliss MOOD | Analog dry path: only the wet reaches the DAC | Kill Dry |
| Mutable Clouds | A static Padé soft clip on dry plus wet. Nominal level already sits 0.6 dB into the curve | none |
| Mutable Rings | A linked peak follower (attack 0.05 per sample, release 2·10⁻⁵ per sample), gain 1/peak above 1, then ×0.8 into the same Padé soft clip | none |
| DaisySP `Limiter` | Rings' follower, then ×0.7 into the soft clip | library |
| Fractal Axe-Fx | A hard limit at 0 dBFS before the D/A, which users call harsh (forum) | none |
| Eventide H90, EHX 45000, Red Panda Particle | No output limiter documented. The H90's clip LEDs sit before its gain stages and miss real clipping | per-algorithm gain |
| Airwindows ClipOnly2 | A safety clipper: unclipped samples pass untouched | none |
| Chase Bliss Lossy, Meris LVX, Beads | Limiters as creative effects | user |
| Ableton Limiter | 1.5–6 ms look-ahead, so it adds latency | full |

Three patterns stand out:

- Limiters meant as protection are always on and have no control. Limiters a user can adjust
  are creative effects.
- The products with a digital dry either soft-clip the whole sum or hard-clip it. None documents
  look-ahead.
- Limiting inside the feedback loop is universal. Brainscape already has it, in the tamer's
  saturator.

### 3.2 The candidates

Each candidate was prototyped by the DSP lane and run on sine bursts with a 0.5 ms onset, 2.5 dB
and 10.7 dB over full scale (host-run). The residual is measured against the best linear gain,
in dB relative to the output. "Onset" covers 0–20 ms and "steady" 0.3–1.3 s.

| | Algorithm | Unity below threshold | Latency | Onset residual, +2.5 dB (110 Hz / 1 kHz / 5 kHz) | Steady state | Cost | Verdict |
|---|---|---|---|---|---|---|
| A | Static soft clipper (Clouds-like) with exact identity below T | yes | 0 | −19.1 / −19.0 / −19.0 dB | −19.0 dB residual, aliasing −29.6 dB at 5 kHz; at +10.7 dB −9.6 dB, a fuzz | 10–40 cycles per channel | Rejected: it is a distortion |
| **B** | **Peak limiter, linear gain, hard knee at full scale** | **yes, exact** | **0** | **−24.2 / −29.1 / −31.9 dB** | **−153 dB from 82 Hz up; −67 dB at 41 Hz** | **15–25 idle, ≤ 110 limiting** | **Chosen** |
| C | B with a quadratic soft knee from −1 dBFS to 1.0 | yes, below −1 dBFS | 0 | −24.4 / −29.1 / −32.0 dB | as B | B plus four operations; a second division on frames where the dry passes −1 dBFS | The owner's alternative (D2) |
| D | B with a dB-domain gain computer (`LogF`, `Exp2F`) | yes | 0 | within 0.1 dB of B and C | as B | +300–450 cycles per limited frame | Rejected: cost |
| E | 1 ms look-ahead brickwall | yes | 48 frames | −29.5 / −39.5 / −46.5 dB | −51 dB at 110 Hz | 0.6–1 KiB state, 40–80 cycles | Rejected: delays the dry, moves every golden, needs a reported latency |
| F | Finite attack (0.5 ms) then a soft clipper (the Rings and DaisySP topology, adapted to unity) | yes, once adapted | 0 | −21.0 / −22.9 / −23.2 dB | −57 dB at 110 Hz | two stages, about 1 KB | Rejected: a slower attack moves the onset distortion into the clipper |
| G | No limiter (the codec clips), or a clamp in the firmware only | — | — | — | hard clip on every over | 0 | Rejected: the owner asked for one, and pedal and plugin keep differing |
| H | A limiter in the plugin only, or in the firmware only | — | — | — | — | — | Rejected: breaks "the plugin plays what the pedal plays" (§6.3) |

The originals of F scale every sample by 0.8 or 0.7 and let their envelopes decay toward 0. With
those, nothing is bit-exact below the threshold, and the envelopes would need a flush site.

### 3.3 Where the gain applies

| Variant | On the pedal | In the plugin, dry over 0 dBFS | Contracts |
|---|---|---|---|
| Scale the whole sum: a master limiter | The dry dips with the wet while limiting | A hot dry at Mix 0 is pulled to 0 dBFS | Breaks the hostile Mix-0 test and the plugin's "dry unclipped above 0 dBFS" |
| Scale the wet only, detecting on the sum | The dry is never touched. But the wet's gain then depends on the dry's instantaneous value: with a dry at full scale and a wet at −6 dBFS, the wet drops 38.6 dB (host-run), and it intermodulates with the dry | as on the pedal | Keeps every dry contract; rejected for its sound |
| **Scale the whole sum, with each channel's ceiling at max(1, \|dry term\|)** | **The same as the master limiter, because the codec's dry is always under 1.0** | **Never louder than the hot dry. Mix 0 is the identity** | **Keeps every dry contract** |

The dry-referenced ceiling was checked two ways (measured):

- **Mix 0 on hostile input.** 1,000,000 random finite dry frames up to ±`FLT_MAX`, with no wet
  term: 0 changed.
- **A hot dry with a wet.** A dry clipped at +3 dBFS plus a wet at −6 dBFS, at Mix 0.5: the
  output peaks at +3.00 dBFS, the dry's own peak, and the lowest gain is −2.64 dB.

### 3.4 The choice: B with a hard knee at full scale, on the whole sum with a dry-referenced ceiling

**Why a hard knee at full scale.** It is the only threshold at which three properties hold by
construction, not by measurement:

1. **A 1.0 sample is never touched.** Contract #2's full-scale impulse and its test are
   unchanged.
2. **On the pedal, the dry alone can never engage it.** The codec's input is under 1.0
   (`InputCondition.h:21-27`; LD `s242f`), and the dry gain is at most 1.
3. **Only renders that clip today change.** That is 8 of 45 goldens, and on the factory set
   only the renders that already pass 0 dBFS.

**What a soft knee from −1 dBFS (C) would cost.** It changes renders that never clipped:

- 10 goldens instead of 8: `selfosc` at −0.60 dBFS and `onset_burst` at −0.84 join.
- Every factory sweep and S11 render between −1 and 0 dBFS, including the tightest at −0.01
  dBFS (Halation at Pinhole's positions).
- Contract #2 must be restated "below −1 dBFS", because the knee turns the 1.0 impulse into
  0.973.
- The Mix-law test holds only while its sums stay under −1 dBFS.

**What the soft knee buys.** Less bending of the attack edge on large, sudden, low overs:

| Over | Frequency | HF-weighted onset residual, hard knee | Soft knee |
|---|---|---|---|
| +2.5 dB | 110 Hz | −4.9 dB | −21.8 dB |
| +10.7 dB | 110 Hz | +14.4 dB | +5.7 dB |
| either | 1 kHz and above | within 2 dB of the soft knee | |

The residual is weighted by the third difference (host-run).

On program material the difference shrinks. On `strum_marks`, the one changed golden the DSP
lane computed it on, its HF splatter index is −1.45 dB with the hard knee and −1.46 dB with the
soft knee (host-run).

The engine's click test, `post-delay time automation is click-free`, shows the same. Its
feedback-0.9 render runs 0.9 dB over full scale. The largest local outlier of its second
difference is 5.84 times its neighbourhood with the hard knee, and 4.19 times with the soft
knee, against the test's 2.0 (measured). Both knees fail the same way: when a new maximum
arrives from a held gain, the knee's slope is below the held gain, so the waveform bends
(§4.6). Only look-ahead avoids that bend.

**Recommendation.** B with the hard knee (D2). Lane L2 renders an A/B of both knees for the
owner before the revision is minted (§11.1). The knee is two constants and one branch in the same
code. The alternative costs four operations per limited frame, and a second division on frames
whose dry passes −1 dBFS, which happens on the pedal. It changes nothing structural.

---

## 4. The algorithm

### 4.1 Constants

| Name | Value | binary32 | Source |
|---|---|---|---|
| `kLimCeiling` (C, also T) | 1.0 | `0x3F800000` | literal |
| `kLimFloor` | 2⁻²⁴ (−144.5 dB) | `0x33800000` | literal `0x1p-24f` |
| `kLimHoldTol` | 10^(0.25/20) ≈ 1.0292005 | `0x3F83BCD8` (1.02920055) | hex-float literal `0x1.0779b0p+0f` |
| `kLimReleaseDbPerS` | 40 dB/s | — | literal, binary64 |
| `kLimHoldSeconds` | 0.010 s | — | literal, binary64 |
| `release` (k) | 2^(40·log₂10/20 / R) | at 48 kHz `0x3F800325` = 1 + 805·2⁻²³ ≈ 1.0000960, which is 40.0074 dB/s | `Init`: `(float)Exp2D(40.0 * 0x1.542a5a12e1c5ap-3 / sampleRate)` |
| `holdFrames` (H) | round(0.010·R) | 480 at 48 kHz | `Init`: `RoundHalfAwayI32(0.010 * sampleRate)` |

`0x1.542a5a12e1c5ap-3` is log₂10/20 in binary64, the same value as `WetGainTarget`'s
`0.16609640474436813f` (`Engine.cpp:167`), carried at double precision.

### 4.2 State

| Field | Type | Range | Reset |
|---|---|---|---|
| `gain` (G) | float | [2⁻²⁴, 1] | 1 |
| `hold` | uint32 | [0, H] | 0 |
| `release`, `holdFrames` | float, uint32 | from `Init` | kept |
| `limitedFrames`, `engagements` | uint64 | counts, not sound | `Init` only |
| `minGain` | float | (0, 1] | `Init` and `ConsumeLimiterMinGain` |

These add 40 bytes to `Engine::Impl`: 7,944 bytes become about 7,984 of `kEngineImplBytes`' 8,192
on the M7 (compiler §7.1). The firmware's 9 KiB DTCM slot is unchanged, and the prototype's live
image linked with the same DTCM use (measured).

### 4.3 Per frame (normative)

`detail/OutputLimiter.h`, header-only, called once per frame from Pass 3c (§6.1). The order of
operations is as written. Each line is one IEEE operation, a comparison or a selection.

```cpp
// a: the dry terms of the mix (dry * mix.dry); s: the mix, after the ±FLT_MAX saturation.
inline void OutputLimiter::Frame(float aL, float aR, float& sL, float& sR) noexcept {
  const float pL = detmath::Abs(sL);                 // sign bit cleared
  const float pR = detmath::Abs(sR);
  const float p  = pL > pR ? pL : pR;                // linked detector
  if (gain == 1.0f && !(p > kLimCeiling)) return;    // idle: the mix's own bits
  const float cL = Ceiling(aL);                      // max(1, |a|): 1 on the pedal
  const float cR = Ceiling(aR);
  float r = 1.0f;                                    // the gain this frame requires
  if (p > kLimCeiling) {
    if (cL == kLimCeiling && cR == kLimCeiling) {
      r = kLimCeiling / p;                           // the pedal: one division
    } else {                                         // a dry over 0 dBFS (plugin only)
      const float rL = Need(cL, pL);                 // x > c ? c / x : 1
      const float rR = Need(cR, pR);
      r = rL < rR ? rL : rR;
    }
    if (r < kLimFloor) r = kLimFloor;
  }
  const bool wasOne = gain == 1.0f;
  if (r < gain) {                                    // attack: instant
    gain = r;
    hold = holdFrames;
  } else if (r < 1.0f && r <= gain * kLimHoldTol) {  // demand within 0.25 dB: keep holding
    hold = holdFrames;
  } else if (hold != 0u) {
    --hold;
  } else {                                           // release, never past r <= 1
    const float up = gain * release;
    gain = up < r ? up : r;
  }
  if (gain == 1.0f) return;                          // released this frame: no sample was over
  engagements += wasOne ? 1u : 0u;                   // statistics, not sound
  ++limitedFrames;
  if (gain < minGain) minGain = gain;
  sL = Clamp(sL * gain, cL);                         // one-sided compares: NaN passes through
  sR = Clamp(sR * gain, cR);
}
```

`Ceiling(a)` returns `Abs(a) > 1 ? Abs(a) : 1`. `Clamp(x, c)` is `if (x > c) x = c; if (x < -c) x = -c;`.

When the channel ceilings are both 1, the one-division branch equals the general branch bit for
bit. That is because `fl(1/x)` is non-increasing in x, so the smaller of the two channels'
required gains is `fl(1/p)`. The branch is therefore an optimisation, not a second rule.

### 4.4 Properties

1. **Identity.** With G = 1 and every sample at or below 1.0 in magnitude, the function returns
   before any arithmetic. The output is the input's bits, −0 and subnormals included, and the
   state is unchanged.
   - Checked exhaustively at G = 1 on all 1,065,353,217 magnitudes from 0 to 1.0, with both
     signs: 0 changed (measured).
   - The general path leaves the same state, because G = 1 implies `hold` = 0: G reaches 1 only
     in the release branch, which runs only at `hold` = 0. So the early return is an
     optimisation and not a rule of its own.
2. **Ceiling.** For every finite input, |out_c| ≤ max(1, |a_c|).
   - After the update, G ≤ r. The attack sets G = r, the hold branches run only when r ≥ G, and
     the release takes min(G·k, r).
   - A channel at or under its ceiling therefore stays under it. For the channel above it, the
     one-step product fl(p·fl(1/p)) never exceeds 1 for any float 1 < p < 2¹²⁶: 0 of 1,056,964,607
     floats exceed it (measured, exhaustive). Over the mantissas of (1, 2), which repeat at every
     exponent below 2¹²⁶, the lowest result is 1 − 2⁻²⁴.
   - From 2¹²⁶ to `FLT_MAX` the reciprocal is subnormal, and 4,027,544 floats give up to
     1 + 2⁻²². The gain floor can also leave s·G above the ceiling. The final clamp catches both,
     and is why it runs on every limited frame.
3. **Return to unity.** After the last frame with demand, G is exactly 1.0f within H frames plus
   the release. From that frame on, the output is bit-identical to the unlimited engine's,
   because the limiter never fed back. Measured times to exact unity at 48 kHz, from a single
   over:

   | Gain reduction | Frames to exact unity | Time | Of which hold | Of which release |
   |---|---|---|---|---|
   | 0.1 dB | 601 | 12.5 ms | 10 ms | 2.5 ms |
   | 1.0 dB | 1,681 | 35.0 ms | 10 ms | 25.0 ms |
   | 2.5 dB | 3,481 | 72.5 ms | 10 ms | 62.5 ms |
   | 6.0 dB | 7,680 | 160.0 ms | 10 ms | 150.0 ms |
   | 11.4 dB | 14,159 | 295.0 ms | 10 ms | 285.0 ms |
   | 27.7 dB | 33,715 | 702.4 ms | 10 ms | 692.5 ms |
   | From the floor, after a ±`FLT_MAX` burst | | 3.622 s | | |
4. **Liveness.** For every G in [2⁻²⁴, 1), G·k > G after rounding: k − 1 = 805·2⁻²³, about 400
   ulps of G. So the release rises strictly every frame and never stalls short of 1.
5. **The hold is restarted only under demand.** A variant that restarted the hold on any frame
   whose required gain sat within 0.25 dB of G, demand or not, never released. After the over
   ended it stayed at −0.25 dB, or −0.20 dB after a 0.2 dB over (measured). The `r < 1` term is
   normative.
6. **Linked channels.** Both channels share one gain, so the stereo image and the level
   difference between channels never move. On Plucks' alternating pan, an over in one channel
   lowers both.
7. **The dry contracts.** At Mix 0, or under the cutoff kill, s = a exactly (up to the sign of a
   zero).
   - On the pedal, |a| < 1, so the limiter is idle.
   - In the plugin, each channel's ceiling is |a| = |s|, so r = 1. The limiter is idle if G = 1,
     and otherwise releases without demand.
   - So "Mix 0 is the input" holds on every input whenever no limiting is in progress. The one
     exception is a Mix that reaches 0 within a release that is still running. That cannot
     happen in the hostile test, which loads at Mix 0.
8. **Split and environment invariance.** Every decision is per sample and keyed on nothing but
   the stream, and the code runs inside `Process`' guard. With the limiter patched into `main`,
   all 45 golden hashes are identical in eight cases (measured):
   - blocks of 48, of 1, of 512 and the pattern {48, 1, 127, 32};
   - random block sizes (seed 7);
   - a caller environment with FTZ|DAZ and round toward zero;
   - a fresh engine per render;
   - split event delivery.
9. **Finite.** Finite in, finite out. G stays within [2⁻²⁴, 1] and |s| ≤ `FLT_MAX`, so s·G
   cannot overflow. NaN, which the wrappers' sanitizing excludes, passes through unchanged and
   reaches the Debug assertion, as today.

### 4.5 Behaviour by case

| Case | What happens |
|---|---|
| A preset that never passes full scale | Nothing, bit for bit |
| A transient over (a pluck's attack 1.3 dB over) | The over's samples are scaled into the ceiling. The sample at the new peak sits at 1.0, and 1–8 frames per render sit exactly at the ceiling on the goldens with moderate overs. Then 10 ms of constant gain, then a 40 dB/s rise |
| A sustained over (feedback build-up, the +2.5 dBFS chord) | The gain settles to 1/peak. While the peaks keep returning within 0.25 dB the hold restarts and the gain holds still: ripple 0.012 dB at 41 Hz and 0 from 82 Hz up (measured) |
| A level that keeps rising | Each new maximum lowers the gain on its sample. Each lowering bends the waveform there, by the amount of the rise (§4.6) |
| Self-oscillation (feedback above 1) | Unchanged inside the loop. The tamer's saturator still bounds it, and the limiter caps what is heard. When the loop decays the gain follows it back up |
| DC | A steady gain reduction |
| The input stops | Tails decay through the release. The gain is 1 again within §4.4's times, and the output is today's from then on |
| A Mix move | The gain applies to the whole sum, so with a hot input the dry dips by the gain reduction while limiting. On the pedal the dry alone never engages it |
| The cutoff kill | Equivalent to Mix 0 for the wet: idle on the pedal |
| Freeze, triggers, bursts | No interaction: the limiter sees only the mix |
| Spillover load (Trails, FastCut) | The gain and hold carry over, as the smoothers do (`Engine.cpp:754-770`), so a load mid-limiting does not jump the level |
| Exact load, `Restart`, `Reset` | G = 1 and hold = 0 (§5.4) |
| A plugin dry over 0 dBFS | The output is never louder than the dry; a wet that would add is held to the dry's level |
| ±`FLT_MAX` input (plugin, hostile) | Output at most max(1, \|a\|) and finite. The gain can reach the floor; recovery takes up to 3.6 s |

### 4.6 What it sounds like

- **Sustained material.** From 82 Hz up the gain is exactly constant: the residual against a
  linear gain is about −153 dB at 82, 110 and 220 Hz (measured). The DSP lane measured the
  aliasing of a 5 kHz sine at −152.5 dB.
  - At 41 Hz with the hold restart, the residual is −67 dB and the ripple 0.012 dB.
  - Without the restart, a plain 10 ms hold gives −49 to −61 dB at 41–220 Hz and 0.05–0.135 dB
    of ripple.
- **Onsets.** The onset residual of a +2.5 dB burst is −24.2, −29.1 and −31.9 dB at 110 Hz,
  1 kHz and 5 kHz. Today the codec clips the same burst on every cycle, so it is never clean
  there.
- **The bend at a new maximum.** Zero latency means the gain can only drop at the sample that
  needs it. The rising edge of a new maximum is held at the ceiling up to its peak, and the
  waveform bends there.
  - On the goldens this means 1–8 samples per render at the ceiling (79 on
    `automation_offgrid`, 1,524 on `hot_out`), where today the codec clips 1–5,752.
  - The engine's click test measures the bend at 5.84 times its neighbourhood's second
    difference on a 0.9 dB over (§3.4).
  - A soft knee lowers that to 4.19 times, and only look-ahead removes it.
  - This is the behaviour the owner's A/B (D2) and the audition renders (§9.5) should be heard
    for.
- **Hot inputs at Mix 0.5.** On the Saturation vector the whole mix is pulled down by up to
  10.7 dB while the wet sounds, the dry included. This is the expected cost of a safety device
  on a mis-staged input. The fix is input staging (§1.3).

---

## 5. Arithmetic and determinism

### 5.1 The operations

All of these are in profile §3.1's set:

- sign-bit clears (`detmath::Abs`);
- compares and selections;
- one binary32 division per limited frame on the pedal, and two on the plugin's hot-dry path;
- two multiplies by the gain;
- one multiply each for the hold test and the release step;
- an integer counter.

There is no transcendental per sample and no expression that a compiler could contract.

The two-multiply mix becomes `aL = dryL * mix.dry; oL = aL + wl * mix.wet`. That is the same two
products and one sum in the same order, so the mix's bits do not change (§9.1). The products
`gain * kLimHoldTol` and `gain * release` are single roundings.

The M7's `VDIV.F32` and the x86 `DIVSS` are both correctly rounded, so they agree. The division
costs 18 cycles on the Seed (`firmware/records/rev7-2026-10-07/session-1/bench-all.md:64`).

### 5.2 Constants computed at `Init`

`release` and `holdFrames` are computed once, in `Init`, as `Smoother::SetTau` computes its
coefficient (`dsp/src/detail/Smoother.h:18-21`). The release uses `Exp2D` in binary64 with one
rounding to binary32 (profile §3.9), and the hold uses the exact `RoundHalfAwayI32`
(`DetMath.h:51-57`).

At 48 kHz the results are `0x3F800325` and 480. A unit test pins both. Every pedal-exact path
runs at 48 kHz (profile §2.2), and other rates compute their own k and H in the same way.

### 5.3 No flush site; subnormal outputs

**No flush site.** The gain never decays toward 0. It lies in [2⁻²⁴, 1] and moves up
multiplicatively, and the hold is an integer. So profile §4.3 adds no site. This is why the
design rejects an envelope follower that decays to 0: as in the Rings and DaisySP limiters, that
would need a per-sample `FlushTiny`.

**Subnormal outputs.** The product s·G is subnormal only when |s| < 2⁻¹²⁶/G.

- On pedal-faithful inputs such a sample is not expected. The dry is on the 2⁻²³ grid, the
  wet's recursive states are flushed below 10⁻²⁰, and G ≥ 2⁻²⁴. With the prototype,
  `SubnormalOutFrames` and every other existing counter were unchanged on all 45 golden presets
  (measured).
- Elsewhere the product is deterministic under the profile's gradual underflow on every target
  (profile §4.2).
- `Corpus.h:71-73` says arithmetic "cannot produce" a subnormal output. Its comment is amended in
  L2 to name the limiter as the one exception. The `subnormal_*` goldens peak at −4.9 and
  −10.5 dBFS and never engage it.

### 5.4 Lifecycle and invariance

| Entry point | Limiter |
|---|---|
| `Init` | computes `release` and `holdFrames`; G = 1, hold = 0; counts zeroed |
| `Reset` (and so `Restart` and an Exact load) | G = 1, hold = 0 (`Engine.cpp:686-690`'s priming). Counts kept, as `Stats()` keeps them |
| Spillover load (event or direct) | carried over |
| `Process` | per frame, inside the guard |

`exact_load_mid` restarts at 7 s while the limiter is active (it engages at 5.031 s and changes
seconds 5–7). Its RestartTail invariance still holds (measured: "invariances ok" in all eight
variants).

---

## 6. Where it sits, and why the plugin gets it too

### 6.1 In Pass 3c, last

```
 … ─► POST CHAIN ─► MIX (dry·gd + wet·g·gw) ─► ±FLT_MAX ─► LIMIT ─► OUT
                     ▲
        dry (never delayed) ┘          feedback tap: Pass 3a, before all of this
```

The change to `Engine.cpp:1246-1260` (prototype, measured):

```cpp
    const float aL = dryL * mix.dry;   // was inline in oL's expression: same operations
    const float aR = dryR * mix.dry;
    float       oL = aL + wl * mix.wet;
    float       oR = aR + wr * mix.wet;
    // ... the ±kMaxFinite saturation, unchanged ...
    limiter_.Frame(aL, aR, oL, oR);   // output-limiter.md §4.3
    outL[n] = oL;
    outR[n] = oR;
```

The rest of the engine change:

- `Engine::Impl` gains `detail::OutputLimiter limiter_` next to the smoothers (`Engine.cpp:422`).
- `Init` calls `limiter_.Init(cfg.sampleRate)` next to the `SetTau` calls (`:642-645`).
- `Reset` calls `limiter_.Prime()` next to the smoothers' priming (`:686-690`).
- `Engine.h:97-112`'s list of what `Restart` and `Reset` do gains "the output limiter's gain
  returns to 1".

### 6.2 Outside the feedback loop

- **The loop is not changed.** The tamed wet is written to the feedback FIFO in Pass 3a
  (`Engine.cpp:1209-1225`), before the post chain and the mix. So the limiter never changes
  regeneration, the tamer's saturation, tails or anything the engine remembers.
- **The goldens can be predicted.** The changed renders could be computed from the revision-7
  output alone, and the harness agreed (§9.1).
- **Unity returns exactly.** Once the gain is back at 1, the output equals the unlimited
  output.

### 6.3 The plugin plays it too

The engine is the one place both targets share. Putting the limiter there has three effects:

1. **The pedal and the plugin agree above full scale, which they do not today (§2.2).** Both
   targets check the same goldens, so a limited render is one sound on both.
2. **The plugin needs no wrapper code.** A limiter in the plugin alone would play a sound the
   pedal cannot. A limiter in the firmware alone would make every limited render a parity
   failure.
3. **What stays outside is chosen.** The wrapper's output level and Curation monitor trim
   (`PluginProcessor.cpp:1098-1107`, ±24 dB) act after the engine, as the pedal's analog output
   level does. A user who raises them can exceed 0 dBFS in the DAW by choice.

`LatencySamples()` stays 0. The plugin's live dry keeps its headroom above 0 dBFS through the
dry-referenced ceiling (companion §4.8).

### 6.4 The pedal's analog side

- **The codec clamp.** With the limiter, LD's `f2s24` clamp touches only the 125 codes between
  0.999985 and 1.0 (§2.2). On the changed goldens that is 1–89 frames per render, and 1,701 on
  `hot_out`.
- **Inter-sample peaks.** A sample-peak ceiling does not bound the peaks between samples at the
  DAC. The PCM3060's datasheet says nothing about headroom in its 8× interpolation filter
  (reported). Benchmark Media reports that every D/A chip it tested clips inter-sample overs,
  which reach 3.01 dB in theory and 1.5–2 dB in practice.
- **Hardware requirement.** The pedal's output stage must swing the DAC's full scale plus about
  3 dB. Today the Seed is passive after the codec.
- **Full-scale level.** The Rev7's 0 dBFS output level needs measuring. TI's formula gives
  0.8 × 4.5 V = 3.6 Vp-p, about 1.27 Vrms. Electrosmith's datasheet says 1 Vrms (reported).
- These belong to the pedal's output-stage design (`hardware/` is empty) and to lane L6 (D10).

---

## 7. Control and display

### 7.1 No user control

The limiter is always on. It has no parameter row, no device setting and no per-preset switch.

- **Nothing to switch off for.** It changes nothing below full scale. Turned off, the pedal's
  codec would clip and the plugin would emit overs: the two would stop matching.
- **It follows practice.** Protective limiters are always on (§3.1).
- **A per-preset switch is worst.** It would change every package's `sound_hash` and need the
  package-change label (clock §11.3's precedent). It would also let a preset opt out of safety.
- **If an off switch is ever wanted**, it is a Global row, a device setting like
  `global.effect_volume_db`. That needs:
  - row 87, since rows 83–86 are claimed by clock §10.4;
  - a seventh `ParamDomain` bit, which touches `kAllParamDomains`, the static assert at
    `Engine.cpp:124` and `RebuildDirty`'s loop and switch (`:1105-1125`);
  - plugin registration, a `StateCodec` key and the firmware's `set` allowlist;
  - a place in render recipes and PARITY requests (compiler §3.8).

  It would be its own revision. D5 recommends against it.

### 7.2 The engine API (not sound)

```cpp
// Engine.h, beside GrainStats
struct OutputStats {
  uint64_t limitedFrames  = 0;  // frames output at a gain below 1
  uint64_t limiterEngages = 0;  // frames at which the gain left 1
};
// Audio thread only (plain 64-bit counts, as Stats). Counts since Init, which Reset,
// Restart and loads keep, so a caller reads the difference over a render.
OutputStats OutStats() const noexcept;
// Audio thread only: the lowest limiter gain since the last call (1.0f if none), then 1.
float ConsumeLimiterMinGain() noexcept;
```

With the hard knee, the lowest gain is fl(1/p) at the largest over, so −20·log₁₀ of it is the
"would-be peak" in dBFS. The audition reports that figure (§9.5).

### 7.3 The plugin

- **The lamp and the readout.** Beside the output meter (`plugin/src/PluginEditor.cpp:267-268`):
  - a **LIM** lamp, lit while `limitedFrames` has advanced in the last 250 ms;
  - the gain reduction in dB, the lowest gain over the last UI interval, held for 1 s.
- **The audio side.** `RenderChunk` reads `OutStats()` and `ConsumeLimiterMinGain()` after
  `Process` and raises an atomic, as `outPeak_` does (`PluginProcessor.cpp:1107`).
- **The Curation view.** Its one-click render report shows each render's limited frames and
  would-be peak.
- The meter is measured at the engine's output, which is the last digital stage of the pedal.
  This avoids the H90's fault, a clip LED placed before the last gain stage (§3.1).

### 7.4 The pedal

- **The console.** `stats` (`firmware/live/main.cpp:333-353`) gains three fields:
  - `limitedFrames`;
  - `limiterEngages`;
  - `limiterMinGain`, as float bits (`Hex`), which the host tools decode, read through
    `ConsumeLimiterMinGain`.
- **A panel indication.** That belongs to the control-surface design, which owns the LEDs. The
  Rev7 breadboard's one user LED already pulses for onsets (`main.cpp:116-120`).

---

## 8. CPU and code placement

### 8.1 Cost (model)

These figures come from the prototype's Pass 3c built for the M7 (`-O3 -mcpu=cortex-m7
-mfpu=fpv5-d16 -mfloat-abi=hard -ffp-contract=off`, profile §3.2) and the Seed's measured
latencies: `VMUL`/`VADD` 3 cycles and `VDIV` 18 (`bench-all.md:56-68`).

| Path | Work per frame | Cycles per frame | Per 48-frame block |
|---|---|---|---|
| Idle (G = 1, no over) | two sign clears, a compare and select, two compares with their `VMRS`, a branch: about 12 instructions | about 15–25 | 0.7–1.2k (0.15–0.25 %) |
| Limiting, on the pedal | idle plus the active path's 40–60 instructions, with one `VDIV` and 8–10 `VMRS` | about 70–110 | ≤ 5.3k (≤ 1.1 %) |
| Limiting, plugin with a hot dry | plus a second division | about 90–130 | host only |

The worst block is one where every frame limits, for example `hot_out`. These figures are
estimates until bench session 2 times them (§8.5).

### 8.2 How the governor charges it

The limiter's cost depends on the signal, not on parameters. The governor's per-frame refund
P(f) is a function of parameter targets, which keeps it split-invariant (budget §5.1). The
limiter's activity in a frame is known only in Pass 3c, after the grains of that block have
rendered in Pass 2, so the governor cannot condition admission on it.

The limiter's worst case therefore joins the **static reserve S** (budget §5.2–§5.3):

- **Placeholder:** S grows by 5.3k cycles per 48 frames, from about 148,000 to about 153,300.
- **Effect on R:** by budget §5.3's formula, R = (408,000 − S − Cmax)/48 − 1,270, R falls by
  about 110 cycles per frame.
- **Voices:** that is about 0.8–0.9 of a voice at the Hermite weights 119–145, or 2 voices at
  the unity weight 54. The loss applies only at corners where the governor binds (calculated).
- **The final figure:** session 2 measures the limiter's worst block and S takes it plus the
  margin of budget §8.3, before D8 freezes the constants.
- **The shared model:** `CostModel.h` and the plugin's "Seed load" meter (budget §7.4) include
  the same fixed term.

### 8.3 Code placement: ITCM

**Where the code goes.** It is header-only and inlined into `Engine.cpp`'s `RenderFrames`.
`Engine.cpp` is placed whole in ITCM: the ITCM input pattern is the whole `libbrainscape_dsp.a`
apart from the decoder, encoder, SHA-256 and test-signal objects (`firmware/CMakeLists.txt:198`,
`:217`). So the code lands in ITCM with no linker change.

- It must stay there. It runs every frame, and from QSPI the engine runs 1.05–1.21× slower warm
  and 1.26–1.70× cold (budget §1).
- It uses scalar constants only. An `inline constexpr` table would be a COMDAT section, which
  `cmake/ItcmCheck.cmake` rejects unless it is listed in `_bs_engine_comdat`
  (`firmware/CMakeLists.txt:211-216`).

**What it costs (measured).** The prototype's live image was built against `main`, with the same
compiler and libDaisy:

| | `.itcm_text` | Spare, counting the 64-byte offset |
|---|---|---|
| `main` (revision 7, with steps 1–2) | 62,264 bytes | 3,208 |
| With the limiter | 63,128 bytes (+864: `Engine.cpp`'s `.text` +868) | 2,344 |

That figure includes the statistics and the plugin's dry-referenced path.

**If bytes are needed later:**
- moving the active path out of line saves nothing (measured in the mock: the call adds more
  than it removes);
- dropping the plugin path's code saves about 120 bytes;
- 32-bit counters save about 30 bytes.

**Effect on the ITCM gates:**
- The limiter fits today without moving cold code.
- It adds 0.86 KB to every tally against budget G6's 8 KB spare and CLOCK's entry gate for T1
  and T2 (clock §9.6).
- The cold code that budget §7.3 plans to move rises from about 11.0 KB to about 11.9 KB. That is
  still inside the roughly 16 KB the audio callback never reaches.
- `Init`'s share, an `Exp2D` call and a rounding of about 40 bytes, leaves ITCM with the rest of
  the main-thread API when the engine is placed by function.

### 8.4 DTCM

`Engine::Impl` grows by 40 bytes (§4.2). There is no new arena and no buffer, because there is no
look-ahead. The live image's DTCM use is unchanged at 35,840 bytes (measured).

### 8.5 Bench session 2

The additions to budget §8:

- **Images:** every image of budget §8.1 at revision 8 or later carries the limiter.
- **`SuiteMicro`:** the limiter's idle frame, its limiting frame and a fully limited block.
- **Non-grain worst block:** measured with `hot_out` playing, so S includes it.
- **G1:** the image's renders equal the host's and qemu's for the eight changed presets.
- **The Rev7's analog side (D10):**
  - the 0 dBFS output level, from a 1 kHz sine at 1.0, read on a scope or an audio interface;
  - inter-sample behaviour: a 12 kHz sine (fs/4) at 45° phase with sample peaks at 1.0, whose
    true peak is +3.01 dB, captured to see whether the PCM3060 clips it.

---

## 9. Golden, test and audition impact

### 9.1 Goldens that change

With the limiter patched into `main`, the golden harness reports exactly the eight renders of
§2.3 as changed. Each first differs in the second the WAV analysis predicted (measured):

| Preset | First differing second |
|---|---|
| `strum_marks` | 3 |
| `post_max` | 8 |
| `hot_out` | 0 |
| `automation_offgrid` | 3 |
| `spillover_chain` | 5 |
| `exact_load_mid` | 5 |
| `lone_changes` | 11 |
| `midi_gate` | 3 |

The other 37 keep their hashes. Every existing counter of all 45 presets is unchanged, the eight
included (measured). Per-second hashes change only in the seconds where the gain is below 1:

| Preset | Seconds changed |
|---|---|
| `strum_marks` | 3 |
| `automation_offgrid` | 3, 5–8, 10 |
| `lone_changes` | 11 |
| `exact_load_mid` | 5–7 |
| `spillover_chain` | 5 |
| `midi_gate` | 3, 6 |
| `hot_out` | 0–5 |
| `post_max` | 8 |

This is the review check for the re-mint: **any other hash that moves is a bug.**

Three of the eight (`automation_offgrid`, `spillover_chain`, `exact_load_mid`) also change under
the governor (budget §6). Whichever lands second re-mints them on top of the first.

### 9.2 New counters (`dsp/tests/golden/Corpus.h`)

| Counter | Meaning | Lands in |
|---|---|---|
| `OutOverFrames` | output frames with \|out\| > 1 on either channel | L1, at revision 7: corpus version 13, sound-neutral. Shows today's clipping |
| `LimitedFrames` | `OutStats().limitedFrames` over the render | L2, revision 8: corpus 14 |
| `LimiterEngages` | `OutStats().limiterEngages` over the render | L2 |
| `LimiterMinGainBits` | the bits of `ConsumeLimiterMinGain()` over the render, `0x3F800000` when none: an exact integer on every leg | L2 |

From revision 8 the harness requires `OutOverFrames` = 0 on every preset of every vector. All
golden inputs are on the codec's grid, so their dry is under 1.0.

`hot_out` gains `require LimitedFrames ≥ 200,000` (`Corpus.cpp:944-945`).

New counters and corpus versions change no committed hash (`tools/ci/sound_rev_gate.py`'s
docstring).

### 9.3 New golden presets (corpus 13, L1)

Each preset is minted at revision 7, where it clips and `OutOverFrames` shows it, and re-minted
at revision 8. The parameters are starting points: L1 tunes them to the stated overs.

| Preset | Vector | Settings | What it pins |
|---|---|---|---|
| `limit_attack` | Plucks, 12 s | Mix 0.5, `wet_trim_db` +6, feedback 0.4 | repeated sudden overs 3–6 dB over; attack, hold, full release between plucks; at least 10 engagements; Plucks' alternating pan exercises the linking |
| `limit_sustain` | SoftNotes, 12 s | Mix 0.5, post delay mix 1, feedback 0.9, time 400 ms | the owner's case: coherent build-up to about +2.5 dBFS held for 2 s or more; the hold restart; release inside the silent tail (`LimitedFrames` ≥ 96,000; the last limited frame before the tail's last second) |
| `limit_hot_mix0` | Saturation, 6 s | Mix 0, `wet_trim_db` +24, feedback 0.6 | a dry at full scale with the wet at its loudest: `LimitedFrames` = 0, the pedal's dry never engages it |
| `limit_hot_mix25` | Saturation, 6 s | Mix 0.25, `wet_trim_db` +12 | limiting with the dry at full scale: the dry dips |

Each preset carries `Invariance::HostileFpEnv`, and `limit_sustain` also block-split coverage at
odd sizes. Every leg's block patterns, random sizes, fresh engines and split delivery cover all
four, and so does the M7 under qemu, which the prototype did not run (§12).

### 9.4 Unit tests

**`dsp/tests/test_limiter.cpp` (new).** It tests `detail::OutputLimiter` directly:

1. **Identity.** At G = 1: 2²⁰ magnitudes at or under 1.0, plus ±0, subnormals, 1.0 and
   `nextbelow(1.0)`, both signs, keep their bits and the state. The exhaustive sweep of all
   1,065,353,217 runs under the nightly label.
2. **The one-step bound.** fl(p·fl(1/p)) ≤ 1 over every mantissa of (1, 2), which is 8,388,607
   floats. A case at 2¹²⁶ or above shows the clamp holding the output at 1.0.
3. **Constants.** At 48 kHz, `release` is `0x3F800325` and `holdFrames` is 480.
4. **Timing.** One over of 2.5 dB gives exactly 1.0f after 3,481 frames. The other rows of §4.4
   item 3 are checked the same way.
5. **No stuck hold.** From every gain on a 0.05 dB grid down to −60 dB, with no input, G = 1
   within H + ⌈GR/(40 dB/s)⌉ + 2 frames.
6. **Liveness.** From the floor, the gain rises strictly every frame.
7. **Linking.** One channel over, the other at −20 dBFS: both are scaled by the same G.
8. **The dry-referenced ceiling.**
   - With no wet term, 10⁶ hostile frames are unchanged.
   - With a hot dry plus a wet, the output never exceeds max(1, |a|).
9. **Ceiling.** Random frames up to ±`FLT_MAX` never exceed max(1, |a|) and are always finite.

**Engine tests.**

- **Contract #2** (`test_engine.cpp:320`), **the Mix-law tests** (`test_modes.cpp:469`, `:506`)
  and **the trim test** (`:356`) pass unchanged (measured). The Mix-law test gains `CHECK(OutStats
  ().limitedFrames == 0)`, which makes its precondition explicit.
- **`post-delay time automation is click-free`** (`test_engine.cpp:1070`) is the one test that
  fails (measured). Its feedback-0.9 renders reach 1.11–1.45 (+0.9 to +3.2 dBFS), so the limiter
  acts there and the click detector sees its bend (§3.4). At half the input level, 0.25 for the
  noise, every render of the test stays at or under 0.75. The test then measures the delay glide
  again, and the whole suite passes with the limiter: 201 of 201 test cases (measured). L1 makes
  that change, at revision 7, as a test-only commit.
- **The block-split test** gains a limiting render: `hot_out`'s settings at blocks of
  {1, 7, 32, 48, 64, 127, 512}.

### 9.5 The audition pre-screen

Once the limiter exists, no render on a pedal-faithful input can exceed 0 dBFS. So the peak alone
judges nothing, and the checks move to the engine's count.

| Check (`tools/audition/README.md`'s table) | Today | From revision 8 |
|---|---|---|
| Peak (stored), class inputs | ≤ −1 dBFS | **Unchanged.** A factory preset must not lean on the limiter at its stored positions, and the 1 dB margin also keeps the soft-knee alternative off them |
| Peak (moved), sweeps and S11 on the class input | ≤ 0 dBFS, unrounded (`Suite.cpp:29`, `:519`, `:677-680`) | **Limiter (moved): zero limited frames** during sweeps and S11 on the class input. This is the same criterion, read from `OutStats()`. A failure names the render, its limited frames and its would-be peak |
| Peak (other) | the other inputs' stored peaks, and the highest of S0's other vectors, S7–S10 and the Clicks renders: reported | Reported with the limiter's frames and would-be peak per render. These are the attack modes' SoftNotes corners at up to +2.5 dBFS, all S0 Saturation renders, Lull's S10 and Runaway's S8 |
| Over full scale (new) | — | **No sample over full scale in any render** (`Metrics.overFull` = 0, `Metrics.cpp:251`). This is a guard on the limiter itself |
| Load | births per second | unchanged |

`Render.cpp` reads `OutStats()` deltas and `ConsumeLimiterMinGain()` around each render, as it
reads `Stats()` (`tools/audition/src/Render.cpp:87`, `:137`, `:142`). The would-be peak is
−20·log₁₀ of the lowest gain.

At revision 8, L3 re-runs the pre-screen. These renders' hashes change:

- every S0 Saturation render: 18 modes;
- the attack modes' SoftNotes sweeps and corners that passed 0 dBFS;
- Lull's S10 and Runaway's S8;
- any other render with an over.

Every render on a class input judged by Peak (moved) keeps its hash, because it was already at or
under 0 dBFS. L3 records the count and the per-mode list with `ratings.py note`.

### 9.6 Ratings and carry-forward

A rating row carries forward only when all of its S0–S11 render hashes held. Any other row is
marked "re-listen", with each changed render and its first differing second
(`tools/audition/ratings.py:21-27`, `:227-281`).

Every mode's S0 Saturation render changes. So **every rated row would be marked "re-listen"** if
ratings were written before revision 8. No row exists yet: the owner's knob ratings are pending
(`docs/STATUS.md:176-180`). This is the main reason for D7's order.

If rows exist by then, L3 annotates each re-listen entry with the render's limited frames and
would-be peak, so the owner hears only the seconds that changed.

---

## 10. Sound revision and order

The limiter is one commit that raises `kSoundRevision` by one and mints `golden.json` at it
(profile §5.12, the per-commit rule). It has no Leaf row and changes no package byte, so no
`sound_hash` changes and it needs no package-change label.

It is independent of the other two planned revisions:

| | Cost governor (budget §5) | CLOCK tempo core (clock §11.3) | Output limiter |
|---|---|---|---|
| Where | `GranularCore`: admission, fades | events 6–10, `TempoCore`, CLOCK births | Pass 3c, after the mix |
| Touches the loop or the draws | yes | yes | no |
| ITCM | about 2.3 KB; waits for cold code to leave | 1.0–1.6 KiB; waits for the 8 KiB gate | 0.86 KB; fits today |
| Changes goldens | 5 of 33 (budget §6) | new presets only (tempo off elsewhere) | 8 of 45 |

**Recommended order (D7).** The limiter is **revision 8, first**. The governor and the tempo core
become 9 and 10, in landing order. The reasons:

1. It is the smallest change, and the only one that fits ITCM now.
2. Its cost enters S before D8 freezes the governor's constants (budget §9).
3. The knob ratings, the listening pass and the governor's D11 factory gate then all run on the
   limited sound. No rating has to be re-heard for it (§9.6).
4. It changes nothing CLOCK or the governor depend on. CLOCK's 48-frame cap still belongs to
   whichever of those two lands second (clock §11.3).

Adopting this order makes some text in the other designs stale, listed in §11.6. The
"revision 8, or 9" wording becomes "9, or 10" when the owner confirms D7.

---

## 11. Plan and decisions

### 11.1 Lanes, order and estimates

| Lane | Work | Files | Starts | Days (*estimated*) |
|---|---|---|---|---|
| **L0** design | this document and its notes | `docs/` | done | — |
| **L1** counters first | `OutOverFrames`; corpus 13 with §9.3's presets minted at revision 7; the click test's input halved | `dsp/tests/` | now: tests only, no sound change | 1 |
| **L2** the limiter, revision 8 | `detail/OutputLimiter.h`; `Engine` Impl, `Init`, `Reset`, Pass 3c; `OutStats`, `ConsumeLimiterMinGain`; corpus 14's counters and requirements; `test_limiter.cpp` and §9.4's amendments; the `Corpus.h` and `MixLaw.h` comment notes; `SoundRevision.h`'s history line; the re-mint; parity on every host leg and the M7. Before minting, an A/B of both knees for the owner (D2): Echolalia's S11 corner a1r0s1t0 on SoftNotes, `limit_sustain` and `hot_out` | `dsp/` | after L1; before the knob-rating rows (D7) | 2–3 |
| **L3** audition | §9.5's checks; the would-be peak; README; the revision-8 pre-screen and its `ratings.py note`; re-listen annotation (§9.6) | `tools/audition/`, `firmware/factory/AUDITION.md` (through `ratings.py`) | after L2 | 1–2 |
| **L4** plugin | the LIM lamp and gain-reduction readout; the Curation report's counts | `plugin/` | after L2 | 1 |
| **L5** firmware | the console's `stats` fields; `firmware/README.md`'s ITCM table | `firmware/` | after L2 | 0.5 |
| **L6** bench | §8.5 within session 2: the limiter in `SuiteMicro` and S; the Rev7's 0 dBFS level and the DAC's inter-sample test | `firmware/`, records | with session 2 | 0.5–1 |

**Order:** L1 now. Then L2, before the owner writes knob ratings if possible. Then L3, L4 and L5
in parallel. L6 runs with bench session 2.

**Total:** about 6–8.5 engineer-days beyond this design (*estimated*).

### 11.2 Owner decisions

Each answer is the recommendation, for the owner to confirm or change. As with every owner answer
in these designs, each stays reversible before the first public release. Reversing one is an
owner decision of its own and, where it changes the sound, a sound revision.

| # | Decision | Recommended answer | Consequence | § |
|---|---|---|---|---|
| D1 | Where it lives | **In the engine, for the pedal and the plugin alike**, last in Pass 3c | Both targets play the same limited samples; the plugin needs no wrapper code | 6 |
| D2 | Threshold, ceiling and knee | **A hard knee at full scale (T = C = 1.0)**, confirmed by ear on L2's A/B. The alternative is a quadratic soft knee from −1 dBFS to 1.0 | Only renders that clip today change (8 of 45 goldens); contract #2 stays as written. The soft knee bends sudden low overs less, but changes 10 goldens and every render between −1 and 0 dBFS, and restates contract #2 | 3.4, 4.6 |
| D3 | Time behaviour | **Zero latency, instant attack, a 10 ms hold restarted under demand within 0.25 dB, a 40 dB/s release to exactly 1** | No dry delay; no ripple from 82 Hz up; 72.5 ms back to unity after a 2.5 dB over. Each constant can be tuned by ear before the first public revision, each change a revision | 4.1, 4.4 |
| D4 | What it scales | **The whole mix, linked across channels, with each channel's ceiling at max(1, \|dry term\|)** | On the pedal a hot input's dry dips with the wet while limiting. The plugin's Mix 0 stays the input on any input | 3.3 |
| D5 | Control | **None: always on**, no row, no device setting, no per-preset switch | No package changes; nothing to forget on stage. Turning it off would bring back codec clipping and a pedal–plugin difference | 7.1 |
| D6 | Indication | **Plugin: a LIM lamp and gain reduction at the output meter. Pedal: console counts now, a panel indication in the control-surface design** | Engagement is visible where it happens: at the last digital stage | 7.3, 7.4 |
| D7 | Revision and order | **Sound revision 8, first**, before the governor and the tempo core (9 and 10 in landing order), and before the knob-rating rows are written | Ratings and the factory gate run on the limited sound; no re-listen; the governor's S includes it before D8's freeze | 10, 9.6 |
| D8 | The audition | **Keep Peak at stored positions (≤ −1 dBFS). Replace Peak (moved) with zero limiter engagement on the class input during sweeps and S11. Report engagement elsewhere. Add "no sample over full scale"** | Factory presets never lean on the limiter where the class input is judged; the +2.5 dBFS SoftNotes corners become reported limiting, not clipping | 9.5 |
| D9 | CPU charge | **The worst limited block (model 5.3k cycles) in the governor's static reserve S**, replaced by session 2's measurement | About 110 cycles per frame less for grains at binding corners: about 1 voice at Hermite weights | 8.2 |
| D10 | The analog side | **Measure the Rev7's 0 dBFS level and the PCM3060's inter-sample behaviour in session 2. Require the output stage to swing DAC full scale + 3 dB. Lower the ceiling only if the DAC is shown to clip** | No ceiling below 1.0, and no contract change, without evidence. A later revision can lower C if needed | 6.4, 8.5 |

### 11.3 Design decisions taken

| Decision | Choice | § |
|---|---|---|
| 1. Gain computer | Linear domain: r = c/\|s\|, one division; no log or exp per sample | 4.3 |
| 2. Release law | Multiplicative k per frame, a constant number of dB per second, capped at r ≤ 1; k from `Exp2D` at `Init` | 4.1, 5.2 |
| 3. Hold restart | Only under demand (r < 1); the variant without that condition never released | 4.4 |
| 4. Floor and clamp | 2⁻²⁴, so the gain stays normal and rises. The final clamp, with one-sided compares, runs on every limited frame | 4.4 |
| 5. State and flush | A gain in [2⁻²⁴, 1] and an integer hold; no `FlushTiny` site | 5.3 |
| 6. Placement | After the ±`FLT_MAX` saturation and before the store; outside the loop | 6.1 |
| 7. Lifecycle | Primed by `Reset` (so `Restart` and Exact loads); kept by Spillover | 5.4 |
| 8. Code | Header-only `detail/OutputLimiter.h`, inlined into `RenderFrames`, in ITCM; scalar constants only | 8.3 |
| 9. Statistics | `OutputStats` and `ConsumeLimiterMinGain`, outside the sound; exact integer counters in the corpus | 7.2, 9.2 |

### 11.4 Risks

1. **The bend at new maxima.** A sudden, large, low-frequency over bends its first rising edge.
   It is inherent to zero latency. The soft knee reduces it by 17 dB on a sudden +2.5 dB burst at
   110 Hz, but only from 5.84 to 4.19 times its neighbourhood on the click test's build-up. Only
   look-ahead removes it.
   Mitigation: D2's A/B before minting, and the soft knee kept as a constant-level alternative.
2. **Ducking on hot inputs.** With a full-scale input the whole mix, dry included, dips by up to
   about 11 dB while the wet sounds.
   Mitigation: input staging in the hardware design (§1.3). The Saturation renders show the
   amount (§9.5).
3. **Inter-sample overs at the DAC** could still clip in the PCM3060's filter.
   Mitigation: D10's measurement, and a lower ceiling as a later revision if needed.
4. **CPU.** About 1 % of the block at worst, in S.
   Mitigation: session 2 measures it before the constants freeze.
5. **ITCM.** 0.86 KB of the 3.2 KB spare, which raises the cold-code move every later lane
   already needs.
   Mitigation: budget §7.3's placement by function, with room left (§8.3).
6. **Leaning on the limiter.** A recipe tuned until its renders "just stop clipping" would rely
   on the limiter.
   Mitigation: D8 keeps the class-input criteria, and `bspc` could lint a stored position whose
   render limits (a candidate L-lint, beside L10).

### 11.5 Open questions

1. By ear: the knee (D2), and whether 40 dB/s and 10 ms suit sustained pads and feedback swells.
2. The Rev7's 0 dBFS level and the PCM3060's inter-sample behaviour (D10).
3. Whether the pedal's panel shows engagement, and how: the control-surface design.
4. Whether a later true-peak option is wanted for the plugin's bounces. It would be a sound
   change for both targets, never for the plugin alone.

### 11.6 Amendments made with this design

These notes are dated 2026-10-09 and written in each document's style:

- **mode-compiler.md** §11.3, in the as-built note on the limiter question;
- **docs/STATUS.md**: step 4's owner's answers, the Mix law's open ends and the plan's item 4;
- **determinism-profile.md** §3.7, after its list: the final mix's saturation and the live
  dry's headroom;
- **companion-app.md** §4.8, on the live path's headroom;
- **docs/README.md**: the index row.

The following become stale only when the owner confirms D7, and are amended then:

- cpu-budget.md's "revision 8, or 9 if CLOCK's tempo core lands first": §5.1's note, §7.1,
  §8.1's `bench-r8-gov`, §9's D2 and D8;
- clock.md §9.6 and §11.3's "8 if it precedes cpu-budget.md's governor";
- docs/STATUS.md's CPU-budget lines.

L2 amends the code comments that state today's rules. These are `Corpus.h:71-73` (no subnormal
output from arithmetic), `MixLaw.h`'s header (the dry at unity up to the middle, now "except while
the output limiter engages"), `Engine.h:97-112`'s `Reset` and `Restart` lists, and
`Engine.cpp:1252-1254`'s finite-output comment.

---

## 12. Evidence

**The prototype of this document (measured).** It is not committed.

- **The code.**
  - The limiter of §4.3 as a header, patched into a scratch checkout of `main` `011b294`: one
    member, one line each in `Init` and `Reset`, and the Pass 3c change of §6.1.
  - A standalone copy with an evidence program.
- **Builds.** MSVC 17 (`/fp:precise`) and arm-none-eabi-gcc 10.3.1 (the firmware's flags).
- **Results.**
  - The golden harness at `--mode check`: 37 of 45 hashes held, and the 8 of §9.1 differed from
    the seconds given there.
  - The 45 hashes were identical under `--block 1`, `--block 512`, `--pattern 48,1,127,32`,
    `--random-blocks 7`, `--fp-env hostile`, `--fresh-engine` and `--delivery split`.
  - The unit suite: 200 of 201 test cases passed, and 201 of 201 with the click test's input
    halved.
  - The live image's map: §8.3.
- **The evidence program.** It ran §4.4's exhaustive and timing checks, §3.3's dry-referenced
  checks, §4.6's sine measurements and §2.3's table from the 45 revision-7 golden renders, dumped
  as float WAV.
- **Mock code sizes.** A Pass 3c mock built for the M7 measured 360 bytes without the limiter and
  1,012 bytes with it inline. The out-of-line variant measured 500 plus 548 bytes.
- **Not run:** the M7 under qemu, which is not installed on this machine (L2's `parity-m7` leg
  runs it); cycle counts on the Seed (L6).
- The soft-knee comparison of §3.4 used the same checkout with the quadratic knee of §3.2 C.

**The DSP lane (host-run).** It prototyped A–F of §3.2 in a scratch harness against the in-tree
`DetMath`. It measured:

- the sine-burst residuals and aliasing (§3.2, §4.6);
- the hold and refresh ripple at 41–220 Hz;
- the HF splatter index on the goldens (§3.4);
- the hot-dry case (§3.3);
- M7 code sizes for the standalone variants.

It also found that the in-tree `SoftSat` returns 1 + 2⁻²³ for 10,220 inputs in [2.983, 3], which
is harmless in the tamer but rules out using it for a ceiling without a clamp. Its scheme to
restart the hold within 0.25 dB is the one §4.4 item 5 corrects.

**The constraints lane.** It mapped the tree's contracts, tests, integration points and ITCM
rules (cited throughout), and computed the golden peaks at revision 7 with thresholds from
0 to −3 dBFS:

| Threshold | Renders changed |
|---|---|
| 0 dBFS | 8 |
| −1 dBFS | 10 |
| −2 dBFS | 14 |
| −3 dBFS | 18 |

**The practice lane (reported).** Sources:

- Strymon's BigSky MX firmware notes, https://strymon.net/faq/bigsky-mx-firmware-revision-release-notes,
  and its plug-in FAQ;
- Mutable Instruments' code: `clouds/dsp/granular_processor.cc`, `rings/dsp/limiter.h` and
  `stmlib/dsp/dsp.h` at https://github.com/pichenettes/eurorack and
  https://github.com/pichenettes/stmlib;
- DaisySP's `Source/Dynamics/limiter.cpp`;
- the Fractal Audio forum's thread on the output clipping light;
- Eventide's forum posts on H90 clipping;
- the EHX 45000 manual;
- Airwindows' ClipOnly2, https://airwindows.com/cliponly2;
- Ableton's Live audio effect reference;
- Benchmark Media's note on inter-sample overs;
- TI's PCM3060 datasheet (SLAS533B), https://www.ti.com/lit/ds/symlink/pcm3060.pdf;
- Electrosmith's Daisy Seed datasheet v1.2.0;
- the libDaisy v9.0.0 sources cited in §2.2;
- Giannoulis, Massberg and Reiss, *Digital Dynamic Range Compressor Design*, JAES 60(6), 2012,
  for §3.2 D;
- ITU-R BS.1770-4, Annex 2, for true peak.

Unverified, by the lane's own account:

- whether the BigSky MX's limiter can be turned off;
- the Microcosm's internal limiting;
- the Fractal behaviour, which rests on a forum only;
- the PCM3060's interpolation headroom;
- the op-amp swing figures.
