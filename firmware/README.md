# Brainscape firmware: Daisy Seed Rev7 bring-up

Bring-up firmware for the owner's **Daisy Seed Rev7** (STM32H750, Cortex-M7 at 480 MHz, 64 MB
SDRAM, 8 MB QSPI flash, PCM3060 codec, micro-USB): the first part of merged step 5
([docs/STATUS.md](../docs/STATUS.md); [determinism-profile.md](../docs/design/determinism-profile.md)
§8.4 step 13; [companion-app.md](../docs/design/companion-app.md) §8 phase D). It is a test bench,
not the pedal firmware: no controls, no presets on SD, no device link.

| Image | `.bin` | What it does | Host tool |
| --- | --- | --- | --- |
| **A, parity** | `brainscape_parity.bin` | Renders the golden corpus on the Seed exactly as the golden harness does and streams every hash | [`tools/hil/parity_check.py`](../tools/hil/parity_check.py) |
| **B, bench** | `brainscape_bench.bin` | The DWT measurement pass: cycles per block, per stage, per birth, `Restart`, subnormal latency, the flush idiom, silent tails | [`tools/hil/bench_report.py`](../tools/hil/bench_report.py) |
| B, code-placement A/B | `brainscape_bench_xip.bin` | Image B with the engine's code executing in place from QSPI instead of ITCM (profile §7.1) | `bench_report.py` |
| B, flush A/B | `brainscape_bench_hooks.bin` | Image B on the engine built with the FP guard's test hooks: the FZ = 0/1 silent-tail test and flag census of profile §4.2 | `bench_report.py` |
| **C, live** | `brainscape_live.bin` | The engine in the audio callback on the Seed's codec, presets and parameters over USB | [`tools/hil/console.py`](../tools/hil/console.py) |

Every image links the engine archive `libbrainscape_dsp.a` exactly as CI's `parity-m7` job builds,
audits and renders it under QEMU (same toolchain, flags and deterministic archive: SHA-256
`4f4ddaa3583e46f2…` at sound revision 1), and reports that hash in its hello line.

## 1. What to flash, in order

Everything goes through Electrosmith's web programmer, **<https://flash.daisy.audio>** (WebUSB:
Chrome or Edge; Firefox and Safari have no WebUSB). No `dfu-util` is needed.

**App type: `BOOT_QSPI`.** Every image is 192–200 KiB, past the 128 KiB internal flash, so the images
run from the 8 MB QSPI flash and are loaded by the **Daisy bootloader**, which lives in the internal
flash. `grain-engine.md` §7 plans the pedal this way. The engine's code and constants (and the
64-bit division helpers it calls) are copied into the 64 KiB ITCM at boot, as profile §7.1 asks for
the inner loops; everything else executes in place from QSPI behind the 16 KiB I-cache.

### Once: install the Daisy bootloader v6.4

1. Connect the Seed's micro-USB port to the computer with a **data** cable.
2. Put the STM32 into its built-in DFU mode: **hold BOOT, press and release RESET, release BOOT**.
   The user LED stays dark.
3. On flash.daisy.audio click **Connect** and pick the DFU device ("DFU in FS Mode").
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

1. Put the Seed in the bootloader's DFU mode: press **RESET**, then press **BOOT while the LED is
   breathing** (a few rapid blinks acknowledge: the bootloader now waits indefinitely). If one of
   these images is already running you can skip the buttons: send it `dfu`
   (`python tools/hil/console.py --port auto dfu`) and it reboots into the waiting bootloader.
2. On flash.daisy.audio click **Connect** and pick the DFU device again.
3. Open the **File Upload** tab, **Choose or drag a file**: the image's `.bin`, then **Flash**. The
   programmer writes bootloader apps to QSPI at `0x90040000` by itself (it sees the bootloader's
   QSPI memory map); there is no address to type.
4. The bootloader checks the image and starts it. A running image blinks the LED **once a second**
   (idle), fast while it renders or measures, steadily when a parity run has finished, and three
   quick flashes with a pause on a fatal error. (A single **SOS** comes from the bootloader: it
   refused the image, usually a `BOOT_NONE` binary.)

