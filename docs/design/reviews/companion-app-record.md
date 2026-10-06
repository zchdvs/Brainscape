# Companion app — evidence record

> The evidence behind [companion-app.md](../companion-app.md) (draft v2): what was measured,
> by which probe, where sources disagreed and how the design resolved it, and which earlier
> claims were corrected. The design document is normative; this record is not. Section numbers
> such as "§4.9" refer to the design document, "record §N" to this file, and "profile §N" to
> [determinism-profile.md](../determinism-profile.md).
> Code and repository documents are cited as `path:line` at `main` `e86e971`, JUCE at the
> 9.0.3 tag, and libDaisy at `master` as pushed on 2026-09-29. Documents amended alongside the
> design on 2026-10-05 (`grain-engine.md`, `docs/STATUS.md`, `README.md`) are cited as they read
> at `e86e971`, the text the evidence was checked against; their line numbers have since moved.

---

## 1. Evidence sources

All evidence was produced on 2026-10-05 in four stages:

1. **Feasibility investigation**, before the owner's decisions: could the engine ship as a
   desktop plugin beside the pedal? Five studies with compiled probes.
2. **Decision investigation**, after the owner chose JUCE, one monorepo and sample identity:
   five studies, including a working prototype of the determinism profile.
3. **Block-split verification**: an independent reproduction of the freeze/mark block-split
   bug on the unmodified repository, its root causes and a verified fix.
4. **Review round**: four reviews (numerics and determinism; firmware, USB/SD and licensing;
   JUCE hosting; product consistency) that checked both design documents against the evidence
   and ran further probes. Their findings were checked against the evidence before being
   applied.

| Label | Source | Probe directory under `tools/parity/` |
|---|---|---|
| [host] | Host-readiness audit (stage 1): block sizes, sample rates, lifecycle, threading, shared-library builds, memory, CPU. MSVC 14.40 and g++ 14.2. | not stored |
| [plan-of-record] | What the existing documents decided and where they disagree (stage 1) | not stored (document survey) |
| [ecosystem] | Framework, licence, signing and precedent-product facts as of 2026-10-05 (stage 1) | not stored (web sources, cited inline) |
| [parity-v1] | First pedal–plugin parity study (stage 1): contraction simulated on x86, libm differences, rate dependence, data layout; MSVC and arm-none-eabi-gcc 10.3 | not stored; superseded by [prototype] |
| [challenge] | Adversarial challenge of stage 1, which re-ran the key probes, refuted or corrected several claims and added missed findings (libDaisy SLA0044, time-parameter clicks, bounce reproducibility, clipping above 0 dBFS, GPLv3 Installation Information). The profile cites the same source under the same label. | not stored |
| [prototype] | Determinism-profile prototype (stage 2): in-tree math plus contraction off, a 32-build battery including emulated Cortex-M7 code, costs, negative controls | `prototype/` (harness, patches, `hashes/`) |
| [fp-isa] | Floating-point semantics across x86-64, arm64 and the Cortex-M7 (stage 2): flush modes, NaN, casts, ODR, pragmas, guard design | `fp-semantics/` |
| [oracle] | M7 verification oracle (stage 2): QEMU user mode, the `mps2-an500` full-system plan, hardware-in-the-loop plan, ring-size tests | `oracle/` (harness, syscall shim, `logs/`) |
| [juce] | JUCE integration (stage 2): JUCE 9.0.3 source read at the tag; parameter normalisation, VST3 automation timing, ODR and LTO probes | not stored (read JUCE source); ODR probe in `fp-semantics/odr/` |
| [preset] | Preset pipeline and upload (stage 2): state and load-order probes, exhaustive float-formatting probe, libDaisy licence and mock-link probes | `preset/` (mock-link probe not stored) |
| [bugcheck] | Block-split verification (stage 3): harness, 32-subset matrix, instrumented copies counting write-ahead reads, the three-part fix and both freeze-mark variants, regression test | `bugcheck/` (`bugcheck.cpp`, `fix.diff`, `fixB-over-fix.diff`, `results/out-*.txt`) |
| [review-num] | Numerics review (stage 4): write-ahead and old-invariant counters, Spillover as specified, DetMath domain edges, DAZ canonicalization, MSVC symbol audit | `review-numerics/` (`spill.cpp`, `inv.cpp`; raw outputs not stored) |
| [review-fw] | Firmware and licensing review (stage 4): libDaisy and DaisyBootloader source at HEAD, a mock link of `system.cpp`'s USB handlers, TinyUSB releases checked tag by tag | not stored (third-party sources) |
| [review-juce] | JUCE hosting review (stage 4): JUCE 9.0.3 and clap-juce-extensions source, a float-normalisation probe, a PIC probe on the arm toolchain | `juce/` (`floatnorm.cpp`) |
| [review-pc] | Product-consistency review (stage 4): a second PIC probe measuring GOT relocations | not stored |

**Environment.** All desktop measurements were taken on one Intel i9-12900K under Windows 11
with MSVC 19.40 (VS 2022 17.10). GCC 14.2 and Clang 14 ran in Docker with no network.
Cortex-M7 code ran under `qemu-arm` 10.2.3 with `QEMU_CPU=cortex-m7` through Docker Desktop's
binfmt handler. Firmware code was compiled with GNU Arm Embedded Toolchain 10.3-2021.10
(`arm-none-eabi-gcc` 10.3.1) unless stated.

**Where the probes live.** The probes worth keeping are committed under
[`tools/parity/`](../../../tools/parity/), whose README explains how to rebuild them (modified engine
copies are stored as patches against `e86e971`). Sources marked "not stored" depended on
third-party source trees or were one-off audits; their method and results are written up here.

## 2. Evidence by design section

### 2.1 Feasibility and cost (design §1)

**The 32-build battery** [prototype]. A scratch copy of `dsp/` with every libm transcendental
replaced by in-tree, correctly rounded kernels and contraction off produced one SHA-256 over
10 presets × 30 s on: MSVC 19.40 with `/fp:precise`, `/fp:strict`, AVX2 and LTCG; GCC 11, 12
and 14 at `-O2`/`-O3` on baseline x86-64 and on x86-64-v3; Clang 14; GCC and Clang with LTO;
and the firmware code generation (arm-none-eabi-gcc 10.3.1 and 12.3.1, `-mcpu=cortex-m7
-mfpu=fpv5-d16 -mfloat-abi=hard -O3 -ffp-contract=off`) executed under QEMU 10.2.3 with
`QEMU_CPU=cortex-m7`. A 60 s battery with a long silent tail also matched. Every negative
control (contraction on, fast-math, today's firmware flags) diverged.

**Today's code** [prototype]: the emulated pedal build against the MSVC build nulls at −1.3 to
−5 dB on jittered presets, −19 dB on the all-post-stages preset and about −89 dB on a clean
delay. Nothing in the battery is sample-identical.

**Why contract #7 could not hold** [parity-v1]: with `scheduler.jitter` above 0 (the default is
0.2, `dsp/include/brainscape/Params.h:72`), two builds that differ in fusion decorrelate into
different grain patterns from about second 1: null depth −4.8 dB, correlation 0.83.

