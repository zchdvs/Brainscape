# Brainscape — Project Status

> Snapshot as of **2026-10-06**.
> Brainscape is an open-source granular delay — a spiritual successor to the Hologram
> Microcosm — targeting a Daisy Seed3 hardware pedal **and** a JUCE desktop plugin and
> companion app from one shared C++ DSP core. Licensed [GPLv3](../LICENSE).

---

## Where the project is

The project has completed its **research**, the **core DSP engine** (v1 scope), the
**designs for pedal/desktop parity and the companion app**, and the **engine side of the
determinism profile**: one `dsp/` build profile, in-tree math, a full floating-point
control-word guard, a deterministic denormal flush, a NaN-free boundary and the block-split
fix. A golden-hash harness renders a 22-preset corpus bit-identically with MSVC, GCC, Clang
and the Cortex-M7 code run under emulation, at any block size; it reports and does not yet
gate. A JUCE plugin and standalone skeleton hosts the engine. Nothing has touched real
hardware, and the parity and plugin CI workflows have not yet run on GitHub.

| Phase | State |
| --- | --- |
| Research corpus | ✅ Complete — 11 sourced documents + synthesis ([docs/research/](research/)) |
| Engine design | ✅ Complete — reviewed v2 ([grain-engine.md](design/grain-engine.md)) |
| `dsp/` core: contracts + skeleton | ✅ Shipped & hardened |
| `dsp/` core: grain scheduler + 64-voice pool | ✅ Shipped & hardened (block-split defect found and fixed 2026-10-05) |
| `dsp/` core: post chain + feedback taming | ✅ Shipped & hardened |
| `dsp/` core: onset detector + trigger layer | ✅ Shipped & hardened |
| Determinism profile (sample-identical pedal ↔ desktop) | 🚧 Engine side landed; golden harness report-only ([determinism-profile.md](design/determinism-profile.md)). `Restart`, epoch, `LoadPreset` and stamped events are **next**, then minting sound revision 1 |
| Companion app + plugin (JUCE: VST3, AU, standalone) | 🚧 Skeleton built ([plugin/README.md](../plugin/README.md); design in [companion-app.md](design/companion-app.md)): wrapper, plain-value parameters, test-bench editor; no presets, library or device link yet |
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
presets × 30 s. `dsp/` now has this property (measured 2026-10-06): the golden corpus (11
vectors, 22 presets, up to 120 s each) gives the same SHA-256 per preset with MSVC 19.40
(SSE2 and AVX2), GCC 11 and 14 (also at `-march=x86-64-v3`, and in Debug), Clang 14 and the
Cortex-M7 build from the pinned arm-none-eabi 10.3 run under `qemu-arm -cpu cortex-m7`, and
the prototype's 10-preset battery agrees across the same builds. A build with contraction
turned back on (the negative control) differs on 20 of the 22 presets.

What the guarantee covers is precise. Two conforming builds of the same sound revision,
restarted into the exact-restart state, loading the same compiled preset with an Exact load,
and fed the same 48 kHz float32 input and the same frame-stamped events, write identical
float32 output, whatever block sizes each side uses. Not
covered: the pedal's analog path, live playing, preset loads that keep trails (Spillover),
DAW sessions at other sample rates, and DAW automation. The full contract is in
[determinism-profile.md](design/determinism-profile.md). The exact-restart state, the exact
preset load and frame-stamped events are not built yet, so today the guarantee holds for
renders that start from a fresh `Init`, set every parameter and apply events at block
boundaries, which is how the golden harness renders.

## What the engine does today

One `brainscape::Engine` (`dsp/`: ≈4,400 lines of platform-agnostic C++17, the test-signal
generator and the parameter display table included; no allocation and no locks in the audio
path; no libm at all, with in-tree math (`DetMath`) for every transcendental and the IEEE
square root; an opaque engine object whose floating-point code stays in private headers)
implementing:

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
  stereo delay (damped, DC-blocked regeneration; a time change glides the tap and bends
  pitch like tape instead of clicking), a Clouds-style Dattorro/Griesinger
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
  "Identities to freeze before the first public release"). Tapers, step counts and display
  text live beside the descriptors in `dsp/` (`ParamDisplay.h`), so a pedal pot and a plugin
  knob at the same position give the same plain bits.
- **The determinism profile's engine side** ([determinism-profile.md](design/determinism-profile.md)):
  - a build profile (`cmake/BrainscapeFpProfile.cmake`: contraction off, no fast-math, no
    `errno` square roots) that `brainscape_dsp` passes on PUBLIC, tripwire headers, and a
    configure-time check, with a self-test, that rejects forbidden FP and LTO settings on
    every target that compiles or links the engine, the JUCE targets included;
  - a full control-word guard (`dsp/src/detail/FpEnvGuard.h`): every entry point (`Init`,
    `Reset`, `ClearHistory`, `Process`, `SetParam`, `PlanMemory`, `Canonicalize` and the
    taper and display functions) writes the complete word and restores the caller's;
  - gradual underflow with a deterministic in-code flush of every recursive state, so FTZ
    hosts and the M7's FZ give the same bits and silent tails raise no subnormal flags;
  - a NaN-free boundary: canonical parameter values decided on the bit pattern,
    `SanitizeInput` and `ConditionInput24` for input, and finite input giving finite output;
  - the engine defects the profile listed, fixed: mono in-place aliasing, the dither key
    (it now folds the whole 64-bit sample counter), and the three block-split mechanisms,
    with pin-eligible marks (a frozen grain positions only at marks recorded before the pin).

### Verified behavioral contracts (the test suite enforces these)

