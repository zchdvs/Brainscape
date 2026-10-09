# CPU budget: the Seed's worst case and how it is bounded

> The fix for the CPU budget that the first Rev7 bench session found not met
> ([reviews/rev7-silicon-record.md](reviews/rev7-silicon-record.md) §3.3, "record §N"). It explains
> why [grain-engine.md](grain-engine.md) §8's estimate ("engine §N") was wrong, gives a cost
> model, and plans three layers: a bit-exact speed pack, a deterministic cost governor that is
> part of the sound, and the shared constants that keep the factory set clear of it. It extends
> engine §8, [determinism-profile.md](determinism-profile.md) ("profile §N") and
> [mode-compiler.md](mode-compiler.md) ("compiler §N"). It is built from the first bench
> session, five investigation lanes (bench data, hot path, births, constraints, bit-exact
> prototypes), three proposals (P1 bit-exact speed, P2 deterministic governor, P3 hybrid), two
> judgments of them, and one experiment made for this document (the governor re-run with the
> bit-exact FFT constants and a post-stage refund, §5, §6). The lanes' and proposals'
> prototypes, scripts and logs were scratch work and are not committed; each is described where
> it is cited, with its result. Code is cited as `path:line` at `main` `1624f80` (sound revision
> 3), before this change; "W1" cites wave 1 at `claude/mode-compiler-impl` `83db918` (sound
> revision 7, pull request #9, not merged). The session-1 captures are cited as the record cites
> them, `session-1/<file>:<line>` under
> [`firmware/records/rev7-2026-10-07/`](../../firmware/records/rev7-2026-10-07/README.md).
> Status: **owner-approved design** (2026-10-09): the owner confirmed the thirteen decisions of
> §9 as proposed. Steps 1 and 2 of §4, both bit-exact, are built at sound revision 3 (§4.1,
> §4.2); nothing else is.

**Evidence labels.**

- **Measured:** DWT cycles on the Seed in session 1, or bytes from a firmware build's map.
- **Host-run:** a prototype rendered on the host, and where stated also on the M7 under qemu.
  qemu checks bits, not cycles.
- **Model:** an estimate from disassembly plus an issue model, or from a fit. Treat it as ±10 %
  at best.

---

## Summary

The pedal misses deadlines in ordinary use, not only at extremes:

- The nominal 64-voice setting peaks at 99.1 %.
- The live image's "glitch" slot (`dense_1ms`) peaks at 118.6 %.
- The pessimistic family runs 135–169 %.
- One legal setting models at about 196 %.

There are three causes, and engine §8 counted none of them:

- **Per-voice arithmetic.** Each voice-sample costs 79–131 cycles. The design assumed about 20.
- **Per-birth re-rendering.** Every birth re-renders every live voice: up to 6,203 cycles per
  birth against an assumed ~530.
- **The onset-detector spike.** It costs about 98,500 cycles and lands in 3 of every 16 blocks. It
  is inside 115 of the 117 measured worst blocks.

The plan has three layers.

1. **A bit-exact speed pack.** It changes no output bit. The onset FFT rewrite and the post-chain
   hygiene are built (steps 1 and 2). The grain-loop, birth and bookkeeping rewrites land on top of
   wave 1. Everyday presets drop to roughly 20–70 % in their worst block, but the pessimistic
   family stays at about 90–110 %.
2. **A deterministic integer cost governor inside the engine, at sound revision 8.** It bounds the
   cost of any 48 consecutive frames to 85 % for every input, provided its constants are measured
   upper bounds. It is part of the sound, so the plugin plays exactly what the pedal plays.
3. **Shared constants.** The same constants drive a plugin "Seed load" meter and a compiler lint,
   so factory presets stay clear of the governor.

What the governor costs musically:

- **Below its caps, nothing.** Output is bit-identical for default, nominal, density-max at unity
  pitch, `pitch_reverse_spray`, and 28 of the 33 corpus presets.
- **At the corners, voices.** With conservative constants and no working PLD:
  - +24 st with every post stage: 52 → about 30 voices.
  - +12 st at density max: 63 → about 46.
  - `dense_1ms`: 34 → about 31.
- **With working PLD:** about 40, 56 and 32 respectively.
- Bench session 2 sets the final numbers.

---

## 1. The problem in numbers

All rows are engine code in ITCM, warm cache, as a percentage of the 480,000-cycle block budget
(`session-1/bench-all.md:25`).

The two "over" columns give the share of blocks:

- The first figure is interpolated from the bench's percentile ladder.
- The figure in brackets is the bench-data lane's per-block estimate from its model E (§3).
- The bench sends only percentiles, not per-block data (`firmware/bench/main.cpp:176-230`).

| Configuration | Mean | Worst block | Blocks over 85 % | Blocks over 100 % | Source (`session-1/`) |
|---|---|---|---|---|---|
| default preset | 19.0 % | 41.4 % | 0 | 0 | `bench-all.md:166` |
| corpus `tail_post_fb` | 31.7 % | 54.4 % | 0 | 0 | `:176` |
| corpus `pitch_reverse_spray` | 54.8 % | 84.6 % | 0 | 0 | `:178` |
| nominal (64 unity voices, 20 ms, post stages moderate) | 77.7 % | 99.1 % (cold 100.3 %) | 30 % (19 %) | 0 warm; 1 of 10,000 cold | `:168-169` |
| corpus `dense_1ms` (the live "glitch" slot, `firmware/live/LivePresets.h:33`) | 80.0 % | 118.6 % | 36 % (25 %) | 8 % (6.5 %) | `:182` |
| `pess_events` (sweeps every block, triggers, loads) | 89.2 % | 146.3 % | 51 % (54 %) | 21 % (18 %) | `:174` |
| `pess_render` (+24 st, every post stage, 20 ms) | 90.6 % | 135.5 % | 55 % (70 %) | 33 % (26 %) | `:170` |
| pessimistic, no onset marks | 99.2 % | 138.4 % | 59 % (95 %) | 45 % (39 %) | `:281` |
| noise tail, feedback 0.95, 122,000 blocks | 99.1 % | 143.1 % | 59 % (94 %) | 46 % (39 %) | `:323`, `bench-bench-ITCM.log:76` |
| `pess_births` (1 ms grains) | 119.6 % | 168.5 % | 85 % (100 %) | 69 % (98 %) | `:172` |
| births: 48 voices, 1 ms | 123.3 % | 146.5 % | 90 % (100 %) | 72 % (100 %) | `bench-bench-ITCM.log:71` |
| legal corner: 64 voices, 1.33 ms, jitter 0, +24 st, every post stage (*model*, never run) | ~177 % | ~196 % | – | – | the bench-data lane's worst-case probe |

What else the measurements show:

- **The worst block is the onset-hop block.** Recomputed from the `maxBlock` fields: a block is a
  hop block when it contains a frame with (abs + 1) mod 256 = 0 (`dsp/src/OnsetDetector.cpp:172`).
  The run maximum falls on a hop block in 37 of 37 ITCM runs, 35 of 37 XIP runs and 43 of 43
  hooks runs.
- **The hop costs about 98,500 cycles.** That is the p90 − p50 gap of steady configurations, for
  example 170,903 − 74,150 = 96,753 for the default preset (`bench-bench-ITCM.log:44`). It runs
  on 3 of every 16 blocks (`dsp/src/detail/OnsetDetector.h:30-31`), always, whatever the preset
  (`dsp/src/Engine.cpp:1063-1071`).
- **Maxima grow with run length, so they are not bounds.** The same pessimistic parameters peak
  at 135.5–138.4 % over 6,000 blocks and at 143.1 % over 122,000 (`bench-all.md:170`, `:281`,
  `:323`).
- **Engine code must stay in ITCM.** XIP is 1.05–1.21× slower warm and 1.26–1.70× cold
  (`bench-all.md:237-254`); cold XIP `pess_births` reaches 202.8 % (`:191`). In ITCM, cold caches
  add at most 1.6 points to the maxima (`:166-183`).
- **The bench ran sound revision 1** (`bench-all.md:7`). The lanes' host replicas reproduced every
  bench output hash. The render path is unchanged in revisions 2–3 apart from the mix law and
  routing.
- **What a late block does.** The SAI DMA replays the unfinished half-buffer, an audible
  discontinuity. Sustained overload loses whole blocks, after which the pedal's timeline
  diverges from the plugin's (the bench lane, from libDaisy's `sai.cpp` and the HAL DMA
  handler). The firmware only counts overruns (`firmware/live/main.cpp:115`).

