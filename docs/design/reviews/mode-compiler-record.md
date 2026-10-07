# Mode compiler and presets — evidence record

> The evidence behind [mode-compiler.md](../mode-compiler.md) (draft v2): the inputs, what
> each probe measured and how, where the sources disagreed and how the design resolves it,
> the alternatives it rejected, and how the three reviews of draft v1 were disposed of (§6). The design document is normative; this record is not.
> "Design §N" is the design document, "record §N" this file, "engine §N"
> [grain-engine.md](../grain-engine.md), "companion §N" [companion-app.md](../companion-app.md)
> and "profile §N" [determinism-profile.md](../determinism-profile.md). Code is cited as
> `path:line` at `main` `42773a2`. Numbers are *measured*, *calculated* or *estimated*.

---

## 1. Inputs

### 1.1 How the evidence was produced

All evidence was produced on 2026-10-06 by three independent investigations, each reading the
design documents, STATUS.md and the code at `42773a2`, and each writing only to a scratch
directory. A fourth pass, the writing of the design, re-ran the routing probe (record §2.1)
and checked every cited line against the code. Three reviews of draft v1 followed (engine and
determinism, product and Microcosm fidelity, implementation and tooling), each with its own
probes (§2.8); the revision re-checked their evidence against the code and probe outputs,
re-ran the number and JavaScript probes, and compiled one probe of its own (§2.6).

| Label | Investigation | What it produced |
|---|---|---|
| [inventory] | Requirements inventory for the design: what the schema, macro model, package and app need, and where the documents contradict each other or the code | 43 findings, 16 open decisions; the routing probe |
| [curation] | Microcosm feasibility map and factory-mode curation: all 44 variations against today's engine, recipes for a first set, rendered and measured | the feasibility map, 14 recipes and 4 reserves, the curation probe |
| [tech] | Technical probes: exact binary32 text, a package-format prototype, decoder fuzzing, macro-curve cost, and how the code would sit relative to the gates | number code and its exhaustive tests, a package sketch with decoder, fuzz runs, cost and layout measurements |

### 1.2 Environment

Desktop measurements ran on one 24-thread Windows 11 workstation: MSVC 19.40.33811 (STL 143,
`_MSVC_STL_UPDATE` 202402); GCC 11.4 (Ubuntu 22.04) and GCC 14.2 (Debian trixie) in Docker;
Clang 14.0.0 on Ubuntu 22.04 with libstdc++ 11. Cortex-M7 code was compiled with GNU Arm
Embedded Toolchain 10.3-2021.10 (`arm-none-eabi-gcc` 10.3.1) with the firmware flags, and
cycle estimates used LLVM 14's `llvm-mca` with its Cortex-M7 model. No Mac was available, so
every macOS statement below comes from source reading.

### 1.3 Where the probes live

In the session's scratch directory, under `scratchpad/modes/`: `inventory_probe/` (the
routing probe), `probe/` (the curation probe: `probe.cpp`, `recipes.inc`,
`recipes_draft.inc`, `run1.txt`, `final.txt`) and `tech/` (`bsnum.h`, `numtest.cpp`,
`blob_format.h`, `blob_runtime.cpp`, `blob_compile.cpp`, `blob_libfuzzer.cpp`, `macro_cost.cpp`,
`hazard.cpp`, `chkstk.cpp`, `jsonh_probe.cpp`, run scripts and every `out_*.txt`); the
reviews' `review-engine-determinism/`, `review-product-microcosm/` and
`review-implementation-tooling/`; and the revision's `revise/evalmacro.cpp`. None is
committed. Design §12.4 lane G copies those worth keeping into `tools/parity/modes/`, as the
determinism work did with `tools/parity/`; the number code and the package sketch are the
starting points of lanes A and B.
Lane G did so (2026-10-07): the sources and run scripts, under the same directory names, are in
[`tools/parity/modes/`](../../../tools/parity/modes/) ([`tools/parity/README.md`](../../../tools/parity/README.md),
"Mode-compiler probes"); outputs, binaries and the manual text stayed out.

### 1.4 External sources

- The Hologram Microcosm manual v1.13, pp. 4–13 and the MIDI table on p. 17, fetched from
  Hologram's CDN (`cdn.shopify.com/.../MC_manual_WEB.pdf`); `docs/research/microcosm.md` §2.5,
  §4 and §11–§13.
- libc++'s `__configuration/availability.h` at LLVM `main`, and LLVM 20's libc++ release
  notes, for float `to_chars` and `from_chars` on Apple platforms.
- GCC 11's release notes, and GCC commit r12-6646 (2022-01-17, "libstdc++: Import the
  fast_float library"), for libstdc++.
- cppreference's C++17 support table for MSVC (complete `<charconv>` since 19.24).

## 2. Evidence by design section

### 2.1 The parameter-routing bug (design §7.2)

**Method** [inventory]. `inventory_probe/probe.cpp`, compiled with MSVC directly against
`dsp/src`. For each of `OnsetTrigger` (27) and `PositionSource` (28): a complete preset in
ascending ID order with `Mix` 1, `Overlap` 0.2 (sparse scheduler births), `GrainSizeMs` 60
and, for 28, `OnsetTrigger` 1; an Exact load; 4 s of the test signal's Plucks vector in
48-frame blocks. Three renders: (A) the parameter on in the preset; (B) off in the preset and
turned on by a `SetParam` event at frame 0; (C) off throughout.

**Result** (*measured*, and reproduced unchanged when this record was written):

| Parameter | Onsets (A/B/C) | A vs B | B vs C |
|---|---|---|---|
| `OnsetTrigger` (27) | 8 / 8 / 8 | differ from frame 13,748 | identical over all 192,000 frames |
| `PositionSource` (28) | 8 / 8 / 8 | differ from frame 1,849 | identical over all 192,000 frames |

**Cause.** `Engine::Impl::ApplyParam` (`dsp/src/Engine.cpp:552-580`) handles `Mix`,
`Feedback`, `OutTrimDb` and `TriggerSens` itself and otherwise marks the post chain dirty for
every ID at or above `ModRateHz` (16) and the granular block for the rest (`:569-578`).
`RebuildGranularParams` reads 27 and 28 (`:638-639`), so a change to only one of them sets
`postDirty_`, the granular parameters are never rebuilt, and the change is lost until some
other granular parameter changes. Spillover loads, pedal pots and host automation that change
only these values are affected; a freeze toggle or any granular edit hides the bug.

**Why the golden corpus missed it.** The automation preset's script changes 27 and 28 in
the same events as many granular parameters (`dsp/tests/golden/Corpus.cpp:59-79`), so the
granular block is rebuilt anyway.

### 2.2 Microcosm feasibility and the curation probe (design §11)

**The feasibility map** [curation], against r1 (28 leaves; one layer; a periodic scheduler
with an OR'd onset trigger; live or newest-mark positioning; one pitch value with cents spread
and reverse probability; the window; feedback to 1.1; the post chain; freeze):

| Class | Count | Variations |
|---|---|---|
| Buildable now | 3 | Haze A, Haze B, Warp C |
| Approximate: a defining element missing | 21 | Mosaic A, B, C; Haze C, D; Tunnel A–D; Strum A, B (no decay until W1); Strum C, D; Blocks B, C; Pattern A (repeats late by the FIFO); Pattern B–D; Warp A, B |
| Blocked: the defining mechanism is absent | 20 | Mosaic D; Seq A–D; Glide A–D; Blocks A, D; Interrupt A–D; Arp A–D; Warp D |

Draft v1 counted Strum A, Strum B and Pattern A as buildable (6), but its own first-set table
gave Echolalia and Déjà Vu (Strum A, B) W1's `decay_ms`, because mark grains were measured
never to decay, and Pattern A's repeats drift by the FIFO (below); the product review
corrected the count. Even the buildable ones take Time in free milliseconds instead of a
tempo subdivision, and none has macros yet. **Variations each missing feature gates** (*derived* from the map): macros
all 44; CLOCK with tempo about 20; `decay_ms` and `repeat` about 16; step tables 12; pitch
sets 11; source selection with burst and intermittency 8 or more; modulators about 8; mark
walk 7; per-voice SVF 7; a second layer 5; glide 4; crush 4; `dry_duck` 4. The design's wave
order (design §1.2) follows this, putting the small, high-yield features (W1) first.

