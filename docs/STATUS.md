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
control-word guard, a deterministic denormal flush, a NaN-free boundary, the block-split
fix, the engine state API (an exact `Restart`, a random-number epoch, one `LoadPreset`
entry point and frame-stamped events) and the post-delay time glide. **Internal sound
revision 1 is minted**: `dsp/tests/golden/golden.json` holds the hashes of a 28-preset corpus
that MSVC, GCC, Clang and the Cortex-M7 code run under emulation all reproduce, at any block
size and from a hostile caller floating-point environment, and CI now fails a pull request
that changes them (see [Internal sound revision 1](#internal-sound-revision-1)). A JUCE
plugin and standalone skeleton hosts the engine through its stamped events and `LoadPreset`,
with reproducible bounces and an offline audition render. Nothing has touched real hardware,
and the parity, sound-revision and plugin CI workflows have not yet run on GitHub.

| Phase | State |
| --- | --- |
| Research corpus | ✅ Complete — 11 sourced documents + synthesis ([docs/research/](research/)) |
| Engine design | ✅ Complete — reviewed v2 ([grain-engine.md](design/grain-engine.md)) |
| `dsp/` core: contracts + skeleton | ✅ Shipped & hardened |
| `dsp/` core: grain scheduler + 64-voice pool | ✅ Shipped & hardened (block-split defect found and fixed 2026-10-05) |
| `dsp/` core: post chain + feedback taming | ✅ Shipped & hardened |
| `dsp/` core: onset detector + trigger layer | ✅ Shipped & hardened |
| Determinism profile (sample-identical pedal ↔ desktop) | 🚧 Internal sound revision 1 minted and gating ([determinism-profile.md](design/determinism-profile.md) §8.4 steps 1–9 and most of step 10; what step 10 still lacks is under [Known gaps](#known-gaps-and-deferred-work)). Next: the rest of step 10 and the nightly full-system emulation leg; then the hardware measurements and the decisions they gate |
| Companion app + plugin (JUCE: VST3, AU, standalone) | 🚧 Skeleton built ([plugin/README.md](../plugin/README.md); design in [companion-app.md](design/companion-app.md)): wrapper on stamped events and `LoadPreset`, plain-value parameters, Restart on transport start, offline audition, test-bench editor; no presets, library or device link yet |
| Mode system (JSON → compiled mode, desktop-only compiler) | ⬜ Not started — needs its own design doc |
| Preset package + upload to the pedal | 📐 Designed (in companion-app.md) — after the mode compiler / needs hardware |
| Tempo/clock trigger source | ⬜ Not started (`ProcessContext` fields reserved) |
| Looper subsystem | ⬜ Not started (memory/CPU envelope budgeted in the design) |
| Firmware bring-up (Daisy Seed Rev7 prototype; custom H750 board later) | 🚧 Bring-up images built and verified off-hardware ([firmware/README.md](../firmware/README.md)): silicon parity check, DWT measurement pass (contraction-off costs, the §4.2 silent-tail rule, the §7.3 budget rule; it cannot compare explicit FMA or kernels against tables), live audio, on pinned libDaisy v9.0.0; faults are recorded and reported after a reset; awaiting the owner's bench session |
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
presets × 30 s. `dsp/` now has this property (measured 2026-10-06), and it is now enforced:
the golden corpus (13 vectors, 28 presets, up to 123 s each) gives the SHA-256 per preset
committed for sound revision 1 with MSVC 19.40 (SSE2 and AVX2), GCC 11 and 14 (also at
`-march=x86-64-v3`, and in Debug), Clang 14 and the Cortex-M7 build from the pinned
arm-none-eabi 10.3 run under `qemu-arm -cpu cortex-m7`. A build with contraction turned back
on (the negative control) misses it on 25 of the 28 presets, with GCC 11 and with MSVC.

What the guarantee covers is precise. Two conforming builds of the same sound revision,
restarted into the exact-restart state, loading the same compiled preset with an Exact load,
and fed the same 48 kHz float32 input and the same frame-stamped events, write identical
float32 output, whatever block sizes each side uses. Not
covered: the pedal's analog path, live playing, preset loads that keep trails (Spillover),
DAW sessions at other sample rates, and DAW automation. The full contract is in
[determinism-profile.md](design/determinism-profile.md). The engine now has every piece of
that recipe except the compiled preset package: the golden harness Inits one engine, and
for every render restarts it with `LoadPreset(…, Exact)` from a decoded preset state and
hands its scripted events to `Process` through the engine's `EventQueue`; three presets also
load Spillover, restart, or load Exact mid-render. Rendering with a fresh `Init` per preset,
with the events applied between blocks split at their frames, or from a caller whose control
word is FTZ|DAZ (on the M7, FZ|DN) with round toward zero, gives the same bits.

## Internal sound revision 1

Minted 2026-10-06 (determinism-profile.md §5.12, §6.1, §8.4 step 10): `kSoundRevision` is 1
and [`dsp/tests/golden/golden.json`](../dsp/tests/golden/golden.json) is its golden file.
It is internal: nothing carrying it has left the project, and the hardware-gated decisions
below will bump it before the first published revision.

**What it certifies.** The `dsp/` sources as of the mint, the shared constants, the canonical
`EngineConfig` (48 kHz, a 2²² ring, stereo input, dither on the ring write) and the profile's
rules and flags, for every input. The golden file holds 13 integer-generated input vectors
and 28 presets (639 s of rendered audio): per preset, the SHA-256 of the whole float32 output,
one per second, and 18 coverage counters, rendered from one restarted engine with each
preset loaded Exact and its scripted events stamped through the engine's `EventQueue`. It
covers the post-delay glide (two presets glide from second 0, one through a Spillover load),
Spillover and Exact loads and a `Restart` mid-render, freeze, onset marks, feedback above 1,
every post stage, subnormal input, and a 120 s silent tail (in a 123 s vector).

**Which builds agree.** Every one tried, in `--mode check` against the file: MSVC 19.40
(Release SSE2, Debug, Release AVX2), GCC 11.4 (Release, Debug, x86-64-v3), GCC 14.2 (Release,
Debug), Clang 14 (Release, Debug) and the Cortex-M7 archive from arm-none-eabi 10.3 under
`qemu-arm -cpu cortex-m7`. Every Release build and the M7 ran blocks of 1, 48 and 512 frames,
the patterns {48, 1, 127, 32} and {300, 512, 5, 64} and the random pattern of seed 1 (257
sizes of 1–512 frames, `--random-blocks`); the x86 Release builds also 7 and seed 2, and
MSVC SSE2 also 32, 37, 64 and 127. Every Debug build ran 48-frame blocks,
{300, 512, 5, 64}, 1-frame blocks from a hostile caller and seed 2. Events went through the
engine or through a wrapper that splits its blocks at them (seed 3 included); renders came
from a restarted engine or one `Init`'d per render, and from a caller whose control word is
FTZ|DAZ (FZ|DN on the M7) with round toward zero. The forced-flush control reproduces it
too, on every host build and with FZ on the M7. The M7 archive is the same bytes across two
builds, across the bump and across the review fixes (`4f4ddaa3583e46f2`).

**What gates.** Check mode fails on any difference from the file: a hash, a per-second hash or
a counter of any preset, a vector's input hash or ring sizes, or the header's revision,
versions and engine configuration. `parity.yml` with `PARITY_HASHES_GATING` on: six host legs
and the emulated M7 render in check mode at 48-frame blocks, 512-frame blocks and from a
hostile caller (the M7 also at {48, 1, 127, 32}, at a random pattern and with flushing forced
on inside the guard, FZ; the Linux GCC leg at every block size above, two random patterns, with
wrapper-side splitting, a random pattern included, and with fresh engines); each leg uploads
a WAV file of every preset that misses the file on the 48-frame grid, for triage; the
contraction-on negative control must miss the file; the static audits (no libm import, no
fused instruction, no forbidden flag, no JUCE include outside `plugin/`); and parity-summary
fails on any canonical leg that differs from another. ctest's `golden_check`,
`golden_check_edits` (edited copies of the file must fail check) and `golden_forced_flush`
run in every build, so `host.yml` and `plugin.yml` gate on the file as well. `sound-rev.yml`
(companion-app.md §3.4's `sound-rev-gate`) fails a pull request that changes a golden hash
without bumping `kSoundRevision`, whatever its labels, and one that touches `dsp/src`,
`dsp/include`, `dsp/CMakeLists.txt`, the root `CMakeLists.txt`, the profile CMake file, the
forbidden-flag list or the arm toolchain file without a bump or the "sound-neutral" label; a
bump is exactly one and must regenerate the golden file (`brainscape_golden --mode mint`).
Since 2026-10-06 this binds on GitHub: branch protection on `main` requires all 23 CI checks
(every `parity-*` leg, `parity-summary`, `sound-rev-gate`, and the `host` and `plugin` jobs),
an up-to-date branch and code-owner review (Known gaps has the caveats).

**The gates were shown to fail.** A one-ULP change to a binary32 filter constant fails check
on MSVC and on the emulated M7 (the feedback tamer's diffuser gain: 16 of 28 presets; a reverb
allpass gain: 7), and so does DetMath's ln 2 written at binary32 precision (6 of 28); the same
presets fail on both, and each mutant still renders identically on both. A one-ULP change to a
binary64 DetMath coefficient leaves every corpus output bit unchanged (ln 2 in `Exp2D` on MSVC
and the M7, 1/3! in the sine kernel and 2/π in `SinCosD` on MSVC): each DetMath result is
rounded to binary32 before it reaches the signal, and one binary64 ULP moves that rounding
only when the result lies that close to a binary32 rounding boundary, roughly one evaluation
in 2²⁹. Only the path trigger catches such a change, which is why §5.12's rule is "any change
that can change output", not golden coverage. A contracting build is refused by the configure
check (`-ffp-contract=fast`, `/fp:contract`); through the test-only escape it misses 25 of 28
presets and the harness refuses check and mint. Check mode fails on each edit
`golden_check_edits` makes to a copy of the file (one per-second hash, the per-second list cut
short, the ring sizes, the generator version, a counter, a counter the harness does not count);
the harness of the mint (`06557f3`) passed all of them but the changed counter. The gate's
self-test runs 26 synthetic pull requests on every run.

**What remains.** The rest of profile step 10 ([Known gaps](#known-gaps-and-deferred-work)) and
the nightly legs (profile step 11). On hardware, the DWT measurements and
the decisions they gate (subnormal cost and the flush, explicit FMA, polynomial kernels or
tables, `Restart` time and the pedal's default load mode), each adopted one bumping the
revision. The resampled 48 kHz plugin mode, the mode compiler, the `.bsp` preset package and
the device link with its PARITY check ([Next steps](#next-steps-recommended-order) 3–5).

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
    with pin-eligible marks (a frozen grain positions only at marks recorded before the pin);
  - the state API: `Engine::Restart` (the exact post-`Init` state, parameters kept; on an
    engine that has rendered nothing since its buffers were cleared it skips the clears, so
    a load before the first block is real-time safe), a
    random-number epoch that Spillover loads restart, `LoadPreset(PresetState, Exact or
    Spillover)` in the profile's fixed order with a report of inexact loads, frame-stamped
    events in `ProcessContext` (parameter, freeze, trigger and Spillover load) applied at
    their frames by splitting the block inside `Process` (freeze settling once per frame),
    the `EventQueue` transport (it refuses and counts overflows and out-of-order stamps, is
    cleared with a restart, and retires events so a staged preset can be reused), and a
    toolchain ID (compiler, version, target, FP-flag hash).

### Verified behavioral contracts (the test suite enforces these)

- **Bit-exact block-split invariance within one build**: identical output whether the host
  chops the stream into 1-, 7-, 48-, 127- or 512-frame blocks, for the configurations the
  suite exercises — dither, jitter, spray, reverse, pitch, onset-mark positioning with onsets
  that actually fire, freeze engaged mid-render and held past the re-anchor point, grain
  positions on the ring's far rail, and queued manual triggers; the golden corpus agrees at
  1-, 48- and 512-frame blocks, mixed patterns and random sizes of 1–512 frames on every
  toolchain. A Debug assertion
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
  counter (relative to the random-number epoch), never on block structure or call history.
- **Exact restart and stamped events**: a used engine after `Restart` renders exactly what a
  fresh `Init` renders (feedback, freeze, marks, every post stage, a Spillover epoch and
  queued triggers left behind), and events at odd frames render what a wrapper splitting its
  blocks there renders, at block sizes 1 to 512, a freeze released and re-engaged at one
  frame and Spillover loads while frozen included. A Spillover load never reconverges with the
  Exact render (4 s measured, as the profile states); followed by `Reset` it reconverges after
  0.250–0.264 s, the record's figures. The golden corpus checks the same on every leg: after a
  `Restart` or an Exact load mid-render, the rest of the output equals a render of the rest
  from the exact-restart state.
- **Bit-identical across conforming builds** on the golden corpus (above), whatever the
  host's floating-point environment: renders under FTZ|DAZ with round-toward-zero, or with
  every exception unmasked, match the clean render, and flushing forced on inside the guard
  reproduces every golden vector on the 24-bit grid (`golden_forced_flush` on the host, FZ on
  the emulated M7). One vector feeds
  subnormal input where the plucks are silent and must output subnormals, which no flushing
  mode can produce: forced flushing must change it, so a guard that lets a caller's flush bit
  through fails the corpus. The corpus renders eight presets again on a fresh engine from a
  hostile caller (Init included) on every leg, the M7 too, and a whole-corpus hostile run
  matches the clean one.
- **The plugin reproduces the engine bit for bit**: host blocks of any size against the
  engine driven by stamped events from `LoadPreset(…, Exact)`, one `Process` call per chunk of
  at most 512 frames; restores, MIDI, host automation and scripted events at their frames, at
  every host block pattern; offline and real-time bounces with Restart on transport start equal
  a fresh render, with MIDI, with an automation lane (the start folds the first block's values
  into the load) and with the wrapper passing the offline mode before every block; a stamp made
  before the restart is void; the audition render equals the engine's from the exact-restart
  state, and its WAV and hashes (whole and per second) match it.
- **NaN-free boundary**: a fuzz test feeds NaN, ±inf, subnormal and ±`FLT_MAX` input and
  parameters under a hostile environment; the output stays finite and equals the run fed
  sanitized input and canonical values.
- Onset acceptance: plucks count once each (including 200 ms decays and −30 dB
  levels), hiss and steady tones fire nothing, held-distorted sustain chatter is
  bounded, mid-stream `Reset()` fires nothing.

**Suite** (`ctest`): `dsp_unit` (106 test cases / ~2.61M assertions in Release, 105 in Debug),
the forced-flush tests, the undefined-symbol audit and its negative control, the
configure-check self-test, `golden_check` (every golden hash of sound revision 1),
`golden_check_edits` (check mode fails on edited copies of the golden file) and the corpus's
forced-flush control `golden_forced_flush`; a plugin build adds the wrapper tests (29
test cases), the editor snapshot and a hosted-VST3 check. The `dsp/` tests are green in
Release and Debug with MSVC 19.40, GCC 11 and 14 and Clang 14; the plugin tests with MSVC
(Linux and macOS plugin builds are left to CI).
**CI**: `host.yml` (Linux/macOS/Windows with `-Werror`, Debug+ASan/UBSan, Release+ASan, a
compile-only Cortex-M7 build), `parity.yml` (six host legs and the emulated M7 checking the
golden file, block-size, random-block, hostile-FP-environment and, on the M7, forced-flush
perturbations, a contraction-on negative control and the static audits, all gating),
`sound-rev.yml` (the sound-revision gate) and
`plugin.yml` (every format on three OSes, Release and Debug). `parity.yml`, `sound-rev.yml`
and `plugin.yml` have not yet run on GitHub.

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

- **Parity is proven on emulation, not yet on silicon.** The owner prototypes on a Daisy Seed Rev7
  (STM32H750, PCM3060), with a custom H750 board later; the Rev7 parity, DWT bench and live-audio
  images are built, fit their memory and pass every off-hardware check (the parity stream's code
  matches the golden file under `qemu-arm -cpu cortex-m7` in the parity image's own memory placement), and wait for the bench
  ([firmware/README.md](../firmware/README.md)). They use libDaisy's ST USB code and must not be
  distributed. The first GitHub runs (2026-10-06) rendered the golden corpus bit-identically
  on every leg: Windows x64 (MSVC, MSVC AVX2), Linux x64 (GCC, Clang), Linux arm64 (GCC 13), macOS arm64 (AppleClang 15 on
  `macos-14` and AppleClang 21 on `macos-latest`) and the Cortex-M7 under QEMU 10.2.3, whose
  engine archive was byte-identical to a local build (`4f4ddaa3…`). The ARM toolchain's MD5
  passed against Arm's download and QEMU is now pinned to the commit that run recorded. The
  run's failures were all in tooling (fixed in #1; see the record there): LLVM 21 folding
  the harness's subnormal bit tests into floating-point compares under the hostile control
  word, two symbol-audit allowlist gaps, and a Windows smoke-test path. A hardware-in-the-loop
  runner in CI and the nightly legs (full-system QEMU with the interrupt `FPDSCR`, exhaustive
  DetMath accuracy, toolchain drift) are not built.
- **How far the gates bind.** Branch protection on `main` requires all 23 CI checks, an
  up-to-date branch and code-owner review; [`.github/CODEOWNERS`](../.github/CODEOWNERS)
  names @zchdvs for `dsp/` (`dsp/tests/golden/golden.json` included), `cmake/`, the root
  `CMakeLists.txt`, the arm toolchain file, `.github/workflows/` and `tools/ci/`, as profile
  §5.12 and §6.1 require. Only collaborators can apply the "sound-neutral" label, so today
  only the owner can waive the path trigger. Caveats: every gate runs the pull request's own
  code (a pull request that edits the harness or a workflow can pass its own checks), so
  code-owner review of those paths is the real control; and the owner is the only code owner
  and cannot approve their own pull requests, so owner merges go through the administrator
  bypass, which the protection allows on purpose. Running `sound-rev-gate` from the base
  branch (`pull_request_target`) would not close the first caveat alone, since a pull request
  can add a workflow whose job has the same name.
- **Profile step 10 is not finished** (determinism-profile.md §8.4 lists it). Not yet built:
  the engine-side coverage counters (births, steals, reverse and mark-positioned births,
  re-anchors, far-rail clamps, blocks with an underflow flag, the write-ahead counter) and the
  gcov thresholds of §6.1 (revision 1 substitutes the harness's 18 counters with per-preset
  minimums, ablations, and the Debug write-ahead assertion over the whole corpus); host blocks
  up to 8,192 frames through the wrapper's chunker against the corpus (§6.4; the wrapper tests
  reach 4,096 against the engine); the ODR controls, renders and the audit with `dsp/` and
  plugin objects in swapped link order (§6.3, §6.4); the literal-bit audit (§6.3); the plugin
  format targets' flags compared with the engine's through `compile_commands.json` (§6.3);
  the negative control with one DetMath function swapped back to libm, as a hash control
  (§6.4); the Rosetta 2 and Prism host legs (§6.2); and a mint job: revision 1 was minted
  locally, and its pull request must pass every x86 leg and the emulated M7 against the
  committed file in one CI run, the deviation §6.1 records.
- **Engine API still to come:** `PresetState` holds the STAT leaves only (the mode blob,
  macros and the stored performance state arrive with the mode system, and the `.bsp`
  decoder with the package format); tap/tempo, mode-switch, macro and expression events;
  `SaveState`/`LoadState` (which will carry the epoch). Smaller items: automating `DelayMs`
  still splices clean delays (the grain engine's glide, below), input above 0 dBFS
  hard-clips in the int16 ring, and a trigger's source and velocity are carried but unread.
- **Plugin skeleton gaps:** the resampled 48 kHz mode (other host rates run the engine
  natively), the wrapper bypass with crossfade, the pedal-faithful live input option
  (`ConditionInput24`; the audition render applies it), event scripts in the audition, MIDI CC
  mapping, pluginval in CI, CLAP and LV2, `.bsp` session state, and the freeze of parameter
  IDs and tapers. The In/Out level controls are wrapper code outside the guard and never part
  of a preset.
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

1. **Determinism profile and the `dsp/` API.** *Done:* the build profile, header hygiene,
   in-tree math with the symbol audit, the full control-word guard and denormal flush, the
   NaN-free boundary, the block-split fix, mono aliasing and the dither key, the post-delay
   time glide, the parity harness and its M7 leg, the JUCE skeleton, and `Restart`, the
   random-number epoch, `LoadPreset` and frame-stamped events (profile steps 1–9).
2. **Mint internal sound revision 1.** *Minted* (profile step 10): golden hashes, the
   sound-revision gate and the emulated Cortex-M7 parity job check every pull request, and
   branch protection makes them required on `main`; every leg, AArch64 included, matched the
   golden file on its first GitHub run. Remaining: the rest of step 10 (Known gaps) and the
   nightly legs (profile step 11).
3. **Mode compiler** (own design doc first), parameter-ID reconciliation and macro IDs, and
   the `.bsp` preset package with its desktop compiler.
4. **First factory modes through the app's offline audition** — burning down the feel risk.
   App integration continues in parallel: the resampled 48 kHz plugin mode for other host
   rates, `.bsp` presets and session state, and the rest of the plugin gaps above.
5. **Hardware bring-up and the hardware-gated decisions, then the device link.** On a
   Daisy Seed3: the DWT measurement pass and the decisions it gates (subnormal cost and the
   flush, explicit FMA, polynomial kernels or tables, `Restart` time, and the pedal's
   default load mode); the pedal side of the device link (TinyUSB, GPL-clean SD disk I/O,
   the preset slot store, the upload protocol with its PARITY check, the firmware update
   path, and the engine's SPSC event queue for the firmware's producers); and the app side
   (upload, download, verified upload, sound-revision skew handling). A desktop pedal
   simulator lets the protocol work start before the hardware is finished. Each adopted
   hardware-gated decision bumps the internal sound revision.
6. **First public sound revision** — parameter IDs, names, tapers and sound fixed together;
   only then do the public plugin, app and firmware ship.

In parallel when ready: **clock/tempo sync and looper** feature work (rhythmic quantization
remains the Microcosm's most-praised musical trait), and the **hardware schematic** (Seed3 + the
Electrosmith reference stereo I/O front end, per
[pedal-control-surface-and-io-hardware.md](research/pedal-control-surface-and-io-hardware.md)).
