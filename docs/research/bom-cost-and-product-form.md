# Brainscape BOM Cost Model and Product Form Decision

*Research reference for Brainscape — costing the control-surface-heavy Daisy Seed3 pedal spec'd in
`pedal-control-surface-and-io-hardware.md` and `daisy-seed-platform.md`, and answering whether
"stereo, open, under $350" survives as a retail claim, a kit claim, or only a raw-parts-cost claim.*

## Summary

- **Daisy Seed3 pricing is re-verified today (Aug 30, 2026) and unchanged from the existing corpus
  snapshot**: $29.99 single unit, live stock of ~809 units, volume tiers $28.49/$26.99/$25.49/$23.99
  at 100/300/500/1000 pcs, sold direct from Electrosmith with a 100-unit "carton" minimum for bulk
  tiers. No action needed on `daisy-seed-platform.md`'s flagged re-check — the number holds.
  [daisy.audio/products/seed3](https://daisy.audio/products/seed3), [2026 Fulfillment/Pricing Update](https://daisy.audio/blogs/seeds-n-circuits/daisy-fulfillment-pricing-update-2026)
- **A DIY-quantity (buy-everything-yourself, hobbyist single-unit prices) parts-cost bottom-up
  estimate lands at roughly $115–180 per unit**, excluding the builder's own labor, tools, or
  shipping-per-vendor overhead — see the full line-item table below. This is directionally
  consistent with (a bit above) GuitarML Funbox's own self-reported "~$120–150 all-in" for a much
  *smaller* 125B-format board with 6 pots and none of Brainscape's MIDI/OLED/relay/encoder — a useful
  sanity check that the estimate isn't wildly off.
- **A small-batch-quantity (100–500 units) landed manufacturing cost — parts + bare PCB + enclosure +
  SMT assembly + hand-labor for panel-mount hardware — lands at roughly $95–140 per unit**, well below
  the DIY per-unit parts cost on components (volume pricing) but with real added assembly-labor costs
  a DIY builder doesn't pay (their own time is "free").
- **"Stereo, open, under $350" survives cleanly as a DIY-kit claim and trivially as a bare-PCB-plus-BOM
  claim, but is tight-to-optimistic as a blanket small-batch retail claim for the full Microcosm-parity
  feature set.** A $95–140 landed cost needs roughly a 2.5–3.5x multiplier to cover assembly QC,
  packaging, a manual, warranty reserve, payment processing, and any dealer margin — landing full-spec
  units in the **$300–450** range depending on sales channel and feature completeness, i.e. genuinely
  competitive with **Mood MKII / Onward ($399)**, not automatically cheaper than **Fable / Lore
  ($299)**. Only a slimmed-feature, direct-to-consumer SKU comfortably clears "under $350" at retail.
- **The bare-PCB-plus-BOM form is the cheapest and most defensible "under $350" claim by a wide
  margin** — selling just the Gerbers/boards (a few dollars to ~$30 depending on layer count and qty)
  and letting the builder source parts themselves mirrors exactly the Hothouse/Terrarium/Funbox model
  already documented in the corpus, and is the version of Brainscape that can legitimately say "under
  $50" not just "under $350."
- **The control surface, not the Daisy Seed3, is what makes this pedal expensive relative to
  Terrarium/Hothouse/Funbox-class boards.** The Seed3 is a flat $23.99–29.99 regardless of form; the
  ~8 pots + encoder + OLED + LED matrix + 2 relays + MIDI + USB-C + 4 jacks + footswitches + 1590DD
  enclosure the corpus specifies is what pushes total cost meaningfully above the 125B-class open
  Daisy pedals surveyed in `daisy-seed-platform.md` and `daisy-pedal-platforms.md`.
- **Assembly labor for panel-mount hardware — not SMT IC placement — is the dominant added cost at
  small-batch scale.** Pots, jacks, footswitches, DIN connectors, and relays are not machine-placed
  SMT parts on any commodity contract-assembly line (JLCPCB, PCBWay); they are hand-soldered/wired
  regardless of who does it. At small-batch this shows up as an explicit hand-assembly labor line
  (JLCPCB itself quotes a flat **hand-soldering fee plus per-joint THT cost** on top of its SMT
  pricing) rather than disappearing into a "just get it assembled" line item.
  [RayPCB PCB Assembly Cost Guide](https://www.raypcb.com/what-affect-pcb-assembly-cost/)
- **Through-hole vs. SMT is a real DIY-buildability fork, and the cheaper/more future-proof part is
  usually the harder-to-hand-solder one.** CD4051 in DIP (~$0.39–1.00, easy to socket/hand-solder) vs.
  TSSOP (~$0.26 at 1k, SMD-only) is the clearest example found; the same tension applies to the op-amp
  choice (DIP MCP6004 vs. SOIC/audio-grade OPA1652) and argues for **socketed DIP ICs on the DIY-kit
  SKU** and SMD-native design on the small-batch-assembled SKU, rather than forcing one PCB layout to
  serve both.
- **Socketing the Daisy Seed3 itself (pin headers, not direct solder) is cheap insurance
  ($1–2 in header/socket cost) against the exact codec churn `daisy-seed-platform.md` documents**
  (AK4556 → WM8731 → PCM3060 → TAC5242 across four board generations) and against ESD/power mistakes
  destroying a $24–30 module — recommended for both the DIY-kit and small-batch-assembled forms.
- **No pre-drilled enclosure exists for Brainscape's specific control-surface layout.** A 1590DD
  ($9.99 at Tayda, single-unit) is the right blank enclosure size (confirmed in the corpus against the
  real Microcosm's footprint), but drilling/routing the front panel for 8 pots + encoder + OLED + LED
  matrix + 2–3 footswitches is bespoke either way: hand-drilling for a DIY builder, or a CNC/laser
  panel-cutting service (ballpark $15–40/panel at low quantity) for a small-batch build — this is a
  real buildability and cost line the "just buy a 1590DD" framing understates.
- **Daisy Seed supply carries real single-vendor concentration risk that "open hardware" doesn't
  fully insure against.** Electrosmith is currently shipping direct with healthy stock (809 units at
  time of writing) and has published volume pricing, but 2026 already saw a pricing/fulfillment
  restructuring explicitly tied to a DRAM/memory market crunch, a retreat from retail distribution to
  direct-only sales, and an unconfirmed-but-plausible "production halt" rumor in March 2026 — the
  Seed3 (or its successor) is Brainscape's single most expensive and least substitutable line item,
  sourced from one small manufacturer.
  [PedalPCB forum thread](https://forum.pedalpcb.com/threads/psa-electrosmith-has-apparently-temporarily-halted-production-of-daisy-seed.28926/)
- **A tiered product form (open files → bare PCB → curated DIY kit → small-batch assembled) lets
  Brainscape make an honest, different price/feature/support claim at each tier**, rather than one
  blanket claim that's true for the cheapest tier and false for the most-assembled one — this is the
  core recommendation of this document.

## 1. Re-verifying Daisy Seed3/Seed2 DFM pricing and stock (today, Aug 30 2026)

`daisy-seed-platform.md` flagged its pricing snapshot as needing re-check before BOM lock. Re-pulled
directly from the live product page and the 2026 pricing-update blog post today:

| Quantity | Seed3 unit price |
|---|---|
| 1 | $29.99 |
| 100 | $28.49 |
| 300 | $26.99 |
| 500 | $25.49 |
| 1,000 | $23.99 |

- **Stock**: "In Stock — 809 Available" at time of writing (single-unit checkout), i.e. direct orders
  are shipping normally right now. [daisy.audio/products/seed3](https://daisy.audio/products/seed3)
- **Volume/carton rule confirmed**: Electrosmith's own 2026 pricing-update post states bulk pricing
  requires purchasing "in multiples of the carton quantity since these come pre-packaged in 100 piece
  boxes" — i.e. **100-unit minimum increments** for any volume tier below 100 doesn't exist; Brainscape
  cannot buy, say, 40 units at a discount — it's 1-off retail price up to 99 units, then jumps to the
  100-carton tier. [2026 Fulfillment/Pricing Update](https://daisy.audio/blogs/seeds-n-circuits/daisy-fulfillment-pricing-update-2026)
- **Conclusion: no change needed to the existing corpus's numbers.** The pricing snapshot in
  `daisy-seed-platform.md` was accurate and remains accurate today. The underlying risk that prompted
  the flag — 2026 DRAM-market-driven price/allocation volatility — is still real and worth tracking
  close to actual BOM lock, but the specific dollar figures do not need correction right now.
- **Seed2 DFM** carries the identical price/volume structure per the same source page structure
  (not independently re-fetched today, since `daisy-seed-platform.md` already sourced this from the
  same 2026 pricing-update post cited above and nothing in that post's mechanism has changed).

## 2. Line-item BOM: DIY quantity (1–10) vs. small-batch quantity (100–500)

Prices below are gathered from primary distributor/retailer pages (DigiKey, Mouser, LCSC, Tayda
Electronics — the standard DIY-pedal parts source) at the time of writing. **DIY-quantity** figures are
real single-unit (or small-pack) retail prices actually payable today by a hobbyist buying one of each
part from a normal consumer-facing storefront. **Small-batch figures are estimates interpolated from
published volume-pricing curves** (Digikey/LCSC price breaks at 100/300/500/1000) where a distributor
publishes them, and **flagged as estimates** where no small-batch-specific price point was directly
observed (LCSC/Alibaba-tier OEM sourcing for OLEDs and connectors was not directly quoted in this pass
and should be re-verified with an actual quote before BOM lock).

| Line item | Example part | DIY qty (1–10) | Small-batch qty (100–500, est.) | Source / note |
|---|---|---|---|---|
| Compute module | Daisy Seed3 | $29.99 | $23.99–28.49 | [daisy.audio](https://daisy.audio/products/seed3) — see §1 |
| Quad op-amp, ×2 (stereo front end) | MCP6004 (DIP, hand-solderable) | $0.59 ea → **$1.18** | ~$0.30–0.40 ea SOIC → **~$0.70** | [DigiKey MCP6004-I/P](https://www.digikey.com/en/products/detail/microchip-technology/MCP6004-I-P/MCP6004-I-P-ND/523060): $0.59(1)/$0.44(100) DIP |
| — *audio-grade alternative* | OPA1652 (dual, per Electrosmith's own Seed3 pedal reference circuit; need 2 per channel = 4 for stereo in, or 2 more for stereo out) | ~$2.75 ea (tube qty) → **~$11–16.50 for 4–6 pkgs** | not independently quoted; DigiKey listing shows tube-qty MOQ ~211 units at $2.75, implying real volume pricing is lower but unverified | [DigiKey OPA1652AID](https://www.digikey.com/en/products/detail/texas-instruments/OPA1652AIDGK/3053284) — recommended in `pedal-control-surface-and-io-hardware.md` §1.3 over MCP6004 for noise floor |
| Voltage regulation (LDO, misc rails) | AMS1117-3.3 or equiv | ~$0.20–0.29 ea → **~$0.50** | $0.09–0.15 ea (100+) → **~$0.20** | [DigiKey UMW AMS1117-3.3](https://www.digikey.com/en/products/detail/umw/AMS1117-3-3/17635254): $0.29(1)/$0.1524(100); [LCSC AMS1117-3.3](https://www.lcsc.com/product-detail/Linear-Voltage-Regulators-LDO_Advanced-Monolithic-Systems-AMS1117-3-3_C6186.html) shows sub-$0.10 clone variants |
| Pot mux, ×1 | CD4051 (8:1 analog mux) | $0.39–1.00 → **~$0.75** | $0.26–0.39 (1000, TSSOP) → **~$0.35** | [DigiKey CD4051BE / CD4051BPWR](https://www.digikey.com/en/products/detail/texas-instruments/CD4051BE/67305): $0.385(1000) DIP, $0.257(1000) TSSOP |
| Switch shift register, ×2 (daisy-chained) | CD4021 | $1.30 ea → **$2.60** | est. ~$0.50–0.80 ea (100+, not directly quoted) → **~$1.20** | [DigiKey CD4021BE](https://www.digikey.com/en/products/detail/texas-instruments/CD4021BE/67261): $1.30 single-unit; volume price not confirmed in this pass |
| Pots, ×8 | Alpha 9mm RD901F (or Tayda-house equivalent) | $0.59–1.29 ea → **~$8.00** | est. ~$0.30–0.45 ea at qty100+ (Mouser/distributor volume, not directly quoted) → **~$3.20** | [Tayda 9mm pot listings](https://www.taydaelectronics.com/catalogsearch/result/?q=9mm+potentiometer): $0.59 (house brand), $1.29 (Alpha brand) |
| Encoder, ×1 (w/ push switch) | Bourns PEC11R-4220K / Alpha equivalent | $1.59–1.82 → **~$1.75** | ~$1.29 (50+, TME) → **~$1.20** | [Tayda 11mm encoder](https://www.taydaelectronics.com/catalogsearch/result/?q=rotary+encoder+with+switch): $1.59; TME lists $1.29 at 50+ for the Bourns part |
| Display | SSD1306 0.96" I2C OLED module | $2.49–5.48 → **~$4.00** | est. $1.50–2.50 (OEM/direct-from-panel-maker volume, not directly quoted) → **~$2.00** | [eBay bulk listing](https://www.ebay.com/itm/197226090069) ($5.48→$5.15 at qty4); [BuyDisplay](https://www.buydisplay.com/128x64-oled-i2c-0-96-display-white-color-connector-fpc-ssd1306) ($2.49) |
| LED-grid driver + matrix | IS31FL3731 + discrete/charlieplexed LEDs | ~$0.90–1.65 (driver) + ~$3–4 (LEDs) → **~$5.00** | ~$0.50–0.79 (driver, 1k+) + ~$1.00–1.50 (LEDs bulk) → **~$2.00** | [LCSC IS31FL3731-QFLS2-TR](https://www.lcsc.com/product-detail/C191206.html): $0.90; DigiKey SSOP variant $1.65(1)/$0.79(10k) |
| Bypass relays, ×2 (latching) | Panasonic TQ2-L or Omron G6SU-2 | $1.72–5.10 ea → **~$5.00** | est. ~$1.20–2.00 ea (100+, not directly quoted) → **~$3.00** | [DigiKey TQ2-L-9V](https://www.digikey.com/en/products/detail/panasonic-electric-works/TQ2-L-9V/649329) $1.72; [DigiKey G6SU-2 DC5](https://www.digikey.com/en/products/detail/omron-electronics-inc-emc-div/G6SU-2-DC5/369302) $2.25 |
| Relay driver components | small transistors, flyback diodes, resistors | **~$1.00** | **~$0.30** | general passive pricing, not itemized |
| MIDI in/out/thru | 2–3× 5-pin DIN jack + 1× optocoupler (H11L1M or equiv) | $0.30–0.32 ea jack ×3 (~$0.90) + $1.07 opto → **~$2.00** | est. ~$0.15–0.20 ea jack + ~$0.40–0.60 opto (100+) → **~$1.00** | [Tayda 5-pin MIDI jacks](https://www.taydaelectronics.com/catalogsearch/result/?q=5+pin+din+jack): $0.30–0.32; [DigiKey H11L1M](https://www.digikey.com/en/products/detail/on-semiconductor/H11L1M/284866): $1.07 |
| Expression jack, ×1 | 1/4" TRS jack | $1.16–1.99 → **~$1.50** | est. ~$0.50–0.80 (100+) → **~$0.65** | [Tayda 1/4" TRS jack](https://www.taydaelectronics.com/6-35mm-1-4-stereo-chassis-socket-jack-3-terminals.html): $1.16 (generic), $1.99 (Neutrik/REAN) |
| USB-C (panel jack wired to Seed3's own breakout pins) | generic USB-C panel/PCB receptacle | ~$0.20–2.00 (module/breakout dependent) → **~$2.00** | est. ~$0.20–0.40 SMD (100+) → **~$0.50** | search-level pricing only (SHOU HAN/DEALON SMD parts $0.04–0.07 bare; a usable panel-mount breakout runs higher) — **treat as unverified, re-quote a specific part before BOM lock** |
| 1/4" jacks, ×4 (stereo in ×2, stereo out ×2) | Switchcraft/Neutrik-quality or Tayda generic | $1.16–1.99 ea → **~$7.96** (quality) / **~$4.64** (generic) | est. ~$0.80–1.20 ea (OEM volume, not directly quoted) → **~$4.00** | [Tayda 1/4" jack](https://www.taydaelectronics.com/6-35mm-1-4-stereo-chassis-socket-jack-3-terminals.html); [Switchcraft/B&H](https://www.bhphotovideo.com/c/product/929790-REG/switchcraft_e11bpkg_e_series_1_4_mono.html) $3.99 branded mono |
| Footswitches, ×2–3 (momentary, for relay trigger / tap / looper) | PIC/Alpha momentary stomp switch | $1.59–2.99 ea ×3 → **~$7.50** | est. ~$1.00–1.50 ea (100+) → **~$3.75** | [Tayda momentary footswitch search](https://www.taydaelectronics.com/catalogsearch/result/?q=momentary+footswitch): $1.59–2.99 |
| Bare PCB fabrication | 2-layer, ~160×100mm (est.), JLCPCB-class fab | est. $5–15/board at qty 5–10 (setup-fee-amortized) → **~$8.00** | est. $1.50–4.00/board at qty 100–500 → **~$3.00** | interpolated from [JLCPCB 4-layer 100×100mm pricing example](https://jlcpcb.com/blog/JLCPCB-High-precision-Multi-layer-PCB-Extended-to-8-20-Layer) (~$70.60 board cost /100pcs at 100×100mm 4L) and general prototype pricing ($2–7 for 5pcs small 2L boards); **not a firm quote — get a real Gerber-based quote before BOM lock** |
| Enclosure | Hammond 1590DD-class diecast, undrilled | $9.99 | est. $6–8 (100+, not directly quoted) → **~$8.00** | [Tayda 1590DD listing](https://www.taydaelectronics.com/1590dd-style-aluminum-diecast-enclosure.html): $9.99 single-unit |
| Panel drilling/machining | custom CNC/laser-cut front panel for Brainscape's specific layout | est. $0 (hand-drilled by builder, DIY labor) to $15–40 (outsourced service, low qty) | est. $5–15/panel at qty 100–500 (tooling/jig amortized) | **no primary source found for a Brainscape-specific drilled panel** — no off-the-shelf pre-drilled option exists for a custom control-surface layout; see §4 |
| Misc (wire, solder, hardware, screws, knobs, DC jack, passives) | — | **~$10–15** | **~$5–8** | general estimate, not itemized |
| **DIY-quantity subtotal (components + PCB + enclosure, excludes builder's own labor)** | | | | **≈ $115–145** (budget op-amp path) to **≈ $125–180** (audio-grade OPA1652 path, quality jacks) |
| **Small-batch-quantity subtotal (components + PCB + enclosure, excludes assembly labor)** | | | | **≈ $65–75** |

**Cross-check against known prior art**: `daisy-seed-platform.md` cites GuitarML/FunBox — a much
*simpler* 125B-format open Daisy pedal (6 pots, no display, no encoder, no MIDI, no relay bypass, no
LED matrix) — as self-reported at **"~$120–150 all-in with enclosure/pots/jacks."** Brainscape's DIY
subtotal above ($115–180) sits in the same neighborhood despite having a meaningfully larger control
surface, which is a reasonable, not suspicious, result once you account for (a) Funbox's own figure
likely folding in per-vendor shipping and minimum-buy waste that a naive line-item sum doesn't capture,
and (b) Brainscape's extra parts (MIDI, OLED, encoder, relay, LED matrix, bigger enclosure) being
individually cheap even if numerous. **Treat the DIY total as a $115–180 band, not a single number**,
and budget toward the higher end for a first prototype run (small-vendor minimum buys, wasted/scrapped
parts, and shipping across 4–5 separate vendors — Tayda, Mouser/DigiKey, Electrosmith direct, an
enclosure supplier, and a PCB fab — are real and not reflected in per-unit list prices).

## 3. From components to a landed manufactured unit (small-batch)

The BOM line-item table above stops at "components + bare PCB + enclosure." Getting to an actual
sellable assembled unit adds labor that a DIY builder pays in their own time but a small-batch run
must pay for in cash:

- **SMT assembly** (ICs, passives, the digital-side placement): JLCPCB and comparable contract
  assemblers price this as PCB cost + BOM cost + a setup fee (**typically $100–400, amortized across
  the run**) + per-point placement (**$0.01–0.05/point SMT**). For a moderately dense mixed-signal
  board (op-amps, mux, shift registers, LED driver, passives — maybe 60–100 SMT points), this plausibly
  lands around **$5–15/board** once setup is spread over 100–500 units.
  [RayPCB PCB Assembly Cost Guide](https://www.raypcb.com/what-affect-pcb-assembly-cost/)
- **Hand assembly / THT labor for panel-mount hardware**: pots, the encoder, footswitches, DIN jacks,
  1/4" jacks, and the relays are not SMT-placeable — they are either through-hole soldered by hand or
  panel-wired with lugs/harnesses. JLCPCB's own pricing model calls this out explicitly as an added
  **hand-soldering labor fee plus a per-joint cost** on top of the SMT line. For Brainscape's control
  surface (roughly 20+ discrete panel-mount parts with multiple leads/lugs each), this is plausibly
  **$15–30/unit** at small-batch contract-assembly labor rates — a cost that simply doesn't exist in
  the DIY-kit form, where the builder's own time absorbs it for free.
  [RayPCB PCB Assembly Cost Guide](https://www.raypcb.com/what-affect-pcb-assembly-cost/)
- **Panel machining**: see §4 below — a real, separate cost from the raw enclosure.
- **Test/QC and burn-in**: not itemized here (no primary-source figure found for a board of this
  complexity), but any small-batch audio-hardware run budgets *some* per-unit time for power-on test,
  audio-path verification, and relay-click/MIDI-loopback checks — flagged as an unquantified addition.

**Working small-batch landed-cost estimate**: $65–75 (components+PCB+enclosure) + $5–15 (SMT) +
$15–30 (hand assembly/panel wiring) + an unquantified QC/test allowance ⇒ **roughly $95–140 per unit**,
before packaging, a printed manual, a warranty reserve, payment processing, and any dealer margin. This
is a planning-grade estimate synthesized from general contract-assembly pricing models, **not a
JLCPCB/PCBWay quote pulled against Brainscape's actual Gerbers/BOM** — treat it as a first-pass budget
figure to validate with a real quote once schematics exist.

## 4. Manufacturing and fulfilment realities

- **JLCPCB/PCBWay assembly for a board of this complexity is dominated by hand labor, not SMT
  placement cost.** The IC-and-passives side of the board (op-amps, muxes, shift registers, LED
  driver, regulator) is cheap to machine-place even at low-to-mid volume; the panel-mount control
  surface (pots, encoder, footswitches, jacks, DIN connectors, relays) is not, regardless of which
  contract manufacturer is used, because none of those parts are SMT-compatible in their standard
  footprints. Brainscape should budget assembly cost around this reality rather than assuming "send it
  to JLCPCB" makes the whole board turnkey-cheap the way a pure-digital board would be.
- **No off-the-shelf pre-drilled enclosure exists for Brainscape's control-surface layout.** The 1590DD
  itself is a standard, cheaply-stocked blank ($9.99, Tayda) — but pre-drilled *templates* only exist
  for well-established, popular single-purpose pedal formats (single-knob fuzzes, etc.), not for a
  bespoke 8-pot+encoder+OLED+LED-matrix+multi-footswitch layout. This means:
  - **DIY builders must hand-drill/file the panel themselves** — a real skill and tooling barrier
    (drill press, step bits, a way to lay out an accurate template) that is arguably a bigger hurdle
    than any of the soldering in this BOM, and one the existing corpus's buildability discussion
    doesn't fully surface.
  - **A small-batch run needs a CNC-routed or laser-cut panel from a job shop** (e.g., a
    Front-Panel-Express-style service or a local CNC shop), which was not directly quoted in this pass
    but plausibly runs **$15–40/panel at low quantity**, dropping meaningfully once a run justifies a
    stamping die or a dedicated CNC fixture — worth getting an actual quote against a real DXF once the
    panel layout is finalized.
  - **Recommendation**: publish a laser-cuttable/CNC-ready panel file (DXF/SVG) alongside the Gerbers
    from day one — this is a cheap, high-leverage open-hardware deliverable that meaningfully lowers
    the DIY barrier described above, the same way Gerbers themselves lower the PCB barrier.
- **Daisy Seed lead-time/allocation risk is real but currently benign.** As of today's re-verification
  (§1), Electrosmith is shipping direct with hundreds of units in stock and clear volume pricing. But
  2026 already saw (a) a pricing increase explicitly attributed to AI-driven DRAM cost pressure, (b) a
  shift to direct-only sales (retailers were cut off), and (c) an unconfirmed but plausible March 2026
  "production halted" rumor that Electrosmith's own communications did not fully rebut, only reframed
  as a pricing/fulfillment restructuring. **This is Brainscape's single largest concentration risk**:
  every product form (DIY, kit, or small-batch) depends on one small manufacturer's continued
  willingness and ability to sell the Seed3 (or a compatible successor) at a stable price. Unlike the
  rest of the open-hardware stack (which anyone can fab from Gerbers), the compute module itself cannot
  be independently manufactured by Brainscape or its community if Electrosmith's supply falters — worth
  an explicit contingency note in the hardware README (e.g., documenting the pin-compatible upgrade
  path across Seed revisions that `pedal-control-surface-and-io-hardware.md` already confirmed, so a
  future Seed4-class module can be dropped in without a board respin).
  [PedalPCB forum thread on the March 2026 halt rumor](https://forum.pedalpcb.com/threads/psa-electrosmith-has-apparently-temporarily-halted-production-of-daisy-seed.28926/)

## 5. Price-anchor comparison: does "stereo, open, under $350" survive?

| Anchor | Price | Stereo? | Open? | Comparable Brainscape form |
|---|---|---|---|---|
| Montreal Assembly Count to Five | $170 | No (mono) | No | Closest to Brainscape's **DIY-kit** price band if the kit is priced to undercut it |
| Walrus Fable / Lore | $299 | No (mono) | No | The price zone a **slimmed, DTC small-batch SKU** could realistically hit |
| Chase Bliss Mood MKII / Onward | $399 | Yes | No | The price zone the **full-feature small-batch assembled unit** most realistically lands in |
| Hologram Microcosm | $459 | Yes | No | Feature-parity target; Brainscape's full spec costs less to manufacture than this implies Hologram's own margin/BOM structure runs |

Using the §3 landed-cost estimate of **$95–140/unit** and a typical small-hardware-business retail
multiplier of roughly **2.5–3.5x landed cost** (covering packaging, a manual, warranty reserve, payment
processing, and — if sold through dealers — a distributor/dealer margin on top of Brainscape's own):

- **Low end of the estimate ($95 landed, DTC, thin margin, ~2.5x)** → **~$240–330** retail — genuinely
  under $350, competitive with or cheaper than Fable/Lore, and would be a legitimate, strong marketing
  claim ("the only stereo, open pedal in this category under $350").
- **High end of the estimate ($140 landed, normal margin, ~3x, or any dealer-channel markup)** →
  **~$400–490** retail — lands next to Mood MKII/Onward/Microcosm, not under them.

**Honest conclusion**: "Stereo, open, under $350" is **not automatically true for a fully-loaded,
Microcosm-parity, dealer-distributed small-batch unit** — it depends heavily on (a) which features
survive into v1 (the OLED+LED-matrix combo, MIDI I/O, and relay bypass are the priciest additions after
the Seed3 itself), (b) direct-to-consumer vs. dealer distribution, and (c) how thin a margin the project
is willing to run as an open-source/community effort rather than a venture-scale hardware business. It
**does survive cleanly** as:
1. **A DIY-kit or self-sourced-parts claim** ($115–180, see §2) — genuinely, comfortably under $350,
   and in fact competitive with or cheaper than Count to Five at $170 while offering far more depth and
   real stereo I/O.
2. **A bare-PCB-plus-BOM claim** — the cheapest possible framing (PCB fab cost only, likely under $30
   for a small run, with the builder sourcing everything else) — trivially under $350.
3. **A slimmed, direct-to-consumer small-batch SKU** — plausible under $350 if Brainscape cuts the LED
   matrix (keeping just the OLED) and/or sells factory-direct without a dealer margin.

It is **at genuine risk of being an overpromise** if stated as a blanket claim covering a
fully-featured, retail-channel, small-batch assembled unit — that SKU more honestly belongs in the
$349–449 band, adjacent to Mood MKII and Onward rather than underneath Fable and Lore.

## 6. Product form decision

Four forms were evaluated, following the corpus's Gap Analysis finding that open/hackable + genuinely
low-cost + genuinely stereo is Brainscape's clearest unclaimed wedge (`granular-pedal-landscape.md`):

### 6.1 Gerbers/firmware only (always-free baseline)

- **Buildability**: N/A — this is the substrate, not a product.
- **Support burden**: none directly, but is the foundation every other tier depends on.
- **Supply resilience**: maximal — this is the "can never truly disappear" claim
  `granular-pedal-landscape.md` recommends leaning on, directly analogous to Mutable Clouds/Parasites.
- **Recommendation**: publish from day one regardless of which paid tiers exist, including the panel
  DXF/SVG files flagged in §4.

### 6.2 Bare-PCB-plus-BOM

- **Cost**: ~$5–30 for boards (qty-dependent, see §2's PCB-fab line), builder sources everything else
  (~$90–150 in components per §2's small-batch-component subtotal, scaled toward DIY per-unit pricing
  since a lone builder can't hit volume breaks).
- **Buildability**: hardest tier — no curated parts kit, no guaranteed part availability/compatibility,
  full through-hole-vs-SMT and Daisy-socketing decisions land entirely on the builder.
- **Support burden**: highest per-unit (every builder's sourcing mistakes, substitutions, and
  soldering errors become a support request), but this is the tier where an active community (Discord,
  forum) most directly substitutes for paid support — the Parasites/Mutable Clouds precedent.
- **Supply resilience**: excellent — anyone can re-fab boards indefinitely from the same Gerbers.
- **Recommendation**: keep this tier alive permanently as the "true DIY" option and the proof-point for
  the "this design can never disappear" marketing claim, even once other tiers exist.

### 6.3 Curated DIY kit (all parts bundled, no assembly)

- **Cost**: the $115–180 DIY-component band from §2, plus a modest kitting/curation markup (sourcing,
  packaging, a build guide) — realistically **$150–230 retail**, comfortably undercutting Count to Five
  ($170) at the low end while offering far more depth and genuine stereo I/O.
- **Buildability**: meaningfully easier than bare-PCB — guaranteed correct part values/footprints,
  ideally with **socketed DIP ICs where a DIP/SMD choice exists** (CD4051, the op-amps) so a hobbyist
  without reflow/hot-air tools isn't blocked, and a **socketed Daisy Seed3** (see Summary) so the
  priciest single part is field-replaceable without desoldering skill.
- **Support burden**: moderate — a curated kit removes sourcing-mismatch failures but soldering-quality
  and assembly-order mistakes remain a real support load; a well-written build guide (à la Hothouse's
  or Funbox's own documentation) does most of the work here.
- **Supply resilience**: good — Brainscape (or any future maintainer) needs ongoing access to the same
  parts list, but nothing here is harder to source than the bare-PCB tier's own BOM.
- **Recommendation**: this is the tier where **"stereo, open, under $350" is not just true but
  underselling it** — position it explicitly against Count to Five ($170, mono) as "everything Count to
  Five does, in stereo, for a comparable price," which is a defensible, well-supported claim given the
  cost model above.

### 6.4 Small-batch assembled units (100–500 run)

- **Cost**: the $95–140 landed-manufacturing estimate from §3, translating to a realistic
  **$300–450 retail** band depending on feature completeness and sales channel (§5).
- **Buildability**: N/A for the end customer — this is the "just works" tier, and should be the one
  where SMD-native design, factory-soldered Seed3 (or a deliberately-chosen socketed module if
  field-serviceability is prioritized over marginal cost), and full QC/burn-in apply.
- **Support burden**: lowest per-unit (tested, working hardware shipped), but shifts the burden onto
  Brainscape (or a manufacturing partner) to own inventory, warranty, and returns — a real ongoing
  operational cost the other three tiers avoid entirely, and the exact operational load that made
  Habit's discontinuation and Beebo's supply collapse (`granular-pedal-landscape.md`) into genuine
  cautionary tales rather than one-off bad luck.
- **Supply resilience**: weakest of the four tiers in isolation (exactly the Beebo/Habit failure mode)
  — **but uniquely among this survey's boutique makers, Brainscape's own open files mean this tier's
  disappearance doesn't kill the product**, since the bare-PCB and kit tiers persist independently.
  This is worth stating explicitly in marketing: "if the small-batch run ever stops, the design does
  not disappear with it" is a claim none of Hologram, Chase Bliss, or Poly Effects can make.
- **Recommendation**: price this tier honestly at $350–450 for the full Microcosm-parity spec rather
  than force-fitting a sub-$350 number onto it; if a sub-$350 small-batch SKU is strategically
  important (e.g., as the lead marketing price point), ship it as a **explicitly reduced-feature
  variant** (OLED only, no LED matrix; 2 jacks instead of 4 for a mono-in/stereo-out variant; no MIDI
  DIN, USB-MIDI only) rather than quietly under-costing the full-feature version.

### 6.5 Recommended structure: tiered, not single-SKU

Ship all four, explicitly positioned:

1. **Free**: Gerbers, BOM, firmware, panel DXF — the permanent, can't-disappear baseline.
2. **~$20–30**: bare PCB set (or PCB + Seed3, since that's the one truly hard-to-substitute part) —
   for builders who already have a parts bin or prefer to source everything themselves.
3. **~$150–230**: curated DIY kit, socketed ICs + socketed Seed3, full build guide — the "stereo Count
   to Five" pitch, and the tier where the "under $350" claim is not just true but generous.
4. **~$350 (slimmed) to ~$449 (full-spec)**: small-batch assembled, tested units — positioned honestly
   against Mood MKII/Onward/Microcosm for the full-spec version, and against Fable/Lore for a slimmed
   variant, rather than claiming Microcosm-parity at a Fable/Lore price.

## Recommendations for Brainscape

1. **Do not lock a single "under $350" retail claim across every product form.** Use the tiered
   structure in §6.5 so the claim is true and specific at each price point instead of technically-true
   only for the cheapest tier while implicitly promised for all of them.
2. **Design two PCB variants from one schematic, not one board for both DIY and small-batch.** Socket
   DIP-package ICs (CD4051, op-amps) and the Daisy Seed3 itself on the DIY-kit board; go SMD-native and
   factory-solder the Seed3 on the small-batch-assembled board. This resolves the DIP-vs-SMD cost/
   buildability tension identified in §2 without compromising either tier.
3. **Publish a laser-cut/CNC-ready panel file (DXF/SVG) alongside the Gerbers from day one.** No
   pre-drilled enclosure exists for this control-surface layout (§4); this is the single highest-
   leverage, lowest-cost thing Brainscape can do to lower the real (and currently unaddressed) DIY
   buildability barrier that panel drilling represents.
4. **Get a real Gerber-based PCB fab/assembly quote from JLCPCB and PCBWay before BOM lock**, not the
   interpolated estimates in this document — the bare-PCB and SMT-assembly figures here are directional
   planning numbers, not firm quotes, flagged accordingly in §2 and §3.
5. **Document an explicit Daisy-module upgrade/substitution path in the hardware README**, given the
   single-vendor concentration risk in §4 (Electrosmith's 2026 pricing/allocation volatility) — lean on
   the pin-compatibility Seed3 already has with the Rev7 footprint (confirmed in
   `pedal-control-surface-and-io-hardware.md`) as precedent that a future module swap is a realistic,
   low-risk mitigation, not a hypothetical one.
6. **Treat the OLED+LED-matrix combo, MIDI I/O, and relay bypass as the explicit cost/feature levers**
   for hitting different price points, since they are the largest cost deltas above a bare-bones
   pot-only board (§2) — a "slimmed" sub-$350 small-batch SKU should cut from this list first, not from
   the pot count or stereo I/O that define Brainscape's core positioning.
7. **Price the curated DIY kit to explicitly undercut/match Count to Five ($170)** while marketing it
   as strictly more capable (stereo vs. mono, deeper control surface) — this is the cleanest, most
   defensible "under $350" win available and should be the headline consumer-facing price claim rather
   than the small-batch assembled unit's price.
8. **Re-verify the small-batch component volume-pricing estimates flagged in §2 (CD4021, OLED, USB-C
   connector, expression jack, 1/4" jacks, footswitches, enclosure) with real distributor quotes at
   actual target quantities before BOM lock** — several of these were interpolated from list-price
   curves rather than directly observed 100+/500+ unit pricing, and are individually small but
   collectively could shift the small-batch subtotal by 10–20%.

## Sources

- Daisy Seed3 product page (price/stock, re-verified today) — https://daisy.audio/products/seed3
- Daisy 2026 Fulfillment/Pricing Update blog post — https://daisy.audio/blogs/seeds-n-circuits/daisy-fulfillment-pricing-update-2026
- PedalPCB forum — "PSA: ElectroSmith has apparently temporarily halted production of Daisy Seed" — https://forum.pedalpcb.com/threads/psa-electrosmith-has-apparently-temporarily-halted-production-of-daisy-seed.28926/
- DigiKey — MCP6004-I/P (quad op-amp) — https://www.digikey.com/en/products/detail/microchip-technology/MCP6004-I-P/MCP6004-I-P-ND/523060
- DigiKey — OPA1652AIDGK (audio-grade dual op-amp) — https://www.digikey.com/en/products/detail/texas-instruments/OPA1652AIDGK/3053284
- DigiKey — CD4051BE / CD4051BPWR (analog mux, DIP vs. TSSOP) — https://www.digikey.com/en/products/detail/texas-instruments/CD4051BE/67305
- DigiKey — CD4021BE (shift register) — https://www.digikey.com/en/products/detail/texas-instruments/CD4021BE/67261
- DigiKey — AMS1117-3.3 (UMW variant, LDO regulator) — https://www.digikey.com/en/products/detail/umw/AMS1117-3-3/17635254
- LCSC — AMS1117-3.3 (Advanced Monolithic Systems) — https://www.lcsc.com/product-detail/Linear-Voltage-Regulators-LDO_Advanced-Monolithic-Systems-AMS1117-3-3_C6186.html
- LCSC — IS31FL3731-QFLS2-TR (LED matrix driver) — https://www.lcsc.com/product-detail/C191206.html
- DigiKey — IS31FL3731-QFLS2-EB — https://www.digikey.com/en/products/detail/issi-integrated-silicon-solution-inc/IS31FL3731-QFLS2-EB/4286504
- DigiKey — Omron G6SU-2 DC3/DC5 (latching relay) — https://www.digikey.com/en/products/detail/omron-electronics-inc-emc-div/G6SU-2-DC3/306931 , https://www.digikey.com/en/products/detail/omron-electronics-inc-emc-div/G6SU-2-DC5/369302
- DigiKey — Panasonic TQ2-L-5V/9V/12V (latching relay) — https://www.digikey.com/en/products/detail/panasonic-electric-works/TQ2-L-9V/649329
- DigiKey — H11L1M (MIDI input optocoupler) — https://www.digikey.com/en/products/detail/on-semiconductor/H11L1M/284866
- Tayda Electronics — 1590DD-style aluminum diecast enclosure — https://www.taydaelectronics.com/1590dd-style-aluminum-diecast-enclosure.html
- Tayda Electronics — 3PDT stomp switch (PCB) — https://www.taydaelectronics.com/3pdt-stomp-foot-pedal-switch-pcb.html
- Tayda Electronics — momentary footswitch search results — https://www.taydaelectronics.com/catalogsearch/result/?q=momentary+footswitch
- Tayda Electronics — 5-pin MIDI/DIN jack search results — https://www.taydaelectronics.com/catalogsearch/result/?q=5+pin+din+jack
- Tayda Electronics — 6.35mm 1/4" stereo chassis jack — https://www.taydaelectronics.com/6-35mm-1-4-stereo-chassis-socket-jack-3-terminals.html
- Tayda Electronics — 9mm potentiometer search results — https://www.taydaelectronics.com/catalogsearch/result/?q=9mm+potentiometer
- Tayda Electronics — rotary encoder w/ switch search results — https://www.taydaelectronics.com/catalogsearch/result/?q=rotary+encoder+with+switch
- Mouser — Alpha (Taiwan) RD901F-40-15R1-B10K-00DL1 (9mm pot) — https://eu.mouser.com/en/ProductDetail/Alpha-Taiwan/RD901F-40-15R1-B10K-00DL1
- Mouser / TME / Octopart — Bourns PEC11R-4220K-S0024 (encoder) pricing references — https://octopart.com/pec11r-4220k-s0024-bourns-26648305
- eBay bulk listing — 0.96" SSD1306 OLED module pricing tiers — https://www.ebay.com/itm/197226090069
- BuyDisplay — 128x64 OLED I2C 0.96" SSD1306 module — https://www.buydisplay.com/128x64-oled-i2c-0-96-display-white-color-connector-fpc-ssd1306
- Switchcraft E11BPKG 1/4" mono panel-mount jack (B&H) — https://www.bhphotovideo.com/c/product/929790-REG/switchcraft_e11bpkg_e_series_1_4_mono.html
- RayPCB — "PCB Assembly Cost in 2026: Complete Pricing Guide + Calculator" (setup fee, per-joint SMT/THT cost model) — https://www.raypcb.com/what-affect-pcb-assembly-cost/
- JLCPCB — "High-precision Multi-layer PCB Extended to 8-20 Layer" (4-layer 100×100mm pricing example) — https://jlcpcb.com/blog/JLCPCB-High-precision-Multi-layer-PCB-Extended-to-8-20-Layer
- JLCPCB — PCBA cost breakdown blog (cost-driver framework, no firm figures) — https://jlcpcb.com/blog/pcba-cost-breakdown
- EasyEDA forum — JLCPCB SMT assembly minimum-quantity discussion — https://easyeda.com/forum/topic/JLCPCB-SMT-minimum-quantity-db87f867cb8243d4b81c18b9186e0fb9
- Brainscape internal reference — `docs/research/granular-pedal-landscape.md` (price anchors, gap analysis)
- Brainscape internal reference — `docs/research/daisy-seed-platform.md` (Daisy Seed3 pricing/history baseline, Funbox cost cross-check)
- Brainscape internal reference — `docs/research/pedal-control-surface-and-io-hardware.md` (control-surface spec, CD4051/CD4021/OPA1652/relay/enclosure recommendations this BOM is built against)

## Open questions

- **Small-batch (100–500) volume pricing for CD4021, the OLED module, the USB-C connector, the
  expression jack, the 4× 1/4" jacks, footswitches, and the enclosure was not directly observed from a
  primary distributor quote in this pass** — the figures in §2's "small-batch" column for these items
  are interpolated estimates from list-price curves on adjacent parts, not confirmed 100+/500+ unit
  price breaks. Get direct Mouser/DigiKey/LCSC/OEM quotes at Brainscape's actual target quantities
  before BOM lock.
- **No firm PCB fabrication or SMT/THT assembly quote exists for Brainscape's actual board(s)** — every
  PCB-fab and assembly-labor figure in this document is interpolated from general JLCPCB pricing
  examples and cost-model articles, not a quote generated against real Gerbers/BOM/pick-and-place
  files. This is the single most important open item before any BOM-lock decision, since it's also the
  largest source of estimate uncertainty in the small-batch landed-cost figure.
- **No firm quote exists for a CNC/laser-cut custom front panel** matching Brainscape's specific control
  layout — the $15–40/panel (low-qty) and $5–15/panel (100+ qty) figures in §4 are order-of-magnitude
  estimates from general knowledge of panel-fab services, not a quote against a real DXF.
- **Whether one or two PCBs (e.g., a separate control/footswitch daughterboard vs. one monolithic
  board) is the right physical architecture** was not resolved here — it directly affects both the
  bare-PCB fab cost (more, smaller boards vs. fewer, larger ones price differently) and the DIY-vs-SMT
  socketing recommendation in Recommendation #2, and should be decided alongside the actual schematic/
  layout work, not purely from a cost-model document like this one.
- **The USB-C connector line item is the least-verified single component in this BOM** — pricing
  search results returned bare SMD receptacle prices ($0.04–0.07) that are almost certainly not
  representative of a real panel-mount-friendly part wired to Seed3's external USB breakout pins per
  `pedal-control-surface-and-io-hardware.md` §5.2; a specific part number should be chosen and quoted
  directly before BOM lock.
- **Real-world hand-assembly labor rates** (the $15–30/unit estimate in §3 for panel-mount-hardware
  assembly) were not sourced from an actual contract-manufacturer quote — this figure is a planning
  estimate based on general PCBA cost-model literature (JLCPCB's own stated fee structure) rather than
  a quote specific to a board of Brainscape's exact connector/switch/pot count.
- **Whether Electrosmith would offer any direct volume-pricing negotiation or partnership terms for a
  documented, larger open-hardware project like Brainscape** (as opposed to generic published web-store
  volume tiers) was not investigated in this pass — worth a direct outreach before assuming only the
  public carton-tier pricing in §1 is available.
