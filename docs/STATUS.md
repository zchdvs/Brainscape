# Brainscape — Project Status

> Snapshot as of **2026-10-05**.
> Brainscape is an open-source granular delay — a spiritual successor to the Hologram
> Microcosm — targeting a Daisy Seed3 hardware pedal **and** a JUCE desktop plugin and
> companion app from one shared C++ DSP core. Licensed [GPLv3](../LICENSE).

---

## Where the project is

The project has completed its **research**, the **core DSP engine** (v1 scope), and the
**designs for pedal/desktop parity and the companion app**. The engine runs today as a host
library with a full contract-test suite; it has not yet touched real hardware or a DAW.

| Phase | State |
| --- | --- |
| Research corpus | ✅ Complete — 11 sourced documents + synthesis ([docs/research/](research/)) |
| Engine design | ✅ Complete — reviewed v2 ([grain-engine.md](design/grain-engine.md)) |
| `dsp/` core: contracts + skeleton | ✅ Shipped & hardened |
| `dsp/` core: grain scheduler + 64-voice pool | ✅ Shipped & hardened (block-split defect found and fixed 2026-10-05) |
| `dsp/` core: post chain + feedback taming | ✅ Shipped & hardened |
| `dsp/` core: onset detector + trigger layer | ✅ Shipped & hardened |
| Determinism profile (sample-identical pedal ↔ desktop) | 📐 Designed and prototyped ([determinism-profile.md](design/determinism-profile.md)) — **next** |
| Companion app + plugin (JUCE: VST3, AU, standalone) | 📐 Designed ([companion-app.md](design/companion-app.md)) — skeleton can start alongside the profile |
| Mode system (JSON → compiled mode, desktop-only compiler) | ⬜ Not started — needs its own design doc |
| Preset package + upload to the pedal | 📐 Designed (in companion-app.md) — after the mode compiler / needs hardware |
| Tempo/clock trigger source | ⬜ Not started (`ProcessContext` fields reserved) |
| Looper subsystem | ⬜ Not started (memory/CPU envelope budgeted in the design) |
| Firmware bring-up (Daisy Seed3) | ⬜ Not started (CI cross-compiles `dsp/` for Cortex-M7 today) |
| Hardware (schematic/PCB) | ⬜ Not started (reference design chosen in research) |

**The one-engine bet is validated in code.** The design's central claim — that the
Microcosm's 11 effects are one voice engine + one scheduler with modes as data — now has
its strongest possible evidence: Brainscape's clean delay *is* a grain configuration
(rectangular window, abutting unity-rate grains), and it nulls **bit-exactly** against
the raw int16 history ring.

## Decisions recorded on 2026-10-05

The owner set three product requirements for the desktop side:

1. **The companion app is for preset creation with 1:1 recreation.** Presets authored on the
   desktop must render exactly as the Brainscape engine on the pedal renders them, and the
   app uploads them to the pedal.
2. **JUCE, in one C++ monorepo.** This replaces the research's iPlug2 recommendation. JUCE
   must be used under AGPLv3 (its commercial licence forbids combining with copyleft code),
   so released desktop binaries are a GPLv3 + AGPLv3 combined work; see the README.
3. **"Same sound" means sample-identical output**, not perceptual similarity.

The parity investigation showed requirement 3 is achievable. A prototype that replaced every
standard-library transcendental with in-tree math and disabled compiler multiply-add fusion
produced **one identical SHA-256 across 32 builds** — MSVC, GCC 11, 12 and 14 and Clang on
x86, and the real firmware code generation for the Cortex-M7 run under emulation — over 10
presets × 30 s. The current code does not have this property: the same comparison today
decorrelates within seconds on any preset with timing jitter.

What the guarantee covers is precise. Two conforming builds of the same sound revision,
restarted into the exact-restart state, loading the same compiled preset with an Exact load,
and fed the same 48 kHz float32 input and the same frame-stamped events, write identical
float32 output, whatever block sizes each side uses. Not
covered: the pedal's analog path, live playing, preset loads that keep trails (Spillover),
DAW sessions at other sample rates, and DAW automation. The full contract is in
[determinism-profile.md](design/determinism-profile.md).

## What the engine does today

One `brainscape::Engine` (≈2,500 lines of platform-agnostic C++17; no allocation and no locks
in the audio path, and no transcendental libm calls in the per-sample loops — only IEEE-exact
square roots; transcendentals still run at grain birth and parameter changes, which the
determinism profile replaces with in-tree math) implementing:

