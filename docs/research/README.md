# Brainscape research corpus

Foundation research for Brainscape, produced 2026-08-30 by a multi-agent web-research fanout
(six parallel deep-dives, a completeness critic, and five critic-driven gap-fill rounds).
Every document cites its sources inline; claims that could not be verified are explicitly
flagged as such. Note: Reddit, TGP, and Gearspace blocked automated access, so community
sentiment is sourced from ilovefuzz, Elektronauts, professional reviews, and search snippets.

> **Update (2026-10-05):** three findings here are superseded by later decisions. The
> plugin framework is **JUCE**, not iPlug2 (conclusion 6 and the vst-and-shared-dsp.md row
> below): the project owner chose it, inside one C++ monorepo, and
> [companion-app.md](../design/companion-app.md) specifies the desktop side. The licence is
> **GPLv3** ([LICENSE](../../LICENSE)), no longer an open question (see "Known risks"); one
> consequence is that aubio, GPLv3 itself, is no longer study-only (conclusion 7). Preset
> storage and parsing (conclusion 9) follow companion-app.md: JSON is authored on the desktop
> and compiled there into binary preset packages, the pedal never parses JSON, and user
> presets live on microSD rather than in QSPI flash. Dated notes at those places say what
> replaced them. The research content itself is unchanged.

## The documents

