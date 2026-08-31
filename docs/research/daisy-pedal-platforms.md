# Daisy Seed Guitar-Pedal Hardware Platforms — Research Reference

> **Status: DRAFT — research in progress.** Written incrementally; the sections below are
> verified against primary sources (datasheets, schematic files, repo contents) except where
> explicitly marked *unverified*.
>
> **Purpose:** choose an open hardware reference design for **Brainscape**, a stereo-in /
> stereo-out granular delay pedal built on the Electrosmith Daisy Seed, with a shared
> platform-agnostic C++ DSP core also targeting a desktop VST.

---

## Summary

- **The Daisy Seed's own audio I/O is line-level and low-impedance — 13.6 kΩ typical input impedance and ±1.8 V absolute maximum (3.6 Vpp ≈ 1 Vrms = 0 dBFS).** Every serious pedal design therefore puts an op-amp buffer between the jacks and the Seed. ([datasheet](https://daisy.nyc3.cdn.digitaloceanspaces.com/products/seed/Daisy_Seed_datasheet.pdf))
- The Seed Rev7 uses a **TI/Burr-Brown PCM3060** codec wired in **hardware mode with no I²C control** — meaning **there is no programmable input gain / PGA available in firmware**. All level setting must be analog. ([Seed datasheet p.12](https://daisy.nyc3.cdn.digitaloceanspaces.com/products/seed/Daisy_Seed_datasheet.pdf))
- The Seed's on-board input network is `pin → 3.6 kΩ series → 4.7 µF → codec VIN`, and the output network is `codec VOUT → 4.7 µF → 47 kΩ pulldown → 100 Ω series → pin`. The 3.6 kΩ plus the PCM3060's ~10 kΩ input impedance is exactly the specified 13.6 kΩ. ([Rev7 schematic](https://daisy.nyc3.cdn.digitaloceanspaces.com/products/seed/ES_Daisy_Seed_Rev7.pdf))
- **PedalPCB Terrarium** is the most widely built Daisy pedal platform, but it is **mono only** — a single TL072 provides one input buffer and one output buffer. Schematic is published as a PDF; it is *not* an open-hardware release (no gerbers/CAD, "Copyright © 2022 PedalPCB.com"). ([Terrarium PDF](https://docs.pedalpcb.com/project/Terrarium.pdf))
- **Cleveland Music Co. Hothouse** is a genuine **open-source hardware** release under **CC BY-SA 4.0**, with Eagle schematics, board files, gerbers, BOM, CPL and a drill template on GitHub. Its I/O daughterboard is **stereo via TRS jacks** (tip = L, ring = R) on both input and output. ([repo](https://github.com/clevelandmusicco/open-source-pedals/tree/main/hothouse))
- **GuitarML Funbox** is **MIT-licensed**, **stereo in/out on four separate 1/4" jacks**, with **TRS MIDI in** and an **expression input**, KiCad sources published, in a 125B enclosure — and it already ships a granular-delay firmware ("Uranus"). ([repo](https://github.com/GuitarML/Funbox))
- **Hothouse and Funbox v3.2 have converged on nearly the same analog design**: an **MCP6024** rail-to-rail quad op-amp on a **+5 V single supply from a 78L05**, unity-gain buffers, ~2.5 V bias from a resistive divider, and a **3.3 kΩ + 2.2 nF (~22 kHz) RC reconstruction filter** on each Daisy output before the buffer.
- That 3.3 k/2.2 n output filter exists because the **original Terrarium schematic lacks it** and later Seed revisions are noticeably noisier without it — a widely repeated community fix. *(Partially verified; see Open questions.)*
- **None of the three community platforms uses a relay.** All use **soft-touch momentary SPST footswitches read by GPIO**, with bypass implemented in DSP. For a granular delay with trails this is workable, but it means no true bypass and no signal path when unpowered.
- The **Hologram Microcosm** — Brainscape's spiritual ancestor — takes the opposite approach: a **single mono/stereo TRS input**, two separate 1/4" outputs, and **two electromechanical relays (one per channel)** giving user-selectable *buffered*, *buffered + trails*, or *true bypass*. It draws **400 mA minimum** from 9 V center-negative. ([Microcosm manual](https://www.hologramelectronics.com/s/MC_manual_WEB.pdf))
- Hothouse gives the best **input impedance** of the community boards: 1 kΩ + (2 MΩ ∥ 2 MΩ) ≈ **1 MΩ**, which is guitar-friendly. Funbox v3.2's divider is 1 MΩ ∥ 1 MΩ ≈ **500 kΩ**, which will audibly load a passive single-coil pickup.
- **No community board has input clipping protection or an input level pad.** A hot boosted signal or a line-level source will clip the codec at 3.6 Vpp with nothing to stop it.
- All three community boards use a **series Schottky (1N5817 / SM5817)** for reverse-polarity protection and feed the Seed's VIN directly from the (protected) 9 V rail, deriving the op-amp +5 V from a **78L05 (100 mA)**.
- **Analog control reference matters:** Hothouse and Funbox both take pot reference from **Daisy pin 21 (+3V3_A)**, not the digital 3.3 V rail — the correct choice for low-noise ADC reads.
- *(more takeaways to be added as research completes)*

---

## 1. The Daisy Seed audio front end — what the hardware actually is

Everything below is from the official Electrosmith **Daisy Seed datasheet v1.2.0** and the
**ES_Daisy_Seed_Rev7** schematic PDF.

### 1.1 Codec history

| Seed revision | Active years | Codec | Reason for change |
|---|---|---|---|
| Rev 4 | 2020–2021 | AK4556 | original Kickstarter Seed |
| Rev 5 | 2021–2023 | WM8731 | AKM factory fire, AK4556 discontinued |
| Rev 7 | 2023– | **PCM3060** | WM8731 discontinued |

All revisions are pin-to-pin compatible and libDaisy auto-detects the revision at runtime
(the version pin — PD3 for Rev5, PD5 for Rev7 — is shorted to GND on-board).

**Critical for Brainscape:** the PCM3060 on Rev7 is configured **in hardware mode and is not
connected via I²C**. Quoting the datasheet: *"The PCM3060 is configured by the hardware, and is
not connected via I2C for serial control."* Configuration is fixed at **24-bit left-justified,
de-emphasis off, slave mode for both ADC and DAC**, single-ended VOUT. Digital VDD is 3.3 V
filtered from +3V3_D; **analog VCC is 4.5 V from an on-board LDO**.

Consequence: **you cannot set an input gain in firmware.** Unlike a WM8731-based design where the
ADC has a programmable input volume, on a Rev7 Seed the ADC full-scale point is a fixed analog
property of the board. Any instrument/line level matching must be done in the analog front end.

### 1.2 Audio electrical limits (Table 1 / Table 3)

| Parameter | Value |
|---|---|
| Audio input absolute max | **−1.8 V to +1.8 V** |
| Audio input full scale | **3.6 Vpp ≈ 1 Vrms = 0 dBFS** |
| Audio inputs AC coupled? | **Yes**, on-board |
| Audio input impedance (Rev7) | **13.6 kΩ typ.** |
| Audio output impedance | **100 Ω** |
| VIN range | **+5 V to +17 V** |
| GPIO | 0 to +5 V tolerant, **except** pins 24, 25, 28, 29, 30 which are 3.3 V-only |
| Sample rate / depth | up to 96 kHz / 24-bit |
| SDRAM | 64 MB (≈10 minutes of audio buffer) |

`AGND` (pin 20) **must** be connected to `DGND` (pin 40) — stated explicitly in the datasheet's
typical-applications section.

### 1.3 The Seed's on-board input/output networks (from the Rev7 schematic)

Reading the Rev7 schematic directly, per channel:

```
INPUT :  AUDIO_IN_L (pin 16) ──[ R24 = 3K6 ]──┬──[ C33 = 4.7 µF ]── CODEC_LIN (PCM3060 VINL)
                                              (PCM3060 single-ended input Z ≈ 10 kΩ)

OUTPUT:  CODEC_LOUT ──[ C64 = 4.7 µF ]──┬──[ R30 = 47 K to AGND ]
                                        └──[ R29 = 100 R ]── AUDIO_OUT_L (pin 18)
```

Right channel is identical (R26 = 3K6, C54 = 4.7 µF, C65 = 4.7 µF, R28 = 47 K, R31 = 100 R).

This confirms and explains the headline numbers:

- **13.6 kΩ input impedance = 3.6 kΩ series + ~10 kΩ codec input.** There is a built-in
  ~0.735× attenuator (10/13.6) between the Seed's input pin and the codec — which is why 3.6 Vpp at
  the pin corresponds to roughly 2.65 Vpp at the codec, right at the PCM3060's ~0.6 × VCC full
  scale for VCC = 4.5 V. *(The 0.6 × VCC figure is from the PCM3060 datasheet; the arithmetic
  agreement is my inference, not an Electrosmith statement.)*
- **The Seed already AC-couples both directions** (4.7 µF in and out). An external coupling cap is
  a second series capacitor — harmless, but you are stacking high-pass corners.
- **100 Ω output impedance** with a 47 kΩ pulldown means the Seed can drive a buffer input directly
  but should not drive a 1/4" jack and cable straight out.

### 1.4 Audio pin map (Seed, Rev4/5/7 identical)

| Pin | Function |
|---|---|
| 16 | `in[0]` — **Audio In L** |
| 17 | `in[1]` — **Audio In R** |
| 18 | `out[0]` — **Audio Out L** |
| 19 | `out[1]` — **Audio Out R** |
| 20 | AGND |
| 21 | **+3V3 Analog** (use this as pot reference) |
| 38 | +3V3 Digital |
| 39 | VIN (+5 V to +17 V) |
| 40 | DGND |

Internally the STM32 **SAI1** peripheral drives the codec (PE2 MCLK, PE3 SD B, PE4 FS A,
PE5 SCK A, PE6 SD A). SAI2 is broken out to the header (pins 31–35) and is what an *external*
codec would use — see §5.

### 1.5 What signal conditioning an instrument-level input actually needs

Putting numbers on it:

| Source | Typical level | Level at Seed pin needed |
|---|---|---|
| Passive single-coil, moderate playing | ~50–150 mVpp | very low — uses a tiny fraction of full scale |
| Passive humbucker, hard strum | ~0.5–1.5 Vpp | comfortable |
| Active pickups / boosted / another pedal's output | 2–6 Vpp | **clips** (3.6 Vpp limit) |
| Line level (+4 dBu balanced, or synth) | 3.5–10 Vpp | **clips hard** |

Because the Seed is a 24-bit converter, *undershooting* the ADC range costs far less than it
would on a 16-bit system — a signal 20 dB below full scale still has enormous dynamic range.
*Overshooting* is fatal (hard digital clipping). This is why every community board uses a
**unity-gain buffer with no gain**, and why none of them clip on normal guitar input. It is also
why none of them handles a genuine line-level source gracefully.

---

## 2. Platform survey

### 2.1 PedalPCB Terrarium

**Source of truth:** [`docs.pedalpcb.com/project/Terrarium.pdf`](https://docs.pedalpcb.com/project/Terrarium.pdf)
(rev. 1/7/22), plus the [product page](https://www.pedalpcb.com/product/pcb351/).

**Verified parts list (from the official PDF):**

| Ref | Value | Notes |
|---|---|---|
| R1, R2, R3, R9 | 1 M | |
| R4 | 100 R | |
| R5, R7, R8 | 1 K | |
| R6 | 100 K | |
| C1 | 100 p | ceramic |
| C2, C3, C6 | 1 µ | MLCC |
| C4, C5, C8, C9 | 100 n | film |
| C7 | 1 n | film |
| C100 | 100 µ | electrolytic |
| D100 | **1N5817** | Schottky, DO-41 — reverse-polarity protection |
| IC1 | **TL072** | dual op-amp, DIP8 |
| IC2 | **L78L05** | 5 V linear regulator |
| SEED1 | Daisy Seed | |
| POT1–POT6 | **B10K** ×6 | 16 mm right-angle PCB mount |
| SW1–SW4 | SPDT ×4 | 2-position ON/ON |
| FS1, FS2 | **momentary SPST** ×2 | |
| LED1, LED2 | ×2 | |
| — | 2 × 20-pin female headers | Seed is socketed |

**Assessment:**

- **Mono only.** One TL072 = two op-amp sections = one input buffer + one output buffer. There is
  physically no second channel. The PedalPCB forum states plainly that the Terrarium as-is cannot
  do stereo.
- **Enclosure:** 125B, with a drill template in the PDF (6 pots in two rows of 3, 4 toggles, 2 LEDs,
  2 footswitches, in/out/DC on the top edge).
- **Bypass:** momentary SPST footswitches → GPIO. **Software/DSP bypass only; no relay, no true
  bypass.**
- **Power:** 9 V → 1N5817 → Daisy VIN, and → L78L05 → 5 V for the TL072.
- **Licensing:** the PDF is marked "Copyright © 2022 - PedalPCB.com". **A published schematic PDF
  is not an open-hardware license** — there are no gerbers, no CAD source, and no stated permission
  to manufacture. You buy the bare PCB from PedalPCB. This is the single biggest strike against it
  as Brainscape's reference.
- **Known issue:** community consensus is that the Terrarium schematic needs an **RC low-pass filter
  added before the output buffer** to tame noise on current (Rev5/Rev7) Seeds. Both Hothouse and
  Funbox include exactly such a filter (3.3 kΩ + 2.2 nF). *(The specific claim that this is a
  Terrarium deficiency comes from forum discussion, not from PedalPCB — see Open questions.)*
- **Maintenance:** actively sold; docs last revised January 2022. Huge community firmware ecosystem
  (many DIY effects target "Terrarium" pinout as a de-facto standard).
- The TL072 is a **JFET-input, non-rail-to-rail** part running on a single +5 V rail here. Its
  output swing on 5 V is roughly 5 V − 3 V ≈ limited, and its input common-mode range does not
  include the rails. It works, but it is the weakest analog choice of the three platforms.

### 2.2 Cleveland Music Co. Hothouse

**Source of truth:** [`clevelandmusicco/open-source-pedals/hothouse`](https://github.com/clevelandmusicco/open-source-pedals/tree/main/hothouse)
and the firmware repo [`clevelandmusicco/HothouseExamples`](https://github.com/clevelandmusicco/HothouseExamples).

**License: Creative Commons Attribution-ShareAlike 4.0 International (CC BY-SA 4.0).** Explicitly
declared Open Source Hardware. Files provided: **Eagle `.sch` / `.brd` / `.lbr`, PDF schematics,
zipped gerbers, BOM (.csv and .xls), CPL placement files, and a PDF drill template for a DIY 125B
enclosure.** BOM reflects late-2025 production runs — this is actively maintained.

**Three-board architecture**, joined by 6-pin ribbon cables:

1. **Main PCB** — Daisy Seed, analog buffers, regulator, pots, toggles (mixed SMD/THT)
2. **Switching & LED daughterboard** — footswitches and LEDs (THT only)
3. **Power & Audio I/O daughterboard** — jacks and DC barrel (THT only)

**Verified netlist — I/O daughterboard (`hothouse-io-no-branding.sch`):**

```
INPUT  (stereo TRS jack):  Tip → L_IN   Ring → R_IN   Sleeve → GND
                           Tip-normal and Ring-normal both tied to GND
OUTPUT (stereo TRS jack):  Tip → L_OUT  Ring → R_OUT  Sleeve → GND
PWR    (5.5 × 2.1 mm barrel): center pin → GND, sleeve → +9V   [center-negative]
J2 (6-pin to main board): 1=L_IN 2=R_IN 3=+9V 4=GND 5=L_OUT 6=R_OUT
```

So **Hothouse is stereo — over a single TRS jack per direction**, exactly like the Microcosm's
input. Insert a mono TS plug and the ring shorts to sleeve, grounding the R channel.

**Verified netlist — main board (`hothouse-no-branding.sch`), per channel:**

```
INPUT PATH (L shown; R is C4/C5/R4/R5/R6/C6 and op-amp B)

  L_IN ──┬──[ C2 = 100 pF ]── GND            (RF shunt)
         └──[ C1 = 2.2 µF ]──[ R1 = 1 K ]──┬── IC1 pin VINA+  (MCP6024 section A)
                                           ├──[ R2 = 2 M ]── +5 V
                                           ├──[ R3 = 2 M ]── GND      (2.5 V bias)
                                           └──[ C3 = 1 nF ]── GND     (≈159 kHz LPF w/ R1)

  IC1 VOUTA ──┬── IC1 VINA-        (unity-gain follower)
              └── U1 AUDIO_IN_L    (Daisy pin 16)

OUTPUT PATH (L shown)

  U1 AUDIO_OUT_L ──[ R7 = 3K3 ]──┬──[ C7 = 2.2 nF ]── GND    (≈21.9 kHz reconstruction LPF)
                                 └──[ C8 = 2.2 µF ]──┬── IC1 VIND+  (section D)
                                                     ├──[ R8 = 1 M ]── +5 V
                                                     └──[ R9 = 1 M ]── GND   (2.5 V rebias)

  IC1 VOUTD ──┬── IC1 VIND-       (unity-gain follower)
              └──[ R10 = 100 R ]──[ C9 = 4.7 µF ]──┬── L_OUT
                                                   └──[ R11 = 100 K ]── GND  (anti-pop bleed)
```

**Input impedance = R1 + (R2 ∥ R3) = 1 kΩ + 1 MΩ = ~1 MΩ.** Guitar-friendly.
High-pass corner at the input: 2.2 µF into ~1 MΩ ≈ **0.07 Hz** (effectively DC).

**Power (verified):**

```
+9V ──[ D1 = SM5817PL-TP Schottky, series ]── VA ──┬── U1 V_IN (Daisy)
                                                   ├── C13/C14 100 n, C17 electrolytic
                                                   └── IC2 = UA78L05AIPK ── +5 V ──┬── IC1 VDD
                                                                                   └── C15/C16 100 n, C18 10 µF
```

Series Schottky protection (not a shunt diode), ~0.3–0.4 V drop. Note the op-amp rail is only
**+5 V at up to 100 mA** from a 78L05 — fine for one MCP6024, tight if you add much more analog.

**Controls (verified pin map):**

| Control | Daisy pin/GPIO |
|---|---|
| POT1–POT6 (16 mm, ref = **3V3_A**, Daisy pin 21) | GPIO16–GPIO21 (ADC1–ADC6) |
| SW_1 (SPDT, 3-position via 2 GPIOs) | GPIO10 (down) / GPIO9 (up) |
| SW_2 | GPIO8 / GPIO7 |
| SW_3 | GPIO6 / GPIO5 |
| LED_1 | GPIO22 (ADC7/DAC2), 1 kΩ CLR |
| LED_2 | GPIO23 (ADC8/DAC1), 1 kΩ CLR |
| FSW_1 | GPIO25 (ADC10) |
| FSW_2 | GPIO26 (SAI2_SD_A) |
| PIN_1 (spare, to switching board) | GPIO0 (USB_HS_ID) |

The 3-position toggles are wired as the datasheet's "On-Off-On Toggle" typical application: two
GPIOs per switch, both released in the centre position. That is a nice trick — **3 states per
switch for 2 pins**.

**Bypass:** `SW_1.P` and `SW_2.S` are `SPST.PBS.MOM` — **momentary soft-touch footswitches to
GPIO**. No relay anywhere in the BOM. **Software bypass only.**

**Not present:** MIDI, expression input, rotary encoder, display, SD card.

**Free GPIO after the stock design:** roughly GPIO1–4 (SD card pins), 11–15, 24, 27–30. Enough
room to add MIDI (UART), expression (an ADC), and a relay driver.

**Firmware:** `HothouseExamples` provides C++ examples plus a `Hothouse` hardware-abstraction class,
and supports Pure Data/plugdata and Max/MSP gen~ workflows. Prebuilt binaries are published.

### 2.3 GuitarML Funbox

**Source of truth:** [`GuitarML/Funbox`](https://github.com/GuitarML/Funbox) — **MIT license**.
KiCad sources for three hardware revisions live under `hardware/`:

| Revision | Status | Features |
|---|---|---|
| `funbox_v1` | "VERIFIED WORKING" | stereo I/O, 6 pots, 3× 3-way toggles, 2 momentary footswitches, LEDs, 2 DIP switches, 125B |
| `funbox_v2_midi` | "CURRENTLY UNTESTED" | adds **MIDI In/Out via two 3.5 mm TRS jacks**, 4 DIP switches |
| `funbox_v3_midi_exp` (v3.2) | "VERIFIED WORKING" | replaces MIDI Out with an **Expression input**; **TL072 → MCP6024** swap to reduce noise |

**Verified BOM (v3.2, `funbox_v3_BOM.csv`):** MCP6024-I/P (DIP-14 quad op-amp), MCP6002-I/P (DIP-8
dual), L78L05ACZ, 1N5817, H11L1M optocoupler (MIDI in), 1N4148, 6 × B10K 16 mm pots
(RV16AF-41-15R1-B10K), 3 × SPDT toggles, DS03-254-04BE 4-way DIP switch, 2 × PJ-320A 3.5 mm jacks
(MIDI IN + EXPRESSION), 2 × soft-touch momentary SPST footswitches, Daisy Seed Rev7.

**Verified netlist — audio buffers (`AudioBuffers.kicad_sch`), per channel:**

```
INPUT
  jack ──┬──[ 100 pF ]── GND
         └──[ 1 µF ]──┬── MCP6024 +IN
                      ├──[ 1 M ]── +5 V
                      └──[ 1 M ]── GND          (2.5 V bias; Zin ≈ 1M ∥ 1M = 500 kΩ)
  MCP6024 out ── (tied to its own −IN) ── AUDIO_IN_BUFFER_L/R ── Daisy pin 16/17

OUTPUT
  Daisy pin 18/19 ──[ 3K3 ]──┬──[ 2.2 nF ]── GND        (≈21.9 kHz LPF)
                             └──[ 1 µF ]──┬── MCP6024 +IN
                                          └──[ 1 K ]── (2.5 V bias divider, 1 nF bypassed)
  MCP6024 out ── (unity) ──[ 100 R ]──[ 1 µF ]──┬── output jack
                                                └──[ 100 K ]── GND
```

**Verified power (`Power.kicad_sch`):**

```
9V in ──[ D1 = 1N5817 series ]── VCC ──┬── Daisy pin 39 (VIN)
                                       ├── C6 100 µF + 100 nF
                                       └── U2 = L78L05 ── +5 V ── MCP6024 VDD (+100 nF ×2)
```

**Verified control map (root sheet):** POT_1…POT_6 → Daisy pins 23–28, referenced to
**3V3_A (pin 21)**; DIP1–DIP4 → pins 2, 6, …; SW1–SW3 3-position toggles → pins 3, 5, 11, 12, 13, 14;
FOOT_SWITCH_1/2 → pins 32 and another GPIO; LEDs via ~1 kΩ; MIDI IN jack (J4, TRS) → H11L1M
optocoupler; EXPRESSION jack (J5, TRS, 3.5 mm) with `EXPRESSION_REF` on ring.

**Assessment:**

- **Stereo, on four separate 1/4" jacks** (In L, In R, Out L, Out R) — the layout the DIY community
  generally prefers over TRS, and different from Hothouse/Microcosm.
- **Only community board with MIDI and expression.** Both use 3.5 mm TRS (requires a 1/4"→3.5 mm
  adapter for standard expression pedals — an explicit caveat in the docs).
- **Input impedance 500 kΩ** — half of Hothouse's, and low enough to slightly roll off the top end
  of a passive single-coil pickup.
- **Bypass:** momentary footswitches → GPIO. **No relay. Software bypass only.**
- **Directly relevant firmware:** "Uranus — granular delay and synth," and "Pluto — dual stereo
  looper." Funbox is the closest existing thing to Brainscape.
- **BOM/schematic discrepancy noted:** the generated `funbox_v3_BOM.csv` lists `R19 = 2M`,
  `R30 = 220R`, `R31 = 470R` while the root schematic shows LED current-limiting resistors as
  `R19/R20 = 1K`. The auto-generated BOM appears to be out of sync with the annotated schematic.
  *(Unverified which is authoritative — check before ordering.)*

### 2.4 Electrosmith Daisy Petal / Petal 125B SM

*(left unfinished in this pass — see Open questions, and
[pedal-control-surface-and-io-hardware.md](pedal-control-surface-and-io-hardware.md) which
completes the platform survey and makes the reference-design decision)*

The **Daisy Petal** is Electrosmith's own pedal-format dev board: stereo 1/4" in and out,
expression input, 1 rotary encoder, 6 knobs, 4 stomp switches, 3 toggle switches, ring of LEDs,
SD card slot. It has been moved to Electrosmith's **[Legacy page](https://daisy.audio/pages/legacy)**,
which strongly implies it is discontinued.

### 2.5 Other community designs

- **[`bkshepherd/DaisySeedProjects`](https://github.com/bkshepherd/DaisySeedProjects)** — a set of
  Daisy pedal hardware designs including `GuitarPedal125b` (6 pots, rotary encoder, OLED display,
  2 footswitches, 2 LEDs, MIDI, JLCPCB gerbers + CPL provided, 3D-printable enclosure option) and
  `GuitarPedal1590b-SMD`. The author states the schematics were "kit bashed" from Electro-Smith's
  Daisy Petal and DIY Electro-Music projects. Openly shared on GitHub.
- **[`PlayableElectronics/DaisySeedProjects`](https://github.com/PlayableElectronics/DaisySeedProjects)** — *(to verify)*

---

## 3. Reference point: what the Hologram Microcosm does

From the [official Microcosm manual](https://www.hologramelectronics.com/s/MC_manual_WEB.pdf):

- **Power:** 9 V center-negative, 2.1 mm barrel, **400 mA minimum**. The manual explicitly warns
  that switching supplies and daisy chains add noise.
- **Jacks:** *Mono/Stereo TRS Input* (one jack), *Output L*, *Output R*, *EXP IN*,
  *MIDI In*, *MIDI Out/Thru*, 9 VDC.
- **Levels:** input and output are both specified as **"Instrument Level or Line Level"** —
  i.e. the front end is designed to accept both.
- **Bypass — three user-selectable modes:**
  1. **Buffered bypass** — input stays buffered when disengaged, keeps the stereo image, effects
     stop immediately.
  2. **Buffered bypass + Trails** — effect and loops fade out naturally when bypassed.
  3. **True bypass** — *"Microcosm uses two electromechanical relays for the left and right
     channels independently. These switch between the buffered signal (engaged) and unbuffered
     signal (disengaged)."* Trails mode is unavailable in true bypass.
- Stereo input must be **enabled in global configuration**; it is mono-in by default.

This is the bar Brainscape is aiming at, and it is meaningfully above what any of the community
Daisy boards currently do — specifically on **relay bypass** and **line-level tolerance**.

---

## 4. Comparison table

*(superseded — the platform comparison and board decision live in
[pedal-control-surface-and-io-hardware.md](pedal-control-surface-and-io-hardware.md), §1 and §9)*

## 5. Recommendations for Brainscape

*(superseded — see [pedal-control-surface-and-io-hardware.md](pedal-control-surface-and-io-hardware.md),
§10 "Recommendations for Brainscape", which was written with this document's front-end analysis as input)*

## 6. Sources

- Daisy Seed datasheet v1.2.0 — https://daisy.nyc3.cdn.digitaloceanspaces.com/products/seed/Daisy_Seed_datasheet.pdf
- Daisy Seed Rev7 schematic — https://daisy.nyc3.cdn.digitaloceanspaces.com/products/seed/ES_Daisy_Seed_Rev7.pdf
- Daisy Seed docs page — https://docs.daisy.audio/hardware/Seed/
- PedalPCB Terrarium documentation PDF — https://docs.pedalpcb.com/project/Terrarium.pdf
- PedalPCB Terrarium product page — https://www.pedalpcb.com/product/pcb351/
- Cleveland Music Co. open-source-pedals (Hothouse) — https://github.com/clevelandmusicco/open-source-pedals/tree/main/hothouse
- HothouseExamples firmware — https://github.com/clevelandmusicco/HothouseExamples
- GuitarML Funbox — https://github.com/GuitarML/Funbox
- bkshepherd DaisySeedProjects GuitarPedal125b — https://github.com/bkshepherd/DaisySeedProjects/blob/main/Hardware/GuitarPedal125b/docs/README.md
- Hologram Microcosm manual — https://www.hologramelectronics.com/s/MC_manual_WEB.pdf
- Daisy legacy hardware page — https://daisy.audio/pages/legacy

## 7. Open questions

- Is the "Terrarium needs an added output RC filter" claim documented by PedalPCB anywhere, or is it
  purely forum folklore?
- What exactly is the Electrosmith **Petal 125B SM** (vs. the original Petal), and are its design
  files published?
- Funbox BOM vs. schematic resistor discrepancy (R19/R20/R30/R31).
- Current draw budget: what does a Daisy Seed running a granular delay with SDRAM actually pull?
