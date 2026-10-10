# Companion app: preset creation, 1:1 audition, and upload to the pedal

> Design for the desktop side of Brainscape: one JUCE audio processor, built as a standalone
> companion app and as DAW plugins, hosting the same `dsp/` engine as the pedal firmware. Users
> create presets in it, audition them with output sample-identical to the pedal, and upload them.
> [determinism-profile.md](determinism-profile.md) ("the profile", cited as "profile §N") owns
> the numeric parity contract, numerics rules, floating-point environment and denormal policy,
> the engine changes determinism needs, and verification and CI. This document owns the product
> shape, JUCE build and licensing, wrapper hosting, the parameter layer, the preset format, the
> device link and upload, and the merged delivery plan (§8.1).
> Evidence, resolved disagreements and provenance are in
> [reviews/companion-app-record.md](reviews/companion-app-record.md); bracketed labels such as
> [host] name the probe behind a number (record §1). Numbers are *measured*, *calculated* or
> *estimated*. Code and unchanged documents are cited as `path:line` at `main` `e86e971`, JUCE
> at 9.0.3. The documents amended alongside this one on 2026-10-05 (`grain-engine.md`,
> `docs/STATUS.md`, `README.md`) are cited by section or item name, because their line numbers
> moved. Status: **draft v2**, not implemented.

---

## 1. Purpose, and what "1:1" means

### 1.1 The owner's decisions

1. **The companion app is for preset creation on the desktop, with 1:1 recreation of the
   sound against the Brainscape engine, and for uploading those presets to the pedal.**
2. **JUCE is the framework, in one C++ monorepo**: engine, firmware, plugin, app, tools and
   tests in one tree and one CMake build.
3. **"Same sound" means sample-identical output** between pedal and app: the same float bits.

They replace the research's iPlug2 recommendation (`docs/research/vst-and-shared-dsp.md`
rec #1), which STATUS.md's next steps followed until 2026-10-05, with JUCE; and they replace
the −120 dBFS tolerance of the grain-engine design's contract #7 (`grain-engine.md` §10, since
rewritten) with bit-exactness (profile §2.6). #7 was unattainable anyway, since once two builds
differ in multiply-add fusion any jittered preset decorrelates within seconds (*measured*
[parity-v1]). §7 is the "USB/firmware update flow" that `grain-engine.md` §1 ("Scope") deferred.

### 1.2 Definition of "1:1"

**1:1** means: for the same preset, input stream and event stream, the float32 output of
`brainscape::Engine::Process()` inside the app is bit-for-bit equal to that inside the pedal
firmware, at 48 kHz. This is the profile's parity contract (profile §2.1, preconditions §2.3).
This document supplies the desktop half of those preconditions (§3.3, §4, §5, §6, §7.6) and,
until all three parts of the block-split fix land, the pedal's grid: 48-frame blocks from frame 0
with every event on a multiple of 48 (§4.11).

The output is then identical on every conforming build (x86-64 with MSVC, GCC or Clang; arm64
Apple clang; the Cortex-M7 firmware). **Demonstrated** so far: x86-64 and Cortex-M7 code under
emulation (§1.3). arm64, x86-64 under Rosetta 2 or Windows Prism, and STM32H750 silicon are
labelled unverified until a CI leg or a PARITY check (§7.4) covers them (§7.6).

**Not covered**, and the UI says so (§7.6): the analog path; live playing on the pedal, whatever
the load mode (§6.7); Spillover loads (§6.7); host rates other than 48 kHz (§2.3); DAW
playback or bounces on raw host buffers before the block-split fix (§2.3, §4.11); DAW automation
(§5.6); host tempo sync, unless turned into the same event stream (the CLOCK trigger source
does not exist yet: STATUS.md, "Tempo/clock trigger source"; placed in §8.1); lossy or
non-48 kHz input files across machines (§4.9); untested platform combinations; looper content
(not designed, `grain-engine.md` §12 item 12; placed in §8.1). Profile §2.4 gives the shared
reasons. A pedal and an app on **different sound revisions** are not covered either: identity is
a property of the package bytes plus the engine revision on each side, so a package compiled
under r3 is still 1:1 between an app and a pedal that both run r4 (§6.5).

### 1.3 Why this is achievable, and what it costs

A prototype of the profile (in-tree correctly rounded kernels instead of libm, contraction off)
produced **one SHA-256 across 32 builds** over 10 presets × 30 s, including MSVC, GCC, Clang, LTO
builds and the firmware's own code generation under QEMU as a Cortex-M7; every negative control
diverged. Today's code nulls at only −1.3 to −5 dB between the emulated pedal build and MSVC on
jittered presets (*measured* [prototype]; record §2.1).

| Cost (profile §7; in-tree math costs nothing measurable on x86) | Size | Status |
|---|---|---|
| Contraction off, x86 | 11–13 % slower than contracted AVX2; ~80× realtime | *measured* [prototype] |
| Contraction off, Cortex-M7 | ≈+1 % CPU typical, ≈+4–5 % at 64 voices | *estimated* from *measured* instruction counts |
| In-tree math per grain birth, M7 | ≈+300 cycles per birth: ≈0.2 % CPU at 20 ms grains (≈3,200 births/s); ≈+300 cycles/sample, ≈3 %, at the code's maximum of one scheduler birth per sample, 48,000/s (`dsp/src/Engine.cpp:309-314`, `dsp/src/Granular.cpp:321`) | *estimated* (profile §3.9) |
| Firmware flash | 19.6 KB + 10.1 KB libm → 27.5–34.8 KB, no libm | *measured* |

Silicon confirmation comes in phase D. The app adds no numerical requirements beyond §4: it puts
the engine into the same state, feeds it the same input and events, and moves presets without
changing a bit.

### 1.4 Terms

Numerics and engine terms (contraction, ULP, subnormal, FTZ/DAZ/FZ, DWT, golden hash,
write-ahead window, pin, mark positioning, feedback tamer) are defined in profile §1.5.

| Term | Meaning |
|---|---|
| Sound revision, rN | `kSoundRevision`, the `dsp/` constant certifying engine output (§6.5). **Minted** when its golden file is created, **published** when a build carrying it first leaves the project; earlier revisions are **internal**. |
| Exact / Spillover load | Applying a preset from a cleared engine, or on top of the running one with trails kept (§6.7). |
| `.bsp`, `bspc` | The preset package (§6.3) and its compiler (§6.6); sections STAT (leaf values, performance state), MODE (compiled mode), CTRL (controller assignments), META. |
| Mode, `ModeBlob` | The design's structural recipe (layers, pitch sets, macro maps), JSON compiled to a `ModeBlob`, held in a 4-slot ring (`grain-engine.md` §5); FastCut and Trails are its switch styles. |
| APVTS | `juce::AudioProcessorValueTreeState`, JUCE's standard parameter and state container (§5.2). |
| OTG_FS, OTG_HS, DWC2 | The STM32H750's Full-Speed and High-Speed USB controllers, and the Synopsys DesignWare USB core inside both, which TinyUSB's driver targets (§7.2). |
| HAL, SAI, SDMMC1 | ST's hardware abstraction layer (C drivers for the chip's peripherals); the serial audio interface that exchanges samples with the codec through a DMA stream; the SD-card controller. |
| MSC | USB mass-storage class: the pedal presenting its card as a disk ("USB disk mode", §7.7). |
| DFU, DfuSe | USB Device Firmware Upgrade, and ST's extension of it that addresses memory explicitly (§7.6). |
| WinUSB, Zadig | Windows' generic USB driver, and the tool that binds it to a device that does not request it. |

## 2. Product shape

### 2.1 One audio processor, several formats

One JUCE `AudioProcessor`, `BrainscapeProcessor`, with one editor, parameter layer (§5) and
preset library (§6), built with `juce_add_plugin(... FORMATS Standalone VST3 AU LV2)`: the
**Standalone** is the companion app (§2.2); VST3 (bundled SDK, MIT) and AU v2 (Logic hosts
neither VST3 nor CLAP) ship in v1; LV2 and CLAP (through `clap-juce-extensions`, MIT, pinned and
upgraded with JUCE because it broke at 8.0.11) are optional; no AAX (Avid/PACE signing
agreements). Platforms: Windows 10+ x64, macOS (x86_64 and arm64), Linux x86-64. Native
Windows-on-ARM is not v1: the guard has no MSVC ARM64 branch (`DenormalGuard.h:9-24`) and ARM64EC
is out of profile (profile §3.8). The x64 build under Prism and the x86_64 slice under Rosetta 2
get CI legs (§3.4) and a run-time label (§7.6).

### 2.2 The standalone is the companion app