**Probe method.** A scratch CMake project built against the main checkout's `dsp/`
(`probe/probe.cpp`, recipes in `probe/recipes.inc`) renders each recipe through
`LoadPreset(Exact)` at the canonical configuration over three integer-generated scores: an
8-pluck phrase, a 4-string strum (strings 25 ms apart) and a 3-tone soft pad with 0.5 s
attacks. It reports dry and wet RMS (wet rendered at `Mix` 1), wet−dry, output RMS, peak, the
tail to −70 dBFS after the input stops, the onset count, and the envelope autocorrelation's
period and peak; a second mode finds tap times on a noise click. All numbers below are
*measured*, r1, static recipes (no macros).

**Results** (`probe/final.txt`; dB, dBFS and seconds; period in milliseconds):

| Recipe | Score | Dry | Wet | W−D | Out | Peak | Tail | Onsets | Period | AC |
|---|---|---|---|---|---|---|---|---|---|---|
| Engram | phrase / chord / pad | −24.8 / −29.7 / −15.8 | −24.8 / −29.5 / −15.7 | 0.0 / +0.2 / +0.1 | −27.3 / −32.1 / −19.1 | −9.9 / −10.9 / −10.6 | 2.35 / 1.50 / 3.00 | 8 / 3 / 0 | 40 / 40 / 210 | 0.88 / 0.86 / 0.87 |
| Callback | | | −24.6 / −28.8 / −16.5 | +0.2 / +0.9 / −0.7 | −27.5 / −32.0 / −18.5 | −10.8 / −11.6 / −9.4 | 3.05 / 2.05 / 3.45 | | 40 / 40 / 55 | 0.89 / 0.83 / 0.71 |
| Retrograde | | | −27.4 / −31.7 / −18.2 | −2.6 / −2.0 / −2.4 | −28.9 / −33.6 / −20.2 | −11.9 / −13.2 / −8.0 | 2.55 / 1.75 / 2.55 | | 40 / 40 / 190 | 0.88 / 0.83 / 0.74 |
| Updraft | | | −26.5 / −31.8 / −17.1 | −1.8 / −2.1 / −1.4 | −28.3 / −33.3 / −19.2 | −11.4 / −12.4 / −6.7 | 3.45 / 2.70 / 3.00 | | 40 / 40 / 340 | 0.87 / 0.84 / 0.17 |
| Pinhole | | | −21.9 / −28.0 / −26.6 | +2.9 / +1.7 / −10.9 | −26.1 / −31.9 / −22.7 | −11.2 / −13.2 / −13.3 | 3.80 / 1.85 / 3.45 | | 40 / 40 / 210 | 0.88 / 0.88 / 0.85 |
| Afterimage | | | −26.6 / −32.1 / −18.4 | −1.8 / −2.4 / −2.6 | −28.0 / −33.4 / −19.6 | −10.9 / −10.3 / −6.7 | 3.00 / 2.20 / 2.15 | | 40 / 40 / 210 | 0.91 / 0.77 / 0.16 |
| Murmuration | | | −24.8 / −30.3 / −17.5 | −0.1 / −0.6 / −1.7 | −26.7 / −32.0 / −18.9 | −9.8 / −11.8 / −6.9 | 5.60 / 5.40 / 4.20 | | 40 / 40 / 40 | 0.83 / 0.83 / 0.14 |
| Halation | | | −25.2 / −30.6 / −16.0 | −0.4 / −0.9 / −0.2 | −26.7 / −31.9 / −17.7 | −9.7 / −11.2 / −5.9 | 4.50 / 3.80 / 3.25 | | 40 / 40 / 40 | 0.88 / 0.89 / 0.34 |
| Undertow | | | −25.4 / −29.8 / −17.1 | −0.6 / −0.1 / −1.3 | −27.3 / −32.1 / −18.8 | −11.2 / −12.2 / −6.8 | 3.10 / 2.25 / 2.45 | | 40 / 40 / 40 | 0.77 / 0.80 / 0.42 |
| Lull | | | −25.7 / −30.2 / −16.9 | −0.9 / −0.4 / −1.1 | −28.2 / −32.9 / −19.3 | −12.4 / −14.1 / −6.6 | 7.55 / 8.10 / 7.75 | | 40 / 40 / 55 | 0.82 / 0.83 / 0.69 |
| Echolalia | | | −27.7 / −26.0 / −24.1 | −2.9 / +3.7 / −8.3 | −31.2 / −31.3 / −24.6 | −14.7 / −10.9 / −15.0 | 7.55 / 8.60 / 1.80 | | 180 / 180 / 175 | 0.90 / 0.90 / 0.69 |
| Déjà Vu | | | −27.3 / −26.4 / −21.3 | −2.5 / +3.3 / −5.6 | −30.4 / −31.6 / −23.6 | −14.5 / −16.2 / −10.8 | 7.55 / 8.60 / 2.05 | | 40 / 40 / 55 | 0.44 / 0.33 / 0.56 |
| Kaleido | | | −27.3 / −26.6 / −18.3 | −2.5 / +3.1 / −2.5 | −28.7 / −30.9 / −19.9 | −12.2 / −12.9 / −7.9 | 7.55 / 8.60 / 2.00 | | 235 / 530 / 60 | 0.21 / 0.22 / 0.75 |
| Runaway | | | −27.6 / −32.2 / −17.8 | −2.8 / −2.5 / −2.0 | −30.1 / −34.9 / −20.8 | −12.9 / −15.2 / −7.3 | 5.70 / 8.05 / 7.50 | | 40 / 40 / 45 | 0.88 / 0.89 / 0.55 |

Dry levels and onset counts are the same for every recipe. From the first run
(`probe/run1.txt`, earlier drafts) and variant runs: Vesper (a Lull through a band-pass at
900 Hz, resonance 0.7) +4.2 / +3.7 / −8.8 dB; Lethe, named Fathom in v1 (sub-octave drone, LP at 2.5 kHz)
+1.0 / +2.2 / −1.5 dB; Shards −3.0 / +1.2 / −3.6 dB with no periodicity; Pinhole's first draft
(1.2 kHz, resonance 0.65) +6.0 / +6.3 / −13.7 dB.

**Findings that shaped the design:**

- **Grain-feedback repeats are late by the FIFO.** Engram (base 375 ms) gave a first tap at
  376 ms and repeats about 386 ms apart, base plus 10.67 ms (the 512-frame FIFO,
  `dsp/include/brainscape/Engine.h:16-22`), smeared slightly by the diffuser: the echoes fall
  at 760.7, 1146.3, 1532.0 and 1917.7 ms against a 750, 1125, 1500, 1875 ms grid, about 43 ms
  behind by the fourth (*calculated*, product review). Callback's post-delay taps are exact:
  251 and 626 ms. Draft v2 therefore re-authors Engram on the post delay; design Q11 asks how
  W2 makes grain feedback tempo-exact.
- **Every recipe is quieter engaged than bypassed.** Output against dry at the stored Mix
  (Out − Dry above): phrase −1.3 to −6.4 dB, strum −1.2 to −5.2, pad −1.9 to −8.8
  (*calculated*, product review, re-checked from the table). Mix is a linear crossfade
  (`dsp/src/Engine.cpp:895-906`): at Mix *m* a decorrelated wet at the dry's level gives
  (1 − m)² + m² of the dry's power, −2.4 dB at 0.35 and −3 dB at 0.5, so a trim cannot close the
  gap without pushing the wet about 5 dB above the dry. Hence design Q13 and the "Engaged"
  pre-screen check. Murmuration and Halation's +2 dB OutTrim masked part of the drop by
  raising the dry too.
- **Mark-positioned grains never decay.** Every recipe with `PositionSource` = Mark ran its
  tail to the end of the render (7.5–8.6 s) at feedback 0.1–0.3, because the source region in
  the ring does not change until the next onset or until the mark goes stale after one ring
  (about 87 s). Feedback cannot shorten it. Hence W1's `decay_ms` and lint L6 (design §2.7).
- **The free-running scheduler never stops**: `allowed ≥ 1` (`dsp/src/Granular.cpp:400-402`),
  so an onset-only mode keeps one voice sounding. Hence W1's source selection.
