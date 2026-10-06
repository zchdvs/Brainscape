# Brainscape Grain Engine — Design

> Design document for the core of `dsp/`: the granular voice engine, its scheduler, and the
> modes-as-data model that expresses Microcosm-class behavior and beyond.
> Synthesized from three independent architecture proposals over the research corpus in
> [docs/research/](../research/), then revised against a three-lens adversarial review
> (DSP arithmetic, real-time/platform safety, product coverage) that produced 60 findings —
> the substantive ones are incorporated below. Every load-bearing number is traceable to a
> corpus document; figures marked *derived* are cycle-count estimates, not measurements.
> Status: **reviewed draft, v2** — pending prototype validation on hardware.

> **Update (2026-10-05).** Two later design documents extend this one and supersede parts of
> it. [determinism-profile.md](determinism-profile.md) specifies how engine output becomes
> bit-identical on the pedal and every desktop build (proposed, not yet implemented; today's
> builds diverge): it replaces contracts #6 and #7 and adds test requirements to contract #1
> (§10), adds `Restart`, `LoadPreset` and frame-stamped event delivery to the API (§9),
> replaces the lookup tables specified for pitch ratios and pan with in-tree math kernels
> (§3, §8) and the public denormal guard with a private floating-point-environment guard
> (§9), and raises the §8 CPU estimates. [companion-app.md](companion-app.md) moves mode and
> preset compilation to the desktop only (§1, §5), makes a mode switch a frame-stamped event
> (§5), has the plugin pass a fixed `maxBlockSize` of 512 (§9), and specifies the USB and
> firmware-update flow that §1 leaves to another document. Both documents run the engine at
> 48 kHz everywhere (§11). The passages changed here point at the governing text, and §13
> lists them.

---

## 1. The design bet

**One voice engine + one scheduler + a small modifier vocabulary, with modes as data.**

[microcosm.md](../research/microcosm.md) §4.5 establishes (as flagged inference) that the
Microcosm's 11 effects × 4 variations are one engine differing only in scheduling and
per-voice modifiers. We build exactly that engine once, and ship the 44-destination
experience — and everything the Microcosm structurally cannot do — as JSON mode files.

Consequences that drive everything below:

- **Modes are files, not firmware builds.** User-loadable, git-diffable, shared between pedal
  and plugin. They are authored as JSON and compiled on the desktop into binary packages that
  the pedal loads; the pedal never parses JSON (§5). The mode schema is the project's real ABI
  (microcosm.md §13.2).
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
   frozen" as an open alternative (§12). The code re-anchors once the live write head is
   three quarters of the ring past the pin (`dsp/src/Engine.cpp:405-414`; 65.5 s at 2²²
   frames, calculated), and that decision must be made per sample, not per block
   (contract #1, §11).
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

**Pitch.** `ratio = SemitonesToRatio(st + cents/100)` resolved at birth **by an in-tree
DetMath kernel, never libm's `powf` or `exp2f`** (schedule-time transcendentals are charged
in §8). DetMath is the determinism profile's private library of math kernels, written so
that every target computes the same bits. This was amended on 2026-10-05; the earlier text
specified a lookup table with linear interpolation. Both are deterministic, so the cost per
birth decides: polynomial kernels are the provisional choice, to be confirmed or reversed by
a DWT measurement of `ScheduleGrain` at the maximum birth rate, and DetMath-built tables for
`SemitonesToRatio` and the pan law are the fallback
([determinism-profile.md](determinism-profile.md), "DetMath replaces every libm
transcendental"). Today's code still calls `std::exp2`
(`dsp/include/brainscape/GrainMath.h:52`), and the TODO above that call (`:48-51`), which
asks for a lookup table, predates this decision. Composable layers: per-mode weighted
interval set (≤ 8 entries, `cycle | random` selection) → optional scale/chord
quantization → per-grain `spread_cents` detune. Output duration is fixed; pitch changes how
much source is consumed (§3.7 mapping (a)). **Glide** is specified as endpoints —
`glide: {st_start, st_end, curve}` — so the interval is stable when `size_ms` changes, and
`curve` is the trajectory shape that Glide's Shape macro drives (the manual's one explicit
per-effect Shape meaning). **Reverse** is a per-grain coin flip on `reverse_prob`; a global
FWD/REV flag covers the Microcosm's Reverse button.