- **Granular core** — 64 POD voices, split-32.32 phase, resolve-once-at-birth
  scheduling, periodic↔Poisson jitter morph, overlap-referenced cubic density with a
  dithered fractional ceiling, spray (guard-reflected), ±24 st pitch with cents spread,
  per-grain reverse, equal-power pan, tiered interpolation (8× cubic Hermite / linear)
  with a bit-exact integer path at unity rate, coherence-aware `N^−p` normalization,
  and freeze as a pinned anchor (holding onset-mark positions too) with per-sample
  re-anchor-on-wrap.
- **Feedback path** — a fixed taming chain (DC → HP 100 Hz → feedback-dependent LP →
  soft saturator → allpass diffusion) that makes **feedback up to 1.1 a bounded
  self-oscillation feature**, with counter-keyed TPDF dither so the loop decays to
  *exact* silence.
- **Post chain** — ordered, bypassable stages: stereo chorus-class mod, the Space-knob
  stereo delay (damped, DC-blocked regeneration), a Clouds-style Dattorro/Griesinger
  reverb with multi-tap early output, and a double-sampled SVF with continuous
  LP→BP→HP→Notch morph (equal-power laws throughout).
- **Trigger layer** — spectral-flux onset detection (512/256, in-tree FFT) with
  adaptive whitening, a relative whitening floor, Dixon's peak-picker and growth
  hysteresis; a mark ring feeding POS_MARK grain positioning (the Strum-family
  mechanism); ONSET as an OR'd trigger source with oldest-steal allocation; an
  external `Trigger()` fallback that never drops; and a counted onset indicator for
  the trigger LED. End-to-end onset→grain latency: **5.3 ms** (measured).
- **28 permanent-ID plain-value parameters.** They do not yet match the design's leaf list
  exactly; reconciling them gates the first public plugin release (companion-app.md,
  "Identities to freeze before the first public release").

### Verified behavioral contracts (the test suite enforces these)

- **Bit-exact block-split invariance within one build**: identical output whether the host
  chops the stream into 1-, 7-, 48-, 127- or 512-frame blocks, for the configurations the
  suite exercises — dither, jitter, spray, reverse, pitch, onset-mark positioning with onsets
  that actually fire, freeze engaged mid-render and held past the re-anchor point, grain
  positions on the ring's far rail, and queued manual triggers. A Debug assertion checks
  that no grain reads a ring frame the current block has already written ahead of the live
  write head. The freeze/onset-mark/far-rail defect found on 2026-10-05 is fixed (see
  determinism-profile.md, "The block-split bug").
- **Bit-exact degenerate-delay null** through the int16 ring (the one-engine proof).
- **Level consistency** within ±1 dB across the whole overlap sweep, including
  coherent, decorrelated, frozen, and mark-anchored populations.
- **Feedback decays to exact zero** (not just quiet) and self-oscillates bounded
  above unity.
- **Deterministic, reproducible renders within one build** — every random draw is keyed on a
  free-running counter, never on block structure or call history. Across builds (pedal vs
  desktop) output is *not* identical yet; that is the determinism profile's job.
- Onset acceptance: plucks count once each (including 200 ms decays and −30 dB
  levels), hiss and steady tones fire nothing, held-distorted sustain chatter is
  bounded, mid-stream `Reset()` fires nothing.

**Suite: 41 test cases / ~633k assertions**, green in Release and Debug.
**CI**: Linux/macOS/Windows host matrix with `-Werror`, a Debug+ASan/UBSan leg, a
Release+ASan leg (for NDEBUG-gated contract tests), and a compile-only
**Cortex-M7 cross build** of `dsp/`.

## How it was built (methodology)

Every increment followed the same loop: **implement → contract tests green →
adversarial multi-agent review with empirical probes → fix → commit**. Six review
rounds so far produced **232 findings** (engine design: 60; skeleton: 34; grain engine: 26;
post chain: 26; trigger layer: 23; parity + companion designs: 63), the large majority
verified by compiled probes, bit-exact replicas, mutation testing, emulated Cortex-M7 runs, or
disassembly rather than inspection. Highlights of what that caught before it could ship: a
voice population that varied −3.5 dB with the DAW buffer size, a reverb that was silent for
its first 107 ms, an unbounded filter at its own knob stop (+76 dB), a detector that free-ran
on rig hiss, a normalization law wrong by +18 dB on the flagship delay modes, and — in the
parity work — a block-split defect under freeze and onset marks (since fixed) and a
licensing conflict in libDaisy. Review
records live in [docs/design/reviews/](design/reviews/).

## Known gaps and deferred work

- **Pedal ↔ desktop output is not identical today.** The firmware and Apple Silicon builds
  fuse multiply-adds that x86 builds do not, and standard-library math differs between
  toolchains. The determinism profile fixes both (contraction off everywhere, in-tree math)
  and adds a full floating-point control-word guard, a deterministic denormal policy,
  golden-hash CI and an emulated Cortex-M7 parity job.