- **`OutTrimDb` scales dry too**: the output is `(dry·(1 − mix) + wet·mix)·g`
  (`dsp/src/Engine.cpp:895-906`), so per-mode level matching would move the dry level against
  bypass; the Microcosm's Effect Volume is wet-only (manual p. 6). Hence the wet-only trim.
- **Soft pads fire no onsets** at sensitivity 0.5–0.85, so mark modes fall back to `base_ms`
  behind the live input (a tremolo-like delay, period 175 ms for Echolalia's pad). After an
  Exact load the mark ring is empty, so every onset mode starts in that fallback.
- **Sensitivity matters**: the 4-string strum gave 3 onsets at sensitivity 0.6 and 4 at 0.85.
- **Strum A works**: with one voice re-reading the newest mark, the envelope period equals
  the grain size (180 → 180 ms, 90 → 90 ms, autocorrelation 0.90–0.95), and overlap 0.4
  (4 voices) gives 45 ms, so Activity as density works.
- **Repeats as drone decay works**: Lull's tail by feedback: 0.8 → 4–6 s, 0.9 → about
  6–7.5 s, 0.98 → about 8 s and more, 1.05 → endless but bounded.
- **Normalization holds**: most recipes sit within about ±3 dB of dry; band-pass recipes
  depend strongly on the source.

**Recipes** (non-default leaves at r1; macro targets as `leaf [lo → hi]^curve`). These are
lane E's starting points (design §12.4), not final values; under design §3.5 every targeted
leaf is then derived from its stored position. Draft v2's changes: Engram re-authored on the
post delay (the v1 recipe, measured above, used DelayMs 375 and grain Feedback 0.45 with Time
DelayMs [40 → 1500]^2 and Repeats Feedback [0 → 0.92]^1.3); Lull's Activity loses ModDepth and
Pinhole's Shape loses Res, both Shift secondaries (L7); Afterimage and Runaway move to the
reserves; Refrain and Shards join the set. Rows marked "not rendered" have no measurement.

| Mode | Leaves | Macros |
|---|---|---|
| Engram (v2, not rendered) | DelayMs 1, Mix 0.35, Feedback 0, Size 100, Overlap 0, Spray 0, Jitter 0, Sustain 1, Smooth 0, Pan 0, ModDepth 0.05, ModRate 0.6, DelayTime 405, DelayFb 0.45, DelayMix 1, ReverbMix 0.12, ReverbTime 0.4 | Time DelayTime [40 → 1500]^2; Repeats DelayFb [0 → 0.9]; Activity Overlap [0 → 0.45]^2, Spray [0 → 40]^2, Jitter [0 → 0.5], Spread [0 → 8]; Shape Sustain [1 → 0.25], Smooth [0 → 1], Skew [0.5 → 0.8]; Space ReverbMix [0 → 0.5] |
| Callback | DelayMs 250, Mix 0.4, Feedback 0, Size 100, Overlap 0, Spray 0, Jitter 0, Sustain 1, Smooth 0, Pan 0, DelayTime 375, DelayFb 0.5, DelayMix 0.55, ReverbMix 0.12 | Time DelayMs [60 → 1000]^2 and DelayTime [90 → 1500]^2; Repeats DelayFb [0.15 → 0.85]; Activity DelayMix [0.25 → 0.75], Feedback [0 → 0.35]; Shape as Engram; Space ReverbMix only |
| Retrograde | DelayMs 40, Mix 0.5, Feedback 0.35, Size 400, Overlap 0.32, Spray 0, Jitter 0, Reverse 1, Sustain 0.6, Skew 0.5, Smooth 1, Pan 0.3, ReverbMix 0.2 | Time Size [120 → 500], DelayMs [20 → 200]; Repeats Feedback [0 → 0.85]; Activity Overlap [0.26 → 0.5], Spray [0 → 120]; Shape Skew [0.2 → 0.95], Sustain [0.7 → 0.1] |
| Updraft | DelayMs 400, Mix 0.45, Feedback 0.55, Size 120, Overlap 0.45, Spray 8, Jitter 0.15, Pitch +12, Spread 6, Sustain 0.35, Smooth 0.9, Pan 0.6, ReverbMix 0.3, ReverbTime 0.6 | Time DelayMs [150 → 1200]^2; Repeats Feedback [0 → 0.95]; Activity Spray [0 → 80], Overlap [0.35 → 0.6], Spread [0 → 15]; Shape Skew [0.2 → 0.8], Sustain [0.6 → 0.1] |
| Pinhole | DelayMs 330, Mix 0.5, Feedback 0.6, Size 100, Overlap 0, Spray 0, Jitter 0, Sustain 1, Smooth 0, Pan 0, Morph 1, Cutoff 900, Res 0.55, ReverbMix 0.25 | Shape Sustain [1 → 0.4], Skew [0.5 → 0.2] (v1: Res [0.3 → 0.85], a Shift secondary); Repeats Feedback [0 → 0.95]; Time DelayMs [60 → 1500]^2; Activity as Engram v1; Filter sweeps the band, curved so only its last travel reaches bypass (L8) |
| Afterimage (reserve) | DelayMs 30, Mix 0.45, Feedback 0.25, Size 80, Overlap 0.62, Spray 60, Jitter 1, Spread 12, Sustain 0.2, Smooth 1, Pan 0.8, ReverbMix 0.35 | Activity Overlap [0.45 → 0.85], Spray [15 → 300]^2; Repeats Feedback [0 → 0.85]; Shape Size [30 → 250]^1.5, Skew [0.3 → 0.7]; Time DelayMs [10 → 800]^2 |
| Murmuration | DelayMs 300, Mix 0.55, Feedback 0.4, Size 140, Overlap 0.8, Spray 600, Jitter 1, Spread 25, Reverse 0.35, Sustain 0.15, Smooth 1, Pan 1, ModDepth 0.15, ReverbMix 0.5, ReverbTime 0.7, OutTrim +2 | Activity Overlap [0.55 → 0.95], Spray [100 → 1500]^2; Repeats Feedback [0.1 → 0.9]; Shape Size [40 → 400]^1.5, Reverse [0 → 0.5]; Time DelayMs [50 → 2000]^2 |
| Halation | DelayMs 450, Mix 0.5, Feedback 0.35, Size 180, Overlap 0.55, Spray 80, Jitter 0.6, Pitch +12, Spread 8, Sustain 0.3, Smooth 1, Pan 0.8, ReverbMix 0.4, ReverbTime 0.65, OutTrim +2 | Activity Overlap [0.4 → 0.75], Spray [20 → 250]; Repeats Feedback [0 → 0.9]; Shape Size [80 → 300], window; Time DelayMs [320 → 1500]^2 |
| Undertow | DelayMs 120, Mix 0.5, Feedback 0.3, Size 220, Overlap 0.55, Spray 60, Jitter 0.5, Pitch −12, Spread 6, Sustain 0.3, Smooth 1, Pan 0.7, ReverbMix 0.3, OutTrim +1 | Activity Overlap [0.4 → 0.75], Spray [20 → 250]; Repeats Feedback [0 → 0.85]; Shape Size [100 → 400], window; Time DelayMs [20 → 1200]^2 |
| Lull | DelayMs 600, Mix 0.55, Feedback 0.95, Size 450, Overlap 0.6, Spray 150, Jitter 0.5, Spread 10, Sustain 0.2, Smooth 1, Pan 0.9, ModDepth 0.2, ModRate 0.2, ReverbMix 0.55, ReverbTime 0.85 | Repeats Feedback [0.7 → 1.06]^0.8; Activity Spray [20 → 800]^2, Spread [0 → 35], Overlap [0.5 → 0.75] (v1: ModDepth [0.05 → 0.4], a Shift secondary); Shape Size [150 → 500], Smooth [0.5 → 1]; Time DelayMs [200 → 3000]^2 |
| Echolalia | PositionSource 1, OnsetTrigger 1, Sens 0.65, DelayMs 250, Mix 0.5, Feedback 0.15, Size 180, Overlap 0, Spray 0, Jitter 0, Sustain 0.6, Skew 0.15, Smooth 0.5, Pan 0.3, ReverbMix 0.3; wet trim about −3 dB once wet-only | Activity Overlap [0.25 → 0.5], Jitter [0 → 0.3]; Time Size [60 → 500]^1.5; Repeats Feedback [0 → 0.6] (to become `decay_ms`); Shape Skew [0.05 → 0.9], Sustain [0.8 → 0.2] |
| Déjà Vu | PositionSource 1, OnsetTrigger 1, Sens 0.65, DelayMs 250, Mix 0.5, Feedback 0.1, Size 300, Overlap 0.7, Spray 4, Jitter 0.6, Spread 4, Sustain 0.3, Smooth 1, Pan 0.9, ReverbMix 0.4; wet trim about −3 dB | Activity Overlap [0.4 → 0.85], Spray [1 → 25]; Time Size [100 → 500]; Repeats Feedback [0 → 0.7]; Shape window |
| Kaleido | PositionSource 1, OnsetTrigger 0, Sens 0.65, DelayMs 300, Mix 0.5, Feedback 0.3, Size 240, Overlap 0.4, Spray 40, Jitter 0.35, Pitch +12, Sustain 0.5, Smooth 0.8, Pan 0.7, ReverbMix 0.35 | Activity Overlap [0.25 → 0.55] ("loopers"); Time Size [100 → 400]; Repeats Feedback [0 → 0.8]; Shape window |
| Runaway (reserve) | DelayMs 500, Mix 0.5, Feedback 1.05, Size 200, Overlap 0.5, Spray 30, Jitter 0.4, Pitch +12, Sustain 0.3, Smooth 1, Pan 0.7, ReverbMix 0.6, ReverbTime 0.9 | Repeats Feedback [0.6 → 1.1]^0.7; Activity Spray [0 → 200], Spread [0 → 20]; Shape window; Time DelayMs [250 → 2000]^2 |
| Shards | PositionSource 1, OnsetTrigger 1, Size 70, Overlap 0.45, Spray 400, Jitter 1, Reverse 0.5, Pitch +12, Sustain 0.7, Skew 0.1, Smooth 0.2, Pan 1, DelayMix 0.2, DelayTime 250; W1: sources {onset} only | needs onset-only scheduling (W1); macros to author |
| Refrain (not rendered) | W1: live position, set {0, +12} cycle, repeat 4, voice_count 4; DelayMs 250, Mix 0.5, Size 120, Overlap 0.5, Sustain 0.5, Smooth 0.8, Pan 0.7, ReverbMix 0.25 | Activity voice_count [1 → 8]; Repeats repeat [2 → 8]; Time Size [60 → 400]^1.5; Shape window (product review's Mosaic A sketch) |

Pitched recipes keep `base_ms ≥ size·(r − 1) + 1.33 ms` over their macro ranges (the near
guard, engine §3). Overlap maps to voices as 64·overlap³: 0.25 → 1, 0.4 → 4, 0.5 → 8,
0.55 → 11, 0.6 → 14, 0.7 → 22, 0.8 → 33.

**Names to avoid in product strings** [curation]: Hologram's marks and names (Microcosm,
Chroma Console, Infinite Jets, Mosaic, Seq, Glide, Haze, Tunnel, Strum, Blocks, Interrupt, Arp,
Pattern, Warp, Dream Sequence, and, to be cautious, Hold Sampler and Phrase Looper) and other
makers' product names (Mood, Blooper, Habit, Onward; Particle, Tensor; Clouds, Beads; Lore,
Fathom, Fable; Ricochet; Drift). The product review found that v1's reserve name "Fathom" is
Walrus Audio's reverb pedal and that Dream Sequence was missing, so the recall-based list is
not a search: no trademark search was done (design Q3), and `bspc lint` L9 enforces the list
only as a floor.