| Document | What it covers |
| --- | --- |
| [microcosm.md](microcosm.md) | The Hologram Microcosm, reverse-read from the official v1.13 manual: all 44 effect variations, signal path, looper, MIDI, why it's beloved, verified criticisms, community wishlist, extension opportunities |
| [grain-delay-theory.md](grain-delay-theory.md) | Granular synthesis and grain delay: history, the core algorithm (buffers, scheduler, windows, voices, pitch, feedback), and architecture lessons from Clouds/Beads, DaisySP, EmissionControl2, SuperCollider, Csound |
| [daisy-seed-platform.md](daisy-seed-platform.md) | The Daisy Seed as a platform: STM32H750 memory map, codec revision history, libDaisy/DaisySP, toolchains, bootloader/QSPI, SRAM-vs-SDRAM performance |
| [daisy-pedal-platforms.md](daisy-pedal-platforms.md) | Survey of open Daisy pedal hardware (Terrarium, Hothouse, Funbox, GuitarPedal125b) and the Seed's analog front end. §2.4/§4/§5 are superseded by the control-surface doc below |
| [pedal-control-surface-and-io-hardware.md](pedal-control-surface-and-io-hardware.md) | The hardware reference-design decision: Seed3 vs Rev7, official stereo I/O reference circuit, control-surface multiplexing, relay bypass, MIDI, current budget, enclosure, licensing of candidate designs |
| [hardware-supply-2026-10.md](hardware-supply-2026-10.md) | The Seed3 stock-out (Oct 2026): what the DaisyKiCad package and JLCPCB can and can't build, module alternatives (Seed2 DFM, Rev7), the custom STM32H750 core option and its triggers, compliance before selling; the decision to prototype on a Seed Rev7 |
| [vst-and-shared-dsp.md](vst-and-shared-dsp.md) | Plugin framework choice (VST3 SDK is now MIT; iPlug2 lead candidate), real-time DSP-core rules, embedded+desktop code-sharing precedents (OWL, Mutable→VCV), CMake dual-target build, monorepo layout. The framework choice is superseded: JUCE was chosen on 2026-10-05 |
| [granular-pedal-landscape.md](granular-pedal-landscape.md) | Market survey: Microcosm, Chase Bliss, Red Panda, Walrus, ZOIA, Beebo, Meris — user sentiment, price anchors, and the gap analysis behind Brainscape's positioning |
| [preset-parameter-and-patch-format.md](preset-parameter-and-patch-format.md) | Parameter model and preset format: VST3/CLAP parameter-ID conventions, ZOIA/Surge/VCV precedents, libDaisy persistence constraints, why JSON presets would be a category first |
| [post-fx-chain-looper-and-system-budget.md](post-fx-chain-looper-and-system-budget.md) | Everything downstream of the grain engine: reverb algorithm/memory options, DaisySP Looper's real limitations, filter, and a whole-system CPU/RAM budget |
| [onset-detection-on-cortex-m7.md](onset-detection-on-cortex-m7.md) | The trigger layer (the Microcosm's #1 complaint): onset-detection methods vs distorted-guitar/pad failure cases, CMSIS-DSP costs, adaptive whitening, calibration UX, trigger-source fallbacks |
| [bom-cost-and-product-form.md](bom-cost-and-product-form.md) | Costed BOM at DIY and small-batch quantities, honest test of the "under $350" claim, and the recommended tiered product form |

## Design-shaping conclusions

The findings that should directly shape the design phase:

1. **One engine, modes as data.** The Microcosm's 11 "effects" are almost certainly one grain
   engine (circulating buffer + resampled voices + scheduler + modifier vocabulary) with 44
   presets on top. Brainscape should build one engine and ship modes as data, not 11 DSP classes.
2. **Cheap differentiators are sitting in the open.** The Microcosm's pitch is octaves-only
   (×½/×1/×2/×4 resampling — no semitone shifting anywhere), its firmware has been frozen since
   May 2022, and its onset detection is its most-complained-about subsystem. Semitone pitch, a
   living firmware, and a robust trigger layer are all directly requested by its community.
3. **The market wedge is verified.** No granular pedal ships a desktop plugin twin, and nobody
   combines true stereo + deep granular control + open firmware at a fair price. Red Panda's
   Particle 2 is the UI pattern to copy (accessible performance layer + hidden expert mode);
   Walrus Lore is the anti-pattern.
4. **Hardware: target the Daisy Seed3** (pin-compatible with Rev7, TAC5242 codec, USB-C), socket
   it, and start the analog front end from Electrosmith's own measured instrument-level stereo
   I/O reference circuit in the Seed3 datasheet. Latching relays for true bypass; CD4051/CD4021
   for the control surface; plan ~300–420 mA at 9 V.
5. **Memory placement is a first-order design constraint.** Same algorithm: ~12% CPU in SRAM vs
   ~42% in SDRAM. Long grain buffers live in the 64 MB SDRAM; the reverb tank (Dattorro-class,
   50–150 KiB — not DaisySP's LGPL, 4×-oversized `ReverbSc`) lives in internal SRAM. Loop audio
   outgrows the 8 MB QSPI; loops belong on SD card.
6. **DSP core contract:** float32 everywhere, no allocation in the audio path,
   `Init(sampleRate, maxBlockSize)` / `Process()` interface, block size a runtime parameter
   (libDaisy itself treats it as one — the compile-time-constant recommendation in
   grain-delay-theory.md is overruled by primary source). VST3's SDK went MIT in Oct 2025;
   iPlug2 is the lead plugin-framework candidate, DPF second, JUCE viable.

   > **Update (2026-10-05):** the framework sentence is superseded; the core contract above
   > still holds. The project owner chose **JUCE**, in one C++ monorepo with `dsp/`, the
   > firmware and the app. JUCE is used under AGPLv3 and only in desktop targets under
   > `plugin/`; one JUCE audio processor is built as the standalone companion app and as DAW
   > plugins. See [vst-and-shared-dsp.md](vst-and-shared-dsp.md), recommendation 1, and
   > [companion-app.md](../design/companion-app.md), "One audio processor, several formats"
   > and "Licensing".
7. **Trigger layer v1:** spectral flux via CMSIS-DSP (~1–1.5% CPU) with adaptive whitening, a
   multiband time-domain path for latency-critical grain modes, a Chroma-Console-style
   auto-calibration routine, a visible trigger indicator, and selectable non-audio fallback
   sources (clock/stochastic/MIDI/footswitch/sidechain). No published algorithm fully solves
   distorted-guitar + slow-pad onsets — do not market it as solved; `bonk~` (BSD) is the
   adaptable reference implementation, aubio/Essentia are study-only (GPL/AGPL).

   > **Update (2026-10-05):** with the project licensed GPLv3, aubio (GPLv3) is vendorable;
   > Essentia (AGPLv3) stays study-only. See the licence note under "Known risks".
8. **Pricing honesty:** "stereo, open, under $350" holds as a kit claim (~$150–230 curated kit)
   and trivially as bare-PCB-plus-BOM (~$20–30), but a fully-loaded assembled unit honestly
   lands at $300–450. Recommended form: tiered — free design files → bare PCB → DIY kit →
   small-batch assembled. Panel-mount hand-labor, not the Seed3 (~$24–30), dominates cost.
9. **Presets:** permanent numeric parameter IDs, plain (denormalized) stored values,
   per-key-tolerant JSON — human-readable, git-diffable presets would be a first in this
   category. Never save from the audio thread (libDaisy's `PersistentStorage::Save()` blocks).

   > **Update (2026-10-05):** superseded for storage and parsing. JSON is the desktop authoring
   > and exchange format only: the companion app and its command-line compiler, `bspc`, compile
   > it into binary `.bsp` preset packages, and the firmware decodes and structurally validates
   > packages without ever parsing JSON. User presets live in a microSD slot store; QSPI flash
   > holds only the firmware image, because a QSPI write stalls execution. Permanent parameter
   > IDs and plain stored values still hold. See [companion-app.md](../design/companion-app.md),
   > "The `.bsp` package", "Compilation happens on the desktop only" and "Pedal storage: a
   > microSD slot store with A/B copies", and the update notes in
   > [preset-parameter-and-patch-format.md](preset-parameter-and-patch-format.md).

## Known risks and open questions

- **Daisy supply concentration:** the most expensive BOM line is single-sourced from one small
  manufacturer that restructured pricing in 2026 (DRAM costs) and sells direct-only.
- **License not yet chosen** (GPLv3 leading candidate) — it gates which reference code is
  vendorable vs study-only; decide before code lands.

  > **Update (2026-10-05):** decided: the project is GPLv3 ([LICENSE](../../LICENSE)). The
  > resulting posture on reference code is in [grain-engine.md](../design/grain-engine.md)
  > §11, "License posture"; under it aubio (GPLv3) becomes vendorable, though the engine's
  > onset detector is its own in-tree implementation, while Essentia (AGPLv3) stays
  > study-only. The firmware-side licensing problems found since (libDaisy's USB and SD-card
  > code) are in [companion-app.md](../design/companion-app.md), "Why not libDaisy's USB, and
  > the SD-card glue problem".
- Per-document "Open questions" sections list the rest — notably: no Microcosm teardown exists
  (DSP chip unknown), its latency has never been measured, and the complex-domain onset
  detector's advantage under real guitar distortion needs a listening test.
