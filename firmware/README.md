# Brainscape firmware: Daisy Seed Rev7 bring-up

Bring-up firmware for the owner's **Daisy Seed Rev7** (STM32H750, Cortex-M7 at 480 MHz, 64 MB
SDRAM, 8 MB QSPI flash, PCM3060 codec, micro-USB): the first part of merged step 5
([docs/STATUS.md](../docs/STATUS.md); [determinism-profile.md](../docs/design/determinism-profile.md)
§8.4 step 13; [companion-app.md](../docs/design/companion-app.md) §8 phase D). It is a test bench,
not the pedal firmware: no controls, no presets on SD, no device link.

| Image | `.bin` | What it does | Host tool |
| --- | --- | --- | --- |
| **A, parity** | `brainscape_parity.bin` | Renders the golden corpus on the Seed exactly as the golden harness does, from the corpus's packages compiled in, and streams every hash | [`tools/hil/parity_check.py`](../tools/hil/parity_check.py) |
| **B, bench** | `brainscape_bench.bin` | The DWT measurement pass: cycles per block (with and without a stream of events), per stage, an upper bound per birth, `Restart` against the SDRAM's own floor, subnormal latency, the flush idiom, silent tails | [`tools/hil/bench_report.py`](../tools/hil/bench_report.py) |
| B, code-placement A/B | `brainscape_bench_xip.bin` | Image B with the engine's code (and the firmware's `memcpy`/`memmove`/`memset`) executing in place from QSPI instead of ITCM (profile §7.1) | `bench_report.py` |
| B, flush A/B | `brainscape_bench_hooks.bin` | Image B on the engine built with the FP guard's test hooks: the FZ = 0/1 silent-tail test and flag census of profile §4.2 | `bench_report.py` |
| **C, live** | `brainscape_live.bin` | The engine in the audio callback on the Seed's codec, presets, modes, macros and parameters over USB | [`tools/hil/console.py`](../tools/hil/console.py) |

Every image links the engine archive `libbrainscape_dsp.a` exactly as CI's `parity-m7` job builds,
audits and renders it under QEMU (same toolchain, flags and deterministic archive: SHA-256
`ea7c5dfb08cf44e3…` at sound revision 7, `1d6fe1dc41f02fd9…` at revision 3, `89b73b51fe4261bf…` at
revision 2, `4f4ddaa3583e46f2…` at revision 1; `firmware.yml` checks the two are
byte-identical), reports that hash in its hello line, and the firmware build writes it to
`build/fw/firmware/engine-archives.sha256` for the host tools' `--expect-archive`.

**Sound revision 2** ([mode-compiler.md](../docs/design/mode-compiler.md) §7). The engine plays
compiled modes: the onset trigger and mark positioning are a mode's structure (rows 27 and 28
are retired), macro and expression-pedal moves are events, a mode switch is a Trails or FastCut
Spillover load, and the trim and the effect volume scale only the wet signal. The golden corpus
(version 7, `golden.json` re-minted for revision 2) takes its structure from 18 committed packages
(`dsp/tests/golden/presets/*.bsp`, listed in its `MANIFEST`), which every image carries compiled
in, since the Seed has no file system ([`EmbeddedPackages.h`](../dsp/tests/golden/EmbeddedPackages.h),
generated from the committed bytes at build time): the parity image renders the corpus from
them, and the live and bench images take their modes from them.

**Sound revision 3** (the Mix law, [mode-compiler.md](../docs/design/mode-compiler.md) §7.1)
changes only how Mix blends: the dry stays at unity up to the knob's middle and the wet is at
unity from it, instead of a linear crossfade, so `set mix 0` is still the clean pass-through and
`set mix 1` the wet alone. The corpus is version 8, `golden.json` re-minted for it, and its
packages are re-stamped (their sound and control hashes unchanged). At revision 3 the five images
build and their engine archives equal the M7 oracle's, built on Windows and on Linux alike. The
emulated checks of §9 (the golden check and the parity stream under `qemu-arm`) were cut off
part-way locally when Docker stopped, every hash that arrived matching the file, and lane F's
review then ran them in full, 1-frame blocks included. CI's `parity-m7` job passed the golden
check at blocks of 48 and 512, {48, 1, 127, 32}, random pattern 1, from a hostile caller and under
the forced-flush control, and the parity stream at `maxBlockSize` 48 in the image's placement, on
pull requests #6 and #7 and on `main` at `4090270`, with the same archive `1d6fe1dc…`. The Rev7
ran the stream at 512 and from a hostile caller on the chip (§6); 1-frame blocks have run in full
on the M7 at revision 3 only under emulation, in lane F's run.

**Sound revisions 4–7** (wave 1, [mode-compiler.md](../docs/design/mode-compiler.md) §7.5) add
the mode vocabulary's first wave: trigger sources (a mode can leave out the free-running
scheduler, the footswitch or MIDI notes), bursts and intermittency (4), pitch sets (5), micro-loop
repeat and decay (6) and a voice count (7), with six new leaves the console names (`repeat`,
`decay`, `voices`, `skip`, `burst`, `spacing`). The corpus is version 12: 45 presets and 29
packages, every package compiled into the images. Revision 6 grew the grain pool, so
`sizeof(Engine)` is 8,192 bytes and the DTCM engine slot 9 KiB. At revision 7 the five images build
and their engine archives equal the M7 oracle built on Windows and on Linux (`ea7c5dfb08cf44e3…`,
hooks `959493f9b4cf03f2…`), and the emulated checks of §9 pass at each of revisions 4 to 7: the
golden check, the parity stream in the parity image's placement, the package fuzzer and the frozen
fixtures under `qemu-arm -cpu cortex-m7`. Lane F's review moved `DecayGain` to a 32-bit age, so the
engine imports no soft-float helper (`__aeabi_ul2f` had pulled 540 bytes of libgcc's `float`
arithmetic into the parity and bench images' ITCM at revisions 6 and 7 as first built).

**On the board** (2026-10-07). Two sessions on the owner's Rev7: at revision 1 the parity image
and the three bench images, at revision 3 the parity image. Every parity run passed, and the
bench found the worst-case CPU budget not met (§4, "Session 1"). The captures, with the commands
that re-check them, are in [`records/rev7-2026-10-07/`](records/rev7-2026-10-07/README.md), and
what they settle in
[docs/design/reviews/rev7-silicon-record.md](../docs/design/reviews/rev7-silicon-record.md).
The live image has not run yet, and no image at revisions 4 to 7 has run on the board.

## 1. What to flash, in order

Everything goes through Electrosmith's web programmer, **<https://flash.daisy.audio>** (WebUSB:
Chrome or Edge; Firefox and Safari have no WebUSB). No `dfu-util` is needed.

