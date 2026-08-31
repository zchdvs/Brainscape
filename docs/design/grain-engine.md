# Brainscape Grain Engine — Design

> Design document for the core of `dsp/`: the granular voice engine, its scheduler, and the
> modes-as-data model that expresses Microcosm-class behavior and beyond.
> Synthesized from three independent architecture proposals (Microcosm-fidelity-first,
> DSP-first, and contracts-first lenses) over the research corpus in
> [docs/research/](../research/). Every load-bearing number is traceable to a corpus document;
> figures marked *derived* are cycle-count estimates, not hardware measurements.
> Status: **reviewed draft** — pending prototype validation on hardware.

---

## 1. The design bet

**One voice engine + one scheduler + a small modifier vocabulary, with modes as data.**

[microcosm.md](../research/microcosm.md) §4.5 establishes (as flagged inference) that the
Microcosm's 11 effects × 4 variations are one engine differing only in scheduling and
per-voice modifiers. We build exactly that engine once, and ship the 44-destination
experience — and everything the Microcosm structurally cannot do — as JSON mode files.

Consequences that drive everything below:

- **Modes are files, not firmware builds.** User-loadable, git-diffable, shared between pedal
  and plugin byte-for-byte. This is the killer feature no commercial competitor matches
  (microcosm.md §13.2), and it means the mode schema is the project's real ABI.
- **The macro layer is the product.** Activity/Shape/Time/Repeats as per-mode multi-parameter
  macros are why the Microcosm feels good (microcosm.md §11.2). We keep that layer exactly,
  as data, and put the depth in the editor/plugin (§13.1).
- **Novel capability is new field values, not new code.** Semitone/scale-quantized pitch,
  per-grain reverse, spray, glissando, intermittency, and "Mosaic pitch + Seq scheduler"
  (the #2 community request) all land inside the same vocabulary.
- **The engine internals are deliberately conservative** — Clouds' architecture (MIT,
  vendorable) wherever it is right — so the risk budget is spent on the scheduler
  vocabulary and the contracts, which are the genuinely new work.