- **Engine defects to fix before the first sound revision:** a mono in-place aliasing bug
  (`dsp/src/Engine.cpp:526-527`: with mono input and a host that shares input and output
  buffers, every right-channel sample is wrong); the dither key truncating the sample counter
  (the pattern repeats every 2²⁹ samples); and non-finite input passing through, plus a NaN
  path in the grain envelope setup once denormals are no longer flushed. The determinism
  profile lists them ("A NaN-free boundary", "Output-changing fixes").
- **Engine API the companion needs:** an exact `Restart`, a random-number epoch for
  preset loads that keep trails, one `LoadPreset` entry point with a fixed order, and
  frame-stamped events. Smaller items: automating delay times clicks (no smoothing on the
  post-delay tap), input above 0 dBFS hard-clips in the int16 ring, and `Trigger()`'s
  source/velocity/offset are accepted but unread.
- **Licensing, firmware side:** libDaisy's USB device/host code and its stock SD-card glue
  carry ST's SLA0044 licence, which forbids open-source redistribution, and libDaisy's
  `System` object links the USB interrupt handlers into every firmware. GPLv3 firmware needs
  a pinned libDaisy patch, TinyUSB (MIT) for USB, and its own SD disk-I/O layer — see
  companion-app.md. Not yet reviewed by a lawyer.
- **M7 budget pass (design §8/§10 gates)**: all §8 numbers remain derived, not
  DWT-measured. The scheduler's 64-slot sweep wants a free bitmask; segment batching at
  extreme birth rates; the detector's per-hop FFT is a single-sample cost spike (~2–2.4× the
  budgeted pessimistic row in its worst block) that likely wants stage-splitting. The
  determinism profile adds costs to measure (contraction off, in-tree math at the maximum
  birth rate, denormal flush sites, subnormal timing); **hardware measurement gates
  everything**.
- **Trigger layer**: no sidechain input; detector constants are calibrated for 44.1/48 kHz
  (the plugin always runs the engine at 48 kHz and resamples at other host rates).
- **Engine features from the design not yet built**: glide, per-grain SVF/crush
  modifiers, dual layers, step tables, `POS_GRID`, CLOCK-quantized triggering, scale
  quantization of the pitch set, intermittency.
- **Post chain**: reverb damping/bandwidth as parameters, tempo-synced delay time,
  runtime stage reordering, delay-time change crossfade.
- **The central product risk is unchanged**: mode *feel*. No amount of architecture
  replaces the curation effort on the 44 factory modes — and no mode exists yet.

## Next steps (recommended order)

These are the six steps of the merged sequence in
[companion-app.md §8 (Delivery plan)](design/companion-app.md), which also places the
determinism profile's own steps; the numbers match the designs' "merged step" references.
Steps 1–4 need no hardware.

1. **Determinism profile and the `dsp/` API.** Contraction off on every toolchain, in-tree
   math replacing libm, the full control-word guard and denormal flush, a NaN-free input
   boundary, the remaining output-changing fixes (mono aliasing, the dither key; the
   block-split fix has landed), then `Restart`, the random-number epoch, `LoadPreset` and
   frame-stamped events. The JUCE skeleton (build, plain-value parameter layer, forced-48 kHz
   standalone shell) runs in parallel.
2. **Mint internal sound revision 1.** Golden hashes and CI gates turn on, including the
   emulated Cortex-M7 parity job on every pull request; then the nightly legs.
3. **Mode compiler** (own design doc first), parameter-ID reconciliation and macro IDs, and
   the `.bsp` preset package with its desktop compiler.
4. **First factory modes through the app's offline audition** — burning down the feel risk.
   App integration continues in parallel.
5. **Hardware bring-up and the hardware-gated decisions, then the device link.** On a
   Daisy Seed3: the DWT measurement pass and the decisions it gates (subnormal cost and the
   flush, explicit FMA, polynomial kernels or tables, `Restart` time, and the pedal's
   default load mode); the pedal side of the device link (TinyUSB, GPL-clean SD disk I/O,
   the preset slot store, the upload protocol with its PARITY check, the firmware update
   path, and the engine's SPSC event queue for the firmware's producers); and the app side
   (upload, download, verified upload, sound-revision skew handling). A desktop pedal
   simulator lets the protocol work start before the hardware is finished.
6. **First public sound revision** — parameter IDs, names, tapers and sound fixed together;
   only then do the public plugin, app and firmware ship.

In parallel when ready: **clock/tempo sync and looper** feature work (rhythmic quantization
remains the Microcosm's most-praised musical trait), and the **hardware schematic** (Seed3 + the
Electrosmith reference stereo I/O front end, per
[pedal-control-surface-and-io-hardware.md](research/pedal-control-surface-and-io-hardware.md)).