### 2.3 Exact binary32 text (design §6.4, §6.5)

**The standard library** [tech]:

| Library | Float `to_chars` / `from_chars` | Evidence |
|---|---|---|
| MSVC STL (19.40) | complete since 19.24; exact on every float | *measured* exhaustively |
| libstdc++ 11 | present, built on `strtod` and `uselocale` | *measured* exhaustively (GCC 11.4) |
| libstdc++ 12 and later | `from_chars` uses an embedded fast_float (r12-6646) | *measured* exhaustively (GCC 14.2); GCC 13, used by the Linux CI legs, inferred |
| libc++ on Apple | float `to_chars` gated to a macOS 13.3 deployment target; float `from_chars` first in LLVM 20 and gated to macOS 26.0 | read from `availability.h`; not measured |

So the standard library cannot be the canonical path on macOS, where the plugin today falls
back to `strtof` (`plugin/src/BrainscapeParam.cpp:29-44`).

**The in-house code** (`tech/bsnum.h`, about 330 lines). The reader keeps 120 significant
digits plus a sticky digit (every binary32 midpoint has at most 113 significant digits, so any
input length rounds correctly), multiplies by 5^e for positive exponents and divides by 5^−e
for negative ones in a fixed 640-bit integer of 32-bit limbs with 64-bit intermediates, and
rounds half to even with subnormals; overflow is a range error and anything below 10⁻⁴⁶ is
zero. The writer follows `to_chars`'s "shortest" rule: for 1 to 9 digits it tests the floor and
ceiling candidates exactly against the rounding interval (half-width below powers of two,
endpoints included when the significand is even) and picks the closest valid candidate, the
even digit on a tie. Layout follows ECMAScript's `Number::toString`.

**Results** (*measured*):

| Check | MSVC 19.40 | GCC 11.4 | GCC 14.2 | Clang 14 |
|---|---|---|---|---|
| All 4,278,190,080 finite floats written and read back | 0 mismatches (1,162 s, 24 threads) | 0 (1,010 s) | 0 (984 s) | 0 (543 s) |
| FNV-1a of all written text | `b9dddab197d0eb00` | same | same | same |
| Digits equal to `std::to_chars` shortest | all | all | all | not compared |
| `std::from_chars<float>` reads the text exactly | all | all | all | not compared |
| Halfway cases at stride 1009 (13,122,714, up to 269 characters) | 0 mismatches | 0 | 0 | 0 |
| 20,000,000 random strings, hash of results | `61fd33dc7b73fcd1` | same | same | same |
| Write / read time per value, one thread | 1.14 / 0.22 µs | 1.00 / 0.24 µs | 0.91 / 0.19 µs | 1.15 / 0.21 µs |

MSVC additionally read 1,889,845,423 halfway cases at stride 7 in 190.7 s with no mismatch,
for the in-house reader and for `std::from_chars`. Edge cases behaved as specified, for
example `0.55` → `0x3F0CCCCD` → "0.55", `1e-46` → 0, `7.1e-46` → the smallest subnormal,
`3.40282356e38` → `FLT_MAX`, `340282356779733661637539395458142568448` → range error,
`0.30000001192092896` → "0.3", and `1.`, `.5`, `+1` and `1e` rejected.

**Double rounding.** Reading the shortest text as binary64 and then rounding to binary32 (as
JavaScript's `JSON.parse` plus `Math.fround`, Python or newlib's `strtof` do) gives the wrong
float for exactly two of the 4.28 × 10⁹ values: `0x15AE43FD` and `0x95AE43FD`,
±7.038531e-26, on MSVC, GCC 11 and GCC 14. This confirms companion §6.4's earlier
measurement. The prototype has no special case for them (`tech/bsnum.h` is pure shortest),
so its hash `b9dddab197d0eb00` is a pure-shortest writer's. Draft v1 said they "get 9
significant digits" without saying which; the implementation review showed 8 suffice: the
exact value is 7.03853069185…e-26, and `7.0385307e-26` reads back to `0x15AE43FD` both under
correct binary32 rounding and through binary64, while `7.038531e-26` reads back to
`0x15AE43FE` through binary64 (`review-implementation-tooling/exact_check.py`, re-run for the
revision). Design §6.4 now specifies `7.0385307e-26` and its negative.

**JavaScript.** Draft v1 claimed a JavaScript client prints the same text through
`Number.prototype.toString`. It does not: JavaScript formats the binary64 value of the float,
so `Math.fround(JSON.parse("0.55"))` prints `0.550000011920929`, 0.35 prints
`0.3499999940395355` and `3.4028235e+38` prints `3.4028234663852886e+38`
(`review-implementation-tooling/js_probe.js` under Node, re-run for the revision). Reading is
exact once the two values above are written with eight digits.