**The target.** No document set a ceiling below 100 %:

- Engine §8 says "Worst case, 10,000 cycles/sample".
- Profile §7.2 estimated 77–78 % pessimistic, "about 22 % headroom".
- `bench_report.py`'s verdict compares against 480,000 cycles
  (`tools/hil/bench_report.py:403-417`).
- The live image counts blocks over 100 % (`firmware/live/main.cpp:114-115`).

85 % (408,000 cycles per 48 frames) is therefore the owner's choice (D1). The remaining 15 % must
cover:

- **the audio interrupt outside `Process`.** libDaisy's conversion loops run from QSPI. The bench
  lane estimates them at about 1–2 %; nothing measures them, because the live stats time only
  `Process` (`firmware/live/main.cpp:99-107`).
- **USB, MIDI and UI.** They already run below audio priority (`firmware/README.md`, "Platform
  layer": the USB interrupts sit below the audio DMA's priority).

Two unbuilt features are budgeted in engine §8 and must be charged when they arrive: per-voice
modifiers at 450–700 cycles/sample and the looper at 40–60.

---

## 2. Why the design's estimate was wrong

Engine §8 costed every stage with derived per-sample means. Profile §7.2 added +9–14 % for
determinism, reaching 77–78 %. Every line below was measured or traced to code.

| Item | Design | Measured or derived | Why |
|---|---|---|---|
| **Grain render** | ~4,000 c/smp pessimistic for 64 voices (engine §8). That came from 15–25 cycles per grain-sample, cache-hot, with "single-cycle FMA" ([grain-delay-theory.md](../research/grain-delay-theory.md) §5.4). | 6,549 c/smp (`session-1/bench-all.md:283`) for only **52.3** mean voices. Mark capping shortens up-pitched grains (`dsp/src/Granular.cpp:141-146`), per the hot-path lane's host replay. Per voice-sample: unity 79, linear 93, Hermite 131 cycles (*model*, calibrated on the bench ELF). | Several causes add up:<br>- The in-order M7 runs this loop at about 1 instruction per cycle, with 3-cycle dependent FP latency (`bench-all.md:56-62`) and contraction off.<br>- The envelope is evaluated every sample: leg tests plus a LUT morph (`Granular.cpp:48-59`, `detail/GrainMath.h:217-224`). That is about 55 cycles of each voice-sample.<br>- Five grain fields are reloaded every sample because the `float*` wet stores may alias the `Grain` (bench ELF `0x2dd0-0x2ed8`).<br>- L and R are interpolated separately (`Granular.cpp:251-252`, `:263-264`). |
| **Rate and cache term** | +2,400 at r = 4 ("conflict ×2") | Ring line fills are about 14 % of the grain stage (≈943 c/smp in `pess_render`). The bench's "−pitch" difference of 532 (`bench-all.md:282`) is confounded by voice-count changes. | Misses are not the dominant term; arithmetic is. |
| **Births** | ~530 c/smp at one birth per sample, about 530 cycles per birth | ≤ 6,203 cycles per birth at 48 voices and ≤ 3,846 at 16 (`bench-all.md:309-310`). This fits ≈ 2,730 + 74 × live voices. | - Every birth first renders all live voices up to the birth frame (`Granular.cpp:445-448` for the scheduler, `:331` for triggers; the code's own TODO at `:377-378`).<br>- A full 64-slot sweep runs on every attempt (`:426-435`), including refused ones, which retry every frame (`:462-467`).<br>- DetMath calls plus libgcc soft-float for `inc` (`:190`, `:205`). |
| **Onset detector** | 105–140 c/smp | About 98,500 cycles in each hop block, 20.8 % of that block. Averaged over all blocks it is about 390 c/smp. | The design costed the average, but the 512-point FFT lands in a single block. |
| **Post stages** | 1,190 c/smp in total | 1,338 (`bench-all.md:276-280`): delay 319 against ~60, filter 200 against 70, modulation 192 against 60, reverb 559 against 800. | The total was right; the parts were not. |
| **Peaks** | Not modelled; the table holds means only | Worst blocks run 1.3–1.5× the mean (`bench-all.md:166-183`) | Births and voices cluster, and the hop adds a fixed spike. |
| **Parameter space** | "Even the pessimistic case stays inside budget" | The legal corner models at about 196 % | 1.33 ms grains at overlap 1 sustain 64 voices and 48 births per block (`dsp/src/Engine.cpp:809-816`, `Granular.cpp:461`). Clouds' shortest grain is 32 ms (the constraints lane, from the upstream source). |
| **Contraction off** | +1–5 % (profile record) | Not separately measured; no FMA build exists (`bench-all.md:229-231`) | Explicit FMA recovers 80–90 % of a small loss (profile §7.3). It cannot close a 69-point overrun. |

The proposals contained one counting error, corrected here. The figure "6,549 / 63 ≈ 104 c per
voice-sample" assumed 64 voices. `pess_render` averages 52.3, so the per-voice cost including
fills, calls and births is about 125 cycles, and voice compute alone about 99 (the hot-path
lane's attribution).

---

## 3. The cost model

**Whole-block fit (model E).** The bench-data lane fitted it on 16 configurations, ITCM warm:

> cycles per block ≈ 55,081 + 3,557·V_unity + 4,562·V_interp + 4,524·births + 43.4·(segment
> visits) + 70.9·(ring-line misses)

- The fixed term is 36,331 per non-hop block plus 3/16 of a ~100,000-cycle hop.
- The RMS residual is 2.9 % of the budget; the worst leave-one-out residual is +15.1 %
  (`pess_births`).
- Block by block, the model's worst block is the Seed's worst block in every stress run (the
  lane's per-block validation).

**Unit costs, bottom-up.** These come from disassembling `RenderSpan` at `0x29e8` in session 1's
bench ELF (engine archive `4f4ddaa3…`, `session-1/bench-all.md:7`), and from the hot-path and
births lanes. "After" means after plan steps 1–7 (*model*).

| Unit | Today | After steps 1–7 | Notes |
|---|---|---|---|
| Voice-sample, unity | 79 (74–97 by envelope leg) | 39 | prototype A |
| Voice-sample, linear | 93 (79–111) | 53 | prototype A |
| Voice-sample, Hermite (slots 0–7, `Granular.cpp:200`) | 131 (117–149) | 96 | prototype A |
| Ring line fill | ≈50 cycles; ≤ r/8 lines per voice-sample (0.36 measured at r ≈ 4) | same | PLD might hide about 70 % |
| `RenderRun` call | 75–100 | 75–100 | calls per block go from V × (1 + births) to about V + births |
| Birth, fixed part | ≈2,730, including a 400–700-cycle sweep and ~500–650 of DetMath and libgcc | ≈2,000–2,300 (*estimate*) | |
| Birth, re-render | 74 per live voice | ≈0 | step 5 |
| Refused scheduler attempt | one sweep, ≈650–700 | O(1) | step 6 |
| Onset hop | ≈98,500 per hop | ≈38,000–51,000 (the two rewrites, *model*) | step 1 |
| Post stages | modulation 192, delay 319, reverb 559, filter 200, feedback-on 68 (`bench-all.md:276-280`) | −0.5 to −3 points (*estimate*) | step 2 |

The two birth fits agree. Model E's 4,524 + 43.4·V and the births lane's 2,730 + 74·V both give
about 6,200–6,600 cycles at 48 voices, against the bench's own bound of ≤ 6,203
(`bench-all.md:309`).

**Worst-block anatomy** (the bench-data lane's decomposition):

- `pess_render` block 1482 is 44.9 points above its run mean. Of that, the hop is +16.9, extra live
  voices +9.1 (61.8 against 52.3), extra births +4.7 (8 against 3.0), segment visits +3.1 and ring
  misses +2.9.
- `pess_events` block 8010 is +57.1 points, of which births are +12.1 (14 against 1.1).

So the cost of a window is: a fixed part, plus the hop when the window holds one, plus the post
stages, plus Σ over voices of frames × weight(path, rate, leg), plus births × per-birth cost,
plus calls, plus events. That sum is what the governor of §5 bounds.

---

## 4. The plan

| # | Step | When | Output | Estimated saving (points of budget unless stated) | Effort | Risk |
|---|---|---|---|---|---|---|
| 1 | Onset hop analysis rewrite | now, on `main`, at revision 3: **built** (§4.1) | bit-exact | −10 to −12.6 in every hop block (*model*); −1.9 to −2.4 on the mean | 1 day + CI | low |
| 2 | Post-chain hygiene | now: **built** (§4.2) | bit-exact | −0.5 to −3 with post stages on (*estimate*) | 2 days | low |
| 3 | Merge wave 1 at revision 7, as built | when ready | – | – | – | – |
| 4 | `RenderRun` restructure (prototype A) | after 3 | bit-exact | −19 to −23 on 64-voice presets; `pess_render` block 1482 −20.5 | 2–3 days (includes the wave-1 rebase) | low-medium |
| 5 | Batched schedule and render (prototype B) | after 3 | bit-exact | worst block: `pess_births` −44, `dense_1ms` −30, 48 voices at 1 ms −55, `pess_render` −12 | 3–4 days | medium |
| 6 | End-time heap and 64-bit free mask in place of the slot sweep | after 3 | bit-exact | −450 to −650 cycles per birth; default worst block −4.9 | 1–2 days | low |
| 7 | Birth trims | after 3 | bit-exact | −90 to −340 cycles per birth | 1–2 days | low |
| 8 | Shadow ledger, `Stats` fields, firmware event budget, bench-image updates | with 4–7 | bit-exact | 0; this is what calibrates the governor | 3–4 days | low |
| 9 | **Bench session 2** (§8) | owner and Seed | – | measures every constant | about 1 day | – |
| 10 | Tier 2/3 as session 2 warrants: PLD, integer interpolation coefficients, shared envelope tables, onset scratch in DTCM, post-delay staging, DetMath fast paths | after 9 | bit-exact | PLD up to −7 to −11 at r ≈ 4; the others −1 to −9 each | 0.5–10 days per item | low-medium |
| 11 | **Cost governor** (§5) | sound revision 8 | **sound revision** | every 48 frames ≤ 85 % by construction | 10–13 days, including the re-mint | medium |
| 12 | Tooling: `CostModel.h`, the plugin's "Seed load" meter, `bspc` L10/L11, criteria for the audition Load row | with 11 | bit-exact | 0 | 4–6 days | low |
| 13 | Bench session 3: final constants, soak test | owner and Seed | – | – | about half a day | – |

Steps 4–5 and 11 need ITCM that wave 1 leaves no room for (§7.3); placing the engine's cold code
out of ITCM comes first.

### 4.1 Step 1, the onset hop analysis (built)

**Two candidates.** Both restructure `AnalyzeHop` (`dsp/src/OnsetDetector.cpp:91-164`) without
changing a bit; both were host-run against the textbook form.

- **P1's v2.** The window, the bit reversal and FFT stages 1–2 fused (the input is real and the
  twiddles there are exactly (1, −0) and (−0, −1), `dsp/src/DetMath.cpp:71-77`, `:220-228`); the
  W⁰ and W^N/4 butterflies without multiplies; the other butterflies in pairs, per-stage
  templates with hoisted twiddles and `__restrict`; magnitudes four at a time, whitening two bins
  at a time, the flux summed in bin order. About −60k cycles per hop (*model*). Its comparison
  harness found 0 mismatches in 24,000 hops on GCC 11, Clang 14 (x86-64-v3) and the M7 under
  qemu, and its perturbed control 9,664 (a judge re-ran it); the golden corpus passed 33/33 on the
  host at 48-frame, random and 1-frame blocks and on the M7 at 48-frame and 1-frame blocks; all 19
  bench-configuration hashes were unchanged.
- **P3's.** The same fused first pass and multiply-free W⁰ and W^128 butterflies, the other
  butterflies with the twiddle hoisted out of the group loop, and stage 9 computed only for bins
  1–256, each straight into its magnitude. About −49k cycles per hop (*model*). Verified on
  500,000 host hops and 50,000 M7 hops (qemu), and the golden corpus 33/33 on host and M7.

`OnsetDetector.cpp/.h` are byte-identical on `main` and in wave 1, so this is the one step that
cannot conflict with it.

**D4 applied: P3's rewrite.** D4 takes P1's v2 unless the ITCM spare would fall below 8 KiB, and
P3's otherwise. Measured in the firmware builds of both trees (bytes of `.itcm_text`; the region is
65,536 bytes, of which `.itcm_text` may use 65,472 from its 64-byte offset; "spare" is what is
left of the region):

| Tree, change | `live` | `parity` | `bench` | `bench_hooks` |
|---|---|---|---|---|
| `main` (r3), as it was | 57,376 (spare 8,096) | 55,296 (10,176) | 55,152 (10,320) | 55,632 (9,840) |
| `main` + P1's v2 | 65,392 (spare 80) | 63,312 (2,160) | 63,168 (2,304) | 63,648 (1,824) |
| `main` + P3's prototype | 58,576 (6,896) | 56,496 (8,976) | 56,352 (9,120) | 56,832 (8,640) |
| `main` + step 1 as built | 57,936 (7,536) | 55,856 (9,616) | 55,712 (9,760) | 56,192 (9,280) |
| `main` + steps 1–2 as built | 57,864 (7,608) | 55,784 (9,688) | 55,640 (9,832) | 56,120 (9,352) |
| W1 (r7), as it is | 61,840 (3,632) | 59,760 (5,712) | 59,616 (5,856) | 60,080 (5,392) |
| W1 + P1's v2 | does not link: 69,856, 4,384 over the region | 67,776, 2,304 over | 67,632, 2,160 over | 68,096, 2,624 over |
| W1 + P3's prototype | 63,048 (2,424) | 60,968 (4,504) | 60,824 (4,648) | 61,288 (4,184) |
| W1 + step 1 as built | 62,400 (3,072) | 60,320 (5,152) | 60,176 (5,296) | 60,640 (4,832) |
| W1 + steps 1–2 as built | 62,264 (3,208) | 60,184 (5,288) | 60,040 (5,432) | 60,512 (4,960) |

P1's v2 grows `OnsetDetector.cpp`'s object from 1,792 to 9,804 bytes of code and constants, 8,012
more (the proposal's 6.3 KB counted from 3,476 bytes, not the object's 1,792); on wave 1's tree the live
image would need 69,920 bytes of a 65,536-byte region. Keeping 8 KiB spare with it would mean
moving 12,576 bytes out of ITCM. The engine archive is compiled without `-ffunction-sections`, so
the linker script (`firmware/linker/seed_h750.ld.in`, `firmware/CMakeLists.txt`'s `EXCLUDE_FILE`
list) places whole objects, and of the objects in ITCM only `SoundRevision.cpp` (233 bytes) never
runs in the audio path: `Validate.cpp` (8,612 bytes) and `blob/Mode.cpp` (1,468) run inside
`Process` at every Spillover load (`ResolvePreset`, `dsp/src/Engine.cpp:314`, `:972-978`), and
every other object runs in `Process` too. So code that never runs in the audio path cannot free
that much, and D4's switch applies: **P3's rewrite**, now.

**As built.** `dsp/src/OnsetDetector.cpp`'s `AnalyzeHop` is P3's form with two changes: the
prototype's run-time fallback to the old loop (taken if the twiddles were not exactly 1, −0, 0 and
−1) is gone, replaced by an assertion at `Init` and a unit test, since DetMath is exact at quarter
turns (`dsp/tests/test_detmath.cpp`); and the 7-bit bit-reversal table is a `constexpr` table in
the object (128 bytes of ITCM) rather than a member built at `Init`, so the engine object's size
does not change. That makes it 560 bytes of ITCM in the live image instead of the prototype's
1,200 (above). Every value that reaches a magnitude is made by the same IEEE operations on the same
operands as before; a skipped multiply by 1, −0, 0 or −1 changes only the sign of a zero, which a
square erases, for finite operands, which the engine's ±2¹⁶ clamp on the detector input
guarantees (`dsp/src/Engine.cpp:198`).

**Verification** (Windows, MSVC 19.40; GCC, Clang and the M7 under qemu run in CI):

- **The unit test** (`dsp/tests/test_onset.cpp`) keeps the old `AnalyzeHop` verbatim as its
  reference and runs it beside the detector on 24,576 hops (12 input kinds × 2,048: noise at
  levels from 2⁻⁴⁰ to 2¹⁵, sparse spikes among exact zeros, plucks over a tone, subnormal samples,
  DC, an impulse train, tones on the 24-bit grid, bursts at the clamp, signed zeros only, any
  finite float within the clamp, and all of them in turn). Compared each hop, bit for bit: the
  flux, the 256 magnitudes, the peak memory and both previous-hop states. **0 mismatches.** Two
  perturbed references, each one rounding away from the old form, mismatch on 4,600 (a twiddle one
  ULP off) and 3,065 (the flux summed in reverse) of 6,144 hops, so the comparison sees such a
  difference; and the engine itself perturbed as P1's control was (2⁻⁶⁰ added to one product of
  the general butterfly) fails the test on 8,974 hops. It runs in `dsp_unit` on every host leg;
  the M7 is covered by the golden corpus under qemu (`parity-m7`).
- **Golden corpus, sound revision 3, 33/33** on MSVC x64 SSE2 Release, `/arch:AVX2` Release and
  Debug: at 48-frame, 512-frame and 1-frame blocks, the patterns 48,1,127,32 and 300,512,5,64,
  random blocks (seeds 1, 2), from the hostile caller environment, and (Release) split delivery at
  random blocks and a fresh engine per render.
- **Golden corpus, sound revision 7, 45/45** with the same change applied to wave 1's tree, on
  MSVC SSE2 Release in the same ten configurations, and its `ctest` (22 of 22).

### 4.2 Step 2, post-chain hygiene (built)

Each of the three is bit-exact by construction: the same operations in the same order on the
same values. `PostChain.cpp` is identical on `main` and in wave 1.

- **Settled-mix square roots hoisted.** A settled smoother (value equal to target) returns its
  target exactly (`dsp/src/detail/Smoother.h:23-27`: the step is a zero, so `next == value` and
  the value snaps to the target), so the delay's and the reverb's equal-power gains, four `SqrtF`
  per frame (`dsp/src/PostChain.cpp:344-345`, `:380-381`; `vsqrt` is 14 cycles,
  `session-1/bench-all.md:66`), are taken once per block while the mix is settled. A settled mix
  of 0 never reaches the loop (the stage breaks out first), so the per-sample gate cannot close in
  it, and the smoother's state is what `Next` would have left.
- **`__restrict` on the wet buffers.** The engine's wet buffers (`dsp/src/Engine.cpp:574-575`, the
  Hot arena) overlap neither each other nor any line or state of the chain, so a store through
  them need not reload the stages' state. `__restrict` changes which loads the compiler may keep,
  never an operation.
