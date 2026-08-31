# The Hologram Electronics Microcosm — Deep Reference

> Research document for **Brainscape**: an open-source granular delay pedal (Daisy Seed firmware + open hardware) and desktop VST, from one shared platform-agnostic C++ DSP core.
> Primary source for all functional detail below is the **official Microcosm user manual v1.13 PDF** unless otherwise noted. Inferences and community claims are marked.

---

## Summary

- The Microcosm is Hologram Electronics' stereo "granular looper & glitch pedal" — announced Feb 2020, shipped from ~May 2020, **$459 USD**, still in production in 2026 (a 10-Year Anniversary edition of 350 units and a Matte Black colorway exist). It is the reference point for the whole modern granular-pedal category.
- Architecture: **11 effects in 4 categories, each with 4 variations (A–D) = 44 factory variations**, plus **16 user presets** in 4 colour-coded banks. Categories are **Micro Loop** (Mosaic, Seq, Glide), **Granules** (Haze, Tunnel, Strum), **Glitch** (Blocks, Interrupt, Arp), **MultiDelay** (Pattern, Warp).
- Signal path is fixed and simple: `INPUT → [+] → EFFECTS → MODULATION → REVERB → FILTER → [+] → OUTPUT`, with the **Phrase Looper injectable at either `[+]`** (Pre-FX or Post-FX).
- Eight knobs — **Activity, Shape, Time, Repeats, Space, Filter, Mix, Loop Level** — each with a shift-accessed secondary function. **Activity and Repeats are explicitly documented as multi-parameter macros**, remapped per effect. This is the pedal's central design decision.
- All pitch manipulation is **binary resampling only** (×1/2, ×1, ×2, ×4 → −1 oct / unison / +1 oct / +2 oct). There is no arbitrary/semitone pitch shifting anywhere in the pedal. *(Inference from the manual's own variation descriptions — every pitch statement is in octaves.)*
- **Rhythmic quantisation is the differentiator.** Tap tempo, Subdiv (1/4, 1/2, TAP, 2x, 4x, 8x), and MIDI clock lock the granular scheduler to a grid — this is why the glitches sound composed rather than random, and it is the single most-praised musical trait.
- The **60-second stereo Phrase Looper is a first-class feature**, not an add-on: infinite overdub, undo, pre/post-FX routing, quantise-to-effect-tempo, 1/4x–4x speed, reverse, Burst mode, Looper Only mode, and **loops are saved inside user presets**. The resample workflow (record post-FX, then re-process pre-FX) is a genuine compositional loop.
- A separate **Hold Sampler** freezes recent audio and **continuously re-feeds it into the effects section**, so the effect keeps evolving on the frozen material while you play over it. Mutually exclusive with the Phrase Looper.
- **Full MIDI:** 34 CCs, PC 1–60 (all 44 variations + 16 user presets), MIDI clock in *and* out, thru, selectable channel, and firmware updates over SysEx.
- Audio spec: **48 kHz / 24-bit**, input impedance 500 kΩ, output < 1 kΩ, >450 mA at 9 V, single TRS stereo input, dual TS outputs, true-bypass relays or buffered/trails.
- **Firmware has been frozen at v1.13 since May 2022.** No new algorithms, no editor, no user content have ever been added. Hologram moved on to the Chroma Console. This is the biggest strategic opening for an open-source successor.
- The dominant technical criticism is **fragile note-onset detection**: the pedal is "VERY picky about incoming audio" and can go silent with distorted guitar, thick pads, or sustained/low-transient sources. Effects built on onsets (Strum, Arp, Blocks, Interrupt) suffer most.
- Other consistent criticisms: inconsistent gain staging between presets, latency before granular effects "kick in", the Shape knob often doing nothing audible, knob positions not matching loaded presets (no soft takeover), preset switching interrupting loop playback, tiny buttons, single TRS stereo input, LP-only filter, no expression range limits, and no way to footswitch the effect independently of the looper.
- The most-requested feature by far is a **desktop editor with deeper algorithm access and the ability to combine algorithms** — exactly the gap a shared-core pedal-plus-VST project is structurally positioned to fill.

---

## 1. What it is, positioning, and specifications

Hologram Electronics (Knoxville, TN) announced the Microcosm in February 2020 and began shipping around May 2020 after COVID-related delays. It sells **direct-to-consumer only** (Hologram's own site and their Reverb page) — no dealers — which contributed to early availability complaints.

The marketing line is: "Microcosm rearranges and reinterprets your sound in new and exciting ways using a variety of granular sampling, delay, and looping techniques."

### Specifications

| Spec | Value | Source |
|---|---|---|
| Price | $459 USD | Hologram product page |
| Sample rate / bit depth | **48 kHz / 24-bit** | Hologram FAQ |
| Power | 9 V DC, 2.1 mm, centre-negative; manual says **400 mA min**, the support FAQ says **>450 mA** | manual / FAQ (*discrepancy — treat 450 mA as the real figure*) |
| Input impedance | 500 kΩ | Hologram FAQ |
| Output impedance | < 1 kΩ (recommends ≥10 kΩ next stage) | Hologram FAQ |
| Dimensions | 7.1 × 4.7 × 2.0 in | Vintage Technology Archive / retailers (*unverified against Hologram*) |
| Weight | ~2.0 lb | Vintage Technology Archive (*unverified*) |
| DSP / processor | **Unknown — no teardown found** | — |
| Input | 1 × TRS mono/stereo jack, instrument or line level | manual |
| Outputs | 2 × TS (L, R) | manual |
| Expression | 1 × TRS: tip 0–3 V in, ring 3 V out, sleeve 0 V; pot ≥10 kΩ | manual |
| MIDI | 5-pin IN, 5-pin OUT/THRU | manual |
| Looper | 60 s, stereo | manual |
| Bypass | True bypass (2 relays, L/R independent) or buffered, with optional trails | manual |
| Warranty | 1 year | Hologram support |

**On "Line Level" mode:** the FAQ clarifies this **attenuates the incoming signal — it does not change input impedance**, and headroom is essentially identical between Instrument and Line modes. Useful precedent: pedal-level input padding is a mode switch, not an impedance change.

### Variants

- Standard colourway (white/cream)
- **Matte Black Colorway** (special edition)
- **10-Year Anniversary Edition**, limited to 350 units (2026)

All are functionally identical.

---

## 2. Physical layout and controls

### 2.1 Panel

- **Left side:** the eight effect knobs (two rows of four) plus the SELECT/SHIFT button.
- **Centre:** the Indicator Lights — a grid of LEDs that doubles as the effect selector, the level meter, and the entire menu system. Each of the 11 effects has a slot with four sub-slots (A–D); an **amber cursor** shows position, and surrounding bars animate on selection. **There is no screen.**
- **Right side:** the Preset Selector rotary encoder, the Phrase Looper button, and the Reverse button.
- **Bottom:** three footswitches.

Effect categories are colour-coded in the artwork and iconography: Micro Loop (blue), Granules (cyan), Glitch (red), MultiDelay (amber).

### 2.2 Preset Selector (rotary encoder)

| Gesture | Function |
|---|---|
| Rotate | Scroll through effects / variations / menus. Configurable: **circular** or **two-row list** scrolling. |
| Press | **FWD/REV** — reverses effect playback direction; lights animate to show direction |
| Press & hold | Copy current settings + active loop (start of the preset-save flow) |
| Press ×3 rapidly (in Global Config) | Toggle scroll direction style |
| Rotate/press (in Looper Config or Global Config) | Navigate and select |

### 2.3 SELECT (tap) / SHIFT (hold)

- **Tap** → toggles the Time knob between **Subdiv** and **Tempo** modes; an LED under the button shows which is active.
- **Hold** → Shift: exposes every knob's secondary control (labelled in grey on the panel).

### 2.4 Footswitches

| Switch | Effects mode | Phrase Looper mode |
|---|---|---|
| Left | **Tap Tempo** (matches quarter-note taps, smoothly) | **REC / PLAY / DUB**; hold = erase Overdub Layer |
| Middle | **Bypass** | Bypass |
| Right | **Hold Sampler** (toggle or momentary) | **STOP**; hold = **ERASE** loop |

### 2.5 The eight knobs

| Knob | Primary | Secondary (hold Shift) |
|---|---|---|
| **Activity** | Density/complexity of the effect. "Turn clockwise to add complexity and variety." Meaning is remapped per effect. | — (Global Config: Input Mode) |
| **Repeats** | Effect duration or frequency; works in tandem with Activity | **Mod Depth** |
| **Shape** | "Applies a contour to the volume or filter characteristics of the effect" | **Mod Rate** |
| **Time** | **Subdiv** (musical subdivision of the effect) or **Tempo** (global tempo; subdivision forced to quarter notes, allows smooth accel/decel) | **Looper playback speed** |
| **Filter** | Low-pass cutoff. Fully CW = filter bypassed; CCW = kills all effect | **Filter resonance** |
| **Mix** | Balance between input signal and effect | **Effect Volume** (effect master volume; does not affect dry) |
| **Space** | Mixes in **reverberation and delay**; fully CW = 100% wet | **Reverb Time** — also selects 1 of 4 reverb modes |
| **Loop Level** | Phrase Looper playback volume | **Loop fade time** (fade direction set globally) |

Time subdivisions (from the MIDI CC table): **1/4, 1/2, TAP, 2x, 4x, 8x** — six steps.

Note that **Tap Tempo still works while Tempo mode is active**, and Tap Tempo is **disabled while synced to external MIDI clock**.

### 2.6 Expression

Plug an expression pedal in while the unit is powered on → Indicator Lights flash blue → move the control you want to assign. Assignable to **Activity, Shape, Filter, Mix, Repeats, Space, or Loop Level**. Persists across power cycles; to keep the existing assignment, plug in before power-on or don't touch anything while the lights flash.

**Gotcha documented in Hologram's own FAQ:** if an expression pedal is connected at power-on and no assignment is made, it defaults to **Filter**, typically heel-down = filter fully closed — which makes the pedal appear to produce **no wet signal at all**. This is their single most common support case.

### 2.7 I/O and routing

| Configuration | How |
|---|---|
| Mono in / mono out | Default. Instrument cable to the TRS input, instrument cable from OUTPUT L |
| Mono in / stereo out | Instrument cable in, both OUTPUT L and R out. **No global setting change needed.** |
| Stereo in / stereo out | Select **Stereo** input mode in Global Config, use a TRS (insert/Y) cable in |
| Stereo in / mono out | Use OUTPUT L only — sums to mono |

Hologram's FAQ flags the common failure: setting Stereo input mode while feeding a mono source results in **left channel only**.

---

## 3. Signal path

The manual's block diagram (p. 13) is:

```
                                       ┌──────────────────────────────────────┐
   INPUT ──►(+)──► EFFECTS ──► MODULATION ──► REVERB ──► FILTER ──►(+)──► OUTPUT
             ▲                                                       ▲
             │                                                       │
        PRE-FX ─────────────────► [ LOOPER ] ◄──────────────── POST-FX
```

Key structural facts:

- The **granular/glitch engine is a single block** — one algorithm at a time, no parallel or serial chaining of effects.
- **Modulation, Reverb and Filter are fixed, always in that order, always after the effects block.** They cannot be reordered or (per user reports) fully removed from the path other than by turning their controls down.
- The **Filter is the last thing in the chain** and at fully-CCW it kills all effect output — it is effectively a wet-kill control as well as a tone control.
- The **Looper is the only routing choice**, and it is binary: inject before the effects block (Pre-FX) or at the output (Post-FX).
- **Mix** blends dry input against the wet chain; **Effect Volume** (Shift+Mix) trims the wet path independently.
- **Space** covers both a stereo delay and the stereo reverb on one knob.

---

## 4. The 11 effects and all 44 variations

*Descriptions below are the manual's own wording, condensed. Algorithmic interpretation follows in §4.5.*

### 4.1 Micro Loop
> "Layers of short loops, played back at various speeds, combine to form new rhythmic phrases and tonal colors."

**MOSAIC** — Overlapping loops play back at multiple speeds.
*Activity: determines the number of active loopers.*

| | |
|---|---|
| **A** | Micro loops at normal and double speed → octave-up harmonies |
| **B** | Loops at normal and half speed → one octave below input |
| **C** | All loops at double speed |
| **D** | Loops at half, normal, double and quad speed — one octave below to two octaves above |

*Mosaic is widely described in reviews as "the most famous effect" on the pedal — the sound people buy a Microcosm for.*

**SEQ** — Short looping samples are rearranged into new rhythmic sequences.

| | |
|---|---|
| **A** | Live samples filtered and shuffled into random rhythms. *Activity: introduces filter variations* |
| **B** | Sequenced samples alternate between normal and half speed. *Activity: bounce between the two speeds; fully CW adds a **sustainer layer** so soft pads sit under the sequence* |
| **C** | Overlapping layers of samples with filter sweeps. *Activity: adds layers of samples* |
| **D** | Interlocking layers of rhythmic samples and bit-crushing. *Activity: adds rhythmic layers; fully CW introduces bit-crushed sub-octave samples* |

**GLIDE** — Short overlapping loops shift in pitch over time.
*Activity: controls the rate of pitch-shifting. **Shape: determines the shape of the Glide pattern** (the only place the manual gives Shape a per-effect meaning).*

| | |
|---|---|
| **A** | Glides between half speed and normal speed |
| **B** | Glides between double speed and half speed |
| **C** | Glides between normal speed and double speed |
| **D** | Overlapping samples glide in **both directions simultaneously** |

### 4.2 Granules
> "Fragments of sound create giant atmospheres and subtle textural effects."

**HAZE** — Clusters of grains create a wash of sound.
*Activity: controls **grain density and spread**.*

| | |
|---|---|
| **A** | Short, diffused effect from stretching overlapping samples |
| **B** | Many simultaneous randomized grains → diffused textural effects |
| **C** | Mixture of normal- and double-speed grains |
| **D** | Mixture of normal- and half-speed grains |

**TUNNEL** — Cyclical micro-loops generate hypnotic drones with unique modifiers.
*Activity: determines the depth of each modifier. Repeats: how long the drone takes to decay.*

| | |
|---|---|
| **A** | Drone sample length compresses and lengthens |
| **B** | Sub-octave drone with filter sweeps |
| **C** | Drone samples get resonant band-pass filters |
| **D** | **Envelope-triggered** compressing/lengthening of drone sample length |

**STRUM** — Rhythmic chains of recent **note onsets** create pointillistic textures.
*Activity: controls density of pattern.*

| | |
|---|---|
| **A** | Repeats the most recent note continuously |
| **B** | Many copies of the most recent onset overlap → phasing effects |
| **C** | Cascading chains of recent note onsets |
| **D** | Cascading chains of recent onsets plus double-speed grains |

### 4.3 Glitch
> "Real-time rearrangements of your incoming signal play back at random or controlled intervals."

**BLOCKS** — Incoming audio triggers predictable glitches or random bursts of notes.
*Activity: controls the amount of variation and sample manipulation.*

| | |
|---|---|
| **A** | Rearranges your playing, adds bursts of sequenced runs of recent notes |
| **B** | Rearranges and pitch-shifts incoming signal |
| **C** | Filtered samples overlap and fade out → softer, less angular glitch |
| **D** | Rearranges using pitch-shifts and bit-crushing |

**INTERRUPT** — Glitches interrupt dry signal with pitch-shifted bursts, micro-montages and other modifiers.
*Activity: variation & sample manipulation. **Repeats: how often glitches are triggered**.*

| | |
|---|---|
| **A** | Glitches and rearranged versions of your playing interrupt live signal |
| **B** | Interruptions include various pitch-shifted samples |
| **C** | Interruptions include filter sweeps and delay |
| **D** | Bit-crushing and more drastic sample manipulations |

> Manual note: with **Mix at 100%**, Interrupt truly "interrupts" the input by **muting the dry signal**. This is the one effect where Mix has a semantic, not just proportional, role.

**ARP** — Sequences samples of recent note onsets into arpeggios.
*Activity: **determines the number of steps in the arpeggio**.*

| | |
|---|---|
| **A** | Basic arpeggios from the most recent samples of your playing |
| **B** | Arpeggios include various playback speeds → pitch-shifted pattern |
| **C** | Each sample in the arpeggio gets a **random filter value** |
| **D** | Bit-crushing added to the arpeggios |

### 4.4 MultiDelay
> "A delay line with a selectable number of taps creates a wide variety of rhythmic and textural effects."

**PATTERN** — Delay taps arranged into four rhythmic patterns.
*Activity: controls the number of active delay taps.*

| | |
|---|---|
| **A** | Rhythmic pattern 1 — **classic, linear delay** (the pedal's "normal delay" setting) |
| **B** | Rhythmic pattern 2 |
| **C** | Rhythmic pattern 3 |
| **D** | Rhythmic pattern 4 |

**WARP** — Delay taps manipulated with filters and pitch shifting.
*Activity: controls the number of active delay taps.*

| | |
|---|---|
| **A** | Envelope-controlled filter on each delay tap |
| **B** | Resonant band-pass filter on each delay tap |
| **C** | Pitch shifting added to delay taps |
| **D** | Delay taps cross-fade with double-speed grains |

### 4.5 Algorithmic reading (inference)

*The following is my structural interpretation of what the 11 effects actually are, derived from the manual's own descriptions plus how they behave in demos. Hologram publishes no DSP details, so **treat this section as informed inference, not verified fact.** It is included because it is the most useful part of this document for building Brainscape's DSP core.*

**All 11 effects appear to share one engine.** The differences between them are almost entirely in (a) the **scheduler** that decides when voices fire and how long they live, and (b) the **per-voice modifier chain**. The core is:

```
  circulating input buffer (recent audio history)
        │
        ├─► voice 1 ─► [resample ×0.5/×1/×2/×4] ─► [window/env] ─► [modifier] ─┐
        ├─► voice 2 ─► ...                                                    ├─► sum ─► stereo spread
        └─► voice N ─► ...                                                    ┘
        ▲
   scheduler: clock-quantised, onset-triggered, or stochastic
```

Evidence for this reading:

1. **Every pitch statement in the manual is an octave relationship** (half / normal / double / quad speed = −1 / 0 / +1 / +2 octaves). That is variable-rate table playback, not a phase vocoder or PSOLA. It is cheap, it is why the pedal never pitch-shifts by a fifth or a third, and it explains the characteristic "octave stack" Microcosm sound. **Brainscape's single biggest sonic differentiator is available here for almost free.**
2. **Activity ≈ number of concurrent voices / event density.** The manual says so directly in different vocabulary per effect: Mosaic "number of active loopers", Pattern/Warp "number of active delay taps", Haze "grain density and spread", Arp "number of steps in the arpeggio", Seq "adds layers".
3. **Repeats ≈ voice lifetime / decay / retrigger rate.** Tunnel: "the amount of time it takes for the drone to decay". Interrupt: "how often glitches are triggered". The manual's global description — "determines the effect duration or frequency" — covers both readings.
4. **Shape ≈ the grain/voice envelope (window) and filter contour.** "Applies a contour to the volume or filter characteristics of the effect." In Glide it becomes the *trajectory* shape of the pitch ramp. This also explains the widely-reported complaint that Shape often does nothing audible: in effects whose windows are already fixed by the algorithm, there is little for it to do.
5. **Three distinct trigger sources exist across the effect set:**
   - *Clock-quantised* — Seq, Arp, Pattern, Warp, Mosaic. Grid-locked to Subdiv/Tempo.
   - *Onset-triggered* — Strum, Arp, Blocks, parts of Interrupt and Tunnel D. Requires transient/envelope detection to pick capture points ("recent note onsets", "envelope-triggered", "envelope-controlled filter").
   - *Stochastic* — Haze B ("randomized grains"), Seq A ("random rhythms"), Arp C ("random filter value"), Blocks ("random bursts").
   The onset-triggered family is exactly where the community reports the pedal failing on distorted, sustained or dense sources. **The trigger layer is the weak point of the design.**
6. **The per-voice modifier vocabulary is small and reused everywhere:** resample (octaves), low-pass, resonant band-pass, filter sweep, bit-crush, sub-octave, reverse, cross-fade with grains, sustainer/pad layer. Eleven "effects" × four variations is largely a curated cross-product of {scheduler} × {modifier}.
7. **MultiDelay is the same engine with voice positions pinned to a delay grid.** Pattern = tap rhythm presets; Warp = per-tap modifiers. Pattern A being "classic, linear delay" is the degenerate case where all voices are ×1 speed on an even grid — strong evidence the delay and the granular engine are the same code.
8. **Reverse (Preset Selector press) reverses effect playback**, i.e. voices read backwards through the buffer. It is a property of the read head, not a separate algorithm.
9. **Latency behaviour supports the buffer model.** Reviewers report "sometimes it needs a couple of seconds before the effect kicks in" on short parts. That is not I/O latency — it is the engine needing enough recent history in the buffer before voices have anything to play.

**Implication for Brainscape:** you do not need eleven algorithms. You need **one voice engine + one scheduler with three trigger modes + a modifier chain**, and then *44 presets* over that. This is a far smaller and far more maintainable core than "11 effects," and it is almost certainly what Hologram actually built.

---

## 5. Hold Sampler

The right footswitch (in effects mode) captures and sustains **the most recent segment of your playing indefinitely**. Properties:

- Held samples **persist while you change effects and parameters** — you can freeze on Tunnel and then scroll to Blocks and keep the frozen material.
- The sampled material is **continuously fed back into the effects section**, so the effect keeps dynamically processing it. This is the key distinction from a simple freeze/sustain: it is a *source*, not a *tail*.
- Mixing in dry lets you play over the sustained texture.
- Configurable **Toggle** or **Momentary** (Global Config → Repeats knob).
- **Mutually exclusive with the Phrase Looper.** Activating the looper clears whatever the Hold Sampler captured, and the Hold Sampler is inaccessible while the looper is active.

That mutual exclusivity is a real limitation and an obvious extension target — there is no architectural reason a freeze buffer and a phrase loop cannot coexist.

---

## 6. Phrase Looper

### Core operation

- **Up to 60 seconds** for the initial phrase; the Phrase Looper button blinks during the last 5 seconds.
- **Infinite overdub layer.** Indicator Lights: red = recording, green = playing, yellow = overdubbing.
- **Undo** = hold left footswitch → erases the Overdub Layer, retains the initial phrase (lights flash yellow).
- **Stop** = press right footswitch (retains recording). **Erase** = hold right footswitch (lights flash red/green alternately).
- **Reverse** button reverses playback. If enabled *while recording* the initial phrase, the phrase plays back reversed.
- **Playback speed** = hold Shift + turn Time. In **Subdiv** assignment it steps through fixed speeds; in **Tempo** assignment it's a fluid shift from **1/4x to 4x**. "TAP" (1×) returns to normal.
- The Phrase Looper button light: **pulsing** = armed and ready to record; **solid** = material stored.

### Looper configuration (hold the Phrase Looper button)

| Option | Behaviour |
|---|---|
| **Pre-FX / Post-FX** | Default **Post-FX**: the looper records the pedal's effects output and plays back at the output. **Pre-FX**: the looper is placed in front of the effects block, so recorded material is processed like the instrument input — you manipulate the phrase without altering the recording. Saved in User Presets. |
| **Quantize** | Trims the loop to the nearest quarter note to sync with effect tempo. Effect tempo must be set *before* recording; starting recording syncs effect and looper start points. Changing global tempo desyncs; switching presets resets global tempo to the quantised loop's tempo, re-syncing. Subdivision can change freely. |
| **Looper Only** | Mutes the effects — the pedal becomes a traditional looper. **Pitch Modulation, Reverb and Filter stay active**, and Mix blends. |
| **Burst** | Momentary recording: records only while the left footswitch is held; playback starts on release; pressing again deletes and immediately re-records. **Disables overdub.** Burst phrases are not saved into User Presets unless Burst is deselected after recording. |

Configurations can be toggled at any time **without disrupting looper playback**, and they persist across power cycles and are saved into presets.

### Order of operation (Global Config)

- **Rec > Play > Overdub** (default)
- **Rec > Overdub > Play** — begin recording new material the moment you close the loop

### The resampling workflow

The manual explicitly suggests it: *"Try recording an effect in the default post-FX mode, then sending that loop back through the effects again."* Record the granular output post-FX, switch routing to pre-FX, and the loop now feeds the granular engine — a two-generation resample. Combined with saving loops into presets, this is the compositional heart of the pedal and is what reviewers mean when they call the looper "not an afterthought."

---

## 7. Presets

- **16 slots in 4 colour-coded banks** (Red, Yellow, Green, Blue), reached by scrolling past the effects to the "User" bank.
- Presets retain **all parameters and any recorded loop**. Overdubs are **"mixed down" into a single loop file** so more overdubs can be layered on top later.
- **Global Configuration settings are NOT saved with presets.**

**Save:** hold Preset Selector until lights flash blue (settings + loop copied) → scroll to the target slot → hold again to save. Click to cancel. Longer loops take longer to save.

**Preview before overwriting:** while settings are held in the copy buffer, the pedal still operates and loads presets, so you can audition what you're about to overwrite.

**Copying loops out of presets:** idle the cursor over a user preset, hold the Preset Selector (copies the loop), navigate to an effect, hold again — the loop is transferred to the live effect section. **Only the loop transfers, not the settings**, and it overwrites any loop currently held.

### Known preset friction (documented in the manual itself)

> "Knob position does not necessarily reflect the actual settings when using saved presets. The User presets retain settings from the moment they were saved. If you want to tweak a parameter, simply turn the knob and the setting will be updated to the actual knob position."

There is **no soft takeover / pickup / relative mode**. Touching any knob after a preset load causes a jump. This is the single most fixable UX defect in the pedal.

---

## 8. MIDI implementation

### 8.1 Control Change

| CC | Function | Range |
|---|---|---|
| 5 | Subdiv | (0)=1/4, (1)=1/2, (2)=TAP, (3)=2x, (4)=4x, (5)=8x |
| 6 | Activity | 0–127 |
| 7 | Shape | 0–31 / 32–63 / 64–95 / 96–127 (four steps) |
| 8 | Filter | 0–127 |
| 9 | Mix | 0–127 |
| 10 | Time | 0–127 |
| 11 | Repeats | 0–127 |
| 12 | Space | 0–127 |
| 13 | Loop Level | 0–127 |
| 14 | Mod Freq. | 0–127 |
| 15 | Filter Resonance | 0–127 |
| 16 | Effect Volume | 0–127 |
| 17 | Looper Playback Speed | 0–127 |
| 18 | Looper Playback (Stepped) | (0)=1/4, (1)=1/2, (2)=TAP, (3)=2x, (4)=4x, (5)=8x |
| 19 | Mod Depth | 0–127 |
| 20 | Reverb Time | 0–127 |
| 21 | Looper Fade Time | 0–127 |
| 22 | Looper On/Off | Off 0–63, On 64–127 |
| 23 | Looper Playback Dir. | Fwd 0–63, Reverse 64–127 |
| 24 | Looper Routing | Post-FX 0–63, Pre-FX 64–127 |
| 25 | Looper Only | Looper & Effects 0–63, Looper Only 64–127 |
| 26 | Looper Burst | Default 0–63, Burst 64–127 |
| 27 | Looper Quantized | Free 0–63, Quantize 64–127 |
| 28 | Looper Record | 0–127 |
| 29 | Looper Play | 0–127 |
| 30 | Looper Overdub | 0–127 |
| 31 | Looper Stop | 0–127 |
| 34 | Looper Erase | 0–127 |
| 35 | Looper Undo | 0–127 |
| 45 | Copy Preset | 0–127 |
| 46 | Save Preset | 0–127 |
| 47 | Reverse Effect | 0–127 |
| 48 | Hold Sampler | Off 0–63, On 64–127 |
| 93 | TAP Tempo | 0–127 |
| 102 | Bypass | 0–63 bypass, 64–127 engage |

**Note the CC-numbering hazard:** CC#7 is Shape and CC#10 is Time — these collide with the MIDI-standard meanings of Channel Volume (7) and Pan (10). A DAW writing standard automation on the same channel will fight the pedal. Worth avoiding in Brainscape's map.

Notable **gaps**: no CC for effect/variation selection (PC only), no CC for the reverb *mode*, no relative/increment messages, no CC for Global Config settings.

### 8.2 Program Change

PC #1–60 in this order — note it is the **reverse** of front-panel reading order:

| PC | Target |
|---|---|
| 1–4 | ARP A–D |
| 5–8 | INTERRUPT A–D |
| 9–12 | BLOCKS A–D |
| 13–16 | GLIDE A–D |
| 17–20 | SEQ A–D |
| 21–24 | MOSAIC A–D |
| 25–28 | HAZE A–D |
| 29–32 | TUNNEL A–D |
| 33–36 | STRUM A–D |
| 37–40 | PATTERN A–D |
| 41–44 | WARP A–D |
| 45–48 | USER BANK 1 A–D |
| 49–52 | USER BANK 2 A–D |
| 53–56 | USER BANK 3 A–D |
| 57–60 | USER BANK 4 A–D |

### 8.3 Clock and Thru

- Accepts MIDI clock on MIDI IN. **MIDI Start** switches the pedal from internal to external clock; **MIDI Stop** reverts to internal. **Tap Tempo is unavailable while externally synced.**
- Can also **transmit** MIDI clock on MIDI OUT.
- Echoes MIDI IN → MIDI OUT by default (soft thru), so one MIDI cable can feed a chain. Listens on **Channel 1** by default.
- Thru/clock behaviour is a four-way global setting: send clock + thru / don't send clock + thru / send clock + no thru / neither.
- **Firmware updates are delivered over MIDI SysEx** — via an in-browser WebMIDI installer or a downloadable `.syx`.

---

## 9. Global Configuration

Enter by holding **Shift + Phrase Looper** for 2 s (amber animation confirms). Each of the eight knobs opens a menu; **move the knob**, then **use the Preset Selector to scroll**, then **press the Preset Selector to confirm** — all three steps are required, and Hologram's FAQ notes steps 3 and 4 are the most commonly missed. Exit with the Bypass footswitch or Shift + Phrase Looper.

| Knob | Menu | Options (factory default in bold) |
|---|---|---|
| Activity | Input Mode | **Mono**, Stereo |
| Shape | Looper Shape | **Fade In/Out**, Fade In Only, Fade Out Only |
| Filter | MIDI Thru / Clock | **Send internal clock + transmit thru**, Don't send clock + transmit thru, Send clock + don't transmit thru, Neither |
| Mix | Input Level | **Instrument Level**, Line Level |
| Time | MIDI Channel | **1–4**, 5–8, 9–12, 13–16 |
| Repeats | Hold Style | **Toggle**, Momentary |
| Space | Bypass Style | **Buffered, no trails**, Buffered + trails, True Bypass |
| Loop Level | Looper Operation | **Rec > Play > Overdub**, Rec > Overdub > Play |

- **Scroll direction:** in Global Config, press the Preset Selector 3× rapidly; lights animate in the new direction.
- **Reset globals only:** hold Phrase Looper + Reverse for 3 s (presets and loops preserved).
- **Full factory reset:** hold Shift + Phrase Looper + Reverse for 2 s, confirm with the Preset Selector. **Clears all user preset banks and all loops.** Power-cycle afterwards.

---

## 10. Firmware history

| Version | Date | Identifying LEDs (Global Config → tap Reverse 3×) | Documented changes |
|---|---|---|---|
| v1.0 | ~May 2020 | none illuminate | shipping firmware |
| **v1.1** | Sept 2020 | one LED in the Glitch bank | Improved triggering/tracking for synths, drones, fuzz and low-transient sources. **More output gain in the effects section**, adjustable via the Effect Volume secondary control — at 100% granular effects can be much louder than the dry input. Pre-loaded on units ordered Aug 2020+. |
| v1.12 | undated | Mosaic, Haze, Tunnel | *changelog not published — unverified* |
| **v1.13** | **May 2022** | Mosaic, Haze, Tunnel, Strum | Current version. Units shipping June 2022+ have it. Hologram's FAQ states "triggering issues … were present in firmware version v1.1 and older", implying v1.12/v1.13 further improved onset detection. |

### The strategic observation

**The Microcosm has been feature-frozen since May 2022 — over four years.** In that time Hologram never shipped:
- a single new algorithm or variation
- a desktop editor
- user-loadable content of any kind
- USB firmware updates (the Chroma Console got those; the Microcosm never did)
- preset/loop backup or export

The pedal is still being manufactured and sold, and even got an anniversary edition — so this is not abandonment through lack of demand. It is a closed, finished product by design, and its owners have four years of accumulated unaddressed requests. **That is the opening Brainscape exists to fill.**

---

## 11. Why the Microcosm is beloved

1. **Instant musical results with zero programming.** The most-repeated point across every review. Sine Squares: it "provides instant gratification for anyone that plays a sound through it," with a "wow factor without messing with the knobs." Engadget frames it as effectively a cheat code for ambient music. Guitar World: "operation is surprisingly straightforward and simple" despite the sophistication. The 44 variations are *destinations*, not algorithm slots — you land on one and it's already finished.
2. **Macro controls instead of parameter soup.** The manual is unusually candid: "rather than controlling any one specific parameter, Repeats and Activity are both sophisticated macro controls that encompass many different parameters." The pedal deliberately hides granular synthesis' huge parameter space behind two knobs whose meaning is re-mapped per algorithm. It trades ceiling for floor — and the floor is very high.
3. **Rhythmic quantisation of glitches.** Tap tempo + Subdiv + MIDI clock put the granular chaos *on the grid*. Seq, Arp, Blocks and Pattern produce sequenced output locked to tempo. This is what separates it from stochastic granular boxes and makes it viable with a band or a drum machine — and it's why people describe results as "compositions" rather than "textures."
4. **The reverb is genuinely good and correctly placed.** Widely cited as good enough to replace a dedicated reverb. Because granular output is spiky and pointillistic, a quality reverb *downstream* is what turns fragments into wash. Space bundling delay + reverb on one knob also means one gesture takes you from dry sequence to ambient smear.
5. **The looper is a first-class citizen.** Sound On Sound and Waveform both single this out as the thing that isn't an afterthought. Pre/post-FX routing, quantise-to-effect-tempo, 1/4x–4x, reverse, undo, Burst, Looper Only, and loops saved inside presets. The resample workflow makes it a composition tool.
6. **The Hold Sampler is a genuinely different musical object.** A freeze whose captured material *keeps being processed by the effect* while you play over it — not a static sustain, not a reverb tail.
7. **Stereo end-to-end.** Stereo in/out, stereo reverb, stereo looper, at a time when most granular pedals were mono or mono-in/stereo-out.
8. **UI philosophy: lights, not screens.** The LED grid replaces a menu display. It keeps the pedal feeling like an instrument. Waveform notes the trade-off honestly — beautiful, but less informative about exactly which preset is loaded.
9. **It works on everything.** Reviewers and demo artists consistently succeed with synths, vocals, drum machines, bass, banjo, upright piano, omnichord and harp — Emily Hopkins' harp demo, Andy Othling's generative ambient sessions, Chords of Orion's "Frippertronics monster" videos, and dozens of others.
10. **Ownership stickiness.** Long-term forum reports describe it as a "killer pedal with a lot of personality and usefulness," with owners saying they "would never sell it (and I don't typically get attached to pedals)."
11. **The supporting cast, not just the granular engine.** Guitar Pedal X's framing is the sharpest: what makes it stand out "among a growing sea of granular and glitch pedals is the supporting cast of features" — reverb, modulation, resonant filter, and the looper. The granular engine alone would not have made it iconic.

---

## 12. Criticisms and community wishlist

### 12.1 Verified criticisms

| Criticism | Detail | Source |
|---|---|---|
| **Fragile onset detection** | The #1 technical complaint. "Struggles with heavily distorted guitar or thick synth pads despite firmware improvements." Forum users: it's "VERY picky about incoming audio" and when too much comes in "Microcosm is simply silent and adds nothing to the dry signal." v1.1 improved it, but ilovefuzz users report it "went back to being kind of random." | Engadget, TGP, ilovefuzz |
| **Wet-level / gain staging** | Launch firmware was too quiet; partially fixed in v1.1 via Effect Volume. Owners still report level varying widely preset to preset — "sometimes hard to trigger, sometimes very loud." | ilovefuzz, Elektronauts, Hologram FAQ |
| **Onset latency** | "Sometimes it needs a couple of seconds before the effect kicks in" when recording short parts. Inherent to buffer-fill, but a real limitation for short stabs. | Sine Squares, ModWiggler |
| **Shape knob often inaudible** | Controls "feel not that responsive," Shape specifically called out as sometimes producing no perceptible change. | Sine Squares |
| **No soft takeover** | Knob positions don't match loaded presets; touching a knob jumps the value. Documented in the manual as expected behaviour. | Manual |
| **Preset switching interrupts performance** | Scrolling through user banks (which contain loops) stops playback mid-performance; mode/preset changes cut trails. | Engadget, Elektronauts |
| **Small buttons; effect can't be toggled independently** | "Difficult to toggle granular effects on/off independently"; tiny buttons frustrate quick live adjustments. | Engadget |
| **Single TRS stereo input** | "Dual L-R input jacks would be nice"; stereo requires an insert/Y cable. | Sound On Sound, Engadget |
| **Expression range not definable** | One control, full sweep only — can't restrict the useful part of a filter sweep, so it's no good as a wah. | Engadget |
| **LED display is ambiguous** | Hard to identify precisely which preset is loaded. | Waveform |
| **Strum can be harsh** | "Can be unpleasant and harsh without a decent amount of reverb." | Engadget |
| **"The tool does the creative heavy lifting"** | Because everything sounds amazing, it can flatten authorship — Microcosm recordings tend to sound like Microcosm recordings. The flip side of curated presets. | Waveform |
| **Genre-limited** | Guitar World warns blues/rockabilly/country players will find limited use; the effects "dictate very distinct sounds." | Guitar World |
| **Too clean for some** | Players wanting lo-fi/analogue grit compare it unfavourably to OTO devices. | Elektronauts |
| **MIDI-only firmware updates** | Requires a MIDI interface; owners wanted USB. Hologram gave the *later* Chroma Console USB updates but never backported the idea. | ilovefuzz, Hologram firmware page |
| **Price and availability** | $459, direct-to-consumer only, no dealers, COVID-disrupted launch. | Hologram support, Elektronauts |
| **Confusing interface** | Reviewers of Hologram's *next* pedal described the Microcosm's interface as "baffling" by comparison — Hologram themselves treated interface clarity as the thing to fix in the Chroma Console. | Chroma Console reviews |
| **Slight delay when switching presets** | Preset recall is not instantaneous; combined with the playback interruption above, this makes live preset changes awkward. | Vintage Technology Archive, forum reports |
| **Early MIDI clock sync problems** | Early firmware reportedly showed "sync issues like drifting, clicks, and pops" when following external MIDI clock. Presumably improved by v1.13. *(Unverified against Hologram.)* | Vintage Technology Archive |
| **"Slightly overhyped"** | A recurring owner sentiment: reviews imply infinite versatility, but in practice it is "a collection of decent presets which can be tweaked" rather than an open-ended granular instrument. This is the honest ceiling of the macro-preset design. | Mod Wiggler / owner reports |
| **Bypass fade** | The bypass button applies a short fade-in rather than a hard switch — fine musically, but not a hard-cut gate. *(Unverified.)* | Vintage Technology Archive |

### 12.2 Community wishlist

Ranked roughly by how often it appears:

1. **A desktop editor with deeper algorithm access.** Sine Squares asks for it by name. This is the single most direct request.
2. **Ability to combine algorithms** — e.g. Mosaic's pitch-shifting *plus* Seq's sequencing. Currently strictly one at a time.
3. **Deeper parameter access under the macros** — grain size, density, spray, pitch set, window shape as real parameters.
4. **Independent footswitchable effect vs. looper.** "Being able to turn the granular effects on and off independent of the looper with a footswitch, or to be able to run the looper even when the pedal is off would be huge."
5. **Consistent gain staging / per-preset output normalisation.**
6. **Defeatable reverb** — a hard reverb bypass, and per-preset control over it.
7. **High-pass as well as low-pass filter** (or a morphing/state-variable filter).
8. **Expression pedal min/max range definition.**
9. **Non-interrupting preset switching** — preserve trails and loop playback across changes; preset preview/queue.
10. **Relative/stateful preset navigation over MIDI.** Morningstar MC controller users had to build elaborate workarounds (bank-enter initialisation plus global variables on the controller) because the Microcosm only accepts absolute PCs — there is no "next effect", "previous variation", or "recall last-used variation of effect N."
11. **MIDI coverage gaps** — at least one user wants tempo/division control beyond the stepped Subdiv CC and reports inconsistent MIDI responsiveness. *(Unverified; conflicts with the manual, which documents both CC#5 Subdiv and CC#10 Time.)*
12. **USB firmware updates.**
13. **Preset and loop backup/export.** Currently impossible — a factory reset destroys everything with no way to save it first.
14. **Effect order flexibility** — reordering reverb/filter/mod relative to the granular block. Hologram validated the demand by making re-orderable FX the headline feature of the Chroma Console.

### 12.3 What Hologram themselves did next (the Chroma Console signal)

The Chroma Console ($399, 2024) is instructive because it is Hologram's own answer to the Microcosm's limits:

- **4 re-orderable modules × 5 stereo effects each** — "easily re-order effects and experiment with different signal chains."
- **80 user presets** (vs. the Microcosm's 16).
- **Gesture recording** — knob movements captured and replayed.
- **USB-C** alongside MIDI In/Out/Thru, with in-browser USB firmware updates.
- **Drift** (musical randomness) and **Capture** (ephemeral looping).
- Reviewers describe it as the philosophical inverse: the Microcosm is preset-driven and hands you finished sounds; the Chroma Console gives you the tools and makes you build them.

Hologram thereby split the difference across two products rather than solving it in one. **A successor that delivers Microcosm-grade instant results *and* Chroma-grade configurability in the same box is the unoccupied position.**

---

## 13. Extension opportunities for an open-source successor

Grounded in §12. Ordered by leverage.

### 13.1 The two-tier parameter model (highest leverage)

Keep the macro layer *exactly* — Activity/Repeats/Shape as multi-parameter macros are the reason the Microcosm feels good, and abandoning them is the classic failure mode of "more powerful" successors (see ZOIA/Beebo, which reviewers describe as more capable but requiring patch-building). Then add a second tier:

- Every macro is a **user-editable mapping** from one knob to N underlying parameters, each with its own curve and range.
- The **editor and VST expose the full parameter tree**; the pedal front panel exposes only the macros.
- Macro definitions are data, saved with presets, shareable.

This satisfies "deeper access" and "an editor" without diluting the instant-results property. It is also the natural shape for a shared C++ core: one parameter graph, two front-ends.

### 13.2 Modes as data, not code

Given the §4.5 reading — one voice engine, one scheduler, a small modifier vocabulary — define a **mode** declaratively:

```
mode := { scheduler config, voice count/speeds, window/env, modifier chain, macro map, defaults }
```

Consequences:
- **User-loadable modes.** A mode is a file, not a firmware build. Load from SD/USB/editor.
- **Combining algorithms** becomes trivial in the cases people ask for — "Mosaic pitch set + Seq scheduler" is just mixing two fields of a mode.
- **Community patch sharing** becomes a repo of small text files, not a plugin ABI problem.
- The VST and firmware consume the identical mode files, so a patch made in the plugin runs on the pedal unchanged. **This is the killer feature no commercial competitor can match**, because it requires the shared core the project already plans.

### 13.3 Fix the trigger layer

This is the pedal's real weakness and the most valuable engineering win:

- **Multi-band / spectral-flux onset detection** rather than a single broadband envelope follower — this is what fails on distorted guitar and sustained pads.
- **User-adjustable sensitivity and threshold**, plus a visible trigger indicator so players can see why nothing is happening.
- **Selectable trigger source per mode:** onset / clock / stochastic / **MIDI note** / **manual footswitch** / external audio sidechain. The Microcosm hard-wires this per effect; making it a parameter multiplies the sound space and gives users a guaranteed-working fallback when onset detection fails.
- **Adaptive input gain / auto-calibration** (the Chroma Console has a calibration routine — the Microcosm does not).

### 13.4 Go beyond octaves

The Microcosm can only do ×1/2, ×1, ×2, ×4. Adding **arbitrary pitch ratios with scale/chord quantisation** — grains snapped to a user-chosen scale, chord voicings across voices, per-voice interval sets — produces sounds the Microcosm structurally cannot make, at modest DSP cost. This is the clearest "obviously better, obviously new" differentiator.

### 13.5 Buffer and looper generosity

The Daisy Seed has **64 MB SDRAM** (Electrosmith rates it at ~10 minutes of audio) and **96 kHz / 24-bit** hardware. A 60-second stereo loop at 48 kHz float is only ~23 MB. So:

- Multi-minute looper, or **multiple loop slots / parallel loopers** (a repeated request in the wider looper community).
- **Longer granular history buffer** than the Microcosm's, enabling grains sourced from much further back.
- **Freeze and phrase looper simultaneously** — remove the Microcosm's mutual exclusivity, which has no architectural justification.
- **Loop and preset export/import over USB** — backup, sharing, and DAW round-tripping. The Microcosm can't do this at all.

### 13.6 Performance-workflow fixes (cheap, high goodwill)

- **Soft takeover / pickup / relative knob modes** after preset load — selectable, default to pickup.
- **Non-interrupting preset changes** with crossfade; loop playback continues across mode changes.
- **Preset preview/queue** — audition and arm without committing.
- **Assignable footswitches**, including effect-bypass independent of looper, and looper active while the effect is bypassed.
- **Per-preset output trim / auto-normalisation** so variations are level-matched.
- **Expression min/max range per assignment**, plus multiple simultaneous expression targets.
- **Defeatable, reorderable post chain** — reverb bypass, filter type selection (LP/BP/HP/notch), and at minimum a filter-before-vs-after-reverb choice.

### 13.7 MIDI done properly

- Keep the Microcosm's CC map *conceptually* but **avoid CC#7 and CC#10** (Channel Volume / Pan collisions).
- Add **relative navigation**: next/previous effect, next/previous variation, recall last-used variation of effect N — exactly what Morningstar users had to fake.
- **MIDI notes trigger grains/voices**; MIDI clock in *and* out; song-position pointer.
- **CC learn** for every parameter, and **MIDI out of parameter changes** so the pedal can drive the editor.
- **USB-MIDI** in addition to DIN, and **USB firmware updates**.

### 13.8 Desktop editor + VST twin

- One shared core → the VST is not a "port", it is the same engine.
- Editor shows the parameter tree, the macro mappings, preset/loop management, and firmware updates.
- **Preset and mode files identical across pedal and plugin.** Write a patch in the DAW, drop it on the pedal.
- A **community patch index** (a git repo of modes and presets) is the obvious open-source-native move, and it is precisely what the Microcosm's four-year content freeze leaves undone.

### 13.9 Openness as a feature

Publish schematics, panel/enclosure files, the DSP core, and the mode format. The Microcosm is a black box that cannot be extended by anyone but Hologram, and Hologram has stopped. Being extensible *by anyone* is the one thing a $459 closed pedal can never offer.

---

## Recommendations for Brainscape

Concrete and opinionated:

1. **Build one engine, not eleven effects.** Implement a single granular voice engine + scheduler + modifier chain (per §4.5), and ship "modes" as data on top. Resist the temptation to write Mosaic, Haze and Blocks as separate DSP classes — they are the same code with different scheduling.
2. **Preserve the macro-first philosophy on the hardware panel.** Activity / Repeats / Shape / Time as macros, with meaning remapped per mode. The Microcosm's high floor is its whole value proposition; do not ship a pedal that requires patch-building to make a sound. Put the depth in the editor.
3. **Ship a curated preset set that is the product.** 40–60 hand-dialled destinations, level-matched, named, with the same "turn to it and it's already finished" quality. Treat preset curation as a first-class deliverable with as much effort as the DSP.
4. **Make the trigger layer a headline feature.** Multi-band onset detection with visible feedback and adjustable sensitivity, plus selectable trigger sources including MIDI note and footswitch. Market this explicitly — "works on fuzz and pads," because the Microcosm demonstrably doesn't.
5. **Add scale-quantised pitch.** Break the octave-only limitation. It is cheap, it is audibly new, and it directly extends the sound people already love (Mosaic).
6. **Keep the looper first-class, and make it better in three specific ways:** multiple loop slots, freeze coexisting with the phrase looper, and loop import/export over USB. Keep pre/post-FX routing and quantise — those are the good ideas worth copying verbatim.
7. **Fix the five UX defects the community has complained about for six years:** soft takeover, non-interrupting preset changes, per-preset gain normalisation, assignable footswitches (effect independent of looper), and expression range limits. These are cheap and they are the difference between "another granular pedal" and "the one that fixed everything."
8. **Design the preset/mode file format before the DSP.** It is the contract between firmware, VST, editor and the community repo. Make it human-readable and diffable so patch sharing works over git.
9. **USB-C from day one** for firmware, MIDI, preset sync, and ideally audio. MIDI-DIN as well, but never MIDI-only updates.
10. **Design the MIDI map deliberately.** Avoid CC 7/10; include relative navigation messages; document it as a published spec, not a table in a PDF.
11. **Post chain: keep the same components, make them movable.** Modulation, reverb, filter after the granular block is the right *default* — but make each defeatable and the order configurable. Hologram's own Chroma Console proves the demand.
12. **Stereo everywhere, with two real input jacks.** The single TRS input is the one hardware complaint that appears in every professional review.
13. **Take the "instant results" mandate seriously as an acceptance test.** A useful bar: a stranger plugs in, turns to any preset, plays one chord, and it sounds finished — without reading anything.

---

## Sources

**Primary (Hologram Electronics)**
- Microcosm user manual v1.13 (PDF, 25 pp): https://cdn.shopify.com/s/files/1/0920/2928/8752/files/MC_manual_WEB.pdf
- Manual landing page: https://www.hologramelectronics.com/pages/microcosm-manual
- Product page: https://www.hologramelectronics.com/pages/microcosm
- Firmware page (version history + LED identification test): https://www.hologramelectronics.com/pages/microcosm-firmware
- Support / FAQ page (48 kHz/24-bit, impedances, 450 mA, expression gotcha, level modes): https://www.hologramelectronics.com/pages/support
- Microcosm FAQ article (login-gated at time of research): https://hologramelectronics.freshdesk.com/support/solutions/articles/64000287589-microcosm-faqs
- Chroma Console product page: https://www.hologramelectronics.com/pages/chroma-console
- Microcosm Limited Edition: https://www.hologramelectronics.com/products/microcosm-limited-edition
- Microcosm Matte Black Colorway: https://www.hologramelectronics.com/products/microcosm-matte-black-colorway
- Firmware v1.1 announcement: https://www.facebook.com/hologramelectronics/posts/were-happy-to-announce-microcosm-firmware-v11-available-now-on-our-website-weve-/3669230886434126/ and https://www.instagram.com/hologram_electronics/p/CFNXQ4ohAse/

**Reviews**
- Sound On Sound: https://www.soundonsound.com/reviews/hologram-electronics-microcosm
- Engadget ("A cheat code for making ambient music"): https://www.engadget.com/hologram-electronics-microcosm-guitar-effect-pedal-review-ambient-music-170002707.html
- Sine Squares: https://www.sinesquares.net/musicgear/hologram-electronics-microcosm-review
- Waveform Magazine: https://waveformmagazine.com/waveform-reviews/microcosm-hologram-electronics/
- Guitar World review: https://www.guitarworld.com/reviews/hologram-electronics-microcosm-review (paywalled; mirror: https://www.yahoo.com/entertainment/hologram-electronics-microcosm-review-094647532.html)
- Guitar World announcement: https://www.guitarworld.com/news/get-good-and-glitchy-with-hologram-electronics-microcosm-granular-effect-pedal-and-looper
- Guitar Pedal X: https://www.guitarpedalx.com/news/hologram-electronics-reveals-the-microcosm---the-most-mature-and-feature-rich-granular-synthesis-multi-fx-unit-to-date
- Guitar.com: https://guitar.com/news/music-news/hologram-electronics-microcosm/
- Delicious Audio: https://delicious-audio.com/hologram-electronics-microcosm-hologram-microcosm/
- Pedal of the Day: https://www.pedal-of-the-day.com/2020/05/26/hologram-electronics-microcosm-granular-delay-looper/
- Sound On Sound, Chroma Console: https://www.soundonsound.com/reviews/hologram-electronics-chroma-console
- Sine Squares, Chroma Console: https://www.sinesquares.net/musicgear/hologram-electronics-chroma-console-review
- "Why would I buy this pedal?" (Chroma Console, contains Microcosm interface comparison): https://wolfewithane.com/why-hologram-chroma-console

**Community / forums**
- ilovefuzz Microcosm thread (triggering + volume complaints, firmware): http://ilovefuzz.com/viewtopic.php?f=149&t=63679&start=60
- Elektronauts Microcosm thread: https://www.elektronauts.com/t/microcosm-hologram-electronics/121669 and post #508: https://www.elektronauts.com/t/microcosm-hologram-electronics/121669/508
- Morningstar forum, preset scroll/toggle feature request: https://forum.morningstar.io/t/feature-request-hologram-microcosm-preset-scroll-toggle/8084
- Mod Wiggler, "how is the Hologram Microcosm?": https://www.modwiggler.com/forum/viewtopic.php?t=282983 (403 to automated fetch; content reached via search snippets only)
- Mod Wiggler, "Hologram microcosm": https://modwiggler.com/forum/viewtopic.php?t=227881
- The Gear Page Microcosm thread: https://www.thegearpage.net/board/index.php?threads/hologram-electronics-microcosm.2121670/ (403 to automated fetch; via search snippets)
- Gearspace, "Anyone using a Hologram Microcosm?": https://gearspace.com/board/so-many-guitars-so-little-time/1353956-anyone-using-hologram-microcosm.html (403; via search snippets)

**Other**
- gearnews, Microcosm alternatives: https://www.gearnews.com/hologram-microcosm-alternatives/
- Effects Database entry + demo-video index (useful list of notable demo artists): https://www.effectsdatabase.com/model/hologram/microcosm
- Vintage Technology Archive specs page (dimensions, weight, early MIDI sync reports): https://vintagetechnologyarchive.com/synth/hologram-electronics/microcosm/
- Equipboard: https://equipboard.com/items/hologram-electronics-microcosm-5235c3bf-504e-4acc-b937-9ba9f1b67dac
- Reverb news, black colorway: https://reverb.com/news/hologram-electronics-releases-a-special-edition-black-colorway-microcosm
- MatrixSynth, 10th anniversary edition: https://www.matrixsynth.com/2026/05/hologram-microcosm-10th-anniversary.html
- ManualsLib mirrors: https://www.manualslib.com/manual/1852211/Hologram-Microcosm.html , https://www.manualslib.com/manual/3719344/Hologram-Microcosm.html
- Daisy Seed datasheet (96 kHz/24-bit hardware, 64 MB SDRAM) — Electrosmith, consulted for §13.5 feasibility

---

## Open questions

1. **What DSP does the Microcosm actually run?** No teardown was found. 48 kHz/24-bit is confirmed but the processor is unknown. *(Unverified.)*
2. **What changed in v1.12 and v1.13?** Hologram publishes no changelog for either. The FAQ implies further triggering fixes. *(Unverified.)*
3. **Measured latency.** No published figure for dry-path or wet-path latency. The "couple of seconds" reports are about buffer fill, not converter latency. Would need bench measurement.
4. **Exact grain size / density ranges.** Never published; the macro design deliberately hides them. Would require careful listening tests or spectral analysis of demo recordings.
5. **Is Pattern A truly a plain delay?** The manual calls it "classic, linear delay." If so, its feedback/repeat structure would confirm the shared-engine hypothesis in §4.5. Needs hands-on verification.
6. **How is Shape implemented per effect?** The manual only gives it a specific meaning in Glide. The widespread "Shape does nothing" complaint suggests it is inert or near-inert in several modes. Needs hands-on verification.
7. **How does the looper's record tap interact with Pre-FX mode?** The diagram shows injection points; it does not show whether Pre-FX also changes what the looper *records* (dry input) or only where it plays back. The manual's wording ("recorded material will be sent through effect processing… without altering the recorded material") implies it records dry and plays into the effects, but this is inferred.
8. **Reddit r/guitarpedals sentiment was not directly accessible** during this research (reddit.com is blocked to the tooling). The criticisms in §12 come from ilovefuzz, Elektronauts, Morningstar, TGP/Gearspace snippets and professional reviews. A manual pass over r/guitarpedals would likely add texture but is unlikely to change the substance, since the same complaints recur across every accessible venue.
9. **Does the Microcosm's Space knob delay have its own time control?** The manual says Space "mixes in reverberation and delay" but only exposes Reverb Time as a secondary. Whether the delay component is tempo-linked is unclear.
10. **Sales/install-base numbers** are not public, so "beloved" is qualitative — inferred from six years of continuous production, an anniversary edition, and consistent long-term-owner enthusiasm.