**Costs** [prototype]: in-tree math on x86 is not measurable (realtime factor 79–81× against
78–81×). Contraction off on x86 is 11–13 % slower than a contracted AVX2 build. On the M7,
contraction off adds 3–17 % instructions in hot loops (disassembly); the CPU figures (≈+1 % at
typical voice counts, ≈+4–5 % at 64 voices) are estimates that need on-chip cycle counter
(DWT) measurement. Engine flash: `.text` 19.6 KB plus 10.1 KB of libm today, 27.5–34.8 KB with
no libm (`arm-none-eabi-size`).

**Per-birth in-tree math.** About +300 cycles per birth, estimated from measured kernel
instruction counts. An earlier estimate in this design ("≈0.06 % at 530 births/s") had read the
design's ~530 **cycles per sample** for `ScheduleGrain` (`grain-engine.md:524`) as a birth rate.
The code allows one scheduler birth per sample (`dsp/src/Engine.cpp:309-314`,
`dsp/src/Granular.cpp:321`), 48,000 births/s at 48 kHz, which gives ≈+300 cycles/sample, about
3 % of the 10,000-cycle budget. One review computed 64,000 births/s from the design's
voices-per-size formula; the code caps the rate at one per sample, and both documents use
48,000/s (profile §3.9).

### 2.2 Product shape (design §2)

- **Rate dependence** [host], [parity-v1]: a 100 ms delay repeats every 110.667 ms at 48 kHz
  and every 105.333 ms at 96 kHz; a 5 ms resonator self-oscillates 4.16 semitones sharp at
  96 kHz.
- **Platforms** [ecosystem]: JUCE targets Windows, macOS and Linux; iPlug2 has no Linux target.
  Under ARM64EC the denormal guard would take its SSE branch through an emulated MXCSR whose
  effect on the real FPCR is unverified.
- **Format facts** [ecosystem], [juce]: the VST3 SDK became MIT-licensed with 3.8.0 (October
  2025) and JUCE moved to it in 8.0.11; JUCE 9.0.3 has no native CLAP despite its 2024 roadmap;
  `clap-juce-extensions` broke at JUCE 8.0.11.
- **Standalone device setup** [juce], [review-juce]: the holder passes both its saved
  `audioSetup` XML and its preferred options to `AudioDeviceManager::initialise`
  (`Standalone/juce_StandaloneFilterWindow.h:333-363`); whenever the XML exists,
  `initialiseFromXML` starts from the preferred setup and then overwrites its rate with the
  saved `audioDeviceRate` (`juce_AudioDeviceManager.cpp:395-397`, `:480-515`), and the current
  rate is saved back (`:1001`). Passing a 48 kHz preferred setup is therefore not enough.
- **Raw host buffers before the block-split fix** [bugcheck], [host]: after one pluck and
  87.5 s of silence, a mark-positioned reverse preset diverged at host blocks of 127 and 512
  but not at 48. Presets with live positioning and no freeze held past 65.5 s reach none of the
  three mechanisms (record §2.8) and did not diverge in any probe (record §2.4, "Chunking"),
  but the profile's contract excludes real-time plugins on arbitrary buffers before the fix
  (profile §2.1, §2.4). Design §2.3 therefore reports that class as an observation and promises
  identity in a DAW only on the pedal grid until the fix lands.
- **Bounce reproducibility** [challenge]: two bounces separated by `Reset` + `ClearHistory`
  differed in 265,908 samples; after a re-`Init`, 0.
- **Device link** [juce], [ecosystem]: classic WinMM MIDI ports are single-client before Windows
  MIDI Services (Windows 11, February 2026). Whether a sandboxed or out-of-process AU can open
  the pedal's port is an inference, not a test. Precedent: Line 6 Helix Native with HX Edit,
  Neural DSP with Cortex Control, IK TONEX.

### 2.3 Build, CI and licensing (design §3)

- **Inline functions compiled twice** [fp-isa], [juce]: `Engine.h` includes the `detail/`
  headers (`dsp/include/brainscape/Engine.h:7-10`), which define inline floating-point code
  (`detail/Smoother.h`, `detail/PostChain.h`, `GrainMath.h`). A JUCE translation unit using one
  of them under different flags emits a competing copy, and when the function is emitted out of
  line the linker keeps whichever copy comes first: with the plugin object first, the engine's
  call site ran the plugin's fused copy, on GCC 14 and MSVC.
- **LTO** [juce]: under GCC 14 `-flto`, a strict engine function inlined into a contracting
  caller took on the caller's contraction in **both** link orders; under MSVC `/GL` + `/LTCG`
  the result depended on link order. The prototype measured LTO with mixed per-file flags as
  safe; both are right, depending on whether engine code can be inlined into a contracting unit
  (profile §3.4).
- **Linux shared objects** [host]: without PIC the static `dsp/` library cannot link into a
  `.so`; with `-fPIC` alone, the parameter table became a process-wide `STB_GNU_UNIQUE` symbol,
  and a second Brainscape build loaded in the same process read the first build's table
  (g++ 14.2).
- **PIC on the M7** [review-juce], [review-pc]: the same target properties under
  `tools/cmake/arm-none-eabi-toolchain.cmake` add `-fPIC -fvisibility=hidden
  -fvisibility-inlines-hidden` to `arm-none-eabi-g++`. `Engine.cpp` compiled that way had 8 GOT
  relocations against 0, `.text` went from 6,379 to 4,683 B and `.data` grew by 672 B (GNU Arm
  10.3.1), a different archive from the one the oracle measured. An earlier draft of the
  profile put these properties on `brainscape_dsp` itself.
- **JUCE needs C** [review-juce]: JUCE 9.0.3's `CMakeLists.txt:35-42` reads the globally enabled
  languages before its `project()` call and fails with "A C compiler is required…"; 22 module
  sources are `.c` (zlib, FLAC, Ogg, libpng, SheenBidi).
- **Flags** [juce], [prototype]: MSVC predefines `_M_FP_CONTRACT` under both `/fp:contract` and
  `/fp:fast`. The firmware build today emits 152 fused multiply-adds (Engine 13, Granular 35,
  OnsetDetector 6, PostChain 98; `arm-none-eabi-objdump`). Clang 14 targeting
  `arm64-apple-macos11` fused by default [fp-isa]; the existing `macos-latest` CI leg was not
  inspected, so "it contracts today" is an inference.
- **Build cost:** `juceaide` takes ~45 s on Windows CI according to a forum report; not
  measured here.
- **CI cost** [oracle]: the M7 oracle ran an 8-preset × 10 s battery in 9.8 s, about 8×
  realtime.
- **Licensing** [ecosystem], [juce]: since JUCE 8.0.0 every module is under the AGPLv3/EULA
  dual licence; JUCE 9 ignores `JUCE_DISPLAY_SPLASH_SCREEN` with a compiler warning; JUCE
  9.0.2+ ships an SPDX bill of materials (`JUCE.spdx.json`). Licence compatibility of the other
  desktop dependencies follows the FSF licence list; design §3.5 lists each dependency's
  licence, including the ASIO SDK's GPLv3 licence as bundled by JUCE 8.0.11 and later and the
  one-way Apache-2.0 compatibility of the AudioUnitSDK. Since macOS Sequoia users can no longer
  Control-click past Gatekeeper. Microsoft Artifact Signing is open to individuals in the US and
  Canada only.