- **The post delay's lines interleaved.** The two `DelaySlice`s, two Bulk-arena slices 2 s apart
  (`PostChain.cpp:179-180`), become one `StereoDelaySlice` over the same `2 × len` floats (left at
  2i, right at 2i + 1), so a frame's two samples share a cache line and one index computation.
  The two lines always moved in lockstep (one write each per frame, one `Clear` each), so it holds
  the same values; `TapGlide`'s Catmull-Rom read runs the same arithmetic on each channel.
  `BulkFloats` is unchanged, so the arena plan is too (`firmware_arena_plan` and the host
  memory-plan test pass); the `PostChain` object is 12 bytes smaller on the M7.

The saving is unmeasured until bench session 2's variant; the live image's ITCM fell by 72 bytes
(136 on wave 1's tree). Verified with step 1, by the same golden runs.

### 4.3 Steps 4–8

**4. `RenderRun` restructure (prototype A).** The hot-path lane's prototype A:

- Envelope legs are bounded once per run.
- The envelope is computed in a separate four-lane pass into a 64-float DTCM buffer.
- Read-and-accumulate is unrolled ×2 on `__restrict` locals. The wet buffers are disjoint
  (`dsp/src/Engine.cpp:574-575`).

It is verified together with step 5: golden 33/33 on host and M7 with 48-frame, random 1–512 and
1-frame blocks, and all 19 bench hashes on x86 and M7 (re-diffed against the base). It adds 3.75 KB
of ITCM (`Granular.o` grows from 8,444 to 12,196 bytes). It must be rebased onto wave 1's pass
splits and decay gain (W1 `dsp/src/Granular.cpp:216`, `:323-357`).

**5. Batched schedule and render (prototype B).** The hot-path lane's prototype B, on top of A:

- A birth no longer flushes. A newborn whose slot still holds unrendered frames goes into one of 64
  pending records, and each block renders once.
- Output is bit-exact: liveness depends only on `endAbs`, and the per-sample summation order is
  still birth order.
- The verification covered 115 steals and 2,942 fallback flushes.

B is chosen over the births lane's lazy prefix flush (also bit-exact; D5). With B, the per-birth
cost is a constant the governor can charge. The lazy flush renders an O(V) birth-order prefix
whenever grain lengths vary (mark capping at `Granular.cpp:141-146`, or size sweeps).

Under the governor's one-birth-per-frame rule (§5), a 48-frame pedal block holds at most 48
pending births, so the pedal never takes the fallback flush. The plugin's 512-frame chunks might,
which costs it nothing that matters and changes no bits.

Costs and integration:

- It adds 5.2 KB of DTCM. Wave 1's engine object is already 8,192 bytes (W1
  `dsp/include/brainscape/Engine.h:36`) in a 9 KiB slot (W1 `firmware/platform/Placement.h:27`),
  so `kEngineImplBytes` and `kEngineSlotBytes` must grow. The DTCM budget for the Hot arena plus the
  slot is 80 KiB, of which 33 KiB is used (W1 `Placement.h:29-31`).