A custom standalone (`JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP=1`, supplying
`juce_CreateApplication()`; `juce_audio_plugin_client_Standalone.cpp:222-230`) hosts the same
processor through `StandalonePluginHolder` and adds a forced 48 kHz device with a
**Pedal-exact**/**Resampled** indicator (§4.2), input setup (§4.8), the preset library (§6),
offline audition of a DI file (a dry, direct-input recording) or the test signal, A/B'd against
the pedal's PARITY hashes (§4.9, §7.4), and the device link (§7). A separate app hosting
`Engine` directly is rejected: it would re-implement that plumbing and drift from the plugin.

The shell overrides three JUCE defaults:

- **The saved device rate wins over the preferred one** on every launch after the first
  (`juce_StandaloneFilterWindow.h:333-363`, `juce_AudioDeviceManager.cpp:480-515`; record §2.2).
  The shell sets 48 kHz through `setAudioDeviceSetup` after start-up, re-derives Pedal-exact or
  Resampled after **every** device restart (a `ChangeListener` on the device manager, or
  `audioDeviceAboutToStart`), and restricts or annotates the rate selector.
- **Input is muted by default** (`juce_StandaloneFilterWindow.h:98-99`): "input on" is an
  explicit step with a feedback warning (macOS also needs the entitlement of §3.2).
- **Both input channels are enabled** (`juce_AudioDeviceManager.cpp:517`): expose channel
  selection and the input mode (§4.8), or a guitar on input 1 feeds `R = 0`.

### 2.3 What the DAW plugin does and does not promise

Plugins author presets, keep them in the DAW project and export `.bsp` files; they do not talk
to the pedal (§2.4). **The engine always runs at 48 kHz**: the pedal is 48 kHz only
(`grain-engine.md` §11), and constants such as the 512-frame feedback FIFO and the onset hop are
counted in frames, so at 96 kHz a 100 ms delay repeats every 105.333 ms instead of 110.667 ms
(*measured* [host], [parity-v1]).

| Host rate | Behaviour | Latency | Promise |
|---|---|---|---|
| 48 kHz | engine on host buffers, chunked (§4.3) | 0 | **Once the block-split fix lands:** identical to the pedal **for a static preset from a known start, given the same input**, that is an offline bounce, or a transport start with "Restart on transport start" (§4.9). **Until then, identity is promised only on the pedal grid (next row).** Observed, not promised: on raw host buffers, presets with `PositionSource` = Live and freeze never held past 65.5 s (¾ of the 87.4 s ring) reach none of the bug's three mechanisms and did not diverge in any probe (*measured* [host], [bugcheck]), while mark positioning with reverse grains diverges once a mark is about one ring old, even without freeze (*measured* [bugcheck]). |
| 48 kHz, **pedal grid** option (until the fix) | FIFO; engine on 48-frame blocks from the restart frame | 48 samples reported (47 is the minimum, profile §2.1) | The row above's promise, for every preset, before the fix lands. |
| other | engine at 48 kHz behind a two-way resampler (§4.2) | ≈3–4 ms reported (*estimated*) | Internal 48 kHz stream identical, host-rate output not; the same character as the pedal (same feedback period, same onset timing); UI shows **Resampled**. |

Not promised: **automation**, because JUCE's VST3 wrapper applies only the last point per
parameter per host block (`juce_audio_plugin_client_VST3.cpp:3495-3540`), before the block's
audio is processed (`:3562-3590`), and drops `approximatelyEqual` changes (`:827-834`), so
automated renders depend on the host buffer size (§5.6); and **reproducible bounces without
the restart option**, because `reset()`, called from `setProcessing(false)` (`:3473-3477`), maps
to `Engine::Reset()`, which does not rewind the counter that keys every random draw
(`dsp/src/Engine.cpp:203-228`): two bounces differed in 265,908 samples (*measured*
[challenge]).

### 2.4 Why the device link lives only in the standalone

Classic WinMM MIDI ports are single-client (Windows 10 stays so), so a DAW holding the port
blocks every plugin instance; sandboxed or out-of-process AUs may not reach the port at all
[juce]; and every confirmed plugin-twin product (Helix Native, Neural DSP, IK TONEX) leaves USB
to a separate app sharing the plugin's preset format [ecosystem].

### 2.5 Shape of the app UI

Five views: **Pedal** (the 8 performance knobs once macros exist; until then the 28 raw
parameters, `Params.h:61-90`), **Editor**, **Library** (desktop presets beside the pedal's
slots), **Audition** (DI render, A/B, parity status) and **Device** (connection, the labels of
§7.6, upload, firmware). The GUI is the largest unsized item (§8.2).

## 3. Repository, build, CI and licensing

### 3.1 Layout

Everything stays in the existing CMake superbuild (`CMakeLists.txt:1-26`). Each new directory
has one responsibility, and nothing outside `plugin/` includes JUCE (§3.5).

```
dsp/                      engine (firmware links it). Additions:
  include/brainscape/       PresetState.h, Preset.h (LoadPreset, Restart, LoadMode), Taper.h,
                            InputCondition.h (+ input mode), TestSignal.h, SoundRevision.h
  src/blob/                 .bsp decoder + structural validator (firmware links it)
  src/compiler/             modes::Compile/Serialize/Validate, canonical JSON; separate
                            target brainscape_compiler, desktop only (§6.6)
protocol/                 device-protocol framing, CRC32, message structs; no dependencies;
                          compiled into firmware AND app (§7.3)
link/                     desktop device-link client over an abstract MIDI port; no JUCE
plugin/                   JUCE: processor, parameters, editor, standalone, MIDI adapter
  parity-host/              renders the golden set through every plugin binary (§3.4); it
                            links JUCE's plugin-hosting classes, so it lives under plugin/
firmware/                 TinyUSB device, protocol server, slot store, FatFs disk I/O,
                          render mode; factory/ (JSON + committed .bsp), patches/ (libDaisy)
tools/bspc/               preset compiler/verifier/renderer (§6.6); no JUCE
tools/parity/             golden-hash tooling (profile §6); no JUCE
cmake/BrainscapeFpProfile.cmake   floating-point profile target (§3.3)
```

The compiler sits beside the decoder so schema and blob layout change in one place (the
grain-engine design's single shared compiler, `grain-engine.md` §5), in its own target so the
firmware never links a text parser (§6.6).

> **Update (2026-10-07, mode-compiler lane A).** As built, the compiler is the top-level
> `compiler/` (target `brainscape_compiler`, namespace `bsc`, desktop hosts only), not
> `dsp/src/compiler/`, so a compiler change is gated by its committed outputs and the package
> rule rather than the sound-revision path trigger ([mode-compiler.md](mode-compiler.md) §8.1).
> `tools/bspc/` is its command-line tool. That design amends this section when it is accepted
> (its §12.5).

### 3.2 JUCE in the build

- **Fetched only when needed:** inside `if(BRAINSCAPE_BUILD_PLUGIN)` (`CMakeLists.txt:9`,
  `:24-26`), `FetchContent_Declare(JUCE URL …/9.0.3.tar.gz URL_HASH SHA256=…)`, the same for
  `clap-juce-extensions` under `BRAINSCAPE_BUILD_CLAP`, then `add_subdirectory(plugin)`.
  `dsp/` and firmware contributors never download JUCE (`CMakeLists.txt:4-6`).
  `FETCHCONTENT_SOURCE_DIR_JUCE` serves offline builds; a submodule would make every recursive
  clone pull JUCE.
- **CMake 3.22** (JUCE 9.0.3's minimum) everywhere, raised from 3.21 (`CMakeLists.txt:1`).
- **Enable C before JUCE:** JUCE fails to configure without it and compiles 22 `.c` sources.
  Call `enable_language(C)` at root scope inside the plugin branch, before
  `FetchContent_MakeAvailable(JUCE)`, so other configures stay C++-only.
- **Flag hygiene:** `juce_disable_default_flags()` only inside `plugin/` (at the root it would
  blank `-O3 -DNDEBUG` for `dsp/`); never `juce::juce_recommended_lto_flags` (§3.3).
- **Linux shared objects:** in plugin builds only, set `CMAKE_POSITION_INDEPENDENT_CODE ON`,
  `CMAKE_CXX_VISIBILITY_PRESET hidden` and `CMAKE_VISIBILITY_INLINES_HIDDEN ON` before
  `add_subdirectory(dsp)`; without them the engine cannot link into a `.so`, and with `-fPIC`
  alone a second Brainscape build in the same DAW reads the first one's parameter table
  (*measured* [host]). They must **not** reach the firmware or the M7 oracle archive, where they
  add GOT relocations (*measured* [review-juce], [review-pc]; profile §3.5).
- **macOS audio input:** notarized builds need `MICROPHONE_PERMISSION_ENABLED TRUE`, a
  `MICROPHONE_PERMISSION_TEXT`, `HARDENED_RUNTIME_ENABLED TRUE` and `HARDENED_RUNTIME_OPTIONS
  com.apple.security.device.audio-input` on `juce_add_plugin` (`JUCEUtils.cmake:371`, `:414`,
  `:2056`, `:2133`), checked on a clean notarized install. On Windows the microphone privacy
  setting blocks WASAPI input; the Standalone detects silence on an open input and says so.
- **Build cost:** cache `_deps` and use `sccache`/`ccache` (`juceaide` alone takes ~45 s on
  Windows CI, *estimated*).

### 3.3 One floating-point profile for every translation unit that touches the engine

Profile §3.2–§3.5 own the flags, tripwires, LTO rule and header hygiene, because engine code
compiled or inlined under a JUCE translation unit's flags picks up its contraction (*measured*
[fp-isa], [juce]). In the JUCE build:

1. `brainscape::fp_profile` (`cmake/BrainscapeFpProfile.cmake`: GCC/Clang `-ffp-contract=off
   -fno-fast-math -fno-math-errno`, MSVC `/fp:precise`, never `/fp:contract` or `/fp:fast`) is
   linked PUBLIC by `brainscape_dsp`. JUCE compiles each format wrapper in its own target, which
   links the shared-code target PRIVATE (`JUCEUtils.cmake:1480-1497`), so `plugin/` links
   `brainscape_dsp` **PUBLIC** on the shared-code target and sets PUBLIC every definition a
   wrapper must see (`JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP=1`, `JUCE_ASIO`, …), as JUCE's own
   example does. Linked PRIVATE, the wrappers compile without the profile (AppleClang contracts
   by default, so the macOS arm64 slice diverges) and the standalone switch yields a duplicate
   `juce_CreateApplication` (`juce_audio_plugin_client_Standalone.cpp:222-230`). Cost: 11–13 %
   on engine code (*measured* [prototype]), negligible on JUCE's (*estimated*).
2. `tools/cmake/arm-none-eabi-toolchain.cmake:14-16` gains `-ffp-contract=off -fno-math-errno`;
   today the firmware build has 152 fused multiply-adds (*measured* [prototype]).
3. The profile's tripwire headers and configure-time flag check apply to every target; no LTO on
   any target that links the engine in v1.
4. **No inline floating-point code in public headers:** `Engine` gets opaque aligned storage
   (profile §3.5), not a pointer-to-implementation, which would allocate; display helpers (a
   grain-envelope curve, a pitch ratio) are exported, non-inline `dsp/` functions.

The existing `macos-latest` leg passes no contraction flag and is inferred to contract today.

### 3.4 CI

> **Update (2026-10-07, mode-compiler lane G).** `bspc-roundtrip` is built as
> [mode-compiler.md](mode-compiler.md) §8.3 specifies, on the seven host legs of `parity.yml`
> (Linux GCC, Clang and arm64, MSVC and MSVC AVX2, both macOS legs) rather than three OSes: every
> committed document compiles to its committed package and `parity-summary` requires the legs'
> manifests identical. Beside it: the compiler audit (a source ban over `compiler/src` and, on
> the GCC and Clang legs, an import check), the package decoder's mutation fuzzer and frozen
> fixtures on every parity leg and the emulated M7, a libFuzzer job, nightly legs, and the
> sound-revision gate's package rule (a changed package needs a "package-change" label and a
> named cause). `symbol-scan`, when built, is scoped to functions (mode-compiler.md §8.1). That
> design amends this section when it is accepted (its §12.5).

The existing jobs in `.github/workflows/host.yml` stay. The profile owns the parity legs (profile
§6.2), including the full specification of `standalone-parity` and `plugin-format-parity`.

| Job | What it does | Cost (*estimated*) |
|---|---|---|
| `plugin` (3 OSes) | All formats; Linux needs `libxi-dev` and `libegl-dev`; `pluginval` (strictness ≥ 5) for VST3 and AU, `clap-validator` for CLAP. | +5–10 min per OS |
| `standalone-parity` | The shipped Standalone's headless `--parity-render` reproduces the golden hashes on each shipped binary: macOS arm64 and under Rosetta 2, Windows x64 natively and under Prism, Linux x64. | minutes per leg |
| `plugin-format-parity` | `plugin/parity-host` loads **each built format binary**, restores a golden package through `setStateInformation` and drives it in place with host block patterns {0, 1, 37, 441, 513, 1024, 4096, random}, mono→stereo and stereo layouts and transport toggles; the only job that exercises the wrapper code of §4 (the VST3 state-echo path, zero-frame calls, in-place buffers, state round trips), including the block-split cases (§4.11). | minutes per OS |
| `link-order` | `dsp/` objects linked first and last; renders bit-identical. | small |
| `symbol-scan` | No weak or COMDAT `brainscape::` definition outside `dsp/` objects (`nm` / `dumpbin`); every format target's compile flags, read from `compile_commands.json`, match the profile. | small |
| `boundary-grep` | No `#include <juce` outside `plugin/`, so none under `dsp/`, `protocol/`, `link/`, `firmware/` or `tools/` (§3.5). | seconds |
| `bspc-roundtrip` | Every factory JSON compiles to identical bytes on all three OSes, equal to the committed `firmware/factory/*.bsp` (§6.6). | small |
| `firmware-elf-audit` | From the **first** firmware image, test images included: no ST USB or SD-glue symbol (`USBD_*`, `USBH_*`, `SD_Driver`) or object from libDaisy's `src/usbd/`, `src/usbh/`, `src/util/*diskio*` or ST's USB middleware (§7.2); no GOT relocation in the `dsp/` archive (§3.2). | seconds |
| `sound-rev-gate` | Profile §5.12's triggers: a diff under `dsp/src` or `dsp/include`, or to `dsp/CMakeLists.txt`, the root `CMakeLists.txt`, the profile CMake file, the forbidden-flag list or the arm toolchain file, needs a `kSoundRevision` bump or a CODEOWNERS-approved "sound-neutral" label; a golden-hash change always needs the bump. Bumps are checked per commit: each revision is one commit that raises `kSoundRevision` by exactly one, and its golden file is minted at that revision, by that commit or a later one before the next bump; a pull request may carry several consecutive revisions; the job checks out the whole history (`fetch-depth: 0`) to walk them and fails on a shallow one, and the rule holds on `main` only for pull requests merged with a merge commit. A golden file below the head's revision is checked here by its key; parity renders the head's, and `sound-rev-render` renders each lower one on every parity leg (while it is a required check, a pull request may push several revisions at once; until the owner requires it, each revision commit is pushed and passes parity and host as the pull request's head before the next). | seconds |
| `sound-rev-render` | Every sound revision a pull request introduces below its head's, rendered on every parity leg: for each, the gate's `--list-revisions` names every commit at that revision that a commit at another revision has as a parent (the next revision's bump, or a merge into a later one). One job per leg of the head's `parity-host` (Linux x64 GCC and Clang, Linux arm64 GCC, MSVC SSE2 and AVX2, AppleClang pinned and latest) renders every listed commit, and one job per listed commit renders it on `parity-m7`'s runner (the Cortex-M7 under the pinned qemu-arm): each builds the commit's own harness in a worktree as that commit's own `parity.yml` builds it on the leg and makes every render that `parity.yml` makes there, against the commit's golden file (`parity_plan.py` reads `parity.yml` and fails on any shape it does not know; a commit whose `parity.yml` does not run a leg gets the head's renders there; today 48 and 512-frame blocks and the hostile FP environment on every host leg, on Linux GCC also the other block sizes, random sizes, split delivery and fresh engines, on the M7 its block sizes, the hostile FPSCR, the parity stream and the forced-flush control); the runners and the pinned M7 toolchain are the head's. The job `sound-rev-render` lists the revisions itself and waits for the listing and every render job: it passes at once with at most one revision and fails when a listed commit cannot be built or checked on any leg or its golden file is not keyed to its revision. It has no `needs`, so its check is pending from the start of every run and a base change or a reopen (which keep the head commit) cannot leave an older run's result standing. Require only that job, never `sound-rev-render (list)` or a `sound-rev-render (<leg>)` job, which a pull request with at most one revision skips under a name that is not expanded. While it is a required check, a pull request may push several revisions at once; until the owner requires it, each revision commit is pushed and passes parity and host as the pull request's head before the next. The head alone still gets `parity-negative-control`, `host.yml`'s tests and the checks that read no golden file. It runs on description edits too, since a base change fires only that event. | seconds with at most one revision; with more, on GitHub (*estimated* from `parity-m7`'s 22–43 min for r7's 45 presets, fast and slow runners, and four emulated renders at once running about 2.4 times as fast as one): each M7 job about 10–20 min whatever the number of revisions, Linux GCC about 3–6 min per listed commit and the other host legs 1–2 min per commit, so about 10–20 min and 90–140 runner-minutes (the waiting job's included) for three lower revisions, again on each description edit; *measured* here for step 4's r4–r6: the M7 329–375 s per commit in Docker on 4 CPUs, the host legs 54–191 s for all three |

**Implementation constraints.** `plugin/parity-host` is built on JUCE's own plugin hosting
(`juce::AudioPluginFormatManager`, which loads VST3, AU and LV2) and adds a minimal CLAP host
for the CLAP build. `symbol-scan` needs `compile_commands.json`, which CMake writes only for the
Ninja and Makefile generators, so that leg cannot use the Visual Studio or Xcode generators.

The M7 oracle runs at about 8× realtime (*measured* [oracle]), so it fits every pull request.

### 3.5 Licensing

**JUCE must be used under AGPLv3.** Its commercial EULA (clause 2.3) forbids combining JUCE with
software under a licence that requires source disclosure, so no tier, the free Starter included,
fits GPLv3 Brainscape. **Desktop binaries are GPLv3 + AGPLv3:** GPLv3 §13
(`LICENSE:552-561`) permits linking with AGPLv3 code, and the AGPL's network clause bites only for
a modified version offered for remote interaction, which a desktop app talking USB is not. This
is a reading of the licence texts, not legal advice (§10.2 Q1). Every binary release ships, or
links to, the **Corresponding Source**: the Brainscape commit, the exact JUCE archive matching
the pinned hash, `clap-juce-extensions`, the resampler and the build scripts, with JUCE's SPDX
bill of materials cited in the notices.

**Contributor licence rule** (in README.md's licence section since 2026-10-05, where it
replaced "AGPL sources … remain study-only"):

> AGPLv3 code may be linked only into desktop targets under `plugin/` (today: JUCE).
> `dsp/`, `protocol/`, `link/` and `firmware/` must never include AGPL code, which keeps the
> engine framework-free and the firmware GPLv3-only. Desktop release artifacts state that
> they are GPLv3 combined with AGPLv3. Other AGPL sources (e.g. Essentia) remain study-only.

Every target that links JUCE therefore lives under `plugin/`, the CI parity host included
(§3.1), and `boundary-grep` (§3.4) rejects a JUCE include anywhere else.

The other desktop dependencies are GPLv3-compatible according to the FSF's licence list:
the VST3 SDK 3.8 (MIT); the ASIO SDK as bundled by JUCE 8.0.11 and later (GPLv3, which is what
makes `JUCE_ASIO` usable in a GPL build); Apple's AudioUnitSDK (Apache-2.0, compatible with
GPLv3 in one direction only: Apache-2.0 code may be combined into a GPLv3 work, not the
reverse); LV2 (ISC); CLAP and `clap-juce-extensions` (MIT); r8brain-free-src (MIT); and
fast_float (MIT, Apache-2.0 or Boost, at the user's choice). AAX is not used. With the VST
logo, its attribution line is required and the product is never "Brainscape VST" (§10.2 Q6).
Shipping needs the $99/yr Apple Developer Program for notarization and Windows signing through
the SignPath Foundation or Microsoft Artifact Signing (§10.2 Q7). Firmware licensing is §7.2 and
§7.9.

## 4. Engine hosting requirements for the wrapper

The engine is ready to host: instances are isolated, it has no mutable global state, and a dense
64-voice preset uses ~1.7 % of one desktop core (*measured* [host]). The wrapper
(`BrainscapeProcessor` and the standalone shell) carries the obligations below.

### 4.1 Canonical configuration and lifecycle

- `Init` **once per engine instance** with the canonical configuration (`sampleRate` 48000,
  `historyFrames` 2²², `stereoInput` and `ditherRingWrite` true; profile §2.3) and
  `maxBlockSize = 512` whatever the host's maximum (`dsp/src/Engine.cpp:118` rejects more). A
  host rate change reconfigures only the resampler, so prepare/release never re-clears the ring.
- After any re-`Init`, re-apply the preset with `LoadPreset`: `Init` resets every parameter and
  clears freeze (`Engine.cpp:158-161`, `:184-188`). The wrapper holds the authoritative preset.
- **Memory:** 16.88 MiB per engine, every page touched by `Init` (*measured* [host]). The spare
  engine of §4.7 doubles it, so it is allocated **lazily**: only while "Restart on transport
  start" is on or an Exact load is pending, and released when the option is turned off.
  Offline audition adds a third engine only while it runs. `looperFrames` stays 0 until the
  looper exists.

### 4.2 Sample rate: the 48 kHz path and the resampled path

At 48 kHz the engine runs on host buffers (chunked, §4.3); otherwise the wrapper converts host →
48 kHz → host. **The resampler is r8brain-free-src** (MIT; real-time pull API; reports its
latency), with libsamplerate (BSD-2) as the fallback if r8brain's measured latency or quality
disappoints (§10.2 Q14). Rejected: `juce::WindowedSincInterpolator` (aliases when downsampling,
`juce_Interpolators.h:49-104`), Lagrange (5 taps) and `juce::dsp::Oversampling` (power-of-two
ratios only). The round trip, ≈3–4 ms (*estimated*), is reported with `setLatencySamples`; the
mode is fixed at `prepareToPlay`. The engine mixes dry internally (`Engine.cpp:529-530`), so
resampled mode delays dry too, against the grain-engine design's "dry path never enters the
block-delayed wet path" (`grain-engine.md` §2, commitment 5); host delay compensation realigns
it. A wrapper-side dry mix at host rate would restore zero-latency dry but would duplicate the
engine's mix and trim smoothing outside `dsp/`, so it is deferred (§10.2 Q26). The Standalone
forces 48 kHz (§2.2): ASIO (whose SDK JUCE bundles under GPLv3, §3.5), WASAPI exclusive mode and
CoreAudio can set the rate, while WASAPI shared mode follows the system mixer; failing that, the
Standalone runs resampled and says so. Offline audition always renders at 48 kHz (§4.9).
Rate-invariant engine constants, and with them a native-rate plugin mode, are no longer needed
by any product; both are deferred to phase F.

### 4.3 Host blocks

- **Chunk to ≤ 512 frames:** a larger `Process` call outputs zeros without advancing the counter
  and asserts in Debug (`Engine.cpp:379-392`). Chunking adds no error (*measured* [host],
  [plan-of-record], on presets the block-split bug does not reach).
- **Zero-frame calls:** JUCE's VST3 wrapper calls `processBlock` with 0 samples whenever buses
  exist (`juce_audio_plugin_client_VST3.cpp:3607-3612`, `:3704-3730`). Apply parameter changes
  and skip `Process`, whose Debug assert (`Engine.cpp:379`) kills the DAW; relaxing it is
  optional.
- **Null or deactivated input bus:** pass zeros, never `in == nullptr`, which zero-fills the
  output and cuts the tail (`Engine.cpp:380-391`).

### 4.4 Buffers and bus layouts

JUCE processes in place, and stereo in place works. **Mono in place is an engine bug:** with
`stereoInput = false` the right input aliases the left (`Engine.cpp:432`) and the mix writes
`outL[n]` before reading `inR[n]` (`:529-530`), so every right-channel sample is wrong
(*measured* [host], [challenge]); the fix is queued (profile §5.6). The wrapper Inits with
`stereoInput = true`, feeds mono through the Mono input mode (§4.8), and until the fix lands
copies mono input to scratch when it aliases `out[0]`. `Process` always writes `out[1]`
(`Engine.cpp:530`), so mono out needs a scratch right buffer. Layouts: mono → stereo, stereo →
stereo, mono → mono. No sidechain in v1 (`ProcessContext` has none, `Engine.h:74-82`).

### 4.5 Latency and tail

- **Latency:** `LatencySamples()` is 0 (`Engine.h:137`) and dry is bit-exact at Mix = 0
  (*measured* [host]); report 0 at 48 kHz, the resampler round trip otherwise. Wet onset delay is
  musical (the design's opt-in wet-latency report stays open, `grain-engine.md` §12 item 8).
- **Report an infinite tail:** tails run from 0.27 s to forever (record §2.4). Return infinity
  from `getTailLengthSeconds()`, which VST3 maps to `kInfiniteTail`
  (`juce_audio_plugin_client_VST3.cpp:3481-3492`) — **except in CLAP**, where
  `clap-juce-extensions` converts it to 0 frames (`clap-juce-wrapper.cpp:1560-1578`): when
  `clap_properties::is_clap`, return `INT32_MAX / sampleRate` seconds, which reports at least
  `INT32_MAX` frames, CLAP's value for an infinite tail. Silence detection uses a threshold,
  never exact zero.

### 4.6 Floating-point environment: the engine's guard owns the control word

The profile's guard writes the **complete** control word around **every** engine entry point,
`SetParam`, taper mapping, preset decoding, `LoadPreset`, macro fan-out, expression curves and the
desktop compilers included (profile §4.1), and the profile mandates gradual underflow with an
in-code flush (§4.2–§4.3 there). Today's guard only ORs flush bits, in `Process` alone
(`DenormalGuard.h:32`, `Engine.cpp:394`), and a host thread left in round-toward-zero changed
every golden hash (*measured* [oracle]). So everything whose result reaches the engine is computed
by those guarded `dsp/` functions or the exact input functions (§4.8), never in wrapper code;
`juce::ScopedNoDenormals` is redundant and, by style, left out around engine calls so the
environment has one owner; meters and GUI may do as they like. File decoders and the resampler
are platform code outside the profile, which limits cross-machine audition (§4.9).

### 4.7 Threading

- The processor owns the `Engine`; only the audio thread calls it while it is live.
- **One event queue, several producers:** VST3 automation and MIDI on the audio thread
  (`juce_audio_plugin_client_VST3.cpp:3536`), UI on the message thread, state restores on a host
  thread (`:2782-2821`) feed **one wrapper-owned multi-producer, single-consumer queue** (or one
  SPSC queue per producer merged by frame stamp), drained at sub-block boundaries (§4.10).
  Same-frame order: state load, host automation, MIDI, UI, each in arrival order. An overflow is
  counted, never coalesced, and puts a scripted render outside the contract (profile §5.11).
- **State restore is applied as a unit**, since `setStateInformation` may run during
  `processBlock` and 28 separate `SetParam` stores could straddle a block start
  (`Engine.cpp:358-367`, `:415-421`): decode on the calling thread into a `PresetState`, make a
  generation counter odd (a sequence lock), post it; the audio thread applies it at its next
  block's first frame (with the load mode of §6.9), updates the parameter atomics (§5.3) and
  makes the counter even. The atomics are mirrors for host and UI, never engine input, and
  unread while the counter is odd.
- `SampleCounter()` and `ClearHistory()` race with `Process`; the other any-thread calls are
  race-free under ThreadSanitizer (*measured* [host]).
- **Exact loads use a spare engine:** `Restart` clears ~17 MiB and must not run beside `Process`,
  so a worker runs `Restart` + `LoadPreset(Exact)` on the lazy spare from one consistent snapshot
  of the plain values, the audio thread swaps pointers at a block boundary, and the worker
  restarts the retired engine. The pedal has no SDRAM for a spare (`grain-engine.md` §7) and
  mutes the wet path instead (§6.7). An engine that has rendered nothing since `Init`,
  `Restart` or `ClearHistory` skips the clears, so an Exact load before its first block (a
  project being opened, a transport start straight after `prepareToPlay`) runs in place on the
  audio thread at the cost of `Reset`.
- The onset LED reads `ConsumeOnsetCount()` in `processBlock` and publishes an atomic.

### 4.8 Input: sanitizing, the 24-bit grid, and mono input

Profile §3.7 defines two exact functions in `dsp/include/brainscape/InputCondition.h`, and
**every input path uses exactly one**: `SanitizeInput` (NaN and ±inf → `+0.0f`, finite values
unchanged) on **live** paths, that is `processBlock` in every format and the Standalone's live
input; `ConditionInput24` (the codec's 24-bit grid) on **pedal-faithful** paths: offline audition,
PARITY on the app side, golden vectors from recordings, `bspc render` and the optional
**Pedal-faithful input** toggle. The firmware calls neither, because libDaisy already delivers
exactly *i* × 2⁻²³ (profile §2.2); it keeps `postgain` at 1, and bring-up confirms the Seed3 takes
the 24-bit branch.

- **The live path keeps headroom:** dry passes above 0 dBFS bit-exact, while wet clips at ±1.0 in
  the int16 ring identically everywhere (*measured* [challenge]). An **Input level** control
  before either function stages gain, and an **Output level** reference (pedal 0 dBFS ≈ 1 Vrms,
  `docs/research/pedal-control-surface-and-io-hardware.md`) matches loudness; both are global
  settings, never in presets.
- **Pedal-faithful input** (Standalone, off by default) applies `ConditionInput24` after the Input
  level, so dry and wet clip as on the pedal; live playing stays uncovered.
- **Mono input:** the onset detector hears `0.5·(L + R)` (`Engine.cpp:469`), so a mono DI fed as
  `R = L` and as `R = 0` renders differently. One `dsp/` input-mode function, `Mono` (`R := L`) or
  `Stereo`, serves the firmware's mono/stereo setting (the pedal has two input jacks,
  `docs/research/bom-cost-and-product-form.md:133`) and every desktop path, and is part of the
  render recipe (§4.9) and the PARITY request (§7.4).

> **Update (2026-10-09, the output limiter).** [output-limiter.md](output-limiter.md), a draft and
> not built, puts a safety limiter at the end of the engine. The wrapper therefore receives what
> the pedal's codec receives, never over full scale unless the dry itself is. Each channel's
> ceiling is max(1, |dry term|), so the live path keeps its headroom above: no output exceeds
> that ceiling, and the limiter scales the wet first, by up to 12 dB, before it touches the dry.
> A dry over 0 dBFS therefore passes at Mix 0 bit for bit whenever the wet's gain is at −12 dB or
> above. Past that, the gain, linked across channels, can pull a hot dry below its own level (its
> §4.4 item 7). The wrapper's Output level stays after the engine, outside parity, so a user who
> raises it can still exceed 0 dBFS. The editor gains a limiter lamp, a mark for when the dry dips
> too, and the gain-reduction readout at the output meter (its §7.3). There is no control and no
> latency. *(2026-10-10: the owner's answer to its D5 adds a per-preset switch, Leaf row 87,
> `output.limiter`, default On. With it Off the engine clips at the same ceiling instead of
> limiting, so the wrapper still never receives an over that the dry does not carry. The plugin
> registers it without automation, shows it in the Leaves view and captions the lamp CLIP while it
> is Off (its §7.1, §7.3). There is still no latency.)* *(Draft 4, 2026-10-10, corrected: the
> switch is the owner's choice for D5, made in the session from four options; draft 4 first
> called it a proposal. Whether Off clips, as above, or passes overs to the host unclipped is
> still the owner's question (its §11.5 Q1), recommended Clip so the plugin matches the pedal.
> The plugin shows its states as Limit and Clip and tags Clip presets in the Library list and the
> Modes menu.)*

### 4.9 Restart, offline renders and reproducible bounces

**`Engine::Restart()`** (profile §5.8) returns a running engine to the exact post-`Init` state,
keeping parameters, and is the one primitive behind Exact load, audition, the plugin's restart
option and PARITY. On the pedal it costs about **45–160 ms** (*estimated*; record §2.4); profile
§5.8's two alternatives, a restart watermark that would remove the wet mute (it must reproduce
the clearing implementation's golden hashes) and a DMA2D fill, are proposed but not yet
prototyped, and are evaluated before the default load mode is chosen (§10.2 Q20).

> **Update (2026-10-07, Rev7 bench, sound revision 1).** `Restart` is measured on the owner's
> Daisy Seed Rev7: 47.18 ms after 2 s of rendering (an Exact load 47.17 ms), of which clearing
> 16 MiB of SDRAM at the core's floor without DMA is 44.97 ms; 0.02 ms on an engine that has
> rendered nothing since. The default load mode no longer waits on the measurement, only on the
> two alternatives above, neither yet prototyped, and the owner's choice
> ([reviews/rev7-silicon-record.md](reviews/rev7-silicon-record.md) §3.6).

**Offline audition render:** a separate `Engine` on a worker thread, canonical configuration,
`LoadPreset(Exact)`; input decoded with `juce::AudioFormatManager`, converted once to 48 kHz if
needed, passed through `ConditionInput24` and the input mode; 48-frame blocks until the
block-split fix lands; events at their stamps; float32 WAV hashed with PARITY's segmented hash.
`dsp/tests/render_main.cpp` becomes a front end over one shared render function used by the app,
`bspc` and CI. Each render stores its **recipe**: `sound_hash`, the hash of the 48 kHz float32
input, input mode, event script, `kSoundRevision`, generator version. **Across machines** it is
identical only for **48 kHz integer-PCM WAV, AIFF or FLAC**, because JUCE's platform and
floating-point decoders (`juce_AudioFormatManager.cpp:63-91`) and r8brain's run-time kernel
design with libm `sin`, `cos` and `exp` (`CDSPSincFilterGen.h:249`, `:470`, `:877-879`) are
outside the profile. For other input the app shares the converted 48 kHz stream and its hash,
and labels the render "reproducible on this machine only" until then.

**DAW "Restart on transport start"** (off by default: delay plugins keep trails across stops,
and the design keys randomness on a free-running counter, `grain-engine.md` §9, "Counter-based
RNG") swaps in the spare prepared with `Restart` + `LoadPreset(Exact)` at transport start. This
makes bounces reproducible; at 48 kHz without automation it also makes them identical to the
pedal, on the pedal grid (§2.3) until the block-split fix lands.
(a) **Offline, restart synchronously:** when `isNonRealtime()` (set through `setNonRealtime`,
`juce_audio_plugin_client_VST3.cpp:3466`, `:3715`), restart the live engine in place
(~0.92 ms, *measured* [host]). (b) **Realtime, never block:** if the spare is not ready, keep the
running engine and clear a **reproducible** flag that the labels read (§7.6). (c) A transport
start is the first `processBlock` with `isPlaying` after a non-playing block, `prepareToPlay` or
a switch to offline; locates and cycles do not restart. The VST3, VST2 and LV2 wrappers call
`setNonRealtime` before every block, so only a change of mode counts: re-arming at every call
restarted an offline export every block. (d) A restart also resets the resampler
and bypass crossfade. (e) The spare comes from one consistent snapshot (§4.7). (f) The restart
loads the values in effect at the block's first frame: a restore, the host automation and the UI
edits of that block are part of the Exact load, so an automation lane starts on its own value
instead of gliding from where the last playback left the parameter. In real time the spare is
swapped in only when it holds exactly those values; a host that sends the lane's start value
while stopped, as on a locate, gives the worker time to prepare it. A scripted stamp made
before the restart is void, even when its frame has already passed (§4.10).
`AudioProcessor::reset()` maps to `Engine::Reset()` (profile §5.8) plus a resampler flush; it
keeps the ring, so it neither silences old audio nor makes bounces reproducible.

### 4.10 Events are applied at exact frames

> **Update (2026-10-08, CLOCK design pass).** [clock.md](clock.md) §4 adds the tempo events
> (Tap, Tempo, ClockTick, Transport, Subdivision) and amends two bullets below for the pedal.
> Every pedal producer stamps an event a fixed number of blocks after the block in which it was
> captured: two for MIDI bytes (captured in the UART's DMA callback), the console and the knobs,
> five for the tap switch, which the audio callback samples and debounces by state. The control
> loop, the queue's single producer, pushes stamps that never fall and at most two clock ticks per
> block; the UART hands its bytes to the loop through a ring whose entries carry a timeline epoch,
> so bytes captured before an Exact load's reset are parsed but never stamped into the new
> timeline. A session log records the frame where each event **applied**, not its stamp, since a
> late event applies at the next block's start. After an Exact load the producer re-asserts the
> running tempo, and under a running MIDI clock the master's position, as logged events at
> frame 0 (clock.md §2.5). The plugin turns the host's tempo and transport into those events
> itself (clock.md §4.4), and its Standalone feeds device MIDI clock through the same translator
> the pedal uses (§4.3 there).

`SetParam` ignores its sample offset and applies at the next `Process` (`Engine.h:90-96`), which
made 48- and 512-frame renders differ; splitting at the event's frame made them identical
(*measured* [preset], [challenge]). So every event the app generates (scripted UI edits, MIDI,
macro moves, freeze, triggers, taps, Spillover loads) carries an **absolute frame stamp** and
takes effect exactly there, with the semantics of profile §5.11; a macro's fan-out applies in
target-list order. Profile §5.11 moves the split into `dsp/`: `Process` takes the block's
events (`ProcessContext::events`) and splits there itself. A wrapper that splits host blocks
and calls `SetParam`, `Trigger` and `SetFreeze` between sub-blocks renders the same bits; the
plugin skeleton hands its events to `Process` with their offsets instead.

- The engine's SPSC queue (`grain-engine.md` §9, threading contract) is **not a prerequisite
  for the app**. On the pedal it is the transport from the engine's single producer, the
  control loop; interrupt and MIDI sources post their events to the control loop, never to the
  queue (phase D).
- On the pedal an event applies at the start of the 48-frame block it arrives in, stamped with
  that frame; under VST3 automation the wrapper stamps each block's value at frame 0.
- Until the block-split fix lands a split point is not neutral (§4.11), so scripted renders meant
  to match the pedal keep events on multiples of 48 frames.
- Mode switches and Spillover loads are ordinary stamped events (§6.1).
- Stamps count frames from the engine's last `Init` or restart. A restart begins a new
  timeline: every stamp made before it is void, whether its frame lies ahead or has passed.

### 4.11 Engine defects that gate identity

**The block-split bug** (profile §5.7 normative; record §2.8): grains can read ring frames that
`Process` has already written ahead of the live write head (`Engine.cpp:457-498`), so output
depends on block size through three mechanisms, one reachable **without freeze** (a mark about
one ring old with reverse grains) and one with any position source (freeze held past 65.5 s).
The verified three-part fix uses a fixed `kBlockWriteAheadFrames` = 512, never derived from
`maxBlockSize`, so pedal and plugin clamp identically; part A alone was measured insufficient. It
changes sound and lands in merged step 1 (§8.1). For the app:

- **Until it lands,** pedal-exact renders use 48-frame blocks from frame 0 with events on
  multiples of 48, for every preset. In a DAW only the "pedal grid" option is covered (§2.3);
  the Live-positioning presets that did not diverge on raw host buffers are an observation,
  not a promise, unless the owner extends the profile's contract to them (§10.2 Q20).
- **Freeze semantics for marks are a product decision:** pin-eligible marks (recommended; freeze
  holds the Strum position) or live-head marks (freeze has no effect on mark-positioned grains),
  by a listening test (§10.2 Q19).
- `plugin-format-parity` (§3.4) runs the profile's regression cases with host block patterns such
  as 1024, 441 and variable sizes.

**Other defects:** mono in-place aliasing (§4.4); the `MakeEnv` reciprocal of a subnormal attack
leg (`GrainMath.h:114-126`), fixed by the profile (§3.7 there; §5.5 here); the dither-key
truncation (`Engine.cpp:56-59`), which repeats the dither every 2²⁹ samples (3.1 h) invisibly to
golden vectors, fixed before internal revision 1 (profile §5.6) and the motivating case for
§6.5's rule; and **time-parameter clicks**, identical on both sides. Decided in merged step 1
(profile §8.4 step 8) and landed before revision 1: the post-delay tap glides to a new time
inside `dsp/`, so pot moves and automation bend pitch like tape instead of splicing, on every
block grid (profile §5.6). `DelayMs` still splices clean delays until the
grain engine's glide lands.

### 4.12 Checklist for `BrainscapeProcessor`

| # | Wrapper obligation | Section |
|---|---|---|
| 1 | `Init` once with the canonical configuration and `maxBlockSize` 512; re-apply the preset after any re-`Init`; allocate the spare engine lazily and release it when the restart option is turned off | §4.1 |
| 2 | Engine at 48 kHz; r8brain-free-src with reported latency at other host rates | §4.2 |
| 3 | Chunk to ≤ 512 frames; apply parameters and skip `Process` on zero-frame calls; never pass null input | §4.3 |
| 4 | `stereoInput` always true; Mono/Stereo input mode; copy aliased mono input until the engine fix lands; scratch right output for mono out; supported layouts only | §4.4, §4.8 |
| 5 | Latency 0 at 48 kHz; infinite tail (CLAP: `INT32_MAX / sampleRate` seconds); threshold-based silence detection | §4.5 |
| 6 | Everything that feeds the engine is computed in guarded or exact `dsp/` code; no `ScopedNoDenormals` around engine calls | §4.6 |
| 7 | One wrapper-owned event queue (multi-producer, or one SPSC queue per producer merged by stamp) with a fixed same-frame order; state restores as a unit behind a generation counter; parameter atomics are mirrors only | §4.7 |
| 8 | `SanitizeInput` on live input; `ConditionInput24` only on pedal-faithful paths; Input and Output levels outside presets | §4.8 |
| 9 | Fresh engine for offline renders, with a stored recipe; restart on transport start synchronous offline, never blocking in real time | §4.9 |
| 10 | Absolute frame stamps; blocks split at event frames; mode switches and Spillover loads are stamped events | §4.10 |
| 11 | Until all three parts of the block-split fix land: the 48-frame grid for pedal-exact renders and the "pedal grid" option for DAW identity | §2.3, §4.11 |

## 5. Parameter layer

### 5.1 The exact binary32 plain value is the source of truth

The engine takes **plain** values (milliseconds, semitones, hertz) through
`SetParam(ParamId, float)` (`dsp/include/brainscape/Params.h:7-10`); hosts automate
**normalised** values in [0, 1]. A preset is 1:1 only if the float the author heard is the float
the pedal loads, so the plain value is authoritative everywhere; the normalised value is a view
for host automation.

### 5.2 Why `AudioProcessorValueTreeState` cannot carry presets

`juce::AudioProcessorValueTreeState` (APVTS) and `juce::AudioParameterFloat` store normalised
values and convert on every access, and they drop any change within `approximatelyEqual`
(|a − b| ≤ FLT_EPSILON·max), so a value one ULP away is **silently ignored or not saved**. The
plain → normalised → plain round trip changes 8–20 % of all floats in most of the 28 parameter
ranges by 1–2 ULP (only [0, 1] is exact); a build that fuses `start + (end − start)·p`, the Apple
Silicon default, maps the same normalised value to different plain values; and a real preset
sent through it changed two of 18 values and diverged at −118.5 dBFS (*measured* [juce];
record §2.5).

### 5.3 `BrainscapeParam`

One `BrainscapeParam : juce::RangedAudioParameter` per descriptor row (`Params.h:61-90`):

- **Identity:** `juce::ParameterID` = the stable string name (e.g. `"layer0.position.base_ms"`)
  with a version hint; the never-reuse rule (`Params.h:7-10`) keeps host identities stable.
  Pinning JUCE's derived VST3 numeric IDs to the permanent `uint32` IDs is open (§10.2 Q17).
- **Storage:** `std::atomic<float> plain`, a mirror for host and UI (§4.7); the engine receives
  values only as events. **`getValue()`** returns `NormalizedFromPlain(id, plain)` (§5.4).
  JUCE carries the normalised value as `float` (`juce_AudioProcessorParameter.h:113`, `:129`),
  so computing the mapping in double would not make the normalised round trip exact.
- **`setValue(n)`** returns at once if `n` equals `getValue()` exactly, which keeps exact plain
  values when a host echoes a stored normalised value (VST3 `setComponentState` does); otherwise
  it enqueues `Canonicalize(id, PlainFromNormalized(id, n))` as an event.
- **Preset and state loads** apply as a unit (§4.7), then write the mirrors and call
  `setValueNotifyingHost(getValue())`, whose inner `setValue` returns early.
- **`getNormalisableRange()`** (required by the base class and used by JUCE's LV2 wrapper,
  `juce_audio_plugin_client_LV2.cpp:164-190`) returns a range whose three lambdas call `dsp/`'s
  `PlainFromNormalized`, `NormalizedFromPlain` and `Canonicalize`.
- **Retired parameters** stay registered as non-automatable tombstones (`Params.h:50-53`).
  *Superseded by [mode-compiler.md](mode-compiler.md) §4.1 and §9.2 (amended here when that
  design is accepted, its §12.5): neither Retired nor Reserved rows are registered.*

**The GUI cannot use `juce::SliderParameterAttachment`:** it routes typed text and slider values
through the normalised float, drops edits within `approximatelyEqual` and displays a
reconstructed value (`juce_ParameterAttachments.cpp:90-98`, `:131-169`), losing up to 47 % of
three-decimal values in [−24, 24] (*measured* [review-juce]). A **`BrainscapePlainAttachment`**
instead writes plain bits through `BrainscapeParam::setPlainNotifyingHost(float)` (begin gesture,
enqueue the canonical value, update the mirror, `setValueNotifyingHost(getValue())`, end
gesture), parses typed text with the correctly rounded parser (§6.4) and shows the canonical
value, displays the plain mirror, and compares plain values with `==`.

**Host-originated sets are lossy, and the app says so:** after any set that is not an exact echo
(host undo, restored automation, a generic host UI), the plugin recomputes `sound_hash` from the
plain bits and shows the preset as modified.

### 5.4 Tapers and display metadata live in `dsp/`

`ParamDescriptor` (`{id, name, min, max, def, unit}`, `Params.h:43-48`) is the one table "consumed
unchanged by firmware pots, VST3/CLAP registration" (`grain-engine.md` §6). Extend it in
`dsp/` with a **taper** (log/power for `DelayMs`, `GrainSizeMs`, `ModRateHz`, `DelayTimeMs`,
`FilterCutoffHz`; offset-log for `SprayMs`, which allows 0; linear elsewhere), a **step count**
for discrete parameters (`OnsetTrigger`, `PositionSource`), a **display kind** (`FilterMorph` as
LP/BP/HP/Notch, `FilterCutoffHz` at maximum as "Off", an exact bypass, `Params.h:34-35`;
`PositionSource` as Live/Mark), **flags** (automatable, discrete, read-only), and a **title and
group**.

`PlainFromNormalized` and `NormalizedFromPlain` are exported, non-inline and guarded (§4.6). The
pedal's pot path is ADC code → deadband and hysteresis (firmware) → exact `code / codeMax` →
`PlainFromNormalized` → `Canonicalize` → event, and the plugin's knob calls the same function, so
**a pedal pot and a plugin knob at the same normalised position yield the same plain bits**;
macros and expression curves follow the same rule. Hosts store normalised automation, and
`clap-juce-extensions` by default advertises every parameter as 0–1
(`clap-juce-wrapper.cpp:174-179`), so once a plugin release is in users' projects every taper is
a frozen contract for VST3, AU **and** CLAP: tapers are final before the first public release
(§5.7). Descriptors are never serialized; presets carry IDs and values only.

### 5.5 Canonical values

Profile §3.7 adds canonicalization inside `SetParam`, written as integer tests on the bit
pattern because comparisons change under DAZ: NaN or ±inf → the descriptor minimum; ±0 and every
subnormal → `+0.0f`; then clamp. **Values are not quantized to a grid**, which would silently
change authored values. So canonicalization is identical on both sides, `bspc` and the app store
only canonical values, and the UI always shows the canonical value.

### 5.6 Host automation

> **Update (2026-10-07, mode-compiler lane C, sound revision 2).** The engine plays the macro
> and expression moves (events 4 and 5, through the shared evaluator) and the effect volume
> (`global.effect_volume_db`, a device setting that scales the wet signal with the mode's
> `wet_trim_db`); the plugin's parameters for them arrive with lane D
> ([mode-compiler.md](mode-compiler.md) §9.2), and the onset trigger and mark positioning, rows
> 27 and 28 until then, are mode structure, which the plugin does not load yet.

- **VST3 (JUCE 9.0.3):** last value per parameter per host block, `approximatelyEqual` changes
  dropped (§2.3); stamped at frame 0 of the block (§4.10). AU and LV2 are unchecked; assume the
  same.
- **CLAP via `clap-juce-extensions`:** values pass through JUCE's normalised float even for a
  `supportsDirectProcess()` processor (record §2.5). The post-v1 exact path adds a parameters
  extension that advertises plain ranges and reports the plain mirror (a wrapper patch, or direct
  `clap_direct_paramsFlush` and parameter-info calls).
- Soft takeover never applies to host automation (`grain-engine.md` §6, "Soft takeover").
- An **Expression** host parameter (0–1) feeds the same `dsp/` assignment evaluator as the
  pedal's expression jack, arriving with the macro work.

### 5.7 Identities to freeze before the first public release

> **Update (2026-10-06, mode-compiler lane 0).** The ID table is now
> [mode-compiler.md](mode-compiler.md) §4.2, built in `Params.h`: 82 rows with kinds, the
> macro IDs 69–76, and IDs 4 and 8 renamed `wet_trim_db` and `layer0.pitch.transpose_st`.
> That design's §4.5 restates the gate below; this section is amended when it is accepted
> (its §12.5).

VST3 and AU write parameter identities, and VST3 normalised automation, into users' projects, so
the first public plugin release freezes IDs, names and tapers for good. The 28 current IDs
(`Params.h:11-41`) do not match the design's leaf list (`grain-engine.md` §6, "The
leaf/structure split"):
`layer0.pitch.st`, `trigger.sensitivity`, `scheduler.onset_trigger` and
`layer0.position.source` are exposed though the design treats position source and pitch sets as
mode structure; there is no `layer1`, modifier slot or macro ID; and `post.delay.time_ms` differs
from the design's tempo-division `post.delay.time`.

**Gate:** no plugin binary leaves CI artifacts until (a) the ID set is reconciled with the mode
compiler's leaf list, (b) macro IDs exist, and (c) every taper is in the shared table.
**Freeze** gets a permanent ID as a performance toggle shared by footswitch, MIDI and DAW, never
stored engaged (§6.2). **Trigger** and **Tap** are momentary events (MIDI notes, footswitches),
not host parameters. **Bypass** is a wrapper-owned `host.bypass`, outside `Params.h` and presets:
the wrapper crossfades to the sanitized dry input while the engine keeps running, so trails
resume.

## 6. Preset model and file format

### 6.1 Complete-state presets, applied by one `dsp/` function

> **Update (2026-10-07, mode-compiler lane C, sound revision 2).** `LoadPreset` applies the
> whole decoded package as [mode-compiler.md](mode-compiler.md) §7.3 specifies: it validates the
> mode and CTRL first and applies nothing when they fail, copies the mode in at the load's
> frame, and counts a missing leaf only if its row existed at the package's `sound_rev`
> (`sinceRev`), so a leaf a later revision adds does not make an older package inexact. "A new
> leaf can land as sound-neutral" is withdrawn there (§7.6): any change that can alter output
> bumps the revision. That design amends this section when it is accepted (its §12.5).

A preset is **complete state**: every leaf with an explicit value, defaults included, because
load order changes the output (smoothers gliding versus snapping differed by −56 to −69 dB,
reconverging after ~0.7 s without feedback and still differing at the end of a 40 s render with
it) and delta loads leave unmentioned parameters at old values (−4.0 to −6.4 dB for a whole
render) (*measured* [preset]). `dsp/` exposes the only way to apply
one, so firmware and wrapper cannot order it differently:

```cpp
namespace brainscape {
enum class LoadMode : uint8_t { Exact, Spillover };   // §6.7
struct PresetState;                                   // decoded STAT + MODE (+ CTRL)
bool DecodePreset(const void* bsp, size_t bytes, PresetState* out, PresetError* err) noexcept;
// Exact is non-RT (calls Restart). Spillover does no large clears: it is a frame-stamped
// event (§4.10) applied exactly at its frame; its ModeBlob is staged into a free slot of the
// 4-slot ring BEFORE the event is stamped, off the audio path.
bool Engine::LoadPreset(const PresetState&, LoadMode) noexcept;
void Engine::Restart() noexcept;                      // §4.9
}
```

The order is profile §5.10's: every default, every stored value, mode publish (FastCut for Exact,
Trails for Spillover), performance initial state, then `Restart` (Exact) or an epoch restart
(Spillover), inside the guard.

**A mode switch is a stamped event.** The grain-engine design as written at `e86e971` picked up
a published blob once per block and had `PublishMode` return `false` until a slot retired, so
the caller retried; the applied frame then depended on the block grid, and mid-render Spillover
loads could never have been split-invariant or replayed. Its §5 was amended on 2026-10-05 to
this rule: the producer (firmware control loop or wrapper) stages the blob first and, if no slot
is free, **delays the stamp**, never the application.

A preset naming an ID this build lacks, or lacking one it has, loads but is marked **inexact**,
and the UI says so. A new leaf must default to a value that leaves existing presets
bit-identical, so it can land as "sound-neutral" (§6.5); otherwise it bumps the revision.

### 6.2 What a preset captures, and what it deliberately excludes

> **Update (2026-10-08, CLOCK design pass).** The table's "tempo source" and the exclusions'
> "clock source" contradicted each other. [clock.md](clock.md) §2.4 and §3.6 resolve it as the
> exclusions say, the Microcosm's way (its D3): the tempo source is a device setting ("receive
> MIDI clock" on the pedal, the Tempo source in the plugin), and STAT's `tempo_source` byte is
> reserved and must be 0, so presets that sound the same keep one `sound_hash`. A preset stores
> its time mode, subdivision and tempo as an integer µs per quarter, which saving a preset
> captures from the live performance (clock.md §10.3); whether a load recalls that tempo is the
> device setting `global.tempo_recall`. Under Keep, the default, the running tempo carries
> across every load: the engine's Exact load plays the stored tempo, and the producer re-asserts
> the running one as a logged event at frame 0 (clock.md §2.5, its D4 and D17). The owner
> confirmed these answers, with the rest of clock.md §11.5, on 2026-10-08.

| Captured | Section |
|---|---|
| Every leaf parameter as exact binary32 (authoritative for sound) | STAT |
| Performance initial state: global reverse, Time-knob mode, subdivision, tempo source, tempo as an exact value (binary64 BPM or integer µs per quarter) | STAT |
| The compiled `ModeBlob` and its JSON source | MODE + JSON |
| Macro positions (pickup references, never authoritative over leaves; outside `sound_hash`, so presets differing only in knob position share one) | CTRL |
| Expression (`{target ParamId, min, max, curve}`), footswitch and per-preset MIDI assignments | CTRL |
| Looper routing and loop reference (reserved; loop audio is a separate content-addressed file) | STAT / LOOP |
| Name, author, tags, timestamps, authoring app version, copied from the JSON (§6.6) | META |

**Excluded:** engine runtime state (the load mode defines it), physical knob positions (§6.8),
freeze engaged (presets load with freeze off), global settings such as MIDI channel and clock
source (as on the Microcosm, `docs/research/microcosm.md` §7), Input/Output level and onset
calibration (§4.8), and the DAW session snapshot (§6.9).

### 6.3 The `.bsp` package

> **Update (2026-10-07, mode-compiler lane B).** The package is built as
> [mode-compiler.md](mode-compiler.md) §5–§6 specifies, in `dsp/src/blob/` behind
> `brainscape/Preset.h`: a 128-byte header that adds `control_hash`, MODE as tagged chunks (a new
> chunk is an `UnsupportedFeature`, not a `blob_format` bump), CTRL and META with their own rules,
> and `DecodePreset`, `ValidateMode` and the encoder, integer-only (no floating-point instruction
> on the M7). That design amends this section when it is accepted (its §12.5).

A fixed header then tag-length-value sections; integers little-endian, floats as raw binary32
bits, nothing `memcpy`'d from a struct.

| Header field | Type | Meaning |
|---|---|---|
| `magic` | 4 bytes | `BSPK` |
| `package_format`, `flags` | u16, u16 | container version; e.g. `FACTORY`, `JSON_STALE` (§6.9) |
| `sound_rev` | u32 | revision compiled or last verified under |
| `blob_format` | u32 | STAT/MODE/CTRL layout |
| `schema_version` | u32 | JSON authoring schema |
| `sound_hash` | 32 bytes | SHA-256(STAT ‖ MODE): the identity of "the same sound" |
| `package_hash` | 32 bytes | SHA-256 over the header (this field zeroed) and all sections |

Sections: `STAT` (sorted `(u32 ParamId, f32 value)` per leaf, plus performance state), `MODE`
(the `ModeBlob` field by field, ≤ 4 KiB), `CTRL` (with its own `control_hash`), `JSON` (canonical
source), `META`, `LOOP` (reserved). ≤ 16 KiB per preset (*estimated*). Fixed-width fields,
explicit zeroed padding, `static_assert` on `sizeof`/`offsetof` in both builds; no pointers,
`size_t`, `long`, `bool` or enums without a fixed underlying type, whose sizes differ between x64
and the M7 (*measured* [parity-v1]). Hashes cover serialized bytes, never structs.

### 6.4 Canonical JSON

> **Update (2026-10-07, mode-compiler lane B).** The numbers are written and read by in-house
> integer code (`compiler/src/Number.*`, mode-compiler.md §6.5; no vendored `fast_float`, owner
> question Q1 provisionally), and the two values below are written with eight digits,
> `±7.0385307e-26`, not nine (mode-compiler.md §6.4). Amended here when that design is accepted.

The JSON is what people read, edit and keep in git. It follows the mode schema rules
(`grain-engine.md` §5: `schema_version`, stable names never reused, plain units, per-key
defaulting) plus:

- **Numbers:** the shortest decimal that round-trips the binary32 value, or 9 significant digits
  where reading that decimal as binary64 and rounding to float would differ; only ±7.038531e-26
  need the fallback (*measured* exhaustively [preset]).
- **Readers** are correctly rounded binary32 parsers (vendored `fast_float`); JavaScript's
  `JSON.parse` plus `Math.fround` is exact thanks to the fallback.
- **Layout:** schema key order, two-space indent, LF, UTF-8; `bspc fmt --check` gates commits.
- The JSON carries `sound_rev` and a derived `sound_hash` that tooling verifies.
- Rejected: RFC 8785 (prints 0.55 as `0.550000011920929`) and hex floats (not JSON numbers).

### 6.5 Version fields

| Field | Changes when |
|---|---|
| `schema_version` | the JSON schema changes (per-key defaulting for additions, name-keyed migration for breaks) |
| `blob_format` | the STAT/MODE/CTRL layout changes; firmware keeps decoders for older formats |
| `package_format` | the container changes |
| `sound_rev` | any change to `dsp/`, shared build constants or the FP profile that **can** change output, whether or not a golden vector notices (profile §5.12; `sound-rev-gate`, §3.4). The dither-key fix (§4.11) changes output only after 2²⁹ samples, yet an old pedal and a new app would differ. Firmware-only changes never touch it. |
| `protocol_version` | device messages change; minor = additive (§7.3) |
| firmware semver + git hash | every release; reported in `HELLO` |

### 6.6 Compilation happens on the desktop only

> **Update (2026-10-07, mode-compiler lane A).** `bspc` is built
> ([tools/bspc/README.md](../../tools/bspc/README.md)): `compile`, `decompile`, `fmt`, `verify`,
> `stamp`, `lint`, `diff`, `derive`, `roundtrip` and `migrate-session`
> ([mode-compiler.md](mode-compiler.md) §8.2); `render` comes with the audition tooling. The header's `FACTORY` flag is set for ids under `factory.`, so it too
> comes from the JSON. Amended here when that design is accepted (its §12.5).

The app and **`bspc`** (`tools/bspc/`: `compile`, `decompile`, `fmt`, `verify`, `render`) compile
JSON into STAT + MODE + CTRL and pack the `.bsp`. The firmware links only the decoder and a
**structural validator** (section lengths, hashes, integer bounds, table sizes), so a corrupt or
hostile upload cannot cause an out-of-bounds read; it never parses JSON. One compiler means
compile-time math cannot diverge across C libraries (UCRT's `exp2f` is 1 ULP off on 0.063 % of
the semitone grid, *measured* [parity-v1]), and the pedal's newlib `strtof` rounds twice
(*measured* [preset]).

**Compilation is a pure function of the JSON.** Every package byte, META included, comes from the
JSON or fixed constants, never the clock, user, host OS or compiler build, so compiles are
byte-identical on every CI leg (profile §2.6; `bspc-roundtrip`, §3.4). The app writes authoring
fields into the JSON on save; `bspc` only copies them. On-pedal compilation may come later, gated
on CI proving identical bytes from the ARM build.

**Factory packages** are committed twice under `firmware/factory/` (JSON and `.bsp`), because one
CMake configure has one toolchain and the firmware configure cannot run a host `bspc`. The
firmware embeds the committed bytes; desktop CI fails unless recompiling the JSON reproduces them.
The mode schema and `modes::Compile` get their own design document (merged step 3, §8.1).

### 6.7 Exact and Spillover loads

| | **Exact load** | **Spillover load** |
|---|---|---|
| What happens | `Restart`, then the complete preset | complete preset applied; history, feedback, tails, in-flight grains and scheduler phase continue; mode switches with Trails; the random-number epoch restarts |
| Comparable to an app render | **yes**: a defined start state | no |
| Player hears | trails cut; on the pedal the wet path mutes during the clear | trails continue |
| Cost | ≈45–160 ms on the pedal (*estimated*, §4.9), or none if the restart watermark (§4.9, not yet prototyped) proves out | a stamped event, no large clears (§6.1) |
| Pedal | global setting "Exact preset load" | recommended default, pending the DWT measurement of `Restart` (§10.2 Q20) |
| App | audition, offline render, PARITY, plugin restart option | live monitoring of a pedal set to Spillover |

> **Update (2026-10-07, Rev7 bench, sound revision 1).** The Exact load's cost is measured:
> 47.17 ms on the owner's Daisy Seed Rev7, so the wet path mutes for about 47 ms (§4.9). The
> pedal's default no longer waits on that measurement, only on §4.9's two alternatives (the
> watermark and a DMA2D fill, neither yet prototyped) and the owner's choice (§10.2 Q20;
> [reviews/rev7-silicon-record.md](reviews/rev7-silicon-record.md) §3.6).

**Neither mode makes live playing comparable to the app**; Exact's value is a defined start state
for PARITY and audition, so the pedal default is a user-experience choice. Spillover is
recommended because the Microcosm's most-cited preset complaints are cut trails and interrupted
loops (`docs/research/microcosm.md` §7, §11), unless `Restart` or the watermark makes Exact
seamless. **Spillover never reconverges** with a canonical render, even without feedback
(*measured* [review-num]; record §2.6), but it is deterministic, so a captured session replays it;
convergence would need the scheduler re-armed at the load (profile §8.3 Q10). Spillover restarts
randomness through the **random-number epoch** (profile §5.9), never by moving the counter, which
the feedback FIFO indexes (`Engine.cpp:458-459`); whether Spillover loads align to the onset
detector's 256-frame hop is profile §8.3 Q4, and any alignment is applied by the producer's
stamp, never by the engine deferring an event. Global reverse loads from the preset; freeze
always loads off.

### 6.8 Knobs are events, not state

After a load, sound depends only on stored values. A pot affects sound only after soft takeover
(`grain-engine.md` §6, "Soft takeover") unlocks it, then as events. Firmware deadband and
hysteresis ensure **a stationary pot emits no events**, since a leaked `SetParam` re-runs the
coefficient rebuild (`Engine.cpp:415-429`) and the pedal drifts from the app. Pickup state lives
in `firmware/` (and the app's external-controller path); macro fan-out and expression curves
live in `dsp/`.

### 6.9 Pedal-side edits, the desktop library and DAW session state

- **Edits saved on the pedal** rewrite STAT and CTRL, keep the JSON verbatim and set
  `JSON_STALE`; on download the app patches the values into the JSON by stable name and
  re-canonicalizes. The pedal never writes JSON.
- **Desktop library:** a folder of self-contained `.bsp` files, with "Export JSON" for git.
  Factory presets ship with the app at its revision, identical to those in its firmware.
- **DAW session state** (`getStateInformation`): a wrapper header, the current `.bsp` and wrapper
  settings (levels, restart and pedal-grid options). If a host set changed a value (§5.3), STAT
  is rewritten and the JSON patched before saving, so the saved `sound_hash` matches the bits.
  Engine runtime state is never saved; the design's unimplemented `SaveState`/`LoadState` stays a
  separate session artifact, never sent to the pedal. A restore goes through `LoadPreset`:
  **Exact** when nothing has played since the engine's last `Init` or restart (a project being
  opened), because there are no trails to keep and the restore then defines the start state
  (the engine has nothing to clear then, so the load costs what `Reset` does, §4.7);
  **Spillover**, as a load event at the next block's first frame, when it arrives while the
  engine runs (a host's preset recall), because an Exact load would clear 17 MiB on the audio
  thread and cut the trails, which a delay keeps across a preset change as the pedal's
  recommended load does (§6.7). The engine's own preset also loads Exact after every `Init`
  (§4.1) and at a restart on transport start (§4.9). Reproducibility comes from that restart
  option, never from a restore.

## 7. Upload and device link

### 7.1 Transport

| Layer | Choice |
|---|---|
| Pedal USB stack | **TinyUSB** (MIT), replacing libDaisy's USB code (§7.2) |
| USB device | USB-MIDI with two virtual cables, each named by its jack string: cable 0 "Brainscape" for performance MIDI (CC, program change, clock), cable 1 "Brainscape Editor" for the SysEx (System Exclusive) protocol of §7.3. **No DFU (Device Firmware Upgrade) runtime interface in v1**: Windows has no in-box DFU driver and would show an unknown device on every connect; DFU is entered through `REBOOT_TO_BOOTLOADER`. Optional later: a vendor bulk interface (WebUSB/WinUSB) and USB disk mode |
| Desktop | JUCE `MidiInput`/`MidiOutput` behind the plain-C++ `link/` library; hotplug through `juce::MidiDeviceListConnection` |
| Day-one fallback | **microSD sneakernet** (§7.7) |
| Secondary client | a WebMIDI page speaking the same protocol (§7.7) |

USB-MIDI SysEx is driverless everywhere, reachable from WebMIDI, and spoken by JUCE (which has no
serial or bulk API); each cable is its own port, so a DAW can hold one while the app holds the
other. The editor port's name varies by OS (`MIDIIN2 (Brainscape)` under Windows' legacy
naming), so the app finds it with a rate-limited `HELLO` probe, never by name, and on
single-client Windows 10 tells the user which port to disable in a DAW that holds it. Windows
MIDI Services on Windows 11 25H2 corrupts inbound SysEx until a fix slated for November 2026
(microsoft/MIDI issue 1040); CRC and retry cover it (§10.2 Q16). A 16 KiB preset uploads in well
under a second (*estimated*); loop audio never travels over SysEx.

### 7.2 Why not libDaisy's USB, and the SD-card glue problem

libDaisy's USB glue (`src/usbd/`, `src/usbh/`) and stock FatFs SD glue (`src/util/sd_diskio.c`,
`usbh_diskio.c`) carry ST's SLA0044, whose clause 5 forbids redistribution under open-source
terms and names the GPL. **Every libDaisy firmware links them today:** `src/sys/system.cpp:96-121`
(beside `System::Init`) defines OTG_HS interrupt handlers that reference the SLA0044 files, kept
by the vector table (`core/STM32H750IB_qspi.lds:27-32`), and `FatFSInterface::Init` pulls the USB
host stack into an SD-only build (*measured* by mock links [review-fw], [preset]; record §2.7).
libDaisy's USB is also CDC **or** MIDI, never composite, truncates SysEx at 128 bytes
(`src/hid/MidiEvent.h`) and ships ST's VID/PID `0x0483:0x5740`. **Replace, don't negotiate:**

- **TinyUSB** for USB.
- **A pinned, upstreamable libDaisy patch** (`firmware/patches/`, ~30 lines, *estimated*) moving
  the three OTG_HS handlers into libDaisy's `usb.cpp` and `usb_host.cpp`. Mandatory if TinyUSB
  runs on the High-Speed core (pins D29/D30), whose `OTG_HS_IRQHandler` it must own; on the
  onboard USB-C (OTG_FS) an interim GPL shim defining zeroed `hhcd_USB_OTG_HS` and
  `hpcd_USB_OTG_HS` keeps the ST files out.
- **A GPL-clean FatFs disk I/O layer** (`diskio.c`) over ST's BSD-3 HAL SD driver or libDaisy's
  `SdmmcHandler` (confirm its header), never `FatFSInterface`; the FatFs core and ST's
  `ff_gen_drv.c` are BSD-style and fine.
- `firmware-elf-audit` (§3.4) checks every image from the first. A GPLv3 §7 permission for ST
  code is rejected: SLA0044 clause 5 may defeat it, and outside GPL contributions would block it.

**TinyUSB** (MIT) has a DWC2 driver for the STM32H7 at Full and High Speed, composite devices
and a Daisy Seed board package. The Seed3's onboard USB-C is OTG_FS (PA11/PA12); D29/D30
(PB14/PB15) reach the High-Speed core through its internal Full-Speed PHY (§10.2 Q9). **Two
cables need TinyUSB ≥ 0.21.0:** `tud_midi_n_stream_read` ignores its cable argument, so cable-0
clock would corrupt every editor frame. Use `tud_midi_n_demux_stream_read` (or
`tud_midi_n_packet_read` with one SysEx assembler per cable), drop real-time bytes (0xF8–0xFF)
inside SysEx, never assume one SysEx message per transfer, and test uploads under cable-0 clock.

**Bring-up rules:**

- Keep libDaisy's `System::Init` clock tree and **never call the TinyUSB board package's
  `board_init` or `SystemClock_Config`**: its clock setup would give a 256 MHz system clock on
  the Seed's 16 MHz crystal (*calculated*), and it links for internal flash. Take only the
  package's pin and VBUS settings.
- Clock USB from HSI48 (the internal 48 MHz oscillator) with CRS (clock recovery) synchronized
  to start-of-frame: libDaisy selects HSI48 (`src/sys/system.cpp:510`) but never enables CRS,
  and PLL3 is the audio PLL (`:489-492`).
- Put the OTG, SDMMC1 and non-audio DMA interrupts below the SAI DMA stream, because libDaisy
  sets them all to priority 0 (`src/sys/dma.c:17-49`, `src/per/sdmmc.cpp:84`).
- Set the USB pins' GPIO alternate functions and the composite descriptors of §7.1 in
  Brainscape's own code; the OTG interrupt calls `tud_int_handler`, and `tud_task()` runs in the
  main loop, never starved.

### 7.3 Protocol

**Framing:** one message per SysEx frame on the editor cable; the codec lives in `protocol/`,
shared by firmware and app:

```
F0  <manufacturer ID: 3 bytes (00 xx yy), or 1 byte (7D) in development builds>
    <device: 0x7F = any>  <protocol major>  <message type>
    <sequence number: 2 × 7-bit>
    <8-to-7-bit packed: u16 payload length | payload (≤ 512 bytes) | u32 CRC32>
F7
```

At most ~600 bytes per frame; fields little-endian before packing; the ID is variable-length as
the MIDI specification defines.

| Message | Request → reply |
|---|---|
| `HELLO` | app protocol version and revision → protocol version, firmware semver, git hash and toolchain ID, `sound_rev`, supported `blob_format` and schema range, capability bits (PARITY, LOOPS, MSC, FW_SD, and FW_DFU, meaning the bootloader can be entered for DFU), slot count, size limits, **per-command timeouts**, the STM32's 96-bit unique device ID, SD presence, file system and free space, active slot |
| `LIST(start, count)` | per slot: `sound_hash` prefix, `package_hash`, `sound_rev`, flags, name (verification results live in the app) |
| `WRITE_BEGIN(slot, size, sha256, expected_old_hash)` / `WRITE_CHUNK(offset)` / `WRITE_COMMIT` | chunks go to the slot's inactive copy (§7.5), each acknowledged once written; `WRITE_COMMIT` (a job) reads back, verifies, validates, writes the header |
| `STAGE_*`; `LOAD(slot or staged, Exact or Spillover)` | as WRITE, into a staging copy; audition on the pedal (a job when Exact) |
| `DELETE(slot, expected_hash)`, `SWAP(a, b)`, `READ_BEGIN` / `READ_CHUNK` | slot management; download |
| `PARAM_SET` / `PARAM_GET` | tethered edits; the reply gives the frame the change applied at |
| `PARITY(…)` | §7.4; a job |
| `FW_BEGIN` / `FW_CHUNK` / `FW_COMMIT`, `REBOOT_TO_BOOTLOADER` | firmware update (§7.6) |
| `BACKUP_ALL`, `LOG`, `STATUS(job id)` | backup (a job); diagnostic log; job state, progress and result |

**Reliability:** each request carries a sequence number and gets ACK with a status or NAK with an
error; short commands time out after ~250 ms and retry up to 3 times (*estimated*). Chunks are
idempotent, keyed by (package SHA-256, offset), and `WRITE_BEGIN` reports bytes already held, so
transfers resume; `expected_old_hash` makes a write fail rather than clobber an edit saved on the
pedal meanwhile.

**Long commands are jobs**, because PARITY renders for seconds, an Exact `LOAD` runs `Restart`
and one SD write may stay busy 250–500 ms, all in the firmware main loop beside `tud_task()`, and
a retried PARITY must never restart a render and wipe the player's history again. A long command
is ACKed at once with a **job id** and expected duration and completes with a separate message
(or `STATUS`); it sends BUSY/progress at least every 100 ms; it runs in slices of a few
milliseconds interleaved with `tud_task()`; retries are idempotent by job id; and durability is
confirmed only by `WRITE_COMMIT`'s completion, with a host timeout of at least 2 s.

**Physical confirmation:** any web page with WebMIDI SysEx permission can send these messages, so
firmware update, delete-all, factory reset and restore-all need a footswitch press within a
timeout, signalled on the LEDs. Also proposed: an **editing-session unlock**, one press before a
connection's first state-changing command, so that a hostile page cannot overwrite slots one by
one (§10.2 Q23).

### 7.4 PARITY and verified upload

PARITY is the only end-to-end proof of 1:1 on hardware; profile §6.7 defines its meaning. The
firmware's **render mode** serves it, and before USB exists reports hashes over UART or SWD to a
hardware-in-the-loop CI runner.

- **Request:** `PARITY(source = slot or staged, signal_id, seed, seconds, input mode,
  segment = 1 s)`, a job.
- **Pedal:** mutes live processing; `Restart` + `LoadPreset(Exact)`; renders the test signal
  through the input mode (§4.8) with its scripted events (parameters, a macro move, freeze, taps)
  at fixed frames, multiples of 48 until the block-split fix lands; returns SHA-256 per segment
  and overall, the input hash, `sound_rev`, the firmware git hash (which keys the Verified label,
  §7.6) and the toolchain ID (diagnostic only, never compared to decide identity; profile
  §5.12); restores live operation. The player's history is lost; the app warns first.
- **App:** renders the same through the shared render function (§4.9) and shows
  **bit-identical** or **differs from second N**.
- **Test signals** come from the integer-only generator (`TestSignal.h`, profile §5.13), whose
  version is part of the golden data; the golden data also carries coverage counters so that
  vectors provably fire onsets and marks (profile §6.1).
- **Cost:** a 10 s vector takes ~3.5–7 s plus `Restart` at the design's derived 30–68 % load
  (`grain-engine.md` §8, before the profile's additions; *estimated*). **Verified upload** runs
  a 2 s PARITY on the stored slot (~1–2 s, *estimated*) and, on a pass, shows **Verified 1:1 on
  rN · firmware ‹hash›** (§7.6).

> **Update (2026-10-08, Rev7 silicon record).** The cost is measured. On the owner's Daisy Seed
> Rev7 the parity image rendered the whole golden corpus, each preset restarted by
> `LoadPreset(Exact)`, at 2.57× realtime at sound revision 3 (709 s of audio in 276 s) and 2.66×
> at revision 1 (639 s in 240 s), with the engine's code in ITCM and the renders in the main
> loop ([reviews/rev7-silicon-record.md](reviews/rev7-silicon-record.md) §1.2, §1.3, §2). At
> that rate a 10 s vector takes about 3.9 s and verified upload's 2 s about 0.8 s, each plus
> `Restart`'s 47.2 ms (§4.9) (*derived*). The rate is the corpus's average; a vector's own time
> follows its preset's load. The DWT pass measured the corpus preset `dense_1ms` at a mean of
> 80.0 % of the budget at revision 1, about 8 s of engine time for a 10 s vector (*derived*),
> and the densest bench configuration, `pess_births`, at 119.6 %, slower than realtime (silicon
> record §3.2).

**No scratch engine in v1:** PARITY reuses the live engine with the canonical 2²² ring, because
the design plans SDRAM at 63.3 of 64 MiB once the looper exists (`grain-engine.md` §7) and a
second engine also needs hot and warm arenas (profile §6.6). Smaller rings appear only in test
oracles, for vectors a host leg has shown to hash identically at that size (profile §6.4).

### 7.5 Pedal storage: a microSD slot store with A/B copies

QSPI flash holds only the firmware image, because a QSPI write stalls execution
(`grain-engine.md` §7); the preset research's QSPI preset blob
(`docs/research/preset-parameter-and-patch-format.md` rec #4) is superseded. Factory presets are
`const` data in the image, so they always match its sound revision. User presets live on
microSD:

- **Slots:** 0–127 in banks of 8; program change *n* selects slot *n*, Bank Select factory or
  user (proposed; the Microcosm has 16 preset slots and the Chroma Console 80; §10.2 Q21).
- **`/BRAINSCAPE/SLOTS.DAT`**, created once with a contiguous cluster chain: 128 slots × 2 copies
  × 32–64 KiB plus a staging copy (8–16 MiB). libDaisy's FatFs lacks `f_expand`
  (`src/sys/ffconf.h:70`), so the project's FatFs configuration enables it, or contiguity is
  verified at boot.
- **Power-loss layout:** A copies in one region and B copies in another, each aligned to and
  separated by at least one SD allocation unit (`AU_SIZE`, typically 4 MiB); each copy has a
  header `{magic, slot, generation, length, sha256}` in its own sector.
- **Save:** chunks stream into the **inactive** copy's precomputed sectors with no FAT changes,
  so a package never has to fit in RAM (12 KiB of staging is budgeted, `grain-engine.md` §7);
  the commit reads back, verifies the SHA-256, validates and **writes the header last**; a load
  takes the newest copy whose hash verifies. `LOAD(staged)` keeps only decoded STAT and MODE in
  RAM. Access is by raw sectors only, never through an open FatFs `FIL`, whose private buffer
  would go stale (`_FS_TINY 0`, `ffconf.h:218`).
- **Brown-out:** a PVD (voltage detector) interrupt stops new writes; bulk capacitance finishes
  the sector in flight (§10.2 Q8).
- **The guarantee is limited:** FAT is not journaled and `f_rename` will not overwrite, hence the
  A/B copies, but an interrupted program on a consumer card can corrupt other data in its erase
  block (record §2.7). The claim is **designed to survive power loss mid-save on cards that
  honour the SD specification's write semantics**; phase D adds a relay-driven power-cut test of
  at least 1,000 saves.
- **Folders** under `/BRAINSCAPE/`: `IMPORT/` (validated and ingested at boot or on command, then
  moved to `IMPORT/DONE`), `EXPORT/`, `LOOPS/<sha256>.raw` (immutable, written before the slot
  commit that references them, orphans collected at boot), `FW/` (§7.6). **Nothing whose name
  contains `.bin` is left at the card root** (§7.6).
- **RAM cache** of at least the current bank's STAT + MODE, so a switch never waits on the card
  (§10.2 Q22). All SD I/O runs from the main loop in slices that never starve `tud_task()`, under
  the design's DMA cache-coherency rule (`grain-engine.md` §7).

### 7.6 Firmware and sound-revision alignment

Each app release bundles the firmware built from the same commit. On connect `HELLO` compares
sound revisions, and the app is never silent about the result: it shows exactly the labels of
profile §2.5 (Same engine, Verified 1:1, 1:1 render, Resampled, the emulation suffix and the two
update prompts), never a bare "1:1". In this app, *Verified 1:1 on rN · firmware ‹hash›* is per
slot and cleared when the slot, build or revision changes; *1:1 render* also requires the
reproducible flag (§4.9); and upload stays allowed when the pedal is newer.

After a firmware update that changes `blob_format`, the app recompiles the pedal's presets from
their JSON, re-uploads them with verification, and reports per preset whether `sound_hash`
changed. The firmware keeps decoders for older blob formats, so an SD-only update still loads
old presets, flagged inexact.

**The stock Daisy bootloader** (record §2.7) flashes the **first root entry whose name contains**
`.bin` or `.BIN`, validates only the stack pointer and entry point, cannot read exFAT
(`ffconf.h:225`) and then silently does nothing, re-flashes a differing root `.bin` on every boot
(silently reverting an update made another way), and has no rollback. Hence the update path
(primary, no drivers):

1. `FW_BEGIN` is refused unless the card is FAT12/16/32; the pedal offers an in-pedal FAT32
   format (`_USE_MKFS 1`, `ffconf.h:64`) behind a footswitch press.
2. The app backs up every slot (`BACKUP_ALL`); the pedal copies its running image from QSPI to
   `/BRAINSCAPE/FW/previous.img`.
3. `FW_BEGIN`/`FW_CHUNK` upload the image (~0.5–1 MB, *estimated*) to
   `/BRAINSCAPE/FW/incoming.tmp`; the pedal checks its SHA-256, vector table, size and embedded
   header carrying `sound_rev`.
4. `FW_COMMIT` (a job, after a footswitch press) moves **every** root entry containing `.bin`
   (including `._*` files) into `/BRAINSCAPE/FW/`, renames the image to the fixed root name
   `BRAINSCAPE.BIN`, and calls `System::ResetToBootloader`.
5. The bootloader (v6.4) flashes it to QSPI at `0x90040000`.
6. The new firmware boots, self-checks, and only then renames the root file to
   `/BRAINSCAPE/FW/applied-<version>.bix`, so an interrupted flash is retried; an independent
   watchdog (IWDG) turns a hang into a reset.
7. The app reconnects, migrates presets if `blob_format` changed, and optionally runs PARITY.

**Rollback:** manual and published (§7.9): copy `/BRAINSCAPE/FW/previous.img` to the root as
`BRAINSCAPE.BIN` and power-cycle. Automatic rollback needs a rebuilt bootloader with a
boot-attempt counter (§10.2 Q4). **Alternatives:** DFU through `REBOOT_TO_BOOTLOADER` and a DfuSe
download to `0x90040000` with a bundled `dfu-util` (GPL-2.0-or-later, as a subprocess) or libusb
(LGPL), which on Windows needs WinUSB bound with Zadig unless the bootloader is rebuilt with
Microsoft OS 2.0 descriptors; and the WebUSB programmer at flash.daisy.audio. Both need the
bootloader variant for the exposed USB port.

### 7.7 microSD sneakernet and the WebMIDI client

**Sneakernet, from day one:** `.bsp` files go into `/BRAINSCAPE/IMPORT/`, a firmware image to the
root of a FAT card as `BRAINSCAPE.BIN` with no other root `.bin`. It needs no USB code or drivers,
is the simplest route for modified firmware (§7.9), and assumes the enclosure exposes the card
(§10.2 Q8). **USB disk mode** (later, optional) hands the block device to the host, so the pedal
stops all SD access for the session and re-indexes and verifies hashes afterwards; only as an
explicit gesture. **WebMIDI client** (optional): Chrome, Edge and Firefox (not Safari), HTTPS and
SysEx permission; in-browser compilation needs the `dsp/` compiler built to WebAssembly under the
profile.

### 7.8 Identifiers

- **SysEx manufacturer ID:** buy a perpetual MIDI Association ID ($240 once, or with a $600/yr
  membership). 0x7D (private, non-commercial) is for development builds only; 0x7C
  (non-commercial public releases) forbids proprietary-format SysEx. Neither may ship in a sold
  product.
- **USB VID/PID:** a free pid.codes PID under VID `0x1209`, which requires the firmware **and**
  the PCB files to be published under open licences, so the request follows publication of the
  PCB files. One PID for the application, ideally one for a rebuilt bootloader. Never ST's
  `0x0483:0x5740`.

### 7.9 GPLv3 Installation Information

A pedal sold with GPLv3 firmware is a "User Product" (GPLv3 §6): conveying it requires the
Corresponding Source (or a written offer to supply it) **and** the Installation Information to
install and run modified versions. Firmware stays unsigned (any future signing accepts
user-enrolled keys), and STM32 readout protection stays at level 0. Level 2 is irreversible: it
permanently disables JTAG/SWD and ST's system bootloader and freezes the option bytes, so the
Daisy bootloader could never be repaired or replaced. Level 1 adds nothing for an open-source
product. The published procedures cover the build with the pinned toolchain (profile §6.8) and
libDaisy patch, the three install routes (SD, DFU, web flasher) and the manual rollback; the
bootloader's source is shipped or linked. Warranty may be voided for modified firmware.

## 8. Delivery plan

### 8.1 Milestones, phases and dependencies

**One merged sequence** governs this design, the profile (whose §8.4 places its steps 1–14 in
it) and STATUS.md's next steps. Before 2026-10-05 those next steps put the engine's SPSC queue
second and an iPlug2 plugin seventh; they now follow this sequence. A sound-revision bump is
cheap until the revision is **published** (only goldens and factory packages are regenerated),
so every sound-changing change lands before revision 1 is published.

1. **The profile and the `dsp/` API, no hardware** (profile steps 1–9; phase A), including all
   three parts of the block-split fix (§4.11), `Restart`, the epoch, `LoadPreset` and
   frame-stamped events.
2. **Mint internal revision 1** (profile steps 10–11): golden hashes and CI gates, then nightly
   legs.
3. **Mode compiler** (its own design document), parameter-ID reconciliation and macro IDs, the
   inputs of the §5.7 gate (phase C); and the engine features the factory modes need before they
   can be curated, above all the **CLOCK trigger source with tempo sync** (tap, subdivision,
   MIDI clock and host tempo, all delivered as stamped events, §4.10), which the acid-test modes
   Seq, Arp, Mosaic, Pattern and Warp use (`grain-engine.md` §4, trigger vocabulary), plus the
   other unbuilt features STATUS.md lists under "Engine features from the design not yet built".
   Each lands as an internal revision bump.
4. **Factory-mode curation through the app's offline audition**, the central product risk
   (STATUS.md, "Known gaps and deferred work"); inaudible bumps only mean recompiled packages and
   regenerated goldens.
5. **Hardware bring-up and the hardware-gated decisions** (profile steps 13–14; phases D, E):
   subnormal cost and flush (profile §4.2), explicit FMA (profile §7.3), polynomial kernels or
   tables (profile §3.9), `Restart` time or the watermark and the default load mode (§4.9, §6.7).
   Several change sound, so they precede step 6.
6. **First public sound revision:** parameter IDs, names, tapers and sound are fixed together;
   only then do the public plugin, app and firmware ship.

**The looper** (not designed; `grain-engine.md` §12 item 12) changes the canonical
configuration (`looperFrames`, profile §2.3 precondition 2), which `kSoundRevision` certifies
(profile §5.12), so it is a sound-changing change. By the rule above it lands before step 6,
designed and built in parallel with steps 3–5. The alternative, shipping the first public
release without a looper and adding it later as a revision bump (with the update prompts of
§7.6 and risk 3 of §10.1), is an owner decision (§10.2 Q29).

Phase B (the JUCE skeleton, with profile step 12) runs in parallel with steps 1–4. A, B and the
start of C need no hardware; D needs a Seed3; E needs D's protocol server or the simulator.

- **A — `dsp/`:** profile steps 1–9, plus `PresetState`, `DecodePreset`, `LoadPreset` (STAT-only
  until phase C), the input-mode function, tapers with `PlainFromNormalized` and
  `NormalizedFromPlain`, and the shared render function; time-parameter smoothing is decided here.
- **B — JUCE skeleton:** §2.2, §3, §4 and §5.1–§5.6. **Nothing public ships** before the §5.7
  gate and step 6.
- **C — Presets** (needs the mode compiler): §6.3–§6.6, library and editor views, §5.7.
- **D — Pedal side:** §7 from the first firmware build (libDaisy patch and audit first),
  bring-up checks (§4.8, §7.2), the DWT measurements (§10.2 Q13), render mode over UART/SWD, and
  the engine's SPSC queue as the transport from its single producer, the control loop, to which
  interrupt and MIDI sources post (§4.10). A **desktop pedal simulator** (protocol server and
  slot store over abstract transport and block-device interfaces with an in-memory card) lets CI
  test the protocol end to end and E start early.
- **E — Device link in the app:** `link/`, its MIDI adapter and the Device view (§7.3–§7.6).
- **F — After v1:** session capture (§10.2 Q27), CLAP direct processing (§5.6), WebMIDI and disk
  mode (§7.7), older engine revisions in the app (only if Q25 is decided against its
  recommendation), on-pedal compilation, rate-invariant constants and a native-rate plugin mode
  (§4.2), Windows-on-ARM.

### 8.2 Effort

All figures are *estimates* in engineer-days; per-item figures and their sources are in record
§4. **(shared)** work is also counted in profile §7.1, so the two subtotals must not be added.

| Work | Days |
|---|---|
| `dsp/` additions: FP profile target, `Restart`, preset loading and epoch, input functions and generator, opaque `Engine`, block-split fix **(shared)**; tapers, input mode, render function (2–3 unshared) | 8.5–14.5 |
| JUCE build and CI, parameter layer, resampled mode, standalone shell, wrapper hardening | 7.5–15 |
| `plugin/parity-host` and the format, Rosetta and Prism legs **(shared)** | 3–5 |
| `.bsp`, canonical JSON, `bspc` | 5–10 |
| Pedal: TinyUSB device, libDaisy patch and audit, disk I/O, slot store, power-loss rig | 16–26 |
| Protocol, jobs, PARITY, firmware update and safeguards, render mode | 19–30 |
| Device-link UI | 5–10 |
| **This design** | **64–111** (≈ 13–22 weeks; 55–94 unshared) |
| **Known work with the profile** (profile §7.1: 28–46) | **83–140** (≈ 17–28 weeks) |

Not estimated: the GUI and editor views (the largest unknown), the mode compiler, factory-mode
curation, and hardware bring-up with the DWT pass. Cash: $240 SysEx ID, $99/yr Apple Developer
Program, ~$60 hardware-in-the-loop rig (Seed3 + ST-Link V3).

## 9. Decisions

### 9.1 Decisions taken

The reasons are in the cited sections.

| Decision | Choice |
|---|---|
| Framework, licence | JUCE 9.0.3 under AGPLv3; AGPL code only in desktop targets (§3.5) |
| Product shape | one `AudioProcessor`; the custom Standalone is the app and owns the device link; VST3 and AU, LV2 and CLAP optional (§2) |
| Engine rate | 48 kHz everywhere; at other host rates two-way resampling with r8brain-free-src, libsamplerate as the fallback (§2.3, §4.2) |
| Build | pinned FetchContent; `brainscape_dsp` PUBLIC on JUCE's shared-code target; PIC in plugin builds only; no LTO (§3.2, §3.3) |
| Engine inputs | computed only in guarded or exact `dsp/` code (§4.6, §4.8) |
| Parameters | plain-value layer, no APVTS; tapers in `dsp/` (§5) |
| Presets | complete state via `LoadPreset`; `.bsp`; shortest-digit JSON; desktop-only compilation (§6.1–§6.6) |
| Loads and events | Exact for audition and PARITY, Spillover the recommended pedal default; absolute frame stamps (§4.10, §6.7) |
| Block-split bug | three-part fix before internal revision 1; 48-frame grid until then (§4.11) |
| Pedal USB and storage | TinyUSB, libDaisy patch, own disk I/O; A/B microSD slot store (§7.2, §7.5) |
| PARITY, update, skew | live engine reused; SD update through the Daisy bootloader; revisions compared on connect (§7.4, §7.6) |
| Identifiers, GPLv3, safety | bought SysEx ID, pid.codes PID; unsigned firmware, readout protection 0; footswitch confirmation (§7.3, §7.8, §7.9) |

### 9.2 Where the evidence disagreed, and how this document resolves it

Record §3 keeps the sixteen points where the evidence disagreed, with measurements and
resolutions. Their stable numbers: 1 flush-to-zero in the guard (profile §4.2); 2 the SPSC queue
(§4.10); 3 rate-invariant engine (§2.3); 4 where JSON is parsed (§6.6); 5 flushing an engine
(§4.9); 6 PARITY scratch engine (§7.4); 7 hash function (§7.4); 8 macro positions (§6.2); 9 PIC
and visibility (§3.2); 10 iPlug2 and contract #7 (§1.1); 11 block-split fix scope (§4.11);
12 input pipeline (§4.8); 13 parameter grid (§5.5); 14 Spillover convergence (§6.7);
15 session-capture start (§10.2 Q27); 16 mode switches retried per block (§6.1).

## 10. Risks and open questions

### 10.1 Risks

1. **Silicon may disagree with the emulator** (errata, cache/DMA, the audio interrupt's FPU
   state; profile §8.2): hardware-in-the-loop PARITY on release firmware (phase D).
2. **M7 CPU:** contraction off, the flush and in-tree math at grain birth (≈3 % at one birth per
   sample, 48,000/s) put the profile's pessimistic total at about 77–78 % of the budget (profile
   §7.2, *estimated*); its levers (explicit FMA, Init-built tables, ITCM placement) are profile
   decisions. Explicit FMA would make the x86 app need FMA3 or a software path ~43 % slower on
   one preset (*measured* [prototype]).
3. **Sound-revision churn** after release: field presets show "Not identical" until the pedal
   updates; land known sound changes before revision 1 is published (§8.1).
4. **The parameter-ID one-way door** (§5.7).
5. **The GUI is unsized** and probably the largest item.
6. **JUCE and `clap-juce-extensions` churn:** pin and upgrade together.
7. **Expectation management:** users will null an analog re-amp against a render; the labels
   (§7.6) say what is not covered.
8. **Thin onset and mark coverage** in test vectors, which is how the block-split bug survived;
   coverage counters (profile §6.1).
9. **Windows MIDI:** single-client Windows 10, DAWs opening every port, varying port names, the
   Windows 11 SysEx corruption (§7.1).
10. **SD reliability:** stalls, power loss on consumer cards, FAT corruption in disk mode, exFAT,
    stray root `.bin` files; mitigated (§7.5, §7.6), not eliminated.
11. **Exact-load mute time** (≈45–160 ms, *estimated*) affects only how an Exact switch feels.
12. **The block-split fix changes sound and freeze behaviour**; listen before revision 1 is
    published.
13. **The ST USB link** sits in every firmware until the libDaisy patch lands (§7.2).
14. **Shipping friction:** unsigned macOS plugins fail confusingly; notarization costs $99/yr;
    Windows signing eligibility is unconfirmed.

> **Update (2026-10-07, Rev7 silicon record).** From the owner's Daisy Seed Rev7
> ([reviews/rev7-silicon-record.md](reviews/rev7-silicon-record.md)). Risk 1: the parity image
> rendered the golden corpus bit for bit at sound revisions 1 and 3, at `maxBlockSize` 512 and
> 48 and from a hostile caller (silicon record §2); the audio interrupt's FPU state is still
> untested on silicon. Risk 2 is measured, and came true: with the engine's code in ITCM and warm
> caches, the nominal row's worst block takes 99.1 % of the budget (100.3 % cold) and the
> pessimistic rows' 135.5–168.5 % (136.2–169.0 % cold), against the 77–78 % estimated above; the
> flush costs 0.9–1.8 %, and contraction off is inside every figure. Explicit FMA is a candidate
> under profile §7.3's rule, to build and measure before adopting, and ITCM placement is
> confirmed (silicon record §3.2–§3.5, §3.9). A fix for the budget is under design, with owner
> decisions pending. Risk 11's mute is measured at 47.2 ms (silicon record §3.6). *(2026-10-08:
> the fix is [cpu-budget.md](cpu-budget.md), owner-approved, with its decisions D1–D13; its steps
> 1–2, bit-exact, are built, and its D10 defers explicit FMA.)*

### 10.2 Open questions

> **Update (2026-10-07, Rev7 silicon record).** Q13's DWT pass ran on the owner's Daisy Seed Rev7,
> not a Seed3 ([reviews/rev7-silicon-record.md](reviews/rev7-silicon-record.md) §3–§4): profile
> §8.3 Q1 is settled (no subnormal penalty at FZ = 0; gradual underflow stays) and Q5 answered
> (the engine's bit-test flush, 0.9–1.8 % of the budget), `Restart` takes 47.18 ms, and the
> parity image rendered at 2.66× realtime at sound revision 1 and 2.57× at revision 3 (silicon
> record §2). Q2 and the watermark remain: there is no contraction-on or explicit-FMA build, and
> the watermark is not built. Q20 no longer waits on the `Restart` measurement, only on §4.9's
> watermark and DMA2D alternatives.

**Legal review** (licence-text readings, not legal advice):

1. GPLv3 + AGPLv3 desktop binaries, and the reading of JUCE EULA clause 2.3.
2. SLA0044: confirm GPLv3 firmware cannot link libDaisy's USB or SD glue, and what Corresponding
   Source must and must not redistribute, since libDaisy vendors ST's USB libraries.
3. Provenance of `src/util/bsp_sd_diskio.c` (a CubeMX template) and the `SdmmcHandler` licence.
4. Shipping a rebuilt `DaisyBootloader` (MIT, with ST USB code) with Brainscape's VID/PID and a
   boot-attempt counter (§7.6).
5. The contributor licence rule (§3.5).
6. The VST naming rule: is "Brainscape for VST" allowed? Check it against the guidelines PDF in
   the VST SDK.
7. SignPath eligibility.

**Hardware facts:**

8. Does the enclosure expose a microSD slot, with a PVD input and bulk capacitance (§7.5)? The
   slot store (§7.5), the primary firmware update (§7.6) and sneakernet (§7.7) all assume it.
   The control-surface research (`docs/research/pedal-control-surface-and-io-hardware.md`)
   lists no SD slot, while the grain-engine design assumes SD storage (`grain-engine.md` §7).
9. Which USB port is exposed, onboard USB-C (OTG_FS) or D29/D30 (High-Speed core)? It fixes the
   TinyUSB configuration, patch versus shim, and the bootloader variant (§7.2).
10. Seed3 USB bring-up against §7.2's rules.
11. Seed3 board detection takes the 24-bit, postgain-1 path (§4.8).
12. STM32H750 errata (ST ES0392) for FP divide and square root (profile §6.6).

> **Update (2026-10-08, Rev7 prototype).** The owner prototypes on a Daisy Seed Rev7 (STM32H750,
> PCM3060 codec, micro-USB) while the Seed3 is out of stock, and keeps a custom STM32H750 core
> board as a later option ([hardware-supply-2026-10.md](../research/hardware-supply-2026-10.md)),
> so Q10 and Q11 are asked of the Rev7 first and again of whichever module ships. Q10: the
> Rev7's onboard port is micro-USB on OTG_FS, as the Seed3's USB-C is, so §7.2's rules stand.
> The bring-up images use libDaisy's CDC device (bench only, not distributable) with the OTG_FS
> interrupts below the audio DMA, one of those rules, and the Rev7 has shown USB enumeration and
> whole-corpus streams without a lost byte ([firmware/README.md](../../firmware/README.md) §9);
> TinyUSB and §7.1's composite device are not built, so Q10 stays open. Q11: libDaisy detects
> the Rev7 by its PD5 strap and configures its PCM3060 for 24 bits with `postgain` 1 (firmware
> README §7), and every hello on the board reports that board and 24-bit audio
> ([reviews/rev7-silicon-record.md](reviews/rev7-silicon-record.md) §1.1); samples through the
> codec wait for the live image, and a Seed3 (TAC5242, PH6 strap) or a custom board needs the
> check again.

**Measurements:**

13. DWT on a Seed3: the profile's measurements (profile §8.3 Q1, Q2, Q5), plus `Restart` and the
    watermark, and PARITY speed.
14. Resampler latency and quality for the chosen r8brain configuration (§4.2).
15. Ubuntu's `qemu-user` 8.2.2 for M-profile (profile §6.2).
16. USB-MIDI matrix: Windows 10 and 11 port naming; Windows MIDI Services before and after its
    SysEx fix, through JUCE; the macOS UMP path's largest SysEx; uploads under cable-0 clock.
17. JUCE 9.0.3: VST3 numeric parameter IDs and pinning them; AU and LV2 automation timing;
    whether hosts accept `setLatencySamples` changes mid-session.

**Product decisions:**

18. Parameter-ID reconciliation and the Freeze ID (§5.7).
19. Freeze semantics for marks: pin-eligible (recommended) or live-head, by a listening test
    (§4.11; profile §8.3 Q3).
20. The pedal's default load mode (Spillover recommended) once `Restart` or the watermark is
    measured; whether "Restart on transport start" defaults on; whether the plugin offers the
    pedal grid until the block-split fix lands or claims no DAW identity until then (§2.3,
    §6.7). Promising identity on raw host buffers for the Live-positioning presets that did not
    diverge would first need profile §2.1, §2.4 and §2.5 to cover them.
21. Slot count, banks and the program-change mapping (§7.5).
22. Cache one bank or all 128 slots (~2 MiB, *estimated*)?
23. Confirmation UX without a screen, and the session unlock (§7.3).
24. Are loops part of a cross-device preset, and do they travel by SD, disk mode or a later bulk
    interface (never SysEx)?
25. Embed older engine revisions in the app, or only prompt a firmware update (recommended)?
26. A host-rate dry mix in resampled mode (§4.2), and the design's wet-latency report.
27. **SD session capture** (post-v1): the pedal logs engine input and stamped events from
    power-on, with no `Restart` (`Init` already leaves the counter at 0 and every buffer zero),
    and the app replays the session bit-exactly, Spillover loads included; the only way to extend
    1:1 to live playing (profile §6.7).
28. Release toolchain: the profile pins Arm GNU Toolchain 10.3-2021.10 and revisits it at
    bring-up (profile §6.8); §7.9's procedures name whatever is pinned.
29. Does the first public release wait for the looper (the plan's default, §8.1), or ship
    without it and add it later as a revision bump? Profile §8.3 Q11 asks the same of the other
    candidate sound changes.

## 11. Related documents

[grain-engine.md](grain-engine.md), [determinism-profile.md](determinism-profile.md),
[STATUS.md](../STATUS.md) and the evidence record
([reviews/companion-app-record.md](reviews/companion-app-record.md)). The passages this design
and the profile made stale (profile §1.3) were amended on 2026-10-05, together with those this
design adds: README.md's contributor licence note (§3.5), grain-engine.md §5's per-block mode
publish (§6.1), and the preset and plugin research notes
(`docs/research/preset-parameter-and-patch-format.md`: QSPI storage and JSON parsing;
`docs/research/vst-and-shared-dsp.md`: framework), each of which now points here.
STATUS.md's next steps follow §8.1.