The central risk, named up front: a data-driven scheduler is harder to tune by ear than 11
hand-written algorithms. The 44 factory mode files are therefore a first-class deliverable
with as much effort as the DSP (microcosm.md rec #3), and the acceptance test is the
corpus's: *a stranger plugs in, turns to any preset, plays one chord, and it sounds finished.*

## 2. Signal path

```
                       ┌──── dry (single-sample path, never block-delayed) ─────────────┐
                       │                                                                ▼
 IN ─► inTrim ─► DCblk ─┴─► [+] ─► HISTORY RING ──► GRAIN ENGINE ──► [norm] ─┬─► POST ─► MIX ─► OUT
                             ▲      SDRAM, int16     64 POD voices           │   CHAIN
                             │      interleaved LR   render per-voice        │   MOD → REVERB → FILTER
                             │      2^22 frames      over whole block        │   (ordered, per-stage
                             │      = 87.4 s         into DTCM accum         │    bypassable list)
                             │      writeHead ── pinnable ► FREEZE           │
                             │                                               │
                             └── FEEDBACK TAMING ◄───────────────────────────┘
                                 DCblk → HP 80–120 Hz → LP 4–8 kHz (fb-dependent)
                                 → soft saturation → 4-stage allpass diffuser

 side paths:
   ONSET DETECTOR (spectral flux + adaptive whitening, control rate) ─► mark ring ─► scheduler
   LOOPER (separate SDRAM buffers A+B, 4-way record/playback tap matrix)
   HOLD SAMPLER = the freeze pin — no buffer of its own
```

Structural commitments, each corpus-grounded:

1. **Resolve-once-at-schedule-time.** A voice never re-reads a global parameter after birth —
   the one rule Clouds, SuperCollider, and EmissionControl2 all state as contract
   (grain-delay-theory.md §2.1, §4.6). All mode interpretation happens in `ScheduleGrain()`
   at trigger rate, never in the inner loop. This is also what makes macro sweeps glitch-free
   by construction: in-flight grains are untouched; new grains arrive with new values.
2. **Render per-grain over the whole block into a DTCM float accumulator**, never per-sample
   over all grains (grain-delay-theory.md §5.4 — "the single biggest optimization available").
   Each grain gets a contiguous SDRAM read burst instead of 64 interleaved scattered streams.
3. **Feedback is topology (A)**: tamed wet summed into the record buffer, so grains eat their
   own output and pitch shifts compound (grain-delay-theory.md §3.10). Feedback > 1.0 is a
   designed self-oscillation feature bounded by the saturator. The taming chain's order is a
   stability requirement and is not user-reorderable; the post chain is.
4. **Freeze is a pinned write-head reference, not a buffer**
   (post-fx-chain-looper-and-system-budget.md §5.2). The Hold Sampler and Tunnel-style drones
   are the same mechanism. Zero extra SDRAM, one branch — and it is why freeze and the phrase
   looper coexist, fixing the Microcosm's unjustified mutual exclusivity.
5. **The dry path never enters the block-delayed wet path** (grain-delay-theory.md §3.11), so
   blend cannot comb and the plugin reports 0 latency.

## 3. Grain voice

**Pool.** `kMaxGrains = 64` POD structs in a fixed DTCM array — no allocation, no virtuals,
free-list bitmask. 64 voices ≈ 1,280–1,600 cycles/sample of the 10,000-cycle budget
(grain-delay-theory.md §5.3–5.4); Clouds sustains 40–60 grains on a 168 MHz M4 (§4.1).

**Voice struct (~80 B):**

```cpp
struct Grain {
  int32_t  first_frame;    // absolute (masked) ring frame where the grain's source begins
  int32_t  offs;           // grain-relative source offset, integer part   ┐ split 32.32
  uint32_t frac;           //                       fractional part (2^32) ┘ phase
  int32_t  inc_i;          // signed per-sample increment, integer part — pitch AND direction
  uint32_t inc_f;          //                              fractional part
  int32_t  frames_left;    // output samples remaining
  int32_t  pre_delay;      // sample-accurate onset within the block (consumed first)
  float    env_phase, env_inc;   // env_inc = 2.0f / width_samples
  float    shape, skew;          // window morph + attack/decay fold point
  float    gain_l, gain_r;       // equal-power pan, resolved at birth
  float    glide_inc;            // per-grain ratio ramp per sample (T0 only)
  float    filt_z, filt_c;       // per-grain one-pole state/coeff (T0/T1 only)
  uint8_t  tier, flags;          // quality tier; active | reverse | unity bits
};
```

**Phase format: split 32.32, not Clouds' 16.16, not float.** This is a deliberate departure,
forced by arithmetic: 16.16 caps the grain-relative range at 65,536 samples ≈ 1.37 s at
48 kHz, and a 500 ms grain (top of the recommended size range, grain-delay-theory.md §3.5) at
+24 st (ratio 4.0) traverses 96,000 source samples — overflow. Clouds never hit this at
32 kHz with octave-capped ratios; we would. Bare float32 is separately ruled out — it loses
sub-sample precision past ~2^24 samples on a multi-minute buffer (§3.3). The split
`int32 + uint32` accumulator costs one extra add-with-carry per sample and keeps precision
constant everywhere. (Fallback if measurement shows a material cost: 16.16 with grain length
validated against `65536 / ratio` in the mode validator — a documented limitation, never a
silent wrap.)

**Windowing.** Clouds' triangle-morphed-to-LUT (grain-delay-theory.md §3.4 strategy 1): the
envelope phase runs 0→2, folds at 1.0 into a triangle, the triangle value indexes a
4096-entry LUT, and `smoothness` crossfades raw-triangle↔LUT — boxcar↔triangle↔Hann on one
cheap parameter. Exposed partikkel-style as **`shape`** (flat-top amount) plus **`skew`**
(attack/decay fold point — percussive ↔ symmetric ↔ reverse-swell, free, and under-served in
existing pedals). `sustain→1` yields a trapezoid with short tapers — this is what makes the
delay modes sound like a real delay rather than a granular approximation. Loudness is
compensated by `window_gain = 1 + 2·shape` (§3.6).

**Pitch.** `ratio = SemitonesToRatio(st + cents/100)` resolved to the split increment at
birth. Three composable layers:

1. **Per-mode ratio/interval set** — up to 8 weighted entries sampled per grain (`cycle` or
   `random` selection). `{−12, 0, +12, +24}` *is* Mosaic D; `{0, +7, +12}` is a chord voicing
   the Microcosm structurally cannot make (microcosm.md §13.4 — the clearest differentiator).
   A `RatioGen` tag (`LITERAL | SEMITONE | SCALE | CHORD`) lets the editor generate the
   array; `dsp/` only ever sees resolved floats.
2. **Scale/chord quantization** of the resulting ratio.
3. **`pitch_spread_cents`** per-grain random detune (±10 c thickens enormously, §3.5).

Output duration is fixed; pitch changes how much source is consumed (§3.7 mapping (a)) —
grain rate never changes with pitch. **Glide**: `glide_inc` ramps the increment across the
grain's life (Argotlunar's per-grain glissando, §4.4) — Glide's variations are literally
`(ratio_start, ratio_end)` pairs. **Reverse** is a per-grain coin flip on `reverse_prob`
(§3.5), implemented as a negative increment starting at the end of the source region; reverse
grains recede *from* the write head, the safer direction (§2.1). A global FWD/REV flag covers
the Microcosm's Reverse button (a property of the read head, microcosm.md §4.5).