**Thread dependence.** The 20-million-string and halfway tests split work by
`std::thread::hardware_concurrency()` and chain per-thread hashes (`tech/numtest.cpp:45`,
`:284-337`), so their hash depends on the core count: `61fd33dc7b73fcd1` at 24 threads,
`a5ec77aca40696d0` at 4, `46862378f8b1a792` at 3, `756c88de63896363` at 2
(`review-implementation-tooling/out_hash_vs_threads.txt`). The exhaustive round trip uses 256
fixed chunks and gave `0530292719017104` at step 4099 for both 24 and 4 threads. Every "same
on four toolchains" result above came from the one 24-thread host (Docker reports 24 too), and
no AArch64 or AppleClang build of the number code has run. Per-pull-request cost on 4 CPUs:
about 8 s in Release, 92 s in Debug with AddressSanitizer and UBSan
(`out_number_cost.txt`).

**On the M7** (`tech/out_arm.txt`): `bsnum` compiles to 5,416 bytes of code with no
floating-point arithmetic instruction (one floating-point select, `vseleq.f32`, plus moves,
loads and stores) and imports only `__aeabi_uldivmod`, `memcpy` and `memset`. The number code
could therefore run on the pedal under emulation, the precondition for on-pedal compilation
(companion §6.6).

**Nightly cost.** The exhaustive round trip of the in-house code alone takes about 3.6
CPU-hours (*estimated* from the runs above, which also ran the `std` cross-checks), about
55 minutes on a 4-vCPU runner.

### 2.4 The golden harness's JSON reader (design §6.5)

`tech/jsonh_probe.cpp` fed `dsp/tests/golden/Json.h` (MSVC) a set of inputs (*measured*):

| Input | Result |
|---|---|
| `0.55`, `1e3` | rejected: integers only, no float node and no number text kept |
| `007`, raw CR or 0x01 in a string, invalid UTF-8 byte 0xFF | accepted (none is valid JSON) |
| a duplicate key | accepted; both kept, `Find` returns the first |
| `\r`, `\b`, `é` escapes | rejected (all valid JSON) |
| integer 2⁶² + 1 | rejected |
| 33 nested arrays; trailing comma | accepted; rejected |

Errors carry only a byte offset, and the writer keeps insertion order and inlines scalar
arrays, a golden-file layout. The reader is integer-only and builds for the M7 oracle, which is
why it stays as it is and the compiler gets its own strict module.

### 2.5 The package prototype, layout hazards and fuzzing (design §5, §6, §8.3, §10.2)

**Package sketch** (`tech/blob_format.h`, `blob_runtime.cpp`, `blob_compile.cpp`): a 96-byte
header, then STAT (sorted leaves and the performance state) and a fixed-capacity MODE of
988 bytes (2 layers, 16 pitch entries, 8 macros, 32 targets), every unused slot and pad zero,
fields written one by one. The design keeps its principles and changes its shape: a 128-byte
header with `control_hash`, MODE as chunks (so a feature can land without a `blob_format`
bump), 8 pitch entries per layer, and CTRL and META sections.

| Measurement | Result (*measured*) |
|---|---|
| Three sample packages, 1,368 bytes each | SHA-256 `8b6c8efc…dd55`, `54198828…89f9`, `99746476…4a6f`, identical on MSVC 19.40, GCC 11.4, GCC 14.2 and Clang 14 |
| Decode, then re-encode | identical bytes for all three |
| Decoded struct (2,068 bytes) `static_assert`s | hold on x64 and on the M7 build |
| Decoder, validator and SHA-256 on the M7 | 4,775 bytes of code, no floating-point instruction, imports `memcpy` and `memset` only |
| Decoder on MSVC x64 | imports only `__GSHandlerCheck`, `__security_check_cookie`, `__security_cookie`, `_fltused` and `memset`, all on the symbol audit's allowlist; byte comparisons must be loops, since `memcmp` is not |

**Layout hazards** (`tech/hazard.cpp`): one struct `{bool; enum without a fixed underlying
type; size_t; long; float; void*}` is 20 bytes on the M7, 32 on MSVC x64 and 40 on Linux x64.
A plain enum is 1 byte on `arm-none-eabi` (short enums) and 4 on x64; `long` is 4 / 4 / 8,
`wchar_t` 4 / 2 / 4 and `long double` 8 / 8 / 16 (M7 / MSVC / Linux); `enum class : uint8_t`
is 1 byte everywhere.

**Fuzzing** (*measured*):

- A deterministic mutation fuzzer (bit flips, byte sets, interesting 32-bit values,
  truncation and extension, hashes fixed up on three iterations in four) ran 3,000,000
  iterations in 103 s under GCC 14.2 with AddressSanitizer and UndefinedBehaviorSanitizer, with
  no sanitizer report. It reached every reject path (section bounds 1,253,769; padding 431,978;
  total length 430,026; package hash 280,484; truncation 105,083; MODE non-finite 69,235; STAT
  non-finite 59,378; STAT order 122,131; and the rest) and accepted 81,971 packages, every one
  of which re-encoded to the same `sound_hash`. The check compared only `sound_hash`, the
  re-encode used a fixed JSON section (`tech/blob_compile.cpp:273-277`), and the layout was the
  fixed-capacity sketch, not draft v2's chunks; design §5.2 cites it accordingly and lane B
  re-runs it on the chunked layout, comparing CTRL bytes and carried sections.
- libFuzzer (Clang 14, both sanitizers, with and without a hook that skips hash checks, and
  with invariant traps on index ranges) ran 8,906,130 inputs in 181 s, added 221 coverage
  units and found no crash.
- The M7's 32-bit `size_t` is not exercised by host fuzzing; the decoder checks every length
  against the remaining bytes before using it, and design §10.2 adds the fuzzer to the M7 leg.

### 2.6 Macro-curve cost and monotonicity (design §3.3)

`tech/macro_cost.cpp` evaluates one target as the design specifies, with DetMath's `PowF`.

| Measurement | Result |
|---|---|
| One target, M7, `llvm-mca` (LLVM 14) | 212 instructions (68 binary64 operations, 2 divisions), 538 cycles; 53,701 cycles per 100 in steady state (*estimated*; untaken branches are counted, so slightly high) |
| A 257-point table with linear interpolation, same model | 15 instructions, 31 cycles |
| Static instruction counts, GCC 10.3 for the M7 | evaluator 30, `PowF` 61, `Log2D` 8, `LogD` 125, `Exp2D` 69, exponential kernel 53 |
| Host, MSVC | 26.9 ns per target (*measured*) |
| Monotonicity, every 12-bit pot code, exponents 1/16 … 16 (9 values) | 0 non-monotonic steps, 0 endpoint misses (*measured*) |
| Monotonicity, every binary32 position in [0, 1], exponents 1.5 and 3.0 | 0 non-monotonic steps (*measured*) |

At 480 MHz, 538 cycles is about 1.1 µs; an 8-target move is about 4,300 cycles, about 0.9 %
of a 48-frame block (10,000 cycles per sample, engine §8), and all knobs moving every block
with 4 targets each would be about 3.6 % while they move (*calculated*). Tables would cost
1 KiB per target, up to 32 KiB per mode, and would need DetMath in the compiler.

**The example's values** (`revise/evalmacro.cpp`, MSVC 19.40 against `dsp/src/DetMath.cpp`,
inside the guard; *measured*). Draft v1's Engram example stored leaves that its own positions
do not produce: Time 0.479 on [40, 1500]^2 gives 374.983856 ms (`0x43BB7DEF`), not 375;
Repeats 0.577 on [0, 0.92]^1.3 gives 0.450107694, not 0.45; and Space 0.12 through the default
Space macro gives `post.delay.mix` 0.06, not the stored 0 (the product review found all
three). Draft v2's example: `PowF(0.5, 2)` is exactly 0.25, so Time 0.5 on [40, 1500]^2 gives
405 exactly; Repeats 0.5 on [0, 0.9] gives `0x3EE66666`, the bits of 0.45; Space 0.24 on
[0, 0.5] gives `0x3DF5C28F`, the bits of 0.12. The probe uses design §3.3's linear fast path
(`curve == 1` skips `PowF`), which draft v2 adds so that linear targets are exact and cost a
few dozen cycles instead of 538.

### 2.7 Stack frames and `__chkstk` (design §5.1)

