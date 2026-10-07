# Daisy module supply, the DaisyKiCad package and JLCPCB (October 2026)

> Research note, 2026-10-07. Question from the owner: the Daisy Seed3 is out of stock; can
> Electrosmith's DaisyKiCad package be used to have the module made at JLCPCB? **Decision taken
> the same day:** prototype on the owner's Daisy Seed Rev7; keep a custom STM32H750 core board as
> a later option. Stock levels and prices below were read on 2026-10-06/07 and go stale fast.
> Tariff and compliance points are readings of public sources, not legal advice.

## Findings

**The package cannot produce a Daisy module.** `DaisyKiCad-main.zip` (MIT, © 2025 Electrosmith;
linked as the "KiCad Package" on [docs.daisy.audio/hardware/Seed3](https://docs.daisy.audio/hardware/Seed3/))
holds one KiCad 9 symbol library (Daisy_Seed, Daisy_Seed3, Daisy_Seed2_DFM, Daisy_Patch_SM) and six
footprints: `DAISY_SEED` and `DAISY_SEED_SMT` (40 pads), `DAISY_SEED2_DFM` (SMD) and
`DAISY_SEED2_DFM_PTH` (50 pads), `DAISY_PATCH_SM` and `DAISY_PATCH_SM_SMT` (40 pads). They are
land patterns for a carrier board. There is no schematic, layout or BOM of any module, and
Electrosmith/Daisy publishes only "REDUCED" schematics for older modules and none for the Seed3,
with no hardware licence.

**What it does enable is the carrier.** `DAISY_SEED` (2 × 20 at 2.54 mm, rows 15.24 mm apart,
1.7 mm pads, 1.1 mm drill) matches the [Seed3 datasheet v2.1.0](https://daisy.nyc3.cdn.digitaloceanspaces.com/products/seed3/Daisy_Seed3_datasheet.pdf)
landing pattern; the library's Daisy_Seed3 symbol is a copy of Daisy_Seed on that footprint, and
the Seed3 is pin-to-pin compatible with the Rev7. KiCad's stock
[`Module:Electrosmith_Daisy_Seed`](https://gitlab.com/kicad/libraries/kicad-footprints/-/raw/master/Module.pretty/Electrosmith_Daisy_Seed.kicad_mod)
has the same geometry, so for the Seed the package adds nothing new; its real additions are the
Seed2 DFM and Patch SM land patterns. JLCPCB can fabricate and assemble a carrier with sockets,
and the module is bought from Daisy and plugged in by hand (JLC cannot source the module;
consigning one to JLC is possible but uneconomic with US import duty on the return).

**A ready starting point for the carrier:** Daisy's
[Seed3 Pedal Dev Kit](https://docs.daisy.audio/product/Seed3-Pedal-Dev-Kit/)
([KiCad sources](https://github.com/electro-smith/ES-Seed3-DevKit-Pedal), CERN-OHL-P-2.0, Rev4,
KiCad 10) is a 2-layer, top-side-only board that already ships a JLC placement file and a BOM
with LCSC numbers. Caveats: it solders the Seed in place (no socket line in its
[BOM](https://daisy.nyc3.cdn.digitaloceanspaces.com/products/seed-3-pedal/Seed3-DevKit-Pedal_Rev4-bom.csv)),
so Brainscape adds sockets; its `libs/` folder carries a SnapMagic licence that forbids
redistributing those models, so they must not be copied into this public repo; it needs KiCad 10.
CERN-OHL-P allows relicensing the derived carrier under CERN-OHL-S with notices kept.

**Carrier cost (estimates, not a quote):** about $31–77 per assembled carrier at 5 units and
$13–43 at 50, excluding the module, footswitches, display, enclosure and knobs, built from
[JLC's assembly fee schedule](https://jlcpcb.com/help/article/pcb-assembly-price) and live LCSC
prices; add part minimum-order quantities and attrition, shipping and US import duty (JLC
pre-collects 35 % DDP; the legal basis changed on 2026-07-24 —
[JLC tariff FAQ](https://jlcpcb.com/help/article/us-tariff-policy-faq)). Use `dfm.jlcpcb.com`
and a real quote from actual Gerbers, BOM and placement files before ordering. On a 2-layer
board use the SMD `DAISY_SEED2_DFM` footprint, not the PTH one (its 0.229 mm annular ring and
zero mask web are below JLC's 2-layer recommendation of 0.25 mm,
[capabilities](https://jlcpcb.com/capabilities/pcb-capabilities)). Choose ENIG or lead-free
HASL for RoHS.

**Module availability (2026-10-07):**

| Module | Status | Notes |
| --- | --- | --- |
| [Seed3](https://daisy.audio/products/seed3) | Sold out, "Restock en route" | Restocks have sold out within hours; turn on alerts. $29.99, carton tiers from 100. |
| [Seed2 DFM](https://daisy.audio/products/daisy-seed2-dfm) | In stock (374) | $29.99. Same STM32H750 and 64 MB SDRAM; D0–D30 map to the same STM32 pins; libDaisy runs the same 24-bit / 48 kHz / postgain 1 audio path, so no engine or parity change. Carrier changes: 50-pad 1.27 mm footprint with five female 2 × 5 receptacles, a differential-to-single-ended output stage, VIN ≥ 6.1–6.5 V (the codec does not run on USB power), USB-C wired to D29/D30 (OTG_HS, which makes companion-app.md §7.2's libDaisy patch mandatory), no SWD on the pads. |
| Seed Rev7 | Discontinued by Daisy; [Electrokit](https://www.electrokit.com/en/electrosmith-daisy-seed-embedded-dsp-platform) listed 128 | 40-pin, PCM3060, micro-USB. **The owner has one; it is the prototype module.** |
| [Patch SM](https://daisy.audio/products/patch-submodule) | Sold out | Poor fit: ±12 V supply, Eurorack levels. |
| Seed3 Dev Kits | Desktop ($349) and Eurorack ($299) in stock; Pedal ($199) sold out | Each includes a Seed3. |

**A custom STM32H750 core is buildable at JLCPCB but not worth it now.** The bottleneck is the
512 Mb SDR SDRAM: every 64 MB part checked (16 M × 32 and 32 M × 16, ISSI and Alliance) was out
of stock at authorised distributors, with brokers at $31–59. The open route is two Winbond
W9825G6KH-6 (256 Mb × 16) on a 32-bit bus — the 13-row, 9-column, 4-bank geometry libDaisy
already programs — which needs a 176-pin STM32H750 (the IBK6 BGA, stocked at
[LCSC](https://www.lcsc.com/product-detail/C730200.html)). Estimated unit cost about $44 / $28 /
$23 at 5 / 50 / 500 against $25–30 for a module, plus roughly 3–6 months of engineering, 2–3
board spins and $1–2k of hard costs (all estimates). Determinism would hold in principle (same
Cortex-M7 FPv5 core), but each core design needs its own hardware-in-the-loop golden check
(determinism-profile.md §3.8, §6.6). **Triggers to revisit:** both the Seed3 and the Seed2 DFM
unavailable for more than three months, or volume above about 1,000 units a year.

## Consequences for Brainscape

- **Prototype on the Seed Rev7** (owner's decision). Same MCU, SDRAM size and libDaisy audio
  path as the Seed3, so the engine, the golden hashes and the parity contract are unchanged.
  Rev7 differs only in its PCM3060 codec, micro-USB, and no second USB on pins 36/37.
- **Carrier:** keep the 40-pin Seed footprint (takes Rev7 and Seed3), start from the Pedal Dev
  Kit's design, add sockets and mechanical retention for a stomped enclosure. Make it Seed2
  DFM-capable only if Seed3 supply is still unconfirmed when layout starts.
- **Verification:** every module model that ships needs its own HIL leg; add an SDRAM march
  test and a hot soak, because the 16 MiB history ring in SDRAM would silently break identity
  on a retention error at enclosure temperature (determinism-profile.md §6.6).
- **Before selling any pedal:** FCC Part 15B Class B SDoC for the finished pedal (a module's own
  certification does not cover the system, [47 CFR 15.101](https://www.law.cornell.edu/cfr/text/47/15.101));
  CE (EMC, RoHS, WEEE, a GPSR responsible person) and UKCA if sold abroad; ESD protection on
  jacks, USB and footswitches and enclosure bonding; export self-classification (the STM32H750
  is ECCN 5A992.c per LCSC); and a functional test for every carrier, since JLC does not test
  boards with the module fitted.
- **Supplier:** 2026 notices name Qu-Bit Electronix, Inc. (dba Daisy) as the owner of the Daisy
  products; supply questions go to Daisy/Qu-Bit.