### 2.4 Engine hosting (design §4)

- **Isolation** [host]: 16 interleaved instances and two loaded copies of the module matched a
  solo engine bit for bit; there is no mutable global state.
- **Memory and time** [host]: per engine at 48 kHz, Hot 20,480 B, Warm 129,680 B, Bulk
  17,545,216 B, 16.88 MiB in all, and `Init` touches every page because it clears the ring. One
  processor uses ~17 MiB, sixteen ~270 MiB; with the spare engine, ~34 MiB and ~540 MiB
  (calculated). `Init` takes 3.2 ms on first touch and 1.1–1.4 ms warm. After a re-`Init`,
  `DelayMs` read 250 instead of the 1234 set before it.
- **CPU** [host], [challenge]: a deliberately dense preset (64 voices, +24 st, 2 s spray, every
  post stage, feedback 1.05, onset triggering) ran at 59–61× realtime at 48 kHz (MSVC
  Release), about 1.7 % of one core.
- **Resampler**: `juce::WindowedSincInterpolator`'s 201-tap kernel is not rescaled when
  downsampling, so 48 → 44.1 kHz aliases 22.05–24 kHz content (`juce_Interpolators.h:49-104`).
  Latency of ≈1.5 ms per direction for a 100 dB, 20 kHz-passband linear-phase design is a
  Kaiser-formula estimate.
- **Chunking** [host], [plan-of-record]: host blocks of 1 to 8,192 frames chunked to ≤ 512 gave 0
  differing samples against native 512-frame blocks, and host blocks of 1024, 1000 and 37 gave
  0 of 576,000 differing samples against a pedal-style 48-frame stream. Those probes used
  presets the block-split bug does not reach (no freeze past ¾ of the ring, no far-rail
  positions, no mark positioning under freeze).
- **Zero-frame calls**: a Debug `Process(numFrames = 0)` hits the assert at
  `dsp/src/Engine.cpp:379` and kills the DAW [host].
- **Mono aliasing** [host], [challenge]: 48,000 of 48,000 right-channel samples wrong, maximum
  |Δ| 0.16–0.23. Stereo in place: 0 differing samples.
- **Tails** after a 0.5 s noise burst [host]: defaults 0.27 s; feedback 0.95 reaches exact zero
  after 4.43 s; a post delay at feedback 0.9 takes 208 s to reach −120 dBFS and never reaches
  exact zero; reverb time 1 takes 36.9 s to −120 dBFS; feedback 1.1 on a clean delay sustains
  forever; freeze holds 65.6 s. The VST3 infinite-tail mapping was read in source
  (`juce_audio_plugin_client_VST3.cpp:3481-3492`). In `clap-juce-extensions`, JUCE's
  `roundToIntAccurate` adds 6755399441055744.0 to infinity, which stays infinity and whose low
  32 bits are 0 (`juce_MathsFunctions.h:627-667`).
- **FP environment**: a host thread left in round-toward-zero changed every golden hash on both
  ISAs [oracle] and produced a −26 dBFS null [juce]. A render with FTZ|DAZ already set by the
  caller was bit-identical even with today's OR-ing guard [juce]. No JUCE wrapper sets the
  denormal mode itself (code search) [juce], [fp-isa].
- **Thread safety** [host]: the any-thread calls showed 0 data races over 149 M iterations
  under ThreadSanitizer. That straddling a block with 28 separate `SetParam` stores is audible
  is an inference.
- **Input** [host], [challenge]: the dry path is bit-exact at Mix = 0 and passes values above
  0 dBFS; a probe gave an output peak of 2.0 at Mix = 0 and 1.0 at Mix = 1, the wet path
  clipping in the int16 ring. libDaisy's 24-bit input scale literal rounds to exactly 2⁻²³ in
  binary32 (`0x34000000`, checked with `struct.pack`); its `f2s24` clamps at ±0.999985, about
  126 LSB below full scale, and truncates toward zero (profile §2.2 cites the libDaisy lines).
  The Microcosm defaults to mono input and makes stereo a global setting
  (`docs/research/microcosm.md:124-131`).
- **Restart** [preset]: on a running engine with 10 s of prior history, `Reset` +
  `ClearHistory` + counter rebase matched a fresh `Init` for feedback-free presets but nulled at
  only −51 to −56 dB for feedback presets; adding the tamer-buffer clear made all four presets
  identical over all 1,920,000 frames. `FeedbackTamer::Reset` clears only filter states and
  leaves its allpass buffers to decay (`dsp/src/PostChain.cpp:75-79`); `ClearHistory` clears
  only the ring and post buffers (`Engine.cpp:230-234`).
- **`Restart` time on the pedal** [review-fw]: libDaisy runs the SDRAM on a 32-bit bus at
  SDCLK = 200 MHz (PLL2R) / 2 = 100 MHz (`src/dev/sdram.cpp:76`, `:80`; `src/sys/system.cpp`,
  PLL2R = 1 and `FmcClockSelection` = PLL2), a 400 MB/s peak, so clearing the 16 MiB ring plus
  the 0.73–0.75 MiB post delay (17.56 MB) takes at least 43.9 ms (calculated). The grain-engine
  design's two figures disagree: "~40–80 ms" for the ring (`grain-engine.md:571`) is below that
  floor, while 0.3–0.6 s for all of Bulk (`:491-492`) scales to 79–159 ms for 16.75 MiB. The
  restart watermark reuses the design's looper-undo technique (`grain-engine.md:494-496`);
  clearing the post delay alone is estimated at 2–4 ms.
- **`reset()`** [host]: output on silent input was still audible 266.7 ms after `Reset`, the same
  as without it. `ClearHistory` takes 0.92 ms on the desktop.
- **Events** [preset], [challenge], [host]: one Pitch+Mix change applied at block start made a
  48-frame and a 512-frame render differ (−55.8 dB null around the event); splitting the block
  at the event's frame made them identical over all 480,000 frames. Parameter, trigger and
  freeze events at odd frames under random host blocks of 1–4,096 frames gave 0 of 576,000
  differing samples against a 48-frame reference. Overhead: 121× realtime with 8-frame splits
  and parameter changes on every call, against 131× unsplit; 75× with 1-frame splits.
- **Codec input conversion**: verified from libDaisy v9 source in the review (profile §2.2).

### 2.5 Parameter layer (design §5)

**APVTS round trip** [juce]. Every APVTS load goes plain → normalised → plain (`setNewState` →
`setDenormalisedValue` → `setValueNotifyingHost` → `AudioParameterFloat::setValue` →
`convertFrom0to1`), and the raw value is read back as `denormalise(getValue())`.
`setDenormalisedValue`, `parameterValueChanged`, `flushToTree` and
`AudioParameterFloat::operator=` skip changes within `approximatelyEqual`. Measured over every
float in each distinct range of the 28 parameters, using JUCE's own formulas:

| Range | Values changed by the round trip |
|---|---|
| [1, 5000] ms | 12.14 % |
| [0, 1.1] | 8.19 % |
| [40, 20000] Hz | 11.15 % |
| [0, 3] | 17.51 % |
| [−24, 24] | nearly all values near zero collapse (98.99 %), and 52.4 % of values typed with three decimals |
| [0, 1] | exact |

