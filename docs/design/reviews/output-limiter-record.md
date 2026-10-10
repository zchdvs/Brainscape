# Output limiter design pass — evidence record

> The evidence behind [output-limiter.md](../output-limiter.md) (draft 2, for the owner's
> decisions, 2026-10-09; draft 3, the owner's answers, and draft 4, after two reviews of draft 3,
> both 2026-10-10, draft 4 corrected the same day for D5's form): the inputs, what each
> investigation found and where, the probes the revision ran, where the sources disagreed and how
> the design resolves it, the alternatives it rejected, how the three reviews of draft 1 were
> disposed of (§6), the owner's answers with what draft 3 decided for them and the owner's choice
> of D5's form (§7), and how the two reviews of draft 3 were disposed of (§8). The design
> document is normative; this record is not. "Design §N" is output-limiter.md, "record §N" this file,
> "profile §N" [determinism-profile.md](../determinism-profile.md), "budget §N"
> [cpu-budget.md](../cpu-budget.md), "clock §N" [clock.md](../clock.md), "compiler §N"
> [mode-compiler.md](../mode-compiler.md), "companion §N" [companion-app.md](../companion-app.md)
> and "engine §N" [grain-engine.md](../grain-engine.md). Code is cited as `path:line` at `main`
> `011b294` (sound revision 7); "LD" is the pinned libDaisy v9.0.0. Numbers are *measured*,
> *calculated*, *estimated* or *reported*, as in design's evidence labels.

---

## 1. Inputs

### 1.1 How the evidence was produced

All of it on 2026-10-09. The owner's answer to the audition page (`firmware/factory/AUDITION.md:43`)
and the request to start the design came first. Three investigations then ran in parallel, each
reading the designs and the code at `011b294` and writing only to a scratch directory:

| Label | Investigation | What it produced |
|---|---|---|
| [practice] | Output protection in comparable pedals, Eurorack modules, plug-ins and the DaisySP library; DAC inter-sample behaviour; the Seed's analog path | design §3.1's table, the inter-sample sources, the 0 dBFS level figures |
| [dsp] | Candidates A–F of design §3.2 prototyped against the in-tree `DetMath`: sine-burst residuals, aliasing, hold ripple, the HF splatter index on the goldens, the hot-dry case, M7 code sizes | design §3.2–§3.4 and §4.6's sine figures; the `SoftSat` overshoot |
| [constraints] | The tree's contracts, tests, integration points, ITCM rules, the revision-7 golden peaks at thresholds from 0 to −3 dBFS | design §2, §5–§7 citations; the 8/10/14/18 table |

The author wrote draft 1 from these and checked it with a prototype patched into `main` (record
§2.4). Three reviews of draft 1 followed, each adversarial and each with its own checks:
**[E]** determinism and parity, **[P]** sound and product, **[H]** embedded, CPU and placement.
The revision (draft 2) checked every finding's evidence, re-ran the reviewers' probes, and built
a second prototype into the engine with the limiter's statistics read per render (record
§2.5–§2.8).

### 1.2 Environment

One Windows 11 workstation. Host builds: MSVC 17 (19.40, `/fp:precise`, Release, NMake), the
tree's own CMake. Firmware builds: arm-none-eabi-gcc 10.3.1 (GNU Arm Embedded 10 2021.10) with
the firmware's flags, `BOOT_QSPI`, in the worktree's ignored `build/fw-base`, `build/fw-lim` and
`build/fw-lim2`. Factory renders: the tree's `bspc` built by review [P]. Nothing ran on the Rev7
and qemu is not installed, so no M7 parity check and no cycle count was run for this design.

### 1.3 Where the probes live

In the session's scratch directory, none committed:

- `lim/`: [dsp]'s candidate prototypes (`lim.h`, `dblim.h`, `host.cpp`, the M7 objects and
  `out*.txt`).
- `limiter/`: [constraints]' work (`proto/proto.cpp`, the Pass 3c mock; `report.json`), and the
  three reviews: `limiter/review-determinism/` (`check.cpp`, the variant reports `v-*.json`,
  `full.json`, `flush.json`), `limiter/review-sound/` (`lim.cpp`, `ev.cpp`, `dp.cpp`, `tp.cpp`,
  `ev.txt`, `tp-s0.txt` and the `bspc` renders) and `limiter/review-embedded/` (`path.py`,
  `rf_base.s`, `rf_lim.s`).
- `ol/`: draft 1's prototype (`limiter-prototype.patch`), its evidence program (`host.cpp`,
  `ol.h`, `out.txt`) and its golden report (`report-lim.json`).
- `gwav/`: the 45 revision-7 golden renders as float WAV.
- `limrev/`: draft 2's prototype, a git checkout of `main` `011b294` with three scratch commits:
  `824d1b1` draft 1's prototype, `68eea90` the whole-mix alternative with the dual release
  (candidate A), `7868e22` draft 2 (candidate B, with the golden renderer's statistics on
  stderr). Beside it: the golden reports `v*-r.json` and `vB-*.json`, the unit-test log
  `testsB.txt`, the flush-control log `vB-flush.txt`, and the harnesses `golden-D1.exe`,
  `golden-A.exe`, `golden-B.exe`.
- `limiter/rev2/`: the revision's own programs: `wf.cpp` (what gives way, on reconstructed
  renders and a synthetic hot input; results `wf-all.txt`, `wf-mix05.txt`), `chk.cpp` (constants,
  clamp overshoot, the Mix-0 and hot-dry cases, the floor's recovery) and `rf_B.s` (draft 2's
  `RenderFrames` disassembly).

The implementing pull request of L2 should copy `wf.cpp`'s variant runner into
`tools/audition/` as the reference for design §9.5's reported readings.

### 1.4 External sources

- Strymon's BigSky MX firmware notes (strymon.net/faq/bigsky-mx-firmware-revision-release-notes)
  and plug-in FAQ.
- Mutable Instruments: `clouds/dsp/granular_processor.cc`, `rings/dsp/limiter.h`,
  `stmlib/dsp/dsp.h` (github.com/pichenettes/eurorack, github.com/pichenettes/stmlib).
- DaisySP `Source/Dynamics/limiter.cpp`.
- Fractal Audio forum (output clipping light); Eventide forum posts on H90 clipping; the EHX
  45000 manual; Airwindows ClipOnly2; Ableton's Live audio effect reference.
- Benchmark Media's application note on inter-sample overs.
- TI PCM3060 datasheet (SLAS533B); Electrosmith Daisy Seed datasheet v1.2.0; the libDaisy v9.0.0
  sources (`src/hid/audio.cpp`, `src/daisy_core.h`).
- D. Giannoulis, M. Massberg, J. D. Reiss, *Digital Dynamic Range Compressor Design*, JAES 60(6),
  2012.
- ITU-R BS.1770-4, Annex 2 (true-peak measurement).
- The tree's research notes: `docs/research/microcosm.md` (the effect volume, `:108`, `:495`) and
  `docs/research/daisy-pedal-platforms.md` (§1.5's level table, §3's bar for bypass).

## 2. Evidence by design section

### 2.1 Practice (design §3.1)