`tech/chkstk.cpp` compiled with MSVC x64: a function with a 4,800-byte frame imports
`__chkstk`, which the symbol audit rejects; one with 3,600 bytes does not (*measured*,
`dumpbin`). A `PresetState` (about 2.6 KiB, design §5.1) plus other locals could cross the
4 KiB page, so it is never a `dsp/` stack local.

### 2.8 The reviews' probes and checks (design §3.6, §4.5, §7.5, §8, §11.3)

Each review wrote its probes under `scratchpad/modes/review-*/`; the revision re-read the code
each one cites at `42773a2` and found it as described.

| Probe or check | Result |
|---|---|
| `review-engine-determinism/keyalias.cpp` (MSVC): v1's key extension, `ext << 56` XOR-ed before `DrawKey`'s 32-bit fold (`dsp/src/detail/GrainMath.h:44-48`) | 918,184 of 918,184 sampled (frame, purpose) pairs aliased for each of layer (±2²¹ frames, 43.7 s), ordinal (±2²² frames) and high purpose bits (±2²⁵ frames, 11.65 min); 0 same-frame collisions (*measured*) |
| `review-engine-determinism/gate_probe.py` against `tools/ci/sound_rev_gate.py` | `golden_changes` returns `[]` when only a preset's `soundHash` changes: the gate reads `hash` and `secondHashes` only (`:71-92`) (*measured*) |
| Golden ablations under the wet kill (`dsp/tests/golden/Corpus.cpp:448-455`, `golden_main.cpp:414-425`) | `post_max` sits at the 40 Hz minimum; with the wet gain exactly 0, the output is dry·(1 − mix) + (±0) whatever the post chain does, so three post-stage ablations would "change nothing" and mint would refuse (*derived* from the code) |
| `review-implementation-tooling/audit_probe_fp.cpp`, `audit_probe_int.cpp` (MSVC, `dumpbin /symbols`) | an object calling float `std::to_chars`, `std::from_chars`, `snprintf("%g")` and `std::to_string(float)` imports no symbol naming them, only `?_Large_power_data@std@@3QBIB` and `__stdio_common_vsprintf(_s)`, which the integer-only object imports too (*measured*) |
| `review-implementation-tooling/comdat_probe.cpp` (MSVC) | two `bsc` functions indexing `brainscape::kParamTable` at run time emit a COMDAT definition of it (`?kParamTable@brainscape@@3QBUParamDescriptor@1@B`) in the `bsc` object (*measured*) |
| `review-product-microcosm/engram_check.py`, `outdelta.py` | Engram FIFO drift and the Out − Dry deltas (record §2.2); feedback *g* raises a sustained input's energy by 1/(1 − g²): +7.2 dB at 0.9, +8.1 dB at 0.92 (*calculated*) |
| JUCE 9.0.3 sources (read by the product review) | VST3 reports parameter changes through `performEdit` or `outputParameterChanges` (`juce_audio_plugin_client_VST3.cpp:1470-1485`, `:3615-3626`) and sends `kParamValuesChanged` only from `setComponentState` and program changes (`:1197`, `:1535`); `juce_AudioProcessorParameter.h:51-98` documents Logic's index-based AU parameter identity and ordering by version hint, then string-ID hash. The plugin passes hint 1 everywhere (`plugin/src/BrainscapeParam.cpp:245`, `:311`). Not re-read by the revision: the JUCE tree was not on disk |
| Plugin text parsing (`plugin/src/BrainscapeParam.cpp:47-74`, `:226-230`) | accepts `.5`, `5.` and `05` mantissas and composes `mantissa + "e" + exponent`; the strict reader rejects `.5e0` and `5.e0` and stops after the `0` of `05e0` (`tech/out_gcc14.txt`) |
| `dsp/tests/golden/Sha256.h:43-54` | `Hex()` uses `std::snprintf` and `std::string`, so the header cannot move whole into the firmware archive; it is included by `golden/Render.cpp:9`, `test_state.cpp:21` and `plugin/tests/plugin_tests.cpp:37` |
| `kNumParams` consumers (`git grep` at `42773a2`) | beyond lane C's v1 file list: `plugin/src/PluginProcessor.cpp:24` (`static_assert(kNumParams < 32)`, a 32-bit touched mask), `PluginProcessor.h`, `StateCodec.*`, `PluginEditor.cpp`, `Audition.cpp`; `dsp/tests/golden/{Corpus,EventScript}.cpp` (leaf *i* = row *i*); `dsp/tests/test_{engine,param_display,state,boundary,detmath}.cpp`, `render_main.cpp`; `plugin/tests/` |

## 3. Where the sources disagreed, and how the design resolves it

Stable numbers; the design cites items 2, 4 and 7. Items 26–29 come from the reviews of
draft v1.

1. **Scope of merged step 3.** STATUS.md's step 3 is the pipeline only; companion §8.1 adds
   the engine features factory modes need, "above all" CLOCK; profile §8.4 adds macro and
   expression curves. *Resolution:* step 3 is the pipeline (3a) and W1; W2 (CLOCK) runs
   during step 4; W3 lands only as factory modes need it (design §1.2). W1 precedes CLOCK
   because it is small, completes the 7 of the first 14 modes that the pipeline alone does not
   (the other 7 need only macros), and fixes two measured defects; the owner may reorder
   (design Q2).
2. **Leaf list against macro targets and modifier slots.** Engine §6's leaf list omits values
   its own acid test makes macro targets (`steps.count`, `layer_mix`, `glide.curve`, burst,
   modulator depth), and its modifier leaves are op-generic `p0..p3`, which cannot carry one
   frozen range, unit and taper. *Resolution:* all of those become leaves; modifier leaves are
   per op (`svf.cutoff_hz`, `crush.bits`); engine §5's sub-octave op is a pitch-set entry,
   glissando is glide, and per-grain gain is the step's `gain` (design §2.4, §4.2).
3. **IDs 4, 8, 18, 26, 27, 28** against the design's leaf list (companion §5.7). *Resolution:*
   4 becomes `wet_trim_db`, wet only (the Microcosm's Effect Volume; whole-output gain belongs
   to the global Output level, companion §4.8); 8 becomes `transpose_st`, which keeps r1's bits
   for the default pitch set; 18 keeps milliseconds, with a separate discrete `post.delay.sync`;
   26 stays a preset leaf with a global calibration offset outside presets (companion §6.2
   excludes calibration, but modes need different thresholds, record §2.2); 27 and 28 retire
   into structure (design §4.2).
4. **Compile-time ratios against birth-time semitones.** Engine §5 has `Compile` resolve
   semitones to ratios; the code computes `Exp2F((st + detune)/12)` at birth
   (`dsp/src/Granular.cpp:101-110`). Ratios in the blob would put DetMath into the compiler
   (making compiler code part of the sound) and make detune multiply instead of add, changing
   every pitched preset. *Resolution:* the blob stores semitones (design §7.5).
5. **Random-number keys.** Engine §9 keys draws on (sample, layer, burst ordinal, purpose);
   the code keys on (sample, purpose) with all 8 purposes used (`dsp/src/detail/GrainMath.h:26-48`).
   Re-keying everything is cheap before publication but changes every jittered golden hash.
   *Resolution:* an extension that leaves every r1 key unchanged and, when nonzero, re-hashes
   the folded key with `Hash32`; v1's XOR into the high bits aliased across frames (record
   §2.8, design §7.5).
   Bursts with spacing 0 fire on consecutive frames, as same-frame triggers already do, so
   they need no ordinal; the ordinal serves step entries sharing a slot.
6. **Filter endpoints.** Engine §2 wants the endpoints "as data on the Filter macro, not
   hardwired"; the code hardwires bypass at the descriptor maximum
   (`dsp/src/Engine.cpp:593-597`) and has no wet kill. *Resolution:* both endpoints are leaf
   semantics (maximum = bypass, minimum = wet kill at the end of the chain), and whether the
   knob reaches them is the Filter macro's range, which is data (design §2.3, §4.2). A kill
   through a macro target on the trim was rejected because it would overwrite the mode's
   level match. Draft v2 also separates the player's Effect Volume from that level match
   (item 26).
