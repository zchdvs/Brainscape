# Brainscape — Project Status

> Snapshot as of **2026-09-01** (14 commits, `main` at `311a34b`).
> Brainscape is an open-source granular delay — a spiritual successor to the Hologram
> Microcosm — targeting a Daisy Seed3 hardware pedal **and** a VST/CLAP plugin from one
> shared C++ DSP core. Licensed [GPLv3](../LICENSE).

---

## Where the project is

The project has completed its **research**, **design**, and the **core DSP engine**
(v1 scope). The engine runs today as a host library with a full contract-test suite;
it has not yet touched real hardware or a DAW.

| Phase | State |
| --- | --- |
| Research corpus | ✅ Complete — 11 sourced documents + synthesis ([docs/research/](research/)) |
| Engine design | ✅ Complete — reviewed v2 ([docs/design/grain-engine.md](design/grain-engine.md)) |
| `dsp/` core: contracts + skeleton | ✅ Shipped & hardened |
| `dsp/` core: grain scheduler + 64-voice pool | ✅ Shipped & hardened |
| `dsp/` core: post chain + feedback taming | ✅ Shipped & hardened |
| `dsp/` core: onset detector + trigger layer | ✅ Shipped & hardened |
| Mode system (JSON → `ModeBlob`) | ⬜ Not started — **next** |
| Sample-accurate parameter queue (SPSC) | ⬜ Not started (acceptance test already in the suite, hidden) |
| Tempo/clock trigger source | ⬜ Not started (`ProcessContext` fields reserved) |
| Looper subsystem | ⬜ Not started (memory/CPU envelope budgeted in the design) |
| Firmware bring-up (Daisy Seed3) | ⬜ Not started (CI cross-compiles `dsp/` for Cortex-M7 today) |
| Plugin wrapper (iPlug2, VST3+CLAP) | ⬜ Not started |
| Hardware (schematic/PCB) | ⬜ Not started (reference design chosen in research) |

**The one-engine bet is validated in code.** The design's central claim — that the
Microcosm's 11 effects are one voice engine + one scheduler with modes as data — now has
its strongest possible evidence: Brainscape's clean delay *is* a grain configuration
(rectangular window, abutting unity-rate grains), and it nulls **bit-exactly** against
the raw int16 history ring.

## What the engine does today

One `brainscape::Engine` (≈2,500 lines of platform-agnostic C++17, no allocation, no
locks, no libm in the audio path) implementing:

- **Granular core** — 64 POD voices, split-32.32 phase, resolve-once-at-birth
  scheduling, periodic↔Poisson jitter morph, overlap-referenced cubic density with a
  dithered fractional ceiling, spray (guard-reflected), ±24 st pitch with cents spread,
  per-grain reverse, equal-power pan, tiered interpolation (8× cubic Hermite / linear)
  with a bit-exact integer path at unity rate, coherence-aware `N^−p` normalization,
  and freeze as a pinned anchor with re-anchor-on-wrap.
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
- **28 permanent-ID plain-value parameters** matching the design's leaf-name schema.

### Verified behavioral contracts (the test suite enforces these)

- **Bit-exact block-split invariance**: identical output whether the host chops the
  stream into 1-, 7-, 48-, 127- or 512-frame blocks — including dither, the full
  stochastic feature set, onset detection, and queued manual triggers.
- **Bit-exact degenerate-delay null** through the int16 ring (the one-engine proof).
- **Level consistency** within ±1 dB across the whole overlap sweep, including
  coherent, decorrelated, frozen, and mark-anchored populations.
- **Feedback decays to exact zero** (not just quiet) and self-oscillates bounded
  above unity.
- **Deterministic, reproducible renders** — every random draw is keyed on a
  free-running counter, never on block structure or call history.
- Onset acceptance: plucks count once each (including 200 ms decays and −30 dB
  levels), hiss and steady tones fire nothing, held-distorted sustain chatter is
  bounded, mid-stream `Reset()` fires nothing.

**Suite: 40 test cases / ~633k assertions**, green in Release and Debug.
**CI**: Linux/macOS/Windows host matrix with `-Werror`, a Debug+ASan/UBSan leg, a
Release+ASan leg (for NDEBUG-gated contract tests), and a compile-only
**Cortex-M7 cross build** of `dsp/`.