Every error is 1–2 ULP, inside `approximatelyEqual`, so JUCE never notices. A build that fuses
the `start + (end − start)·p` step (the Apple Silicon default) gives different results from one
that does not: 968,166 differing values in [1, 5000]. Sending a real preset through the round
trip changed two of its 18 values (`PitchSt` 7.02 → 7.02000046, `FilterMorph` 0.4 →
0.400000036), and the render diverged from 0.332 s with a −118.5 dBFS null (MSVC).

**`SliderParameterAttachment`** [review-juce]. In JUCE 9.0.3 it parses typed text through
`getValueForText()` (a normalised float) and `convertFrom0to1()`, converts the slider value with
the non-virtual `RangedAudioParameter::convertTo0to1()` through `getNormalisableRange()`, drops
edits within `approximatelyEqual` of the current value, and displays `convertFrom0to1(lastValue)`
(`juce_ParameterAttachments.cpp:90-98`, `:117`, `:127-128`, `:131-169`;
`juce_ParameterAttachments.h:114`). Carried as a float normalised value, even with the taper
computed in double, three-decimal values that do not survive (`floatnorm` probe, MSVC): 11.49 %
in [1, 5000], 47.16 % in [−24, 24], 10.93 % in [0, 3], 11.16 % in [40, 20000]; [0, 1] is exact,
and [1, 500] and [10, 2000] lose under 1 %.

**CLAP** [review-juce], `clap-juce-extensions` at commit `55525c9858`: with the default
`CLAP_USE_JUCE_PARAMETER_RANGES` = OFF it advertises every parameter as 0–1 and reports
`getValue()`, the JUCE normalised value (`clap-juce-wrapper.cpp:174-179`, `:1383-1404`,
`:1445-1450`). `CLAP_PROCESS_EVENTS_RESOLUTION_SAMPLES` / `CLAP_ALWAYS_SPLIT_BLOCK` split the block
at event times, but values still pass through JUCE's normalised float (`:1505-1523`); a processor
reporting `supportsDirectProcess()` receives raw CLAP events at their offsets, but with OFF they
carry the same normalised values, and with ALL the reported "plain" value is
`convertFrom0to1(getValue())`, the image of a `NormalisableRange<float>` (`:719-757`,
`:1514-1520`).

**Other facts.** Descriptors contain pointers, which make the struct 40 bytes on x64 and 24 on
the M7 [parity-v1]. The DAZ comparison measurement behind bit-test canonicalization is in
profile §3.7 [review-num].

### 2.6 Preset model (design §6)

- **Load order** [preset]: `Init` → set all values → `Process` (smoothers glide from the
  defaults) against `Init` → set all values → `Reset` (smoothers snap) differed by −56 to
  −69 dB and reconverged after ~0.7 s; with feedback, the difference recirculated to the end of
  a 40 s render.
- **Delta loads** [preset]: applying only a preset's own values on top of the previous preset
  left unmentioned post-chain parameters (`ReverbMix`, `DelayMix`, `ModDepth`) at their old
  values: −4.0 to −6.4 dB null for the whole 40 s on every preset.
- **Layout** [parity-v1]: fixed-width POD layouts were byte-identical between x64 MSVC and M7
  GCC; pointer, `size_t` and `long` sizes differed.
- **Number formatting** [preset]: an exhaustive probe over all 4,278,190,080 finite floats found
  exactly two values, ±7.038531e-26, whose shortest round-trip string reads back wrong through a
  double-based parser. Shortest strings average 12.5 characters.
- **libm in compilation** [parity-v1]: on today's code UCRT's `exp2f` differs from a correctly
  rounded result by 1 ULP on 0.063 % of the semitone grid.
- **newlib `strtof`** [preset]: disassembly of `libc.a` and `libc_nano.a` shows `_strtod_l`
  followed by `vcvt.f32.f64`, decimal → double → float, rounding twice. A probe string parsed to
  `0x3f800001` correctly and to `0x3f800002` through newlib's algorithm. Floating-point
  `std::from_chars`/`std::to_chars` arrived in GCC 11; `arm-none-eabi-g++` 10.3.1 lacks them.
- **Spillover convergence** [preset], [review-num]. The stage-2 claim that feedback-free presets
  reconverge with a canonical render 0.30–0.43 s after a Spillover load measured a different
  load: that probe called `Engine::Reset()` and rebased the counter at the load
  (`tools/parity/preset/probe_state.cpp:205-206`). `Reset` kills every grain,
  re-arms the scheduler (`detail/Granular.h:75-84`), zeroes the feedback FIFO
  (`Engine.cpp:210`) and resets the detector; Spillover as specified keeps all of that. The
  review measured both on a repository copy with an RNG epoch, rendering 10 s of a prior preset,
  then the load, then 30 s in 48-frame blocks:
  - default preset (jitter 0.2, feedback 0), Spillover as specified: never reconverged; the
    last differing frame was the final frame, 2,876,160 samples differing;
  - the same with `Reset` + epoch at the load: identical 0.264 s after the load;
  - a periodic preset (jitter 0, spray 0, feedback 0): never as specified; 0.250 s with `Reset`.

  The stage-2 feedback findings (feedback 0.6 reconverged only 6.75 s into silence; a
  reverb/shimmer tail still differed after 60 s) also used `Reset` semantics. Without the epoch
  restart every preset diverged permanently (−1.3 to −21.6 dB).
- **Default load mode**: the Microcosm's most-cited preset complaints are that switching cuts
  trails and interrupts loops (`docs/research/microcosm.md` §7, §11).

### 2.7 Device link (design §7)

**Transport.** Each USB-MIDI virtual cable appears as its own MIDI port on Windows, macOS and
Linux (Microsoft's port-naming documentation, microsoft.github.io/MIDI, "How MIDI 1.0 port names
are generated"; Blokas' USB-MIDI port-mapping notes); under Windows' legacy naming (Windows 10,
and Windows 11 when the device supplies no per-jack strings) the second port is `MIDIIN2
(Brainscape)` / `MIDIOUT2 (Brainscape)`. A USB serial (CDC) link would need ~300 lines of native
code or libserialport (LGPL), since JUCE has no serial or bulk API. USB-MIDI 1.0 carries 3 SysEx
bytes per 4-byte event. Windows MIDI Services on Windows 11 25H2 corrupts inbound SysEx with
`usbaudio.sys` (microsoft/MIDI issue 1040).

**SLA0044** [preset], [review-fw]. libDaisy is MIT at the top level, but `src/usbd/usbd_conf.c`,
`usbd_desc.c` and `usbd_cdc_if.c` carry "licensed by ST under Ultimate Liberty license SLA0044",
and the patched class code defers to the ST USB device library's own licence, also SLA0044.
Clause 5 forbids redistribution "in any manner that would subject this software to any Open
Source Terms" and names the GNU GPL; clause 4 restricts use to ST devices.