- Wave 1's `FireExternal` reads `orderCount_` after a full flush as "voices sounding" (W1
  `Granular.cpp:379-394`). It must count voices with `endAbs > birthAbs` instead.

**6. Slot bookkeeping.** The per-attempt 64-slot sweep (`Granular.cpp:426-435`), which repeats on
every refused retry (`:462-467`), is replaced with:

- an end-time min-heap of at most 68 entries;
- a 64-bit free mask. Count-trailing-zeros keeps "lowest free index", so slot choice and output are
  unchanged.

The births lane's simpler sweep cache is already golden-verified. STATUS already lists this item
(Known gaps, the M7 budget pass). The governor reuses the heap to keep its load sum exact.

**7. Birth trims.**

- Build `inc` from the float's mantissa and exponent: ratio·2³² is an exact integer, and this removes
  `__aeabi_d2lz` and `__aeabi_l2d` (`Granular.cpp:190`).
- Skip `SinCosD` when `panSpread == 0` (`:205`), because its result is overwritten at `:209-212`.
- Cache the ratio per pitch entry when spread is 0, and the `EnvSpec` when a grain is not
  mark-capped.

These are bit-exact by construction, gated by the goldens, and not prototyped.

**8. Instrumentation and the event contract.**

- **Shadow ledger:** the governor's accounting without its admission rules, so it changes no bits.
  It logs a predicted cost per block next to DWT and extends wave 1's `GrainStats` (W1
  `dsp/include/brainscape/Engine.h:278-287`).