**App type: `BOOT_QSPI`.** Every image is 328–368 KiB, past the 128 KiB internal flash, so the images
run from the 8 MB QSPI flash and are loaded by the **Daisy bootloader**, which lives in the internal
flash. `grain-engine.md` §7 plans the pedal this way. The engine's code and constants, the 64-bit
division helpers it calls and the firmware's `memcpy`/`memmove`/`memset` are copied into the 64 KiB
ITCM at boot, as profile §7.1 asks for the inner loops; everything else executes in place from QSPI
behind the 16 KiB I-cache. That excludes the engine archive's members that never run in the audio
path: the package decoder, encoder and SHA-256 (a producer decodes a package before it stages the
state) and the test-signal generator (the harness's input). At sound revision 2 the archive's
linked code would not fit the 64 KiB with them (the parity image needed 76,871 bytes). The
engine's shared tables (C++17 inline variables such as `kParamTable`) go to ITCM whichever object
supplied the copy the linker kept, which is often a golden-harness object's, not the archive's
(`_bs_engine_comdat` in [`CMakeLists.txt`](CMakeLists.txt), checked by
[`cmake/ItcmCheck.cmake`](cmake/ItcmCheck.cmake)).

**Two DFU devices.** In DFU mode the Seed shows up as one of two USB devices with the same ID
(`0483:DF11`), and the web page's device picker tells them apart by name:

| Device name | What it is | How you get there | Writes to |
| --- | --- | --- | --- |
| **DFU in FS Mode** | the STM32's built-in ROM bootloader | **hold BOOT, press and release RESET, release BOOT** (LED dark) | internal flash, `0x08000000`: where the Daisy bootloader lives |
| **Daisy Bootloader** | the Daisy bootloader v6.4 you install once | **press RESET, then press BOOT while the LED breathes** | QSPI flash: the image at `0x90040000` |

Pick "DFU in FS Mode" only to install the bootloader, and "Daisy Bootloader" for every image. An
image uploaded to "DFU in FS Mode" overwrites the Daisy bootloader (and does not fit the 128 KiB):
after that nothing boots and the LED never breathes. **Recovery:** do the "Once" steps below again;
the images already in QSPI are untouched.

### Once: install the Daisy bootloader v6.4

1. Connect the Seed's micro-USB port to the computer with a **data** cable.
2. Put the STM32 into its ROM DFU mode: **hold BOOT, press and release RESET, release BOOT**.
   The user LED stays dark.
3. On flash.daisy.audio click **Connect** and pick **"DFU in FS Mode"**.
4. Open the **Bootloader** tab and set **Bootloader version** to **v6.4**: the plain one, with
   **DFU USB Port: Built-in** and **Timeout: 2000ms**. The page's default is v5.4: do not use it.
   Under a bootloader older than v6.0, libDaisy's `DaisySeed::Init` skips its own clock, LED and
   SDRAM setup (it expects the old bootloader to have done it), so the images would not run at
   480 MHz as measured here, and the `dfu` command below needs v6's boot-info handshake. Not
   "external" (that is the USB on pins D29/D30), not "short timeout".
5. Click **Flash**. When it is done the Seed restarts into the Daisy bootloader: after every reset
   the user LED now **breathes** for about 2 s (the bootloader's grace period).

The same bootloader binary also sits beside the images in the build directory
(`dsy_bootloader_v6_4-intdfu-2000ms.bin`, copied from the pinned libDaisy) for anyone flashing
without the web page.

### Each image

1. Put the Seed in the Daisy bootloader's DFU mode: press **RESET**, then press **BOOT while the LED
   is breathing** (a few rapid blinks acknowledge: the bootloader now waits indefinitely). Do not
   hold BOOT while pressing RESET: that is the ROM's gesture. If one of these images is already
   running you can skip the buttons: send it `dfu`
   (`python tools/hil/console.py --port auto dfu`) and it reboots into the waiting bootloader.
2. On flash.daisy.audio click **Connect** and pick **"Daisy Bootloader"** (never "DFU in FS Mode").
3. Open the **File Upload** tab, **Choose or drag a file**: the image's `.bin`, then **Flash**.
   There is no address to type: with the Daisy bootloader connected, the page starts the write at
   `0x90040000`. Its source (`dfu-util.js` in electro-smith/Programmer, read while writing this)
   moves the start of the first writable segment of the bootloader's memory map,
   `@Flash /0x90000000/64*4Kg/0x90040000/…`, from `0x90000000` to `0x90040000`; the page shows that
   memory map but not the start address. The command-line equivalent, address explicit (libDaisy's
   own `make program-dfu`): `dfu-util -a 0 -s 0x90040000:leave -D brainscape_parity.bin`.
4. The bootloader checks the image and starts it. **After the first image, confirm where it
   landed:** `python tools/hil/console.py --port auto info` must show boot region **"QSPI
   flash"** and bootloader **"Daisy bootloader >= v6.1"** (an image written elsewhere does not
   start at all).

Flash order for a bench session: `brainscape_parity.bin` → run the parity check →
`brainscape_bench.bin` → `brainscape_bench_xip.bin` → `brainscape_bench_hooks.bin` (each with
`bench_report.py`) → `brainscape_live.bin`. Session 1 (2026-10-07, revision 1) ran the first
four; session 2 (revision 3) the parity image alone. Commit each session's logs under
[`records/`](records/rev7-2026-10-07/README.md), as captured.

**Troubleshooting.** Windows: if Connect lists nothing in DFU mode, the STM32 DFU device has no
WinUSB driver bound; Electrosmith's documentation covers binding one (Zadig). Linux: WebUSB needs
a udev rule granting access to `0483:df11`. A charge-only cable shows no device at all.

### What the LED says

| Pattern | Meaning |
| --- | --- |
| Breathes for about 2 s after a reset | The Daisy bootloader's grace period (press BOOT now to stay in it) |
| **SOS** | The Daisy bootloader refused the image (usually a `BOOT_NONE` binary) |
| A short blink once a second | An image is idle, waiting for a command |
| Fast blink (5 Hz) | Rendering or measuring |
| On | A parity run finished |
| A flash on each detected onset | The live image is running |
| **Three quick flashes, a pause**, forever | `Fatal()` in the main loop: the image sent `{"type":"error"}` and still answers on USB |
| **Two long flashes, a pause**, for about 10 s, then a reset | A **hardware fault** (HardFault, MemManage, BusFault, UsageFault, NMI or an unhandled interrupt) or `Fatal()` inside an interrupt handler: the fault record is saved and the next boot's hello line reports it as `lastFault` |
| Frozen | A hang, or a fault inside libDaisy's `DaisySeed::Init` (its own handlers are in force there): reset, then `info` |

After a two-long-flash reset the USB port disappears and comes back; the host tools stop with "the
serial port went away". Reconnect and run `python tools/hil/console.py --port auto info`: the
`lastFault` line gives the kind, the image, the time since boot, the faulting `pc` and `lr`, and the
fault status registers (`CFSR`, `HFSR`, `MMFAR`, `BFAR`), once; a hello with `"lastFault":null`
means the previous run ended without a fault.

## 2. Finding the serial port

Every image enumerates as libDaisy's CDC device: **VID `0483`, PID `5740`**, product
"Daisy Seed Built In". Windows 10/11 binds its in-box driver: **Device Manager → Ports (COM &
LPT) → "USB Serial Device (COMn)"**, or in PowerShell:

```powershell
Get-PnpDevice -PresentOnly | Where-Object InstanceId -like 'USB\VID_0483&PID_5740*' | Select FriendlyName
```

Linux: `/dev/ttyACM0` (your user needs the `dialout` group); macOS: `/dev/cu.usbmodem…`. All three
tools accept **`--port auto`**, which finds the port by VID/PID (Windows registry, Linux sysfs;
macOS takes the first `cu.usbmodem`). The tools are stdlib-only Python 3: no pyserial. Baud rate
does not matter.

## 3. Image A: the silicon parity check

```text
python tools/hil/parity_check.py --port auto --save parity-rev7.log --expect-archive build/fw/firmware/engine-archives.sha256
```

The tool asks the device for its hello line (board, clock, FP registers, caches, archive hash,
last fault), sends `run`, and prints each preset as it arrives. The device renders all 14 vectors
and 33 presets (709 s of audio) offline in its main loop, never in the audio callback: one engine in
the pedal's memory placement (Hot arena and Engine object in DTCM, Warm in AXI SRAM, Bulk in SDRAM),
restarted with `LoadPreset(…, Exact)` for every render, 48-frame blocks, the integer test signal as
input, events through the engine's `EventQueue`, the packages decoded from the image's own copies —
the golden harness's own code
([`dsp/tests/golden/ParityStream.h`](../dsp/tests/golden/ParityStream.h)), not a copy. On the Rev7 at
revision 3 it rendered the 709 s of audio in 276 s, 2.57× realtime (the profile estimated
1.3–3.1×); the stream carries DWT cycles per preset, so the tool prints the measured realtime
factor too. It ends with

```text
VERDICT: PASS - 33 preset(s) match golden.json bit for bit, 18 package(s) match MANIFEST (sound revision 3, whole corpus)
```

or `FAIL` with the first differing second of every preset that differs (exit 1). Before the
vectors the stream lists every package the corpus loads, as the image decoded it (its package,
sound and control hashes): they must be exactly `dsp/tests/golden/presets/MANIFEST`'s, and every
preset that starts from a package must name it with the sound and control hashes `golden.json`
records (mode-compiler.md §8.3, the package rule). The tool checks the stream as strictly as the
hashes: every line is numbered (format `brainscape-parity-stream/3`), and a gap, a line that is
not a JSON object or lacks a key, a preset, vector or package count that disagrees with
`parity-end`, a `resync` notice from the device's USB serial, or a missing or nonzero
`droppedBytes` on the device's final `idle` line gives `VERDICT: FAIL (TRANSPORT)` (exit 2): the
stream was damaged on the way, which says nothing about parity; run it again. It also fails a
run whose header is not the configuration the command asked for, and, with `--expect-archive`, a
device whose engine archive is not the one the build hashed. A revision-1 parity image (format
`/2`) fails the format check: flash the image built from this tree.

Then repeat in the other two configurations worth having on silicon:

```text
python tools/hil/parity_check.py --port auto --run "run pedal"   --save parity-rev7-pedal.log
python tools/hil/parity_check.py --port auto --run "run hostile" --save parity-rev7-hostile.log
```

`pedal` renders with the live engine's `maxBlockSize` of 48 instead of the harness's 512;
`hostile` sets FZ|DN and round-toward-zero in the caller's FPSCR around every engine call, which
the guard must neutralize (profile §6.4), and checks the guard hands the word back. `quick` skips
the three long vectors, `only=VECTOR[/PRESET],…` selects. A saved log can be checked again any
time: `parity_check.py --log parity-rev7.log --run "run"`.

## 4. Image B: the DWT measurement pass

```text
python tools/hil/bench_report.py --port auto --save bench.log --markdown bench.md
```

`run` takes about 10–15 minutes (`--run "run quick"` skips the two-minute silent tails). Every
number is DWT cycles of the 480 MHz core with **interrupts off around the measured call** (and, for
cold-cache blocks, around the cache maintenance before it); the budget is 10,000 cycles per sample,
480,000 per 48-frame block. Every `bench-begin` line records the state the numbers were taken
under: `SCB->CCR` (I- and D-cache), every MPU region (libDaisy's SDRAM region: write-back, no write
allocate), the QSPI controller's `CR`/`DCR`/`CCR` (prescaler and mode, which decide the XIP figures)
and where `memset` runs. The suites (each also a command):

| Suite | What | Decides |
| --- | --- | --- |
| `memory` | `PlanMemory` against the arenas, and the linker map | — |
| `micro` | Dependent `vmul`/`vadd`/`vsub`/`vdiv`/`vsqrt` chains with normal and subnormal operands and results at FZ = 0, including a `vadd`/`vsub` pair whose addend is a nonzero subnormal (the `+0` case may take a zero-operand shortcut); the same programs at FZ = 1, which flushes the operands to zero (so that column times zero arithmetic, not subnormals under FZ); the engine's flush idiom (`FlushTiny`) against no flush and the two-compare form, on 8 independent one-poles (instruction-level parallelism hides the flush: a lower bound) and on 1 recursive one-pole (the flush on the critical path every sample: an upper bound) | profile §4.2 test a, §8.3 Q1, Q5 |
| `restart` | `Restart` after rendering (the 16 MiB ring, post delay and reverb clear) and on a clean engine, `LoadPreset(Exact)`, `Reset`, `ClearHistory`, `Init`, and a raw 16 MiB SDRAM clear through the firmware's `memset` and through an 8-register `STM` loop (the core's floor without DMA) | §8.3 Q9, the `Restart` half: the watermark (§5.8) does not exist yet |
| `blocks` | Cycles per block, mean/p50/p90/p99/p99.9/max, **warm and cold caches** (cold: D-cache cleaned and invalidated and I-cache invalidated before every block, interrupts off), for engine defaults, the design's nominal row, the pessimistic configuration (64 voices at +24 st with full cents spread, reverse, spray, jitter, feedback 1.05, every post stage, and the mode of the corpus package `strum_marks`: onset grains at marks) with 20 ms and 1 ms grains, **`pess_events`** (the pessimistic configuration under parameter sweeps every block — cutoff, pitch, grain size — four footswitch triggers every 250 ms, a freeze toggle every 1.5 s and a Spillover load every second, each a mode switch, alternately a FastCut to the nominal preset in the default mode and Trails back, all through the `EventQueue`), and four corpus presets (`dense_1ms` with its package's mode) | grain-engine.md §8, profile §7.2; §7.3's explicit-FMA rule (below) |
| `stages` | The pessimistic configuration with one stage switched off at a time (the onset grains and marks by loading the default mode instead): each stage's mean cost by difference (the engine has no per-stage counters, and none were added: that would be instrumentation in `dsp/`) | where the cycles go |
| `births` | 48 and 16 voices with 1 ms vs 20 ms grains: equal voices rendered per block, 48 vs 2.4 births per block. The difference is the birth cost at the maximum rate plus the short grains' ring-read locality, so it bounds `ScheduleGrain` from above | an upper bound for grain-engine §8's `ScheduleGrain` row |
| `tail` | 123 s of the golden tail vector (`strums_tail_123s/tail_post_fb`) and 2 s of noise then 120 s of silence through the pessimistic configuration: statistics for the active part and for the **silent tail** separately, and the output hash | §4.2 test b |

