# Brainscape Grain Engine — Design

> Design document for the core of `dsp/`: the granular voice engine, its scheduler, and the
> modes-as-data model that expresses Microcosm-class behavior and beyond.
> Synthesized from three independent architecture proposals over the research corpus in
> [docs/research/](../research/), then revised against a three-lens adversarial review
> (DSP arithmetic, real-time/platform safety, product coverage) that produced 60 findings —
> the substantive ones are incorporated below. Every load-bearing number is traceable to a
> corpus document; figures marked *derived* are cycle-count estimates, not measurements.
> Status: **reviewed draft, v2** — pending prototype validation on hardware.

---

## 1. The design bet

**One voice engine + one scheduler + a small modifier vocabulary, with modes as data.**

[microcosm.md](../research/microcosm.md) §4.5 establishes (as flagged inference) that the
Microcosm's 11 effects × 4 variations are one engine differing only in scheduling and
per-voice modifiers. We build exactly that engine once, and ship the 44-destination
experience — and everything the Microcosm structurally cannot do — as JSON mode files.

Consequences that drive everything below:

- **Modes are files, not firmware builds.** User-loadable, git-diffable, shared between pedal
  and plugin. The mode schema is the project's real ABI (microcosm.md §13.2).
- **The macro layer is the product.** Activity/Repeats/Shape as per-mode multi-parameter
  macros are why the Microcosm feels good (microcosm.md §13.1, rec #2). We keep that layer
  exactly, as data, and put the depth in the editor/plugin.
- **Novel capability is new field values, not new code.** Semitone/scale-quantized pitch,
  per-grain reverse, spray, glissando, intermittency, and "Mosaic pitch + Seq scheduler"
  (the #2 community request) all land inside the same vocabulary.
- **The engine internals are deliberately conservative** — Clouds' architecture (MIT,
  vendorable) wherever it is right — so the risk budget is spent on the scheduler vocabulary
  and the contracts, which are the genuinely new work.

The central risk, named up front: a data-driven scheduler is harder to tune by ear than 11
hand-written algorithms. The 44 factory mode files are therefore a first-class deliverable
with as much effort as the DSP (microcosm.md rec #3), and the acceptance bar is the corpus's:
*a stranger plugs in, turns to any preset, plays one chord, and it sounds finished.*

**Scope.** This document specifies the grain engine, scheduler, mode/parameter model, memory
and CPU plan, and the `dsp/` API. Specified elsewhere (deliberately, not dropped): the
**looper subsystem** (transport, overdub/undo, 4-way routing — gets its own design doc; its
memory and CPU are budgeted here), **footswitch assignment and bypass topology**,
**USB/firmware update flow**, and the **hardware control surface** (see
[pedal-control-surface-and-io-hardware.md](../research/pedal-control-surface-and-io-hardware.md)).

## 2. Signal path

```
                       ┌──── dry (single-sample path, never block-delayed) ─────────────┐
                       │                                                                ▼
 IN ─► inTrim ─► DCblk ─┴─► [+] ─► HISTORY RING ──► GRAIN ENGINE ──► [norm] ─┬─► POST ─► MIX ─► OUT
                             ▲      SDRAM, int16     64 POD voices           │   CHAIN
                             │      interleaved LR   render per-voice        │   MOD → DELAY → REVERB → FILTER
                             │      2^22 frames      over whole block        │   (ordered, per-stage
                             │      = 87.4 s         into DTCM accum         │    bypassable list)
                             │      writeHead ── pinnable ► FREEZE           │
                             │                                               │
                             └── FEEDBACK TAMING ◄───────────────────────────┘
                                 DCblk → HP 80–120 Hz → LP 4–8 kHz (fb-dependent)
                                 → soft saturation → 4-stage allpass diffuser

 side paths:
   ONSET DETECTOR (spectral flux + adaptive whitening, control rate) ─► mark ring ─► scheduler
   LOOPER (separate SDRAM buffers A+B, 4-way record/playback tap matrix — own design doc)
   HOLD SAMPLER = the freeze pin — no buffer of its own
```

Structural commitments, each corpus-grounded:

1. **Resolve-once-at-schedule-time.** A voice never re-reads a global parameter after birth —
   the one rule Clouds, SuperCollider, and EmissionControl2 all state as contract
   (grain-delay-theory.md §2.1, §4.6). All mode interpretation happens in `ScheduleGrain()`
   at trigger rate. This also makes macro sweeps glitch-free by construction: in-flight
   grains are untouched; new grains arrive with new values.
2. **Render per-grain over the whole block into a DTCM float accumulator**, never per-sample
   over all grains (grain-delay-theory.md §5.4 — "the single biggest optimization available").
3. **Feedback is topology (A)**: tamed wet summed into the record buffer (grain-delay-theory.md
   §3.10). Feedback > 1.0 is a designed self-oscillation feature bounded by the saturator.
   The taming chain's order is a stability requirement and is not user-reorderable; the post
   chain is.
4. **Freeze is a pinned write-head reference, not a buffer** (post-fx doc §5.2) — zero extra
   SDRAM, which is why freeze and the phrase looper coexist. **Documented limit:** the ring
   keeps recording during freeze, so a freeze held longer than the ring length (~87 s at
   48 kHz) minus one grain length has its window overwritten by wraparound; behavior at the
   boundary is re-anchor-on-wrap (audible splice), documented, with "stop writing while
   frozen" as an open alternative (§12).
5. **The dry path never enters the block-delayed wet path** (grain-delay-theory.md §3.11), so
   blend cannot comb and the plugin reports 0 latency.
6. **The post chain includes a tempo-syncable stereo delay** — the Microcosm's Space knob
   explicitly bundles "reverberation *and delay*" (microcosm.md §2.5, §3), and that bundling
   is one of the documented reasons it is beloved (§11.4). Default order
   mod → delay → reverb → filter; each stage bypassable with a click-free crossfade ramp.
   The filter keeps the Microcosm's endpoint semantics as *data on the Filter macro*, not
   hardwired: fully CW = stage bypass, fully CCW = wet kill — and the wet-kill meaning is
   defined at the *end of the chain* regardless of the filter stage's position (post-fx doc
   §2.2.5 flags exactly this reorder hazard).

## 3. Grain voice

**Pool.** `kMaxGrains = 64` POD structs in a fixed DTCM array — no allocation, no virtuals,
free-list bitmask. **`kMaxGrains` and the tier fractions are shared build constants across
firmware and plugin** — they are not `EngineConfig` fields, because the density law and the
mode files are authored against them and a differing value silently changes every texture
(review finding; see contract #6).

**Voice struct.** Fields grouped by function; everything is resolved at birth:

```cpp
struct Grain {
  // read position: masked absolute ring frame + fraction, split 32.32; signed increment
  int32_t  frame;          // masked absolute frame index (mask applied per interpolation tap)
  uint32_t frac;           // 2^32 fractional
  int32_t  inc_i;          // signed integer part — pitch AND direction
  uint32_t inc_f;          // fractional part
  int32_t  frames_left;    // output samples remaining
  int32_t  pre_delay;      // sample-accurate onset within the block (consumed first)
  float    glide_inc;      // per-sample ratio ramp (T0 only)
  // envelope: piecewise attack/flat/decay, legs normalized to unit peak, then LUT-smoothed
  float    env_phase;      // 0..2, advanced by env_inc = 2/width
  float    env_inc;
  float    env_t_a, env_t_d;       // attack end / decay start (from sustain & skew)
  float    env_up, env_dn;         // precomputed leg slopes (1/t_a, 1/(2 − t_d))
  float    smoothness;             // triangle↔LUT crossfade
  float    win_gain;               // 1 / window mean, computed at birth (level compensation)
  // output
  float    gain_l, gain_r;         // equal-power pan × voice gain × layer level
  // per-voice modifiers (tier-gated)
  float    svf_g, svf_k, svf_ic1, svf_ic2;   // resonant SVF coeffs + state (T0/T1)
  uint16_t crush_mask; uint8_t decim_n, decim_i; float decim_hold;   // bit-crush
  uint8_t  tier, flags;            // quality tier; active | reverse | unity bits
};
// target ≈ 120 B; the exact sizeof is a static_assert in the header, not an estimate.
// 64 × 120 B ≈ 7.5 KiB in DTCM.
```

**Phase format: split 32.32 (masked absolute + fraction).** Forced by arithmetic: a 16.16
grain-relative phase caps at 65,536 samples ≈ 1.37 s at 48 kHz, and a 500 ms grain (top of
the recommended size range, grain-delay-theory.md §3.5) at ratio 4.0 traverses 96,000 source
samples — overflow. Bare float32 is ruled out on precision, stated exactly: float ULP is
2^(e−23), so at the top of a 2^22-frame ring the spacing is already 0.5 samples — one bit of
sub-sample resolution — and zero beyond 2^24. Fixed point keeps precision constant
everywhere. The 32-bit integer part addresses the ring directly (2^22 needs 22 bits), so
there is no separate `first_sample` field — Clouds needed the split only because its 16-bit
integer part could not address its buffer. Fallback if the carry cost measures material:
16.16 with the validator bound `size_ms ≤ 65536·1000 / (sr · r_max)` where `r_max` is the
max over the pitch set × detune headroom × glide endpoint — never a silent wrap.

**Windowing.** Piecewise attack/flat/decay envelope with LUT smoothing, three mode-file
parameters:

- `sustain` ∈ [0,1] — flat-top fraction (0 = triangle, →1 = rectangle-with-tapers). This is
  what makes the delay modes sound like a real delay.
- `skew` ∈ [0,1] — attack/decay balance (percussive ↔ symmetric ↔ reverse-swell), partikkel's
  `ka_d_ratio` (grain-delay-theory.md §3.4).
- `smoothness` ∈ [0,1] — crossfades the piecewise-linear shape toward the 4096-entry
  raised-cosine LUT (Clouds' mechanism). Note this axis morphs linear-edges↔Hann-like; the
  boxcar end comes from `sustain`, not `smoothness`.

Both envelope legs are **normalized to unit peak** via precomputed slopes (`env_up`,
`env_dn`) — an unnormalized skewed fold would swing grain level by 12 dB across the Shape
knob's range (review finding). **Level compensation divides by the window's actual mean**
(`win_gain = 1/mean(sustain, skew, smoothness)`, computed at birth from a small closed form
plus a LUT-mean term baked at LUT build time). This replaces Clouds' `1 + 2·shape` formula,
which compensates Clouds' smoothness axis and would be sign-inverted against a flat-top
parameter (review finding).

**Pitch.** `ratio = SemitonesToRatio(st + cents/100)` resolved at birth **via LUT + lerp,
not `powf`** (schedule-time transcendentals are charged in §8). Composable layers: per-mode
weighted interval set (≤ 8 entries, `cycle | random` selection) → optional scale/chord
quantization → per-grain `spread_cents` detune. Output duration is fixed; pitch changes how
much source is consumed (§3.7 mapping (a)). **Glide** is specified as endpoints —
`glide: {st_start, st_end, curve}` — so the interval is stable when `size_ms` changes, and
`curve` is the trajectory shape that Glide's Shape macro drives (the manual's one explicit
per-effect Shape meaning). **Reverse** is a per-grain coin flip on `reverse_prob`; a global
FWD/REV flag covers the Microcosm's Reverse button.

**Write-head guards** — derived per direction, clamped once at schedule time. `d` = scheduled
delay behind the write head, `L` = grain length in output samples, `r` = |rate|, `margin` a
few ms. **Reverse convention: a reverse grain starts at its scheduled position `W₀ − d` and
reads backward** (receding from the write head — this is what makes the near guard trivially
safe for reverse; the "start at region end" convention would instead demand `d ≥ L·r`):

| Direction | Near guard (read must not overtake write) | Far guard (write must not lap the grain) |
|---|---|---|
| Forward | `d ≥ L·max(0, r−1) + margin` | `d ≤ bufLen − L·max(0, 1−r) − margin` |
| Reverse | `d ≥ margin` | `d ≤ bufLen − L·(1+r) − margin` |

Plus a third guard from grain-delay-theory.md §2.1's hazard 3 (feedback re-injection): when
`feedback.amount > 0`, enforce `d ≥ d_min_fb ≈ 5 ms` — without it, a zero-delay mode with
feedback forms a margin-period comb inside the loop. **Policy on violation: clamp `d` upward
per grain at schedule time** (every mode stays loadable; high-ratio grains sit further back
than authored); the validator *warns* with the computed minimum
(`base_ms ≥ size_ms · (r_max − 1)`) so mode authors see it. The acceptance test for feedback
boundedness sweeps `base_ms → 0` explicitly.

**Interpolation, tiered** (quality tiering is *the* CPU lever, grain-delay-theory.md §3.8):

| Tier | Voices | Render |
|---|---|---|
| **T0** | 8 | cubic Hermite, per-grain stereo, glide, per-grain SVF |
| **T1** | 24 | linear, per-grain stereo, per-grain SVF |
| **T2** | 32 | linear, precomputed pan, no per-grain filter |

`Tu` — exact rate 1.0 → **integer copy loop, bit-exact** — is a read-path specialization
*within* any tier, mutually exclusive with glide and the per-grain filter; it is what makes
the MultiDelay family sound like a clean delay. **Tier assignment is mode-aware**: grains
that need glide or a filter allocate from T0/T1 first, and `ValidateMode()` rejects a mode
whose declared concurrency exceeds the tiers its features require (e.g. glide needs ≤ 32
tier-0/1 voices) — otherwise a mode's defining feature would silently vanish past the tier
ceiling (review finding). Render is templated on `<Tier, Stereo, Interp>` and dispatched once
per grain per block.

**Normalization — coherence-aware.** The corpus's `1/√N` law is *conditional* on grains
being decorrelated ("spray/pitch/reverse decorrelate the grains", grain-delay-theory.md
§3.6); at unity rate with zero spray, concurrent grains read the *identical* source sample
and sum coherently — `1/√N` would overshoot +18 dB at N = 64 on exactly the delay modes
(review finding). So: `gain = N^(−p)`, with the exponent `p` resolved at mode publish from a
decorrelation heuristic (p = 1.0 when all ratios = 1, spray = 0, spread = 0, reverse = 0;
ramping to 0.5 as those spread the grains), no `−1`/`>2` guard (plain `N^(−p)` is already
unity at N = 1; the guard created a +3 dB bump at N = 2). The gain is smoothed by a
**per-sample one-pole with a fixed time constant** (τ ≈ 100 ms, `a = 1 − exp(−1/(τ·sr))`) —
never per block, which would make the trajectory depend on host block size and violate
contract #1 (review finding). Plus per-mode `out_trim_db` — the fix for the Microcosm's
gain-staging complaint. Level test: sweep the full N = 1…64 range, < 1 dB variation, with a
dedicated coherent-case variant.

**SDRAM access mitigations** (grain-delay-theory.md §5.4): 16-bit interleaved stereo storage
(one miss per 8 stereo frames at rate 1; **miss rate scales with rate** — see §8); per-grain
-over-block rendering; `PLD` prefetch one line **in the direction of travel** (sign of
`inc_i` — a reverse grain prefetching "ahead" fetches the line it just left). A T0 DTCM
staging ring is *optional*, enabled if measurement demands, sized by formula
`T0 × (maxBlockSize · r_max + 4) × 4 B` — at maxBlockSize 48 and r_max 4 that is ~6.3 KiB,
not a fixed 2 KiB. **`r_max = 4.0` (+24 st) is the enforced ratio ceiling**, validated in
mode files; it bounds the staging ring, the guard clamps, and the worst-case cache model.

## 4. Scheduler

**Trigger vocabulary** — OR'd, Clouds' triple-trigger extended (grain-delay-theory.md §3.2;
onset doc rec #8):

| Source | Mechanism | Used by (acid test) |
|---|---|---|
| `CLOCK` | phasor quantized to tap/Subdiv/MIDI clock, advancing an optional **step table** | Seq, Arp, Mosaic, Pattern, Warp |
| `PERIODIC` | overlap-referenced phasor, `space = size / target` | Glide, Tunnel, Haze A |
| `STOCHASTIC` | Bernoulli per sample, self-limiting at target | Haze B, Blocks |
| `ONSET` | mark ring fed by the trigger layer | Strum, Arp, Blocks, Interrupt |
| `MIDI_NOTE` / `FOOTSWITCH` / `SIDECHAIN` | same comparator output stage | fallback (beyond-parity) |

Scheduler modifiers: **`burst`** {count, spacing_ms}; **`jitter`** (CLOCK/PERIODIC →
STOCHASTIC morph — the synchronous↔asynchronous axis); **`intermittency`** (per-event skip
probability, EmissionControl2 — beyond-parity, no acid-test row uses it).

**Step table** — the sequencing spine for Seq/Arp/Pattern/Warp. Reviewed schema (the v1 table
had no *position* field, which silently broke "rearranged" sequences, Arp's step→k walk, and
Pattern's tap rhythms — review finding):

```
steps: { count: 1..16, order: fixed | shuffle | random,
         entries: [ { slot,        // position on the clock grid
                      pos_sel,     // interpreted per position source:
                                   //   GRID → slice index; MARK → mark index k; LIVE → ms offset
                      ratio_idx,   // index into the layer's pitch set
                      gain, prob, mod_flags } ] }
```

`order: shuffle` is the re-ordering operation Seq's "rearranged into new rhythmic sequences"
requires (distinct from `jitter`, which smears timing, and `intermittency`, which skips);
per-entry `prob` gives Seq A's "random rhythms". `steps.count` is macro-addressable (Arp's
Activity = "number of steps"; Pattern's = "active taps").

**Density** is overlap-referenced with a cubic law: `target = kMaxGrains · overlap³`, grain
rate a derived readout. The JSON key is **`overlap`** (renamed from `density` — "density 1"
colliding with "one voice" confused the v1 acceptance tests). Under a `CLOCK` source the
emission rate is the grid; `overlap` then acts as a don't-fire ceiling, documented as such.

**Position sources:**

| Source | Meaning | Notes |
|---|---|---|
| `POS_LIVE(d, spray)` | `d` ms behind the write head, ± spray (exp law) | plus optional per-step ms offsets (taps) |
| `POS_MARK(k, walk, jit)` | k-th most recent onset mark, optionally walking a cascade | mark ring holds 64 marks |
| `POS_PIN{anchor, rearm_ms \| rearm_src}` | pinned reference; re-armed periodically, on onset, or manually | rearm is a schema field (v1 said "per Repeats" with no field) |
| `POS_GRID(slice)` | quantized slice of the last bar | may reduce to quantized `POS_LIVE` — prototype decides |

**Micro-loop behavior**: the Micro Loop family plays *repeating* loops, not one-shot grains
from a sliding buffer (review finding — `POS_LIVE` alone never loops). The position object
carries `repeat: N` — the voice re-reads its resolved source region N times (or until
stolen), which is what makes Mosaic "overlapping loops at multiple speeds" and Tunnel a
drone. Each layer also carries `decay_ms` — a multi-repeat amplitude envelope (Tunnel's
Repeats = "how long the drone takes to decay").

**Layers.** ≤ 2 per mode, sharing the 64 slots. Per layer: `slot_share` (the pool split —
renamed from `voice_weight`), **`level_db`** (static balance), position, size, window, pitch,
pan_spread, `decay_ms`, modifiers. **`layer_mix` is a modulation route destination and macro
target** — Warp D's "taps cross-fade with grains" and Seq B's "fully CW adds a sustainer"
are un-expressible without it (review finding).

**Modulation.** A mode declares `modulators: [{id, type: lfo|env, rate_hz|sync, shape,
depth, attack_ms, release_ms}]` and `routes: [{from, to, amount}]` with destinations
`size | cutoff | ratio | position | layer_mix | pan`. Separately, **`links`** are
schedule-time correlations between per-grain draws (`{from: grain.pitch, to: grain.pan,
amount}`) — Argotlunar's parameter-correlation idea; they cost one multiply at birth, and
are distinct from block-rate routes.

**Allocation policy derives from trigger origin** — don't-fire for free-running sources,
oldest-steal for explicit triggers (onset/MIDI/footswitch) so Strum/Arp/Blocks never drop a
hit. It is **not** a mode-file field (the v1 `steal` key let a mode break the guarantee).

**`pre_delay` is non-negotiable** (grain-delay-theory.md §3.3): without it every grain onset
quantizes to the block boundary and buzzes at the block rate.

**Trigger layer** (onset doc recs #1–#8): spectral flux via CMSIS-DSP, 512-window/256-hop,
with adaptive whitening (~105–140 cycles/sample); the multi-band time-domain filterbank
(`bonk~` architecture, BSD) as the lower-latency alternative for onset-triggered modes; a
Chroma-Console-style calibration gesture; a per-onset trigger LED (counted, not a boolean —
§9); and the non-audio fallback sources. **No algorithm solves both distorted-guitar and
slow-pad onsets — we market the fallbacks and the visibility, not a solved problem.**

## 5. Modes as data

A mode is a JSON document outside `dsp/` and a compiled POD **`ModeBlob`** inside it (one
name throughout; sizeof computed from the vocabulary, reconciled against the §7 slot budget).
`dsp/` never parses text; `modes::Compile()` — built into `dsp/`, host-callable, non-realtime
— is the single shared compiler for firmware and plugin. Schema rules
(preset-parameter-and-patch-format.md recs #1–#3): top-level `schema_version`; values in
plain units keyed by **stable string names, never reused once released**; per-key tolerant
defaulting for additive changes; name-keyed migration table only for genuine breaks; **every
time value in milliseconds or tempo divisions, never sample counts**. `Compile` resolves
semitones/scales to ratio floats in the blob; `Serialize` reproduces the *authored* form
(including the editor-facing `ratio_gen` block) so round-trips are lossless.

```jsonc
{ "schema_version": 1, "id": "mosaic.d", "name": "Mosaic D", "category": "microloop",
  "scheduler": { "sources": ["clock"], "subdiv": "tap", "jitter": 0.0,
                 "overlap": 0.55, "intermittency": 0.0,
                 "burst": { "count": 1, "spacing_ms": 0 } },
  "layers": [ {
      "slot_share": 1.0, "level_db": 0.0, "voice_count": 4,
      "position": { "source": "live", "base_ms": 600, "spray_ms": 40, "repeat": 8 },
      "size_ms": 180, "size_law": "exp", "decay_ms": 2500,
      "window": { "sustain": 0.75, "skew": 0.5, "smoothness": 0.6 },
      "pitch":  { "set": [ {"st": -12, "weight": 1}, {"st": 0, "weight": 2},
                           {"st": 12, "weight": 1}, {"st": 24, "weight": 1} ],
                  "select": "cycle",
                  "quantize": { "mode": "off" },
                  "spread_cents": 0,
                  "glide": { "st_start": 0, "st_end": 0, "curve": 0.5 },
                  "reverse_prob": 0.0 },
      "pan_spread": 0.6,
      "modifiers": [ { "op": "svf", "band": "lp", "cutoff_hz": 6000, "res": 0.2,
                       "cutoff_src": "fixed" } ] } ],
  "ratio_gen": { "kind": "octaves" },          // editor-only; preserved through Serialize
  "modulators": [], "routes": [],
  "links": [ { "from": "grain.pitch", "to": "grain.pan", "amount": 0.4 } ],
  "feedback": { "amount": 0.35, "hp_hz": 100, "lp_hz": 6500, "diffusion": 0.3 },
  "post": { "order": ["mod", "delay", "reverb", "filter"], "bypass": ["delay"],
            "delay":  { "time": "1/4", "fb": 0.3, "mix": 0.0 },
            "reverb": { "time": 0.5, "mode": "hall", "mix": 0.35 },
            "filter": { "morph": 0.0, "cutoff_hz": 12000, "res": 0.1 },
            "mod":    { "rate_hz": 0.4, "depth": 0.15 } },
  "dry_duck": { "enabled": false, "depth": 1.0, "attack_ms": 5, "release_ms": 80 },
  "out_trim_db": 0.0,
  "macros": [
    { "id": "activity", "display_name": "Loopers",
      "targets": [ { "param": "layer0.voice_count", "range": [1, 8], "curve": 3.0 } ] },
    { "id": "shape", "display_name": "Contour",
      "targets": [ { "param": "layer0.window.sustain", "range": [0.2, 0.9], "curve": 1.0 },
                   { "param": "layer0.window.skew",    "range": [0.3, 0.7], "curve": 1.0 } ] },
    { "id": "repeats",
      "targets": [ { "param": "layer0.position.repeat", "range": [2, 16],  "curve": 1.0 },
                   { "param": "layer0.decay_ms",        "range": [500, 6000], "curve": 1.5 } ] },
    { "id": "time",  "targets": [ { "param": "layer0.position.base_ms",
                                    "range": [600, 2000], "curve": 1.0 } ] } ]
}
```

Notes against the v1 example (all review findings): `base_ms` is 600 because the guard
demands `base_ms ≥ size_ms·(r_max − 1)` = 540 at +24 st; Activity drives an explicit
per-layer **`voice_count`** (the old target, the pool-split weight, is inert on a one-layer
mode); `position.repeat` is what makes this *loops* rather than a delay smear; the pitch-set
entries carry weights; `quantize` is an object with room for root/scale.

**Per-voice modifier vocabulary** (≤ 2 slots at v1 — see §12): resonant SVF
(LP/BP/HP/notch; fixed/random/LFO/env cutoff source), bit-crush (bits + decimation),
sub-octave entry, glissando, gain. The voice struct carries real SVF and crush state (§3) —
the v1 one-pole could not represent this vocabulary (review finding). `dry_duck` generalizes
Interrupt's documented Mix-at-100 % dry mute into a per-voice gate with
{depth, attack_ms, release_ms}; its depth scales with the Mix control so the documented
behavior (full interruption at Mix = 100 %) falls out.

**Coverage acid test** (all 11 effects; key rows and their now-explicit mechanisms):

| Effect | Position · Trigger | Expressed as |
|---|---|---|
| **Mosaic** | LIVE+repeat · CLOCK | octave ratio sets; Activity → `voice_count` 1–8 |
| **Seq** | GRID · CLOCK+steps | `steps.order: shuffle`, per-entry `prob`; B: layer 2 sustainer via macro `in_range` gate + `layer_mix` |
| **Glide** | LIVE · PERIODIC | `glide {st_start, st_end, curve}`; Shape → `curve`; D = two layers, opposite endpoint pairs |
| **Haze** | LIVE · STOCHASTIC | jitter = 1; Activity → overlap **and** spray |
| **Tunnel** | PIN{rearm} · PERIODIC | `repeat` + `decay_ms` (Repeats → decay); modifiers via `modulators`/`routes` |
| **Strum** | MARK · CLOCK+ONSET | `mark_walk` cascade; B = burst 6, 3 ms spray |
| **Blocks** | MARK+GRID · ONSET‖STOCH | burst 3–8; C = LP + long decay; D = crush |
| **Interrupt** | MARK · ONSET‖STOCH | `dry_duck`; C = LFO→cutoff + feedback |
| **Arp** | MARK(steps.pos_sel = k) · CLOCK | step table walks the mark ring; Activity → `steps.count` |
| **Pattern** | LIVE + steps (per-tap ms offsets) · CLOCK | ratio {1.0} → Tu path; A = 1 tap + feedback = classic delay |
| **Warp** | as Pattern | per-tap SVF/env; D = layer 2 grains, `lfo → layer_mix` crossfade |

Honesty notes (replacing v1's overclaims): variations differ in ≤ 4 fields *except where a
variation adds a whole layer* (Seq B, Glide D, Warp D). Reuse: CLOCK/steps 5 effects, MARK 4,
LIVE 5, STOCHASTIC 3; single-user elements are `POS_GRID` (Seq — may be eliminated),
`POS_PIN` (Tunnel + the Hold Sampler feature), and `dry_duck` (Interrupt); `MIDI_NOTE`/
`FOOTSWITCH`/`SIDECHAIN`/`intermittency` are used by **zero** parity rows — they are
beyond-parity capabilities and earn their place on the community wishlist, not on coverage.

**Hot-loading and mode switching.** JSON parses off the audio thread into a slot of a
**4-slot ModeBlob ring**; publish is a release-store pointer swap, the audio thread
acquire-loads once per block before scheduling. **Reclamation is explicit** (review finding —
two slots race under repeated switching): each grain carries its blob's epoch; a slot is
reusable only when it is not the published blob *and* its live-grain count is zero.
`PublishMode` returns `false` when no slot is retirable (caller retries off-thread) rather
than clobbering a blob mid-render. Mode switch crossfades *populations* — in-flight grains
finish under the old blob, new grains are born under the new one; `ModeSwitch::Trails`
(default) vs. `ModeSwitch::FastCut`. Presets store the mode by content hash **plus an inline
copy**. Loop playback is a separate subsystem and survives mode changes; preset preview/queue
is an open item (§12).

**Validation** (`modes::Validate`, which takes the target sample rate): non-empty Shape
macro map (the "Shape does nothing" complaint is a mapping defect); guard-limit warnings per
pitch entry (`base_ms ≥ size_ms·(r − 1)`); ratio ceiling `r ≤ 4.0`; tier-feasibility (glide/
filter concurrency vs. tier capacity); `d_min_fb` when feedback > 0; grain-length bounds.

## 6. Parameter and macro model

Two tiers over **one** parameter system (microcosm.md §13.1; preset doc recs #6–#7).

**The leaf/structure split** (review finding — a `constexpr` table cannot enumerate leaves of
a variant document): the permanently-ID'd, automatable **leaf set is fixed**:
`layer{0,1}.{voice_count, level_db, size_ms, decay_ms, pan_spread, position.base_ms,
position.spray_ms, position.repeat, window.{sustain,skew,smoothness},
pitch.{spread_cents, reverse_prob}, modifier{0,1}.{p0..p3}}` (op-generic slots),
`scheduler.{overlap, jitter, intermittency}`, `feedback.*`, `post.*`, `dry_duck.depth`,
`out_trim_db`, and the 8 macros. Everything else — op tags, set/table contents, `size_law`,
`select`, `order`, `ratio_gen` — is **mode structure**: compiled, not automatable, not
host-visible. Macro `targets[].param` addresses stable leaf names only.

- One `constexpr ParamDescriptor` table in `dsp/include/brainscape/Params.h` — permanent
  `uint32_t` ID + stable name per leaf and per macro; plain (denormalized) values everywhere;
  consumed unchanged by firmware pots, VST3/CLAP registration, preset I/O, tests. Never
  reuse an ID or name; retire and rename.
- **Macro targets** carry `{param, range, in_range, curve}` — `curve` is the OWL-style skew
  exponent (the corpus's `curve_amount` is folded into it, noted deliberately), and
  **`in_range`** is a clamped input window so a target can be dead until, say, 0.8 —
  required by Seq B/D's documented "fully CW adds …" threshold behaviors (review finding).
- **Performance tier** (pedal, Microcosm-parity 8 knobs): Activity, Shape, Time, Repeats,
  Space, Filter, Mix, Loop Level. **Time has the Microcosm's dual mode**: a SELECT tap
  toggles Subdiv (enum: 1/4, 1/2, TAP, 2×, 4×, 8×) vs. Tempo (smooth, subdivision forced to
  quarters); tap tempo stays live in Tempo mode and is disabled under external MIDI clock.
  A mode's `subdiv` field is a recall default, overridden by the live control. **Space** is a
  macro fanning out to `post.delay.mix` + `post.reverb.mix` (its shift-secondary: reverb
  time + mode). **Filter** carries the endpoint semantics from §2.6. Mix and Loop Level are
  global, not per-mode.
- **Expression pedal is a first-class control source**: per assignment
  `{target ParamId, min, max, curve}`, multiple simultaneous targets, saved with the preset —
  the Microcosm's undefinable-range defect (microcosm.md §12.1) fixed as data. The jack
  handling lives in `firmware/`; the assignment model and its persistence live here.
- **Automation semantics, documented loudly:** post-chain parameters smooth continuously
  (per-sample one-poles, fixed time constants); grain parameters are sampled at grain
  birth — effective automation resolution equals the grain rate. Feature, not bug.
- **Soft takeover (Pickup)** — Surge XT's lock/proximity-unlock machine — applies to
  physical pots after preset recall and to absolute MIDI CC; never to host automation. MIDI
  map avoids CC 0/1/6/7/10/11/32/38/64/65/98/99/100/101/121/123; relative encoding pinned to
  Relative 2's Complement, published as spec.

## 7. Memory plan

Sizes are **formulas evaluated by `PlanMemory(EngineConfig)`**; the numbers shown are for the
firmware config (48 kHz, maxBlockSize 48) unless noted.

| Region | Contents | KiB |
|---|---|---|
| **DTCM arena** (Tier::Hot) | 64 grain structs (`sizeof(Grain)` ≈ 120 B, static_asserted) 7.5 · grain-size LUT 1 · block accumulator `2ch × maxBlockSize × 4 B` 0.4 (plugin @512: 4) · scheduler/RNG/smoothers 5 · mark ring 0.5 · [optional T0 staging `8 × (maxBlockSize·r_max + 4) × 4 B` ≈ 6.3] | **~15–21** |
| **DTCM, linker-managed** | ISR stack, platform .data/.bss — *not* arena rows; libDaisy's stock linker scripts default .data/.bss/stack to DTCM, so actual headroom **must be read from the .map file** (§10 gate). Window LUT (16 KiB) moves to AXI if DTCM is tight. | — |
| **AXI arena** (Tier::Warm) | reverb tank 72 (worst case, from-scratch Dattorro @ 48 kHz 16-bit; 48 if Clouds-derived) · window LUT 16 · feedback diffuser 8 · mod delay lines "2 × 50 ms @ sr" 19 · onset detector 10 · SVFs/DC/sat/misc 8 · **ModeBlob ring 4 slots × 4 KiB = 16** · preset staging 12 | **~161** |
| **D2** | libDaisy audio DMA, FatFs/SDMMC, MIDI, UI — no DSP state | platform |
| **SDRAM arena** (Tier::Bulk) | history ring 2²² frames stereo int16 = 87.4 s → **16 MiB** · looper A **23 MiB** + looper B **23 MiB** · post-chain delay line (≤ 2 s stereo int16, sequential access — cache-friendly) 0.4 · scratch 0.5 | **≈62.9 / 64 MiB** |

Rules: only the history ring, looper, and post-delay touch SDRAM at audio rate (the reverb
tank in SRAM avoids the measured ~3.5× penalty, post-fx doc §1.6). SDRAM is not zeroed at
boot — cleared by `Init()`, and **only the history ring**: clearing all of Bulk would be a
~0.3–0.6 s boot stall and would destroy a user's recorded loop on every plugin re-prepare
(review finding) — looper clears are explicit (`ClearLooper()`) and incremental. **Looper
undo is O(1) by watermark**, not by 23 MiB memset: buffer B carries a valid-until watermark;
undo stops mixing B and resets the watermark; stale content is overwritten lazily as
recording proceeds (review finding — a synchronous clear would blow 60–200 consecutive 1 ms
deadlines). **SD saves of SDRAM buffers follow an explicit cache-coherency rule**: 32-byte-
aligned, line-multiple ranges, `SCB_CleanDCache_by_Addr` before DMA write-out, invalidate
before DMA read-in (or bounce through non-cacheable D2) — the SDRAM region is MPU-mapped
cacheable, and skipping this writes stale bytes to the card intermittently (review finding).
Loop-bearing presets go to microSD (required from v1). **QSPI holds the firmware image**
(BOOT_QSPI) — and because NOR flash cannot service XIP reads during erase/program, a QSPI
write mid-performance is a *guaranteed* multi-ms stall, not a risk: the settings blob
therefore also goes to **microSD**, and QSPI is written only during firmware update
(review finding — this supersedes the v1 "settings blob in QSPI" plan).

The 16 MiB ring (87 s) over 32 MiB is a taken product decision (post-fx doc rec #10): it is
what lets a ~2-minute undo-capable loop coexist. Multiple loop slots (4 × ~33 s undo-capable)
remain possible within the same 46 MiB — slot count is a looper-doc decision, named here so
the trade is visible.

## 8. CPU budget

Worst case, 10,000 cycles/sample @ 48 kHz / 480 MHz. All figures derived, **not measured**;
DWT counters gate every stage (§10). The v1 table's two systematic errors are fixed: the
cache-miss model is **rate-dependent** (misses/grain/sample = r/8 for interleaved stereo
int16; the corpus's 8 misses/sample total is the r = 1 case), and **schedule-time work is
charged** (SemitonesToRatio, pan, window mean, SVF coefficients — resolved per birth inside
`Process()`; mitigated by LUTs but not free).

| Stage | Nominal | Pessimistic | Basis |
|---|---|---|---|
| Grain render, 64 voices, tiered (misses included) | ~1,600 (r ≈ 1) | ~4,000 (r = 4, conflict ×2) | grain-delay-theory.md §5.4, rate-scaled |
| ScheduleGrain (births/s = voices/size; LUT-based) | ~30 (20 ms grains) | ~530 (1 ms grains, bursts) | derived (review finding) |
| Per-voice modifiers (32 SVFs + crush + glide) | ~450 | ~700 | derived from post-fx §3.1 |
| Feedback taming (DC+HP+LP+sat+4-AP, stereo) | ~150 | ~200 | derived |
| Reverb (Dattorro, tank in SRAM) | 400–800 | 800 | post-fx §1.2–1.3, §6.2 |
| Post delay (stereo, SDRAM sequential) | ~40 | ~60 | derived (same class as looper) |
| Post filter (stereo double-sampled morph SVF) | 56–70 | 70 | post-fx §3.1 |
| Modulation (stereo) | 40–60 | 60 | post-fx §6.2 |
| Trigger: spectral flux 512/256 + whitening | 105–140 | 300 (filterbank alt.) | onset doc §3.2, §4 |
| Looper (stereo, interpolated path) | 40–60 | 60 | post-fx §6.2, §4.5 |
| Norm/smoothing/dry/mix | ~40 | ~50 | derived |
| **Total** | **≈2,950–3,700 (30–37 %)** | **≈6,800 (68 %)** | |

UI/MIDI/LED and *mode-level* coefficient recalc run at control rate and are not charged; the
per-birth transcendentals above are the exception the v1 footnote wrongly excluded. Even the
pessimistic case stays inside budget; the optional CloudSeed-class "big ambient" reverb mode
is gated against it.

## 9. `dsp/` core API

```cpp
namespace brainscape {

enum class Tier : uint8_t { Hot /*DTCM*/, Warm /*AXI*/, Bulk /*SDRAM|heap*/ };
struct MemoryPlan { size_t bytes[3]; size_t align[3]; };
struct Arenas     { void*  base [3]; size_t bytes[3]; };

struct EngineConfig {
  double   sampleRate    = 48000.0;   // fixed for the Engine's lifetime — rate changes
                                      // re-run PlanMemory + Init with fresh arenas
  uint32_t maxBlockSize  = 512;       // worst case; firmware passes 48, plugin the host max.
                                      // Process with numFrames > maxBlockSize is a debug assert.
  uint32_t historyFrames = 1u << 22;  // power of two; must match across builds for contract #6
  uint32_t looperFrames  = 0;         // 0 disables the looper subsystem
  bool     stereoInput   = true;
};
// kMaxGrains (64) and tier fractions are shared build constants, not config — mode files
// are authored against them.

MemoryPlan PlanMemory(const EngineConfig&) noexcept;   // pure; sampleRate-dependent

class Engine {
 public:
  bool Init(const EngineConfig&, const Arenas&) noexcept; // no allocation; clears the history
                                                          // ring + engine state ONLY (never looper)
  void Reset() noexcept;                                  // RT-safe: kills grains, zeroes post/
                                                          // feedback state, drains pending params
                                                          // and snaps smoothers; ring + params kept
  void ClearHistory() noexcept;                           // non-RT (~40-80 ms memset)
  void ClearLooper()  noexcept;                           // non-RT, explicit — a plugin prepare
                                                          // path must NOT call this

  struct ProcessContext {
    const float* const* in;   float* const* out;      // planar, deinterleaved
    uint32_t numFrames;                               // 1..maxBlockSize, varies freely
    double   tempoBpm; int64_t timelinePos; bool transportPlaying;
  };
  void Process(const ProcessContext&) noexcept;  // no alloc/locks/syscalls/exceptions/RTTI

  // Parameters — permanent IDs, plain values; macros fan out internally.
  // sampleOffset = frames from the start of the NEXT Process call.
  void  SetParam(ParamId, float plainValue, uint32_t sampleOffset = 0) noexcept;
  float GetParam(ParamId) const noexcept;
  static const ParamDescriptor* Descriptors(size_t* count) noexcept;

  // Modes — compiled off-thread; 4-slot ring with epoch/live-grain reclamation (§5).
  bool         PublishMode(const ModeBlob*, ModeSwitch style = ModeSwitch::Trails) noexcept;
               // false = no retirable slot yet; retry off-thread
  ModeInfo     ActiveModeInfo() const noexcept;   // value copy — no raw blob pointers escape

  // Transport / performance
  void SetTempo(double bpm) noexcept;  void Tap() noexcept;  void SetSubdiv(Subdiv) noexcept;
  void SetExternalClock(bool) noexcept;
  void SetFreeze(bool) noexcept;             // pins the position anchor (§2.4 limit applies)
  void SetGlobalReverse(bool) noexcept;
  void Trigger(TriggerSource, float velocity = 1.f, uint32_t sampleOffset = 0) noexcept;
  uint32_t ConsumeOnsetCount() noexcept;     // atomic exchange(0); audio thread increments.
                                             // Counted, not boolean — bursts stay visible (LED
                                             // driver stretches each to a visible minimum).

  // State — versioned; includes the RNG sample counter (contract #7)
  size_t SaveState(void* dst, size_t cap) const noexcept;
  bool   LoadState(const void* src, size_t bytes) noexcept;  // absent key = version default
  static constexpr uint32_t kStateSchemaVersion = 1;

  // Latency (all informational; dry is never delayed)
  uint32_t LatencySamples()         const noexcept { return 0; }
  uint32_t WetOnsetLatencySamples() const noexcept;  // ≈ base_ms + size·max(0,1−r_min) + block,
                                                     // from the active blob (mode-dependent)
  uint32_t TriggerLatencySamples()  const noexcept;  // flux 512/256 ≈ 11–16 ms
};

enum class ModeSwitch : uint8_t { Trails, FastCut };

namespace modes {  // non-realtime, host-callable, never from Process
  bool   Compile(const char* json, size_t len, ModeBlob* out, char* err, size_t errCap) noexcept;
  size_t Serialize(const ModeBlob&, char* jsonOut, size_t cap) noexcept;  // lossless round-trip
  bool   Validate(const ModeBlob&, double sampleRate, ModeError* out) noexcept;
}

struct ScopedDenormalGuard { ScopedDenormalGuard() noexcept; ~ScopedDenormalGuard() noexcept; };
} // namespace brainscape
```

**Threading contract** (review finding — the ABI must say who calls what):

| Methods | Context | Mechanism |
|---|---|---|
| `Process` | audio thread only | direct |
| `SetParam`, `Trigger`, `Tap`, `SetTempo`, `SetSubdiv`, `SetFreeze`, `SetGlobalReverse`, `SetExternalClock` | any thread | lock-free SPSC event queue, 256 entries, coalesce-on-overflow (newest wins per ParamId); drained at the top of `Process` |
| `ConsumeOnsetCount`, `GetParam`, `ActiveModeInfo` | any thread | atomics / value copies |
| `PublishMode` | non-RT thread | release-store publish; acquire-load in `Process` |
| `Init`, `Reset`*, `ClearHistory`, `ClearLooper`, `SaveState`, `LoadState`, `modes::*` | non-RT (`Reset` is RT-safe) | — |

**Counter-based RNG, fully specified** (review finding — "absolute sample index" alone is
ambiguous and collides): a Philox/Squares-class counter PRNG keyed on the tuple
`(engine_sample_counter, layer_index, burst_ordinal, draw_purpose)`, where
`engine_sample_counter` is a free-running `int64` owned by the Engine, advanced by
`numFrames` every `Process` regardless of transport or freeze, and saved in `SaveState`.
`draw_purpose` enumerates {spray, pitch-select, detune, pan, reverse, intermittency, jitter,
step-shuffle, dither} so simultaneous draws (two layers on one clock tick; `burst` with
spacing 0) never collide — identical draws would collapse the stereo image exactly where
width is wanted. Independent of `timelinePos` (a stopped DAW transport must not freeze the
texture).

Loop unrolling is recovered by a fixed internal `kChunk = 16` sub-block that never appears
in the public signature; no smoother, phasor, or RNG may key off block count or block
boundaries.

## 10. Contracts and acceptance tests

1. **Block-splitting bit-exactness (within one build).** Rendering 4096 frames as
   `{1, 7, 32, 48, 64, 127, 512}`-frame blocks (all ≤ maxBlockSize) produces identical
   samples. The 7- and 127-frame cases exist to catch `kChunk` alignment leaks; a dedicated
   check diffs the normalization-gain trace across two block sizes.
2. **Unity-rate null test.** Preconditions stated in full: rectangular window
   (`sustain = 1, smoothness = 0`), grains abutting and phase-locked to the delay time,
   overlap → N = 1, feedback = 0, `pan_spread = 0`, dither off, reference = the same int16
   ring read at the same offset. Under those, the `Tu` path nulls **to the bit**; the
   general case (dither on) nulls below −90 dBFS. The feedback variant is a separate
   listening check, not a null.
3. **Level consistency.** Sine sweep across the full N = 1…64 range: < 1 dB variation, with
   a dedicated coherent-case (unity-rate, zero-spray) variant.
4. **Feedback boundedness.** Full-scale impulse, feedback max, pitch ±12 st, **and
   `base_ms` swept to 0** (the `d_min_fb` guard's regression test): bounded limit cycle,
   never a rail.
5. **Mono compatibility** across the full `pan_spread` range.
6. **Mode round-trip.** `Compile → Serialize → Compile` idempotent; the same mode file
   drives firmware and plugin to identical output **given identical `EngineConfig`
   (historyFrames, sampleRate) and starting RNG counter** — within one build, bit-exact.
7. **Cross-build equivalence.** Firmware (Cortex-M7, FMA-contracted) vs. plugin (x86) output
   nulls below **−120 dBFS RMS over a 10 s render** — *not* bit-exact: `-ffp-contract`,
   libm differences, and rsqrt paths make true cross-ISA bit-exactness cost the FMA
   throughput the §8 budget assumes (review finding). If bit-exactness is ever wanted, the
   price is listed there.
8. **State round-trip.** `SaveState → LoadState` (including the RNG counter) restores
   byte-identical behavior within a build.
9. **SD loop save integrity.** Save and reload a loop with a known pattern across a power
   cycle; byte-identical (exercises the cache-coherency rule).
10. **Hardware measurement gates.** DWT counters around every stage; the grain-count stress
    test **at r = 4 with scattered positions** (the worst case the rate-dependent miss model
    predicts) before the polyphony ceiling and tier split are frozen; a `.map`-file audit of
    DTCM occupancy on a hello-world libDaisy build before the arena plan is trusted.

## 11. Decisions taken

| Decision | Choice | Why |
|---|---|---|
| Engine structure | One engine, modes as data | microcosm.md §4.5; the design bet |
| Phase format | Split 32.32, masked absolute + fraction | 16.16 overflows at 500 ms × r 4; float ULP ≥ 0.5 samples at ring top |
| Ratio ceiling | `r_max = 4.0` (+24 st), validator-enforced | bounds guards, staging, and the cache model |
| Reverse convention | Start at scheduled position, read backward | near guard trivially safe; far guard `bufLen − L·(1+r)` |
| History ring | 16 MiB, 2²² frames, int16 interleaved | undo-capable 2-min looper must coexist (post-fx rec #10) |
| Looper undo | Two buffers + O(1) watermark undo | Microcosm parity without 23 MiB memsets in the control path |
| Freeze | Pinned reference; ring keeps recording; ~87 s ceiling documented | zero cost; coexists with looper |
| Feedback | Topology A + fixed taming chain + `d_min_fb` guard | grain-delay-theory.md §3.10 + review |
| Normalization | `N^(−p)`, coherence-resolved exponent, per-sample τ smoothing | √N is wrong for coherent (delay) modes |
| Window | {sustain, skew, smoothness}, unit-peak legs, mean-divide compensation | v1's `1+2·shape` was sign-inverted |
| Post chain | mod → delay → reverb → filter, ordered bypassable list | Space = delay + reverb is documented Microcosm identity |
| Reverb | Dattorro w/ input diffusion, 16-bit tank in AXI (72 KiB budgeted worst-case) | post-fx §1; ReverbSc is LGPL + 4× oversized |
| Block size | Runtime at the API (default max 512); fixed internal `kChunk` | post-fx §7; hosts hand out 1024+ |
| Sample rate | Fixed per Engine lifetime; changes re-run PlanMemory+Init | `SetSampleRate` invalidated already-sized arenas |
| Firmware rate | Pinned 48 kHz v1; `dsp/` rate-agnostic | budget and ring time halve at 96 k |
| Trigger v1 | Spectral flux + whitening + calibration + counted LED + fallbacks | onset doc recs #1–#8 |
| Settings storage | microSD (not QSPI) | XIP stall during QSPI write is guaranteed, not a risk |
| Mode format | JSON, plain units, stable names, tolerant defaults; `ModeBlob` 4-slot ring | preset doc recs #1–#3 + reclamation |
| Dry latency | 0, dry never block-delayed | grain-delay-theory.md §3.11 |
| Layers / modifiers | 2 layers, 2 modifier slots at v1 | all 44 variations fit; raising later is a schema break |
| Voices / tiers | 64; 8/24/32 + Tu read-path specialization | Clouds precedent + budget |

**License posture (GPLv3):** Clouds (MIT) vendorable with attribution; DaisySP core (MIT),
CMSIS-DSP (Apache-2.0) fine; `bonk~` (BSD-3) adaptable; DaisySP-LGPL (LGPL-2.1) linkable but
disclosed — we prefer from-scratch Dattorro; aubio (GPLv3) vendorable but desktop-shaped —
reimplement flux against CMSIS; **Argotlunar is "GPL v2" — verify v2-only vs. v2-or-later
from its source headers before lifting anything** (v2-only is GPLv3-incompatible →
study-only); Essentia (AGPLv3) study-only; GuitarML `DaisyCloudSeed` glue has no LICENSE —
do not vendor.

## 12. Risks and open questions

1. **Feel.** The data-driven scheduler may not reproduce hand-tuned algorithm feel. No
   architecture retires this — only mode-curation effort does. (Central risk.)
2. **Cache behavior at 64 scattered streams** is the load-bearing unverified assumption
   (~40-cycle miss penalty, 3.5× SDRAM figure, 16 KB/32 B cache). Stress test at r = 4
   before any preset is dialled.
3. **16-bit history inside the feedback loop** recirculates quantization noise. TPDF dither
   on write + the saturator; needs a listening test. (Dither uses the counter-based RNG so
   contract #2's dither-off mode and contract #6 stay reproducible.)
4. **Onset default method**: spectral flux vs. complex-domain for distorted repeated notes —
   listening test, not more reading.
5. **`POS_GRID` vs. quantized `POS_LIVE`** — one prototype decides.
6. **Micro-loop `repeat` semantics** (does a repeating voice re-read through the *live* ring
   region or a snapshot reference?) interacts with freeze and feedback — prototype the two
   readings against real Mosaic recordings.
7. **Layer cap (2) and modifier slots (2)** are schema-frozen guesses; validate before the
   first community patch ships.
8. **Wet-latency reporting** as an opt-in plugin toggle: open product question.
9. **Mode-population trails** (up to one grain length of old mode): default Trails with
   FastCut option; verify it reads as a feature. Preset preview/queue (community wishlist
   #9) is not yet designed.
10. **Freeze-past-ring-length behavior**: re-anchor-on-wrap (chosen, documented splice) vs.
    stop-writing-while-frozen (stale ring on unfreeze) — revisit after hardware listening.
11. **DTCM headroom** is asserted from region size, not measured — the `.map` audit (contract
    #10) gates the arena plan; the window LUT moves to AXI if DTCM is tight.
12. **Looper subsystem design** (transport API, slot count, quantize, Burst, Looper-Only,
    varispeed) is deferred to its own document; its memory/CPU envelope is fixed here.

## 13. Provenance

Synthesized 2026-08-30 from three independent proposals over the research corpus — *Tessera*
(Microcosm-fidelity lens), *Alluvium* (DSP lens), *STRATA* (contracts lens) — then revised
(v2, same day) against a three-reviewer adversarial pass (DSP arithmetic; real-time/platform;
product coverage & schema consistency) that produced 60 findings. Material corrections
adopted in v2: per-direction write-head guard derivation and reverse convention;
coherence-aware normalization; window unit-peak legs and mean-divide compensation; widened
voice struct (SVF/crush state); step-table position field; layer `level_db` + `layer_mix`
routes; micro-loop `repeat`; Space delay stage restored; leaf/structure parameter split;
macro `in_range`; expression as a control source; Time dual mode; ModeBlob 4-slot
reclamation; fully-specified counter RNG; per-sample smoothers; rate-dependent cache model +
ScheduleGrain CPU row; Init/Clear split and O(1) looper undo; SD cache-coherency rule;
settings moved off QSPI; contracts #2/#6/#7 restated with preconditions and a cross-build
tolerance. Corpus references: [grain-delay-theory.md](../research/grain-delay-theory.md),
[microcosm.md](../research/microcosm.md),
[post-fx-chain-looper-and-system-budget.md](../research/post-fx-chain-looper-and-system-budget.md),
[preset-parameter-and-patch-format.md](../research/preset-parameter-and-patch-format.md),
[onset-detection-on-cortex-m7.md](../research/onset-detection-on-cortex-m7.md),
[vst-and-shared-dsp.md](../research/vst-and-shared-dsp.md),
[daisy-seed-platform.md](../research/daisy-seed-platform.md).