**`system.cpp` links ST USB code into every firmware** [review-fw]. `src/sys/system.cpp:96-121`,
the object that holds `System::Init` and `SysTick_Handler`, defines strong
`OTG_HS_EP1_OUT_IRQHandler`, `OTG_HS_EP1_IN_IRQHandler` and `OTG_HS_IRQHandler`. They reference
`hhcd_USB_OTG_HS` and `hpcd_USB_OTG_HS`, defined in `src/usbh/usbh_conf.c:38` and
`src/usbd/usbd_conf.c:42`, and call `HAL_HCD_IRQHandler` and `HAL_PCD_IRQHandler`, whose weak
callbacks those files override (`usbh_conf.c:145-201`, `usbd_conf.c:209-391`) with calls into
ST's host and device cores. The vector table is kept by the linker script
(`core/STM32H750IB_qspi.lds:27-32`). A mock link reproducing this structure
(arm-none-eabi-gcc 10.3.1, `-ffunction-sections -fdata-sections -Wl,--gc-sections`): an app
that only calls `System::Init` pulled in `usbh_conf.o`, `usbh_core.o`, `usbd_conf.o` and
`usbd_core.o`; the same app plus a GPL file defining the two handles pulled in none of them; and
a TinyUSB-style `OTG_HS_IRQHandler` failed with "multiple definition of `OTG_HS_IRQHandler`". The
stage-2 mock link modelled only `fatfs.cpp`, not `system.cpp`, and missed this.

**libDaisy USB function** [preset]: CDC or MIDI chosen at run time by a `usbd_mode` switch in the
patched CDC class, never composite; Full-Speed only; its MIDI parser truncates SysEx at 128 bytes
(`SYSEX_BUFFER_LEN 128` in `src/hid/MidiEvent.h`, "TODO: make this adjustable"; the excess is
dropped in `src/hid/midi_parser.cpp` with no error).

**SD glue** [preset]: `src/util/sd_diskio.c` is SLA0044; `src/util/usbh_diskio.c` is SLA0044 and
calls into the ST USB host stack; `FatFSInterface::Init` references both. A mock link with
`-ffunction-sections -Wl,--gc-sections` and an app requesting SD only still pulled in
`USBH_Driver`, `USBH_MSC_Read` and `hUsbHostHS`. `src/util/bsp_sd_diskio.c` has no licence header
but is visibly a CubeMX template.

**TinyUSB** [preset], [review-fw]: an upstream board package `hw/bsp/stm32h7/boards/daisyseed`;
community reports of TinyUSB composites (audio plus two CDC ports) on a Daisy Seed.
`tud_midi_n_stream_read` contains `(void) cable_num;` in every release
(`src/class/midi/midi_device.c`); `tud_midi_n_demux_stream_read` first appears in 0.21.0
(released 2026-06-30) and is absent at 0.18.0, 0.19.0 and 0.20.0 (checked tag by tag); its
header says not to mix the two read styles. Windows updates KB5077181 and KB5074105 split
outbound SysEx across transfers (Microsoft Q&A thread 5828318).

**Bring-up** [review-fw]. The stage-2 bring-up list had two errors, corrected in §7.2. The
TinyUSB board package's `SystemClock_Config` uses `HSE_BYPASS`, PLLM = 5, PLLN = 160 and
PLL3M/N/Q = 25/336/7 while declaring `HSE_VALUE` = 16 MHz, which on the Seed's 16 MHz crystal
gives a 256 MHz system clock and a 30.72 MHz USB clock (calculated), and it links for internal
flash. PLL3 is libDaisy's audio PLL (M/N/P/Q = 6/295/16/4, `src/sys/system.cpp:489-492`, SAI
clock source `:501-502`), whose Q output is about 196.7 MHz, so PLL3Q cannot clock USB.

**SD timing and power loss.** The SD Physical Layer Simplified Specification v4.10 allows a
single write to stay busy up to 250 ms on SDHC and 500 ms on SDXC cards, and real cards are
reported to exceed it (LKML discussion). An interrupted program operation on a consumer card can
corrupt other data in the same flash erase block (1–4 MB on typical cards) through paired pages
and the card's mapping tables (Ts'o and Landley on erase-block power-fail corruption,
yarchive.net `flash_card_errors.html`; USPTO 10809943 background).

**DaisyBootloader** [review-fw], source at HEAD with libDaisy's `src/sys/ffconf.h`:

- `SearchBin` takes the first root entry whose name merely contains `.bin` or `.BIN` (a `strstr`
  match), skipping only hidden entries and directories. A macOS `._fw.bin` metadata file being
  picked is an inference.
- `EnsureValidBinary` checks only that the stack pointer and entry point lie in valid regions;
  the size checks are commented out and there is no hash. An image equal to QSPI is reported as
  `ALREADY_LOADED` and booted.
- `_FS_EXFAT 0` (`ffconf.h:225`), and `SearchBin` reports "absent" when `f_mount` fails. SDXC
  cards (64 GB and up) ship exFAT-formatted.
- Since v6.1 the card is checked at 1-bit, 12.5 MHz before the timeout on every boot (bootloader
  changelog). It re-flashes only when the file differs from QSPI.
- No rollback: an image that passes the stack-pointer check but crashes before its self-check is
  jumped into again on every boot.

**Identifiers** [ecosystem], [review-fw]: the MIDI Association's SysEx ID policy in force since
15 October 2025 ($240 once, or included in a $600/yr membership); pid.codes' "how to" requires
the modifiable design files to be in the public repository under recognised open licences, which
for hardware projects means the PCB files too; `README.md:42` says the hardware "will likely"
ship under CERN-OHL-S, and `hardware/` holds only `.gitkeep`.

**PARITY ring size** [oracle]. The oracle study proposed rendering PARITY in a scratch engine
with a 2²⁰ ring (4.73 MiB of Bulk memory, measured by `PlanMemory`) beside the live engine; the
preset study muted live audio and reused the main engine, which the design adopts (record §3
item 6). With the three-part block-split fix, ring length reaches the output through the
re-anchor, mark staleness and the far rail (profile §6.4): at 500 ms grains and +24 st reverse
the far-rail strip alone is 120,576 frames, about 2.5 s (calculated), and a freeze held between
16.4 s and 32.8 s re-anchors at 2²⁰ but not at 2²¹ (calculated). The oracle measured that 2¹⁷
changes strum and freeze output and that 2²¹ differs for a 59 s freeze. Its 10 s input produced
only 2 onsets, the reason for per-preset coverage counters.

**SD read stalls**: card-internal garbage collection can stall reads (an inference), the reason for
caching the current bank in RAM; caching all 128 slots would take up to ~2 MiB (estimated).

**USB disk mode** (estimated): ~1 MB/s at Full Speed, so a 23 MB loop takes ~25–30 s; host write
caching plus surprise unplug can corrupt FAT.

### 2.8 The block-split bug, as it bears on the app (design §4.11)

Profile §5.7 is the normative account. The determinism prototype found one preset hashing
differently at block sizes 48, 512 and 7 within one build; the verification [bugcheck]
reproduced it on the unmodified repository, found two further mechanisms and verified a fix
(MSVC Release, float32 output compared bit for bit against a block-1 render, every render split
identically at its event frames).

