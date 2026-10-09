# Daisy Seed Rev7 — silicon record, 2026-10-07

> What the first two sessions on the owner's Daisy Seed Rev7 ran and measured, and what each
> result settles in [determinism-profile.md](../determinism-profile.md) ("profile §N"),
> [grain-engine.md](../grain-engine.md) ("engine §N") and
> [firmware/README.md](../../../firmware/README.md) ("firmware §N"). The captures are committed
> in [`firmware/records/rev7-2026-10-07/`](../../../firmware/records/rev7-2026-10-07/README.md),
> whose README gives the command that re-checks each one; this record cites them as
> `session-1/<file>:<line>`, or `:<line>` in the log its paragraph names. Report figures are what `tools/hil/bench_report.py` and
> `parity_check.py` print from those logs (the README's commands reproduce them; session 1's
> bench report is committed beside its logs as `session-1/bench-all.md`). Every number
> here is **measured** (DWT cycles of the 480 MHz core) unless marked *estimated* or *derived*,
> as the profile marks its own.

---

## 1. The sessions in order

### 1.1 The board

Every hello line reports the same board: "Daisy Seed 1.2 / rev7 (PCM3060; PD5 strap)", CPUID
`0x411FC271`, IDCODE `0x20036450`, `sysclkHz` 480000000, boot region "QSPI flash", bootloader
"Daisy bootloader >= v6.1", app type `BOOT_QSPI`, libDaisy v9.0.0 (`08087203`), arm-none-eabi-gcc
10.3.1 20210824, `CCR` `0x00070200` (I- and D-cache on), the SDRAM's MPU region `RASR`
`0x03030033`, QSPI `CR` `0x01400001` and `CCR` `0x1F18EDEB`, `FPDSCR` `0x00000000`, and
`"lastFault": null` (`session-1/*.log:1`, `session-2/*.log:1`). Audio: 48,000 Hz, 48-frame blocks,
24 bits, "PCM3060 (hardware mode, reset on PB11)".

### 1.2 Session 1: sound revision 1, parity then the DWT pass

The images were built from `a91aa8ad64c7` (`"dirty": false`), engine archive
`4f4ddaa3583e46f2…`, hooks archive `da7b4f2e9b44aef7…`, FP flags hash `b5dd86f24f5deffa`
(`session-1/parity-rev7-run.log:1`).

1. **Parity** (`brainscape_parity`): `run`, `run pedal` and `run hostile` on one boot (hello
   `uptimeMs` 48,991, 310,325 and 571,637). Each rendered the whole revision-1 corpus, 13
   vectors and 28 presets, with 0 render failures and an `idle` line of `droppedBytes` 0
   (`session-1/parity-rev7-run*.log:44-45`). Verdict, each: `PASS - 28 preset(s) match
   golden.json bit for bit (sound revision 1, whole corpus)`; "rendered 639 s of audio in 240 s
   (2.66x realtime)".