Then flash the two variants and run the same command, saving `bench_xip.log` and
`bench_hooks.log`, and compare all three:

```text
python tools/hil/bench_report.py --log bench.log --log bench_xip.log --log bench_hooks.log --markdown bench-all.md --expect-archive build/fw/firmware/engine-archives.sha256
```

The report labels every row with the log it came from, adds an XIP/ITCM ratio table (mean,
p99.9, max, warm and cold) for profile §7.1, and marks a run **INCOMPLETE** (exit 1) when its
stream lacks `bench-begin`, `bench-end` or an expected suite line, holds a damaged line, carries a
`resync` notice or a nonzero drop count, reports another engine archive than `--expect-archive`,
or ran with a cache off (its budget figures are then withheld).

**`memcpy`, `memmove` and `memset`.** newlib-nano's move one byte per loop iteration; the engine calls
`memset` in every `Process` and clears 16 MiB of SDRAM with it in `Restart`, so those loops would be
what the pass measured. The images link the firmware's own word-wise versions
([`platform/MemFunctions.c`](platform/MemFunctions.c), 32 bytes per iteration, integer registers
only) and place them with the engine's code: in ITCM in `brainscape_bench`, `_hooks`, `parity` and
`live`, in QSPI in `brainscape_bench_xip`, so the §7.1 comparison covers them as well. The hello
line says where they run.