| | Mechanism | Reachable at the canonical 2²² ring | Measured |
|---|---|---|---|
| **D1** | While frozen the guards are measured from the pin, not the live write head, which keeps recording `age` frames ahead. The mark distance `(anchorFrame − m.frame) & mask_` (`Granular.cpp:82`) wraps to about a ring length for a mark recorded after the pin; the attack length is added after the wrap (`:86-88`) and the bounds use the full ring (`:102-104`), so the distance lands on the far rail (`:106-111`) and the grain starts just after the pin (`:114`). | Freeze + mark positioning + an onset after freeze engages + (reverse probability > 0, or pitch above 0 st, or a large spray: 200 ms broke, 20 ms did not). Spray is not required. | Of the 32 combinations of {spray 20 ms, reverse 0.5, mark, freeze, +12 st}, exactly the 6 containing mark, freeze and (reverse or pitch) broke. A +12 st grain started 1,705 frames behind the live head and overtook it 1,704 samples into its life; a reverse grain started 6,771 frames ahead of it. Up to 39,868 reads ahead of the live head per render. |
| **D2** | The only far-side margin, `kGuardMarginFrames` = 64 (`detail/Granular.h:14`), is smaller than the up-to-511-frame write-ahead (`maxBlockSize` ≤ `kFeedbackDelayFrames` = 512, `Engine.cpp:118`, `Engine.h:24`). | Without freeze: a mark nearly one ring old (about 87 s) passes the staleness test (`Granular.cpp:81`) and its reverse grains clamp to the far rail. On small rings, base delay alone. | "Stale mark" (mark positioning + reverse 0.5, one pluck then 90 s): blocks 127, 512 and the pattern 48, 1, 127, 32 diverge from 87.5 s; block 48 does not. On a 2¹⁵ ring with base delay 678 ms, block 512 diverges from frame 1. |
| **D3** | Re-anchor-on-wrap is decided once per `Process` call (`Engine.cpp:405-414`), so the splice lands on the first block boundary after the threshold. | Any position source, freeze held longer than 65.5 s. | Freeze held 67 s with live positioning: every block size diverges from 66.54 s, about 139,934 samples, with zero reads ahead of the live head. |

**Fix and results.** Part A alone still broke on the D2 and D3 cases. With A + B + C (about
60 lines, `bugcheck/fix.diff`), all 13 named cases and all 32 combinations were bit-identical at
block sizes 1, 7, 48, 64, 127, 512 and the pattern 48, 1, 127, 32, on MSVC Release and Debug and
GCC 14.2 `-O2`; an instrumented copy counted 0 reads ahead of the live head; the suite passed
(MSVC Release 39/39 including the new regression test); and the 26 combinations without both
mark and freeze kept their exact hash at block 48. Part C reproduces the old block-1 output
exactly. Part A's degenerate-case rule (`lo = hi`, so the live-head rail wins) is the opposite of
`ComputeDelayBounds`' own rule, where the near guard wins (`GrainMath.h:75`). Cost: three integer
operations per sample while frozen plus one subtract-and-compare per birth (estimated).

**Freeze-mark variants.** Pin-eligible marks ("Variant B", `bugcheck/fixB-over-fix.diff`) and
live-head marks ("Variant A1", the fix as delivered) both passed; under A1, frozen and unfrozen
mark-positioned output hashed identically.

**Why the existing test missed it.** No block-split comparison engages freeze (the render helper
cannot call `SetFreeze`, `dsp/tests/test_engine.cpp:71-110`); its white-noise input records 0
onsets, so mark positioning never activates despite the comment claiming "the full stochastic
feature set" (`:376-378`); and 4,096 frames with a 100 ms delay on a 2¹⁵ ring reach neither the
far rail nor ¾ of the ring. The regression test written in the scratch copy (after
`test_engine.cpp:1081`) has three sections: the contract-#1 parameter set with freeze at frame
6001 and onsets continuing, 2¹⁵ ring; a freeze held past the re-anchor point, 2¹³ ring; and no
freeze with base delay 678 ms near the far end of a 2¹⁵ ring. All three fail on today's code and
pass with the fix on MSVC and GCC.

**A wrong invariant.** The first report proposed the Debug check "no grain born at sample *t*
reads a frame written at or after *t*". A review counter built to that wording fired 234,036
times in a Strum render that is block-invariant [review-num]; the write-ahead rule of profile
§5.7 is the right test.

## 3. Where the evidence disagreed, and how the design resolves it

The numbering is stable; design §9.2 and other documents cite items by number.

1. **The flush-to-zero bit inside the guard.** The oracle and JUCE studies proposed a guard that
   writes a canonical word **with** flush-to-zero on (x86 MXCSR = 0x9FC0; Arm FPSCR.FZ = 1). The
   floating-point semantics study proposed one **without** it: gradual underflow, the IEEE
   default (MXCSR = 0x1F80, M7 FPSCR = 0, AArch64 FPCR = 0), plus a deterministic in-code flush
   of recursive filter states (|x| < 1e-20 → 0, per sample) and an epsilon snap in the parameter
   smoother. All agree on what matters most to the wrapper: write the whole control word, never
   OR into the host's, around every engine entry point. **Resolution: gradual underflow**, as
   profile §4.2 decides. (a) x86 FTZ and Arm FZ are different functions: x86 decides tininess
   after rounding, Arm before. In 20 M products aimed at the boundary they disagreed on 769,492,
   every one just below FLT_MIN [fp-isa]; on a constructed case x86 returned FLT_MIN where the
   emulated M7 returned 0 [prototype]. x86 raises no underflow flag for those results, so x86-side
   tests cannot see the divergence, and since no render battery ever produced a value in that
   band the hazard is "not provably absent" rather than observed; a 1:1 promise needs the former.
   (b) Compilers constant-fold assuming gradual underflow, so under FTZ a result depends on what
   was folded (`1e-38f * 0.5f` gives a subnormal when folded and 0 at run time) [fp-isa].
   (c) The cost argument for FTZ disappears with the flush: silent-tail CPU under gradual
   underflow matched FTZ mode, and FTZ-forced and IEEE renders were bit-identical on x86
   [fp-isa]. The oracle's proposed invariant, "a render with FTZ forced equals a render
   without", is exactly the test that proves the flush works, so the proposals converge on one
   test. **If the Cortex-M7 is too slow on subnormals** (its cost is undocumented; DWT on a Seed3
   settles it), profile §4.2's acceptance criterion applies: gradual underflow stays if, in the
   silent-tail scenario, CPU at FZ = 0 is at most 1.2× the CPU at FZ = 1, at most 0.5 % of
   silent-tail blocks raise a subnormal flag, and the FTZ-forced render still equals the golden
   hash; otherwise the deterministic flush is widened, which bumps the sound revision but keeps
   identity. **FZ on one target only is ruled out**, because x86 FTZ and Arm FZ disagree in
   [FLT_MIN − 2⁻¹⁵¹, FLT_MIN). An earlier draft of this design proposed FZ on the pedal alone as
   the fallback; it is withdrawn, because the invariant "FTZ forced equals IEEE" is empirical,
   not structural: subnormal flags still occurred in 49–96 of 38,000 silent-tail blocks with the
   flush in place, and the equality was shown on 5 presets × 38 s [fp-isa].
