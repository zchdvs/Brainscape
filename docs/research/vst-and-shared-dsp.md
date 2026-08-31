# Shared DSP Core & Desktop Plugin Strategy for Brainscape

How to write one platform-agnostic C++ DSP core that runs unmodified on a Daisy Seed (Cortex-M7, bare metal) and inside a desktop VST3/CLAP/AU plugin, which plugin framework to build the desktop side on, and how to lay out the polyglot monorepo and CI so both targets build from the same `dsp/`.

## Summary

- **VST3 licensing changed fundamentally on Oct 29, 2025.** Steinberg relicensed the VST 3.8 SDK from the old dual (proprietary/GPLv3) model to plain **MIT**, confirmed directly in the SDK repo's README ("VST 3 SDK is under MIT license... can be used, modified, and redistributed freely — including in commercial products"). ASIO moved to GPLv3 (dual-licensable) at the same time. This removes the single biggest historical reason projects reached for JUCE's GPL escape hatch or avoided the official SDK. [steinbergmedia/vst3sdk](https://github.com/steinbergmedia/vst3sdk), [Steinberg press release](https://ocl-steinberg-live.steinberg.net/_storage/asset/819253/storage/master/Press%20Release%20-%202025-10-29%20-%20VST%203.8%20-%20EN.pdf), [VST3 dev portal licensing FAQ](https://steinbergmedia.github.io/vst3_dev_portal/pages/FAQ/Licensing.html).
- **JUCE itself is still dual-licensed**, and separately from Steinberg's change: core low-level modules (`juce_core`, `juce_events`, `juce_audio_basics`, `juce_audio_devices`) are ISC, but the bulk of the framework (GUI, plugin-client wrappers, `juce_dsp`, etc.) is **AGPLv3-or-commercial**. Commercial tiers: Personal (free, revenue < $50k), Indie (~$40/mo or $800 perpetual, revenue < $500k), Pro (~$130/mo or $2600 perpetual, no cap). For a fully open-source project this is a non-issue — GPLv3 and AGPLv3 have explicit mutual-linking permission written into section 13 of both licenses, so a GPLv3-licensed Brainscape can link AGPLv3 JUCE modules; the combined work is just governed by both licenses' terms. [JUCE LICENSE.md](https://github.com/juce-framework/JUCE/blob/master/LICENSE.md), [GNU GPL FAQ on GPLv3/AGPLv3 compatibility](https://www.gnu.org/licenses/gpl-faq.en.html).
- **iPlug2** uses a permissive zlib/WDL-style license end to end, with no revenue-tier commercial licensing ever. It targets VST2/VST3/AUv2/AUv3/AAX/CLAP/WAM from one codebase. Real precedent for exactly Brainscape's genre: **Neural Amp Modeler** (the most popular open-source guitar-effects plugin project) is built on iPlug2. [iPlug2 GitHub](https://github.com/iPlug2/iPlug2), [NeuralAmpModelerPlugin](https://github.com/sdatkinson/NeuralAmpModelerPlugin).
- **DPF (DISTRHO Plugin Framework)** is ISC-licensed and deliberately avoids depending on Steinberg's official VST3 SDK — it ships its own clean-room "travesty" VST3 implementation so it stays license-clean regardless of Steinberg's terms (this predates the MIT relicense and still stands as a design choice). It's lightweight, has first-class LV2 support (matters for the Linux/open-hardware crowd Brainscape is likely to attract), and is the framework behind **Cardinal**, a real shipped product that wraps a whole hardware-derived modular-synth DSP engine as a VST3/CLAP/AU/LV2 plugin. [DPF LICENSING.md](https://github.com/DISTRHO/DPF/blob/main/LICENSING.md), [DISTRHO/Cardinal](https://github.com/DISTRHO/Cardinal).
- **CLAP** (MIT-licensed spec, championed by Bitwig + u-he) is the cleanest target for a from-scratch plugin: simple C ABI, zero SDK license friction. **clap-wrapper** (MIT for the VST3 half, Apache-2.0 for AU, GPL3/commercial for AAX) projects one CLAP plugin into VST3/AU/AAX/standalone automatically. As of the article checked, native CLAP hosts are FL Studio, REAPER, and Bitwig — real but still a minority of DAWs, so CLAP should be additive, not a VST3 replacement, for reach. [clap-wrapper README](https://github.com/free-audio/clap-wrapper/blob/main/README.md), [CLAP vs VST3 (2026) — Spectral Colors](https://spectral-colors.com/news/clap-vs-vst3-2026/).
- **Real precedent for exactly Brainscape's shape (hardware DSP shared into a desktop plugin) is abundant.** Mutable Instruments open-sources all Eurorack firmware (STM32F code MIT, AVR code GPLv3), and VCV Rack's official **Audible Instruments** plugin (GPL-3.0-or-later) ports that DSP — including Clouds' granular engine — into a software rack. **Cardinal** then wraps VCV Rack itself (GPLv3+, requiring every included module to be GPLv3-or-later compatible) as a standalone VST3/CLAP/AU/LV2 plugin — a two-hop hardware→software-rack→plugin lineage. [pichenettes/eurorack](https://github.com/pichenettes/eurorack), [VCVRack/AudibleInstruments](https://github.com/VCVRack/AudibleInstruments), [Cardinal LICENSES.md](https://github.com/DISTRHO/Cardinal/blob/main/docs/LICENSES.md).
- **Rebel Technology's OWL platform is the closest analog of all — it's literally a programmable guitar/effects pedal.** Patch DSP code (C++, Faust, Pure Data, Max Gen, SOUL...) is written once and runs unmodified either on OWL hardware or, via **OwlSim** (a JUCE-based simulator), as a VST/AU plugin — validating "write the DSP once, ship a pedal and a plugin" for a pedal specifically, not just a synth module. [OwlProgram/OwlSim](https://github.com/RebelTechnology/OwlProgram), [KVR OwlSim listing](https://www.kvraudio.com/product/owlsim-by-rebel-technology).
- **Daisy-specific precedent exists too, at smaller scale.** `electro-smith/DaisySP` — the DSP *algorithms* library (delays, filters, oscillators, granular tools) — is confirmed platform-agnostic: its `CMakeLists.txt` builds a plain static library with no `arm-none-eabi` requirement and no hardware dependency (the hardware/board-support layer lives in the separate `libDaisy`, not DaisySP). The community has already run DaisySP inside a JUCE plugin (`dylan-robins/DaisyPedalTestPlugin`) as a build-and-listen sandbox before flashing hardware. This DaisySP/libDaisy split is the exact architectural precedent Brainscape's `dsp/`/`firmware/` split should follow. [electro-smith/DaisySP](https://github.com/electro-smith/DaisySP), [DaisyPedalTestPlugin](https://github.com/dylan-robins/DaisyPedalTestPlugin).
- **Surge XT demonstrates the end state of a clean core/wrapper split at full-instrument scale.** `src/common` holds the entire synthesis engine and is buildable without pulling in all of JUCE; a headless `surge-testrunner` flavor runs the DSP with no GUI or plugin wrapper at all. [Surge Architecture.md](https://github.com/surge-synthesizer/surge/blob/main/doc/Surge%20Architecture.md).
- **Faust (`faust2daisy` / `faust2juce`) is a real write-once option but a tradeoff, not a free lunch.** The same `.dsp` source cross-compiles to Daisy-targeted C++ (with SRAM/QSPI/SDRAM memory-placement flags, MIDI, polyphony) or to a full JUCE plugin/standalone project. But it's a genuinely different language to learn, generated C++ isn't meant for hand-editing, and fine control over allocation/scheduling on embedded is more indirect than hand-written C++ — which is likely why the more ambitious hand-written projects surveyed here (DaisyPedalTestPlugin, OwlProgram/OWL patches, Surge, ChowDSP) mostly skip Faust for the core engine and hand-write C++ instead. [Faust tools manual](https://faustdoc.grame.fr/manual/tools/), [faust2daisy manpage](https://manpages.debian.org/testing/faust/faust2daisy.1.en.html).
- **libDaisy's CMake toolchain file is the template for the arm-none-eabi side of the build**: Cortex-M7 flags (`-mcpu=cortex-m7 -mfpu=fpv5-d16 -mfloat-abi=hard -mthumb`), exceptions and RTTI disabled (`-fno-exceptions`, `-fno-rtti`), `ARM_MATH_CM7` defined for CMSIS-DSP. A shared `dsp/` core must compile clean under these flags *and* under a normal desktop compiler with exceptions/RTTI on. [libDaisy stm32h750xx.cmake](https://github.com/electro-smith/libDaisy/blob/master/cmake/toolchains/stm32h750xx.cmake).
- **Denormal handling is a genuine two-platform problem, not a detail to hand-wave.** On x86/x64, flush denormals via the MXCSR register (`_MM_SET_FLUSH_ZERO_MODE`/`_MM_SET_DENORMALS_ARE_ZERO_MODE`, or JUCE's `ScopedNoDenormals` RAII wrapper) once per audio callback. On Cortex-M7 the FPU has its own, unrelated flush-to-zero control bit in `FPSCR` (the `FZ` bit) — a shared core cannot assume it's already on; different startup/runtime configurations set it differently, so Brainscape needs an explicit, tested "denormal guard" abstraction with a real implementation on each platform rather than assuming one mechanism covers both. [ARM FPSCR flush-to-zero docs](https://developer.arm.com/documentation/ddi0403/d/Application-Level-Architecture/Application-Level-Programmers--Model/The-optional-Floating-point-extension/Floating-point-data-types-and-arithmetic), [JUCE denormal-prevention thread](https://forum.juce.com/t/state-of-the-art-denormal-prevention/16802).
- **Daisy's SDRAM buffers are declared with a linker attribute, not `malloc`'d**: `DSY_SDRAM_BSS` places a *statically-sized* global object in external SDRAM (e.g. `DelayLine<float, MAX_DELAY> DSY_SDRAM_BSS delay;`), and because SDRAM isn't initialized when C++ global constructors would normally run, SDRAM-resident objects can't rely on non-trivial constructors. On desktop this constraint disappears entirely — you `new`/`malloc` at will, with far larger buffers than 64MB SDRAM allows. The `dsp/` core needs a buffer-provisioning abstraction (buffers passed in / sized at `Init()`) rather than hard-coding either pattern into the algorithm code. [libDaisy External SDRAM guide](https://github.com/electro-smith/libDaisy/blob/master/doc/md/_a6_Getting-Started-External-SDRAM.md), [Electrosmith memory explainer](https://electro-smith.com/pages/memory-what-is-the-difference).
- **Offline rendering + unit tests on host is the standard fast-iteration pattern in the plugin community**, independent of embedded work: a Catch2-based test target (e.g. the `pamplejuce` JUCE/CMake/Catch2/GitHub-Actions template) renders algorithms to buffers or WAV files and asserts on them, so DSP changes can be validated in seconds on a dev machine instead of a flash-and-listen loop on hardware. [sudara/pamplejuce](https://github.com/sudara/pamplejuce), [Melatonin: when to write tests for DSP code](https://melatonin.dev/blog/when-to-write-tests-for-dsp-code/).

## Plugin Framework Options

### JUCE

**License.** Dual: a handful of low-level modules (`juce_core`, `juce_events`, `juce_audio_basics`, `juce_audio_devices`) are ISC; everything else (GUI, `juce_audio_processors`/plugin-client wrappers, `juce_dsp`, etc.) is AGPLv3-or-commercial. [JUCE LICENSE.md](https://github.com/juce-framework/JUCE/blob/master/LICENSE.md). Commercial tiers exist (Personal free under $50k revenue, Indie ≈$800/yr under $500k, Pro ≈$2600/yr uncapped) but only matter if Brainscape ever wants a closed-source build; for a project that intends to be GPL-licensed and open source throughout, AGPLv3 is simply the license, not a cost center. [JUCE forum: open source development with JUCE](https://forum.juce.com/t/open-source-development-with-juce/29579).

**CLAP support is not native** — it requires the separate `free-audio/clap-juce-extensions` (MIT-licensed) project layered onto a JUCE 6/7 plugin target, plus JUCE's own VST3/AU wrappers for those formats natively. [clap-juce-extensions README](https://github.com/free-audio/clap-juce-extensions/blob/main/README.md).

**Precedent directly relevant to Brainscape:**
- `dylan-robins/DaisyPedalTestPlugin` — a JUCE VST plugin that pulls in DaisySP as a git submodule and treats it "identically to how it's used on the Daisy Seed," i.e. exactly the dsp-core-inside-a-JUCE-wrapper pattern Brainscape needs, already validated by a community member. [repo](https://github.com/dylan-robins/DaisyPedalTestPlugin).
- Rebel Technology's **OwlSim** — JUCE-based simulator that runs the *exact same* OWL pedal patch code as a VST/AU. [OwlProgram](https://github.com/RebelTechnology/OwlProgram).
- **ChowDSP** — `chowdsp_utils` is a JUCE module of DSP/plugin-development utilities; **ChowCentaur** is an open-source JUCE plugin emulating the Klon Centaur guitar pedal specifically. [chowdsp_utils](https://github.com/Chowdhury-DSP/chowdsp_utils), [Chowdhury-DSP org](https://github.com/Chowdhury-DSP).
- **GuitarML/SmartGuitarPedal** — another JUCE-based open-source guitar pedal emulation plugin (neural-network amp/pedal modeling). [repo](https://github.com/GuitarML/SmartGuitarPedal).
- **Surge XT** — proves JUCE can host a cleanly separated DSP core (`src/common`, buildable without full JUCE) at full-synth scale, with a headless test-runner flavor. [Surge Architecture.md](https://github.com/surge-synthesizer/surge/blob/main/doc/Surge%20Architecture.md).

**Pros:** by far the largest ecosystem, best-documented CMake API (`juce_add_plugin`, targets per format), most mature GUI/component toolkit, most tutorials, most prior art specifically for pedal-style guitar plugins, works well with `pamplejuce`-style Catch2+CI templates out of the box. **Cons:** heaviest framework to pull in for a project whose desktop half is conceptually "just wrap a small DSP core"; AGPLv3 needs a `CONTRIBUTING.md` note so casual contributors understand license implications if they ever fork privately; CLAP requires a bolt-on extension, not native.

### iPlug2

**License.** Permissive, zlib/WDL-style, free of any revenue-tier commercial licensing. [iPlug2 GitHub](https://github.com/iPlug2/iPlug2).

**Targets:** VST2, VST3, AUv2, AUv3, AAX (Native), CLAP, and Web Audio Module (WAM) from one codebase — CLAP is native here, unlike JUCE. [iPlug2 site](https://iplug2.github.io/).

**Precedent:** **Neural Amp Modeler** (`sdatkinson/NeuralAmpModelerPlugin`) — the most widely used open-source guitar-effects plugin in the current ecosystem — integrates its core DSP with iPlug2 to produce VST3/AU plugins and a standalone app; the maintainers explicitly refactored toward "iPlug2-recommended practices," implying a healthy, opinionated framework/community relationship. [NeuralAmpModelerPlugin](https://github.com/sdatkinson/NeuralAmpModelerPlugin), [neuralampmodeler.com/the-code](https://www.neuralampmodeler.com/the-code).

**Pros:** permissive license end to end (no future licensing decision to revisit), native CLAP, IGraphics GUI layer is lighter than JUCE's but purpose-built for plugins (not general app dev), proven at scale for a guitar-plugin project the exact genre of Brainscape. **Cons:** smaller ecosystem/community than JUCE, fewer tutorials, GUI toolkit less full-featured than JUCE's Component/LookAndFeel system for complex custom UIs.

### DPF (DISTRHO Plugin Framework)

**License.** ISC for the framework itself. Per-format licensing inside DPF: VST2 wrapper is BSD-3 (via a clean-room reverse-engineered header, not Steinberg's), **VST3 wrapper is ISC** (DPF's own "travesty" implementation, deliberately not depending on Steinberg's SDK), LV2 is ISC (with attribution to the LV2 authors), **CLAP wrapper is MIT**, AU is ISC, JACK/standalone (via RtAudio/RtMidi) is MIT. [DPF LICENSING.md](https://github.com/DISTRHO/DPF/blob/main/LICENSING.md).

**Build system:** both CMake and Makefile workflows are first-class — DISTRHO publishes both a `dpf-cmake-action` and a `dpf-makefile-action` GitHub Action for CI. [dpf-cmake-action](https://github.com/DISTRHO/dpf-cmake-action), [dpf-makefile-action](https://github.com/DISTRHO/dpf-makefile-action).

**Precedent:** **Cardinal** (`DISTRHO/Cardinal`) — a DPF-based plugin wrapper that pulls in VCV Rack's engine *directly* (not a fork) to ship a self-contained VST3/CLAP/AU/LV2/standalone modular synth. This is the strongest real-world proof that "small, license-clean plugin framework + a DSP engine that was designed for something else entirely (Eurorack hardware simulation)" works in production, with real users. Cardinal itself is GPLv3+ (a project-level choice, not a DPF requirement — DPF's own license is ISC). [DISTRHO/Cardinal](https://github.com/DISTRHO/Cardinal), [Cardinal LICENSES.md](https://github.com/DISTRHO/Cardinal/blob/main/docs/LICENSES.md).

**Pros:** smallest, most license-clean footprint of the three frameworks (ISC everywhere, no dependency on Steinberg's SDK even now that it's MIT, no revenue tiers ever); native LV2 matters for the Linux/open-hardware audience adjacent to a DIY guitar pedal project; proven in production via Cardinal at real scale. **Cons:** GUI is lower-level (NanoVG/DGL immediate-mode-ish drawing) — more manual work than JUCE's or iPlug2's component systems for a polished custom pedal UI; smaller community/fewer tutorials than JUCE.

### CLAP + clap-wrapper

**CLAP itself** is a C ABI plugin format specified and MIT-licensed, championed by Bitwig and u-he as a modern, cleaner alternative to VST3's C++ ABI and COM-style interfaces.

**clap-wrapper** (`free-audio/clap-wrapper`) projects a single CLAP plugin binary into VST3, AUv2, AUv3 (macOS/iOS), AAX, and a "Simple Standalone" host wrapper. Its own licensing is mixed by target: the **VST3 wrapper is MIT**, the **AUv2 wrapper is Apache-2.0**, and the **AAX wrapper is GPL3-or-commercial** (AAX being Avid's own closed ecosystem). Using clap-wrapper to produce a VST3 binary requires either the VST3 SDK's own terms (now MIT, so this is now frictionless) or, historically, its GPL3 fallback. [clap-wrapper README](https://github.com/free-audio/clap-wrapper/blob/main/README.md).

**DAW adoption (2026):** native CLAP hosts include FL Studio, REAPER, and Bitwig Studio (Bitwig will prefer a plugin's CLAP version over its VST when both are installed); VST3 remains the format with the broadest host/product coverage by a wide margin. The practical read: CLAP is real and growing but is not yet a substitute for VST3 reach — it's a "ship both" situation for the foreseeable future, not an either/or. [CLAP vs VST3 (2026) — Spectral Colors](https://spectral-colors.com/news/clap-vs-vst3-2026/).

**Fit for Brainscape:** whichever host framework is chosen (iPlug2 native, DPF native, or JUCE + clap-juce-extensions), CLAP should be a target format, not the only one — VST3 is still required for reach into Ableton, Logic (via AU instead), Cubase, Studio One, etc.

### Steinberg VST3 SDK (direct, no framework)

Now MIT-licensed (confirmed directly in the [steinbergmedia/vst3sdk](https://github.com/steinbergmedia/vst3sdk) README: "VST 3 SDK is under MIT license... can be used, modified, and redistributed freely — including in commercial products"), CMake-based, and supports Windows/macOS/iOS/Linux out of the box. Going direct against the raw SDK with no framework at all is a defensible choice for a project shipping *only* VST3 with a minimal or no GUI, but Brainscape needs at least VST3 + likely CLAP/AU, plus a real GUI for granular-delay controls (grain size, density, pitch, filter, mix, feedback) — hand-rolling that against three separate native SDKs is a large, unnecessary maintenance burden compared to any of the three frameworks above, all of which already wrap VST3 (and more) behind one API. **Not recommended as the primary approach**, but worth knowing the SDK itself is no longer a licensing obstacle if a future contributor wants a minimal, framework-free build for a specific host.

### Framework comparison table

| | JUCE | iPlug2 | DPF |
|---|---|---|---|
| Core framework license | AGPLv3 or commercial | permissive (zlib/WDL) | ISC |
| Revenue-tier commercial license ever required | only for closed-source builds | never | never |
| Native formats | VST3, AU, AAX, LV2 (community), standalone | VST2/3, AUv2/3, AAX, CLAP, WAM, standalone | VST2/3, LV2, CLAP, AU, JACK/standalone |
| CLAP support | via `clap-juce-extensions` (MIT, bolt-on) | native | native |
| GUI toolkit | full-featured Component/LookAndFeel system | IGraphics (plugin-purpose-built) | DGL/NanoVG (lower-level, more manual) |
| Guitar-pedal-plugin precedent | ChowCentaur, SmartGuitarPedal, DaisyPedalTestPlugin, OwlSim | Neural Amp Modeler | (Cardinal is a synth, not a pedal, but proves the "wrap someone else's DSP engine" pattern) |
| Ecosystem/tutorials | largest by far | medium | smaller, Linux-audio-community-centric |
| Build system | CMake (`juce_add_plugin`) | Makefile-based (CMake support less central) | CMake and Makefile both first-class |

## Platform-Agnostic Real-Time C++ DSP Core Patterns

### No heap allocation in the audio path

The consistent rule across every real-time audio source checked: **all memory must be allocated up front** (at `Init()`/`prepareToPlay()`-equivalent time, sized for a known maximum sample rate and block size), never inside the per-block `Process()` call. `std::vector`/`std::string`/`std::function` and friends are fine to use for *setup-time* bookkeeping, but any container whose growth could trigger `malloc` inside the hot path is disallowed — `push_back` on a vector that might need to reallocate is a classic, easy-to-miss violation. Preallocate-and-reuse ("allocate once, reuse always") is the standard pattern; cross-thread parameter updates should use atomics or lock-free double-buffering rather than mutexes, since a lock can also stall the audio thread unpredictably (priority inversion). [Ross Bencina-style RT rules as summarized via multiple sources: WolfSound C++ pointers for audio](https://thewolfsound.com/c-plus-plus-pointers-explained-for-audio-programming/), [timur.audio: using locks in real-time audio safely](https://timur.audio/using-locks-in-real-time-audio-processing-safely), [LMMS Realtime Conventions wiki](https://github.com/LMMS/lmms/wiki/Realtime-Conventions).

This constraint is identical on Cortex-M7 firmware and on a desktop plugin — the desktop side is not exempt just because `malloc` "works": a page fault or heap-lock contention inside `processBlock` on a busy DAW causes exactly the same audible glitch as a missed DMA deadline on hardware. Brainscape's `dsp/` core should therefore be written to the embedded constraint everywhere, even though the desktop build could technically get away with more.

### Abstracting sample rate / block size

Neither should ever be a compile-time constant baked into the DSP core. The idiomatic pattern (seen across DaisySP-based code, JUCE's `AudioProcessor::prepareToPlay(sampleRate, samplesPerBlock)`, and Faust's runtime `init(int samplingFreq)`) is:

```cpp
class Engine {
public:
    void Init(float sampleRate, size_t maxBlockSize); // called once, allowed to allocate
    void Process(const float* const* in, float* const* out, size_t numFrames); // called every block, must not allocate
};
```

`maxBlockSize` at `Init()` time lets the core size any internal scratch buffers once; `numFrames` per `Process()` call can then vary block-to-block (as it does in most DAWs) as long as it never exceeds `maxBlockSize`. Firmware fixes both values once at boot (Daisy's default block size is commonly 48 or 4 samples depending on config; sample rate 48kHz); the plugin wrapper must re-call `Init()` whenever the host changes sample rate or reports a new maximum block size.

### STL pitfalls on embedded

libDaisy's own toolchain file disables exceptions (`-fno-exceptions`) and RTTI (`-fno-rtti`) for the Cortex-M7 build. Practical consequences for a shared core:
- No `dynamic_cast`, no `typeid`, no exception-based error handling (`try`/`catch`/`throw` must not appear in `dsp/` if it's expected to compile under `-fno-exceptions` — some STL headers still compile fine, but anything that can *throw* internally, like `std::vector::at()` bounds checks or `std::string` allocation failure, becomes undefined behavior on failure paths rather than a caught exception).
- Embedded C++ toolchains commonly link against "newlib-nano," a reduced C library where `iostream`, locale-heavy code, and some `printf` float-formatting paths are unavailable or bloat flash badly — avoid `std::cout`/`std::stringstream` in the shared core; use it only in test/host-only code.
- `std::vector`, `std::map`, `std::string`, and `std::function` are usable for setup-time state but must never be touched in `Process()` for the heap-allocation reasons above; when a resizable-looking container is needed in the hot path, prefer a fixed-capacity alternative (a `std::array`-backed ring buffer, an inline fixed-size arena, or a small custom `StaticVector<T, N>`).
- General embedded C++ guidance converges on: disable exceptions/RTTI explicitly, avoid dynamic memory after startup, prefer static memory pools, and lean on RAII/strong typing/compile-time programming instead of runtime polymorphism where possible. [Bit Bashing: C++ on Embedded Systems](https://bitbashing.io/embedded-cpp.html), [libDaisy stm32h750xx.cmake](https://github.com/electro-smith/libDaisy/blob/master/cmake/toolchains/stm32h750xx.cmake).

### Fixed-size buffers vs SDRAM-sized buffers on desktop

Daisy Seed pairs a small amount of fast internal SRAM with 64MB of external SDRAM, accessed via a linker-section attribute: `DelayLine<float, MAX_DELAY> DSY_SDRAM_BSS delay;` places a statically-sized object in SDRAM at link time. Because SDRAM initialization happens after C++ global-constructor time, SDRAM-resident globals effectively can't rely on non-trivial constructors running correctly — they're typically zero/garbage-initialized data that gets explicitly `Init()`'d later at runtime, not full C++ objects constructed normally. [libDaisy External SDRAM guide](https://github.com/electro-smith/libDaisy/blob/master/doc/md/_a6_Getting-Started-External-SDRAM.md), [Electrosmith memory explainer](https://electro-smith.com/pages/memory-what-is-the-difference).

On desktop, none of this applies: a plugin can `new[]` or `malloc()` a buffer of essentially any size at `Init()` time, with no SDRAM-vs-SRAM distinction and no linker-section dance. The shared `dsp/` core should therefore never hard-code `DSY_SDRAM_BSS` or a fixed array size directly into an algorithm; instead, buffer *capacity* should be a template parameter or constructor/`Init()` argument, and the *placement* of that memory (global BSS-in-SDRAM on firmware vs. heap-allocated on desktop) should live in a thin platform-specific allocation layer outside `dsp/` proper — e.g. `firmware/` declares `DSY_SDRAM_BSS` globals and hands pointers into the shared engine's `Init()`, while `plugin/` just `new`s the same-shaped buffers. This is exactly the seam that keeps `dsp/` platform-agnostic while still letting firmware use Daisy's specific memory map and letting desktop use much larger buffers (e.g. a 10-minute looper is trivial on desktop RAM but must be carefully budgeted against 64MB SDRAM on hardware).

### Single-precision float everywhere

Across the DSP/plugin community consensus: use `float` (32-bit), not `double`, as the default sample type. Reasons converge on: half the memory traffic and cache footprint of `double`, meaningfully better SIMD throughput (more floats fit in a SIMD register/cache line than doubles), and precision that's already far beyond audible for audio signal ranges — float32's granularity in `[-1, 1]` is about `-144dBFS`, well below any real-world noise floor. The known exceptions where double genuinely helps are certain low-frequency/high-order recursive IIR filters (state-variable accumulation error) and very large FFTs — narrow, identifiable cases that can be special-cased inside specific algorithms rather than changing the core's default type. [KVR: DSP code — when to use floats or doubles](https://www.kvraudio.com/forum/viewtopic.php?t=628294). Practically for Brainscape: Cortex-M7's FPU is natively single-precision-optimized (double-precision ops are markedly slower on Cortex-M7), so float32 is doubly justified — it's both the DSP-community default *and* the faster choice on the actual target hardware.

### Denormal handling (x86 vs Cortex-M7)

Denormals (subnormal floats near zero, common in decaying reverb/delay tails and filter feedback loops) are handled by *completely different mechanisms* on the two platforms, and a shared core cannot assume either is on by default:

- **x86/x64:** flush-to-zero and denormals-are-zero are MXCSR register bits, set via `_mm_setcsr()`/`_mm_getcsr()` or the `_MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON)` / `_MM_SET_DENORMALS_ARE_ZERO_MODE` intrinsics (from `<pmmintrin.h>`, SSE3). JUCE packages this as a `ScopedNoDenormals` RAII guard applied at the top of every `processBlock`. Caveat raised on the JUCE forum: a plugin changing these flags can conflict with host expectations (some hosts doing offline/bounce processing may expect different FP behavior, and not every host resets the flags between plugins), and there's no absolute guarantee every compiler emits SSE for every float op — so the guard should be applied defensively per-callback, not assumed to be a global one-time setup. [JUCE denormal-prevention thread](https://forum.juce.com/t/state-of-the-art-denormal-prevention/16802).
- **Cortex-M7:** the FPU's flush-to-zero behavior is controlled by the `FZ` bit in `FPSCR`, an entirely separate register/mechanism from x86's MXCSR — setting it treats subnormal *inputs* as zero, and per ARM's own architecture docs this is specifically valuable for performance because unflushed denormal/underflow arithmetic on hardware that only natively handles normals and zeros falls back to slow software-assisted paths. [ARM flush-to-zero docs](https://developer.arm.com/documentation/ddi0403/d/Application-Level-Architecture/Application-Level-Programmers--Model/The-optional-Floating-point-extension/Floating-point-data-types-and-arithmetic), [ARM Cortex-M7 TRM](https://developer.arm.com/documentation/ddi0489/f/Chdbebfc).

Recommendation embedded in the design: `dsp/` should expose a `DenormalGuard` RAII type with two platform-specific implementations behind a common header (`#if defined(__x86_64__) || defined(_M_X64)` → MXCSR; `#if defined(__ARM_FP) && defined(__arm__)` → FPSCR), instantiated once at the top of every top-level `Process()` call in both firmware and plugin — and this must be *tested*, not assumed, since CMSIS/board startup code enabling `FZ` by default is a per-toolchain/per-startup-file detail, not an architectural guarantee.

## Real Projects That Share DSP Between Embedded and Desktop

### Mutable Instruments → VCV Rack (Audible Instruments)

Émilie Gillet (pichenettes) open-sources all Mutable Instruments Eurorack module firmware in a single repo, with STM32F-target code under **MIT** and AVR-target code under **GPLv3**. VCV Rack's official `VCVRack/AudibleInstruments` plugin (GPL-3.0-or-later) ports that DSP — Braids, Clouds, Elements, Grids, Marbles, Plaits, Rings, Stages, etc. — into a virtual modular rack running on desktop. This is the most directly analogous open-source precedent to Brainscape's "hardware pedal DSP, also as a desktop instrument" ambition, specifically including Clouds' granular texture engine, the closest sonic relative of a granular delay pedal. [pichenettes/eurorack](https://github.com/pichenettes/eurorack), [VCVRack/AudibleInstruments](https://github.com/VCVRack/AudibleInstruments), [VCV Audible Instruments listing](https://vcvrack.com/AudibleInstruments).

**Cardinal** (`DISTRHO/Cardinal`) takes this one step further: it wraps the entire VCV Rack engine (not a fork — the actual Rack code) inside DPF to ship a self-contained VST3/CLAP/AU/LV2/standalone plugin, GPLv3+ throughout (every included module must be GPLv3-or-later compatible; GPL-3.0-*only* modules are explicitly disallowed since they're incompatible with "or later"). This three-layer lineage — hardware firmware → software modular-rack port → plugin wrapper around the rack — is a good structural reference for how far a single DSP investment can be stretched across product forms. [DISTRHO/Cardinal](https://github.com/DISTRHO/Cardinal), [Cardinal LICENSES.md](https://github.com/DISTRHO/Cardinal/blob/main/docs/LICENSES.md).

### Rebel Technology OWL (hardware pedal ↔ VST/AU, same patch code)

The OWL is a real, shipped programmable guitar/effects pedal. Patches — C++, or via Faust/Pure Data/Max Gen/SOUL/Maximilian front-ends — are built against the `OwlProgram` SDK and can be linked either onto the physical OWL hardware or against **OwlSim**, a JUCE-based simulator that turns the identical patch into a VST/AU plugin runnable in any DAW, explicitly marketed for developing and debugging patches "in your favourite DAW/host before uploading to the pedal." This is the single closest real-world precedent to Brainscape's stated goal, because it's the same product category (a guitar effects pedal) solving the same problem (write once, run on hardware and as a plugin) with the same approach (a thin JUCE-based simulator shim around a hardware-agnostic patch API). [RebelTechnology/OwlProgram](https://github.com/RebelTechnology/OwlProgram), [KVR: OwlSim](https://www.kvraudio.com/product/owlsim-by-rebel-technology), [Music Hackspace: The OWL, a programmable guitar pedal](https://musichackspace.org/the-owl-a-programmable-guitar-pedal/).

### Daisy-specific desktop twins

- **`electro-smith/DaisySP`** is already the architectural precedent Brainscape should copy for its own `dsp/`: it's MIT-licensed, has no hardware dependency, and its `CMakeLists.txt` builds a plain host-buildable static library — the hardware-specific board-support code (ADC/DAC, audio callback, GPIO, SDRAM init) lives in the separate `libDaisy` package instead, not in DaisySP. This DaisySP (portable algorithms) vs. libDaisy (hardware I/O) split *is* the DSP-core-vs-firmware split Brainscape needs, already proven at the "official Electrosmith library" level of maturity. [electro-smith/DaisySP](https://github.com/electro-smith/DaisySP).
- **`dylan-robins/DaisyPedalTestPlugin`** — a community project that builds a JUCE VST plugin pulling in DaisySP as a git submodule "used for all DSP in the same way as when writing code for the Daisy Seed," explicitly for prototyping pedal ideas in a DAW before flashing hardware. Confirms the JUCE + DaisySP combination works in practice, though it's a small/individual project (uses a `.jucer`-based build with manually patched Visual Studio project files rather than a clean CMake setup — a rougher build-integration story than Brainscape should aim for). [repo](https://github.com/dylan-robins/DaisyPedalTestPlugin).

### ChowDSP and other guitar-pedal-adjacent consultancies

Jatin Chowdhury's ChowDSP builds JUCE-based plugins, several directly analogous to Brainscape's genre: **ChowCentaur** emulates the Klon Centaur guitar pedal; `chowdsp_utils` is a reusable JUCE module of DSP/plugin-dev utilities (including a real-time neural-network inferencing library used across ChowDSP's products); `chowdsp_wdf` is a header-only Wave Digital Filter circuit-modeling library. ChowDSP has also collaborated on **Multiverse**, a hardware customizable-effects pedal (Aviate Audio) with a companion desktop "Effects Shop" catalog — another commercial example of the hardware-pedal-plus-desktop-software-tooling pattern, though details of that specific code-sharing architecture were not independently verified beyond the product description (**unverified**: exact extent of shared DSP code between Multiverse firmware and its desktop tooling). [Chowdhury-DSP org](https://github.com/Chowdhury-DSP), [chowdsp_utils](https://github.com/Chowdhury-DSP/chowdsp_utils), [chowdsp.com](https://chowdsp.com/).

**GuitarML/SmartGuitarPedal** is a further JUCE-based open-source example specifically in the guitar-pedal-emulation space (neural amp/pedal modeling), reinforcing that JUCE is a well-trodden path for this exact plugin genre. [repo](https://github.com/GuitarML/SmartGuitarPedal).

### Surge XT — the clean core/wrapper split at full-instrument scale

Surge (`surge-synthesizer/surge`) keeps its entire synthesis engine — oscillators, filters, effects, voice management — in `src/common`, decoupled enough from JUCE that it's "buildable without building all of JUCE" by cherry-picking a few JUCE dependencies. Plugin-format wrappers (VST3, AU, LV2) live in `src/surge-xt`; a separate **`surge-testrunner`** target runs the DSP engine completely headless, with no GUI and no plugin wrapper at all, for fast automated testing. This is a good target end-state to reference when deciding how strictly to firewall `dsp/` away from any plugin-framework or hardware-framework type in Brainscape's own core. [Surge Architecture.md](https://github.com/surge-synthesizer/surge/blob/main/doc/Surge%20Architecture.md).

### Neural Amp Modeler — iPlug2, guitar-genre precedent

`sdatkinson/NeuralAmpModelerPlugin` is the plugin half of the broader Neural Amp Modeler project (guitar amp/pedal modeling via neural nets trained on real hardware recordings). Its core DSP integrates with iPlug2 to produce VST3/AU plugins and a standalone app; a separate training/export repo (`sdatkinson/neural-amp-modeler`) produces the `.nam` model files the plugin loads. This is the strongest same-genre precedent for choosing iPlug2 specifically, as opposed to JUCE. [NeuralAmpModelerPlugin](https://github.com/sdatkinson/NeuralAmpModelerPlugin), [neuralampmodeler.com](https://www.neuralampmodeler.com/).

### Airwindows / airwin2rack — consolidated-library pattern

Airwindows is a large (MIT-licensed) catalog of individually tiny, GUI-less DSP algorithms historically shipped as one-plugin-per-algorithm. `baconpaul/airwin2rack` consolidates the entire catalog into a single static library behind one uniform API (`AirwinRegistry.h`), then wraps that library once as a CLAP/VST3/AU/LV2/standalone plugin and once as a VCV Rack module. This is a useful pattern reference for "many small algorithms, one shared core, multiple thin wrappers," though it's a less direct precedent than the others since Airwindows' individual algorithms are simple stateless-ish effects rather than one large stateful granular engine (**unverified**: which specific plugin framework airwin2rack's wrapper layer uses internally). [baconpaul/airwin2rack](https://github.com/baconpaul/airwin2rack), [Airwindows](https://www.airwindows.com/).

### Faust (faust2daisy, faust2juce) as a write-once option

Faust is a functional, block-diagram-oriented DSP language that compiles to C++ (among other backends). Two relevant code generators:
- **`faust2daisy`** compiles a `.dsp` file directly to Daisy-targeted C++ plus a Makefile, with flags controlling memory placement (SRAM/QSPI/SDRAM), MIDI support, sample rate, and polyphony (`-nvoices`). [faust2daisy manpage](https://manpages.debian.org/testing/faust/faust2daisy.1.en.html).
- **`faust2juce`** compiles the same kind of `.dsp` source into a full JUCE project (VST/AU/AAX plugin or standalone), with GUI auto-generation options. [Faust tools manual](https://faustdoc.grame.fr/manual/tools/).

Because both tools accept the *same* Faust source, this is a legitimate "write once, deploy to hardware and to a JUCE plugin" pipeline, and Faust's `architecture/daisy` folder is maintained directly in the Faust repo (not a third-party fork), suggesting reasonable upstream support. [faust/architecture/daisy](https://github.com/grame-cncm/faust/tree/master-dev/architecture/daisy).

**Tradeoffs vs. hand-written C++ (why most of the precedent projects above don't use it for their core engine):**
- Faust is a genuinely different language/paradigm (functional block-diagram composition) with its own learning curve, separate from the C++ the rest of Brainscape (firmware glue, hardware drivers, plugin wrapper) will be written in.
- Generated C++ is compiler output, not meant for a human to read or hand-tune — debugging a subtle real-time-safety or numerical issue means reasoning about generated code rather than authored code.
- Allocation and scheduling control is more indirect: Faust's model manages its own memory layout and block processing, which can conflict with wanting precise, hand-verified control over exactly what's allocated where (SRAM vs. SDRAM on Daisy) for a granular engine that will want carefully budgeted large buffers (grain windows, live-input capture buffer, etc.).
- **Recommendation:** Faust is worth prototyping individual filter/oscillator building blocks or rapidly testing a DSP idea before committing to hand-written C++, but the core granular engine — the part with the tightest memory and real-time constraints and the most product-differentiating detail — should be hand-written C++, matching what OWL, DaisyPedalTestPlugin, ChowDSP, Surge, and NAM all independently converged on.

## Build System: CMake Superbuild

### arm-none-eabi + host in one build

The pattern validated by libDaisy/DaisySP and standard embedded-CMake practice: **one `dsp/` CMake target, built twice** — once cross-compiled for `arm-none-eabi` (linked into the firmware image) and once compiled natively for the host (linked into unit tests and the plugin). This works because `dsp/` itself has zero platform-specific code once the patterns above are followed (no exceptions/RTTI-dependent code paths, no direct `DSY_SDRAM_BSS`, no hardware headers).

Toolchain-file shape for the firmware build, following libDaisy's own `stm32h750xx.cmake`:
- `CMAKE_SYSTEM_NAME Generic`, `CMAKE_SYSTEM_PROCESSOR arm`
- Compiler: `arm-none-eabi-gcc` / `arm-none-eabi-g++`
- Flags: `-mcpu=cortex-m7 -mfpu=fpv5-d16 -mfloat-abi=hard -mthumb`, `-fno-exceptions` (and `-fno-rtti` for C++), `-ffunction-sections -fdata-sections -fomit-frame-pointer`
- Defines: `CORE_CM7`, `STM32H750xx`, `ARM_MATH_CM7`
- `CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY` (common workaround so CMake's compiler sanity-check doesn't try to link a full bare-metal executable during configuration)
[libDaisy stm32h750xx.cmake](https://github.com/electro-smith/libDaisy/blob/master/cmake/toolchains/stm32h750xx.cmake), [CMake Discourse: cross-compile to arm-none-eabi with presets](https://discourse.cmake.org/t/cross-compile-to-arm-none-eabi-with-cmakepresets/4548).

The host build needs no toolchain file at all — it's a normal `add_library(brainscape_dsp STATIC ...)` target compiled with whatever compiler CMake finds (MSVC/Clang/GCC), consumed by both a Catch2 test executable and the plugin target.

### libDaisy CMake integration

`firmware/` should depend on both `dsp/` (the shared engine) and `libDaisy` (Electrosmith's hardware abstraction layer — audio callback, ADC/DAC, GPIO, SDRAM init), with libDaisy pulled in as a git submodule or `FetchContent`, configured with its own `stm32h750xx.cmake` toolchain file for the whole firmware build. libDaisy's actual board init and audio callback registration stay entirely inside `firmware/`; the callback body simply calls into `dsp::Engine::Process()` with pointers to libDaisy's audio buffers. This keeps `dsp/` unaware that libDaisy (or any hardware) exists at all — the dependency direction is strictly `firmware/` → `dsp/`, never the reverse.

### Unit tests and offline audio rendering on host

Standard, community-validated pattern (independent of embedded work — this is how plugin developers iterate on DSP generally): a Catch2 (or doctest) test executable links directly against the host build of `dsp/` and:
- Feeds known input buffers through `Process()` and asserts on numerical properties (impulse response shape, RMS level, absence of NaN/Inf, null-test against a reference).
- Optionally renders longer passages to `.wav` files (via a small `libsndfile`/`dr_wav`-based helper, host-only, not part of `dsp/`) for by-ear or spectrogram inspection when an automated assertion isn't feasible (e.g. "does this granular texture sound right").
- Runs in milliseconds to seconds on a dev machine, vs. minutes for a flash-to-hardware-and-listen loop — this is the single biggest velocity unlock available for DSP iteration and is why `pamplejuce` (JUCE + Catch2 + CMake + GitHub Actions template) exists as a widely-adopted community pattern. [sudara/pamplejuce](https://github.com/sudara/pamplejuce), [Melatonin: when to write tests for DSP code](https://melatonin.dev/blog/when-to-write-tests-for-dsp-code/), [ejaaskel: unit testing audio processors with JUCE & Catch2](https://ejaaskel.dev/unit-testing-audio-processors-with-juce-catch2/).

### CI on GitHub Actions for both targets

A matrix-style workflow with (at minimum) two independent job families:
1. **Firmware job** — installs `gcc-arm-none-eabi` (via `apt-get`, or a dedicated action like `carlosperate/arm-none-eabi-gcc-action`), configures with the `arm-none-eabi` toolchain file, and builds `firmware/` (which pulls in `dsp/` + `libDaisy`). Success = compiles and links to a `.elf`/`.bin`; flashing/hardware-in-the-loop testing is out of scope for CI. [carlosperate/arm-none-eabi-gcc-action](https://github.com/carlosperate/arm-none-eabi-gcc-action).
2. **Host job(s)** — matrix over `ubuntu-latest`/`macos-latest`/`windows-latest`, builds `dsp/` natively, runs the Catch2 test suite, and builds the plugin target(s) (VST3/CLAP/AU as applicable per-OS — AU only on macOS). This mirrors exactly what `pamplejuce` already does for a JUCE-based plugin and is directly reusable/adaptable regardless of final framework choice. [sudara/pamplejuce](https://github.com/sudara/pamplejuce).

Both jobs should build from the identical `dsp/` source tree with no `#ifdef`-forked algorithm logic — if the two jobs need materially different code paths inside `dsp/` beyond the denormal-guard and buffer-provisioning seams described above, that's a signal the platform-agnostic boundary has leaked and needs to be pushed back out into `firmware/`/`plugin/`.

## Recommendations for Brainscape

1. **Choose iPlug2 or DPF over JUCE for the plugin, with iPlug2 as the lead recommendation** — specifically because Neural Amp Modeler proves a permissively-licensed, CLAP-native framework works well for exactly this plugin genre (guitar-effects, not a general synth/DAW-adjacent tool), and because a project this ambitious (hardware + firmware + plugin + hardware design files, likely a small team) benefits from never having to revisit a licensing decision — iPlug2's zlib-style license has no revenue tiers or AGPL considerations to explain to contributors, ever. DPF is a very close second choice, primarily for teams who want CMake+Makefile flexibility, native LV2 for Linux users, and are comfortable with a more manual (NanoVG-based) GUI approach; its Cardinal precedent is the single most battle-tested "wrap a hardware-style DSP engine as a plugin" codebase reviewed here. **JUCE remains a legitimate third option** — it has the strongest same-genre precedent by sheer count (ChowCentaur, SmartGuitarPedal, DaisyPedalTestPlugin, OwlSim) and by far the best GUI tooling and CI templates (pamplejuce) — pick it if the team's primary strength/preference is JUCE-style component-based GUI work, or if a contributor prioritizes ecosystem size and tutorial availability over avoiding AGPLv3 (which, again, is a non-issue for a project that's GPL-licensed and open source throughout).
2. **Ship VST3 + CLAP as the two primary formats, add AU for macOS reach, treat AAX as out of scope initially.** VST3 is now MIT-licensed with no royalty and covers the broadest DAW surface; CLAP is free to add via native support (iPlug2/DPF) or `clap-juce-extensions` (if JUCE is chosen) and costs little given it shares the same underlying engine; AU is needed for Logic Pro users on macOS and is provided by all three frameworks; AAX (Pro Tools) requires a paid Avid developer agreement and is reasonable to defer until there's a clear demand signal.
3. **Design `dsp/` with a single public interface shape from day one**: `Init(float sampleRate, size_t maxBlockSize)` (allowed to allocate) and `Process(const float* const* in, float* const* out, size_t numFrames)` (must not allocate, must not throw, must not lock). No STL container growth, no exceptions, no RTTI, no `iostream`, inside anything reachable from `Process()`. Mirror libDaisy's actual compiler flags (`-fno-exceptions -fno-rtti`) in `dsp/`'s own CMake target even for the host build, so a violation is caught by the desktop compiler immediately rather than only at arm-none-eabi cross-compile time.
4. **Push all memory *placement* decisions (SDRAM-vs-SRAM on firmware, heap-vs-stack on desktop) out of `dsp/` and into `firmware/`/`plugin/`.** `dsp/` should accept pointers/spans to pre-sized buffers (or a capacity passed at `Init()`), never declare `DSY_SDRAM_BSS` globals or hard-coded array sizes itself. This is the single most important seam for keeping the core honestly platform-agnostic rather than "mostly agnostic with a few firmware-only files."
5. **Build a `DenormalGuard` RAII type in `dsp/` with real per-platform implementations** (MXCSR on x86/x64, FPSCR.FZ on ARM/Cortex-M), applied at the top of every top-level `Process()` call on both firmware and plugin — do not assume either platform's flush-to-zero is on by default; verify it explicitly (e.g. a unit test that feeds a decaying-to-denormal signal through a feedback loop and checks CPU time doesn't spike, run on both build targets).
6. **Use `float` (32-bit) as the sole sample type throughout `dsp/`**, reserving `double` only for specifically-identified numerically sensitive spots (e.g. a very-low-cutoff one-pole smoother) if and when a concrete bug demands it — do not default to double anywhere, both because Cortex-M7's FPU favors single precision and because float32 is already effectively transparent for audio.
7. **Stand up the Catch2 + offline-render host test harness before writing much DSP**, not after — given how much velocity every precedent project (pamplejuce, Surge's testrunner, general community practice) gets from being able to validate a granular-engine change in seconds on a dev machine instead of flashing hardware, this should be one of the very first pieces of infrastructure in the repo, ahead of the plugin GUI or most of the firmware I/O plumbing.
8. **Set up the two-job GitHub Actions CI matrix (arm-none-eabi firmware build + host build/test/plugin-package matrix) early**, both to catch platform-agnostic-boundary violations in `dsp/` immediately (a change that breaks the arm-none-eabi build but not the host build is exactly the leak described in recommendation 4) and to keep the project looking active/trustworthy to potential open-source contributors from day one.
9. **License the whole project GPLv3** as the README already leans toward — it's compatible with the now-MIT VST3 SDK, compatible-by-explicit-design with AGPLv3 if JUCE ends up chosen (GPLv3 §13 / AGPLv3 §13 mutual permission), matches Mutable Instruments' STM32 precedent direction and VCV/Cardinal's GPLv3+ precedent, and avoids the extra "must offer source over a network" clause of AGPLv3 that's irrelevant for a desktop plugin/pedal anyway (so there's no reason to reach for AGPLv3 at the project level even though it's fine to *depend on* AGPLv3 code).
10. **Treat Faust as a prototyping tool, not the production path for the core granular engine** — useful for testing a filter or oscillator idea quickly via `faust2daisy`/`faust2juce`, but hand-write the actual grain engine in C++ against the `dsp/` interface above, consistent with every mature precedent project surveyed.

## Recommended Repo Layout

Building on the monorepo scaffold already in place (`dsp/`, `firmware/`, `plugin/`, `hardware/`, `docs/`, `tools/`):

```
brainscape/
├── dsp/                          # platform-agnostic C++ core — the ONLY place the algorithm lives
│   ├── CMakeLists.txt            # add_library(brainscape_dsp STATIC ...), no toolchain assumptions
│   ├── include/brainscape/       # public headers: Engine.h, DenormalGuard.h, Params.h, etc.
│   ├── src/                      # granular engine, delay lines, filters, LFOs, envelope followers
│   └── tests/                    # Catch2 tests + offline WAV-rendering harness, host-only
├── firmware/                     # Daisy Seed firmware — hardware I/O + glue, no DSP algorithm code
│   ├── CMakeLists.txt            # depends on dsp/ + libDaisy (submodule or FetchContent)
│   ├── cmake/toolchains/         # arm-none-eabi toolchain file(s), following libDaisy's pattern
│   └── src/                      # main.cpp: audio callback, pot/footswitch reading, LED/UI, SDRAM buffer decls
├── plugin/                       # desktop plugin — chosen framework wrapper, no DSP algorithm code
│   ├── CMakeLists.txt            # framework-specific plugin target(s): VST3/CLAP/AU
│   ├── src/                      # PluginProcessor-equivalent: owns a brainscape_dsp::Engine instance
│   └── gui/                      # controls mapped 1:1 to firmware pots/switches where possible
├── hardware/                     # schematics, PCB, enclosure (KiCad or similar), BOM
├── tools/                        # build scripts, flashing helpers, CI support scripts
├── docs/
│   ├── research/                 # this document and its siblings
│   └── (design docs, build guides, as the project matures)
├── .github/workflows/
│   ├── firmware.yml              # arm-none-eabi cross-compile job
│   └── host.yml                  # matrix: ubuntu/macos/windows — dsp tests + plugin build
├── CMakeLists.txt                # top-level: add_subdirectory(dsp) always; firmware/plugin conditionally
└── CMakeLists.txt options: BRAINSCAPE_BUILD_FIRMWARE, BRAINSCAPE_BUILD_PLUGIN, BRAINSCAPE_BUILD_TESTS
```

Key structural rules this layout is meant to enforce:
- **Dependency direction is one-way**: `firmware/` and `plugin/` both depend on `dsp/`; `dsp/` depends on neither, and must never `#include` a libDaisy or plugin-framework header.
- **No `#ifdef DAISY` / `#ifdef PLUGIN` branches inside `dsp/`.** If a difference is needed, it belongs in the interface (a parameter, a buffer pointer, a strategy object) or in the calling code in `firmware/`/`plugin/`, not as a preprocessor fork inside the shared algorithm.
- **`dsp/tests/` builds and runs only on the host job**, never attempted under the arm-none-eabi toolchain (Catch2 itself isn't meant for bare-metal), keeping the firmware CI job fast and dependency-light.
- The top-level `CMakeLists.txt` uses options to let a contributor configure just the piece they're working on — e.g. someone doing pure DSP work configures with only `BRAINSCAPE_BUILD_TESTS=ON` and never needs `arm-none-eabi-gcc` or a plugin framework installed at all.

## Sources

- [Steinberg VST 3.8 press release (Oct 29, 2025) — MIT relicensing](https://ocl-steinberg-live.steinberg.net/_storage/asset/819253/storage/master/Press%20Release%20-%202025-10-29%20-%20VST%203.8%20-%20EN.pdf)
- [Steinberg VST3 SDK licensing FAQ (dev portal)](https://steinbergmedia.github.io/vst3_dev_portal/pages/FAQ/Licensing.html)
- [steinbergmedia/vst3sdk (GitHub, primary source for MIT license text)](https://github.com/steinbergmedia/vst3sdk)
- [KVR Audio: Steinberg moves VST3 SDK to MIT, ASIO now GPLv3](https://www.kvraudio.com/news/steinberg-moves-vst-3-sdk-to-mit-open-source-license-asio-now-gplv3-65179)
- [Steinberg VST3 SDK licensing FAQ forum thread](https://forums.steinberg.net/t/vst-3-sdk-licensing-faq/201638)
- [JUCE LICENSE.md](https://github.com/juce-framework/JUCE/blob/master/LICENSE.md)
- [JUCE forum: open source development with JUCE](https://forum.juce.com/t/open-source-development-with-juce/29579)
- [JUCE CMake API docs](https://github.com/juce-framework/JUCE/blob/master/docs/CMake%20API.md)
- [iPlug2 GitHub](https://github.com/iPlug2/iPlug2)
- [iPlug2 site](https://iplug2.github.io/)
- [DPF GitHub](https://github.com/DISTRHO/DPF)
- [DPF LICENSING.md](https://github.com/DISTRHO/DPF/blob/main/LICENSING.md)
- [DISTRHO/dpf-cmake-action](https://github.com/DISTRHO/dpf-cmake-action)
- [DISTRHO/dpf-makefile-action](https://github.com/DISTRHO/dpf-makefile-action)
- [DISTRHO/Cardinal](https://github.com/DISTRHO/Cardinal)
- [Cardinal LICENSES.md](https://github.com/DISTRHO/Cardinal/blob/main/docs/LICENSES.md)
- [clap-wrapper README](https://github.com/free-audio/clap-wrapper/blob/main/README.md)
- [clap-juce-extensions README](https://github.com/free-audio/clap-juce-extensions/blob/main/README.md)
- [CLAP vs VST3 (2026) — Spectral Colors](https://spectral-colors.com/news/clap-vs-vst3-2026/)
- [pichenettes/eurorack](https://github.com/pichenettes/eurorack)
- [VCVRack/AudibleInstruments](https://github.com/VCVRack/AudibleInstruments)
- [VCV Rack: Audible Instruments product page](https://vcvrack.com/AudibleInstruments)
- [RebelTechnology/OwlProgram (OwlSim)](https://github.com/RebelTechnology/OwlProgram)
- [KVR Audio: OwlSim by Rebel Technology](https://www.kvraudio.com/product/owlsim-by-rebel-technology)
- [Music Hackspace: The OWL, a programmable guitar pedal](https://musichackspace.org/the-owl-a-programmable-guitar-pedal/)
- [electro-smith/DaisySP](https://github.com/electro-smith/DaisySP)
- [electro-smith/libDaisy: stm32h750xx.cmake toolchain file](https://github.com/electro-smith/libDaisy/blob/master/cmake/toolchains/stm32h750xx.cmake)
- [electro-smith/libDaisy: External SDRAM guide](https://github.com/electro-smith/libDaisy/blob/master/doc/md/_a6_Getting-Started-External-SDRAM.md)
- [Electrosmith: Memory — What is the difference?](https://electro-smith.com/pages/memory-what-is-the-difference)
- [dylan-robins/DaisyPedalTestPlugin](https://github.com/dylan-robins/DaisyPedalTestPlugin)
- [Chowdhury-DSP GitHub org](https://github.com/Chowdhury-DSP)
- [Chowdhury-DSP/chowdsp_utils](https://github.com/Chowdhury-DSP/chowdsp_utils)
- [ChowDSP site](https://chowdsp.com/)
- [GuitarML/SmartGuitarPedal](https://github.com/GuitarML/SmartGuitarPedal)
- [Surge Architecture.md](https://github.com/surge-synthesizer/surge/blob/main/doc/Surge%20Architecture.md)
- [sdatkinson/NeuralAmpModelerPlugin](https://github.com/sdatkinson/NeuralAmpModelerPlugin)
- [Neural Amp Modeler: the code](https://www.neuralampmodeler.com/the-code)
- [baconpaul/airwin2rack](https://github.com/baconpaul/airwin2rack)
- [Airwindows](https://www.airwindows.com/)
- [Faust tools manual (faust2daisy, faust2juce)](https://faustdoc.grame.fr/manual/tools/)
- [faust2daisy manpage](https://manpages.debian.org/testing/faust/faust2daisy.1.en.html)
- [grame-cncm/faust: architecture/daisy](https://github.com/grame-cncm/faust/tree/master-dev/architecture/daisy)
- [ARM: floating-point data types and arithmetic (flush-to-zero)](https://developer.arm.com/documentation/ddi0403/d/Application-Level-Architecture/Application-Level-Programmers--Model/The-optional-Floating-point-extension/Floating-point-data-types-and-arithmetic)
- [ARM Cortex-M7 Technical Reference Manual](https://developer.arm.com/documentation/ddi0489/f/Chdbebfc)
- [JUCE forum: state of the art denormal prevention](https://forum.juce.com/t/state-of-the-art-denormal-prevention/16802)
- [KVR Audio: DSP code — when to use floats or doubles](https://www.kvraudio.com/forum/viewtopic.php?t=628294)
- [Bit Bashing: C++ on Embedded Systems](https://bitbashing.io/embedded-cpp.html)
- [WolfSound: C++ pointers explained for audio programming](https://thewolfsound.com/c-plus-plus-pointers-explained-for-audio-programming/)
- [timur.audio: using locks in real-time audio processing, safely](https://timur.audio/using-locks-in-real-time-audio-processing-safely)
- [LMMS Realtime Conventions wiki](https://github.com/LMMS/lmms/wiki/Realtime-Conventions)
- [sudara/pamplejuce](https://github.com/sudara/pamplejuce)
- [Melatonin: when to write tests for DSP code](https://melatonin.dev/blog/when-to-write-tests-for-dsp-code/)
- [ejaaskel: unit testing audio processors with JUCE & Catch2](https://ejaaskel.dev/unit-testing-audio-processors-with-juce-catch2/)
- [carlosperate/arm-none-eabi-gcc-action](https://github.com/carlosperate/arm-none-eabi-gcc-action)
- [CMake Discourse: cross-compile to arm-none-eabi with CMakePresets](https://discourse.cmake.org/t/cross-compile-to-arm-none-eabi-with-cmakepresets/4548)
- [GNU: Frequently Asked Questions about the GNU Licenses (GPLv3/AGPLv3 compatibility)](https://www.gnu.org/licenses/gpl-faq.en.html)

## Open questions

- **Final framework decision.** This document recommends iPlug2 (lead) or DPF, with JUCE as a legitimate alternative — the actual choice should be made once the team knows who's building the plugin GUI and how much they value JUCE's larger ecosystem/tutorials vs. the other two frameworks' license simplicity and CLAP-native support. Worth a short internal spike (a trivial "pass-through" plugin in each of the top two candidates) before committing.
- **AAX/Pro Tools support** — deferred per the recommendations above, but worth revisiting if user demand appears; requires a paid Avid developer agreement regardless of framework chosen.
- **Exact Faust usage scope**, if any — this document recommends prototyping-only, but the team should decide explicitly whether any *shipped* algorithm (e.g. a simple utility filter) is allowed to originate from Faust-generated code, or whether the rule is "hand-written C++ only" for everything that ships.
- **ChowDSP/Aviate Audio Multiverse code-sharing architecture is unverified** — the product description implies a hardware-pedal + desktop-software-tooling relationship analogous to Brainscape's goals, but the specific extent of shared DSP code between Multiverse firmware and its desktop "Effects Shop" was not independently confirmed from primary sources; worth a follow-up look if more detail would be useful (e.g. reaching out to Aviate Audio/ChowDSP directly, or finding their firmware/tooling repos if public).
- **airwin2rack's internal plugin-wrapper framework** was not confirmed (the repo wraps a consolidated Airwindows library as CLAP/VST3/AU/LV2/standalone and as a VCV Rack module, but which specific framework glues the non-Rack plugin formats together wasn't verified from the sources gathered) — low priority to resolve since it's a secondary precedent, but flagged here rather than stated as fact.
- **Whether Daisy's default audio block size (varies by project config, commonly very small, e.g. 4–48 samples) versus typical desktop host block sizes (64–2048+ samples) creates any latency-sensitive algorithm behavior worth special-casing** — the `Init(sampleRate, maxBlockSize)` pattern recommended above handles this mechanically, but whether the granular engine's *musical* behavior (grain scheduling granularity, etc.) needs to be block-size-aware in a deeper way is a DSP-design question for whoever builds the engine, not a build-system question — flagged here so it isn't lost.
- **Precise current (2026) DAW-by-DAW CLAP support list** — the Spectral Colors article was current as of its own publication and explicitly warns "the list changes"; before finalizing which formats to ship at launch, re-check the authoritative list (likely at the CLAP project's own site/GitHub) rather than relying on this document's snapshot.