- **Firmware event budget** (D12). Events are already stamped at block boundaries
  (`firmware/live/main.cpp:128-132`), so on the pedal they never split a block
  (`dsp/src/Engine.cpp:918-937`). Add:
  - coalescing of same-frame `SetParam` and `MacroMove` events with the same id;
  - at most one load per block;
  - at most N events per block;
  - a trigger rate cap.
- **Bench image:** per-block logging, `SuiteMicro` additions and the new configurations (§8).

### 4.4 What steps 1–7 achieve without a governor

Two models: P1's Tier 1 (its worst-case model and benched-block table) and P3's pack-only probe
(its "gov off" rows). They disagree by up to 14 points on birth-heavy blocks; session 2 settles
it.

| Worst block | Today | After steps 1–7 |
|---|---|---|
| default | 41.4 % | 19–30 % |
| nominal | 99.1 % | 57–69 % |
| `dense_1ms` | 118.6 % | 60–71 % |
| 48 voices, 1 ms | 146.5 % | about 80 % |
| `pess_events` | 146.3 % | 92–94 % |
| `pess_render` | 135.5 % | 90–94 % |
| noise tail | 143.1 % | 93–94 % |
| `pess_births` | 168.5 % | 96–110 % |
| legal corner, scheduler births only | ~196 % (*model*) | 105–115 % |
| legal corner with a trigger flood | – | about 129 % |

Steps 1–7 fix every everyday and corpus preset. They do not fix the pessimistic family or the
legal corner. That is why step 11 exists.

---

## 5. The guaranteed bound, and how it is enforced

The governor combines four sources:

- **P2's mechanics.** They are the only ones with a correct proof and a prototype that implements
  its specification.
- **P3's accounting.** The post stages are refunded per frame and the hop is a static reserve.
- **The judges' corrections.**
- **One rule from the births lane:** at most one birth per frame across all sources.

### 5.1 Specification

**Where it lives.** All state is in `GranularCore`, in DTCM:

- `bank` and `load` (int32);
- a `uint16` weight per grain;
- the step-6 heap and free mask;
- pending-trigger counters per source, capped at 8;
- 4 fade records.

**Constants.** These are sound-defining shared constants in a new
`dsp/include/brainscape/CostModel.h`:

- R, Cmax and c_b;
- H: 64, or 200 when onset or burst sources are on;
- the weights w_U, w_H[rc] and w_L[rc], where the rate class rc is set by |inc| ≤ 2³², ≤ 2³³ or
  larger;
- the post-stage charges K_mod, K_delay, K_reverb and K_filter, and a settle window K_settle;
- the fade length, 128 frames (`kGranularFastCutFrames`, `dsp/src/detail/Granular.h:24`), and the
  pending cap, 8;
- R0 = R + ΣK_s.

**At every absolute frame f:**

1. Expire grains with `endAbs ≤ f`, and subtract their weights from `load`.
2. Refill the bank: `bank ← min(Cmax, bank + R0 − P(f) − load)`.
   - P(f) = Σ_s K_s · [stage s's mix target ≠ 0, or f is within K_settle frames of the absolute
     frame at which that target became 0].
   - Targets change only at events, which sit at absolute frames, so P(f) does not depend on block
     splitting.
   - It mirrors the post chain's own per-sample gate, `value != 0 || target != 0`
     (`dsp/src/PostChain.cpp:300`, `:308`, `:340`, `:379`, `:426`, `:432`).
   - K_settle must be at least the mix smoother's worst-case time to reach exactly 0.
3. Allow at most one birth across all sources, in this order: manual or MIDI, onset, burst,
   scheduler.
   - **Trigger births** (`TryTrigger`) need load + w_adm ≤ R and bank ≥ c_b + w_adm. Otherwise they
     stay pending, and the oldest non-fading voices get FastCut's 128-frame fade
     (`Granular.cpp:311-324`) until there is room. The fade arithmetic is exact
     (`Granular.cpp:217-226`).
   - **Scheduler births** additionally need load + w_adm ≤ R − H and no trigger pending. A refusal
     takes the existing retry-next-frame path (`:462-467`) and draws nothing.
   - Every birth pays c_b + w.
4. **Static voice cap.** V_eff(params) is the largest V ≤ 64 (and ≤ `voice_count` in wave 1) such
   that:
   - W(V) ≤ R − H, and
   - W(V)·life + V·c_b ≤ (R0 − P_params)·life.

   `targetVoices = min(target, V_eff)`, so both the dithered ceiling (`Granular.cpp:441-443`) and
   the normalization (`dsp/src/Engine.cpp:862`) follow it.

**Random draws.** The governor draws nothing. Every draw is keyed by (frame − epoch, purpose)
(`Granular.cpp:98`, `detail/GrainMath.h:26-27`), so a deferred grain takes the draws of the frame it
is born at. Wave 1's pitch cycle advances only on admitted births.

### 5.2 Proof of the bound

Three invariants hold:

- load ≤ R at every frame, enforced by admission plus fades;
- R ≤ R0 − max P;
- bank ≥ 0.

Then load_f + P(f) + charges_f ≤ bank_{f−1} − bank_f + R0. Summing over any 48 consecutive frames,
at any alignment:

> grain cost + post-stage cost over any 48 frames ≤ 48·R0 + Cmax,
> so the engine's cost per 48 frames ≤ S + 48·R0 + Cmax = 408,000 cycles = 85 %.

Here S is a static reserve that covers the fixed work, the hop in every window, spikes, events and
per-voice call overhead. Because the hop is reserved statically, nothing in the governor follows
the 187.5 Hz hop cadence. This removes P1's density-pattern risk.

The governor's own M7 cost is estimated at about 10 cycles per frame plus 50–80 per birth, end or
fade (P2), and is carried in S.

### 5.3 Constants (worked from measurements; final values come from session 2)

| Term | Cycles per 48 frames | Basis |
|---|---|---|
| Measured non-grain worst block (1 voice, every post stage, hop) | 227,040 | `session-1/bench-all.md:283` (4,730 c/smp); `bench-bench-ITCM.log:70` |
| − step 1's hop saving | −49,000 to −60,000 | *model* (−49,000 for the rewrite built, §4.1) |
| − post stages, now charged through P(f) | −60,960 | 1,270 c/smp, `bench-all.md:276-279` |
| + event reserve | +24,000 | placeholder; session 2 measures it, multiplied by the producer cap |
| + per-voice calls and governor overhead | +7,000 | P2's estimate |
| **= S** | **≈137,000–148,000** | |
| **48·R0 + Cmax** | **≈260,000–271,000** | 408,000 − S |

The resulting R values:

- Cmax 7,500 and c_b 2,500 give R ≈ 3,990–4,220.
- Cmax 13,500 and c_b 4,500 give R ≈ 3,860–4,090.
- The weights in the host runs below are P2's estimates for the post-speed-pack code: worst leg,
  +15 %, every ring line missing. That is unity 54; Hermite 119/128/145 and linear 70/79/96 by
  rate class. With PLD working they are 48; 113/115/121 and 64/66/72.

With step 1 built as P3's rewrite (−49k, *model*), S sits at the top of its range until session 2
times the hop.

### 5.4 Preconditions, which session 2 must establish

1. Every weight, c_b, K_s and the overhead term, and each part of S, is a measured M7 upper bound:
   ITCM, warm and cold, ring lines forced cold, worst envelope leg (D7).
2. The firmware producer caps event work per block within the reserve (D12).
3. Engine code stays in ITCM.
4. Wave 3's per-voice modifiers and the looper enter as new weights or stages when they land.

### 5.5 Enforcement

- The governor itself, in the engine, on every platform.
- In the bench images, the shadow ledger: DWT cycles must stay at or below the ledger in every
  block.
- In the live image, `g_statOver` (`firmware/live/main.cpp:115`), kept as a backstop counter.
- In CI, split-invariance and M7-parity tests of the governed goldens.

### 5.6 Verified so far (host-run)

**P2's prototype:**

- With the governor off, or on with an unbounded budget, 33/33 goldens match and all 19 bench
  hashes match.