2. **The DWT pass**, `run` on three images: `brainscape_bench` (engine code in ITCM,
   `session-1/bench-bench-ITCM.log`), `brainscape_bench_xip` (engine code and the firmware's
   `mem*` executing in place from QSPI, `bench-bench-XIP.log`) and `brainscape_bench_hooks` (the
   engine built with the guard's test hooks, `bench-bench-ITCM-hooks.log`). Each run is complete:
   every suite line arrived, no line is damaged, and `bench-end` reports `droppedBytes` 0 and
   `droppedLines` 0 (`:77`, `:77`, `:79`); the report flags no run INCOMPLETE. An earlier XIP run
   was interrupted; its partial log is not part of the record.

### 1.3 Session 2: sound revision 3, parity

The parity image built from `main` at `4090270` (hello: build `409027096f97`, `"dirty": false`),
engine archive `1d6fe1dc41f02fd9…`, hooks `023a9fa933fa9c0d…`, the corpus's 18 packages
compiled in (`session-2/parity-r3-run.log:1`). `run`, `run pedal` and `run hostile` on one boot
(`uptimeMs` 3,745, 303,403 and 603,157): each rendered 14 vectors and 33 presets with 0 render
failures, listed 18 packages, and ended with `droppedBytes` 0 (`session-2/parity-r3-run*.log:68-69`).
Verdict, each: `PASS - 33 preset(s) match golden.json bit for bit, 18 package(s) match MANIFEST
(sound revision 3, whole corpus)`; "rendered 709 s of audio in 276 s (2.57x realtime)".

The bench images were not run at revision 3, and neither session ran the live image.

### 1.4 CI: the emulated M7 at revision 3

CI's `parity-m7 (qemu-arm cortex-m7)` job passed on pull request #6 (run 37693138971, head
`78c8ba2`) and #7 (run 37701943741, head `aca8d9d`), and on `main` at `4090270`, the commit
session 2's image was built from (run 37700307648). Its log reports the archive
`libbrainscape_dsp.a 1d6fe1dc41f02fd9…` (the hash the device reports) under QEMU 10.2.3 at
commit `2e7e8b7e` and, in check mode against the revision-3 file, "every rendered
preset matches" at blocks of 48 and 512, the pattern {48, 1, 127, 32}, random pattern 1, from a
hostile caller and under the forced-flush control; the parity stream at `maxBlockSize` 48 in the
parity image's placement gives the same `PASS - 33 … 18 package(s) match MANIFEST` verdict, and
the package fuzzer's verdict digest (`9238138b…`) and the 15 frozen fixtures match. That covers
what the local emulated run left unfinished when Docker stopped (STATUS.md, "Internal sound
revision 3") but for three items CI does not run: 1-frame blocks, and the parity stream at
`maxBlockSize` 512 and from a hostile caller, which the Rev7 ran on the chip instead (§2). At
revision 1 the same job, on `eec5c7f` (a documentation commit on top of `a91aa8ad64c7`, run
37564718440), reports the archive `4f4ddaa3583e46f2…` that session 1's images carry.

## 2. Silicon parity

| Session | Revision | Command | `maxBlockSize` | Caller FP environment | Presets | Packages | Realtime |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | 1 | `run` | 512 | clean | 28 of 28 | — | 2.66× |
| 1 | 1 | `run pedal` | 48 | clean | 28 of 28 | — | 2.66× |
| 1 | 1 | `run hostile` | 512 | FZ, DN, round toward zero | 28 of 28 | — | 2.66× |
| 2 | 3 | `run` | 512 | clean | 33 of 33 | 18 of 18 | 2.57× |
| 2 | 3 | `run pedal` | 48 | clean | 33 of 33 | 18 of 18 | 2.57× |
| 2 | 3 | `run hostile` | 512 | FZ, DN, round toward zero | 33 of 33 | 18 of 18 | 2.57× |

Each render is one engine in the pedal's placement (Hot arena and Engine object in DTCM, Warm in
AXI SRAM, Bulk in SDRAM), engine code in ITCM, caches on, restarted with `LoadPreset(…, Exact)`
for every preset, events through the `EventQueue`, run in the main loop.

**What it settles.**
- The STM32H750 conforms (profile §3.8, "silicon pending"): the engine archive the emulated M7
  checks on every pull request renders the golden corpus bit for bit on the chip, at two
  revisions, at both block-size limits, and with the caller's FPSCR set to FZ, DN and round
  toward zero, which the guard neutralizes on silicon as under emulation (profile §4.1, §6.4).
- The revision-3 packages decode on the chip to `MANIFEST`'s hashes (the package rule,
  mode-compiler.md §8.3).