7. **Schema fields the engine does not have.** Engine §5's example names `post.bypass`,
   `feedback.hp_hz`, `.lp_hz`, `.diffusion`, the source `stochastic`, `size_law` and
   `dry_duck.enabled`; the code has a fixed taming chain (HP 100 Hz, a feedback-dependent LP,
   two allpasses per channel where the diagram says four, `dsp/src/detail/PostChain.h:195-216`),
   stages exactly transparent at their neutral leaves, and jitter 1 already drawing exponential
   intervals (`dsp/src/Granular.cpp:412-420`). *Resolution:* those fields are removed from
   schema 1 (design §2.3), the feedback filter as a possible later additive leaf. `d_min_fb`
   is dropped, because the 512-frame FIFO already makes every feedback loop at least 10.67 ms
   long (design §2.7).
8. **Resolve once at birth against modulation during a grain's life.** Engine §2 commitment 1
   versus the acid test's sweeps on long grains, and engine §4's "block-rate" routes.
   *Resolution:* `cutoff` routes act per sample on the current mode's voices, the rest at
   birth, nothing per block (design §2.5). This also lets the mode ring go (item 15).
9. **Which knobs are macros, and Time.** Engine §6 lists 8 macros, makes Space a fixed
   fan-out and Filter a fixed mapping, and gives Time a dual mode, while its §5 example has a
   per-mode `time` macro. *Resolution:* Activity, Repeats, Shape, Time, Space and Filter are
   per-mode macros, Space and Filter with defaults (Callback needs Space on the reverb only);
   Mix is a global leaf; two auxiliary macros complete eight; Time drives tempo in CLOCK modes
   (design §3.1).
10. **Tapers.** Companion §5.4 specifies log and offset-log tapers; the code uses power tapers
    so that no transcendental is needed (`dsp/include/brainscape/ParamDisplay.h:112-118`).
    *Resolution:* power tapers (design §4.3).
11. **"Sound-neutral" new leaves.** Companion §6.1 lets a leaf with a neutral default land
    sound-neutral; profile §5.12 bumps for any change that can alter output. Separately, a
    package lacking a new leaf would load as inexact. *Resolution:* profile §5.12 wins; a
    `sinceRev` column keeps older packages exact (design §7.3, §7.6).
12. **The compiler's location against the sound-revision gate.** Companion §3.1 puts it under
    `dsp/src/compiler`, which the gate's path trigger covers (`tools/ci/sound_rev_gate.py:29-34`).
    *Resolution:* a top-level `compiler/`, gated by `bspc-roundtrip`, the package rule and
    CODEOWNERS (item 27); namespace `bsc`, with the symbol scan scoped to functions (design
    §8.1).
13. **`fast_float` against in-house code.** Companion §3.5 and §6.4 name `fast_float`; the
    owner approves dependencies one at a time. *Resolution:* in-house reader and writer,
    verified exhaustively; the dependency is recorded as an owner decision, recommended
    against (design §6.5, Q1).
14. **Tempo in `ProcessContext` against stamped events.** The engine reserves `tempoBpm`,
    `timelinePos` and `transportPlaying` (`dsp/include/brainscape/Engine.h:172-175`);
    companion §8.1 requires tempo as stamped events. *Resolution:* events only; the engine never
    reads those fields (design §7.4).
15. **A 4-slot mode ring against one active mode.** Engine §5 specifies a ring with epochs,
    reclamation and `PublishMode`. *Resolution:* grains resolve everything at birth and
    during-life routes skip older grains, so the engine copies the staged mode at the load
    frame and keeps one; the ring, its race and 12 KiB of AXI go (design §7.3). Because the
    engine no longer owns a published, validated slot, it validates every staged mode itself
    and decides "same mode" by comparing contents, not `modeHash` (draft v2).
16. **Mode file against preset document.** Engine §1 and §5 speak of 44 mode files and of
    presets storing a mode by hash plus an inline copy; companion §6.6 compiles one JSON per
    preset. *Resolution:* one document per preset with the mode embedded; `modeHash` groups
    presets that share a mode (design §1.4, §6.3).
17. **Macro curve shape.** The research specifies `lin|exp|log` plus `curve_amount`
    (preset-parameter-and-patch-format.md §4.2); engine §6 folds both into one exponent.
    *Resolution:* one power exponent in [1/16, 16]; a true exponential law can be added later
    (design §3.3).
18. **Where macros are evaluated, and conflict order.** The research evaluates macros inside
    `SetParam` with last write per audio block (§4.3, §7.3); profile §5.11 makes a macro move
    its own event ordered by frame and sequence. *Resolution:* the profile's (design §3.4,
    §3.6).
19. **Serializing from the blob.** Engine §9's `Serialize(const ModeBlob&)` cannot reproduce
    editor-only data unless the blob stores it; companion §6.3 carries the JSON as its own
    section. *Resolution:* decompiling returns the JSON section and rebuilds from bytes only
    when it is absent or stale (design §8.2, §10.1).
20. **`Validate(blob, sampleRate)`** in engine §9, while the engine runs only at 48 kHz.
    *Resolution:* no sample-rate argument (design §5.3).
21. **Promoting the golden JSON reader.** Considered for the compiler; its probe (record §2.4)
    shows it accepts invalid JSON and has no floats. *Resolution:* left alone; the compiler
    gets its own module.
22. **Unknown sections and chunks.** The tech sketch skipped unknown sections; skipping inside
    MODE would silently ignore a feature. *Resolution:* unknown top-level sections are skipped
    and kept; unknown MODE chunks are rejected as `UnsupportedFeature` (design §5.2, §6.1).
23. **A mode-switch event against a Spillover load.** Profile §5.11 lists both. *Resolution:*
    a mode change is a preset load; the Spillover event's `id` selects Trails or FastCut, and
    "knobs follow" reproduces the Microcosm's behaviour of applying current knob positions
    (design §3.5, §7.3).
24. **Macro curves from tables or computed per move.** *Resolution:* computed with DetMath
    per move: about 538 cycles per target (*estimated*), no table memory, no DetMath in the
    compiler (record §2.6).
25. **Integer-valued leaves.** Companion §5.5 forbids a value grid; macros produce fractions
    for counts such as `voice_count`. *Resolution:* stored raw, read by the engine as
    `RoundHalfAwayI32` after canonical clamping (design §3.7).
26. **One leaf for the level match and the Effect Volume** (product review). Draft v1 mapped
    Shift+Mix to `wet_trim_db` and made the same leaf the curator's per-mode level match, so a
    player's turn erased the match and every load reset the player's level. *Resolution:* two
    gains: the trim stays a per-mode leaf off the panel, and Shift+Mix drives
    `global.effect_volume_db` (82), a device setting; one function computes the wet gain from
    both and the cutoff (design §3.8, §7.2).
27. **What certifies a document's meaning** (engine review). Draft v1 moved the compiler out
    of the path trigger and said a compiler change altering a corpus package needs a bump, but
    the gate never compares package hashes (record §2.8), and `kSoundRevision` certifies
    `dsp/` only (profile §5.12). *Resolution:* the package rule: an audio-hash change counts as
    an engine change only when the preset's package is unchanged, and a changed package needs
    a CODEOWNERS-approved `package-change` label (design §8.3).
28. **Reporting macro fan-out to hosts** (product review). Draft v1 promised a display-only
    report and a warning about lanes on targeted leaves; JUCE 9.0.3 has no display-only path
    and no format exposes lanes (record §2.8). *Resolution:* the owner picks one of two host
    models before the release gate, (b) recommended (design §3.6, Q12).
29. **What justifies "sound-neutral"** (engine and implementation reviews). Draft v1's
    pipeline pull request rested on golden hashes reproducing, the criterion profile §5.12
    rejects, and could not be built before the ID table it needed. *Resolution:* an ID-table
    pull request first; neutral pull requests are justified by construction and golden hashes
    are a check (design §7.6).

## 4. Alternatives considered and rejected

- **A fixed-capacity MODE layout** (the tech sketch). Simpler, but every feature would bump
  `blob_format` and the firmware would keep a decoder per step; chunks evolve additively.
- **Re-keying all random draws** in one bump. Clean, but every jittered golden hash would
  change at once, losing the check that r2's re-mint changes only what it should.
- **Putting modifier parameters in op-generic slots** (engine §6). Hosts could not show a
  frozen unit or taper.