- Governed goldens are identical under random 1–512-frame, 1-frame and 512-frame blocks and under
  split delivery.
- The governed engine matches 33/33 on the M7 under qemu (its S2 constants).
- The per-window charge stayed within the bound in 27 configurations, including trigger floods.
  That checks the accounting identity, not M7 cycles.

**This document's refund variant** (P2's prototype with the post-stage refund):

- With the refund off, it reproduces P2's S2 output bit for bit.
- With the refund on, using the F2r constants, its minted governed goldens hold under random
  1–512-frame, 512-frame and split delivery (33/33) and under 1-frame blocks (29/29, the quick
  set).
- M7 parity of the refund variant has not been run.

**Not built yet:**

- the heap (the prototype scans 64 slots per frame);
- the pending cap (P2's prototype leaves `pendingManual_` uncapped);
- the one-birth-per-frame rule;
- the post-stage settle window (the refund probe derives the refund from parameters at rebuild);
- the MSVC and Clang legs.

---

## 6. Musical impact

**Steps 1–10 have none.** Every output bit is identical, and on-device hash checks gate it.

**The governor (step 11).** The table gives mean sounding voices from host runs of the bench
configurations on the revision-3 engine, plus one calculated row. The columns:

- **A:** F1r. Conservative weights, c_b 4,500, Cmax 13,500, R 4,093, refund on.
- **B:** F2r. The same weights, c_b 2,500, Cmax 7,500, R 4,218.
- **C:** F5r. B with the weights that apply if PLD works.
- **D:** P3's probe with mean weights. This is **not a valid bound** and is shown only as the
  optimistic end.

The final values will lie between B and D, depending on how close the measured upper bounds come
to the means. "=" means bit-identical output.

| Configuration | Ungoverned | A | B | C | D |
|---|---|---|---|---|---|
| default preset | 10.4 | = | = | = | = |
| nominal (64 unity voices, 20 ms, post moderate) | 62.8 | = | = | = | = |
| density max at unity (64 voices) | 62.7 | = | = | = | = |
| `pitch_reverse_spray` | 35.9 | = | = | = | = |
| density max, +12 st | 62.7 | 44.9 | 45.9 | 55.7 | 56.5 |
| Murmuration at Activity max (calculated from the V_eff rule: overlap 0.95, spread 25 cents, reverse; [reviews/mode-compiler-record.md](reviews/mode-compiler-record.md) §2.2 at W1; two fewer voices if onset triggering is on) | ≈55 | 46 | 47 | = | = |
| `dense_1ms` (the live "glitch" slot) | 34.4 | 24.6 | 31.4 | 32.3 | = |
| 48 voices, 1 ms, no post stages | 47.0 | 27.4 | 37.2 | 42.1 | = |
| pessimistic at r = 1, every post stage | 58.7 | 44.3 | 46.5 | 51.0 | = |
| `pess_events` | 59.4 | 40.3 | 41.7 | 50.2 | 56.0 |
| `pess_render` (+24 st, every post stage) | 52.2 | 28.9 | 29.7 | 40.2 | 43.3 |
| noise tail (+24 st, every post stage, fb 0.95) | 58.8 | 32.3 | 33.1 | 45.4 | 46.6\* |
| `pess_births` (+24 st, every post stage, 1 ms) | 34.4 | 15.8 | 20.8 | 24.7 | 31.3 |
| legal corner: 64 voices, 1.33 ms, +24 st, every post stage | 63.0 | 21.7 | 27.6 | 33.5 | 36.0 |

Sources: the refund probe's runs of the bench configurations under F1r, F2r and F5r, and P3's
pack-only probe (\* its no-marks pessimistic run).

**Corpus.**

- In A and B, 28 of 33 corpus presets stay bit-identical. Five change: `freeze_retoggle_spill` from
  9 s, `dense_1ms` from 0 s, `automation_offgrid` from 6 s, `spillover_chain` from 5 s,
  `exact_load_mid` from 7 s.
- In C, 30 of 33 stay identical.
- Without the post refund, `pitch_reverse_spray` also changes (in the refund probe's F1 run and
  P2's S2 run). The refund is what keeps it.
- Wave 1's own corpus has not been checked yet.

**What the listener hears when the governor binds.**

- **Static cap.** There are fewer voices at the same loudness, because normalization follows
  V_eff (`dsp/src/Engine.cpp:862`). The texture thins rather than drops in level.
- **Bank refusals** (birth-heavy or jittered streams). The next birth slips by a few frames, so
  Poisson clusters are slightly regularized. The sounding count dips below V_eff and the level
  dips with it. A refused birth shifts every later birth of that stream, which is why a governed
  golden differs from its first binding second onward.
- **Triggers.**
  - A trigger fires on its frame when there is room.
  - Otherwise it waits for one 128-frame fade plus the bank refill. The latency distribution has
    not been logged yet; session 2 logs it.
  - At full load the oldest voice fades over 128 frames (2.7 ms) instead of today's hard cut
    (`Granular.cpp:341-348`), which removes a click that exists today.
  - Beyond 8 pending per source, the oldest pending trigger is dropped and counted. This amends
    "explicit triggers never drop" (engine §4) for floods only (D6c).
- **One birth per frame.** Today, same-frame births share every draw key (`Granular.cpp:98`,
  contradicting `detail/GrainMath.h:26-27`; still the case in wave 1, W1 `Granular.cpp:102-114`), so
  they stack identical grains. Deferring the second by one frame makes them distinct.

**For scale.** Clouds caps grains at 40 mono or 32 stereo, with grains of 32 ms or longer (the
constraints lane, from the upstream source). Even column A keeps 64 unity voices at nominal
settings and about 29 voices at +24 st with every post stage.

**Levers that recover density, in order of cost:**

1. PLD, if it does not block the M7 pipeline (column C).
2. Tier-2 bit-exact items, which lower the weights.
3. Measured weights instead of P2's estimates: the gap between columns B and D.
4. The output FIFO, D9: +1 ms latency, no output change, and about +24 % grain budget (P3's
   estimate).
5. Staging the onset analysis into the next hop: a sound revision. Onset decisions arrive about
   5.3 ms later, and the hop reserve falls to about 25k cycles.
6. Hermite demotion under load: about +4 voices, a sound revision.
7. Coupling the voice cap to the enabled post stages. Enabling a stage would then need a
   fade-steal.

---

## 7. Fit with wave 1 and the factory set

### 7.1 Order of work

- Steps 1–2 land now (D3). `OnsetDetector.cpp/.h` and `PostChain.cpp` are byte-identical on `main`
  and in wave 1.
- Wave 1 then merges at revision 7.
- Steps 4–8 rebase onto it. Wave 1 changes `Granular.cpp`, `Engine.cpp` and `Granular.h` by 416
  insertions and 81 deletions.
- The governor is sound revision 8. Bumps are cheap before publication (profile §5.12), and every
  sound change must land before revision 1 is published (profile §8.4).

### 7.2 What wave 1 adds, and how the plan handles it

- **Bursts.** 1–16 grains, spacing down to one frame, at most 8 bursts in flight (W1
  `dsp/src/detail/Granular.h:204`), and no burst grain on a frame where a trigger fired (W1
  `Granular.cpp:535-537`). About 48 trigger births per block become reachable, and each one flushes
  today (W1 `Granular.cpp:381`).
  - Step 5 removes the flush.
  - The governor routes burst grains through `TryTrigger`.
- **Repeat.** Each pass boundary splits a voice's run and calls `DecayGain`, an `Exp2F` (W1
  `Granular.cpp:351-357`). The governor adds a per-pass term to the voice's weight:
  ⌈c_pass·⌈48/passLen⌉/48⌉.
- **`voice_count`** (1–64, W1 `dsp/include/brainscape/Params.h:216`). It folds into V_eff and into
  trigger steals. The `orderCount_` check (W1 `Granular.cpp:387`) changes with step 5.
- **Pitch sets.** One `Exp2F` per birth; the step-7 cache covers entries with spread 0.
- **Intermittency.** For triggers it is drawn before admission and kept while the trigger waits.
  For the scheduler it is drawn after admission (P2).
- **DTCM.** The engine object is 8,192 bytes in a 9 KiB slot. Step 5 (+5.2 KB) and the fade records
  (+0.46 KB) need a larger slot. There is room: 33 of the 80 KiB DTCM budget is used (D13).

### 7.3 ITCM

Measured from the firmware builds' maps (bytes of `.itcm_text`, §4.1's table): wave 1 leaves the
live image 3,632 bytes of ITCM, and 3,208 with steps 1–2. Step 4 adds about 3.75 KB (prototype A),
the governor's prototype about 2.3 KB, and step 5 an amount not yet measured; G6 (§8) asks for 8 KB
spare. So **steps 4, 5 and 11 cannot land in ITCM until cold code leaves it**, and D4's tally
(the spare after wave 1, steps 4–5 and the governor) is below 8 KB even with P3's rewrite; it stays
P3's.