- Firmware §6's `info`, `fp`, `audio`, archive and parity items: FPDSCR is `0x00000000` in every
  hello, and FPSCR's mode bits are zero (`0x00000000` in each session's first parity hello; the
  `0x00000010` and `0x20000010` of the others are the cumulative IXC flag and the C condition
  flag). At both revisions the device's archive equals the one CI's `parity-m7` job built and
  ran (§1.4).
- Render speed (profile §6.6 *estimated* 1.3–3.1× realtime): 2.66× at revision 1 and 2.57× at
  revision 3.

**What it does not settle.** The renders run in the main loop, so the audio interrupt's FP
context (`FPDSCR` at an exception, profile §6.5) is not exercised; nor are SDRAM retention at
enclosure temperature, the errata review, or a HIL runner (profile §6.6). 1-frame blocks were not
rendered on the chip.

## 3. The DWT pass (session 1, sound revision 1)

### 3.1 Conditions

The budget is 480,000 cycles per 48-frame block, 10,000 cycles per sample (480 MHz / 48 kHz).
Interrupts are off around every measured call (and the cold-cache maintenance), so the figures are
the engine's alone: the audio interrupt, USB and the control loop come on top. Measurement
overhead is 1 cycle (`session-1/bench-bench-ITCM.log:2`). "Cold" cleans and invalidates the
D-cache and invalidates the I-cache before every block. Warm and cold renders of every
configuration produced identical output in all three logs.

The configurations are `firmware/bench/main.cpp` at `a91aa8ad64c7`: `default` (engine defaults,
`plucks_12s`, 10 s); `nominal` (engine §8's nominal row: 64 voices at unity rate, 20 ms grains,
every post stage in moderate use, `strums_16s`, 10 s); `pess_render` and `pess_births` (64 voices
at +24 st with full cents spread, reverse, spray, jitter, feedback 1.05, every post stage,
onset-triggered grains at marks, `onset_bursts_6s`, 6 s; 20 ms and 1 ms grains);
`pess_events` (`pess_render` under parameter sweeps every block, triggers, freeze toggles and
Spillover loads, `strums_16s`, 10 s; the queue refused 0 events, `:52`); and four corpus presets.
Revision 1 set onset grains and marks with rows 27 and 28; revisions 2 and 3 take them from a
package's mode (firmware §4), and the bench has not run on them.

### 3.2 Cycles per block, engine code in ITCM

From `session-1/bench-bench-ITCM.log:44-61`, in cycles per sample and as a share of the budget.

| Configuration | Cache | Mean | p99 | p99.9 | Max |
| --- | --- | --- | --- | --- | --- |
| `default` | warm | 1,902 (19.0 %) | 3,664 (36.6 %) | 4,096 (41.0 %) | 4,138 (41.4 %) |
| `default` | cold | 1,948 (19.5 %) | 3,708 (37.1 %) | 4,136 (41.4 %) | 4,183 (41.8 %) |
| `nominal` | warm | 7,774 (77.7 %) | 9,789 (97.9 %) | 9,885 (98.8 %) | 9,915 (99.1 %) |
| `nominal` | cold | 7,911 (79.1 %) | 9,854 (98.5 %) | 9,952 (99.5 %) | 10,028 (100.3 %) |
| `pess_render` | warm | 9,062 (90.6 %) | 12,699 (127.0 %) | 13,254 (132.5 %) | 13,554 (135.5 %) |
| `pess_render` | cold | 9,085 (90.9 %) | 12,753 (127.5 %) | 13,316 (133.2 %) | 13,616 (136.2 %) |
| `pess_births` | warm | 11,958 (119.6 %) | 15,300 (153.0 %) | 16,092 (160.9 %) | 16,847 (168.5 %) |
| `pess_births` | cold | 12,044 (120.4 %) | 15,386 (153.9 %) | 16,168 (161.7 %) | 16,897 (169.0 %) |
| `pess_events` | warm | 8,922 (89.2 %) | 11,812 (118.1 %) | 12,965 (129.6 %) | 14,631 (146.3 %) |
| `pess_events` | cold | 8,992 (89.9 %) | 11,879 (118.8 %) | 13,142 (131.4 %) | 14,786 (147.9 %) |
| `tail_post_fb` | warm | 3,165 (31.7 %) | 4,947 (49.5 %) | 5,391 (53.9 %) | 5,444 (54.4 %) |
| `tail_post_fb` | cold | 3,185 (31.8 %) | 4,950 (49.5 %) | 5,374 (53.7 %) | 5,432 (54.3 %) |
| `pitch_reverse_spray` | warm | 5,482 (54.8 %) | 8,008 (80.1 %) | 8,225 (82.3 %) | 8,464 (84.6 %) |
| `pitch_reverse_spray` | cold | 5,525 (55.2 %) | 8,037 (80.4 %) | 8,246 (82.5 %) | 8,479 (84.8 %) |
| `max_delay_spray_rev_up24` | warm | 2,501 (25.0 %) | 4,301 (43.0 %) | 4,766 (47.7 %) | 4,830 (48.3 %) |
| `max_delay_spray_rev_up24` | cold | 2,521 (25.2 %) | 4,320 (43.2 %) | 4,783 (47.8 %) | 4,826 (48.3 %) |
| `dense_1ms` | warm | 7,997 (80.0 %) | 10,805 (108.1 %) | 11,327 (113.3 %) | 11,863 (118.6 %) |
| `dense_1ms` | cold | 8,118 (81.2 %) | 10,898 (109.0 %) | 11,414 (114.1 %) | 11,975 (119.7 %) |