**Write-head guards** — derived per direction, clamped once at schedule time. `d` = scheduled
delay behind the **live** write head (the ring frame written at the grain's birth sample),
`L` = grain length in output samples, `r` = |rate|, `margin` a few ms, `W_a` =
`kBlockWriteAheadFrames` = 512. **Reverse convention: a reverse grain starts at its scheduled
position `W₀ − d` and reads backward** (receding from the write head — this is what makes the
near guard trivially safe for reverse; the "start at region end" convention would instead
demand `d ≥ L·r`):

| Direction | Near guard (read must not overtake write) | Far guard (write must not lap the grain) |
|---|---|---|
| Forward | `d ≥ L·max(0, r−1) + margin` | `d ≤ bufLen − W_a − L·max(0, 1−r) − margin` |
| Reverse | `d ≥ margin` | `d ≤ bufLen − W_a − L·(1+r) − margin` |

**Live-head reference and write-ahead term** (amended 2026-10-05). The guards are measured
from the live write head even while freeze is engaged. A position measured from the freeze
pin is clamped against the far rail moved `age` frames closer, where `age` is how far the
live head has advanced past the pin; if that rail falls below the near rail, the far rail
wins. `W_a` exists because `Process` writes the whole input block into the ring before it
renders any grain (`dsp/src/Engine.cpp:457-498`). When output sample *n* renders, up to 511
ring frames ahead of the live head already hold new input in a 512-frame block but
one-ring-old audio in a 1-frame block, so no interpolation tap may read inside that window.
`W_a` is a shared build constant equal to the largest legal block, never derived from
`maxBlockSize`, because the pedal (48-frame blocks) and the plugin (up to 512) must clamp
identically; it costs 10.7 ms of ring depth at the far end (calculated). **The code
implements neither rule yet**: `dsp/src/Granular.cpp:102-104` computes the bounds over the
full ring length, and while frozen the start frame is measured from the pin (`:114`). See
contract #1.

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
| `POS_MARK(k, walk, jit)` | k-th most recent onset mark, optionally walking a cascade | mark ring holds 16 marks, enough for a 16-entry step table to address one mark per step (`kMaxMarks`, `dsp/include/brainscape/detail/Granular.h:99`; amended 2026-10-05 from 64 to match the code) |
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
The engine never parses text; `modes::Compile()` — source in `dsp/`, host-callable,
non-realtime — is the single shared compiler. **It runs on the desktop only** (amended
2026-10-05; [companion-app.md](companion-app.md), "Compilation happens on the desktop
only"): the companion app and its command-line preset compiler build JSON into binary preset
packages, and the firmware links only the binary decoder and a structural validator, never a
JSON parser. One compiler then yields one blob everywhere, which a pedal-side parse could
not guarantee: newlib's `strtof` rounds decimal input twice (measured by disassembly,
companion-app.md). Schema rules
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

**Hot-loading and mode switching.** A compiled blob is decoded off the audio thread (on the
pedal, from a compiled package) into a slot of a **4-slot ModeBlob ring**; publish is a
release-store pointer swap that the audio thread acquire-loads. **Reclamation is explicit**
(review finding — two slots race under repeated switching): each grain carries its blob's
epoch; a slot is reusable only when it is not the published blob *and* its live-grain count
is zero. `PublishMode` returns `false` when no slot is retirable rather than clobbering a
blob mid-render. **The switch is a frame-stamped event** (amended 2026-10-05;
[companion-app.md](companion-app.md), "Complete-state presets, applied by one `dsp/`
function"): it takes effect exactly at its stamped frame inside `Process`, not at the next
block. The producer (the firmware control loop or the desktop wrapper) stages the blob into
a retirable slot *before* stamping the event; if no slot is free it delays the stamp, never
the application, so the applied frame is part of the logged event stream and identical on
both sides. A per-block publish with a retry would land on different frames on the pedal's
48-frame grid and the app's 512-frame chunks. Mode switch crossfades *populations* —
in-flight grains finish under the old blob, new grains are born under the new one;
`ModeSwitch::Trails` (default) vs. `ModeSwitch::FastCut`. Presets store the mode by content
hash **plus an inline copy**. Loop playback is a separate subsystem and survives mode changes;
preset preview/queue is an open item (§12).

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
| **AXI arena** (Tier::Warm) | reverb tank (**float32 as implemented: ~96 KiB**; 16-bit Q4.12 halves it and is the M7 fallback if AXI gets tight) · window LUT 16 · feedback tamer diffuser 4 · feedback FIFO 4 · mod delay lines "2 × 25 ms @ sr" 10 · onset detector 10 · SVFs/DC/sat/misc 8 · **ModeBlob ring 4 slots × 4 KiB = 16** · preset staging 12 | **~176** |
| **D2** | libDaisy audio DMA, FatFs/SDMMC, MIDI, UI — no DSP state | platform |
| **SDRAM arena** (Tier::Bulk) | history ring 2²² frames stereo int16 = 87.4 s → **16 MiB** · looper A **23 MiB** + looper B **23 MiB** · post-chain delay line (≤ 2 s stereo, **float32 as implemented: 0.75 MiB**; int16 halves it — sequential single stream either way) · scratch 0.5 | **≈63.3 / 64 MiB** |

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
`Process()`, every transcendental by an in-tree DetMath kernel with DetMath-built tables as
the fallback, and not free; amended 2026-10-05 from "mitigated by LUTs", see §3 "Pitch").

| Stage | Nominal | Pessimistic | Basis |
|---|---|---|---|
| Grain render, 64 voices, tiered (misses included) | ~1,600 (r ≈ 1) | ~4,000 (r = 4, conflict ×2) | grain-delay-theory.md §5.4, rate-scaled |
| ScheduleGrain (cycles/sample, as in every row; birth rate = voices/size, at most one scheduler birth per sample, i.e. 48,000/s, plus onset and manual births — `dsp/src/Engine.cpp:309-314`, `dsp/src/Granular.cpp:236`, `:321`; in-tree kernels, amended 2026-10-05 from "LUT-based"; their extra cost, about +20 nominal and +300 pessimistic (estimated), is in the amended totals below) | ~30 (20 ms grains) | ~530 (1 ms grains, bursts) | derived (review finding) |
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
is gated against it. The determinism profile (contraction off, a per-sample flush of
recursive state, in-tree transcendentals at grain birth) raises the totals to about
3,200–4,050 cycles/sample (32–41 %) nominal and 7,700–7,800 (77–78 %) pessimistic, all
estimated ([determinism-profile.md](determinism-profile.md), "Effect on the grain-engine CPU
budget").

## 9. `dsp/` core API

```cpp
namespace brainscape {

enum class Tier : uint8_t { Hot /*DTCM*/, Warm /*AXI*/, Bulk /*SDRAM|heap*/ };
struct MemoryPlan { size_t bytes[3]; size_t align[3]; };
struct Arenas     { void*  base [3]; size_t bytes[3]; };

struct EngineConfig {
  double   sampleRate    = 48000.0;   // fixed for the Engine's lifetime — rate changes
                                      // re-run PlanMemory + Init with fresh arenas
  uint32_t maxBlockSize  = 512;       // worst case; firmware passes 48, the plugin passes 512
                                      // and chunks larger host blocks (Init rejects > 512).
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
  void ClearHistory() noexcept;                           // non-RT memset of ring + post delay
                                                          // (~17.5 MB): M7 est. ~45-160 ms,
                                                          // floor ~44 ms at the SDRAM peak;
                                                          // desktop 0.92 ms measured (note below)
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

  // State — versioned; includes the RNG sample counter (contract #8)
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

// No public floating-point guard: the engine's entry points set and restore the whole
// control word themselves, through a guard private to dsp/ (note below).
} // namespace brainscape
```

**Threading contract** (review finding — the ABI must say who calls what):

| Methods | Context | Mechanism |
|---|---|---|
| `Process` | audio thread only | direct |
| `SetParam`, `Trigger`, `Tap`, `SetTempo`, `SetSubdiv`, `SetFreeze`, `SetGlobalReverse`, `SetExternalClock` | one producer per engine: the firmware control loop, or the desktop wrapper's audio-thread side; other threads post to that producer | lock-free SPSC event queue, 256 entries, carrying frame-stamped events that take effect at their exact frame inside `Process`; an overflow is counted, never coalesced, and a render with a nonzero count is outside the parity contract |
| `ConsumeOnsetCount`, `GetParam`, `ActiveModeInfo` | any thread | atomics / value copies |
| `PublishMode` | non-RT thread | release-store publish; acquire-load in `Process`; the switch takes effect at its stamped frame (§5) |
| `Init`, `Reset`*, `ClearHistory`, `ClearLooper`, `SaveState`, `LoadState`, `modes::*` | non-RT (`Reset` is RT-safe) | — |

The event row was amended on 2026-10-05. The earlier wording, "any thread" through a
single-producer queue with "coalesce-on-overflow (newest wins per ParamId)", contradicted
itself and silently rewrote the event stream
([determinism-profile.md](determinism-profile.md), "Frame-stamped event delivery"). The
`ClearHistory` figures come from the same document ("Exact restart API"): libDaisy clocks the
SDRAM for a 400 MB/s peak, which puts the floor for ~17.5 MB at 44 ms (calculated); the
earlier "~40–80 ms" started below that floor, and the 45–160 ms range is an estimate pending
a DWT measurement.

Two lines of the listing were amended on the same date. **`maxBlockSize`:** the earlier
comment had the plugin pass the host's maximum. The plugin passes 512 whatever the host's
maximum and splits larger host buffers into calls of at most 512 frames, because `Init`
rejects a larger value (`dsp/src/Engine.cpp:118`) and the engine is initialized once per
instance ([companion-app.md](companion-app.md), "Canonical configuration and lifecycle" and
"Host blocks"); the same stale comment remains in `dsp/include/brainscape/Engine.h:29`.
**The denormal guard:** the listing used to export a `ScopedDenormalGuard` for callers.
Today's `dsp/include/brainscape/DenormalGuard.h` ORs flush-to-zero bits into the caller's
control word and guards only `Process` (`dsp/src/Engine.cpp:394`). The determinism profile
replaces it with a private guard, `dsp/src/detail/FpEnvGuard.h`, that writes the complete
control word (round-to-nearest, gradual underflow, which keeps subnormals rather than
flushing them to zero) on `Init`, `Reset`, `Restart`, `ClearHistory`, `Process`,
`LoadPreset`, `SetParam` and the exported helpers whose results reach the engine, and
restores the caller's word on exit ([determinism-profile.md](determinism-profile.md), "A
full control-word guard on every engine entry point", "The denormal decision: gradual
underflow everywhere" and "Guard rewrite").

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
   check diffs the normalization-gain trace across two block sizes. **The test must also
   reach the features that can break it** (amended 2026-10-05): freeze engaged mid-render,
   with every render split at the freeze event's frame; onsets that actually fire, asserted
   with `ConsumeOnsetCount() > 0`, so that `POS_MARK` and the onset trigger are exercised; a
   grain position on the far rail of the ring; and a freeze held past the re-anchor point.
   Mid-render events in general are covered once frame-stamped event delivery lands.
   **Status: violated by today's code.** Measured in the parity investigation on `main` at
   `e86e971`, three independent mechanisms make output depend on the block grid, because a
   grain can read ring frames that `Process` has already written ahead of the live write head
   in the current block (§3): (D1) while frozen, the write-head guards are measured from the
   pin instead of the live head, so a grain positioned at an onset recorded after the pin
   starts near or ahead of the live head; (D2) the far guard's 64-frame margin (`kGuardMarginFrames`,
   `dsp/include/brainscape/detail/Granular.h:14`) is smaller than the write-ahead of up to
   511 frames; (D3) the re-anchor of a held freeze is decided once per block
   (`dsp/src/Engine.cpp:405-414`). The existing test (`dsp/tests/test_engine.cpp:376-378`)
   never engages freeze and records 0 onsets (measured), so it could not see any of them. A
   three-part fix (A: guards measured from the live head; B: a far rail that excludes the
   512-frame write-ahead; C: re-anchor decided per sample) and a regression test were verified
   on a scratch copy and have not landed. Until they do, renders that must match the pedal
   run in 48-frame blocks aligned to frame 0, with every event on a multiple of 48. Mechanisms,
   fix and tests: [determinism-profile.md](determinism-profile.md), "The block-split bug: three
   verified mechanisms".
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
6. **Preset round-trip and cross-target identity.** `Compile → Serialize → Compile` is
   idempotent. The compiled preset package is the unit of identity. It is produced only on the
   desktop, by `dsp/`'s compiler running under the determinism profile, and compiling the same
   JSON on every desktop CI leg yields byte-identical packages. Loaded with
   `LoadPreset(P, Exact)` from the exact-restart state, one package drives the firmware and
   every desktop build to bit-identical output under the parity contract
   (`docs/design/determinism-profile.md` §2). This holds across builds, not merely within one.

   *Replaced 2026-10-05 with the text of determinism-profile.md §2.6, verbatim.* **Status: not
   met by today's code; builds diverge** ([determinism-profile.md](determinism-profile.md),
   "Why a tolerance is not enough"). The package compiler, `LoadPreset` and the exact-restart
   state (`Restart`) do not exist yet. The contract's preconditions (same sound revision,
   canonical `EngineConfig`, identical input bits and frame-stamped events, exact plain
   parameter values, execution inside the floating-point-environment guard) are listed in
   determinism-profile.md, "The parity contract"; independence from block size additionally
   needs contract #1's fix.