What can leave, by what it costs:

- **The engine's main-thread-only API**: `Init`, the Exact-load and `Restart` clears, `GetParam`,
  `ModeSwitches` and the other accessors the console reads. Placed by object today, it can leave
  only once the engine is placed by function (`-ffunction-sections` and a cold section, which
  moves code without changing what the audio path computes; the goldens check it) or once that
  code moves to a translation unit of its own; `firmware/README.md` ("Sizes") already names the
  option. The CLOCK design's entry gate needs the same room.
- **The package validator** (`Validate.cpp`, 8,612 bytes in the live image; `blob/Mode.cpp`,
  1,468). It runs at every Spillover load inside `Process`, so from QSPI it becomes a cold fetch
  in the load's block, where `pess_events`' worst block is already 146.3 %; session 2 measures that
  block before it moves.
- **String constants** (`.rodata.str*`, about 1.8 KB of `Engine.cpp`'s and `SoundRevision.cpp`'s),
  by section name, once it is shown which of them the audio path never reads.

This is a prerequisite of step 4, to be sized in the build that lands it, and checked against G6 on
the merged tree.

### 7.4 The factory set

- **One function.** The same V_eff/`CostModel` function serves the engine, `bspc` and the plugin,
  so what the editor shows is what the pedal plays.
  - **L10** flags governor-limited density at a stored position or macro corner.
  - **L11** flags birth-limited short grains.
- **Audition gate** (D11). The pre-screen's Load row (compiler §11.3) today only logs voices and
  births per second. It gains pass criteria:
  - zero governor engagements at S0–S10;
  - ledger peak ≤ 70 % at S0;
  - S11 corners listed.
- **Authoring continues now,** guided by the shadow meter. The listening pass is best run at
  revision 8. Rows whose render hashes hold across the bump carry forward without a re-listen
  (compiler §11.3).
- **Recipes against column B:**
  - The default Activity range (overlap 0.25–0.85, a target of at most 39.3 voices; compiler §3.2)
    is capped only at its extreme of +24 st with every post stage. There the cap is 39, or 37 with
    onset triggering on (H = 200).
  - Murmuration at Activity 0.95 (about 55 voices) caps at 47 in column B and is uncapped in
    column C.
  - The live "glitch" slot thins from 34 to 31 voices.

---

## 8. The next Seed bench session (session 2)

### 8.1 Images

All images are ITCM builds of wave 1 plus the plan:

1. **bench-r7:** wave 1 as merged, with steps 1–2. This is the new baseline; session 1 measured
   revision 1.
2. **bench-r7-fast:** steps 4–8, with the shadow ledger and per-block logging. Every
   per-configuration SHA-256 must equal image 1's: a free on-device bit-exactness check.
3. **bench-r7-fast-pld:** image 2 plus PLD. Hashes must equal image 1's.
4. **bench-r8-gov:** the governor with provisional constants. Hashes must equal the host and qemu
   renders.
5. **The live image** on image 2's engine. A FIFO variant is built only if the owner wants to
   evaluate D9.

### 8.2 Configurations

- **Session 1's whole suite, warm and cold:** default, nominal, `pess_render`, `pess_births`,
  `pess_events`, the four corpus presets, the stage ablations, the births suite, the 122 s noise
  tail and the 123 s golden tail.
- **Corners:**
  - 64 voices × 1.33 ms × jitter 0 × +24 st × every post stage, with and without marks;
  - the same with a manual trigger on every frame;
  - 64 voices × 5 ms × jitter 0 × +24 st;
  - default at overlap 1, at +12 st and at +24 st;
  - Murmuration at Activity max.
- **Wave 1 stressors:**
  - burst 16 at spacing 0, triggered every 50 ms;
  - repeat 16 with 16-frame mark-capped passes and decay on, at 64 voices;
  - an 8-entry pitch set with spread;
  - a MIDI flood of 48 per block for 2 s;
  - `voice_count` 32 with triggers.
- **Fuzz:** 50 seeded random parameter-and-event sets × 2,000 blocks.
- **`SuiteMicro`** (DWT per function):
  - `RenderRun` per voice-frame, by path × rate class × envelope leg × direction, with ring lines
    forced cold;
  - the worst birth (Hermite slot, mark path at r > 1, jitter, reverse, pan, a steal) at 0, 32 and
    64 voices;
  - one `RenderRun` call; heap push and pop; the governor's per-frame step;
  - `AnalyzeHop`: the old form, P1's v2 and P3's as built (D4);
  - each post stage alone, including a moving `TapGlide`, with the mix settled and moving;
  - each event type: `SetParam`, `MacroMove` with 8 curved targets (about 538 cycles each,
    *estimated*, compiler §3.3), `SpilloverLoad` with FastCut and with Trails, `Trigger`;
  - a repeat pass boundary;
  - `Exp2F`, `SinCosD`, `LogF` and the int64 conversion pair;
  - one 64-slot sweep;
  - a PLD micro-test: walk SDRAM lines with no PLD and with PLD 1, 2 and 4 lines ahead, and check
    whether a demand miss waits on an outstanding PLD.
- **Per-block logging:** DWT next to the shadow ledger's prediction for the stress configurations.
  Today the bench sends only percentiles (`firmware/bench/main.cpp:176-230`).

### 8.3 Pass criteria

- **G1, determinism:** images 2 and 3 equal image 1 for every configuration, and image 4 equals the
  host and qemu renders.
- **G2, speed pack** (image 2):
  - hop spike (p90 − p50 on steady configurations) ≤ 60,000 cycles;
  - default worst block ≤ 30 %;
  - nominal worst block ≤ 75 %;
  - `dense_1ms` worst block ≤ 75 %;
  - `pess_births` mean ≤ 85 %.
- **G3, conformance:**
  - every measured unit is at or below its provisional constant;
  - the shadow ledger is at or above DWT in 100 % of blocks of every configuration, fuzz included;
  - the ledger is ≤ 1.3 × DWT at p50. A looser ledger wastes voices.
- **G4, bound** (image 4): maximum ≤ 85 % warm and ≤ 88 % cold on every configuration, including the
  floods, the wave 1 stressors and the 122,000-block tail, with zero blocks over 100 %.
- **G5, PLD:** keep it only if the fill cost per voice-frame at r = 4 falls by at least 50 %.
- **G6, memory:** at least 8 KB of ITCM spare, and the engine object fits its slot.
- **G7, live:**
  - one hour of the heaviest factory preset with USB serial and MIDI traffic gives
    `g_statOver` = 0;
  - measure the interrupt time outside `Process`, to confirm that 15 % headroom is enough.

After the session:

1. Set each constant to the measured maximum plus a margin (+10 % suggested).
2. Re-mint on the host.
3. Session 3 (about half a day) repeats G4 and G7 with the final constants.

---

## 9. Owner decisions

The owner confirmed every answer below on 2026-10-09, as proposed and without amendment. Each
stays reversible before the first public release; reversing one is an owner decision of its own
and, where it changes the sound, a sound revision.