The hooks build agrees: every mean within 7 cycles per sample, every percentile and maximum
within 0.7 % (`bench-bench-ITCM-hooks.log:44-61`; its `nominal` cold maximum is 10,009, 100.1 %).

### 3.3 The worst-case budget is not met

Against the deadline (100 % of the block):

- **Over 100 % in their worst block:** `pess_render` (135.5 % warm, 136.2 % cold), `pess_births`
  (168.5 %, 169.0 %), `pess_events` (146.3 %, 147.9 %), `dense_1ms` (118.6 %, 119.7 %), and
  `nominal` with cold caches (100.3 %). `pess_births` is over on average (119.6 %, 120.4 %), and
  every one of these but `nominal` is over at p99.
- **Under 100 % throughout:** `nominal` warm (p99 97.9 %, p99.9 98.8 %, max 99.1 %), `default`
  (41.8 % at most), `tail_post_fb` (54.4 %), `pitch_reverse_spray` (84.8 %) and
  `max_delay_spray_rev_up24` (48.3 %).

These are the engine's cycles alone (§3.1); how much of each block the audio interrupt, USB and
the control loop need is among the decisions pending (§5).

`dense_1ms` is a corpus preset and the live image's 1 ms glitch slot, and `nominal` is engine §8's
nominal row, so the overrun is not confined to stress configurations. Silence does not help: the
pessimistic configuration (feedback 0.95) costs a mean 475,902 cycles per block over its 120 s
silent tail and peaks there at 14,251 cycles per sample, 142.5 % (14,269, 142.7 %, at FZ = 1;
`bench-bench-ITCM-hooks.log:77-78`). A fix is under design, with owner decisions pending; none of
it is in this tree. *(2026-10-09: the fix is [cpu-budget.md](../cpu-budget.md), owner-approved
with its decisions D1–D13; its steps 1–2, both bit-exact, are built.)*

### 3.4 Code placement: XIP against ITCM (profile §7.1)

The ratio of the XIP image's figure to the ITCM image's (`bench-bench-XIP.log` against
`bench-bench-ITCM.log`):