Flash order for the bench session: `brainscape_parity.bin` → run the parity check →
`brainscape_bench.bin` → `brainscape_bench_xip.bin` → `brainscape_bench_hooks.bin` (each with
`bench_report.py`) → `brainscape_live.bin`.

**Troubleshooting.** Windows: if Connect lists nothing in DFU mode, the STM32 DFU device has no
WinUSB driver bound; Electrosmith's documentation covers binding one (Zadig). Linux: WebUSB needs
a udev rule granting access to `0483:df11`. A charge-only cable shows no device at all.

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
python tools/hil/parity_check.py --port auto --save parity-rev7.log
```

The tool asks the device for its hello line (board, clock, FP registers, archive hash), sends
`run`, and prints each preset as it arrives. The device renders all 13 vectors and 28 presets
(639 s of audio) offline in its main loop, never in the audio callback: one engine in the pedal's
memory placement (Hot arena and Engine object in DTCM, Warm in AXI SRAM, Bulk in SDRAM), restarted
with `LoadPreset(…, Exact)` for every render, 48-frame blocks, the integer test signal as input,
events through the engine's `EventQueue` — the golden harness's own code
([`dsp/tests/golden/ParityStream.h`](../dsp/tests/golden/ParityStream.h)), not a copy. It takes
roughly 4–8 minutes (the profile estimates 1.3–3.1× realtime); the stream carries DWT cycles per
preset, so the tool prints the measured realtime factor too. It ends with

```text
VERDICT: PASS - 28 preset(s) match golden.json bit for bit (sound revision 1, whole corpus)
```

or `FAIL` with the first differing second of every preset that differs. Then repeat in the other
two configurations worth having on silicon:

```text
python tools/hil/parity_check.py --port auto --run "run pedal"   --save parity-rev7-pedal.log
python tools/hil/parity_check.py --port auto --run "run hostile" --save parity-rev7-hostile.log
```

`pedal` renders with the live engine's `maxBlockSize` of 48 instead of the harness's 512;
`hostile` sets FZ|DN and round-toward-zero in the caller's FPSCR around every engine call, which
the guard must neutralize (profile §6.4), and checks the guard hands the word back. `quick` skips
the three long vectors, `only=VECTOR[/PRESET],…` selects. A saved log can be checked again any
time: `parity_check.py --log parity-rev7.log`.

## 4. Image B: the DWT measurement pass

```text
python tools/hil/bench_report.py --port auto --save bench.log --markdown bench.md
```

`run` takes about 10–15 minutes (`--run "run quick"` skips the two-minute silent tails). Every
number is DWT cycles of the 480 MHz core with **interrupts off around the measured call**; the
budget is 10,000 cycles per sample, 480,000 per 48-frame block. The suites (each also a command):

| Suite | What | Decides |
| --- | --- | --- |
| `memory` | `PlanMemory` against the arenas, and the linker map | — |
| `micro` | Dependent `vmul`/`vadd`/`vdiv`/`vsqrt` chains with normal and subnormal operands and results, at FZ = 0 and 1; the engine's flush idiom (`FlushTiny`) against no flush and the two-compare form | profile §4.2 test a, §8.3 Q1, Q5 |
| `restart` | `Restart` after rendering (the 16 MiB ring, post delay and reverb clear), on a clean engine, `LoadPreset(Exact)`, `Reset`, `ClearHistory`, `Init` | §8.3 Q9: the pedal's default load mode |
| `blocks` | Cycles per block, mean/p50/p90/p99/p99.9/max, **warm and cold caches** (cold: D-cache cleaned and invalidated and I-cache invalidated before every block), for engine defaults, the design's nominal row, the pessimistic configuration (64 voices at +24 st with full cents spread, reverse, spray, jitter, feedback 1.05, every post stage, onset grains at marks) with 20 ms and 1 ms grains, and four corpus presets | grain-engine.md §8, profile §7.2 |
| `stages` | The pessimistic configuration with one stage switched off at a time: each stage's cost by difference (the engine has no per-stage counters, and none were added: that would be instrumentation in `dsp/`) | §8.3 Q2 |
| `births` | 48 and 16 voices with 1 ms vs 20 ms grains: equal voices rendered per block, 48 vs 2.4 births per block, so the difference is the birth cost at the maximum birth rate (one per sample) | §8.3 Q2 |
| `tail` | 123 s of the golden tail vector (`strums_tail_123s/tail_post_fb`) and 2 s of noise then 120 s of silence through the pessimistic configuration: worst block, p99, output hash | §4.2 test b |

Then flash the two variants and run the same command, saving `bench_xip.log` and
`bench_hooks.log`, and compare all three:

```text
python tools/hil/bench_report.py --log bench.log --log bench_xip.log --log bench_hooks.log --markdown bench-all.md
```

`brainscape_bench_hooks` links the engine built with the guard's test hooks, which can force FZ on
inside the guard and collect each block's FPSCR exception flags. Its `tail` suite renders both
tails at FZ = 0 and FZ = 1 and counts the blocks raising IDC (input denormal) or UFC (underflow);
the report then applies profile §4.2's decision rule: keep gradual underflow if the worst block at
FZ = 0 is within 1.2× of FZ = 1, at most 0.5 % of blocks raise a subnormal flag, and the forced-FZ
render equals the golden hash (the tool checks the tail's hash against `golden.json`). The
shipping-archive images report FZ = 0 only.

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
| `list` | The eight presets: engine defaults, clean delay, strum marks, pitch/reverse/spray, freeze marks, the ambient tail preset, an octave shimmer with onset grains, 1 ms glitch grains — the parameters of musically useful golden-corpus presets, taken from the corpus at boot |
| `preset N` | A **Spillover** load as a stamped event: trails, grains and history carry over (real time) |
| `preset N exact` | An **Exact** load: the output is muted while `Restart` clears 16 MiB; the reply gives how long it took |
| `set NAME VALUE` | A parameter event; NAME is a descriptor name (`layer0.size_ms`), a ParamId (`GrainSizeMs`) or an alias (`delay mix feedback out size density spray pitch spread reverse jitter sustain skew smooth pan modrate moddepth delaytime delayfb delaymix reverbtime reverbmix cutoff res morph sens onset marks`); the reply echoes the canonical value and its bits |
| `freeze on`, `freeze off`, `trigger` | Freeze and footswitch-trigger events |
| `params`, `get` | Every parameter with range and current value |
| `stats [reset]` | **CPU load**: DWT cycles of `Engine::Process` per callback, mean and peak, as a share of the 48-frame budget; blocks over budget; onsets; event-queue refusals |
| `info`, `dfu` | The hello line; reboot into the bootloader |

Every change reaches the engine as a frame-stamped event through its `EventQueue`, stamped at the
next block boundary (a late stamp applies at the start of the block that takes it), exactly as the
pedal's control loop will deliver them (profile §5.11). The engine runs the pedal's configuration:
48 kHz, `maxBlockSize` 48, the 2²² ring, stereo input. The LED flashes on every detected onset.

## 6. Hardware checklist

- [ ] `info` (any image): `"rev7": true`, board "Daisy Seed 1.2 / rev7 (PCM3060; PD5 strap)",
      `sysclkHz` 480000000, boot region "QSPI flash", bootloader ">= v6.1".
- [ ] `fp`: FPSCR `0x00000000` and **FPDSCR `0x00000000`** (written before any constructor runs,
      so every interrupt handler's FP context starts from the profile word).
- [ ] `audio`: sample rate 48000, block 48, bit depth 24, codec "PCM3060 (hardware mode…)".
- [ ] `build.engineArchiveSha256` equals the `parity-m7` CI job's archive hash for the same commit.
- [ ] Parity: `run`, `run pedal`, `run hostile` all PASS; the final `idle` line shows
      `"droppedBytes":0`.
- [ ] Bench: `run` on all three bench builds; keep the logs (`--save`) for the decisions of profile
      §8.3 (Q1 subnormals, Q2 FMA/kernels/birth cost, Q5 flush form, Q9 load mode) and §7.1 (ITCM).
- [ ] Live: clean pass-through at `set mix 0`; presets switch without clicks (Spillover) and with
      a mute (exact); `stats` mean and peak under 100 % for every preset; the LED follows plucks.
- [ ] Look for USB dropouts during a long render and for audio clicks while typing commands (the
      USB interrupts run below the audio DMA's priority).

## 7. Building

The images build with the pinned **GNU Arm Embedded Toolchain 10.3-2021.10** (profile §6.8), CMake
3.24 or later (libDaisy v9's own CMake needs it) and make. On the owner's Windows machine:

```bash
export PATH="/c/Program Files/CMake/bin:/c/Program Files (x86)/GNU Arm Embedded Toolchain/10 2021.10/bin:$PATH"
cmake -B build/fw -G "Unix Makefiles" \
  -DCMAKE_MAKE_PROGRAM="C:/Users/zchdv/AppData/Local/Microsoft/WinGet/Packages/ezwinports.make_Microsoft.Winget.Source_8wekyb3d8bbwe/bin/make.exe" \
  -DCMAKE_TOOLCHAIN_FILE=tools/cmake/arm-none-eabi-toolchain.cmake -DCMAKE_BUILD_TYPE=Release \
  -DBRAINSCAPE_BUILD_TESTS=OFF -DBRAINSCAPE_BUILD_FIRMWARE=ON -DBRAINSCAPE_WERROR=ON