| # | Decision | Answer (confirmed by the owner, 2026-10-09) | Consequence |
|---|---|---|---|
| D1 | Engine ceiling of 85 % (408,000 cycles per 48 frames) for any input | **Yes.** Session 2 confirms (G7) that the interrupt and USB fit in the remaining 15 %. | The governor's S + 48·R0 + Cmax is sized to 408,000 (§5.3) |
| D2 | A deterministic cost governor as part of the sound, sound revision 8, after wave 1 | **Yes.** No bit-exact path bounds the legal corner: 105–115 % after steps 1–7, about 129 % with a trigger flood. | Step 11; the plugin plays the governed sound too |
| D3 | Order: FFT rewrite and post hygiene now; speed pack after wave 1 merges; governor last | **Yes.** Only steps 1–2 avoid conflicting with wave 1. | Steps 1–2 built at revision 3 (§4.1, §4.2) |
| D4 | Which onset rewrite | **P1's v2 now** (−60k cycles per hop, *model*; +6.3 KB ITCM). Time both rewrites in session 2. Switch to P3's 1 KB rewrite if ITCM spare after wave 1, steps 4–5 and the governor falls below 8 KB. | Measured: P1's v2 adds 8,016 bytes and does not link on wave 1's tree, so **P3's rewrite is built** (§4.1); session 2 times both |
| D5 | Birth-render fix: batched B or the lazy prefix flush | **B.** The per-birth cost is constant, so the governor can charge it; it costs 5.2 KB of DTCM. | Step 5 |
| D6 | Governor semantics: (a) 128-frame fade-steal instead of the hard cut; (b) normalization follows V_eff; (c) pending cap of 8 per source, dropping the oldest and counting it, which amends "never drop" for floods; (d) one birth per frame across sources; (e) post-stage refund for births, with a static voice cap | **Yes to all five.** | §5.1, §6 |
| D7 | Weights are measured worst-case upper bounds, not means | **Yes.** The guarantee rests on it. Density is recovered with PLD and Tier 2, not with optimistic weights. | §5.4 |
| D8 | When to freeze the constants | **Once, after session 2, at revision 8.** Later speedups may raise them in a further revision before revision 1 is published. | §8 |
| D9 | Output FIFO slack: +1 ms latency (2 → 3 blocks), no output change, about +24 % grain budget (*estimate*) | **Decide after session 2.** Take it if, with the final constants, the governor still caps density max at +12 st below about 56 voices, or caps any factory recipe inside its macro ranges. | Open until session 2 |
| D10 | Sound-changing reserve levers: staged onset analysis, explicit FMA, Hermite demotion, coupling the voice cap to post stages | **Not now.** Explicit FMA gains little on the M7 and costs the x86 plugin (profile §7.3). The others stay in reserve. | §6 |
| D11 | Factory gate | **Zero governor engagements at stored positions and S0–S10, with S11 corners reported.** Enforced by `bspc` L10 and the audition Load row. | §7.4 |
| D12 | Firmware event contract: coalescing, at most 1 load per block, at most N events per block, a trigger rate cap | **Yes.** N is set from session 2's per-event costs, so that N × the worst event cost fits the reserve. | Step 8 |
| D13 | DTCM priorities | **Now:** B, the fade records and the heap (about 6 KB). Onset scratch (13 KB) and envelope tables (8–16 KB) only if session 2 shows they pay off. Keep at least 16 KB for wave 3's per-voice state. | §7.2 |

---

## Appendix A. Corrections to the proposals

**P1 (bit-exact speed)**

- Its governor's check (2) tests only the next window and charges the hop only in hop windows.
  Voices admitted before a non-hop window can sound into a later hop window that nothing re-checks,
  so the "85 % by construction" depended on Tier-1 speed (its 83.4 % no-birth floor). **Replaced**
  by P2's sliding bank and load cap, with the hop as a static reserve.
- "Land A and B before wave 1 grows" ignores that wave 1 already changed `Granular.cpp`.
  **Corrected:** only steps 1–2 land before the merge.
- −60k cycles per hop is a model figure, calibrated by a factor fitted on latency-bound code.
  **Shown as a range** (−49k to −60k); session 2 measures it.
- Its v2's ITCM cost is 8,016 bytes in the live image (measured, §4.1), not 6.3 KB.

**P2 (deterministic governor)**

- "`pitch_reverse_spray` untouched in S2" is false: its S2 goldens differ from second 10. With the
  post refund it is untouched (F2r).
- S2 assumes a hop of ≤ 25k cycles, which no bit-exact rewrite reaches. **Constants were
  recomputed** for the bit-exact rewrites (R ≈ 3,860–4,220), and the governor was re-run (§6).
- Its musical summary leaves out `pess_births` falling from 35.2 to 17.5 voices. **Shown here.**
- The pending cap it calls necessary is not in its prototype: `pendingManual_` is uncapped, and
  there are 278,866 slot deferrals in its trigger-flood run on the default preset. **Required in
  step 11.**
- The overrun counter is at `firmware/live/main.cpp:115`, not `:114`.

**P3 (hybrid)**

- Its ledger prototype admits every manual and onset birth, steals to fit and lets tokens go
  negative (up to 20 "borrows" in its pack-only probe). So "≤ 84.9 % everywhere" and "triggers
  wait about 0.1 ms" are not evidence. **Replaced** by P2's `TryTrigger` deferral.
- Its weights (41/55/98) are model means after prototype A, not bounds. **Kept only as the
  optimistic column D.**
- A 5,000-cycle event reserve is below one event split at 64 voices. On the pedal, events do not
  split blocks (they are stamped at block boundaries), but `MacroMove` alone is about 4.3k. **24k is
  kept as a placeholder;** session 2 sizes it.
- "The grain lane projects birth-heavy blocks 5–10 points higher" understates the gap: it is about
  14 points for `pess_births`.
- "About 94 KB of DTCM free" is wrong. The budget is 80 KiB for the Hot arena plus the engine slot
  (W1 `firmware/platform/Placement.h:29-31`), of which 33 KiB is used in wave 1. Its conclusion
  (the reverb tank does not fit) stands.
- 64 unity voices at its weights need 3,008 units, not 3,024.
- The lazy prefix flush is O(V) per birth when grain lengths vary. **B is chosen instead.**
- Its FFT rewrite's run-time fallback to the old loop is dropped in the build (§4.1): the
  twiddles it checks are exact by DetMath's construction, which an assertion and a unit test now
  hold.

**Shared by the lanes**

- "6,549 / 63 ≈ 104 c per voice-sample" assumed 64 voices; `pess_render` averages 52.3 (§2).

---

## Appendix B. Evidence

Committed:

- Session 1's bench reports and logs: `firmware/records/rev7-2026-10-07/session-1/` (`bench-all.md`,
  `bench-bench-*.log`), and the silicon record.
- Step 1 and its test: `dsp/src/OnsetDetector.cpp`, `dsp/tests/test_onset.cpp`. Step 2:
  `dsp/src/PostChain.cpp`, `dsp/src/detail/PostChain.h`.

Not committed (scratch work of 2026-10-07, summarised where cited):

- **The bench-data lane:** model E and its fits, the per-block exceedance estimates, the worst-block
  decompositions, the worst-case probes and the per-block validation.
- **The hot-path lane:** prototypes A and B, an issue model of the M7 (`RenderSpan` from the
  session-1 bench ELF), the cycle attribution, and its verification (golden 33/33 on host and M7
  at 48-frame, random and 1-frame blocks; the 19 bench hashes).
- **The births lane:** the lazy prefix flush and its probes.
- **P1:** the onset rewrite (v1 and v2), its comparison harness with the perturbed control, an exact
  interpolation probe, its worst-case and hop models.
- **P2:** the governor prototype and its bench, golden, split and M7 runs.
- **P3:** the onset FFT rewrite (built as step 1), its ledger prototype and probes.
- **The judges' re-check** of P3's rewrite.
- **This document's probe:** P2's governor with the post-stage refund, its runs of the bench
  configurations and the corpus under the F1, F1r, F2r and F5r constants, and its split checks.