| Configuration | Warm mean | Warm p99.9 | Warm max | Cold mean | Cold p99.9 | Cold max | XIP max, warm / cold |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `default` | 1.076 | 1.099 | 1.148 | 1.697 | 1.489 | 1.534 | 47.5 % / 64.2 % |
| `nominal` | 1.137 | 1.130 | 1.135 | 1.355 | 1.304 | 1.303 | 112.5 % / 130.7 % |
| `pess_render` | 1.131 | 1.116 | 1.105 | 1.329 | 1.252 | 1.254 | 149.8 % / 170.7 % |
| `pess_births` | 1.109 | 1.095 | 1.090 | 1.259 | 1.216 | 1.200 | 183.6 % / 202.8 % |
| `pess_events` | 1.210 | 1.231 | 1.217 | 1.379 | 1.352 | 1.310 | 178.1 % / 193.7 % |
| `tail_post_fb` | 1.153 | 1.157 | 1.204 | 1.632 | 1.499 | 1.532 | 65.6 % / 83.2 % |
| `pitch_reverse_spray` | 1.066 | 1.078 | 1.082 | 1.319 | 1.289 | 1.284 | 91.6 % / 108.8 % |
| `max_delay_spray_rev_up24` | 1.045 | 1.043 | 1.072 | 1.540 | 1.398 | 1.428 | 51.8 % / 68.9 % |
| `dense_1ms` | 1.080 | 1.062 | 1.063 | 1.290 | 1.219 | 1.210 | 126.1 % / 144.9 % |

The stage suite's ratios are 1.101–1.147 (mean, warm) and the births suite's 1.053–1.127. Both
builds place `memcpy`, `memmove` and `memset` with the engine's code, so the ratio covers them.
**Settles** §7.1's requirement: the inner loops belong in ITCM, as every image but `_xip` already
places them. Executing in place multiplies the mean by 1.045–1.210 warm and 1.259–1.697 cold, and
puts the nominal row over its deadline (112.5 % warm).

### 3.5 Where the cycles go

**Per stage, by difference** (`pess_render` with one stage switched off, warm, 6,000 blocks each;
`session-1/bench-bench-ITCM.log:62-70`). All stages: mean 9,062, p99.9 13,266, max 13,566 cycles
per sample. The engine §8 column is the design's pessimistic row (*derived*).

| Switched off | ITCM, mean drop | XIP, mean drop | Engine §8, pessimistic |
| --- | --- | --- | --- |
| grains (1 voice instead of 64) | 6,549 | 7,481 | grain render ≈ 4,000 (r = 4) |
| pitch (r = 1 instead of 4) | 532 | 466 | — (the grain row scales with r: ≈ 1,600 at r ≈ 1) |
| reverb | 559 | 641 | 800 |
| post delay | 319 | 372 | ≈ 60 |
| filter | 200 | 414 | 70 |
| modulation | 192 | 278 | 60 |
| feedback taming | 68 | 89 | ≈ 200 |
| onset grains and marks | −855 | −897 | — |

Switching the onset grains and marks off made the block dearer, so the difference isolates no
cost there; the onset detector has no bypass and is inside every figure. Maxima fall in different
blocks of each render, so no per-stage maximum is derived.