**The denormal decision (profile §4.2).** `brainscape_bench_hooks` links the engine built with the
guard's test hooks, which can force FZ on inside the guard and collect each block's FPSCR
exception flags. Its `tail` suite renders both tails at FZ = 0 and FZ = 1, and the report applies
the decision rule to the **silent tail** only (the active part's onsets and transients would set the
maximum at both settings and hide the slow blocks): keep gradual underflow if the silent tail's
worst block at FZ = 0 is within 1.2× of FZ = 1, at most 0.5 % of its blocks raise a subnormal flag
**in the FZ = 1 render**, and the forced-FZ render equals the golden hash (the golden tail: both
renders must equal `golden.json`; the noise tail: FZ = 1 must equal FZ = 0). The flag census comes
from the FZ = 1 render because Armv7-M cannot report subnormals at FZ = 0: IDC is set only when FZ
flushes an input, and UFC only for a tiny result that is also inexact, so exact subnormal
arithmetic (a decaying `x * 0.5`) raises nothing. At FZ = 1 every subnormal operand raises IDC and
every subnormal result UFC; the FZ = 0 count is shown for reference only. The shipping-archive
images report FZ = 0 only.

**What this pass does not decide.** Profile §8.3 Q2 asks for DWT "with contraction on, off and
explicit FMA" and for polynomial kernels against Init-built tables. Every build here has contraction
off (the profile's flags forbid it outside the test-only negative control, which the firmware
refuses), no explicit-FMA variant exists, and the births suite only bounds the per-birth cost. So
the report applies §7.3's rule instead: adopt explicit FMA only if contraction-off threatens the
budget. It prints the worst pessimistic block (warm and cold, `pess_events` included) against the
480,000-cycle deadline and a verdict; a polyphony above 64 voices would need its own row. Kernels
against tables, and the watermark half of Q9, stay open.

**Session 1** (2026-10-07, sound revision 1, images from `a91aa8ad64c7`). The three builds each
ran `run` complete (no run INCOMPLETE, 0 bytes dropped); the logs are in
[`records/rev7-2026-10-07/session-1/`](records/rev7-2026-10-07/README.md), and
[the silicon record](../docs/design/reviews/rev7-silicon-record.md) §3 has the tables. With the
engine's code in ITCM:

- **The worst-case budget is not met.** Worst warm block: `nominal` 99.1 % (100.3 % cold),
  `pess_render` 135.5 %, `pess_births` 168.5 %, `pess_events` 146.3 %, `dense_1ms` 118.6 %;
  the others peak at 41.4 % (`default`), 54.4 % (`tail_post_fb`), 84.6 %
  (`pitch_reverse_spray`) and 48.3 % (`max_delay_spray_rev_up24`). These are the engine's
  cycles alone, with interrupts off around each call. A fix is under design, with owner
  decisions pending.
- **§7.3's rule:** contraction off threatens the budget, so explicit FMA is a candidate, to build
  and measure before adopting.
- **§4.2's rule:** a subnormal operand or result costs nothing at FZ = 0 (1.00× the normal
  latency), and both tails pass (silent-tail worst block FZ = 0 / FZ = 1 1.002 and 0.999, 0 of
  120,000 blocks flagged at FZ = 1, the renders equal): keep gradual underflow.
- **The flush:** the engine's bit test costs 2.02–4.03 cycles per site, 0.9–1.8 % of the budget at
  45 sites; the two compares would cost 3.6–4.6 %.
- **`Restart`** after rendering 47.18 ms (an Exact load 47.17 ms), of which the 16 MiB clear at
  the `STM` floor is 44.97 ms; `Init` 50.14 ms.
- **XIP against ITCM:** the mean grows 1.045–1.210× warm and 1.259–1.697× cold, and the nominal
  row reaches 112.5 %: the engine stays in ITCM.

The bench has not run at revisions 2 to 7.

## 5. Image C: live audio

### Wiring a bare Rev7 at line level