**Write-head guards**, clamped once at schedule time (grain-delay-theory.md §2.1):
`d ≥ L·max(0, r−1) + margin` (read must not overtake write) and
`d ≤ bufferLen − L·(1−r) − margin` (write must not lap the grain).

**Interpolation, tiered** (§3.8, §5.4 — quality tiering is *the* CPU lever):

| Tier | Voices | Render |
|---|---|---|
| **T0** | 8 | cubic Hermite, per-grain stereo, glide, per-grain one-pole filter |
| **T1** | 24 | linear, per-grain stereo |
| **T2** | 32 | linear, precomputed pan |
| **Tu** | any at rate == 1.0 exactly | **integer copy loop, bit-exact** |

The `Tu` path is non-negotiable: it is what makes unity-pitch grains — and therefore the
entire MultiDelay family and every ×1 Mosaic voice — sound like a clean delay (§3.8 rec #9).
Degradation in T1/T2 is masked by the window's own spectral splatter and texture density.
The render is templated on `<Tier, Stereo, Interp>` and dispatched **once per grain per
block**, never per sample (Clouds' `OverlapAdd` technique, §4.1).

**Normalization.** `gain = N > 2 ? rsqrt(N − 1) : 1`, one-pole smoothed at 0.01 per block
(§3.6). The `−1` and `> 2` guard keep a single grain at unity — a one-tap delay must not
change level. The smoothing is mandatory (unsmoothed `1/√N` steps audibly at every grain
birth/death). Plus a **per-mode `out_trim`** stored in the mode file — the fix for the
Microcosm's second-most-cited defect, inconsistent gain staging between presets
(microcosm.md §12.1, §13.6). Acceptance test: sine sweep at density 1 vs. max, < 1 dB
variation (§3.11).

**SDRAM access mitigations, all mandatory** (grain-delay-theory.md §5.4): 16-bit interleaved
stereo storage (one cache miss per 8 stereo frames on 32-byte lines — half the float miss
rate, one stream per grain instead of two); per-grain-over-block rendering; `PLD` prefetch
one line ahead; optionally for T0, a small DTCM staging ring block-copied ahead of read time.
Power-of-two strides make cache-set aliasing *more* likely — measure before optimizing.

## 4. Scheduler

**Trigger vocabulary** — OR'd, Clouds' triple-trigger extended
(grain-delay-theory.md §3.2; onset-detection-on-cortex-m7.md rec #8):

| Source | Mechanism | Microcosm effects needing it |
|---|---|---|
| `CLOCK` | phasor quantized to tap-tempo/Subdiv/MIDI clock, advancing an optional **16-step table** `{slot, ratio_idx, gain, mod_flags}` | Seq, Arp, Mosaic, Pattern, Warp |
| `PERIODIC` | overlap-referenced phasor, `space = size / target` | Glide, Tunnel, Haze A |
| `STOCHASTIC` | Bernoulli per sample, self-limiting at target | Haze B, Seq A, Blocks |
| `ONSET` | mark ring fed by the trigger layer | Strum, Arp, Blocks, Interrupt |
| `MIDI_NOTE` / `FOOTSWITCH` / `SIDECHAIN` | same comparator output stage | guaranteed-working fallback |

Scheduler modifiers: **`burst`** (one trigger emits N voices at spacing S — Blocks' "bursts",
Strum B's phasing stack), **`jitter`** (crossfades CLOCK/PERIODIC toward STOCHASTIC — Roads'
synchronous↔asynchronous axis on one knob), **`intermittency`** (probability a scheduled
grain is skipped — EmissionControl2's idea, "far more musical than lowering density").

**Density is overlap-referenced with a cubic law**: `target = kMaxGrains · overlap³`, grain
rate a derived readout (§3.2) — size and thickness stay perceptually decoupled.

**Position sources** — what lets one engine cover delay, granular, and onset effects:

| Source | Meaning | Used by |
|---|---|---|
| `POS_LIVE(d, spray)` | `d` behind the write head, ± spray (exponential law) | Mosaic, Haze, Glide, Pattern, Warp |
| `POS_MARK(k, walk, jit)` | the k-th most recent onset mark, optionally walking a cascade | Strum, Arp, Blocks, Interrupt |
| `POS_PIN(anchor)` | pinned reference, re-armed each `Repeats` period | Tunnel, Hold Sampler |
| `POS_GRID(slice)` | quantized slice of the last bar | Seq |

(Open question: `POS_GRID` may reduce to `POS_LIVE` quantized — Seq is its only user;
prototype before the schema freezes.)

**Layers.** A mode has ≤ 2 concurrent layers sharing the 64 slots by declared weight. Every
documented Microcosm variation fits in 2 (Seq B's sustainer pad, Glide D's bidirectional
pair, Warp D's taps-crossfading-into-grains). Raising this later is a schema break — decided
at 2 deliberately.

**Allocation policy split by trigger origin** (§3.3): **don't-fire** when the free-running
scheduler exceeds target (click-free); **oldest-steal** for explicit triggers (onset, MIDI,
footswitch) so Strum/Arp/Blocks never drop a hit.

**`pre_delay` is non-negotiable** (§3.3): grains are scheduled at sample resolution and the
render loop consumes `pre_delay` first. Without it, every grain onset quantizes to the block
boundary and buzzes at the block rate.

**Trigger layer** (onset-detection-on-cortex-m7.md recs #1–#8): spectral flux via CMSIS-DSP
`arm_rfft_fast_f32`, 512-window/256-hop, with adaptive whitening, as the v1 audio-onset
detector (~105–140 cycles/sample, ~1.4 %); the multi-band time-domain filterbank (`bonk~`
architecture, BSD) as a genuinely lower-latency alternative for onset-triggered grain modes;
a Chroma-Console-style "play for a few seconds" calibration gesture; a visible per-onset
trigger LED; and the non-audio fallback sources above. **No algorithm in the literature
solves both distorted-guitar and slow-pad onsets — we do not market this as solved**, we
market the fallbacks and the visibility.

## 5. Modes as data

A mode is a JSON document outside `dsp/` and a POD `ModeData` (~1.6 KiB) inside it. `dsp/`
never parses text (preset-parameter-and-patch-format.md rec #7); `firmware/` and `plugin/`
share one compiler (`modes::Compile(json) → ModeBlob`) built into `dsp/` as a host-callable,
non-realtime function. Schema rules (preset doc recs #1–#3): `schema_version` at top level,
values in **plain units keyed by stable string names** — never reused once released — with
per-key tolerant defaulting for additive changes and a name-keyed migration table only for
genuine breaks. **Every time value is milliseconds or tempo divisions, never sample counts**
(DaisySP's own `pos_ < 4800` bug is the cautionary tale).

```jsonc
{ "schema_version": 1, "id": "mosaic.d", "name": "Mosaic D", "category": "microloop",
  "scheduler": { "sources": ["clock"], "subdiv": "tap", "jitter": 0.0,
                 "density": 0.55, "intermittency": 0.0, "steal": "none",
                 "burst": { "count": 1, "spacing_ms": 0 } },
  "layers": [ {
      "position": { "source": "live", "base_ms": 250, "spray_ms": 40 },
      "size_ms": 180, "size_law": "exp", "voice_weight": 1.0,
      "window": { "shape": 0.75, "skew": 0.5 },
      "pitch":  { "set": [{"st":-12},{"st":0},{"st":12},{"st":24}], "select": "cycle",
                  "quantize": "off", "spread_cents": 0, "glide_st_per_s": 0.0,
                  "reverse_prob": 0.0 },
      "pan_spread": 0.6,
      "modifiers": [ { "op": "svf", "band": "lp", "cutoff_hz": 6000, "res": 0.2,
                       "cutoff_src": "fixed" } ] } ],
  "links": [ { "from": "grain.pitch", "to": "grain.pan", "amount": 0.4 } ],
  "feedback": { "amount": 0.35, "hp_hz": 100, "lp_hz": 6500, "diffusion": 0.3 },
  "post": { "order": ["mod","reverb","filter"], "bypass": [] },
  "dry_duck": false, "out_trim_db": 0.0,
  "macros": [
    { "id": "activity", "display_name": "Loopers",
      "targets": [ { "param": "layers.0.voice_weight", "range": [1, 8],  "curve": 3.0 } ] },
    { "id": "shape",    "display_name": "Contour",
      "targets": [ { "param": "layers.0.window.shape", "range": [0, 1],  "curve": 1.0 },
                   { "param": "layers.0.window.skew",  "range": [0.2, 0.8], "curve": 1.0 } ] },
    { "id": "repeats",
      "targets": [ { "param": "feedback.amount", "range": [0, 1.05], "curve": 1.5 } ] } ]
}
```

**Per-voice modifier vocabulary** (≤ 4 slots, all cheap, all resolved at birth): SVF
(LP/BP/HP/notch; fixed/random/LFO/envelope cutoff source), bit-crush (bits + decimation),
sub-octave ratio entry, glissando, gain. Plus mode-level LFO/envelope sources with ≤ 4 routes
(→ size | cutoff | ratio | position). One flag, `dry_duck`, gates the dry signal while voices
are active — Interrupt's documented Mix-at-100% semantics, the only single-use flag in the
vocabulary.

**Coverage acid test.** The Microcosm-fidelity proposal expressed all 11 effects in this
vocabulary; the load-bearing rows:

| Effect | Position · Trigger | Expressed as |
|---|---|---|
| **Mosaic** | LIVE · CLOCK | ratio sets = octave subsets; Activity → voice_weight (1–8 "loopers") |
| **Seq** | GRID · CLOCK + steps | step table shuffle; B adds sustainer layer (size×8, LP, low gain) at Activity CW |
| **Glide** | LIVE · PERIODIC | `(ratio_start, ratio_end)` pairs; D = two layers, opposite directions |
| **Haze** | LIVE · STOCHASTIC | jitter=1; Activity → density **and** spray (the manual's own "density and spread") |
| **Tunnel** | PIN · PERIODIC | pinned anchor re-armed per Repeats; modifiers LFO→size / LFO→cutoff / random BP / ENV→size |
| **Strum** | MARK · CLOCK+ONSET | mark_walk cascade; B = burst 6 with 3 ms spray (phasing) |
| **Blocks** | MARK+GRID · ONSET‖STOCH | burst 3–8; C = LP + long decay; D = crush 5-bit |
| **Interrupt** | MARK · ONSET‖STOCH | **dry_duck**; C = LFO→cutoff + feedback; D = crush |
| **Arp** | MARK(step→k) · CLOCK | step_count 2–8 = Activity; C = random cutoff per step |
| **Pattern** | LIVE+tap table · CLOCK | ratio {1.0} → **Tu integer path**; A with 1 tap + feedback = classic delay |
| **Warp** | as Pattern | per-tap SVF/ENV/BP; C adds ratios; D = taps layer + grains layer crossfaded |

Every A–D variation differs in ≤ 4 fields. Only `dry_duck` is single-use; everything else is
reused ≥ 3× — the test that the vocabulary is minimal rather than merely sufficient. Pattern A
and Mosaic sharing one code path is the strongest validation of the one-engine bet.

**Hot-loading and mode switching.** JSON parses off the audio thread into a `ModeBlob`
double-buffer slot; a single release-store pointer swap publishes it. Structural changes
crossfade *populations*, not audio: in-flight grains finish under the old blob, new grains
are born under the new one — click-free by construction because grains resolve at birth. This
fixes the Microcosm's "preset switching interrupts performance" complaint; a fast-cut option
covers the long-grain edge case (up to one grain length of old-mode trails). Presets store
the mode by content hash **plus an inline copy**, so a shared preset opens without its repo.

**Validation** is part of the schema: `ValidateMode()` rejects, among others, an empty
`shape` macro map (the "Shape does nothing" complaint is a mapping defect — every mode must
bind Shape to something audible, minimally `window.shape`+`skew`), out-of-range voice
weights, and grain-length × ratio combinations that exceed guard limits.

## 6. Parameter and macro model

Two tiers over **one** parameter system (microcosm.md §13.1; preset doc recs #6–#7):

- One `constexpr ParamDescriptor` table in `dsp/include/brainscape/Params.h` — permanent
  `uint32_t` ID + permanent stable name per parameter **and per macro**, consumed unchanged
  by firmware pot code, VST3/CLAP registration, preset I/O, and tests. Values are plain
  (denormalized) everywhere; never reuse an ID or name once released — retire and rename.
- **Performance tier** (pedal panel, Microcosm-parity 8 knobs): Activity, Shape, Time,
  Repeats, Space, Filter, Mix, Loop Level. Macro targets carry `{param, range, curve}` with a
  single OWL-style skew exponent — no piecewise curves at v1. `SetParam()` on a macro ID fans
  out to leaf calls inside `dsp/`; no consumer ever sees two parameter systems.
- **Expert tier** (plugin + editor): the full leaf tree plus the macro mappings themselves,
  editable, saved with presets. CLAP expresses the hierarchy via `module` paths; VST3 gets
  flat IDs plus an `IRemapParamID` table.
- **Automation semantics, documented loudly:** post-chain parameters smooth continuously, but
  grain parameters are sampled at grain birth — effective automation resolution equals the
  grain rate. That is the feature that makes modulation glitch-free, not a bug.
- **Soft takeover (Pickup)** — Surge XT's lock/proximity-unlock state machine — applies in
  exactly two places: physical pots after preset recall, and absolute MIDI CC. Never to host
  automation. MIDI map avoids CC 0/1/6/7/10/11/32/38/64/65/98/99/100/101/121/123 and pins
  one relative encoding (Relative 2's Complement), published as spec.

## 7. Memory plan

| Region | Contents | KiB |
|---|---|---|
| **DTCM** (128 KB, zero-wait) | 64 grain structs (~80 B) 5 · window LUT 4096×f32 16 · grain-size LUT 256×f32 1 · block accumulator 2ch×256×f32 2 · T0 staging rings 2 · scheduler/RNG/param smoothers 5 · mark ring 0.5 · ISR stack 16 | **~48** |
| **AXI SRAM** (512 KB, ~480 usable after bootloader) | reverb tank (Dattorro/Griesinger, 16-bit Q4.12 @ 48 kHz) 48 · feedback diffuser 8 · mod delay lines 19 · onset detector (512-pt rFFT + whitening) 10 · SVFs/DC/sat/misc 8 · ModeBlob double-buffer 16 · looper transport + preset staging 20 | **~129** |
| **D2** (288 KB) | libDaisy audio DMA, FatFs/SDMMC, MIDI, UI — no DSP state | platform |
| **SDRAM** (64 MiB) | history ring 2²² frames stereo int16 = 87.4 s → **16 MiB** · looper A (committed) **23 MiB** + looper B (overdub/undo) **23 MiB** · scratch 0.5 | **62.5 / 64 MiB** |

Placement rules: only the history ring and looper touch SDRAM at audio rate. The reverb tank
lives in SRAM — the corpus documents a ~3.5× CPU penalty (42 % vs 12 %) for a ~240 KB reverb
wrongly placed in SDRAM (post-fx doc §1.6). SDRAM is not zeroed at boot: cleared from
`Init()`, never a constructor. Loop-bearing presets go to microSD (required from v1); the 8 MB
QSPI holds only the parameter/mode blob, saved on explicit gesture (blocking, no wear
leveling — preset doc rec #5).

**The 16 MiB ring (87 s) over the 32 MiB option is a taken product decision** (post-fx doc
rec #10): it is what lets a ~2-minute undo-capable loop coexist. 87 s of history already
exceeds anything a granular pedal is documented to need.

## 8. CPU budget

Worst case, 10,000 cycles/sample @ 48 kHz / 480 MHz. All figures derived from corpus
cycle-count analysis, **not measured** — a DWT cycle counter goes around each stage as it
lands.

| Stage | Nominal cyc/sample | Pessimistic | Basis |
|---|---|---|---|
| Grain render, 64 voices, tiered, mitigations | 1,280–1,600 | 3,200 (cache ×2) | grain-delay-theory.md §5.4 |
| Per-voice modifiers (32 single-pass SVFs, crush, glide) | ~400 | ~800 | derived from post-fx §3.1 |
| Feedback taming (DC+HP+LP+sat+4-AP diffuser, stereo) | ~150 | ~200 | derived |
| Reverb (Dattorro, tank in SRAM) | 400–800 | 800 | post-fx §1.2–1.3, §6.2 |
| Post filter (stereo double-sampled morph SVF) | 56–70 | 70 | post-fx §3.1 |
| Modulation (stereo) | 40–60 | 60 | post-fx §6.2 |
| Trigger: spectral flux 512/256 + whitening | ~110–145 | 300 (filterbank alt.) | onset doc §3.2, §4 |
| Looper (stereo, interpolated path) | 40–60 | 60 | post-fx §6.2, §4.5 |
| Norm/smoothing/dry/mix | ~40 | ~50 | derived |
| **Total** | **≈2,500–3,300 (25–33 %)** | **≈5,500 (55 %)** | |

UI/MIDI/LED and coefficient recalc (`sinf`/`powf`) run at control rate and are not charged.
Even the pessimistic case leaves headroom for an optional CloudSeed-class "big ambient"
reverb mode, gated against the budget.

## 9. `dsp/` core API

```cpp
namespace brainscape {

// Memory seam: firmware maps tiers to linker sections; plugin mallocs (vst doc rec #4)
enum class Tier : uint8_t { Hot /*DTCM*/, Warm /*AXI*/, Bulk /*SDRAM|heap*/ };
struct MemoryPlan { size_t bytes[3]; size_t align[3]; };
struct Arenas     { void*  base [3]; size_t bytes[3]; };

struct EngineConfig {
  double   sampleRate    = 48000.0;   // runtime — no baked rate anywhere in dsp/
  uint32_t maxBlockSize  = 48;        // worst case; Process may pass fewer
  uint32_t historyFrames = 1u << 22;  // power of two (masked indexing)
  uint32_t looperFrames  = 0;         // 0 disables the looper subsystem
  uint32_t maxGrains     = 64;
  bool     stereoInput   = true;
};

MemoryPlan PlanMemory(const EngineConfig&) noexcept;  // pure; callable before any allocation

class Engine {
 public:
  bool Init(const EngineConfig&, const Arenas&) noexcept; // does NOT allocate; clears Bulk
  void Reset() noexcept;                                  // silences tails; keeps params+mode
  void SetSampleRate(double sr) noexcept;                 // not RT-safe; call while stopped

  struct ProcessContext {
    const float* const* in;   float* const* out;      // planar, deinterleaved
    uint32_t numFrames;                               // 1..maxBlockSize, varies freely
    double   tempoBpm; int64_t timelinePos; bool transportPlaying;
  };
  void Process(const ProcessContext&) noexcept;  // no alloc/locks/syscalls/exceptions/RTTI

  // Parameters — permanent IDs, plain values, sample-accurate; macros fan out internally
  void  SetParam(ParamId, float plainValue, uint32_t sampleOffset = 0) noexcept;
  float GetParam(ParamId) const noexcept;
  static const ParamDescriptor* Descriptors(size_t* count) noexcept;

  // Modes — compiled off-thread, published atomically, crossfaded by grain population
  void            PublishMode(const ModeBlob*, ModeSwitch style = ModeSwitch::Trails) noexcept;
  const ModeBlob* ActiveMode() const noexcept;

  // Transport / performance
  void SetTempo(double bpm) noexcept;  void Tap() noexcept;  void SetSubdiv(Subdiv) noexcept;
  void SetExternalClock(bool) noexcept;
  void SetFreeze(bool) noexcept;             // pins the position anchor — no buffer
  void SetGlobalReverse(bool) noexcept;      // read-head direction
  void Trigger(TriggerSource, float velocity = 1.f, uint32_t sampleOffset = 0) noexcept;
  bool ConsumeOnsetFlag() noexcept;          // drives the trigger LED

  // State — versioned, byte-identical across firmware QSPI blob and plugin chunk
  size_t SaveState(void* dst, size_t cap) const noexcept;
  bool   LoadState(const void* src, size_t bytes) noexcept;  // absent key = version default
  static constexpr uint32_t kStateSchemaVersion = 1;

  // Latency
  uint32_t LatencySamples()        const noexcept { return 0; }  // dry is never delayed
  uint32_t WetOnsetLatencySamples() const noexcept;  // ≈ grain length + block; informational
  uint32_t TriggerLatencySamples()  const noexcept;  // flux 512/256 ≈ 11–16 ms; informational
};

namespace modes {  // non-realtime, host-callable, never from Process
  bool   Compile(const char* json, size_t len, ModeBlob* out, char* err, size_t errCap) noexcept;
  size_t Serialize(const ModeBlob&, char* jsonOut, size_t cap) noexcept;  // editor round-trip
  bool   Validate(const ModeBlob&, ModeError* out) noexcept;
}

struct ScopedDenormalGuard { ScopedDenormalGuard() noexcept; ~ScopedDenormalGuard() noexcept; };
} // namespace brainscape
```

Seam notes. Block size is runtime, full stop — libDaisy's own `AudioHandle::Config::blocksize`
is a runtime field (post-fx doc §7); loop unrolling is recovered by a fixed internal
`kChunk = 16` sub-block that never appears in this header. The `Arenas` seam keeps `dsp/`
free of `DSY_SDRAM_BSS` and every other platform name. `LatencySamples() == 0` is a design
commitment (the wet delay is the musical effect, not compensable); whether the plugin offers
an opt-in "report wet latency" toggle is an open product question.

## 10. Contracts and acceptance tests

These are testable contracts, written before the code:

1. **Block-splitting bit-exactness.** Rendering 4096 frames as `{1, 7, 32, 48, 64, 127, 512}`
   -frame blocks produces identical samples. Forced consequence: **the scheduler's RNG is
   counter-based, keyed on absolute sample index** — never a stateful stream advanced per
   call.
2. **Unity-rate null test.** A mode configured as Pattern A (density 1, ratio 1.0, no spray)
   nulls against a reference delay line to the bit.
3. **Level consistency.** Sine sweep, density 1 vs. max: < 1 dB output variation.
4. **Feedback boundedness.** Full-scale impulse, feedback at max, pitch ±12 st: output
   settles to a bounded limit cycle, never a rail (grain-delay-theory.md §3.10).
5. **Mono compatibility.** Mono sum checked across the full `pan_spread` range (a pedal gets
   summed to mono at the PA).
6. **Mode round-trip.** `Compile → Serialize → Compile` is idempotent; the same mode file
   drives firmware and plugin to identical output for identical input.
7. **Same-state round-trip.** `SaveState → LoadState` restores byte-identical behavior across
   firmware and plugin.
8. **Hardware measurement gates.** DWT cycle counters around every stage before any budget
   figure is treated as real; grain-count stress test (scattered-position worst case) before
   the polyphony ceiling and T0/T1/T2 split are frozen.

## 11. Decisions taken

| Decision | Choice | Why |
|---|---|---|
| Engine structure | One engine, modes as data | microcosm.md §4.5; the entire design bet |
| Phase format | Split 32.32 (int32 + uint32) | 16.16 overflows at 500 ms × ratio 4; float loses precision on long buffers |
| History ring | 16 MiB, 2²² frames, int16 interleaved stereo | undo-capable 2-min looper must coexist (post-fx rec #10) |
| Looper | Two-buffer (A committed + B overdub), 23 + 23 MiB | Microcosm-parity undo (post-fx §4.3) |
| Freeze | Pinned write-head reference | zero cost; coexists with looper (post-fx §5.2) |
| Feedback | Topology A into record buffer + fixed taming chain | grain-delay-theory.md §3.10 |
| Reverb | From-scratch/Clouds-derived Dattorro with input diffusion, 16-bit tank in AXI SRAM | diffusion suits pointillistic input; ReverbSc is LGPL + 4× oversized (post-fx §1) |
| Post chain | mod → reverb → filter default; ordered bypassable list from day one | post-fx §2.3 |
| Block size | Runtime at the API; fixed internal chunk | post-fx §7 resolves the corpus contradiction |
| Firmware sample rate | Pinned 48 kHz for v1; `dsp/` stays rate-agnostic | budget and ring time halve at 96 k |
| Trigger v1 | Spectral flux + whitening + calibration + LED + fallbacks | onset doc recs #1–#8 |
| Mode/preset format | JSON, plain units, stable names, tolerant defaults | preset doc recs #1–#3 |
| Dry latency | 0, dry never block-delayed | grain-delay-theory.md §3.11 |
| Layers per mode | 2 | all 44 variations fit; raising later is a schema break |
| Voices | 64, tiers 8/24/32 + unity path | Clouds precedent + budget |

**License posture (GPLv3):** Clouds (MIT) vendorable with attribution; DaisySP core (MIT)
and CMSIS-DSP (Apache-2.0) fine; `bonk~` (BSD-3) adaptable; DaisySP-LGPL (LGPL-2.1) linkable
but disclosed — though we prefer from-scratch Dattorro; aubio (GPLv3) now vendorable but
desktop-shaped — reimplement flux against CMSIS instead; **Argotlunar is "GPL v2" — verify
v2-only vs. v2-or-later from its source headers before lifting anything** (v2-only is
GPLv3-incompatible → study-only); Essentia (AGPLv3) study-only; GuitarML `DaisyCloudSeed`
glue has no LICENSE file — do not vendor.

## 12. Risks and open questions

1. **Feel.** The data-driven scheduler may not reproduce hand-tuned algorithm feel. No
   architecture retires this — only the mode-curation effort does. (Central risk.)
2. **Cache behavior at 64 scattered streams is the load-bearing unverified assumption**
   (~40-cycle miss penalty, 3.5× SDRAM figure, 16 KB/32 B cache — all carried from the
   corpus, none measured). The stress test comes before any preset is dialled.
3. **16-bit history inside the feedback loop** recirculates quantization noise. Mitigation:
   TPDF dither on write + the saturator bounding the loop. Needs a listening test.
4. **Onset default method**: spectral flux vs. complex-domain for distorted repeated notes
   needs a listening test, not more reading (onset doc rec #3).
5. **`POS_GRID` vs. quantized `POS_LIVE`** — one prototype decides; shrinking the vocabulary
   is worth it.
6. **Modifier slot cap (4) and layer cap (2)** are schema-frozen guesses; validate with the
   stress test before the first community patch ships.
7. **QSPI XIP stall during preset save** (firmware runs `BOOT_QSPI`): a save's erase/write
   may stall instruction fetch. Test early; fallback is the audio ISR in ITCM.
8. **Wet-latency reporting** as an opt-in plugin toggle: open product question.
9. **Mode-population crossfade trails** (up to one grain length of old mode): shipped as the
   default "Trails" style with a fast-cut option; verify it reads as a feature, not a bug.

## 13. Provenance

Synthesized from three independent proposals generated against the research corpus
(2026-08-30): *Tessera* (Microcosm-fidelity lens — position-source vocabulary, step tables,
layers, the 11-effect acid test, Shape-validation and out_trim), *Alluvium* (DSP lens — the
32.32 phase-format correction, tier structure, staging rings, pessimistic budget), and
*STRATA* (contracts lens — block-splitting bit-exactness, counter-based RNG, off-thread mode
compilation with atomic publish, automation semantics, content-hash preset embedding).
Corpus references: [grain-delay-theory.md](../research/grain-delay-theory.md),
[microcosm.md](../research/microcosm.md),
[post-fx-chain-looper-and-system-budget.md](../research/post-fx-chain-looper-and-system-budget.md),
[preset-parameter-and-patch-format.md](../research/preset-parameter-and-patch-format.md),
[onset-detection-on-cortex-m7.md](../research/onset-detection-on-cortex-m7.md),
[vst-and-shared-dsp.md](../research/vst-and-shared-dsp.md),
[daisy-seed-platform.md](../research/daisy-seed-platform.md).
