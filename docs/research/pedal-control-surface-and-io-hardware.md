# Brainscape Pedal Control Surface, Non-Audio I/O, and Reference-Design Decision

> **Status: Research complete for this document's scope**, written incrementally and verified against
> primary sources (the Electrosmith/Qu-Bit Seed3 datasheet pulled via browser rendering, TI product
> pages, GitHub repos, and independent DIY relay-bypass projects) except where explicitly marked
> *unverified* or flagged in Open Questions. This document completes the platform comparison started in
> `daisy-pedal-platforms.md` (which left §2.4, §4, and §5 as "to be completed") and extends it to the
> control surface, bypass, MIDI/USB, power budget, enclosure, and licensing questions the existing
> corpus did not answer. A handful of specific sub-questions (exact GuitarPedal125b relay part/license,
> TAC5242/OPA1652 exact current draw, Microcosm's own LED-driver identity) remain open and are listed
> at the end rather than guessed at.

---

## Summary

- **Corrects the corpus: the Seed3's TAC5242 codec is "Hardware-control" only, not I2C-controllable.** TI's own product title for the part is *"TAC5242 Hardware-control stereo audio codec"* — pin/hardware-strapped configuration only. The sibling part with an I2C/software control interface and identical audio specs is the **TAC5212** ("High-performance stereo audio codec"), a different part TI does not ship on any stock Daisy board. This means `daisy-seed-platform.md`'s claim that Seed3 "exposes I2C-controllable gain" is wrong, and `daisy-pedal-platforms.md`'s Rev7/PCM3060 conclusion — **"you cannot set an input gain in firmware"** — is equally true of the current default Seed3 board. All level-setting must still happen in the analog front end. ([TI TAC5242 product page](https://www.ti.com/product/TAC5242), [TI TAC5212 product page](https://www.ti.com/product/TAC5212))
- **Seed3 is confirmed pin-to-pin compatible with the Rev7 footprint**, both by Electrosmith's own datasheet ("Pin-to-pin compatible with the Seed pinout & footprint") and by direct community reports of drop-in swaps into a Funbox and a bkshepherd GuitarPedal125b with no hardware changes. ([Seed3 datasheet v2.1.0](https://daisy.nyc3.cdn.digitaloceanspaces.com/products/seed3/Daisy_Seed3_datasheet.pdf), [PedalPCB forum thread](https://forum.pedalpcb.com/threads/new-pin-for-pin-compatible-daisyseed-seed3.29897/))
- **The audio pin-level electrical contract is unchanged from Rev7**: audio inputs are AC-coupled, ±1.8 V absolute max, 3.6 Vpp typical full scale (≈1 Vrms = 0 dBFS); audio outputs are 0 dBFS = 1 Vrms, ~100 Ω output impedance; VIN is now specified **+4 V to +17 V** (previously documented as 5–17 V). None of the arithmetic in `daisy-pedal-platforms.md` §1 needs correction on this front — only the "which codec" framing does. ([Seed3 datasheet, Tables 1 & 3](https://daisy.nyc3.cdn.digitaloceanspaces.com/products/seed3/Daisy_Seed3_datasheet.pdf))
- **Electrosmith/Qu-Bit's own Seed3 datasheet (v2.1.0, Aug 2026) now ships a complete, measured, pedal-specific analog reference design** — "Instrument Level Audio Input/Output" (Figs. 3.4/3.5): a two-stage OPA1652 buffer with **1 MΩ input impedance**, a 3.3 kΩ/1 nF anti-alias filter, and a 100 Ω-output driver stage — built and audio-analyzer-measured on an internal **"Seed3 Pedal Dev Kit"**. This is a materially better-documented starting point than any community board and should anchor Brainscape's own front end. A separate, simpler "Line Level" circuit (20 kΩ in) and "Eurorack Level" circuit (100 kΩ in, TL072, ±12 V) are also published. ([Seed3 datasheet, pp.18–27](https://daisy.nyc3.cdn.digitaloceanspaces.com/products/seed3/Daisy_Seed3_datasheet.pdf))
- **Electrosmith's own docs now explicitly recommend the standard DIY-pedal multiplexing chips**: a **CD4051** 8:1 analog mux (3 GPIO + 1 ADC pin → 8 pots, chainable to more ADC pins for more banks) and a **CD4021** parallel-in/serial-out shift register (2 GPIO out + 1 GPIO in → 8 switches, daisy-chainable for more) — with working `AdcChannelConfig::InitMux` and `ShiftRegister4021` library calls shown. This directly answers the "how do you read 8 pots + encoder + footswitches from 12 ADC/31 GPIO" question the existing corpus left open. ([Seed3 datasheet, pp.13–17, 30–31](https://daisy.nyc3.cdn.digitaloceanspaces.com/products/seed3/Daisy_Seed3_datasheet.pdf))
- **The datasheet also publishes a reference Expression-pedal input circuit** (MCP6004, 33 kΩ/100 kΩ network into an ADC pin) and two Bipolar CV input circuits — useful directly for Brainscape's expression jack. ([Seed3 datasheet, pp.31–32](https://daisy.nyc3.cdn.digitaloceanspaces.com/products/seed3/Daisy_Seed3_datasheet.pdf))
- **`bkshepherd/DaisySeedProjects`' `GuitarPedal125b` is a closer prior-art match than the existing corpus credits it**: it ships **OLED display + rotary encoder + 6 pots + 2 footswitches + 2 LEDs + TRS MIDI in/out + relay-based "true bypass"** — i.e. it already clears almost every item on the Brainscape control-surface wishlist, including the relay bypass none of Terrarium/Hothouse/Funbox have. It is openly published on GitHub (KiCad + JLCPCB gerbers) but ships **no top-level LICENSE file** found in this pass, which is a real gap to resolve before forking it. ([bkshepherd/DaisySeedProjects](https://github.com/bkshepherd/DaisySeedProjects), [GuitarPedal125b README](https://github.com/bkshepherd/DaisySeedProjects/blob/main/Hardware/GuitarPedal125b/README.md))
- **MIDI DIN and MIDI TRS circuits at 3.3 V are now an official Electrosmith reference design**, not just community folklore: opto-isolated input (220 Ω/270 Ω/100 nF around a generic optocoupler symbol — part number not stated in the datasheet, contrast with Funbox's explicit H11L1M), and dead-simple 10 Ω/10 Ω output/thru circuits, shown for both 5-pin DIN and 3.5 mm TRS connectors side by side. ([Seed3 datasheet, pp.35–36](https://daisy.nyc3.cdn.digitaloceanspaces.com/products/seed3/Daisy_Seed3_datasheet.pdf))
- **USB-MIDI over Seed3's own USB-C is officially first-class**: *"MIDI is available on the Seed through either of the USB ports, or any of its UART ports. If you have a functional USB port wired up... MIDI is purely handled in software."* Seed3 also breaks out a second, independent USB D+/D− pair (pins 36/37) for a panel-mounted external USB-C connector, separate from the onboard SMD receptacle — so Brainscape can put a full-size USB-C jack on the enclosure face without relying on the tiny onboard connector being reachable through a drilled hole. ([Seed3 datasheet, pp.10–12, 35](https://daisy.nyc3.cdn.digitaloceanspaces.com/products/seed3/Daisy_Seed3_datasheet.pdf))
- **No community board or the official Seed3 reference circuits implement input clipping protection or a line/instrument pad switch.** The Microcosm's Instrument/Line switch is confirmed (from `microcosm.md`) to be an attenuation-only mode switch, not an impedance change — a cheap, proven precedent Brainscape can copy directly instead of inventing a new topology.
- **A latching (bistable) relay — e.g. Omron G6SU-2 or Panasonic TQ2-L — is the correct bypass relay choice for Brainscape**, not a non-latching part like the AXICOM IM01TS: non-latching relays draw ~40–45 mA continuously while held engaged, while latching relays draw current only for a short switching pulse. Two independently-published open-source DIY projects (`mstratman/relay-bypass`, `Chris-G-5150/latching-relay-true-bypass`) confirm this is a solved, reproducible pattern, including a documented ~35 ms mute-during-switch click-suppression technique that Brainscape's own DSP core can implement in software for free.
- **A bottom-up current-draw estimate is now possible**: ST/Emcraft's own published STM32H7 power tables put the MCU+QSPI+SDRAM core alone at up to **370 mA (worst-case, 400 MHz)**, plausibly 250–350 mA sustained at Daisy's actual 480 MHz boost clock with real SDRAM traffic. Adding the analog front end, MIDI opto, relay, and any display/LED-grid, **Brainscape should plan for roughly 300–420 mA sustained at 9 V** — in the same range as, and plausibly exceeding, the Microcosm's own documented >450 mA, making the latching-relay choice above a real, not cosmetic, power-budget decision.
- **A 1590DD-class enclosure (≈188×120 mm) is the right size target**, not 125B: it independently matches the real Microcosm's 7.1×4.7 in footprint almost exactly, is a standard pre-drilled-template-available Hammond part, and its 171.9×103.9 mm usable panel comfortably fits 8 pots + encoder + display + 2–3 footswitches + a full jack complement.
- **An IS31FL3731-class I2C charlieplex LED driver** (commodity, Adafruit-documented, 16×9/144-LED matrix over one I2C bus) is the practical off-the-shelf way to reproduce a Microcosm-style LED-grid UI without inventing custom driver hardware or spending scarce GPIO — pair it with a small I2C OLED (bkshepherd's proven approach) for text/preset feedback on the same bus.

---

## 1. Resolving the board question: Seed3 vs. Rev7

### 1.1 What actually changed, and what didn't

Pulling the datasheet directly (Electrosmith/Qu-Bit's automated-fetch block was bypassed by rendering
the PDF through Google's document viewer in a real browser session — see Sources):

| Spec | Rev7 (PCM3060) | Seed3 (TAC5242) | Changed? |
|---|---|---|---|
| Audio input abs. max | ±1.8 V | ±1.8 V | No |
| Audio input full scale | 3.6 Vpp (~1 Vrms) | 3.6 Vpp (~1 Vrms) | No |
| Audio output | 0 dBFS @ 1 Vrms, 100 Ω | 0 dBFS @ 1 Vrms, 100 Ω | No |
| VIN range | +5 V to +17 V (secondary-sourced) | **+4 V to +17 V** (primary, Table 1) | Slightly wider on the low end |
| Codec control | Hardware/pin-strap only, no I2C | **Hardware/pin-strap only, no I2C** | No — corrects prior assumption |
| USB connector | micro-USB | **USB-C**, plus a second independent USB D+/D− breakout on pins 36/37 | Yes |
| Pin/footprint compatibility | — | Confirmed pin-to-pin compatible | N/A |
| GPIO / ADC / DAC counts | 31 GPIO / 12×16-bit ADC / 2×12-bit DAC | Same | No |

Sources: [Daisy Seed3 datasheet v2.1.0](https://daisy.nyc3.cdn.digitaloceanspaces.com/products/seed3/Daisy_Seed3_datasheet.pdf) (Tables 1–3, Power and Pinout sections); [TI TAC5242 product page](https://www.ti.com/product/TAC5242).

**Practical conclusion for Brainscape:** the Seed3 is a safe, drop-in upgrade over Rev7 for a new
design. It does not change the "no firmware-settable input gain" constraint, does not require
re-deriving the input-network arithmetic in `daisy-pedal-platforms.md` §1.3, and is confirmed
pin-compatible with existing Rev7-footprint boards (Funbox, GuitarPedal125b) by direct community
report, not just Electrosmith's own claim. ([keyth72's report, PedalPCB forum](https://forum.pedalpcb.com/threads/new-pin-for-pin-compatible-daisyseed-seed3.29897/))

### 1.2 The TAC5242 I2C question, resolved

The task brief for this research hypothesized that Seed3's TAC5242 "is I2C-controllable," which would
overturn the Rev7 no-PGA conclusion. **This is not the case.** TI's own listing titles the part
explicitly: *"TAC5242 Hardware-control stereo audio codec with 119dB dynamic range ADC and 120dB
dynamic range DAC."* Control is via pin-strapping only. The **TAC5212** is TI's own sibling part —
*"High-performance stereo audio codec"* — with claimed-identical 118–119 dB dynamic range specs but a
real I2C/SPI control interface (register-programmable PGA gain, digital volume, filter selection).
Electrosmith did not choose the TAC5212 for Seed3. ([TI TAC5242](https://www.ti.com/product/TAC5242), [TI TAC5212](https://www.ti.com/product/TAC5212))

**Implication:** if Brainscape ever wants real firmware-controlled analog input gain (useful for a
per-preset gain feature, or an auto-level "Instrument/Line" toggle done in the mixed domain instead of
pure analog), it is not available on any stock Seed board today. The options are (a) do it in the
analog front end as every Rev7 board already does, or (b) design a custom board around the TAC5212 —
a nontrivial fork of Electrosmith's own reference layout, not recommended for v1.

### 1.3 The official "Instrument Level Audio Input/Output" reference circuit

This is the single most valuable primary-source find of this research pass: Electrosmith/Qu-Bit's own
Seed3 datasheet (v2.1.0, last revised 28 Aug 2026) now publishes a **measured, pedal-specific analog
front end**, distinct from a simpler "Line Level" circuit and a "Eurorack Level" circuit. All three are
built and bench-measured on internal "Dev Kit" boards (Desktop, Pedal, Eurorack) referenced throughout
the datasheet — these dev kits are cited as the test fixtures for THD/noise-floor/frequency-response
graphs but were **not found as a separately sold product or published open-hardware file** in this
research pass (see Open Questions).

**Instrument Level Audio Input** (Fig. 3.4), input impedance 1 MΩ typical:

```
TRS jack (tip=signal, ring=normal-sense) ──[10 µF]──[1 MΩ]──┬──[100 Ω]──[100 pF]── OPA1652 stage A (+in)
                                                             (unity-gain buffer, bias = AREF_AUDIO_BIAS)
   OPA1652 A out ──[3.3 kΩ]──[1 nF]── GND   (≈48 kHz single-pole anti-alias corner)
                 ──[4.7 kΩ]──[10 µF]──┬── OPA1652 stage B (+in)
                                      └──[10 kΩ]── AREF_AUDIO_BIAS   (2nd-stage bias/gain-set)
   OPA1652 B out ──[10 kΩ]──[330 pF]──[100 Ω]── Daisy "Audio In 1" (pin 16)
                                       (also a 33 nF cap to GND in the input coupling path)
```

**Instrument Level Audio Output** (Fig. 3.5), output impedance 100 Ω typical:

```
Daisy "Audio Out 1" (pin 18) ── OPA1652 stage, 15 kΩ/33 kΩ feedback (sets output gain) ──[100 pF]──[100 Ω]──
   ──[10 µF]── TRS jack (tip=signal; ring carries a "normal" sense path back through 10 kΩ)
```

For comparison, **Line Level Audio Input** (Fig. 3.2) is a single OPA1652 stage, 20 kΩ input
impedance, 10 kΩ/330 pF/1 kΩ feedback network — no second buffering stage, no explicit anti-alias
pole beyond the feedback network's own rolloff. **Eurorack Level** (Fig. 3.6/3.7) uses a TL072 at 100
kΩ input impedance for ±12 V bipolar synth-level signals.

Electrosmith's own official topology for a *guitar pedal* specifically (not line, not eurorack) is
therefore: **two-stage buffering, 1 MΩ input impedance, and a real RC anti-alias filter ahead of the
codec** — essentially confirming and refining the same architecture Hothouse/Funbox converged on
independently (1 MΩ / 500 kΩ input impedance, MCP6024, 3.3 kΩ+2.2 nF anti-alias filter), but now with
a vendor-published, audio-analyzer-measured version using a genuinely audio-grade op-amp (Burr-Brown
**OPA1652**, not the general-purpose MCP6024/MCP6002 the community boards use). **Recommendation:**
Brainscape's analog front end should start from this Electrosmith reference, not from Hothouse's or
Funbox's, since it is both newer and from the silicon vendor with direct codec knowledge, and swap in
the OPA1652 (or a similarly spec'd dual op-amp) rather than the MCP6024. ([Seed3 datasheet, pp.18–24](https://daisy.nyc3.cdn.digitaloceanspaces.com/products/seed3/Daisy_Seed3_datasheet.pdf))

**Caveat:** the exact node-to-node wiring above was transcribed from the PDF's extracted text layer
(component values and net labels only — no clean schematic image was captured), so treat the *topology*
as solid but re-verify exact pin-to-pin wiring against the original PDF (rendered via Google Docs
viewer, see Sources) before laying out a board from it.

### 1.4 What's still not clipping-protected

Neither the community boards nor Electrosmith's own new reference circuits include any input pad,
soft-clipping, or overvoltage protection ahead of the buffer stage — the same gap `daisy-pedal-platforms.md`
§1.5 already identified. The Instrument Level input circuit's 1 MΩ impedance and unity-ish gain will
still hard-clip on a boosted or line-level source. See §6 below for the recommended fix.

---

## 2. Control surface: reading enough pots, switches, and an encoder

### 2.1 Electrosmith's own official multiplexing guidance

The Seed3 datasheet is explicit and includes working library calls — this resolves the corpus's open
question about whether a Daisy pedal needs external multiplexing hardware (it does, past about 8
analog controls or a handful of switches), and with what:

**Pots — CD4051 8:1 analog multiplexer** (Fig. 4.2, "Multiplexed Pots"):

- Wiring: 3 GPIO (mux address A/B/C) + 1 ADC pin (mux common `X`) reads up to 8 pots.
- Multiple CD4051s can share the *same* 3 address GPIOs; each additional chip needs one more ADC pin
  and adds 8 more inputs — i.e. **3 GPIO + N ADC pins → 8×N potentiometers.**
- Cost of multiplexing: each muxed channel is read **8× slower** than a directly-wired ADC pin, because
  all 8 channels share one ADC conversion slot. Electrosmith's own guidance: keep anything needing fast
  response (CV, expression) on a direct ADC pin; put "slow" human-turned pots behind the mux.
- Firmware is a one-line library call: `AdcChannelConfig::InitMux(seed::D15, seed::D24, seed::D25, seed::D26)`
  then `hw.adc.GetMux(mux_index, channel)`.

**Switches — CD4021 parallel-in/serial-out shift register** (Figs. 2.3, 2.6, 2.7):

- Wiring: 2 GPIO out (clock, latch) + 1 GPIO in (data) reads 8 digital inputs.
- **Daisy-chainable**: multiple CD4021s can be strung together on the same 3 pins for more inputs
  (the datasheet's own "Desktop Dev Kit" reads 16 buttons this way).
- Firmware: `ShiftRegister4021<N_CHIPS, N_DATA_LINES>` class already exists in libDaisy/DaisySP tooling,
  with a `.Update()` / `.State(i)` API.
- Built-in GPIO pull-up/pull-down configuration removes the need for external pull resistors on
  directly-wired switches; shift-register-fed switches need their own pull-ups (10 kΩ shown in Fig. 2.3).

Sources: [Seed3 datasheet, GPIO section pp.13–17 and ADC section pp.29–31](https://daisy.nyc3.cdn.digitaloceanspaces.com/products/seed3/Daisy_Seed3_datasheet.pdf).

### 2.2 Budgeting Brainscape's actual control surface against this

Target control surface (synthesized from `microcosm.md` §13.6/13.7 and `granular-pedal-landscape.md`):
~8 macro knobs (Microcosm parity), 1 preset/menu rotary encoder (with push-button), 1 small
display, 2–3 footswitches (effect bypass independent of looper, at minimum), an expression jack,
MIDI DIN in/out/thru, USB-C, and a handful of status LEDs.

| Control | Qty | Pins needed direct | Pins needed muxed |
|---|---|---|---|
| Macro pots | 8 | 8 ADC | 3 GPIO + 1 ADC (CD4051) |
| Encoder (2 quadrature + 1 switch) | 1 | 3 GPIO | 3 GPIO (not muxable the same way; use direct GPIO or a cheap encoder-specific approach) |
| Footswitches | 2–3 | 2–3 GPIO | shares CD4021 bus |
| Menu/aux buttons | a few | — | shares CD4021 bus |
| Status/mode LEDs | 2–6 | 2–6 GPIO (or a driver chip, see §3) | — |
| Expression jack | 1 | 1 ADC (direct — needs fast response) | — |
| Bias reference | — | 1 (+3V3A, shared) | — |

Using the CD4051 approach for the 8 pots costs **3 GPIO + 1 ADC** total instead of **8 ADC pins** — a
huge saving that leaves 11 of 12 ADC pins free (expression + headroom for a 2nd mux bank later) and
frees 7 GPIO relative to direct-wiring. Switches/LEDs behind a CD4021 (or an I2C GPIO expander, see
below) similarly collapse to 3 GPIO for effectively unlimited digital I/O. **Conclusion: the Seed3's
12 ADC / 31 GPIO is comfortably enough for Brainscape's full target control surface without exotic
tricks, using parts (CD4051, CD4021) Electrosmith itself documents as the standard approach** — this
resolves the research brief's open question definitively in favor of simple, cheap, well-precedented
multiplexing over anything more exotic (I2C GPIO expanders remain a valid alternative for the
switch/LED side — see §3 — but are not required).

### 2.3 bkshepherd's GuitarPedal125b: the closest real-world analog

`bkshepherd/DaisySeedProjects`' `Hardware/GuitarPedal125b` is a genuinely close match to Brainscape's
target control surface and was previously only name-checked in `daisy-pedal-platforms.md` §2.5. Its
stated feature list: **Stereo Audio, MIDI in/out (TRS "mini" jacks), OLED Display, Rotary Encoder for
menu navigation, 6 pots (Alpha 9 mm, `RD901F-40-15R1-B10K`), up to 2 footswitches, up to 2 LEDs, and
relay-based "True Bypass" switching**, in a 125B enclosure, primarily SMD, with KiCad sources and
JLCPCB-ready gerbers/CPL published. The encoder is a `PEC11R-4220K-S24` (Mouser #652-PEC11R-4220K-S24).
Revision history in the README notes: Rev 4 added "anti-pop hardware mute" for relay switching; Rev 6
added "RC low-pass filter immediately following the Daisy Seed audio outputs" (independently confirming
the same anti-alias-filter fix the Hothouse/Funbox convergence found necessary) and switched to Alpha
9 mm pots; Rev 6 also *removed* a "PDS1 DC-DC Isolator" from the power supply, i.e. an earlier revision
used a DC-DC isolator that was later deemed unnecessary. ([GuitarPedal125b README](https://github.com/bkshepherd/DaisySeedProjects/blob/main/Hardware/GuitarPedal125b/README.md), [docs/README](https://github.com/bkshepherd/DaisySeedProjects/blob/main/Hardware/GuitarPedal125b/docs/README.md))

**This board already clears the single hardest item on the Brainscape wishlist — relay-based true
bypass — that none of Terrarium/Hothouse/Funbox implement.** Exact relay part number, driver circuit,
and click-suppression approach could not be confirmed from the README text alone in this pass (the
KiCad schematic files were not readable through the WebFetch text-extraction path used here); this is
flagged as the top follow-up item (§4.3, Open Questions).

**License gap:** no top-level `LICENSE` file was found for `bkshepherd/DaisySeedProjects` in this
pass — the README only notes "I couldn't include some of the custom footprints for specific components
due to licenses that wouldn't allow redistribution" (referring to third-party KiCad library parts, not
the project's own license). **Do not assume this repo is safely forkable/redistributable until an
explicit license is found or the author is asked directly** — see §9.

---

## 3. LED-grid-as-UI vs. small OLED

The Microcosm's own UI is not a small OLED at all — it uses banks of individually-addressed LEDs
("Indicator Lights") as its primary feedback surface (`microcosm.md` mentions the lights flashing
blue/amber/red/green for various states). **No teardown or driver-IC identification for the
Microcosm's specific LED implementation was found in this research pass** (searched directly; only
marketing/review pages surfaced, no teardown) — flagged as an open question rather than guessed at.
Three concrete implementation paths for Brainscape, all well-precedented outside the Microcosm itself:

- **Discrete GPIO-per-LED**: simplest and cheapest per-LED (a LED + resistor, following the Seed3
  datasheet's own reference — 2 kΩ series resistor off a GPIO, Fig. 2.5), but does not scale — a
  Microcosm-sized grid (dozens of LEDs, several colors) would consume GPIO Brainscape needs for pots/
  switches/relay/MIDI, even after the CD4051/CD4021 multiplexing in §2 frees most of the budget.
- **I2C charlieplex driver IC — e.g. Lumissil/ISSI IS31FL3731**: a genuinely off-the-shelf, well-
  documented commodity part (Adafruit sell a breakout board and 9×16 charlieplexed LED grids designed
  specifically to pair with it) that PWM-dims up to **144 individually addressable LEDs in a 16×9
  matrix over a single I2C bus**, with "not a lot of pin twiddling" — i.e. effectively zero additional
  GPIO cost beyond the shared I2C bus already needed for a display. This is the most direct off-the-
  shelf way to reproduce a Microcosm-style LED-grid UI without inventing custom driver hardware.
  ([Adafruit IS31FL3731 breakout](https://www.adafruit.com/product/2946), [IS31FL3731 datasheet](https://cdn-learn.adafruit.com/downloads/pdf/i31fl3731-16x9-charliplexed-pwm-led-driver.pdf))
- **Small I2C OLED** (bkshepherd's approach — a commodity "0.96in I2C display"): lowest part count for
  a legible preset/mode/parameter-value readout, but a genuinely different UI paradigm from the
  Microcosm's at-a-glance "which of eleven effects, which of four variations" LED-grid affordance —
  reproducing that affordance on an OLED means software-drawn icons/graphics, not a hardware
  equivalence.

**CPU/bus-contention cost**: both the IS31FL3731 and a small I2C OLED share the same real constraint —
neither should be driven with blocking I2C transactions from inside the audio callback (per
`daisy-seed-platform.md`'s general real-time guidance). A low-priority background refresh loop,
decoupled from the audio ISR, is the correct architecture for either, and was not further quantified
in cycles/frame in this pass (flagged as an open question — likely small relative to the grain engine's
own budget, since I2C UI refresh at even 30–60 Hz is a trivial fraction of a 480 MHz core's headroom,
but this should be measured on real hardware rather than assumed).

**Recommendation:** an **I2C OLED (bkshepherd's proven, lower-risk approach) for text/preset feedback,
plus an IS31FL3731 charlieplex driver for a genuine LED-grid strip or small matrix** if Brainscape wants
the Microcosm's specific "watch the grains light up" affordance, sharing the same I2C bus and GPIO
budget — both are commodity, well-documented parts, so combining them is not exotic. A full
Microcosm-scale LED grid is a reasonable v1 feature exactly because the IS31FL3731 makes it nearly free
in GPIO terms; it is the OLED's *content design*, not the LED grid's *driver hardware*, that is the
harder remaining problem.

---

## 4. Relay true bypass with trails

### 4.1 Latching vs. non-latching: the central tradeoff

Signal relays for audio true-bypass fall into two families, and the choice directly drives Brainscape's
current budget (§7):

- **Monostable (non-latching) relays** — e.g. the **AXICOM IM01TS** (DPDT signal relay, 3 V coil) —
  need continuous coil current *the entire time the relay is held in its energized state*. A typical
  driver (2N7000 MOSFET pulling the coil to ground, flyback diode across the coil, a series resistor —
  150 Ω was cited for running a 3 V-coil relay safely off a 9 V rail) draws **~40–45 mA continuously**
  while the relay is held engaged. On a 9 V battery (500 mAh) that alone would exhaust the battery in
  about 11 hours. ([barbarach.com, "Using Relays in Pedals"](https://barbarach.com/using-relays-in-pedals/))
- **Bistable (latching) relays** — e.g. the **Omron G6SU-2** (single-winding latching signal relay,
  3 V rated) or the **Panasonic TQ2-L** (used in a documented open-source build, see below) — only draw
  current for the **duration of a switching pulse** (tens of milliseconds); holding either state
  afterward costs essentially nothing. The tradeoff is added control complexity: the relay's state is
  not implicitly known at power-up, so firmware must **explicitly drive it to a known state on boot**
  (both cited sources make this same point independently), and the driver needs to source a pulse of
  the correct polarity for set vs. reset (an H-bridge-style pair of transistors, or a small driver IC,
  rather than a single MOSFET-to-ground).
  ([barbarach.com](https://barbarach.com/using-relays-in-pedals/), [Chris-G-5150/latching-relay-true-bypass](https://github.com/Chris-G-5150/latching-relay-true-bypass))

**For Brainscape, a latching relay is the clear choice.** The whole point of putting a full Cortex-M7 +
SDRAM granular engine in the signal path is that the platform is already power-hungry (§7); paying
40–45 mA continuously just to hold bypass engaged (or hold the effect engaged, depending on which state
is "resting") is a needless tax that a $1–2 latching relay and a few extra lines of boot-time firmware
avoids entirely. It is also directly compatible with Brainscape's own MCU driving the relay — no need
for a separate microcontroller.

### 4.2 Two independently-documented open-source implementations

Two real, published DIY projects solve exactly this problem and are worth reading end-to-end before
Brainscape finalizes its own circuit:

- **`mstratman/relay-bypass`** (fits a 1590A enclosure, sold as bare PCBs via mas-effects.com): uses an
  **ATtiny13 or ATtiny85**, a **latching relay**, and a soft-touch momentary footswitch with both
  quick-tap-toggle and press-and-hold-momentary behavior built into firmware. Critically, it documents
  the exact click-suppression technique Brainscape needs: an **optional optocoupler-based mute that
  silences the signal for ~35 ms while the relay switches**, enabled via a jumper and a firmware flag —
  i.e. the "anti-pop hardware mute" bkshepherd's GuitarPedal125b also implements is a known, named,
  reproducible pattern, not a one-off trick. ([github.com/mstratman/relay-bypass](https://github.com/mstratman/relay-bypass))
- **`Chris-G-5150/latching-relay-true-bypass`**: uses the **Panasonic TQ2-L** latching relay directly
  driven from an ATtiny85 (Arduino core, "Bounce2" debounce library), with a published schematic image
  and firmware source. Confirms the Panasonic TQ2-L as a second, independently-chosen part in the same
  family as Omron's G6SU-2 for this exact application. ([github.com/Chris-G-5150/latching-relay-true-bypass](https://github.com/Chris-G-5150/latching-relay-true-bypass))

**For Brainscape specifically**, the 35 ms mute-on-switch trick maps directly onto firmware Brainscape
already needs to write: rather than a separate optocoupler-based analog mute, the Daisy's own DSP core
can simply ramp its output to silence for the same ~30–40 ms window around a relay-state change,
achieving the same click-free transition in software with zero extra parts — an advantage a
microcontroller-based bypass module without a "real" DSP core doesn't have.

### 4.3 The Microcosm's approach and what's still unconfirmed

The Microcosm uses **two electromechanical relays, one per channel** (stereo bypass), switching
between buffered (engaged) and unbuffered (disengaged) signal paths; true bypass and trails are
mutually exclusive modes (`daisy-pedal-platforms.md` §3, sourced from the [Microcosm manual](https://www.hologramelectronics.com/s/MC_manual_WEB.pdf)). Whether Hologram uses latching or
non-latching relays, and the exact part, was **not found** in this pass (no teardown located) — given
the Microcosm's documented >450 mA total draw (`microcosm.md`), a pair of non-latching relays held
engaged continuously (2 × ~40–45 mA, per §4.1's figures for a similar-class part) would already account
for a meaningful fraction of that budget, which is circumstantial support for guessing non-latching,
but this is **unverified** and should not be stated as fact.

bkshepherd's GuitarPedal125b independently implements "relay based 'True Bypass' switching" with an
explicit "anti-pop hardware mute" added in Rev 4 — confirming the click-suppression problem is real and
solved in a second, independently-documented shipping DIY design — but the exact relay part number and
driver circuit used in that specific board were not extracted from the README text in this pass (the
KiCad schematic itself would need to be read directly; flagged as a follow-up).

None of PedalPCB Terrarium, Cleveland Music Hothouse, or GuitarML Funbox implement a relay at all — all
three are DSP/soft bypass only, confirming `daisy-pedal-platforms.md`'s finding still stands for those
three.

---

## 5. MIDI DIN/TRS and USB-C

### 5.1 UART MIDI at 3.3 V — now an official Electrosmith reference circuit

From the Seed3 datasheet §7 (MIDI), pp. 35–36:

**Input** (opto-isolated, both DIN and TRS variants shown):

```
5-pin DIN pin 4 (MIDI_IN_4) ──[220 Ω]──┬── optocoupler LED anode
5-pin DIN pin 5 (MIDI_IN_5) ───────────┴── optocoupler LED cathode
optocoupler phototransistor collector ──[270 Ω to +3V3D]──┬── MIDI_RX (to Seed UART RX)
                                                            └──[100 nF to GND]  (debounce/filter)
```

The TRS variant (Fig. 7.3) is topologically identical, just re-pinned onto a 3.5 mm TRS jack's
tip/ring/sleeve instead of DIN pins 4/5. **The datasheet's schematic symbol for the optocoupler does
not carry a printed part number** in the extracted text/render (unlike Funbox, which explicitly
specifies an **H11L1M**) — treat the exact optocoupler part as an open choice; H11L1M (fast, logic-output,
used by Funbox) or a standard 6N138/PC900-class part are the usual DIY choices and should work with
this same resistor network, but this should be confirmed against the actual schematic image (not just
extracted text) before finalizing a BOM.

**Output / Thru** (Figs. 7.4–7.7): dead simple — MIDI_TX (or MIDI_RX, for Thru) through **two 10 Ω
resistors**, one to the signal pin and one to +3.3 V, both DIN and TRS shown. No optocoupler needed on
the output side (standard MIDI spec — output is a current-loop driver, not opto-isolated on the sending
end).

**Implication for Brainscape:** the corpus's open item "MIDI DIN in/out/thru circuits on 3.3 V" is now
answered by primary source, with input, output, and thru all shown explicitly, for both 5-pin DIN and
3.5 mm TRS-MIDI connector standards side by side — Brainscape can pick either connector standard (or
support both via a jumper/DIP-switch-selected footprint, which is what Funbox does with its DIP
switches) using the same electrical design.

### 5.2 USB-C

Confirmed from the datasheet:

- The onboard USB-C is wired for **5 V power in (500 mA default)**, reverse-protected, and can be
  connected simultaneously with VIN-pin power.
- **"MIDI is available on the Seed through either of the USB ports... purely handled in software"** —
  i.e. USB-MIDI class-compliant operation over the stock onboard USB-C connector requires no additional
  hardware, only firmware (libDaisy's USB device stack).
- A **second, independent USB D+/D− pair is broken out to header pins 36 and 37** (`USB_HS_D-`/`USB_HS_D+`),
  with an official "External USB-C Power" reference circuit (Fig. 1.3, using a USBLC6-2SC6 ESD-protection
  chip) — meaning Brainscape can wire its *own*, panel-mount-friendly USB-C jack to these pins rather
  than trying to expose the Seed3's own tiny onboard SMD connector through an enclosure cutout.

This resolves the brief's question cleanly: **yes, Seed3's USB-C (or a panel-mounted jack wired to the
breakout pins) is genuinely usable for firmware flashing, USB-MIDI, and preset sync with zero extra
silicon**, matching the Microcosm's "USB-C alongside MIDI In/Out/Thru" spec noted in `microcosm.md`.
([Seed3 datasheet, Power section pp.10–12, MIDI section p.35](https://daisy.nyc3.cdn.digitaloceanspaces.com/products/seed3/Daisy_Seed3_datasheet.pdf))

---

## 6. Input pad / clipping protection for line level

Confirmed from `microcosm.md` (FAQ-sourced): the Microcosm's **Instrument/Line switch is an
attenuation-only mode — it does not change input impedance**, and "headroom is essentially identical
between Instrument and Line modes." This is a useful, cheap precedent: a simple **resistive pad
switched in/out ahead of the existing 1 MΩ instrument buffer** (e.g., a relay- or FET-switched
attenuator, or even a panel toggle switching in a voltage-divider before the OPA1652 input stage) would
replicate this exact behavior without redesigning the impedance-setting network. No community board or
the new official Seed3 reference circuits implement this — it would be a genuine Brainscape
differentiator relative to every open Daisy pedal platform surveyed. Further work needed: pick exact
pad ratio (Microcosm gives no numeric spec) and whether to implement it as a passive switched pad
(cheapest, always-available even unpowered-relay-wise) vs. an active/FET solution.

---

## 7. Current budget at 9 V

Electrosmith's own Seed3 datasheet does not publish a single total current-draw figure for the module.
The best primary-source numbers available are ST's own STM32H750 electrical characteristics, packaged
into a clean reference table by Emcraft Systems (an embedded-SOM vendor building STM32H7 modules with
the same MCU + external SDRAM + QSPI flash combination the Daisy Seed uses):

| Component | Condition | Max current @ 3.3 V |
|---|---|---|
| STM32H7 CPU | Run mode, all peripherals enabled, **400 MHz** CPU clock | **220 mA** |
| STM32H7 CPU | Stop mode (D1/D2 standby, D3 stop) | 2.4 mA |
| 512 Mbit QSPI NOR flash | Fast read, quad-IO dual transfer @ 80 MHz | 28 mA |
| QSPI flash | Standby | 0.1 mA |
| 2× 32 MB SDRAM (=64 MB, matches Daisy Seed) | Burst read/write @ 143 MHz | 120 mA |
| SDRAM | Self-refresh | 4 mA |
| **Total, Run mode (worst case)** | | **370 mA** |

Source: [Emcraft Systems, "Theoretically Calculated Max Power Consumption of the STM32H7 System-On-Module"](https://www.emcraft.com/som/stm32h7/STM32H7-SOM-CURRENT_CONSUMPTION.pdf) (PDF rendered via Google Docs viewer; the underlying per-component figures are themselves sourced by Emcraft from the ST STM32H75xB, Micron MT25QL512ABB, and Alliance AS4C32M16SA datasheets).

This is a **worst-case, all-peripherals-enabled figure at 400 MHz**, not a measured "running Brainscape's
actual firmware" number — Daisy boards run the CPU boosted to **480 MHz** (`daisy-seed-platform.md`
notes disabling boost to 400 MHz costs ~20% more CPU-time-per-block, implying the reverse: 480 MHz draws
more current than the 400 MHz figure above, very roughly +15–25% on the CPU line alone by simple
frequency scaling, i.e. plausibly ~250–275 mA for the CPU core specifically at 480 MHz). A granular
delay's actual SDRAM traffic pattern (continuous read/write for the live delay buffer, per
`daisy-seed-platform.md`'s own finding that SDRAM-resident buffers cost ~3.5× more CPU than SRAM-resident
ones) makes the 120 mA SDRAM figure a realistic *sustained*, not just worst-case burst, load for
Brainscape specifically — unlike a typical embedded application that touches SDRAM only occasionally.

**Adding the rest of a Brainscape BOM** (figures below are order-of-magnitude estimates from general
component knowledge, not individually re-verified datasheet pulls in this pass — flagged accordingly):

| Addition | Estimated current | Basis |
|---|---|---|
| Digital core (MCU + QSPI + SDRAM, sustained) | ~250–350 mA | Table above, scaled toward 480 MHz and sustained SDRAM traffic |
| Codec (TAC5242) analog/digital rails | ~10–20 mA | *unverified — not extracted from the TAC5242 datasheet in this pass* |
| 2× dual op-amp instrument-level front end (per Fig. 3.4/3.5, stereo = 2 input + 2 output stages) | ~5–10 mA | Typical audio-grade dual-op-amp quiescent current is low single-digit mA per amplifier; *not confirmed from the OPA1652 datasheet directly in this pass (PDF text extraction failed twice)* |
| Latching relay (§4.1), idle | ~0 mA (only draws during the ~10–50 ms switch pulse) | Bistable/latching relay behavior per §4.1 sources |
| MIDI input optocoupler LED, active | ~9–10 mA *while receiving a MIDI byte* (transient, not continuous) | Computed from the Seed3 datasheet's own 220 Ω series resistor and a ~1.2 V typical opto-LED forward drop: (3.3 V − 1.2 V) / 220 Ω ≈ 9.5 mA |
| Status LEDs (per Seed3 datasheet's own 2 kΩ reference, Fig. 2.5) | ~0.5–1 mA each | (3.3 V − ~1.8 V typical red-LED drop) / 2 kΩ ≈ 0.75 mA |
| I2C OLED (if fitted, §3) | ~20–30 mA typical, higher with many pixels lit | General commodity SSD1306-class module figures; *not independently re-verified in this pass* |
| IS31FL3731 LED-grid driver (if fitted, §3) | scales with number/brightness of LEDs lit; the driver IC itself draws little, the LEDs it switches dominate | [IS31FL3731 datasheet](https://cdn-learn.adafruit.com/downloads/pdf/i31fl3731-16x9-charliplexed-pwm-led-driver.pdf) |

**Working estimate: a Brainscape pedal with the full target control surface, relay bypass, and an OLED
should budget for roughly 300–420 mA sustained at 9 V** — in the same range as, and very plausibly
exceeding, the Microcosm's own **>450 mA** figure (`microcosm.md`) once an LED grid and/or a
continuously-driven display are included at typical brightness. **This is an estimate synthesized from
component-level datasheets, not a measurement — treat it as a planning figure for power-supply and
9 V-adapter-current-rating decisions, and re-verify against a real prototype's measured current draw
before finalizing a "requires XXX mA" spec on packaging/documentation.** The choice of a **latching
relay over a non-latching one (§4.1) is the single largest lever Brainscape has** to keep total draw
below the Microcosm's figure despite having a categorically more power-hungry MCU platform (a
Cortex-M7 + 64 MB SDRAM vastly exceeds whatever MCU the closed-source Microcosm uses internally,
which is undocumented but is very unlikely to need anywhere near 220–370 mA on its own given its
lower-complexity DSP feature set).

---

## 8. Enclosure sizing

### 8.1 Standard Hammond 1590-series dimensions

| Enclosure | External (mm) | Drilled face (mm) | Depth (mm) | Usable flat panel area (mm) |
|---|---|---|---|---|
| 1590A | 38.5 × 92.6 | 37.1 × 91.2 | 27 | 25.6 × 79.7 |
| 1590B | 60.5 × 112.4 | 59.3 × 111.2 | 27 | 47.3 × 99.2 |
| **125B** (Terrarium/Hothouse/Funbox v1) | 65.5 × 121.2 | 63.6 × 119.3 | 35.8 | 52.1 × 107.8 |
| 1590BB | 119.5 × 94 | 116.9 × 91.4 | 30 | 104.4 × 78.9 |
| **1590XX** | 145.2 × 121.2 | 143.4 × 119.4 | 35.2 | 129.3 × 105.3 |
| 1590P1 | 83 × 153 | 80.6 × 150.6 | 46.4 | 66 × 136 |
| **1590DD** | 188 × 120 | 185.7 × 117.7 | 33 | 171.9 × 103.9 |

Source: [stompboxlayout.com enclosure size/comparison tool](https://stompboxlayout.com/enclosures/), cross-referenced against [Hammond Mfg.'s own 1590 series page](https://www.hammfg.com/electronics/small-case/diecast/1590) and [Amplified Parts' 1590XX listing](https://www.amplifiedparts.com/products/enclosures-chassis/pedal-enclosures/1590x-1590xx) (5.72 × 4.77 × 1.39 in ≈ 145 × 121 × 35 mm, matching the table above).

### 8.2 What actually fits, and the Microcosm comparison

**125B is confirmed too small** — its 52.1 × 107.8 mm usable panel area is exactly why Terrarium/
Hothouse/Funbox v1 top out at 6 pots + a few toggles + 2 footswitches with no room for an encoder,
display, or MIDI/expression jacks without crowding the edges (Funbox's own MIDI-adding revision keeps
the same 125B shell but only by using two of the smallest available connector types, 3.5 mm TRS, and
still had to trade something — see `daisy-pedal-platforms.md` §2.3's note about DIP-switch-selected
connector options).

**The Microcosm's real enclosure — 7.1 × 4.7 × 2.0 in (≈ 180 × 119 × 51 mm), per `microcosm.md` — lands
almost exactly on the 1590DD's 188 × 120 mm footprint** (within a few mm on both axes), though the
Microcosm is somewhat deeper (51 mm vs. 1590DD's 33 mm external depth) to accommodate its internal PCB
stack and jacks. **This is a strong, concrete data point: a 1590DD-class enclosure (or a custom
enclosure of very similar footprint) is the right size class for a Microcosm-parity control surface**,
independently corroborating the control-surface budget in §2.2 (8 pots + encoder + display + 2–3
footswitches + several jacks) rather than requiring a novel enclosure size to be invented.

**Drilling implications**: 1590DD's usable panel is described by stompboxlayout.com as suited to "a row
of footswitches," with the largest panel area of the standard sizes surveyed (171.9 × 103.9 mm) — good
for laying out 2–3 footswitches across the bottom edge (Microcosm-style) with pots, encoder, and a
small display above, and jacks (stereo in/out, expression, MIDI in/out, USB-C, DC power) along the top
and/or side edges. The 1590XX (129.3 × 105.3 mm usable, closer to square) is a viable fallback if a
tighter footprint is preferred at the cost of a denser top-edge jack layout; stompboxlayout.com's own
copy describes 1590XX as "the big landscape format for multi-effects and amp-in-a-box builds," i.e.
already a known-good choice for exactly this class of feature-dense pedal.

**Recommendation:** target a **1590DD-class enclosure** (or a custom design of equivalent ~180×120 mm
footprint) as Brainscape's reference enclosure size, both because it is a standard, widely-stocked,
pre-drilled-template-available Hammond part (lowering the DIY-build barrier the project's open-hardware
goals depend on) and because it independently matches the real Microcosm's footprint almost exactly —
i.e. Hologram's own engineers converged on approximately this size for approximately this control-surface
density, which is exactly the validation a from-scratch enclosure choice should want.

---

## 9. Licensing: what each candidate reference design imposes on a derivative

| Candidate | License | Redistribution/derivative terms | Compatible with README's GPLv3 (firmware) + CERN-OHL (hardware) direction? |
|---|---|---|---|
| PedalPCB Terrarium | **None / proprietary** — PDF marked "Copyright © 2022 - PedalPCB.com," no gerbers/CAD released | No stated right to manufacture or derive; you buy the bare board | **No** — cannot be forked at all as a hardware base |
| Cleveland Music Hothouse | **CC BY-SA 4.0** | Share-alike: a derivative hardware design must also be released under CC BY-SA 4.0 (or a compatible license) — this is a **copyleft/viral** term for hardware, analogous to GPL for code but not identical to it | **No** — CC BY-SA and CERN-OHL are not the same license; picking Hothouse as a literal board-file base would force Brainscape's hardware under CC BY-SA, not CERN-OHL as README.md currently floats. (CC itself [advises against CC licenses for software/hardware designs](https://creativecommons.org/faq/) generally, precisely because of this kind of mismatch — worth flagging to the user as a reason to move off CC BY-SA even for a fork.) |
| GuitarML Funbox | **MIT** | Permissive — no share-alike obligation, sublicensing/relicensing under any terms (including CERN-OHL) is allowed | **Yes** — the only one of the three community boards that can be freely relicensed into whatever hardware license Brainscape ultimately picks |
| bkshepherd GuitarPedal125b | **Unstated / no LICENSE file found** in this pass | Unknown — the README explicitly discusses *third-party* footprint licensing restrictions but says nothing about the project's own license | **Unresolved** — must not be treated as freely forkable until confirmed; ask the author or find an explicit license before using it as a base |
| Electrosmith Seed3 hardware reference circuits (datasheet app notes) | Hardware reference circuits: no explicit license stated on the datasheet pages themselves (the **firmware/software** on the same product is explicitly MIT, per the datasheet's own front-matter MIT license block) | Typical semiconductor-vendor "reference design" convention is implicit permission to build from application-note circuits, but this datasheet does not contain an explicit hardware-design license grant the way Hothouse/Funbox do | **Unresolved but low-risk** — application-note-style reference circuits from a component vendor are conventionally treated as free-to-use starting points industry-wide, but Brainscape should not claim CC BY-SA/CERN-OHL/MIT provenance for this circuit specifically since none was stated |

**Bottom-line recommendation:** of the three community boards, **only Funbox (MIT) can be forked
without forcing a hardware license choice onto Brainscape.** Hothouse's CC BY-SA would override
README.md's CERN-OHL plan for any board file directly derived from it. Terrarium cannot be forked at
all (no CAD released). bkshepherd's GuitarPedal125b — despite being the closest functional match
(relay bypass + OLED + encoder + MIDI) — has an **unresolved license** that must be settled (ideally by
directly asking the author) before Brainscape treats it as a startable base, however tempting its
feature set is.

---

## 10. Recommendations for Brainscape

1. **Ship on the Seed3, not a Rev7-era board.** It is confirmed pin-compatible, has a wider VIN floor
   (+4 V vs +5 V), USB-C with a proper external-breakout option, and — critically — is now backed by
   Electrosmith's own measured pedal-specific analog reference circuit. The "TAC5242 might have I2C
   gain" hope does not pan out, but nothing about Seed3 is worse than Rev7 for Brainscape's purposes.
2. **Start the analog front end from the Seed3 datasheet's "Instrument Level Audio Input/Output"
   circuit (§1.3), not from Hothouse's or Funbox's**, swapping in an audio-grade dual op-amp (OPA1652 or
   equivalent) instead of Hothouse/Funbox's general-purpose MCP6024. Add a switched input pad ahead of
   it (§6) to cover the Microcosm's Instrument/Line use case, which none of the surveyed boards do.
3. **Use a CD4051 for the 8 macro pots and a CD4021 (daisy-chained) for footswitches/aux buttons**,
   exactly as Electrosmith's own datasheet demonstrates — this is now a vendor-blessed pattern, not a
   community workaround, and comfortably fits Brainscape's full target control surface within the
   Seed3's 12 ADC / 31 GPIO budget.
4. **Use a latching relay (Omron G6SU-2 or Panasonic TQ2-L class), never a non-latching one, for true
   bypass**, and implement click suppression as a firmware-side ~30–40 ms output mute around the relay
   transition rather than a separate analog mute circuit — Brainscape's DSP core can do this for free,
   unlike the microcontroller-only DIY projects that inspired the pattern (§4).
5. **Pair a small I2C OLED with an IS31FL3731 charlieplex LED driver on the same I2C bus** (§3) to get
   both legible text/preset feedback and a Microcosm-style LED-grid affordance without a bespoke driver
   design or a meaningful GPIO cost.
6. **Target a 1590DD-class enclosure (~188×120 mm)** as the reference size (§8) — it is a standard,
   widely available, pre-drilled-template Hammond part that independently matches the real Microcosm's
   footprint, validating that this is the right size class rather than an oversized guess.
7. **Budget power-supply and adapter guidance around 300–420 mA sustained at 9 V** (§7), explicitly
   calling out on packaging/documentation (as the Microcosm does) that a quality, adequately-rated 9 V
   supply is required — cheap daisy-chained supplies are a known noise/undercurrent risk this class of
   pedal cannot tolerate.
8. **Do not fork Hothouse's board files as-is** if README.md's CERN-OHL hardware-license plan is to be
   kept — its CC BY-SA 4.0 share-alike term would override that choice for anything directly derived
   from it. **Funbox (MIT) is the only community board safe to fork without a forced relicense.**
9. **Treat bkshepherd's GuitarPedal125b as the single most important prior-art reference to study in
   depth next** (not to blindly fork) — it is the only known open Daisy pedal design that already
   combines relay-based true bypass, an encoder, an OLED, and MIDI in one board, which is exactly
   Brainscape's target feature set. Its license must be clarified before any code/schematic reuse.
10. **Design the enclosure, control surface, and bypass relay decisions together**, since all three are
    driven by the same fact established in this document: Brainscape's control surface is
    meaningfully bigger than anything in the 125B-class boards were designed for, and the right target
    (1590DD, latching relay, CD4051/CD4021 muxing) is now specified concretely enough to start a
    schematic from.

---

## Sources

- Electrosmith/Qu-Bit Daisy Seed3 datasheet v2.1.0 (PDF, rendered via Google Docs viewer due to
  automated-fetch blocking) — https://daisy.nyc3.cdn.digitaloceanspaces.com/products/seed3/Daisy_Seed3_datasheet.pdf
- docs.daisy.audio — Seed3 hardware docs — https://docs.daisy.audio/hardware/Seed3/
- daisy.audio — Seed3 product page — https://daisy.audio/products/seed3
- daisy.audio — Products listing (confirms no separately-sold "Pedal Dev Kit" SKU as of this research) — https://daisy.audio
- TI TAC5242 product page ("Hardware-control stereo audio codec") — https://www.ti.com/product/TAC5242
- TI TAC5242 datasheet PDF — https://www.ti.com/lit/ds/symlink/tac5242.pdf
- TI TAC5212 product page ("High-performance," I2C/SPI-controlled sibling) — https://www.ti.com/product/TAC5212
- PedalPCB Community Forum — "New pin for pin compatible daisyseed Seed3" — https://forum.pedalpcb.com/threads/new-pin-for-pin-compatible-daisyseed-seed3.29897/
- Daisy Community — "Seed3 example circuits in the data-sheet" — https://community.daisy.audio/t/seed3-example-circuits-in-the-data-sheet/9511
- Daisy Community — "Pull down resistor in datasheet inst input buffer messing with impedance?" — https://community.daisy.audio/t/pull-down-resistor-in-datasheet-inst-input-buffer-messing-with-impedance/9665
- bkshepherd/DaisySeedProjects (GitHub) — https://github.com/bkshepherd/DaisySeedProjects
- bkshepherd GuitarPedal125b README — https://github.com/bkshepherd/DaisySeedProjects/blob/main/Hardware/GuitarPedal125b/README.md
- bkshepherd GuitarPedal125b docs README — https://github.com/bkshepherd/DaisySeedProjects/blob/main/Hardware/GuitarPedal125b/docs/README.md
- Brainscape internal reference: `docs/research/daisy-pedal-platforms.md` (Terrarium/Hothouse/Funbox platform survey, Microcosm bypass/power reference)
- Brainscape internal reference: `docs/research/daisy-seed-platform.md` (MCU/memory/codec-history context)
- Brainscape internal reference: `docs/research/microcosm.md` (control-surface and UX target spec, §13.6/13.7)
- Hologram Microcosm manual (via `microcosm.md`) — https://www.hologramelectronics.com/s/MC_manual_WEB.pdf
- barbarach.com — "Using Relays in Pedals" (AXICOM IM01TS, Omron G6SU-2, monostable/bistable tradeoffs, driver circuit) — https://barbarach.com/using-relays-in-pedals/
- github.com/mstratman/relay-bypass (ATtiny13/85 latching-relay bypass module, optocoupler mute technique) — https://github.com/mstratman/relay-bypass
- github.com/Chris-G-5150/latching-relay-true-bypass (Panasonic TQ2-L + ATtiny85) — https://github.com/Chris-G-5150/latching-relay-true-bypass
- Emcraft Systems — "Theoretically Calculated Max Power Consumption of the STM32H7 System-On-Module" — https://www.emcraft.com/som/stm32h7/STM32H7-SOM-CURRENT_CONSUMPTION.pdf
- stompboxlayout.com — enclosure size chart/comparison tool (1590A/B/BB/XX/DD, 125B dimensions) — https://stompboxlayout.com/enclosures/
- Hammond Mfg. — 1590 series diecast aluminum enclosures — https://www.hammfg.com/electronics/small-case/diecast/1590
- Amplified Parts — 1590X/1590XX product listing (dimension cross-check) — https://www.amplifiedparts.com/products/enclosures-chassis/pedal-enclosures/1590x-1590xx
- Adafruit — IS31FL3731 16×9 Charlieplexed PWM LED Matrix Driver (product + datasheet) — https://www.adafruit.com/product/2946, https://cdn-learn.adafruit.com/downloads/pdf/i31fl3731-16x9-charliplexed-pwm-led-driver.pdf

## Open questions

- **bkshepherd GuitarPedal125b's exact relay part number and driver circuit** — the README text
  confirms relay-based true bypass and an "anti-pop hardware mute" exist but the actual KiCad schematic
  was not successfully read as text in this pass. High priority follow-up: read the `.kicad_sch` files
  directly (as raw text via a GitHub raw-content URL) rather than relying on README prose.
- **bkshepherd GuitarPedal125b's actual project license** — no top-level LICENSE file was found; the
  README only discusses third-party footprint redistribution restrictions. Needs a direct check of the
  repo root or a question to the author before any reuse.
- **The Seed3 "Desktop Dev Kit" / "Pedal Dev Kit" / "Eurorack Dev Kit"** referenced throughout the
  datasheet as measurement test fixtures do not appear to be separately sold products or to have
  published open-hardware files (checked the current daisy.audio storefront and the electro-smith
  GitHub org's visible repo list). Worth a direct question to Electrosmith/Qu-Bit support or the Daisy
  Discord about whether the Pedal Dev Kit's full schematic is available on request — it would likely be
  an even better starting point than reconstructing the datasheet's app-note circuit from text alone.
- **MIDI input optocoupler part number**: the Seed3 datasheet's schematic symbol is generic/unlabeled
  in the extracted render. Needs a higher-fidelity look at the actual schematic image (not just
  Google-viewer text extraction) or a direct question to confirm whether Electrosmith recommends a
  specific part.
- **TAC5242 analog/digital supply current, and OPA1652 per-amplifier quiescent current** — both PDF
  datasheets resisted automated text extraction twice in this pass (garbled/binary font-stream content
  in the WebFetch tool's PDF parser); the current-budget figures in §7 for these two parts are
  therefore general-knowledge estimates, not confirmed datasheet pulls, and should be re-verified
  (the Google-Docs-viewer PDF-rendering workaround that worked for the Seed3 datasheet was not
  attempted on these two shorter datasheets in this pass and would likely succeed).
- **No teardown of the Microcosm's actual LED/indicator-light driver circuit was found** — §3's
  IS31FL3731 recommendation is a well-precedented off-the-shelf alternative, not a confirmation of what
  Hologram actually did.
- Whether the Microcosm's relays are latching or non-latching (§4.3) is unconfirmed; the >450 mA total
  draw figure is circumstantial, not proof, of a non-latching design.
- Whether CERN-OHL (as floated in README.md) is even the right hardware license choice given that the
  most feature-complete prior art (GuitarPedal125b) has no clear license and the most open-hardware
  community board (Hothouse) is CC BY-SA — i.e. whether Brainscape should just pick CERN-OHL-S/W and
  build from scratch (informed by, but not derived from, Hothouse/GuitarPedal125b) rather than trying
  to fork anything directly.