| Seed pin | Signal |
| --- | --- |
| 16 | Audio In L (`in[0]`) |
| 17 | Audio In R (`in[1]`) |
| 18 | Audio Out L (`out[0]`) |
| 19 | Audio Out R (`out[1]`) |
| 20 | AGND: the audio ground for both jacks |
| 40 | DGND: **tie AGND (20) to DGND (40)**, as the Seed datasheet requires |

Power comes from the micro-USB port (or 5–17 V on VIN, pin 39). Pin numbers follow Electrosmith's
Seed pinout (40-pin footprint, identical on Rev4/5/7); check the pinout card before connecting.
Use TRS jacks or a breakout: tip → 16 (or 18), ring → 17 (or 19), sleeve → 20. A mono source can
go to pin 16 alone.

Levels: the inputs are AC-coupled on the board, 13.6 kΩ, and **0 dBFS is 3.6 Vpp (about 1 Vrms)**
with an absolute maximum of ±1.8 V, so a consumer line output (−10 dBV) is comfortable and a hot
+4 dBu source clips on peaks. The outputs are AC-coupled with 100 Ω source impedance: feed a line
input (mixer, audio interface, powered monitor), not headphones directly. There is no buffer or
gain stage; a passive guitar straight in works but sounds dull and quiet (docs/research/
daisy-pedal-platforms.md §1).

### Driving it

```text
python tools/hil/console.py --port auto              # interactive
python tools/hil/console.py --port auto list "preset 5" stats
```

| Command | Effect |
| --- | --- |
| `list` | The eight presets: engine defaults, clean delay, strum marks, pitch/reverse/spray, freeze marks, the ambient tail preset, an octave shimmer with onset grains at marks, 1 ms glitch onset grains — musically useful golden-corpus presets, built from the corpus at boot: their parameters, and their package's leaves, mode and CTRL when they start from one (each row says whether it plays onset grains and marks) |
| `preset N` | A **Spillover** load as a stamped event: trails, grains and history carry over (real time); a change of mode is a Trails switch |
| `preset N cut` | A Spillover load with **FastCut**: the grains sounding at the load fade out over 128 frames (2.7 ms) |
| `preset N exact` | An **Exact** load: the output is muted while `Restart` clears 16 MiB; the reply gives how long it took |
| `set NAME VALUE` | A parameter event; NAME is a descriptor name (`layer0.size_ms`), a ParamId (`GrainSizeMs`) or an alias (`delay mix feedback trim out size density spray pitch transpose spread reverse jitter sustain skew smooth pan modrate moddepth delaytime delayfb delaymix reverbtime reverbmix cutoff res morph sens repeat decay voices skip burst spacing volume`); the reply echoes the canonical value and its bits. `trim` (`wet_trim_db`) and `volume` (the effect volume, `global.effect_volume_db`, a device setting that every load keeps) scale only the wet signal; `cutoff 40` kills it |
| `onset on`/`off`, `marks on`/`off` | The **mode structure**: onset-triggered grains, and layer 0 positioned at marks. A Spillover load of the current parameters (as last set) with the mode of a corpus package that differs from the default mode in exactly that (`lone_busy`, `reverse_mark_aging`, `strum_marks`; neither: the default mode), checked at boot. `set onset V` and `set marks V` (V ≥ 0.5 is on, as sessions migrate rows 27 and 28) do the same |
| `macro NAME POS` | A **macro move** (`activity repeats shape time space filter aux1 aux2`, position 0–1): the playing mode's targets of that macro, as the pedal's knobs will send them |
| `expression POS` | The **expression pedal** (0–1): the playing preset's assignments; nothing without any |
| `freeze on`, `freeze off`, `trigger` | Freeze and footswitch-trigger events |
| `params`, `get` | Every parameter (every leaf this sound revision plays and the effect volume) with range and current value |
| `stats [reset]` | **CPU load**: DWT cycles of `Engine::Process` per callback, mean and peak, as a share of the 48-frame budget; blocks over budget; onsets; mode switches; event-queue refusals; the structure playing |
| `info`, `dfu` | The hello line; reboot into the bootloader |

Every change reaches the engine as a frame-stamped event through its `EventQueue`, stamped at the
next block boundary (a late stamp applies at the start of the block that takes it), exactly as the
pedal's control loop will deliver them (profile §5.11). The engine runs the pedal's configuration:
48 kHz, `maxBlockSize` 48, the 2²² ring, stereo input. The LED flashes on every detected onset.
The image keeps its own copy of what it sent (the last load, then every parameter, macro and
pedal move, evaluated with the engine's own `EvalMacro` and `EvalExpression`), from which `onset`
and `marks` build their load. The presets and structures are checked on every host ctest run too
(`firmware_live_presets`: each loads exactly, alone and with each structure).

## 6. Hardware checklist

Ticked items passed on the owner's Rev7 on 2026-10-07 (session 1 at sound revision 1, session 2
at revision 3; [`records/rev7-2026-10-07/`](records/rev7-2026-10-07/README.md)). A new image or
revision runs them again.

- [x] `info` (any image): `"rev7": true`, board "Daisy Seed 1.2 / rev7 (PCM3060; PD5 strap)",
      `sysclkHz` 480000000, boot region "QSPI flash", bootloader "Daisy bootloader >= v6.1",
      `"lastFault": null`, `cpu.icache` and `cpu.dcache` true, `cpu.memFunctions.in` "ITCM" (all
      but `bench_xip`). Every hello of both sessions.