cmake --build build/fw -j 8
```

The `.bin`, `.elf`, `.map` and a memory report (`*.size.txt`) of every image land in
`build/fw/firmware/`. The configure step downloads the pinned libDaisy (below) into the build tree;
`-DFETCHCONTENT_SOURCE_DIR_LIBDAISY=<checkout>` builds offline from a libDaisy checkout at that
commit with its submodules. `BRAINSCAPE_FIRMWARE_APP_TYPE=BOOT_NONE` targets the internal flash
for images that fit it (none of these do). CI builds all five images on every relevant pull request
(`.github/workflows/firmware.yml`, compile only) and runs the static audits on the firmware build.

### Memory map

| Region | Holds | Use (parity image) |
| --- | --- | --- |
| QSPI flash `0x90040000` | the image: vector table, code, constants, initial data | 200 KiB |
| ITCM | the engine's code and constants and libgcc's helpers, copied at boot | 41 KiB of 64 |
| DTCM | Hot arena (24 KiB, `PlanMemory` asks 16.4 KiB at `maxBlockSize` 48, 20 KiB at 512), the Engine object (7 KiB), the main stack (32 KiB reserved at the top, linker-checked) | 31 KiB + stack |
| AXI SRAM (D1) | Warm arena (136 KiB; 126.6 KiB asked), USB serial rings (33 KiB), `.data` and `.bss` | 193 KiB of 512 |
| D2 SRAM | libDaisy's audio DMA buffers (MPU non-cacheable) | 16 KiB |
| SDRAM | Bulk arena (17 MiB; 16.73 MiB asked: the 2²² ring and the post delay), the bench's per-block results, the heap (the harness's containers only, never engine state) | 17 MiB + heap |

The arenas live in sections named `.bss.brainscape_{dtcm,axi,sdram}_*`
([`platform/Placement.cpp`](platform/Placement.cpp)), which the compiler emits as NOBITS (libDaisy's
own `.dtcmram_bss`/`.sdram_bss` names produce 17 MiB object files) and the linker script
([`linker/seed_h750.ld.in`](linker/seed_h750.ld.in), derived from libDaisy's) routes to their regions
and checks: region overflow, 32 KiB left for the stack in DTCM, and the heap's minimum. Every image
also checks `PlanMemory` against the arenas at boot, and the host test `firmware_arena_plan` does on
every ctest run. The heap peaks near 250 KiB in a parity run (measured under QEMU), so it is in
SDRAM; the firmware's `_sbrk` refuses to grow before the SDRAM is initialized, and newlib's malloc
lock masks interrupts because libDaisy's USB stack allocates inside the USB interrupt.

### Platform layer ([`platform/`](platform/))

- **Board**: `DaisySeed::Init(true)` (480 MHz, caches, MPU, SDRAM, QSPI, LED). libDaisy detects the
  revision from ground straps (`DaisySeed::CheckBoardVersion`): PD3 → Seed 1.1/rev5 (WM8731 over
  I²C), PD4 → Seed2 DFM, **PD5 → Seed 1.2/rev7**, PH6 → Seed3, none → rev4. Rev7's PCM3060 runs in
  hardware mode, so libDaisy configures it like rev4: SAI1 block A transmits on PE6, block B
  receives on PE3, MCLK PE2, FS PE4, SCK PE5, the codec reset on PB11; 48 kHz, 24-bit, 48-frame
  blocks, `postgain` 1, so input samples are exactly `i × 2⁻²³` (profile §8.3 Q8).
- **FP boot word**: a `.preinit_array` hook writes FPSCR = 0 and FPDSCR = 0 before any constructor
  (profile §4.1, §8.4 step 13), and copies the engine's code into ITCM.
- **DWT**: CYCCNT enabled (the M7's DWT lock unlocked), extended to 64 bits by the 1 kHz SysTick.
- **USB serial**: libDaisy's CDC device with a 32 KiB transmit ring handed to the CDC class one
  chunk at a time from the main loop, a receive ring filled in the USB interrupt, and the OTG_FS
  interrupts moved below the audio DMA's priority (companion §7.2's bring-up rule).
- **LED**: driven from SysTick by mode (idle, busy, done, fault, onset pulse).

## 8. Protocol

One text command per line to the device; one JSON object per line back, integers and strings only
(`brainscape-hil/1`). Every image answers `info` with `{"type":"hello"}`: image, build (commit,
app type, libDaisy, toolchain, engine archive SHA-256s, the engine's `BuildToolchain()`), board
(revision, clocks, boot region, bootloader, audio configuration), FP registers and the memory map.
The parity image streams `parity-begin`, `vector`, `preset` and `parity-end` lines
(`brainscape-parity-stream/1`, documented in `ParityStream.h`) and then `idle`; the bench streams
`bench-begin`, `memory`, `micro`, `flush-micro`, `restart`, `blocks`, `stage`, `births`, `tail` and
`bench-end`; errors are `{"type":"error","message":...}`.

## 9. Verified without hardware

- All five images build with `-Werror` on Windows, and on Linux with the same pinned toolchain
  (in Docker, whose CMake 3.22 needed libDaisy's CMP0135 line removed for that test only), and fit
  their regions; the engine archive in the firmware build is byte-identical to the
  `BRAINSCAPE_BUILD_M7_ORACLE` archive (`4f4ddaa3583e46f2…`), and so is the hooks archive.
- The static audits pass on the firmware build: no import outside the allowlist (no libm), no
  fused instruction in either engine archive, no forbidden flag in any of the 240 translation
  units (libDaisy's included), no GOT relocation. The only fused instructions in the images are in
  the prebuilt libgcc's float-to-`uint64` helpers and newlib's `strtod` (the live image's `set`
  parsing), outside the engine.
- The parity stream's code, built for the Cortex-M7 with the oracle's syscall shim and run under
  `qemu-arm -cpu cortex-m7`, matches `golden.json` on all 28 presets in the harness's configuration
  and with `maxBlockSize` 48, checked by `parity_check.py` (CI's `parity-m7` job runs the latter on
  every pull request). On the host the same check is the ctests `golden_parity_stream_mb512` and
  `_mb48`.
- Not verifiable without the board: anything electrical, the bootloader handoff, USB enumeration
  and throughput, the SAI/codec path, and every cycle count.

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