7. **Cross-build equivalence (bit-exact).** Every conforming build (determinism profile §3)
   reproduces the golden SHA-256 of every golden vector for the current `kSoundRevision`.
   Verified per pull request on the host matrix and on the emulated Cortex-M7, nightly under
   full-system emulation, and on hardware before every release. The earlier −120 dBFS
   tolerance is withdrawn.

   *Replaced 2026-10-05 with the text of determinism-profile.md §2.6, verbatim.* **Status: not
   met by today's code; builds diverge** ([determinism-profile.md](determinism-profile.md),
   "Why a tolerance is not enough"). A conforming build is one compiled under the profile's
   build and numerics rules that passes its audits; `kSoundRevision` is a constant in `dsp/`
   bumped by any change that can alter output. Neither the constant, the golden vectors nor
   the verification legs exist yet: today's CI runs the host test matrix and only compiles
   `dsp/` for the Cortex-M7 ([STATUS.md](../STATUS.md)). The tolerance was withdrawn because
   a difference as small as multiply-add fusion moves one grain birth by one sample, after
   which a jittered preset decorrelates: the default preset nulled at only −4.8 dB between a
   fused and an unfused build (measured in the parity investigation, determinism-profile.md
   §1.2). The price the earlier text declined (contraction off, in-tree transcendentals, no
   fast-math) is costed in determinism-profile.md, "Costs and the explicit-FMA option".