- **Bit-exact block-split invariance within one build**: identical output whether the host
  chops the stream into 1-, 7-, 48-, 127- or 512-frame blocks, for the configurations the
  suite exercises — dither, jitter, spray, reverse, pitch, onset-mark positioning with onsets
  that actually fire, freeze engaged mid-render and held past the re-anchor point, grain
  positions on the ring's far rail, and queued manual triggers; the golden corpus agrees at
  1-, 37-, 48- and 512-frame blocks and a mixed pattern on every build. A Debug assertion
  checks that no grain reads a ring frame the current block has already written ahead of the
  live write head (for every grain the ring can hold: a ring shorter than one block plus a
  grain's span cannot be invariant, determinism-profile.md §5.7). The freeze/onset-mark/
  far-rail defect found on 2026-10-05 is fixed (see determinism-profile.md, "The block-split
  bug").
- **Bit-exact degenerate-delay null** through the int16 ring (the one-engine proof).
- **Level consistency** within ±1 dB across the whole overlap sweep, including
  coherent, decorrelated, frozen, and mark-anchored populations.
- **Feedback decays to exact zero** (not just quiet) and self-oscillates bounded
  above unity.
- **Deterministic, reproducible renders** — every random draw is keyed on a free-running
  counter, never on block structure or call history.
- **Bit-identical across conforming builds** on the golden corpus (above), whatever the
  host's floating-point environment: renders under FTZ|DAZ with round-toward-zero, or with
  every exception unmasked, match the clean render, and flushing forced on inside the guard
  reproduces the IEEE result.
- **NaN-free boundary**: a fuzz test feeds NaN, ±inf, subnormal and ±`FLT_MAX` input and
  parameters under a hostile environment; the output stays finite and equals the run fed
  sanitized input and canonical values.
- Onset acceptance: plucks count once each (including 200 ms decays and −30 dB
  levels), hiss and steady tones fire nothing, held-distorted sustain chatter is
  bounded, mid-stream `Reset()` fires nothing.

**Suite** (`ctest`): `dsp_unit` (84 test cases / ~2.54M assertions in Release, 83 in Debug),
the forced-flush tests, the undefined-symbol audit and its negative control, the
configure-check self-test and `golden_report`; a plugin build adds the wrapper tests, the
editor snapshot and a hosted-VST3 check. The `dsp/` tests are green in Release and Debug with
MSVC 19.40, GCC 11 and 14 and Clang 14; the plugin tests with MSVC (Linux and macOS plugin
builds are left to CI).
**CI**: `host.yml` (Linux/macOS/Windows with `-Werror`, Debug+ASan/UBSan, Release+ASan, a
compile-only Cortex-M7 build), `parity.yml` (golden reports from six host legs and the
emulated M7, block-size perturbations and a contraction-on negative control; the static
audits gate, the hashes only report) and `plugin.yml` (every format on three OSes, Release
and Debug). `parity.yml` and `plugin.yml` have not yet run on GitHub.

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

- **Parity is measured, not yet enforced.** Nothing is minted: `kSoundRevision` is 0, the
  golden harness only reports, and `PARITY_HASHES_GATING` stays off until internal sound
  revision 1 (profile §8.4 step 10). The parity and plugin workflows have never run on
  GitHub; the AArch64, macOS and Ubuntu-QEMU legs are untested, and the QEMU and toolchain
  pins still need their first CI run to record checksums. Apple Silicon parity is unmeasured.
- **Engine API the companion needs:** an exact `Restart`, a random-number epoch for
  preset loads that keep trails, one `LoadPreset` entry point with a fixed order, and
  frame-stamped events with an overflow counter; each must follow the guard pattern. Smaller
  items: automating `DelayMs` still splices clean delays (the grain engine's glide, below),
  input above 0 dBFS hard-clips in the int16 ring, and `Trigger()`'s source/velocity/offset
  are accepted but unread.
- **Plugin skeleton gaps:** the resampled 48 kHz mode (other host rates run the engine
  natively), Restart on transport start and the spare engine, the wrapper bypass with
  crossfade, the pedal-faithful input option (`ConditionInput24`), MIDI CC mapping, pluginval
  in CI, CLAP and LV2, `.bsp` session state, and the freeze of parameter IDs and tapers. The
  In/Out level controls are wrapper code outside the guard and never part of a preset.
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
  (the plugin design runs the engine at 48 kHz and resamples at other host rates; until that
  mode lands, the skeleton runs it at the host rate).
- **Engine features from the design not yet built**: glide, per-grain SVF/crush
  modifiers, dual layers, step tables, `POS_GRID`, CLOCK-quantized triggering, scale
  quantization of the pitch set, intermittency.
- **Post chain**: reverb damping/bandwidth as parameters, tempo-synced delay time,
  runtime stage reordering.
- **The central product risk is unchanged**: mode *feel*. No amount of architecture
  replaces the curation effort on the 44 factory modes — and no mode exists yet.

## Next steps (recommended order)

These are the six steps of the merged sequence in
[companion-app.md §8 (Delivery plan)](design/companion-app.md), which also places the
determinism profile's own steps; the numbers match the designs' "merged step" references.
Steps 1–4 need no hardware.

1. **Determinism profile and the `dsp/` API.** Landed: the build profile, header hygiene,
   in-tree math with the symbol audit, the full control-word guard and denormal flush, the
   NaN-free boundary, the block-split fix, mono aliasing and the dither key, the post-delay
   time glide, the parity harness and its M7 leg (report-only), and the JUCE skeleton.
   Remaining: `Restart`, the random-number epoch, `LoadPreset` and frame-stamped events
   (profile steps 8–9, before minting because golden scripts place events at odd frames).
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