2. **Is the engine's SPSC queue a prerequisite?** The plan-of-record study said yes (otherwise
   automation lands at chunk boundaries); the challenge refuted it (splitting host blocks at
   event frames is bit-exact); the JUCE study then showed JUCE's VST3 wrapper gives only one
   value per block anyway. **Resolution:** frame-stamped events are the requirement, not the
   queue. The profile puts the block split inside `dsp/` and makes the queue the transport for
   stamped events; until then the wrapper splits blocks itself (§4.10). DAW automation is
   outside the contract regardless, and the queue is still needed on the pedal.
3. **Rate-invariant engine or fixed 48 kHz engine.** The stage-1 parity study recommended a
   time-based feedback FIFO and onset detector so that a native-rate plugin matches the pedal's
   character, with a 48 kHz internal mode as an option. The stage-2 JUCE and preset studies,
   written after the owner chose sample identity, require the engine at 48 kHz. **Resolution:**
   48 kHz in every product (§2.3); rate invariance is deferred to phase F and would bump
   `kSoundRevision`. The stage-1 suggestion of a 2²³ ring at 96 kHz falls away with it: the
   canonical 2²² ring is used everywhere, and test oracles may use smaller rings only within the
   validated limits (§7.4).
4. **Where JSON is parsed.** The grain-engine design puts one shared compiler in `dsp/`
   (`grain-engine.md:324-327`) and also says "`dsp/` never parses text"; the preset research says
   firmware and plugin each parse JSON outside `dsp/`
   (`docs/research/preset-parameter-and-patch-format.md:284`). **Resolution:** the compiler's
   source lives in `dsp/src/compiler/` as a separate, desktop-only target; firmware links only
   the binary decoder (§3.1, §6.6). The preset research and the grain-engine design's §5 now
   carry dated notes (2026-10-05) superseding them on this point, and the preset research also
   on QSPI storage.
5. **How to flush an engine for a reproducible render.** The host-readiness audit proposed
   `Reset` + `ClearHistory`; the challenge showed renders still differ because the sample counter
   keeps running; the preset study showed even a counter rebase is not enough for feedback
   presets until the tamer's buffers are cleared. **Resolution:** `Restart` (§4.9).
6. **PARITY on the pedal: scratch engine, or mute and reuse?** The oracle study proposed a
   scratch engine with a 2²⁰ ring beside the live one; the preset study muted live audio and
   restarted the main engine. **Resolution:** mute and reuse in v1, because the SDRAM plan has no
   room for a second engine once the looper exists (§7.4; profile §6.6).
7. **Hash function.** The oracle used FNV-1a-64 internally; the preset study specified SHA-256
   for packages and PARITY. **Resolution:** the user-facing protocol uses SHA-256 (the pedal
   needs it for package verification anyway); the golden format is the profile's (SHA-256 with
   per-second hashes, profile §6.1), and the shared render function computes what both need.
8. **Macro positions in STAT or CTRL.** The preset study put them in STAT. **Resolution:** CTRL
   (§6.2), so they do not perturb `sound_hash`; they do not change an Exact load's output.
9. **Linux PIC and visibility: engine change or build configuration?** The host-readiness audit
   filed it as an engine change; the challenge showed the superbuild can set it before
   `add_subdirectory(dsp)`. An earlier draft of the profile put the properties on
   `brainscape_dsp` itself, and a review probe measured that this adds `-fPIC` and GOT
   relocations to the M7 archive (record §2.3). **Resolution:** build configuration in plugin
   builds only, with a firmware-archive GOT audit; both documents agree (profile §3.5).
10. **Framework and contract #7.** Earlier documents chose iPlug2 and a −120 dBFS tolerance; the
    owner's decisions replace both (§1.1). No evidence argued otherwise after the decisions.
11. **Scope of the block-split fix.** The determinism prototype reported one mechanism with spray
    in the minimal trigger, and both documents' first drafts prescribed only guards measured from
    the live head (part A). The verification found spray unnecessary and two further mechanisms,
    one reachable without freeze and one with any position source, and measured part A alone as
    insufficient. **Resolution:** the verified three-part fix (§4.11; profile §5.7).
12. **Input pipeline.** This design's first draft put every live input block on the 24-bit grid;
    the profile applies only `SanitizeInput` to live input and reserves the grid for
    pedal-faithful renders. The grid would clip a DAW's dry signal at 0 dBFS, re-quantize it and
    break "dry bit-exact at Mix = 0". **Resolution:** the profile's split, with
    `ConditionInput24` defined once there (§4.8; profile §3.7). The binary64 ties-away-from-zero
    implementation of `ConditionInput24` was proposed by this design during the review and
    adopted by the profile.
13. **Parameter grid.** This design's first draft treated a canonical parameter grid as open; the
    profile decided against any grid. **Resolution:** no grid (§5.5).
14. **Spillover convergence.** The preset study reported that feedback-free presets reconverge
    0.30–0.43 s after a Spillover load; a review probe showed that figure was measured with
    `Reset` at the load and that Spillover as specified never reconverges (record §2.6).
    **Resolution:** no convergence claim (§6.7; profile §2.4, §5.9).
15. **Where session capture starts.** An earlier draft of the profile had the pedal restart a
    capture engine; this design starts capture at power-on. **Resolution:** power-on, with no
    `Restart`: `Init` already leaves the counter at 0 and every buffer zero, a `Restart` would cut
    the player's sound, and replay reproduces the whole session, Spillover loads included, from
    its logged events. Both documents agree (§10.2 Q27; profile §6.7).
16. **Mode switches "retried at the next block".** The grain-engine design's publish rule makes
    the applied frame depend on the block grid. **Resolution:** a mode switch is a stamped event,
    with slot staging done before the stamp (§6.1).

## 4. Effort estimate sources

Per-row estimates behind design §8.2. "This design" means the figure is the design's own; other
rows name the study that gave it. Rows marked **(shared)** are also counted in profile §7.1.

| Work item | Estimate | Source |
|---|---|---|
| FP profile target + toolchain flag **(shared)** | 0.5 day | [juce] |
| `Engine::Restart` **(shared)** | ~1 day | [preset] |
| `PresetState`, `DecodePreset`, `LoadPreset`, random-number epoch **(shared)** | 2–4 days | this design |
| `SanitizeInput`, `ConditionInput24` and the test-signal generator **(shared)** | 1–2 days | this design |
| Taper metadata, input-mode function, shared render function | 2–3 days | this design |
| `Engine.h` opaque-storage refactor **(shared)** | 1–2 days | this design ("small refactor" per [fp-isa]) |
| Block-split fix, parts A + B + C, regression test, freeze-semantics variant **(shared)** | 1–2 days | this design; the verified diff and test exist [bugcheck] |
| JUCE FetchContent + CI | 0.5–1 day | [juce] |
| Parameter layer, including the plain-value slider attachment | 2–4 days | [juce] (1–2 days for the parameter class alone) + this design |
| Resampled mode | 1–2 days | [juce] |
| Custom standalone shell (forced 48 kHz, input setup, entitlements) | 2–4 days | [juce] |
| Wrapper hardening: MPSC queue and generation counter, restart rules, pedal-grid option, emulation detection | 2–4 days | this design |
| `plugin/parity-host` and the per-format, Rosetta and Prism legs **(shared)** | 3–5 days | this design (the profile gives 2–4 days for its two parity legs) |
| `.bsp`, canonical JSON, `bspc` (mode compiler excluded) | 1–2 weeks | this design |
| TinyUSB MIDI device on the Seed3, with enumeration tests on three OSes | 1–2 weeks | [preset] |
| libDaisy USB-handler patch and firmware ELF audit | 1–2 days | [review-fw] |
| GPL-clean disk I/O layer | 2–4 days | [preset] |
| A/B slot store | ~1 week | [preset] |
| Slot-store power-loss hardening and relay test rig | 3–5 days | this design |
| Protocol, firmware and app sides | 1–2 weeks | [preset] |
| Long-command job model with cooperative slicing | 2–3 days | this design |
| PARITY command | 3–5 days | [preset] |
| Firmware update flow | ~1 week | [preset] |
| Firmware-update safeguards (file hygiene, FAT check and format, previous image, watchdog) | 2–3 days | this design |
| Firmware render mode for hardware-in-the-loop | 2–4 days after bring-up | [oracle] |
| Device-link UI in the app | 1–2 weeks | this design |
| GUI and editor views | not estimated | no study sized it [challenge] |
| Cash | $240 SysEx ID; $99/yr Apple Developer Program; ~$60 hardware-in-the-loop rig | [preset], [ecosystem], [oracle] |