8. **State round-trip.** `SaveState → LoadState` (including the RNG counter) restores
   byte-identical behavior within a build. The determinism profile adds the RNG epoch used by
   Spillover preset loads to the saved state.
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
| Reverse convention | Start at scheduled position, read backward | near guard trivially safe; far guard `bufLen − W_a − L·(1+r) − margin`, per §3 (amended 2026-10-05 to add the write-ahead term and margin) |
| History ring | 16 MiB, 2²² frames, int16 interleaved | undo-capable 2-min looper must coexist (post-fx rec #10) |
| Looper undo | Two buffers + O(1) watermark undo | Microcosm parity without 23 MiB memsets in the control path |
| Freeze | Pinned reference; ring keeps recording; the pin re-anchors to the live head once it is three quarters of a ring old (65.5 s at 2²², calculated), decided per sample; write-head guards always measured from the live head (amended 2026-10-05) | zero cost; coexists with looper; a per-block decision made output depend on the block grid (contract #1) |
| Feedback | Topology A + fixed taming chain + `d_min_fb` guard | grain-delay-theory.md §3.10 + review |
| Normalization | `N^(−p)`, coherence-resolved exponent, per-sample τ smoothing | √N is wrong for coherent (delay) modes |
| Window | {sustain, skew, smoothness}, unit-peak legs, mean-divide compensation | v1's `1+2·shape` was sign-inverted |
| Post chain | mod → delay → reverb → filter, ordered bypassable list | Space = delay + reverb is documented Microcosm identity |
| Reverb | Dattorro w/ input diffusion, 16-bit tank in AXI (72 KiB budgeted worst-case) | post-fx §1; ReverbSc is LGPL + 4× oversized |
| Block size | Runtime at the API (default max 512); fixed internal `kChunk` | post-fx §7; hosts hand out 1024+ |
| Sample rate | Fixed per Engine lifetime; changes re-run PlanMemory+Init | `SetSampleRate` invalidated already-sized arenas |
| Engine rate (amended 2026-10-05) | 48 kHz everywhere: the firmware runs at 48 kHz, and the plugin and app run the engine at 48 kHz and resample other host rates. `dsp/` accepts other rates, but its constants are counted in frames (the 512-frame feedback FIFO, the 512/256 onset window and hop, the guard margin), so any other rate is a different sound | budget and ring time halve at 96 k; a 100 ms preset repeats every 110.667 ms at 48 kHz and 105.333 ms at 96 kHz (measured; [determinism-profile.md](determinism-profile.md), "Boundary"; [companion-app.md](companion-app.md), "Sample rate: the 48 kHz path and the resampled path") |
| Trigger v1 | Spectral flux + whitening + calibration + counted LED + fallbacks | onset doc recs #1–#8 |
| Settings storage | microSD (not QSPI) | XIP stall during QSPI write is guaranteed, not a risk |
| Mode format | JSON, plain units, stable names, tolerant defaults; `ModeBlob` 4-slot ring | preset doc recs #1–#3 + reclamation |
| Mode and preset compilation (added 2026-10-05) | Desktop only: the app and its command-line compiler build JSON into binary preset packages; the firmware decodes and validates packages and never parses JSON; a mode switch is a frame-stamped event | one compiler gives one blob on every target; newlib's `strtof` rounds twice (measured); [companion-app.md](companion-app.md) |
| Cross-target output (added 2026-10-05) | Bit-identical float32 on the pedal and every desktop build under the determinism profile: contraction off, no fast-math, in-tree transcendentals, a full floating-point-environment guard, golden hashes per sound revision | the owner's "same sound" requirement; a tolerance fails for stochastic presets (measured); [determinism-profile.md](determinism-profile.md) |
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
13. **Cross-target sample identity** (added 2026-10-05). The risks and open questions of the
    bit-exact contracts (#6, #7) and of the contract-#1 fix are tracked in
    [determinism-profile.md](determinism-profile.md), "Risks" and "Open questions". Among
    them: the unmeasured M7 cost of the profile (pessimistic total estimated at 77–78 % of
    the budget), subnormal timing on the M7, emulation versus silicon, and the fix's effect
    on Strum-family modes under freeze. There the choice between *pin-eligible marks* (only
    marks at or before the pin may position a frozen grain, so freeze holds the Strum
    position; recommended) and *live-head marks* (the newest mark, measured from the live
    head, so freeze has no effect on `POS_MARK` grains, measured) waits on a listening test.
    The app-side risks are in [companion-app.md](companion-app.md).

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
tolerance. Amended 2026-10-05 to align with [determinism-profile.md](determinism-profile.md)
and [companion-app.md](companion-app.md): the header note, §1, §2 commitment 4, the §3
guards and pitch-ratio method (in-tree kernels, tables as the fallback), the §4 mark-ring
size (16, as in the code), §5 compilation and mode switching, the §8 per-birth cost note,
ScheduleGrain row and totals, §9 `ClearHistory`, threading, the `maxBlockSize` comment
(the plugin passes 512 and chunks) and the removal of the public denormal guard (now
private to `dsp/`), §10 #1/#6/#7/#8 (#6 and #7 in the profile's replacement text, with
status lines), §11 (reverse far guard, engine rate, freeze, compilation, cross-target
output) and §12 #13; the evidence is recorded in those documents. Corpus references:
[grain-delay-theory.md](../research/grain-delay-theory.md),
[microcosm.md](../research/microcosm.md),
[post-fx-chain-looper-and-system-budget.md](../research/post-fx-chain-looper-and-system-budget.md),
[preset-parameter-and-patch-format.md](../research/preset-parameter-and-patch-format.md),
[onset-detection-on-cortex-m7.md](../research/onset-detection-on-cortex-m7.md),
[vst-and-shared-dsp.md](../research/vst-and-shared-dsp.md),
[daisy-seed-platform.md](../research/daisy-seed-platform.md).