- [x] `fp`: FPSCR `0x00000000` and **FPDSCR `0x00000000`** (written before any constructor runs,
      so every interrupt handler's FP context starts from the profile word). FPDSCR in every
      hello; FPSCR `0x00000000` in each session's first parity hello, and in the others only the
      cumulative IXC flag (`0x00000010`) and, after a run, the C flag (`0x20000010`).
- [x] `audio`: sample rate 48000, block 48, bit depth 24, codec "PCM3060 (hardware mode…)".
- [x] `build.engineArchiveSha256` equals the `parity-m7` CI job's archive hash for the same commit,
      and `build.dirty` is false (`--expect-archive build/fw/firmware/engine-archives.sha256`
      checks the hash). Revision 1 `4f4ddaa3…` (CI run 37564718440, on a documentation commit
      on top of the images' `a91aa8ad64c7`), revision 3 `1d6fe1dc…` (CI run 37700307648, on the
      images' `4090270`), both from clean trees.
- [x] Parity: `run`, `run pedal`, `run hostile` all `VERDICT: PASS` (never `FAIL (TRANSPORT)`); the
      final `idle` line shows `"droppedBytes":0`. Both sessions: 28 of 28 presets at revision 1,
      33 of 33 and 18 packages at revision 3.
- [x] Bench: `run` on all three bench builds, none INCOMPLETE; keep the logs (`--save`) for the
      decisions of profile §8.3 (Q1 subnormals and the §4.2 rule, Q5 flush form, the `Restart`
      half of Q9, §7.3's explicit-FMA rule) and §7.1 (ITCM against XIP). Session 1, revision 1
      (§4, "Session 1"); not yet at revisions 3 to 7.
- [ ] Live: clean pass-through at `set mix 0`; presets switch without clicks (Spillover, and
      `preset N cut`) and with a mute (exact); `onset on` and `marks on` audibly change the
      texture (grains born on each pluck; grains read from the plucks' attacks) and `stats`
      counts each as a mode switch; `macro time 0.9` lengthens the delay; `cutoff 40` and
      `volume -24` take the wet down, never the dry; `stats` mean and peak under 100 % for every
      preset and structure. Expect the glitch preset to fail the last: its corpus preset,
      `dense_1ms`, peaked at 118.6 % on the bench.
- [ ] Look for USB dropouts during a long render (a `resync` line, a nonzero `droppedBytes`) and
      for audio clicks while typing commands (the USB interrupts run below the audio DMA's
      priority). The long renders are done: no `resync` and 0 bytes dropped in any parity or
      bench run; the clicks need the live image.
- [ ] If the LED ever shows two long flashes: reconnect, `info`, and keep the `lastFault` line.
      Not seen so far: `"lastFault": null` in every hello.

## 7. Building

The images build with the pinned **GNU Arm Embedded Toolchain 10.3-2021.10** (profile §6.8), CMake
3.24 or later (libDaisy v9's own CMake needs it) and make. The configure step refuses another
compiler version or a build type other than Release (the engine archive would no longer be the
audited one). On the owner's Windows machine:

```bash
export PATH="/c/Program Files/CMake/bin:/c/Program Files (x86)/GNU Arm Embedded Toolchain/10 2021.10/bin:$PATH"
cmake -B build/fw -G "Unix Makefiles" \
  -DCMAKE_MAKE_PROGRAM="C:/Users/zchdv/AppData/Local/Microsoft/WinGet/Packages/ezwinports.make_Microsoft.Winget.Source_8wekyb3d8bbwe/bin/make.exe" \
  -DCMAKE_TOOLCHAIN_FILE=tools/cmake/arm-none-eabi-toolchain.cmake -DCMAKE_BUILD_TYPE=Release \
  -DBRAINSCAPE_BUILD_TESTS=OFF -DBRAINSCAPE_BUILD_FIRMWARE=ON -DBRAINSCAPE_WERROR=ON
cmake --build build/fw -j 8
```

The `.bin`, `.elf`, `.map` and a memory report (`*.size.txt`) of every image land in
`build/fw/firmware/`, with `engine-archives.sha256`. Every image is checked as it links: its memory
regions (the linker script), and its boot path ([`cmake/BootCheck.cmake`](cmake/BootCheck.cmake)):
nothing that runs before the preinit hook has filled ITCM — libDaisy's `Reset_Handler`,
`__libc_init_array`, the hook itself and the SysTick path the Daisy bootloader leaves running — may
call into ITCM. libDaisy's startup file is compiled with `-fno-tree-loop-distribute-patterns` for
that ([`cmake/LibDaisy.cmake`](cmake/LibDaisy.cmake)): otherwise GCC turns its `.data` and `.bss`
loops into calls to `memcpy` and `memset`, which an ITCM image keeps in ITCM. An ITCM image is
also checked for the engine's COMDAT sections ([`cmake/ItcmCheck.cmake`](cmake/ItcmCheck.cmake)):
an inline table such as `kParamTable`, or a template instantiation, is carried by every object
that uses it, and the linker keeps the first copy it reads, often a golden-harness object's,
which the per-archive patterns of the linker script would leave in QSPI beside the ITCM code
that reads it. The linker script names the engine's tables so that the kept copy goes to ITCM,
and the check fails the build when any COMDAT section of the archive's ITCM members lands
elsewhere (it flags the four tables when they are not named, as a negative control).

The configure step downloads the pinned libDaisy (below) into the build tree;
`-DFETCHCONTENT_SOURCE_DIR_LIBDAISY=<checkout>` builds offline from a libDaisy checkout at that
commit with its submodules. `BRAINSCAPE_FIRMWARE_APP_TYPE=BOOT_NONE` targets the internal flash
for images that fit it (none of these do). CI builds all five images on every relevant pull request
(`.github/workflows/firmware.yml`, compile only), checks their engine archives against the M7
oracle build's byte for byte, and runs the static audits on the firmware build.

### Sizes (`.bin`, all flashed to QSPI at `0x90040000`)

At sound revision 7 (revision 3's in brackets). At revision 2 the images grew by the 18 embedded
corpus packages (77,768 bytes), the package decoder, validator and encoder, the macro evaluator and
the engine's mode runtime; revision 3's Mix law added 40–56 bytes to each; wave 1 (revisions 4–7)
added 11 packages and the engine's wave-1 code, about 4.4 KiB of it in ITCM.

| Image | Bytes | ITCM used (of 64 KiB) |
| --- | --- | --- |
| `brainscape_parity.bin` | 399,600 (337,896) | 58.4 KiB, 91.2 % (54.0 KiB) |
| `brainscape_bench.bin` | 397,284 (335,924) | 58.2 KiB, 91.0 % (53.9 KiB) |
| `brainscape_bench_xip.bin` | 397,112 (335,744) | 0 |
| `brainscape_bench_hooks.bin` | 397,772 (336,428) | 58.7 KiB, 91.7 % (54.3 KiB) |
| `brainscape_live.bin` | 438,208 (376,392) | 60.4 KiB, 94.4 % (56.0 KiB) |

The ITCM holds the engine's code and constants (`Engine` 9.6–13.4 KiB, `PostChain` 11.0,
`Validate` 8.4, `Granular` 8.2, `DetMath` 5.4, `OnsetDetector`, `Mode`, `ModeEval`, `EventQueue`,
and the shared tables `kParamTable`, `kLeafParams`, `kLeafOrdinal` and `kDefaultModeHash`,
2.5 KiB, that the parity and bench images would otherwise take from a harness object in QSPI),
libgcc's helpers and `mem*`. The live image links more of `Engine`'s API (`GetParam`,
`ModeSwitches` and the rest of what the console reads) and has about 3.6 KiB left at revision 7
(8 KiB at revision 3): the next wave that grows the engine moves `Validate` (it runs once per
load) out of ITCM, or places the engine by function rather than by object.

### Memory map

| Region | Holds | Use (parity image) |
| --- | --- | --- |
| QSPI flash `0x90040000` | the image: vector table, code, constants (the 29 corpus packages among them, 128 KiB), initial data, ITCM's load image | 390 KiB |
| ITCM | the engine's code and constants (not the package decoder, encoder, SHA-256 or test-signal generator), libgcc's helpers and the firmware's `mem*` functions, copied at boot | 58 KiB of 64 |
| DTCM | the relocated vector table (1 KiB) and the fault handler's stack (1 KiB), Hot arena (24 KiB, `PlanMemory` asks 16.4 KiB at `maxBlockSize` 48, 20 KiB at 512), the Engine object (a 9 KiB slot since sound revision 6; `sizeof(Engine)` is `kEngineImplBytes`, 7,168 bytes at sound revision 2 and 8,192 since 6, when wave 1's repeat voices grew the grain pool), the main stack (32 KiB reserved at the top, linker-checked) | 34 KiB + stack |
| AXI SRAM (D1) | Warm arena (136 KiB; 128.2 KiB asked at sound revision 2, 1,616 bytes more than revision 1 for the active mode), USB serial rings (33 KiB), `.data` and `.bss` (the live image's 16 `PresetState`s, 2,656 bytes each) | 193 KiB of 512 (live: 249 KiB) |
| D2 SRAM | libDaisy's audio DMA buffers (MPU non-cacheable) | 16 KiB |
| Backup SRAM | libDaisy's `boot_info` (the Daisy bootloader's handshake, kept first: linker-checked), then the fault record | 148 B |
| SDRAM | Bulk arena (17 MiB; 16.73 MiB asked: the 2²² ring and the post delay), the bench's per-block results, the heap (the harness's containers only, never engine state) | 17 MiB + heap |

The arenas live in sections named `.bss.brainscape_{dtcm,axi,sdram}_*`
([`platform/Placement.cpp`](platform/Placement.cpp)), which the compiler emits as NOBITS (libDaisy's
own `.dtcmram_bss`/`.sdram_bss` names produce 17 MiB object files) and the linker script
([`linker/seed_h750.ld.in`](linker/seed_h750.ld.in), derived from libDaisy's) routes to their regions
and checks: region overflow, 32 KiB left for the stack in DTCM, and the heap's minimum. Every image
also checks `PlanMemory` against the arenas at boot, and the host test `firmware_arena_plan` does on
every ctest run. The heap peaks near 250 KiB in a parity run (measured under QEMU; on the Rev7 the
break stood at 224,904 bytes after the first whole-corpus run at revision 1 and at 290,856 after
the second, and at 290,856 after the first at revision 3), so it is in SDRAM;
the firmware's `_sbrk` refuses to grow before the SDRAM is initialized, and newlib's malloc lock
masks interrupts because libDaisy's USB stack allocates inside the USB interrupt.

### Platform layer ([`platform/`](platform/))

- **Board**: `DaisySeed::Init(true)` (480 MHz, caches, MPU, SDRAM, QSPI, LED). libDaisy detects the
  revision from ground straps (`DaisySeed::CheckBoardVersion`): PD3 → Seed 1.1/rev5 (WM8731 over
  I²C), PD4 → Seed2 DFM, **PD5 → Seed 1.2/rev7**, PH6 → Seed3, none → rev4. Rev7's PCM3060 runs in
  hardware mode, so libDaisy configures it like rev4: SAI1 block A transmits on PE6, block B
  receives on PE3, MCLK PE2, FS PE4, SCK PE5, the codec reset on PB11; 48 kHz, 24-bit, 48-frame
  blocks, `postgain` 1, so input samples are exactly `i × 2⁻²³` (profile §8.3 Q8).
- **Preinit hook** (`.preinit_array`, before any constructor): stops the SysTick the Daisy
  bootloader leaves running (`HAL_Init` restarts it), writes FPSCR = 0 and FPDSCR = 0 (profile §4.1,
  §8.4 step 13), copies ITCM's contents in, and installs the fault vectors.
- **Faults** ([`platform/Fault.cpp`](platform/Fault.cpp)): libDaisy's `HardFault_Handler` ends in
  `BKPT`, which locks the core up without a debugger, and its `Default_Handler` (MemManage, BusFault,
  UsageFault, NMI, every unhandled interrupt) loops forever at the exception's priority, freezing the
  LED and SysTick. The platform copies the vector table to DTCM, points those vectors at its own
  handler and enables the bus, usage and memory-management fault vectors. The handler runs on its
  own stack, saves a record (kind, `CFSR`, `HFSR`, `MMFAR`, `BFAR`, the stacked `pc`, `lr`, `xPSR`,
  `EXC_RETURN`, `sp`, time since boot, image, message) to the backup SRAM, blinks two long flashes by
  writing the LED's GPIO with a DWT busy-wait, and resets after about 10 s. `Fatal()` called from an
  interrupt (the audio callback, USB) takes the same path, because neither SysTick nor the USB
  interrupt could preempt it. libDaisy decides where the program runs from `SCB->VTOR`, so the
  image's own table is installed while `DaisySeed::Init` runs.
- **DWT**: CYCCNT enabled (the M7's DWT lock unlocked), extended to 64 bits by the 1 kHz SysTick.
- **USB serial**: libDaisy's CDC device with a 32 KiB transmit ring handed to the CDC class one
  chunk at a time from the main loop, a receive ring filled in the USB interrupt, and the OTG_FS
  interrupts moved below the audio DMA's priority (companion §7.2's bring-up rule). Lines go out
  whole or not at all. Completion comes from the CDC class's `TransmitCplt` hook and every
  (re)configuration is counted through its `Init`/`DeInit`, so a suspended bus keeps its transfer
  while a USB reset or unplug drops it (and the rest of the line it cut) instead of sending it twice;
  any loss is announced by a `{"type":"resync","droppedBytes":…,"droppedLines":…}` line.
- **LED**: driven from SysTick by mode (idle, busy, done, fault, onset pulse), by writing PC7's GPIO
  directly once `BoardInit` has armed it: until then the SysTick hook does nothing, because it can
  run before the startup code has initialized the variables it reads.
- **`memcpy`, `memmove`, `memset`** ([`platform/MemFunctions.c`](platform/MemFunctions.c)): word-wise
  replacements for newlib-nano's byte loops (section 4).

## 8. Protocol

One text command per line to the device; one JSON object per line back, integers and strings only
(`brainscape-hil/1`). Every image answers `info` with `{"type":"hello"}`: image, build (commit,
dirty, app type, libDaisy, toolchain, engine archive SHA-256s, the engine's `BuildToolchain()`),
sound revision, board (revision, clocks, boot region, bootloader, audio configuration), FP
registers, `cpu` (cache bits, MPU regions, QSPI registers, where `memset` runs), `lastFault` (or
null), `uptimeMs` and the memory map. The parity image streams `parity-begin`, `package`, `vector`,
`preset` and `parity-end` lines (`brainscape-parity-stream/3`: every line numbered by `seq`;
documented in `ParityStream.h`) and then `idle` (with `droppedBytes`, `droppedLines`); the live
image answers `mode`, `macro` and `expression` besides its revision-1 replies; the bench streams `bench-begin`, `memory`, `micro`,
`flush-micro`, `restart`, `blocks`, `stage`, `births`, `tail` and `bench-end` (with the drop counts).
Any image may emit `{"type":"resync",…}` after its USB serial lost lines; errors are
`{"type":"error","message":...}`.

## 9. Verified without hardware

- All five images build with `-Werror` and fit their regions, and pass the boot-path check
  (`BootCheck.cmake`; it fails when `main` is added to its list, as a negative control); the four
  ITCM images pass `ItcmCheck.cmake` (6 of 6 COMDAT sections in ITCM), and no word in their
  `.itcm_text` literal pools points into QSPI (it found `kParamTable`, `kLeafOrdinal` and
  `kDefaultModeHash` read from QSPI before the tables were named). The engine
  archives in the firmware build are byte-identical to the `BRAINSCAPE_BUILD_M7_ORACLE` build's
  (sound revision 7: `ea7c5dfb08cf44e3…`, hooks `959493f9b4cf03f2…`; revision 6:
  `9e07ead26fac7402…`, hooks `bb616b8b55d80185…`; revision 5: `fc2d3076d962a781…`, hooks
  `e6432e13a7e74c91…`; revision 4: `7c20000633f303ee…`, hooks `176b3797a7977851…`; revision 3:
  `1d6fe1dc41f02fd9…`, hooks `023a9fa933fa9c0d…`; revision 2:
  `89b73b51fe4261bf…`, hooks `71a383520843e671…`; revision 1:
  `4f4ddaa3583e46f2…`, hooks `da7b4f2e9b44aef7…`), and, at every revision, to the M7 oracle
  built on Linux with the same pinned toolchain (in
  Docker, whose CMake 3.22 is too old for libDaisy's, so the images themselves build on Windows).
- The packages the images carry are the committed ones: the table is generated from
  `presets/MANIFEST` and the `.bsp` files at build time, the parity stream lists every package
  as the program decoded it, and `parity_check.py` compares those hashes with `MANIFEST`.
- The static audits pass on the firmware build: no import outside the allowlist (no libm, no
  soft-float helper; the arm symbol audit runs on Windows too, with the pinned toolchain's `nm`), no
  fused instruction in either engine archive, no forbidden flag in any of the 256 translation
  units (libDaisy's included; 242 at sound revision 1), no GOT relocation. The only fused instructions in the images are in
  the prebuilt libgcc's float-to-`uint64` helpers and newlib's `strtod` (the live image's `set`
  parsing), outside the engine.
- `platform/MemFunctions.c`, built with the firmware's flags, matches a byte-loop reference in
  119,748 cases under `qemu-arm -cpu cortex-m7` (every size to 300 bytes at every source and
  destination alignment, `memmove` overlaps from −70 to +70 bytes, `memset` with sign-bit and
  out-of-range values, 60 KB copies); its disassembly uses no floating-point register.
- The parity stream's code, built for the Cortex-M7 with the oracle's syscall shim, the packages
  compiled in as the image has them, and run under `qemu-arm -cpu cortex-m7` **in the parity
  image's own placement** (`--placement`: the firmware's arena sizes and alignments,
  `Renderer(cfg, Placement)`, a second renderer over the same storage), matches the revision-7
  `golden.json` on all 45 presets, with their package hashes, and `MANIFEST` on all 29 packages,
  with `maxBlockSize` 48 and 512, and the hostile FP run on the quick set (40 presets), checked by
  `parity_check.py` (CI's `parity-m7` job runs the 48 case on every pull request); so does each
  earlier revision's from 2 to 6 on its own corpus. At revision 3 CI's job matched the revision-3
  file and `MANIFEST` at 48 (pull requests #6 and #7, `main` at `4090270`), and the Rev7 at 48 and
  512 and from a hostile caller (§6). On the host the
  same check is the ctests `golden_parity_stream_mb512` and `_mb48`.
- `parity_check.py` was run on 17 damaged streams at revision 1 (lost, spliced, truncated and
  renamed lines, a missing `parity-end`, a `resync` notice, device captures without or with a lossy
  `idle` line, a wrong configuration, a wrong hash, a wrong archive, the old format) and, at
  revision 2, on streams with a package line lost, a package missing, a package not in `MANIFEST`,
  a package or a preset's package hash changed, a preset's package renamed, a counter changed and
  the revision-1 format, with the intended verdict and exit status each time; `bench_report.py`
  and `console.py` on synthetic logs only.
- The live image's presets and structures load exactly on the host (`firmware_live_presets`), and
  its structure check refuses a package with other macros (`switch_onset`) in place of
  `lone_busy`.
- Not verifiable without the board: anything electrical, the bootloader handoff, the fault
  handler's LED and reset, USB enumeration and throughput, the SAI/codec path, and every cycle
  count. The Rev7 has since shown the bootloader handoff, USB enumeration, whole-corpus streams
  without a lost byte and the cycle counts (§4, §6); the fault handler has not fired, and the
  SAI/codec path waits for the live image.

## 10. Dependencies and licensing

**libDaisy v9.0.0**, commit `08087203debc646d3710ec8caa35bac6cf20ab5e`
(github.com/daisyaudio/libDaisy), fetched by archive SHA-256 at configure time
([`cmake/LibDaisy.cmake`](cmake/LibDaisy.cmake)) and never vendored, with the four submodules its
library build needs at the commits that libDaisy commit records: ARM CMSIS_5 `2b7495b8`, ST
`cmsis_device_h7` `6dac8c24`, ST `stm32h7xx_hal_driver` `404a70d4`, ST `stm32_mw_usb_device`
`7b5e6886` (CMSIS-DSP and googletest are libDaisy submodules it does not build from, and are not
fetched). Nothing else is downloaded.

These images use libDaisy's USB CDC, which is ST middleware under **SLA0044**, a licence that
forbids open-source redistribution (companion-app.md §7.2), and libDaisy's `System` links ST's USB
host stack into every image as well. They are **for the bench only and must not be distributed.**
`tools/ci/audit_firmware_elf.py` (the profile's `firmware-elf-audit`) lists that code in every
image and labels them not distributable (`--prototype`); the product firmware moves USB to TinyUSB
and SD to its own glue and runs the audit without `--prototype`.