[practice] found protection limiters always on and without a control, adjustable limiters only
as creative effects, and no documented look-ahead in any pedal. Mutable Rings' limiter is a linked
peak follower (attack 0.05 per sample, release 2·10⁻⁵ per sample) feeding 1/peak into a Padé soft
clip at ×0.8; DaisySP copies it at ×0.7. Its release is about 8.3 dB/s at 48 kHz (*calculated*:
20·log₁₀(e)·2·10⁻⁵·48,000), the figure review [P] used against draft 1's 40 dB/s. The products
with an analog dry path (Strymon TimeLine and BigSky, Chase Bliss MOOD) put only the wet through
the DAC, so an over clips the wet and never the dry: the precedent for design §3.3's choice.
Benchmark Media reports that every D/A chip it tested clips inter-sample overs, which reach
3.01 dB in theory and 1.5–2 dB in practice; the PCM3060's datasheet says nothing about headroom
in its interpolation filter. Unverified by the lane's own account: whether the BigSky MX's
limiter can be turned off, the Microcosm's internal limiting, the Fractal behaviour (forum only),
the PCM3060's headroom and the op-amp swing figures.

### 2.2 The DSP candidates (design §3.2, §3.4, §4.6)

[dsp]'s host harness ran sine bursts with a 0.5 ms onset at +2.5 and +10.7 dB over full scale at
41–5,000 Hz, and measured residuals against the best linear gain (design §3.2's table), the hold
ripple at 41–220 Hz (0.05–0.135 dB with a plain 10 ms hold, 0.012 dB at 41 Hz with the restart),
and a third-difference HF index on `strum_marks` (−1.45 dB hard knee, −1.46 dB soft knee). It
found the in-tree `SoftSat` returns 1 + 2⁻²³ for 10,220 inputs in [2.983, 3]. Its hold-restart
rule (any frame within 0.25 dB) never released; draft 1's prototype measured the stuck gain at
−0.25 dB, and the `r < 1` condition fixed it (design §4.4 item 5). The instantaneous wet-only
variant dropped a −6 dBFS wet by 38.6 dB under a full-scale dry (design §3.3).

### 2.3 The tree's constraints (design §2, §5–§7)

[constraints] mapped the contracts of design §2.4 to their tests (`test_modes.cpp:356-368`,
`:469-504`, `:506-558`; `test_engine.cpp:320-347`), the integration points (`Engine.cpp:422`,
`:642-645`, `:686-690`, `:1231-1261`), the codec path (LD `src/hid/audio.cpp:447-453`,
`src/daisy_core.h:33-34`, `:146-151`), the plugin's output path
(`plugin/src/PluginProcessor.cpp:1061-1108`), the ITCM placement rules
(`firmware/CMakeLists.txt:198`, `:211-217`; `cmake/ItcmCheck.cmake`) and the audition's checks
(`tools/audition/README.md`, `Suite.cpp:29`, `:519`, `:677-680`; `ratings.py:21-27`,
`:227-281`). Its threshold table: 8, 10, 14 and 18 golden renders change at 0, −1, −2 and
−3 dBFS.

### 2.4 Draft 1's prototype (design §2.3, §4.4, §9)

`ol/limiter-prototype.patch` on `011b294`: the whole-sum limiter, one 40 dB/s release.
*Measured:* 37 of 45 golden hashes held; the eight that changed first differed in the predicted
second; no counter changed; all 45 identical under the eight variants; 200 of 201 unit tests (201
with the click test's input halved); identity exhaustive at G = 1 over all 1,065,353,217
magnitudes; fl(p·fl(1/p)) ≤ 1 for every float in (1, 2¹²⁶) without the floor. Its live image:
`.itcm_text` 63,128 bytes (+864), DTCM unchanged. Its limiting figures on the eight changed
goldens, for comparison with draft 2's (design §2.3):

| Preset | Frames limited | Engagements | Lowest gain | Frames at the ceiling |
|---|---|---|---|---|
| `hot_out` | 275,993 | 1 | −27.73 dB | 1,524 |
| `automation_offgrid` | 68,147 | 6 | −11.41 | 79 |
| `spillover_chain` | 2,605 | 1 | −1.77 | 2 |
| `exact_load_mid` | 5,770 | 4 | −1.32 | 5 |
| `midi_gate` | 2,930 | 2 | −1.22 | 3 |
| `strum_marks` | 1,293 | 1 | −0.40 | 2 |
| `post_max` | 939 | 1 | −0.37 | 8 |
| `lone_changes` | 577 | 1 | −0.08 | 1 |

Draft 2's engine build with F = 1 and one 40 dB/s rate reproduces this table and all 45 of draft
1's hashes (record §2.7). Two of draft 1's evidence claims did not survive review: its §5.4
example of a restart while limiting (record §6, E1) and its one-step bound's region (E4).

### 2.5 Release variants (design §4.6, D3)

Review [P]'s `lim.cpp` applies the whole-sum law to `bspc`'s revision-7 float renders. The
revision re-ran it unchanged (*measured*):

| Render | Release | Limited | Engagements | Attacks | 50 ms windows ≥ 0.5 dB (worst) |
|---|---|---|---|---|---|
| Echolalia S11 a1r0s1t0, SoftNotes | 40 dB/s | 1.54 s | 30 | 476 | 26 (1.14 dB) |
| | dual 40/10 | 1.85 s | 23 | 345 | 3 (1.14 dB) |
| | 20 dB/s | 1.81 s | 24 | 390 | 10 (0.73 dB) |
| | 10 dB/s | 2.27 s | 17 | 307 | 0 (0.46 dB) |
| Echolalia S0 Saturation | 40 / dual / 10 | 8.52 / 10.03 / 10.03 s | 10 / 1 / 1 | | 97 / 0 / 0 |
| Updraft S0 Saturation | 40 / dual / 10 | 9.82 / 10.19 / 10.19 s | 5 / 1 / 1 | | 191 / 0 / 0 |

The windows overlap (a 10 ms hop), so a count is of windows, not events. Review [P]'s isolated
figures under a plain 10 dB/s release (`strum_marks` 2,750 frames, `midi_gate` 8,814) and under
the dual release (`strum_marks` 1,293, `midi_gate` 2,930, `exact_load_mid` 5,770, the same as 40
dB/s) were not re-run on the WAVs; the engine build reproduces the dual release's isolated
behaviour on the goldens (record §2.7: with F = 1 the dual release changes only `hot_out` and
`automation_offgrid` from draft 1).

### 2.6 What gives way (design §3.3, D11)

`limiter/rev2/wf.cpp` rebuilds each factory preset's dry and wet terms from `bspc`'s S0 engaged
render and its Mix-1 wet render, as review [P]'s `ev.cpp` did: dry = engaged − lw·wet, wet term =
lw·k·wet, with lw = min(1, 2m) at the preset's stored Mix (0.30–0.45) and k the effect volume.
Mix 0.5 is the effect volume 20·log₁₀(1/lw). It runs one gain state with design §4.3's time law
under six rules: the whole mix at 40 dB/s, the whole mix with the dual release, the wet first at
F = −6, −12 and −18 dB, and the wet only. Over 18 presets × 3 class inputs (*measured*; full
lines in `wf-all.txt` and `wf-mix05.txt`): design §3.3's tables. In every run, no output sample
passed full scale.

A synthetic hot input, `wf --synth`: a 110 Hz sine at twice full scale clipped to 1 − 2⁻²³ for
2 s, both channels, under a 220 Hz wet at −6 or −20 dBFS for 4 s. Its results are design §3.3's
second table. The wet-only rule drove the wet to −132.5 dB (−118.5 dB at −20 dBFS) and had not
returned to unity when the render ended 2 s after the dry.

### 2.7 Draft 2's prototype in the engine (design §2.3, §4.4, §6.1, §9)

`limrev/tree` at `7868e22`: design §4.3's limiter in `dsp/src/detail/OutputLimiter.h`, with F and
the slow rate as build-time constants; design §6.1's Pass 3c change; the statistics accessors;
the live image's console publishing; and a scratch stderr line per render in the golden harness
(`LIMSTATS`: limited, dry, engages, lowest, peak, frames at ±1.0, frames over 1.0; `LIMRESTART`:
the gain at each restart).

*Measured:*

- **Four builds of one code.** F = 1 with one 40 dB/s rate: all 45 hashes equal draft 1's
  prototype report. F = 1 with the dual release (candidate A): all 45 equal candidate A's own
  earlier build. F = 2⁻² with the dual release (draft 2): 37 of 45 equal revision 7.
- **Draft 2 against revision 7** (`golden.json`): the eight changed presets and seconds of design
  §9.1; `automation_offgrid`'s `outActiveFrames` 568,612 → 568,600 and `tailActiveFrames` 50,261
  → 50,249; no other counter changed.
- **Candidate A against revision 7:** the same eight, `automation_offgrid` seconds 3–10, its
  counters 568,612 → 568,594 and 50,261 → 50,250. Candidate A and draft 2 share `hot_out`'s hash
  (Mix 1, no dry term); the other seven differ.
- **Variants:** draft 2's 45 hashes identical under `--block 1`, `--block 512`, `--pattern
  48,1,127,32`, `--random-blocks 7`, `--fp-env hostile`, `--fresh-engine` and `--delivery split`,
  each with 19 presets' invariance checks passing. `brainscape_golden_flush --force-flush-control`:
  45 of 45 coverage ok, 0 failures.
- **Restarts:** `LIMRESTART restart_kept_params frame 288010 gain 1` and `exact_load_mid frame
  348345 gain 1`, under every candidate.
- **Unit tests:** 200 of 201; `post-delay time automation is click-free` fails at 5.84 times its
  neighbourhood, as under draft 1, because its renders play at Mix 1.
- **`chk.cpp`:** `release` `0x3F800325` (40.0074 dB/s), `releaseSlow` `0x3F8000C9` (9.9898 dB/s),
  H 480; at 44.1 kHz `0x3F80036C`, `0x3F8000DB`, 441; at 96 kHz `0x3F800192`, `0x3F800065`, 960.
  Of 7,950,205 random pedal attack frames (a on the codec grid, wet terms up to 2⁴⁰), 4,750,206
  have a requirement above the floor and 11,178 of those exceed 1.0 before the clamp, each by
  1 ulp; all 3,199,999 at the floor exceed it and are clamped. No clamped output passed its
  ceiling. Mix 0 at G = 0.5: the output is the dry, exactly. The linked hot dry (aL = 3.0, bL =
  0.1, aR = 0.2, bR = 1.3): outL 3.0000, outR 0.5207, G 0.2479. From the floor at 40 dB/s,
  173,842 frames (3.622 s).

### 2.8 The live image: ITCM and instruction counts (design §8)

Built in `build/fw-base` (`main`), `build/fw-lim` (draft 1) and `build/fw-lim2` (candidate A,
then draft 2), from the maps, `size.txt` and `arm-none-eabi-nm -S` (*measured*):

| | `.itcm_text` | `Engine.cpp` `.text` | `RenderFrames` | `Impl::Init` | `Impl::Reset` | Accessors |
|---|---|---|---|---|---|---|
| `main` | 62,264 | 10,980 | 1,740 | 1,556 | 220 | — |
| draft 1 | 63,128 | 11,848 | 2,438 | 1,668 | 236 | — |
| candidate A | 63,264 | — | — | — | — | `OutStats`, `ConsumeLimiterMinGain` |
| draft 2 | 63,544 | 12,264 | 2,766 | 1,708 | 244 | `OutStats` 26, `ConsumeLimiterMinGain` 12, `ConsumeLimiterPeak` 12 |

DTCM is 35,840 bytes in all four. `Engine::Impl`'s member after the limiter moved from offset 104
to 144 in draft 1 (review [H]) and to 160 in draft 2 (`rf_B.s`: `ldrd r3, r2, [r4, #160]`). The
ItcmCheck reports 6 of the engine's 6 COMDAT sections linked, all in ITCM, in candidate A's and
draft 2's images: `Need` and the other helpers inline.

**Instruction counts.** Review [H] traced draft 1's `RenderFrames` (`rf_lim.s`, its `path.py`):
today's tail 18 instructions with 4 `VMRS`; idle 33/7; the pedal attack 102/19 with one `VDIV`;
release under demand 115/22 with one `VDIV`. The revision traced draft 2's (`rf_B.s`,
`0x226c`–`0x2d3a`) by hand from the saturation's compare at `0x2a02` to the loop's branch at
`0x2932`: idle `0x2a02 → 0x275e → 0x2cc4 → 0x2d24 → 0x2a2e → 0x2ce2 → 0x2cfe → 0x2928`, 33
instructions, 7 `VMRS`; the limiting paths enter at `0x278e` (ceilings), `0x27ca` (peak), `0x27e6`
and `0x2846` (each channel's requirement, `VDIV` at `0x2800`/`0x2862` and the second stage's at
`0x2826`/`0x2888`), `0x2bd4`–`0x2bfe` (floor and branch choice), `0x2b68` (attack), `0x2b92`
(release), `0x28d4` (wet-only output) and `0x2a9e` (whole-mix output). Design §8.1's table gives
the totals. The counted paths spill the per-call limited count to the stack (`ldr`/`adds`/`str`
at `[sp, #40]`), three instructions per limited frame that L2 may keep in a register.

### 2.9 The analog side (design §6.4)

Review [P]'s `tp.cpp` estimates true peak with a 4× Hann-windowed sinc interpolator, 32 taps a
side. The revision re-ran it on its validation file (`fs4.wav`: +3.00 dBTP at a sample peak of
−0.01 dBFS) and on Shards' stored Plucks render (−3.27 dBFS, −0.16 dBTP): both as reported. Its
`tp-s0.txt` lists the stored-position OnsetBursts renders at +0.35 (Pinhole) to +1.51 dBTP
(Refrain) with sample peaks of −1.02 to −2.72 dBFS; those renders never reach 0 dBFS, so the
limiter is idle on them and the figures are today's. The firmware path for a test tone does not
exist (review [H], H5): `firmware/bench/main.cpp` never calls `StartAudio`, and the live image's
`AudioCallback` (`firmware/live/main.cpp:86-121`) only runs the engine or writes zeros. `bspc
--metrics` gave Echolalia's S0 engaged OnsetBursts render +0.14 dBFS (1 sample over), Déjà Vu's S0
wet SoftNotes render +0.52 dBFS (65 over), and the S0 Saturation renders +5.53 (Pinhole) to
+10.73 dBFS (Shards).

## 3. Where the sources disagreed, and how the design resolves it

1. **The release rate.** Draft 1 chose 40 dB/s from sine measurements; Rings and DaisySP release
   at about 8.3 dB/s; review [P]'s program renders flutter at 40. *Resolution:* the dual release,
   which keeps isolated transients at 40 dB/s and moves sustained material to 10 dB/s (design
   §4.6, D3).
2. **What gives way.** Draft 1 scaled the whole sum; review [P] proposed a dry-priority variant
   from a held dry peak; the analog-dry pedals let only the wet clip. *Resolution:* one gain
   state with a floor F: the wet first to −12 dB, then the whole mix. The pure wet-only form was
   measured and rejected (record §2.6, §4).
3. **"The goldens can be predicted from the output alone."** True for draft 1 only while no
   restart lands inside limiting, and false for draft 2, which needs the dry and wet terms.
   *Resolution:* the table is measured in the engine, and the order-aware re-mint check names
   both requirements of an offline model (design §2.3, §6.2, §9.1).
4. **"`exact_load_mid` restarts while the limiter is active."** Draft 1 inferred it from the
   changed seconds 5–7; review [E]'s trace shows the gain at 1 at the restart, and draft 2's
   `LIMRESTART` line agrees. *Resolution:* corrected, and coverage added (design §5.4).
5. **The extreme-input bound.** Draft 1 put the clamp's region at 2¹²⁶; review [E] showed the
   floor binds from 2²⁴. *Resolution:* the clamp is stated as the bound, with draft 2's measured
   overshoot counts (design §4.4 item 2).
6. **The cost model.** Draft 1 modelled 70–110 cycles per limited frame from a mock; review [H]
   counted the live image. *Resolution:* draft 2's figures come from its own image (design §8.1).
7. **"No factory preset changes at its stored positions."** S0 is "the stored preset on every
   vector" (`tools/audition/README.md:80`), so the Saturation renders change, and two more S0
   renders pass 0 dBFS. *Resolution:* restated wherever it appeared (design Summary, §9.5;
   STATUS; compiler §11.3's note).
8. **"A hot dry is never pulled under its own level."** Review [E]'s linked counter-example.
   *Resolution:* restated as "never louder than max(1, |a|); untouched while the wet gives way"
   (design §2.4, §4.4 item 7; the profile and companion notes).
9. **When to measure the DAC.** Draft 1 left it to bench session 2, after the ratings; review [P]
   showed a later ceiling change would re-mark rated rows. *Resolution:* lane L1b before revision 8
   (design §6.4, D7, D10).

## 4. Alternatives considered and rejected

- **The wet only, with a hold (dry priority), F → 2⁻²⁴.** Under a full-scale dry the wet has no
  room: it fell 118–132 dB and had not returned 2 s after the dry stopped (record §2.6). The
  effect would vanish on any hot input.
- **Review [P]'s held-dry-peak variant.** It tracks the dry's peak on every frame, idle included,
  and its peak decays toward 0, which would need a floor or a flush site; and under a full-scale
  dry it mutes the wet as the wet-only form does. The floor F gives the same steady dry on normal
  material with neither cost.
- **F = −6 dB or −18 dB.** −6 dB scales the dry at effect volume +12 dB (Lull SoftNotes: −2.95 dB);
  −18 dB lets the wet fall 18.6 dB on hot input and recover more slowly (0.47 s against 0.34 s).
  −12 dB is the smallest power of two that keeps every factory preset's dry untouched at +12 dB.
- **A plain 10 dB/s release.** It removes the flutter but lengthens every isolated duck
  (review [P]: `strum_marks` 2,750 frames against 1,293).
- **Unlinking the channels when a ceiling exceeds 1** (review [E]'s option for E2). It would keep
  a plugin hot dry at its level but move the stereo image, for a case only hostile or mis-staged
  plugin input reaches; with the wet first the dry is already untouched until F (D4).
- **Narrowing the effect volume's top** (review [P]'s option c). Even +3 dB engages 12 of 18
  presets at stored positions under the whole-mix rule, and it does nothing for hot inputs.
- **Keeping draft 1's one-division branch.** Draft 2 has one path for both targets: the pedal
  runs the general requirement with c = 1, so every golden executes it. The branch's equivalence
  test is replaced by §9.4's hash-pinned plugin test and, if L2 shares the second stage's
  division, its equivalence test.
- **Restating budget §6's governor figures on the 45-preset corpus here** (review [E], E5's third
  point). It needs the governor's prototype on today's corpus, which its lane owns; the design
  states the corpus sizes and makes the re-mint check order-aware instead.
- **A true-peak detector now.** Not without L1b's evidence: it costs a 4× side chain or a lower
  ceiling, and changes contract #2.

## 5. Provenance

Written 2026-10-09 on branch `claude/limiter-design` from `main` at `011b294`. Draft 1, commit
`5f7357b`, was reviewed the same day by [E], [P] and [H]; draft 2, commit `2297cbd`, applies
their findings (§6), and `13f9e14` moved its revision after CLOCK's T1 and T2. Draft 3
(2026-10-10) records the owner's answers to design §11.2 and specifies D5's switch (§7), commit
`2895453`. Two reviews read it the same day; draft 4 applies their findings (§8), commit
`7fca660`, and its correction the same day records D5's form as the owner's choice (§7.5). Its
open questions (§11.5) are settled by the owner (what D5's Off does, D14), by L1b, by listening
in L2's A/B (Q9 among them), and by the control-surface and bypass designs.

## 6. Review of draft 1 and its disposition

Three reviews read draft 1: **[E]** determinism and parity (1 major, 4 minor), **[P]** sound and
product (3 major, 4 minor), **[H]** embedded, CPU and placement (1 major, 4 minor). The revision
checked each finding against the code at `011b294`, libDaisy's source, the live images' maps and
disassembly, and the reviewers' probes, re-running review [P]'s release, effect-volume and
true-peak programs and confirming review [E]'s restart trace with its own. **Every finding
survived the check and was applied.** Three were applied with a part changed or deferred, each
explained in §4: P2 (the trade went further than its options: the wet-first rule is recommended,
and its dry-priority form rejected), E2 (its unlink option rejected) and E5 (its restatement of
the governor's figures deferred to the governor's lane). E3's branch-equivalence test no longer
applies, because draft 2 has no separate pedal branch. No finding was rejected outright.

| # | Finding | Disposition (design §) |
|---|---|---|
| E1 | No golden or test covers Restart, Exact load or Spillover while limiting; §5.4's evidence is wrong | applied: `limit_sustain` gains a Trails and a FastCut load inside its held over; new `limit_restart` with an Exact load and a `Restart` while limiting and RestartTail; `RestartsWhileLimiting` and `LoadsWhileLimiting` counters, each required ≥ 1; two engine lifecycle tests; §5.4's sentence corrected, with draft 2's own trace (record §2.7); §2.3 and §6.2 restated: the table is measured in the engine, and an offline model must reset at every `Reset` and needs the dry and wet terms (2.3, 5.4, 6.2, 7.2, 9.2–9.4) |
| E2 | "A hot dry is never pulled under its own level" is false with linked gain | applied: restated in the Summary, §2.4, §4.4 item 7 and the profile and companion notes as "never louder than max(1, \|a\|); untouched while the wet gives way; past F the linked gain can pull it below its own level", with a measured example; the unlink option put in D4 and not recommended (record §4) |
| E3 | The plugin's hot-dry branch has no bit-level coverage | applied: draft 2 runs one requirement path on both targets, so every golden executes it with c = 1; a hash-pinned unit test over fixed-seed hot-dry plus wet frames pins the c > 1 values on every leg; `limit_hot_mix0` replaced by `limit_hot_kill` (Mix 0.5, the cutoff kill, `LimitedFrames` = 0); §9.2's "on the codec's grid" corrected. The branch-equivalence test no longer applies; an equivalence test is required for any bit-identical optimisation L2 takes (8.1, 9.2–9.4) |
| E4 | The extreme-input bound is in the wrong place: the floor binds above 2²⁴ | applied for draft 2's arithmetic: the clamp is stated as the bound; measured, 11,178 of 4,750,206 random attack frames above the floor overshoot by 1 ulp and every frame at the floor is clamped; tests at wet terms 2²⁵, 2³⁰, 2¹⁰⁰ and 2¹²⁶ and above assert G = 2⁻²⁴ and ±c (4.4, 9.4) |
| E5 | Two change-set claims are wrong or depend on D7 | applied: the Summary and STATUS qualified (the stored positions on the class inputs keep their hashes; every S0 Saturation render and two other S0 renders change); §9.1 and §10 order-aware, whichever lands second regenerating its predicted set from the first's renders; the two corpus sizes stated. The governor's figures on today's corpus deferred to its lane (record §4) (Summary, 9.1, 10) |
| P1 | The 40 dB/s release with a 10 ms hold flutters on the owner's case | applied: the dual release (40 dB/s, then 10 dB/s once an over returns during a release, cleared at G = 1), the review's numbers re-run and reproduced; 40, dual and 10 dB/s in L2's A/B; §4.5 and §4.6 rewritten from program renders; a sustained-material count in the engine tests and the audition report (4.1–4.6, 9.4, 9.5, D3) |
| P2 | Raising Mix or the effect volume ducks the dry on most factory presets | applied and taken further: the wet gives way first, down to F = −12 dB, then the whole mix, recommended in a new D11 with the whole mix, the wet only and a narrower effect volume as alternatives; measured on all 18 presets × 3 inputs at Mix 0.5 and effect volume +6 and +12 dB, and on a synthetic hot input; §2.4's row and §4.5's rows restated; effect-volume, Mix 0.5 and +3 dB readings in the pre-screen; the Rev7's LED shows limiting. The review's held-dry-peak form rejected (record §4) (3.3, 4.5, 7.3, 7.4, 9.5, D6, D11) |
| P3 | The DAC's inter-sample test comes after revision 8 is minted | applied: the two analog measurements move to a new lane L1b, before L2, with the `tone` verb (H5); D7 and D10 state the order and the owner's choice if L1b cannot come first; the review's true-peak figures spot-checked (6.4, 8.5, 10, 11.1, D7, D10) |
| P4 | "No factory preset changes at its stored positions" is false; two stored-position overs are hidden | applied: verified with `bspc --metrics` (+0.14 and +0.52 dBFS); the Summary and STATUS reworded; both added to §1.1 and §9.5; the pre-screen reports per S0 vector and per wet render (1.1, 9.5) |
| P5 | No input-level target, while ordinary boosted sources engage the limiter | applied: D12's target, the hottest supported source at or below about −7 dBFS peak at the codec, handed to the hardware design; a +3 dB input reading in the pre-screen. With the wet first such a source dips the wet, not the dry (6.4, 9.5, D12) |
| P6 | No test bounds the limiter's sound | applied: the feedback-0.9 click render kept at its original level as a limiter test with a bound from the A/B (about 6.0 times), the existing test's input halved; the sustained-material count added (9.4) |
| P7 | Bypass and trails not addressed | applied: §6.5 and D13: the bypassed dry never passes through the limiter's gain, the limiter's state continues across bypass; a row in §4.5 (4.5, 6.5, D13) |
| H1 | The cycle model is below the live image's own code | applied: draft 2's costs counted from its own live image (§8.1's table), with draft 1's from the review; statistics moved off the hot path (attack branch, frames with an over, per-call 32-bit locals), as prototyped; the placeholder set at 260 cycles per frame for draft 2 (the review's 160 for the whole-mix alternative) and S, R and D9's voices restated; the bit-identical savings named for L2 (8.1, 8.2, D9) |
| H2 | The session-2 plan would not measure the worst path | applied: every path timed separately in `SuiteMicro` plus a path-switching sequence; S's own line at 48 × the slowest path + 10 %; `hot_out` kept as a G3 conformance case only (8.5) |
| H3 | The console reads audio-thread-only APIs from the main loop | applied: `AudioCallback` folds deltas into 32-bit atomics and keeps the lowest gain and highest peak as float bits, cleared on `g_statReset`, read inside `stats`' snapshot; noted as QSPI code in the 15 %; built in draft 2's prototype (7.4, L5) |
| H4 | The ITCM tally leaves out CLOCK, step 5 and the accessors, and gives `Init`'s share wrongly | applied with draft 2's measured image: the accessors measured (50 bytes) and included in 1,280; `Init` +152 (draft 1's was +112, not 40); the tally restated with CLOCK and step 5: 13,340–13,954 bytes to move, about 2.2–2.9 KB left for step 5; carried into clock §9.6 when D7 is confirmed (8.3, 11.6) |
| H5 | No firmware path produces D10's test tones | applied: the live-image `tone <hz> <phase> <amplitude bits>` verb through `g_muted`, outside the engine and not a sound revision, in L1b with the expected codec value 8,388,482 (6.4, 11.1) |

## 7. The owner's answers (2026-10-10), and draft 3

### 7.1 The answers

The owner marked the thirteen limiter decisions on the decisions page ("Brainscape Open
Decisions", its "Output limiter" set, which shows design §11.2's recommendations). The marks were
saved on 2026-10-10 between 07:25 and 07:27 UTC:

| Decision | Mark | Note |
|---|---|---|
| D1–D4, D6–D13 | Agree | none |
| D5, control | **Change** | none |

The page's D5 note field is empty: its database holds `answers/limiter-D5` =
`{"choice": "change", "note": "", "updated": "2026-10-10T07:26:03.625Z"}`, the only "change". The
page's D5 offered draft 2's recommendation, "None: always on", and no alternatives.

*Corrected in draft 4.* Draft 3 said the form of the change "came with the owner's request to
revise the design". It did not. The owner's message says only that the limiter decisions are
answered. The form came from the task text of the workflow that commissioned draft 3, which
described the change as **a per-preset switch, a stored preset parameter (a Leaf), so that a
mode can turn the limiter off on purpose, for example for deliberate clipping; default On.** That
text may relay something the owner said elsewhere, but nothing the owner wrote that this record
can cite contains it. So draft 3's form is a proposal: draft 4 labels it so and asks the owner
in design §11.5 Q1, with three other readings (a device setting, a stage control, and Off passing
overs). D7's agreement is with its amended answer, the next free revision expected 10, which the
page showed.

*Corrected after draft 4 (2026-10-10).* The owner did choose the form, in the session, from four
options; the task text's description of the change follows the chosen option's own wording.
Draft 4's "proposal" label is withdrawn: §7.5 gives the owner's question and answer, why draft 4
had it otherwise, and what the correction restated.

### 7.2 What draft 3 decided for the switch, and what it rejected

1. **What Off means** (design §4.7). *Chosen:* a clamp at the limiter's own ceiling,
   max(1, |a|) per channel.
   - On the pedal, `f2s24(Clamp(s, 1))` equals `f2s24(s)` for every s, so Off plays code for
     code what the engine without a limiter plays (*calculated* from LD `src/daisy_core.h`'s
     clamp to ±`FBIPMAX`, design §2.2).
   - *Rejected:* an Off that passes s unchanged. On the pedal it is the same sound. In the
     plugin it hands a DAW floats above 1.0, which a floating-point bus plays unclipped, so an
     Off preset would clip on the pedal and not in the plugin. It would also give up Goal 1 and
     the audition's "no sample over full scale" for every Off preset.
2. **Turning Off mid-limiting** (design §4.7). *Chosen:* a drain at k to exactly 1, with no
   attack and no hold, then the clamp. *Rejected:*
   - an instant switch, a step of up to 27.7 dB on the goldens, and up to 144.5 dB from the
     floor;
   - a crossfade over `kFastCutFrames`, 12 dB in 2.67 ms, about 4.5 dB per ms, which also
     computes both outputs while it runs;
   - a longer crossfade, which is a release with more state;
   - the slow rate, or the current rate, for the drain. The slow rate exists to stop re-attack
     flutter, which a drain cannot have, and it would take 14.5 s from the floor where k takes
     3.6 s.
3. **Turning On** (design §4.7). *Chosen:* from a settled Off, the reset state, so the next over
   attacks from unity; during a drain, G continues with `releasing` set. *Rejected:* G to 1
   during a drain, which steps the level up, and G to the frame's requirement, which steps it
   down.
4. **How the engine hears the row** (design §4.7, §6.1). *Chosen:* a seventh `ParamDomain` bit
   whose rebuild calls `Switch` once per span, so only a frame's final value acts and every load
   applies it by marking all domains. *Rejected:*
   - row 85's form, no domain and read where it acts, which here would be a per-frame load in
     the idle path;
   - applying it inside `ApplyParam`, where an Off and an On at one frame would clear the hold
     and flags and fail `AmongEdits`.
5. **The row** (design §7.1). *Chosen:* ID 87; name `output.limiter` in a new `output` object;
   numeric 0 or 1, read with `>= 0.5f`; the `OffOn` display. *Rejected:*
   - `global.limiter`, because `global.*` mixes the device settings 81, 82, 85 and 86 with the
     one preset leaf `global.mix`, and a preset leaf there would read as a device setting;
   - a JSON boolean, because leaves are numbers (compiler §2.2), and the editor's form, typed
     text and name-patching treat them so;
   - draft 2's fallback of a Global device setting, since the owner chose per preset.
6. **Reach** (design §7.1). *Chosen:* no macro or expression target; registered for hosts
   without automation. *Rejected:* expression reach. CTRL is outside `sound_hash`, so it could
   play a package Off whose STAT says On, and a pedal near the threshold would chatter between
   limiting and clipping.
7. **The revision that carries the package change** (design §7.1, §10). *Chosen:* the
   limiter's own, 10, in one commit with its re-mint, with the package-change label and one
   `Package-change:` line, or one per revision if it shares T2's pull request. *Rejected:*
   folding it into T2's re-stamp. At revision 9 row 87 does not exist, and profile §5.12's
   per-commit rule keeps each revision's corpus whole.
8. **Factory policy** (design §9.5). *Chosen by draft 3:* factory presets keep it On, enforced
   by L15 under `--factory` and by the pre-screen. *Rejected for now:* Off allowed under a
   declared `clip` class. *Draft 4:* this is a product decision, so it became the owner's D14,
   recommended as draft 3 chose, with the declaration (now a `--clips` flag, not a class)
   specified as the alternative (§8, findings Q2 and Q4).

### 7.3 Checks run for draft 3

- **The drain's frame counts** (design §4.7's table). `limiter-d5/drain.py` iterates
  G ← min(1, fl(G·k)) with k = `0x3F800325`. Each binary64 product of two binary32 values is
  exact and is rounded once to binary32. Output:

  | Start | Start bits | Frames to 1.0f | §4.4's figure (measured) less 481 |
  |---|---|---|---|
  | −0.1 dB | `0x3F7D11D1` | 120 | 601 − 481 = 120 |
  | −1.0 dB | `0x3F642905` | 1,200 | 1,681 − 481 = 1,200 |
  | −2.5 dB | `0x3F3FF911` | 3,000 | 3,481 − 481 = 3,000 |
  | −6.0 dB | `0x3F004DCE` | 7,199 | 7,680 − 481 = 7,199 |
  | −11.4 dB | `0x3E89CE7C` | 13,678 | 14,159 − 481 = 13,678 |
  | −27.7 dB | `0x3D28CB8F` | 33,234 | 33,715 − 481 = 33,234 |
  | F = 2⁻² | `0x3E800000` | 14,447 | — |
  | 2⁻²⁴ | `0x33800000` | 173,362 (3.612 s; 14.46 s at k_s) | 173,842 − 481 = 173,361. Draft 3 read this as "one frame later"; it is one frame *shorter*, because the prototype counted from the first frame after the burst. By the table's convention the row is 173,843 (§8.3) |

- **Row 87.** `Params.h` was read on `main` `011b294` (rows 1–82) and on `claude/tempo-core`
  `b91b34d` (83–85). Every local and remote branch's `Params.h` and design row tables were
  searched for a row above 85. Only clock §10.4's 86, `global.tempo_glide`, was found.
- **The tree at `claude/tempo-core` `b91b34d`,** read for the switch's integration points:
  - `ParamDisplay.h` and `ParamDisplay.cpp`: the `OffOn` kind at `:429-430` and
    `HostAutomatable` at `:130-134`;
  - `compiler/src/Schema.cpp`: `Visit`'s order at `:459-460` and `global.mix`'s E8 at `:1575`;
  - `dsp/src/blob/Validate.cpp`: `TargetMix` at `:320` and `ExpressionTarget` at `:388-394`;
  - `plugin/src/BrainscapeParam.cpp`: the `OffOn` text at `:184-186` and `isBoolean` at `:310`;
  - `dsp/src/Engine.cpp`: the span loop at `:1085-1092`, `RebuildDirty` at `:1190-1215` and
    the domain assert at `:127`;
  - `dsp/tests/golden/Corpus.cpp`: `LoneChanges` at `:316-334` and `AmongEdits` at `:980`;
  - `Corpus.h`: `kCorpusVersion` = 13;
  - `firmware/live/main.cpp`: the `set` names and `EveryLeafSettable` at `:187-232`;
  - clock §11.11: items 21–23, the re-stamp, the ITCM at 792 bytes spare and T2's corpus 14.
- **`tools/audition/ratings.py:20-27`, `:262-278`:** carry-forward by render hash, which moves
  `sound_hash` without a re-listen (design §9.5).
- **Not run:** the switch is not prototyped. Its ITCM (100–200 bytes), DTCM (about 36 bytes)
  and cycle figures (design §8.1) are *estimated*, and L2 measures them.

### 7.4 Where the probes live

`limiter-d5/drain.py` in the session's scratch directory, not committed, beside the earlier
probes of §1.3.

### 7.5 The owner's choice of D5's form (2026-10-10), and draft 4's correction

**The question and the answer.** After marking D5 "Change" on the decisions page with no note,
the owner was asked in the chat session, on 2026-10-10, "For limiter decision D5 (user control),
what change do you want?", with four options:

1. a device setting, on or off;
2. off in the plugin only;
3. a per-preset switch: "A stored preset parameter, so a mode can turn the limiter off on
   purpose (e.g. for deliberate clipping). Adds a new leaf, so every package hash changes
   (package-change label)";
4. an adjustable ceiling.

The owner answered **"Per-preset switch"**. D5's form is therefore the owner's decision, and
design §11.2 records it as "Changed by the owner: a per-preset switch (row 87, `output.limiter`,
default On)", with draft 2's recommendation, none, kept for reference.

**Why draft 4 had it as proposed.** Review [Q] and the revision that applied its findings read
the decisions page's database and the owner's message that the decisions were answered. Neither
holds the form: the page's note is empty, and the message names no form. The chat session in
which the owner chose it was not visible to them. So finding Q1 (§8.2) concluded that the owner
never wrote the form, and draft 4 labelled the switch "proposed, awaiting the owner's
confirmation", held row 87 "for the limiter's control", and asked the owner, in design §11.5 Q1,
to choose among four readings of its own (a per-preset switch, a device setting, a stage control,
and an Off that passes overs). The evidence it read was accurate but incomplete; the conclusion
was wrong.

**What the correction restated.** No technical content changed. In the design: the status
block, "What changed in draft 3" and "in draft 4", the Summary's control item, §1.3's last
non-goal, §1.4's switch term, §4.7's opening and "What Off means", §7.1's heading, text, ID row
and MIDI CC and panel line, §7.4's product-panel line, §9.5's opening and D14 paragraph, §10's
opening and table, §11.1's L2 row and total, §11.2's preamble, D5 row (Answer: "Changed by the
owner") and D14 row, the "pending D5" labels of D6, D8, D9, D10 and D13 (now "for D5"), §11.3's
decision 11, §11.4's risk 11, §11.5's Q1, Q2 and Q7, §11.6 and §12. In this record: the header,
§5, §7.1, §8's opening and findings Q1, Q2 and Q5. Elsewhere: STATUS's limiter lines, the
README's row and the dated notes in mode-compiler.md §11.3, companion-app.md §4.8 and
determinism-profile.md §3.7. Row 87 is the switch's.

**What stays open.** The owner chose the form, not everything in it:

- **What Off does** (design §11.5 Q1, which now asks only this, and absorbs draft 4's Q7): Clip,
  as designed and recommended, since on the pedal it plays code for code what the unlimited
  engine plays and in the plugin it matches the pedal (§7.2 item 1); or letting overs pass
  unclipped, which would split the plugin from the pedal above full scale.
- **D14,** whether factory presets may use the switch, recommended On for now.
- **Q9,** the instant attack or the entry ramp when a Limit preset loads with Trails after a Clip
  one, recommended instant; L2's A/B decides.

The options the owner did not choose, and draft 4's other readings, are not carried as open
questions.

## 8. Review of draft 3 and its disposition

Two reviews read draft 3 (`2895453`) on 2026-10-10: **[K]** package, determinism and tests
(3 major, 5 minor), and **[Q]** product (3 major, 3 minor). The revision checked each finding's
evidence against the tree at `claude/tempo-core` `b91b34d` and `main` `011b294`, against the
decisions page's database, and with a simulation of design §4.3 (§8.3). **Every finding survived
the check and was applied; none was rejected.** Q1's premise was later found wrong, and its
labels were reversed by the correction of §7.5. Five were applied with a choice among the fixes
the review offered, or with more than it asked, each explained in the table: K1 (read as any
integer-valued leaf, not 0/1-only), K3 (the "gain below F at the Off" check made with two
per-call counts, since the corpus has no windowed requirement), K6 (row 86 reserved, not L2
after T2 only), K8 (four of the eight changed renders are package presets too), and Q3 (the
entry ramp offered, the instant attack kept as the recommendation).

### 8.1 [K] Package, determinism and tests

| # | Finding | Disposition (design §) |
|---|---|---|
| K1 | Reading only the integers 0 and 1 contradicts compiler §2.2 and breaks the round trip, since STAT and `SetParam` accept any value in [0, 1] | applied, the review's first fix: read as any integer-valued leaf, any binary32 in [0, 1], E4 outside, E3 only for non-numbers; a fraction plays by `>= 0.5f`, which equals `RoundHalfAwayI32 != 0` on [0, 1]; the compiler writes the value the document holds; a decompiled 0.3 compiles back; the property test needs no exception. Checked: `Schema.cpp` `Leaf()` checks range only; `CheckStat` checks order and canonical bits only; `test_property.cpp:127` draws every leaf in range. Compiler §2.2 and §3.7 need no amendment (7.1, 9.4, 11.3 #15, 11.1) |
| K2 | Unit test 15 contradicts §4.7's `Switch`, and no golden catches a per-event implementation | applied: test 15 is now "a call with the current setting changes no state"; an engine test applies `SetParam(87, 0)` and `(87, 1)` at one odd frame with G < 1, inside the hold and with `slow` set, at blocks {1, 7, 127}, against the render without them; §4.7 says the coalescing is the engine's; `limit_switch`'s pair is placed at G < 1 with the hold running, and it gains a `feedback.amount` edit there so `AmongEdits` adds its row-87 pair inside the hold. §9.2 corrected: `AmongEdits` adds no pair after a row-87 edit (`other(kDomainOutput, 87)` is null) (4.7, 9.2, 9.3, 9.4) |
| K3 | `limit_switch` inherits `limit_sustain`'s level and cannot reach G < F | applied: confirmed from `BuildSoftNotes` (dry at most 0.5, so G < F needs a wet over 2.0, against a +2.5 dBFS sum's 1.83); `limit_switch` takes `wet_trim_db` +12. The corpus's requirements bound single whole-render counters, so "G < F at the first Off" is pinned by two per-call counts folded from the existing locals when the switch is Off, `drainDryFrames` (and so `LimiterDrainDryFrames` ≥ 1, which also proves an On attack below F, since a drain only rises) and `LimiterDrainWetFrames` ≥ 1 for the second Off at G ≥ F (4.2, 6.1, 7.2, 9.2, 9.3, 11.3 #19) |
| K4 | `Reset` rebuilds before priming, so `Switch` sees the old gain and the drain counters count drains that never run | applied: confirmed at `Engine.cpp` `:737-743` (`b91b34d`) and `:681-690` (`main`); `limiter_.Prime()` runs before `Reset`'s `dirty_ = kAllParamDomains; RebuildDirty();`, and `Init`'s limiter call before its own rebuild; §4.7's lifecycle and §5.4's table corrected; `limit_switch`'s Exact load placed at G < 1 with `OffWhileLimiting` = 2 exactly, and an engine test, pin the order (4.7, 5.4, 6.1, 9.3, 9.4) |
| K5 | The floor row is counted differently from the other rows, and §12's "482 frames" is wrong | applied: simulated (§8.3): a single over at the floor reaches 1.0f on frame 173,843 counted inclusively from the over, as the other rows count; a burst gives 173,843 from its last over frame and 173,842 from the first frame after it, which is how the prototype counted. §4.4's row is now 173,843 with the convention stated; §4.7's sentence, §12 and §7.3's table corrected (draft 3 said "one frame longer" and "482"; the difference is 480); starting bits added to §4.7's table; test 4 uses them (4.4, 4.7, 9.4, 12) |
| K6 | Row 87 cannot exist without row 86, but §10 allows the limiter to land before T2 | applied, the review's second fix, since D7 is confirmed as "the next free revision": an L2 that lands before T2 adds 86 as a `Reserved` row, `global.tempo_glide`, with a display row like 85's, for T2 to make `Global`; the revision, `sinceRev` and `Package-change:` line then take the actual number. Checked: `TableIsContiguous` at `Engine.cpp:36`; Reserved rows are never stored (`Params.h:141-155`) (7.1, 10, 11.4 #11) |
| K7 | Pre-revision-10 sessions load flagged inexact, which §7.1 omits | applied, the review's first fix: stated, with `StateCodec.cpp:179` and `PluginProcessor.cpp:518`, `:532`; T2's row 63 does the same; no exemption added (7.1) |
| K8 | The `Package-change:` line's "no render changes for it" is false for `limit_off_hot` | applied, and extended: the line names `limit_off_hot`'s package set Off and its clamped render; §9.1 lists it in the predicted set with §9.3's float-dump check. Checking the gate further showed that with a bump the gate still attributes a package preset's changed render to its package (`golden_changes`, `sound_rev_gate.py:394-414`), and every package changes at revision 10, so four of §9.1's eight (`strum_marks`, `spillover_chain`, `lone_changes`, `midi_gate`, package presets in `golden.json`) are reported as their packages' too. Draft 3's "the renders that change are attributed to the engine" was wrong; §7.1's gate bullet and the line now say so (Summary, 7.1, 9.1, 10) |

### 8.2 [Q] Product

| # | Finding | Disposition (design §) |
|---|---|---|
| Q1 | D5's per-preset form is recorded as the owner's answer, but the owner never wrote it | applied: confirmed from the page's database (§7.1 above) and the owner's message. D5 is labelled "Change (no note); the form is proposed, awaiting the owner's confirmation" in the status block, the Summary, §7.1, §11.2, STATUS, the README row and the three dated notes; §12 and §7.1 above corrected; §11.5 Q1, first and "(owner, now)", sets out readings a–d and their consequences; row 87 is "held for the limiter's control" until the owner answers, and L2 does not start the switch's part before then (status, Summary, 4.7, 7.1, 10, 11.1, 11.2, 11.5, 12). *Reversed by the correction of 2026-10-10 (§7.5):* the owner had chosen the form in the session, which the review could not see; the labels are restated as the owner's decision, row 87 is the switch's, and §11.5 Q1 asks only what Off does |
| Q2 | The factory policy blocks the stated purpose and is filed under owner-confirmed rows | applied: a new owner decision, D14, recommended On for now with §9.5's four reasons, the declaration as its alternative; Q8 is "(owner, now)"; §11.3 #17 points at D14; the switch's clauses leave the Answer column of D6 and D8 and sit, with D9's, D10's and D13's, in Consequence as "pending D5" ("for D5" since §7.5's correction) or "pending D14"; §11.2's preamble says the Answer column holds only what the owner confirmed (9.5, 11.2, 11.3, 11.5) |
| Q3 | Loading an On preset with Trails after a clipping Off preset ducks the trails, and the dry on hot input | applied: confirmed from §4.3's attack and the whole-mix form; the review's example re-calculated (r = 0.180, the dry −2.87 dB). §4.5's Spillover row qualified and a new row added; §4.7's "Turning On" states the step; risk 12; a new golden, `limit_clip_trails` (an Off package settled and clipping on Saturation, then a Trails load of an On package with `limit_hot_mix25`'s settings, `LimiterDryFrames` > 0, all after the load); L2's A/B plays it. The mirrored "entry ramp" is specified and offered (Q9); draft 4 recommends the instant attack as designed, since it is D3's confirmed attack and the step only occurs on leaving a preset that clips by design, and the A/B decides (4.5, 4.7, 9.3, 11.1, 11.4, 11.5) |
| Q4 | The `clip`-class escape cannot be built as written, and deliberate-Off user presets always "fail" | applied: confirmed (`bspc_roundtrip.py:150` lints documents only; `--declarations` reaches only `bspc render`; `declare --class` takes attack or pad). The declaration is a flag, `ratings.py declare --clips`, beside `--self-oscillating`; `bspc lint` gains `--declarations AUDITION.md`, which `bspc_roundtrip.py` passes; the replaced checks are listed (Peak (stored), Ceiling (moved), S11's Ceiling) with what replaces them; for non-factory ids the switch Off is the declaration, and the report gives the clip share instead of failing. Built now only if D14 chooses the alternative (about 0.5 day) (7.1, 7.3, 9.5, 11.1, 11.2 D14) |
| Q5 | A stored Off cannot be seen on the pedal until it clips | applied: D6's consequence (for D5's switch; "pending D5" until §7.5's correction) hands the control-surface design two requirements, Clip shown from the load and clipping distinct from limiting; Q4 of §11.5 says "How", not "Whether"; the Rev7 LED double-blinks when the published switch goes to Clip, in both `led` modes; the plugin tags Clip presets in the Library list and the Modes menu; the lamp's CLIP caption shows from the load (7.3, 7.4, 11.2 D6, 11.4 #9, 11.5) |
| Q6 | "Off" misleads plugin users: it hard-clips inside the engine | applied: the row's states are **Limit** and **Clip** through a new display kind, `LimitClip`, beside `LiveMark`'s "Mark"/"Live" precedent, with typed text limit, clip, on, off and numbers, boolean to hosts; the row is titled "Output ceiling"; the Leaves view, L15's message, the lamp, the console's `stats` (`limiterMode limit\|clip`) use the same words; the JSON key and the 0/1 encoding stay, and this document keeps On and Off for the two values (1.4, 7.1, 7.3, 7.4) |

### 8.3 Checks run for draft 4

- **The floor row.** `limiter-d5/author/onpath.py` simulates §4.3's On path (gain, hold and
  flags) in binary32, each product rounded once from an exact binary64 product, with
  k = `0x3F800325`, k_s = `0x3F8000C9` and the tolerance `0x3F83BCD8`. A single over at each of
  §4.4's six gains reaches 1.0f on the frames §4.4 gives (601, 1,681, 3,481, 7,680, 14,159,
  33,715), counted inclusively from the over's frame. A single over at 2⁻²⁴ gives 173,843; a
  1,000-frame burst at 2⁻²⁴ gives 173,843 from its last over frame and 173,842 from the first
  frame after it.
- **The drain table** was re-run with `limiter-d5/drain.py` (§7.3): unchanged.
- **The dry dip on leaving a clipping preset** (design §4.5): r = 0.25/(0.891 + 0.25·2.0) =
  0.1797, G/F = 0.719, −2.87 dB (calculated).
- **The decisions page.** `answers/limiter-D5` read from the page's database (§7.1).
- **The tree,** at `claude/tempo-core` `b91b34d` unless noted: `compiler/src/Schema.cpp:551-574`
  and `:1319-1364`; `dsp/src/blob/Validate.cpp:402-414`; `compiler/tests/test_property.cpp:127`;
  `dsp/src/Engine.cpp:30-36`, `:700-743`, `:776-800`, `:1080-1092`, `:1190-1215`, and `main`'s
  `:636-690`; `dsp/include/brainscape/Params.h:141-173`; `dsp/tests/golden/Corpus.cpp:980-1023`
  and its `require` lists; `dsp/src/TestSignal.cpp:104-119`; `plugin/src/StateCodec.cpp:160-179`;
  `plugin/src/PluginProcessor.cpp:518`, `:532`; `plugin/src/BrainscapeParam.cpp:176-200`,
  `:310`; `dsp/src/ParamDisplay.cpp:30-32`, `:123-153`, `:429-433`;
  `tools/ci/sound_rev_gate.py:336-414`, `:469-512`; `dsp/tests/golden/golden.json`'s package
  presets; `tools/ci/bspc_roundtrip.py:140-155`; `tools/audition/ratings.py:15-45`, `:620-626`;
  `firmware/factory/README.md`; clock.md §10.4 and §11.1–§11.3.
- **Not run:** nothing new was built. The drain counts, the `LimitClip` kind, the entry ramp and
  the declaration are specified, not prototyped.

### 8.4 Where the probes live

`limiter-d5/author/onpath.py` in the session's scratch directory, not committed, beside
`limiter-d5/drain.py` (§7.4).