The subtotal is ≈ 64–111 engineer-days (5 days per week), of which ≈ 55–94 days are in no
shared row; the combined known work is the profile's 28–46 days plus those, ≈ 83–140 days. The
preset study's own total for its part (engine API additions, package and compiler, TinyUSB and
disk I/O, slot store, protocol and PARITY) was 6–10 engineer-weeks, consistent with the
corresponding rows before the review's additions.

## 5. Corrections made during review

Claims in earlier drafts of this design that the evidence corrected, kept so that the reasons
are not lost:

- **Block-split bug:** spray removed from the minimal trigger; live positioning **can** reach the
  bound (D3, and a frozen live-position case on a 2¹⁵ ring); part A alone is insufficient; the
  "no read of frames written at or after birth" invariant is wrong (record §2.8).
- **Per-birth math cost:** "≈0.06 % at 530 births/s" replaced by ≈3 % at 48,000 births/s
  (record §2.1).
- **Flush fallback:** "FZ on the pedal alone" withdrawn (record §3 item 1).
- **Input pipeline:** the 24-bit grid on every live block withdrawn (record §3 item 12).
- **Spillover:** the 0.30–0.43 s reconvergence claim withdrawn (record §2.6). A proposal to
  align the random-number epoch to 512 frames, to keep the feedback FIFO's phase, was withdrawn
  as stricter than needed: the FIFO's delay is exactly 512 frames whatever its phase, so only the
  onset detector's 256-frame hop grid matters (profile §8.3 Q4).
- **`AudioProcessor::reset()`:** an early draft mapped it to `Restart`; it maps to the real-time
  `Engine::Reset()`, as the profile specifies (§4.9).
- **`Restart` on the pedal:** the grain-engine design's "~40–80 ms" is below the SDRAM's
  43.9 ms floor; the design uses ≈45–160 ms (record §2.4).
- **libDaisy USB:** the stage-2 mock link missed `system.cpp`; every libDaisy firmware links ST
  USB code (record §2.7). The stage-2 bring-up list's TinyUSB clock setup and PLL3Q were wrong
  (record §2.7).
- **TinyUSB cables:** two virtual cables need 0.21.0's demux reader (record §2.7).
- **DFU runtime interface:** dropped from v1 because Windows shows it as a driverless unknown
  device on every connect.
- **SysEx framing:** the manufacturer-ID field is variable-length, because the development ID
  0x7D is one byte.
- **Factory packages:** committed twice (JSON and `.bsp`) because one CMake configure cannot run
  a host `bspc` inside the firmware build; a CMake `ExternalProject` building a host `bspc` inside
  the firmware configure was the rejected alternative (more moving parts for the same guarantee).
- **Desktop-only compilation:** besides one compiler and exact decimal parsing, it keeps flash
  and RAM free on the pedal and lets errors be reported on a screen.
- **Package determinism:** META fields come from the JSON, never the clock or the host, so two
  compiles of one file are byte-identical.
- **`SliderParameterAttachment`:** ruled out after the `floatnorm` probe (record §2.5).
- **CLAP:** `CLAP_USE_JUCE_PARAMETER_RANGES` = OFF advertises normalised values, so CLAP tapers
  freeze too; the infinite-tail value needs a CLAP-specific workaround (§4.5).
- **Format wrappers:** linking the profile PRIVATE leaves JUCE's per-format wrapper units outside
  it; PUBLIC is required (§3.3).
- **Standalone:** the saved device rate overrides the preferred 48 kHz (record §2.2); macOS
  needs the audio-input entitlement for notarized builds.
- **DAW identity before the block-split fix:** an earlier draft promised identity on raw host
  buffers for a "safe preset class" (live positioning, no freeze held past 65.5 s). The
  profile's contract excludes real-time plugins on arbitrary buffers before the fix, so the
  design now promises DAW identity only on the pedal grid and reports that class as an
  observation (record §2.2; design §2.3, §10.2 Q20).
- **Parity host location:** the CI parity host links JUCE's plugin-hosting classes, which are
  AGPLv3, so it moved from `tools/plugin-parity-host` to `plugin/parity-host`, where the
  contributor licence rule allows AGPLv3 code; `boundary-grep` now forbids JUCE everywhere
  outside `plugin/` (design §3.1, §3.4, §3.5).
- **Pedal event producers:** an earlier wording made the engine's SPSC queue the transport for
  "main-loop and interrupt producers". A single-producer queue has exactly one producer per
  engine, the pedal's control loop, and interrupt and MIDI sources post to it (design §4.10;
  profile §5.11).

## 6. Provenance

Written on 2026-10-05 from the four stages of record §1. The feasibility investigation
comprised a host-readiness audit (probes with MSVC 14.40 and g++ 14.2), a plan-of-record review
across the documents, ecosystem facts as of 2026-10-05, a pedal–plugin parity study (MSVC and
arm-none-eabi-gcc 10.3 builds) and an adversarial challenge. The decision investigation
comprised the determinism prototype (in-tree math and contraction off; 32 builds, one hash,
including Cortex-M7 code under QEMU), floating-point semantics across x86-64, arm64 and the M7,
the M7 verification oracle (QEMU user mode, the `mps2-an500` plan, the hardware-in-the-loop
plan), JUCE integration (JUCE 9.0.3 read at the tag; normalisation, ODR and LTO probes) and the
preset pipeline and upload path (state probes, the exhaustive float-formatting probe, libDaisy
licence and mock-link probes). The block-split verification ran an independent reproduction on
the unmodified repository at `e86e971`, an instrumented copy counting reads ahead of the live
write head, and the three-part fix and its variants on scratch copies, with MSVC Release and
Debug and GCC 14.2. The review round's four reviews each ran new probes, the self-contained ones
kept under `tools/parity/` (record §1).

The design and the profile were revised together after the review and reconciled so that each
topic has one owner: the profile owns the numeric contract, numerics, the floating-point
environment, the determinism engine changes and verification; the design owns the product,
build, hosting, parameters, presets, device link and the merged plan. Repeated material was cut
from the design and replaced by references to the profile; the evidence it carried is in this
record.