- **Writing defaults out of canonical JSON.** Shorter files, but a default changed before
  step 6 would silently change what an old document means; leaves are always written.
- **Separate mode files referenced by presets.** Reuse, but compilation would read several
  files and packages would depend on a library's contents.
- **Pre-computed macro tables** in MODE (record §2.6).
- **Vendoring `fast_float`** (record §2.3; design Q1).
- **Treating `STOCHASTIC` as a separate Bernoulli source.** Jitter 1 already gives
  exponential intervals with the same self-limiting ceiling.
- **`Global` rows for the Mix lock and the default expression assignment** (product review's
  form). Both act only at load, and an assignment is a record, not a float; the producer
  applies them before staging and recipes record the staged state (design §3.8).
- **An `extern` parameter table**, to keep `symbol-scan`'s rule literal (implementation
  review's first option). Consumers use the table and `kNumParams` at compile time; scoping the
  scan to functions loses nothing, since constant data carries no floating-point flags.
- **A `kSoundRevision` bump for compiler changes** (draft v1 §8.3). The revision certifies the
  engine (profile §5.12); the package rule gives the visibility (item 27).
- **FIFO-compensated grain feedback for Engram now.** Feedback re-enters through a causal
  512-frame FIFO, so compensation needs engine work (design Q11); the post delay is exact
  today.
- **A symmetric ±1.5 dB "engaged against bypass" window** (product review). Under the
  recommended Mix law a decorrelated wet at the dry's level adds +1.7 dB at Mix 0.35 and
  +3.0 dB at 0.5 (*calculated*), so the window would fail healthy modes; design §11.3 allows
  −1 to +4 LU, which keeps the point: engaging never takes level away.

## 5. Provenance

Written 2026-10-06 from the three investigations of record §1, on branch
`claude/mode-compiler-design` at `42773a2`, and revised the same day into draft v2 after three
reviews (§6). The design document's open questions (design §12.3) and the amendments it lists
(design §12.5) are to be settled when it is accepted.

## 6. Review of draft v1 and its disposition

Three reviews read draft v1 and this record: **[E]** engine and determinism, **[P]** product
and Microcosm fidelity, **[I]** implementation and tooling. The revision checked each
finding's evidence against the code at `42773a2` and the probe outputs (§2.8), re-running the
number and JavaScript probes. Every finding was applied; two were applied in another form
than proposed and are explained in §4 (P1's threshold, P12's form); where a review offered
options, the choice is noted. No finding was rejected. One change is the revision's own: the
linear fast path in `EvalMacro` (§2.6).

| # | Finding | Disposition (design §) |
|---|---|---|
| E1 | Load and apply paths ignore row kind | applied: per-kind rules (4.1), load steps (7.3), `SetParam` scope (7.4), tests (10.4) |
| E2 | `LoadPreset` trusts the staged `ModeBlob` and `modeHash` | applied: validation at every load, content comparison, indices reduced, r1-equivalent default `ModeBlob`, corpus structure only by encode and decode (4.4, 5.1, 7.3) |
| E3 | The no-bump pipeline pull request cannot be built | applied with I1: ID-table pull request first, compiler with default structure only, packages from r2, neutrality by construction (7.6, 12.4) |
| E4 | A single-valued `domain` cannot express the cutoff kill | applied: domain bitmask, `WetGainTarget`, lone change per `Leaf` row, kill cases (4.1, 7.2, 10.3) |
| E5 | The gate never compares `sound_hash` | applied, second option: the package rule; the bump claim withdrawn (8.3; §3 item 27) |
| E6 | The r2 re-mint fails on `post_max` | applied: `post_max` to 41 Hz, a dedicated wet-kill case (7.6, 10.3) |
| E7 | Round-trip evidence from another format; no canonical chunk rules | applied with I9: chunk rules, invariant scoped to STAT, MODE, CTRL, fuzz re-run required, unknown-section placement (5.2, 5.3, 6.1) |
| E8 | The key extension aliases after the fold | applied: `Hash32` re-key for `ext` ≠ 0, corrected text, offset test (7.5, 10.4) |
| E9 | `Engine::Impl` growth understated | applied: growth per wave, a raise per wave, per-grain `fadeStart` (7.1, 7.3) |
| P1 | Every mode is quieter than bypass; nothing checks it | applied, threshold changed (§4): "Engaged" check, Mix law as Q13 with its own bump, re-measurement before rating (7.1, 7.6, 11.1, 11.3) |
| P2 | One leaf is both level match and Effect Volume | applied, Global option: `global.effect_volume_db` (82) (3.1, 3.8, 4.2) |
| P3 | Pickup jumps where stored leaves disagree with positions | applied: derived leaves, "solve position", `bspc derive`, L4 with tolerance and factory error, example fixed (2.1, 3.5, 8.2) |
| P4 | Display-only fan-out has no mechanism | applied: host model as owner decision Q12, (b) recommended; lane warning dropped; recording test (3.6, 10.4) |
| P5 | No Mosaic and no glitch mode | applied: Refrain and Shards replace Afterimage and Runaway; one keeper per family; echoic Time ratings provisional (11.1, 11.3) |
| P6 | Engram drifts; "buildable now" overstated | applied, option (b): Engram on the post delay; count corrected (11.1; §2.2) |
| P7 | No fast edit-and-listen loop | applied: the curation slice, sized, a prerequisite of the rated pass (9.1, 11.3, 12.4) |
| P8 | Ratings keyed by `sound_hash` | applied with I13: keyed by `id`, render hashes, carry-forward (11.3) |
| P9 | Pre-screen thresholds contradict the data | applied: K-weighted loudness per input class, Repeats rule, looped input, sweeps from stored positions (11.3) |
| P10 | Adding host parameters is not additive for AU | applied: version hints and numeric IDs in the manifest, Q17 in the gate (4.5) |
| P11 | Knob combinations never rendered | applied: script S11 (11.3) |
| P12 | Mix and expression lost their device-level fallback | applied in another form (§4): producer-applied device settings (3.4, 3.8) |
| P13 | Shift secondaries collide with macro targets; endpoints unchecked | applied as lint L7 and L8, errors for factory; Lull and Pinhole re-mapped (2.7, 3.1; §2.2) |
| P14 | "Fathom" is taken; avoid-list gaps | applied: Lethe, Dream Sequence and Fathom listed, L9 denylist, Q3 covers knob names (11.1, 11.2; §2.2) |
| I1 | Circular merge order | applied with E3 (7.6, 12.4) |
| I2 | Lane file lists wrong | applied: lane table from a search, lane 0 owns every `kNumParams` consumer, render extracted first, F's spans, days re-estimated (12.4; §2.8) |
| I3 | Number-test hashes depend on thread count | applied: fixed chunks, one-host caveat, arm64 and macOS runs before commit, nightly legs named (6.5, 10.2; §2.3) |
| I4 | Specified writer never measured; JavaScript claim false | applied: exact outputs specified, hash re-committed by lane A, JavaScript text replaced (6.4, 6.5; §2.3) |
| I5 | Canonical JSON contradicts §2.2; order and duplicates undefined | applied: "of a present element", sorted sets and macros, duplicates E5 and E8 (2.7, 6.2, 6.4) |
| I6 | The compiler audit cannot see MSVC imports | applied: target-scoped source ban, import check on GCC and Clang (8.3) |
| I7 | CTRL and META not validated | applied: rules in 5.3, META parsed by `DecodePreset`, both fuzzed (5.3, 10.2) |
| I8 | `sinceRev` and version skew untested | applied: frozen fixtures (10.3) |
| I9 | "One encoding" overstated | applied with E7 (5.2) |
| I10 | Namespace `bsc` does not keep `symbol-scan` true | applied, second option: scan scoped to functions (8.1; §4) |
| I11 | The strict reader rejects typed text the plugin accepts | applied: lenient entry point, plugin tests (6.5, 10.4) |
| I12 | The SHA-256 move is not a move | applied: digest core split, hex stays in tests, includes in lane B (6.3, 12.4) |
| I13 | Ratings orphaned by leaf-adding pull requests | applied with P8 (11.3) |
| I14 | W-numbers mean waves and warnings; undefined terms | applied: lint L1–L9 with the subnormal finding numbered, terms extended, §7.5's symbols defined (1.3, 2.7, 7.5) |