**Per birth, an upper bound** (engine §8's `ScheduleGrain` row; `bench-bench-ITCM.log:71-74`). Equal voices, 1 ms
against 20 ms grains:

| Voices | Births per block, 1 ms / 20 ms | Mean, 1 ms / 20 ms (cycles per sample) | Cycles per birth | Birth cost at 1 ms (cycles per sample) |
| --- | --- | --- | --- | --- |
| 48 | 48.0 / 2.4 | 12,331 / 6,439 | ≤ 6,203 | ≤ 6,203 |
| 16 | 16.0 / 0.8 | 4,215 / 2,997 | ≤ 3,846 | ≤ 1,282 |

In the XIP image: ≤ 6,313 and ≤ 4,308 cycles per birth. The 1 ms grains also read the ring at a
new spray position every birth, so the bound includes that locality. Engine §8 derived about 530
cycles per sample for `ScheduleGrain` at 1 ms grains, and profile §3.9 about +300 for DetMath at
one birth per sample (*estimated*).

### 3.6 `Restart`, loads, `Init` and the SDRAM clear (profile §5.8, §8.3 Q9)

`session-1/bench-bench-ITCM.log:36-43` and `bench-bench-XIP.log:36-43`:

| Operation | State | ITCM cycles | ITCM ms | XIP ms |
| --- | --- | --- | --- | --- |
| `Restart` | after 2 s of rendering | 22,645,866 | 47.18 | 47.28 |
| `Restart` | again, nothing rendered since (skips the clears) | 9,279 | 0.02 | 0.08 |
| `LoadPreset(Exact)` | after 2 s of rendering | 22,640,272 | 47.17 | 47.29 |
| `Reset` | after 2 s of rendering | 20,487 | 0.04 | 0.13 |
| `ClearHistory` | after rendering | 22,622,276 | 47.13 | 47.14 |
| raw clear of 16 MiB of SDRAM | the firmware's `memset` | 21,586,568 | 44.97 | 44.98 |
| raw clear of 16 MiB of SDRAM | an 8-register `STM` loop (the core's floor without DMA) | 21,585,334 | 44.97 | 44.97 |
| `Init` | canonical configuration, `maxBlockSize` 48 | 24,065,117 | 50.14 | 50.39 |

A dirty `Restart`, and so an Exact load, takes 47.2 ms, of which the 16 MiB clear at the core's
own floor is 44.97 ms; the firmware's `memset` runs at that floor. The profile estimated 45–160 ms
(floor 44 ms, *calculated*). On an engine that has rendered nothing since, `Restart` skips the
clears and costs 9,279 cycles, less than `Reset`'s 20,487, as profile §5.8 intends.

### 3.7 Subnormals (profile §4.2, §8.3 Q1)

**Test (a), dependent chains** (`bench-bench-ITCM.log:4-29`, cycles per instruction):

| Chain | FZ = 0 | Same program at FZ = 1 |
| --- | --- | --- |
| `vmul`, normal | 3.00 | 3.00 |
| `vmul`, subnormal operand and result | 3.00 | 3.00 |
| `vmul` pair, normal | 3.00 | 3.00 |
| `vmul` pair, every other result subnormal (underflow) | 3.01 | 3.00 |
| `vadd`/`vsub` pair, normal | 3.01 | 3.00 |
| `vadd`/`vsub` pair, subnormal operands and results (nonzero addend) | 3.00 | 3.00 |
| `vdiv`, normal / subnormal dividend and result | 18.00 / 18.00 | 18.00 / 4.00 |
| `vsqrt`, normal / subnormal operand | 14.00 / 14.00 | 14.00 / 1.00 |

At FZ = 0 a subnormal costs the M7 nothing: the report's penalty is 1.00× for `vmul` with
subnormal operands, for an underflowing `vmul` pair and for a subnormal `vadd`/`vsub` pair, in all
three images. The FZ = 1 column times zero arithmetic (FZ flushes the operands), hence the faster
`vdiv` and `vsqrt`.

**Test (b), silent tails, and the §4.2 rule** (`bench-bench-ITCM-hooks.log:75-78`; the silent tail
is 120,000 blocks in each):

| Tail | Silent-tail max, FZ = 0 / FZ = 1 (cycles per sample) | Ratio (rule ≤ 1.2) | Blocks raising IDC or UFC at FZ = 1 (rule ≤ 0.5 %) | Render against the rule's reference | Verdict |
| --- | --- | --- | --- | --- | --- |
| golden `strums_tail_123s/tail_post_fb` | 5,438 / 5,426 | 1.002 | 0 of 120,000 (0.000 %) | FZ = 0 and FZ = 1 both equal `golden.json` (`2227acb0df4ff2b8`) | keep gradual underflow |
| 2 s noise, then 120 s of silence, pessimistic, feedback 0.95 | 14,251 / 14,269 | 0.999 | 0 of 120,000 (0.000 %) | FZ = 1 equals FZ = 0 (`a11b5941da389289`) | keep gradual underflow |

**Settles** Q1 and §4.2's rule on revision 1's engine: keep gradual underflow, as decided; no
wider flush is needed. The FZ = 0 census is also 0, which Armv7-M under-reports (firmware §4).

### 3.8 The flush idiom (profile §4.3, §8.3 Q5)

`bench-bench-ITCM.log:30-35`, cycles per update; the per-sample figure multiplies the measured per-site cost by the
profile's 45 sites (*derived* count):

| Form | 8 independent chains (lower bound) | 1 recursive chain (upper bound) | × 45 sites per sample |
| --- | --- | --- | --- |
| none | 2.14 | 6.05 | — |
| bit test (the engine's `FlushTiny.h`) | 4.16 (+2.02) | 10.08 (+4.03) | 91–181 (0.9–1.8 %) |
| two compares (the form §4.3 writes) | 12.29 (+10.15) | 14.04 (+7.99) | 359–457 (3.6–4.6 %) |

**Settles** Q5 for the forms measured: the bit test the engine already uses is the cheaper form on
the M7 too, at 0.9–1.8 % of the budget against the profile's *estimated* 1.5–2.5 %.

### 3.9 Contraction off and explicit FMA (profile §7.3, §8.3 Q2)

The report's verdict, from the ITCM log: the worst warm pessimistic block is 168.5 %
(`pess_births`, p99.9 160.9 %), 169.0 % cold, against the profile's *estimated* 77–78 % for the
pessimistic row. "**Verdict (§7.3 rule):** contraction-off THREATENS the budget at 64 voices (a
block over its deadline): explicit FMA in the inner loops is a candidate; build and measure it
before adopting." No build with contraction on or with explicit FMA exists, so the pass measures
the contraction-off cost only, as part of every figure above. Profile §7.2 *estimated*
contraction off at about +430 cycles per sample on the pessimistic row; against blocks of
13,554–16,847 cycles per sample, explicit FMA is a candidate inside a larger fix, not the fix.

### 3.10 Memory placement and image sizes

From each image's hello (`session-1/hello-bench-*.json`, `session-1/parity-rev7-run.log:1`,
`session-2/parity-r3-run.log:1`), bytes:

| | `bench` r1 | `bench_xip` r1 | `bench_hooks` r1 | `parity` r1 | `parity` r3 |
| --- | --- | --- | --- | --- | --- |
| Image in QSPI at `0x90040000` | 220,692 | 220,552 | 221,068 | 223,312 | 337,896 |
| `.text` in QSPI | 174,600 | 218,244 | 174,608 | 177,080 | 280,304 |
| `.data` / `.bss` (AXI SRAM) | 1,572 / 37,792 | 1,572 / 37,792 | 1,572 / 37,800 | 1,572 / 23,368 | 1,572 / 23,368 |
| DTCM arenas | 33,792 | 33,792 | 33,792 | 33,792 | 34,816 |
| Hot arena (DTCM) | 24,576 at `0x20002400` | same | same | same | 24,576 at `0x20002800` |
| Engine slot (DTCM) | 7,168 at `0x20000800` | same | same | same | 8,192 at `0x20000800` |
| AXI arenas; Warm arena | 173,056; 139,264 at `0x24008400` | same | same | same | same |
| SDRAM arenas; Bulk arena | 18,941,792; 17,825,792 at `0xC0110760` | same | same | 17,825,792; 17,825,792 at `0xC0000000` | same as r1 |
| Heap (SDRAM) | 48,167,072 | 48,167,072 | 48,167,072 | 49,283,072 | 49,283,072 |
| `memset` runs in | ITCM | QSPI | ITCM | ITCM | ITCM |

The sizes equal firmware §7's table (revision 1's in brackets there, revision 3's parity image).
`PlanMemory` at `maxBlockSize` 48 asks, at revision 1, Hot 16,768 of its 24,576-byte arena,
Warm 129,680 of 139,264, Bulk 17,545,216 of 17,825,792 and the Engine object 6,656 of its 7,168-byte
slot (`session-1/bench-bench-ITCM.log:3`). The heap's break, against firmware §7's 250 KiB
measured under QEMU: at revision 1 it stood at 224,904 bytes after the first whole-corpus parity
run and at 290,856 after the second and third (`session-1/parity-rev7-run.log:45`,
`parity-rev7-run-pedal.log:45`, `parity-rev7-run-hostile.log:45`); at revision 3 at 290,856
after the first and after each later run (`session-2/parity-r3-run*.log:69`).

## 4. What the record settles in the design

| Where | Was | Now |
| --- | --- | --- |
| Profile §3.8, §6.6: the M7 on silicon | pending | conforms at revisions 1 and 3, three configurations each (§2) |
| Profile §4.2, §8.3 Q1: subnormal cost and the rule | unknown; DWT tests (a) and (b) | no penalty at FZ = 0; the rule passes on both tails: keep gradual underflow (§3.7) |
| Profile §4.3, §8.3 Q5: the flush | *estimated* 150–250 cycles per sample; cheaper form open | the engine's bit test, 91–181 (0.9–1.8 %), cheaper than the two compares (§3.8) |
| Profile §5.8, §8.2 risk 10, §8.3 Q9: `Restart` | *estimated* 45–160 ms | 47.2 ms, the clear at the core's floor (§3.6); the watermark and the default load mode stay open |
| Profile §7.1: ITCM or XIP | requirement, unmeasured | ITCM confirmed: XIP multiplies the mean by 1.045–1.210 warm, 1.259–1.697 cold (§3.4) |
| Profile §7.2, §8.2 risk 2; engine §8: the budget | *estimated* 32–41 % nominal, 77–78 % pessimistic | measured 77.7 % mean and 99.1–100.3 % worst nominal, 135.5–169.0 % worst pessimistic: **not met** (§3.2, §3.3) |
| Profile §7.3 rule | deferred to DWT | contraction off threatens the budget: explicit FMA is a candidate, to build and measure before adopting (§3.9) |
| Profile §8.3 Q2 | open | still open: no contraction-on or FMA build; births bounded at ≤ 6,203 cycles each, which does not decide kernels against tables (§3.5) |
| Profile §6.6: render speed | *estimated* 1.3–3.1× | 2.66× (r1), 2.57× (r3) (§2) |
| Firmware §6 checklist | open | `info`, `fp`, `audio`, archive, parity, bench and the long-render USB check done; the live items open |

## 5. What stays open

- **The CPU budget.** Five configurations exceed the deadline in their worst block, and the
  nominal row comes within 1 % of it with warm caches (§3.3). A fix is under design, with owner
  decisions pending (among them how much of each block the interrupts, USB and the control loop
  keep); none of it is in this tree, and it will need its own bench session. *(2026-10-09: the
  fix is [cpu-budget.md](../cpu-budget.md), owner-approved; the interrupts, USB and the control
  loop keep 15 % of each block (its D1); steps 1–2, bit-exact, are built; its bench session 2 is
  planned in its §8.)*
- **The bench at revision 3.** Revisions 2 and 3 changed the engine (modes, the Mix law); the
  figures above are revision 1's.
- **The live image** (firmware §6): pass-through, switches, structures, macros, the `stats`
  meter, clicks while typing.
- **Profile §8.3 Q2** (explicit FMA, kernels or tables) and **Q9**'s watermark and default load
  mode; the interrupt `FPDSCR` test (profile §6.5), the SDRAM march test and hot soak and the
  HIL runner (profile §6.6).