## How it was built (methodology)

Every increment followed the same loop: **implement → contract tests green →
adversarial multi-agent review with empirical probes → fix → commit**. Five review
rounds so far produced **169 findings** (design: 60; skeleton: 34; grain engine: 26;
post chain: 26; trigger layer: 23), the large majority verified by compiled probes,
bit-exact replicas, mutation testing, or Cortex-M7 disassembly rather than inspection.
Highlights of what that caught before it could ship: a voice population that varied
−3.5 dB with the DAW buffer size, a reverb that was silent for its first 107 ms, an
unbounded filter at its own knob stop (+76 dB), a detector that free-ran on rig hiss,
and a normalization law wrong by +18 dB on the flagship delay modes. The design
review's findings are preserved in
[docs/design/reviews/](design/reviews/) as an engineering record.

## Known gaps and deferred work (tracked in code TODOs and design §12)

- **Cross-build determinism (design contract #7)**: `SemitonesToRatio`, the jitter
  log draw, and the detector/window tables still route through libm at init or birth;
  in-tree LUTs are specified before any firmware-vs-plugin null test.
- **M7 budget pass (design §8/§10 gates)**: the scheduler's 64-slot sweep wants a
  free bitmask; segment batching at extreme birth rates; the detector's per-hop FFT
  is a single-sample cost spike (~2–2.4× the budgeted pessimistic row in its worst
  block) that likely wants stage-splitting; **all §8 numbers remain derived, not
  DWT-measured — hardware measurement gates everything**.
- **Trigger layer**: `Trigger()`'s source/velocity/sample-offset are accepted but
  unread; no sidechain input; detector constants are calibrated for 44.1/48 kHz
  (resolution degrades at 96 k+, documented).
- **Engine features from the design not yet built**: glide, per-grain SVF/crush
  modifiers, dual layers, step tables, `POS_GRID`, CLOCK-quantized triggering, scale
  quantization of the pitch set, intermittency.
- **Post chain**: reverb damping/bandwidth as parameters, tempo-synced delay time,
  runtime stage reordering, delay-time change crossfade.
- **The central product risk is unchanged**: mode *feel*. No amount of architecture
  replaces the curation effort on the 44 factory modes — and no mode exists yet.

## Next steps (recommended order)

1. **Mode compiler** (`modes::Compile` — JSON → `ModeBlob`, 4-slot publish ring,
   validator). This is the project's declared ABI and what turns one engine into 44
   destinations; nearly every remaining engine feature (layers, step tables, macro
   maps) lands *as schema* here. Design §5 is written; nothing is speculative.
2. **SPSC parameter event queue** — sample-accurate `SetParam`/`Trigger` delivery.
   Small, fully specified (design §9 threading table), and its acceptance test
   already sits hidden in the suite (`[.pending-spsc]`) waiting to be enabled.
3. **CLOCK trigger source + tempo sync** — tap tempo, subdivisions, MIDI clock
   fields already reserved in `ProcessContext`. The research is unambiguous that
   rhythmic quantization is the Microcosm's single most-praised musical trait; Seq /
   Arp / Pattern / Warp coverage is impossible without it.
4. **First factory modes + offline auditioning** — start burning down the feel risk
   the moment the mode compiler exists; the render harness already produces WAVs.
5. **Looper subsystem** (own design doc first, per the main design's scope note):
   two-buffer undo via watermark, 4-way routing; memory is already budgeted.
6. **Firmware bring-up** on a Daisy Seed3: libDaisy scaffold, arenas onto real
   DTCM/AXI/SDRAM, and the DWT measurement pass that turns every §8 estimate into a
   number — this gates all M7 optimization work and should come before deep
   feature-building on top of unverified budgets.
7. **Plugin wrapper** (iPlug2 per research; VST3+CLAP) — mechanical once the queue
   exists; brings the mode editor workflow to life.
8. **Hardware schematic** — Seed3 + the Electrosmith reference stereo I/O front end,
   per [pedal-control-surface-and-io-hardware.md](research/pedal-control-surface-and-io-hardware.md).

Items 1–3 are pure `dsp/` work and independent of hardware availability; item 6 can
proceed in parallel the moment a Seed3 is on the bench.
