# Determinism profile — evidence record

> Engineering record behind [determinism-profile.md](../determinism-profile.md). The design
> document states what is required and why; this file keeps how we know: the measurements and
> probe narratives behind each number, the places where the evidence disagreed and how each
> disagreement was resolved, and the corrections made while the profile was reviewed. Sections
> 1–7 follow the design document's numbering, so record §5.7 holds the evidence behind design
> §5.7; record §0 describes the evidence sources, and record §8 holds cross-cutting material
> (resolved disagreements, review corrections and cross-document reconciliation) rather than
> the design's §8. Code is cited as `path:line` against `main` at `e86e971`; other documents
> are cited by section, because the change that adds the profile also amends them. Every
> number is marked **measured** (with its source label), **estimated** or *calculated*, as in
> the design.

---

## 0. Evidence sources

All evidence was produced on 2026-10-05, in four rounds:

1. **Feasibility round**, which asked whether the engine could ship as a VST3 companion: a
   host-readiness audit, a survey of what the existing documents had decided, a first
   pedal/plugin parity investigation, and an adversarial challenge that re-ran the key
   probes, refuted or corrected several claims and added missed findings.
2. **Decision round**, run after the owner's three decisions: a working prototype of the
   profile, a cross-ISA floating-point semantics investigation, a verification-oracle
   investigation, a JUCE integration investigation and a preset-pipeline investigation.
3. **Block-split verification**: an independent reproduction of the freeze and mark-positioning
   bug on unmodified `main`, its root causes, and a fix verified on a scratch copy of `dsp/`.
4. **Review** of the design documents in four areas (numerics and determinism; firmware, USB,
   SD and licensing; JUCE hosting; product consistency), each with new probes.

**Environment.** Every desktop measurement was taken on one Intel i9-12900K under Windows 11,
with MSVC 19.40 (VS 2022 17.10). GCC 14.2 and Clang 14 ran in Docker with no network. M7 code
ran under `qemu-arm` 10.2.3 with `QEMU_CPU=cortex-m7`, through Docker Desktop's binfmt
handler (the Linux mechanism that runs foreign-architecture executables through an emulator).

**Where the probes live.** The probes worth keeping are committed under
[`tools/parity/`](../../../tools/parity/) (its README explains how to rebuild them; modified
engine copies are stored as patches against `e86e971`). The last column below names each
source's directory there. Sources marked "not stored" depended on third-party source trees
(libDaisy, TinyUSB, the Daisy bootloader, JUCE) or were one-off audits; their method and
results are written up in this record. Every `bugcheck/` path in this record and in design
§5.7 is under `tools/parity/bugcheck/`; the instrumented copies (`dsp_probe`, `dsp_fix_probe`)
were not stored, and the regression test is part of `fix.diff`.

| Label | Source | What it covers | Probe directory under `tools/parity/` |
|---|---|---|---|
| [host] | Host-readiness audit | Block sizes, rates, lifecycle, threading, shared-library build, CPU | not stored |
| [plan-of-record] | Survey of the existing documents | What they decided and where they disagree | not stored (document survey) |
| [parity-v1] | First parity investigation | Contraction simulated on x86, libm rates, rate dependence, data layout | not stored; superseded by [prototype] |
| [challenge] | Adversarial challenge of the feasibility round (the companion record cites it under the same label) | Refutations, confirmations, missed findings | not stored |
| [prototype] | Determinism-profile prototype | DetMath plus contraction off, a 32-build battery including emulated M7, costs, negative controls, the first report of the block-split bug | `prototype/` (harness, patches, `hashes/`) |
| [fp-isa] | Floating-point semantics investigation | The FTZ/FZ boundary, denormal policy, NaN and casts, ODR, pragmas, guard design | `fp-semantics/` |
| [oracle] | Verification-oracle investigation | QEMU user and full-system modes, ring-size independence, CI design | `oracle/` (harness, syscall shim, `logs/`) |
| [juce] | JUCE integration investigation | Versions and licences, parameter normalization, VST3 automation timing, LTO and ODR probes | not stored (read JUCE source); ODR probe in `fp-semantics/odr/` |
| [preset] | Preset-pipeline investigation | Exact restart, load order, Spillover convergence under `Reset`, event timing, number formatting | `preset/` |
| [bugcheck] | Block-split verification | The harness, the 32-subset matrix, instrumented copies counting write-ahead reads, the three-part fix and both freeze-mark variants, the regression test | `bugcheck/` (`fix.diff`, `fixB-over-fix.diff`, `results/`) |
| [review-num] | Numerics review | The write-ahead and old-invariant counters, Spillover as specified, DetMath domain edges, DAZ and canonicalization, the MSVC symbol audit | `review-numerics/` |
| [review-fw] | Firmware and licensing review | libDaisy v9 source excerpts (system, audio, SDRAM, linker script), mock links of the USB handlers | not stored (third-party sources) |
| [review-juce] | JUCE hosting review | JUCE 9.0.3 CMake and wrapper sources, a PIC probe | `juce/` (float-normalisation probe) |
| [review-pc] | Product-consistency review | The `-fPIC` effect on the M7 objects | not stored |

## 1. The requirement

### 1.2 Why a tolerance fails: detail

- **Fusion alone decorrelates stochastic presets.** The default preset (jitter 0.2) nulls at
  −4.8 dB with correlation 0.83 under MSVC `/fp:contract` against `/fp:precise`
  (**measured** [parity-v1]). Against the real firmware code under an emulated Cortex-M7,
  jittered presets null at −1.3 to −5 dB and decorrelate within 1.2–12 s (**measured**
  [prototype]). The trigger is one grain birth moving by one sample: every random draw is
  keyed on the birth sample (`dsp/include/brainscape/GrainMath.h:40-45`).
- **Feedback with a fixed schedule:** one-LSB flips in the int16 ring recirculate and grow;
  −105 to −111 dBFS and growing on a clean delay at feedback 0.5 (**measured** [challenge],
  [parity-v1]).
- **Fixed schedule, no feedback:** −147 to −179 dBFS (**measured** [parity-v1]).
- **Onset triggering is not the cause.** One feasibility probe attributed divergence to onset
  triggering. The challenge showed that its onset preset also had jitter on; with jitter 0,
  onset-triggered renders null at −165.5 dBFS across the fusion change (**measured**
  [challenge]). The design attributes divergence to random grain timing.
- The grain-engine review that set contract #7 had already named the price of bit-exactness:
  `-ffp-contract=off`, in-tree transcendentals and no fast-math. The profile measured that
  price (design §7) and pays it.

### 1.3 Passages amended in other documents: detail

The change that adds the profile also amended, on 2026-10-05, the passages of other documents
that the profile made stale; `grain-engine.md` §13 lists the grain-engine ones. This is what
was wrong in each, as the documents stood at `e86e971`, and what it became.

- `grain-engine.md` §10 #6 and #7 promised identity only within one build and a −120 dBFS
  cross-build null; they now carry the text of design §2.6. §10 #1 now requires event-split
  freeze, real onsets, a far-rail position and a held-freeze re-anchor (design §5.7), and
  records that today's code violates it. §12 gained an item that points at the profile's
  risks and open questions. §11's Freeze row had a "~87 s ceiling"; it is now a re-anchor at
  three quarters of the ring, decided per sample. §3's write-head guard table had no
  live-head reference and no write-ahead term; it now has both. §3's pitch paragraph and §8
  specified a lookup table for `SemitonesToRatio`; they now follow the provisional
  polynomial-kernel decision of design §3.9. §8's ScheduleGrain row was read as a birth rate;
  it now states cycles per sample with at most one scheduler birth per sample, and §8 quotes
  the profile's raised totals. §9's "~40–80 ms" `ClearHistory` estimate was below the 44 ms
  SDRAM floor; it is now about 45–160 ms. §9's threading table had "any thread" and
  "coalesce-on-overflow" for a single-producer queue; it now has one producer per engine and
  counted overflows, as design §5.11 specifies. §9's API listing exported a public
  `ScopedDenormalGuard`, which ORs flush bits into the caller's control word; it now points at
  the profile's private full-word guard (design §4.1, §5.3), and its `maxBlockSize` comment
  has the plugin pass 512. §1 and §5 described mode JSON parsed on the pedal; compilation is
  now desktop-only.
- `docs/STATUS.md` said "no libm in the audio path", although design §3.9's inventory shows
  `exp2`, `cos`, `sin` and `log` at every grain birth inside `Process`; it claimed verified
  block-split invariance with "the full stochastic feature set", which design §5.7 refutes;
  it listed "in-tree LUTs are specified" as the cross-build plan; and its next steps put the
  SPSC queue second and an iPlug2 plugin seventh. It now states each of these as the profile
  does and follows companion §8.1's merged sequence.
- `README.md` said the engine is "inside bit-exact determinism contracts", which holds only
  within one build and, for contract #1, only after the block-split fix. It now says so.

## 2. The parity contract

### 2.1 Origin of the contract

The preset-pipeline investigation proposed an exactness contract with five conditions: the
same sound revision, a canonical configuration, an Exact load from the restart state,
identical 48 kHz float input, and a frame-stamped event stream ([preset]). The design adopts
them and adds two preconditions (exact plain parameter values; profile-conforming execution)
and the caveat that block-size independence holds only after the block-split fix.

The 47-frame minimum latency for a plugin that keeps the 48-frame grid on arbitrary host
buffers is *calculated*: a host buffer can end one frame into a 48-frame block, so up to 47
frames must wait for the block to fill.

### 2.2 Boundary: detail

- **Rate dependence:** a 100 ms preset repeats every 110.667 ms at 48 kHz and every
  105.333 ms at 96 kHz (**measured** [host], [challenge]).
- **libDaisy v9's codec conversion**, read in source [review-fw]:
  - Input: `DaisySeed::ConfigureAudio` selects 24-bit SAI on every Seed revision, the Seed3
    included, with `postgain = 1` (`src/daisy_seed.cpp:259`, `:306`, `:326`). The callback
    computes `s242f(word) * postgain_recip_` (`src/hid/audio.cpp:306`), where `s242f`
    sign-extends and multiplies by `S242F_SCALE`, a literal that rounds to exactly 2⁻²³ in
    binary32 (`src/daisy_core.h:42`, `:138-141`). The engine's input is therefore exactly
    *i* × 2⁻²³ while `postgain` is 1 or another power of two.
  - Output: `f2s24` clamps to ±0.999985 (`FBIPMAX`, about 126 LSB below 24-bit full scale)
    and truncates toward zero (`src/daisy_core.h:33`, `:146-151`).
- A future SD capture feature could widen the boundary to the codec's digital words.

### 2.3 Preconditions: measurements

- **Exact-restart state:** without clearing the feedback tamer's diffuser buffers, feedback
  presets null at only −51 to −56 dB against a fresh `Init` (**measured** [preset]).
- **Frame-stamped events:** one Pitch+Mix change applied at block start made 48-frame and
  512-frame renders differ (null −55.8 dB); splitting the block at the event's frame made
  them identical (**measured** [preset]).
- **Exact plain values:** JUCE's normalize/denormalize round trip changes 8–20 % of all float
  values in most of the 28 parameter ranges. In one preset two values changed by this round
  trip (1–2 ULP each), and that was enough to break identity (null −118.5 dBFS) (**measured**
  [juce]). newlib's `strtof` rounds twice ([preset], [juce]).
- **Profile-conforming execution:** a host-set round-toward-zero mode changed all 8 hashes on
  both ISAs ([oracle]) and nulled at −26 dBFS ([juce]) (**measured**).

### 2.4 What is not guaranteed: detail

- **Spillover loads, as specified.** Probe on a repository copy with the RNG epoch
  [review-num]: 10 s of a prior preset, then a Spillover load as the design specifies (trails
  kept, only the RNG epoch restarted), then 30 s rendered in 48-frame blocks. The default
  preset (jitter 0.2, feedback 0) and a periodic preset (jitter 0, spray 0, feedback 0) both
  still differed on the last frame of the 30 s; for the default preset, 2,876,160 samples
  differed (**measured**).
- **Where the reconvergence figure came from.** The often-quoted 0.30–0.43 s reconvergence
  ([preset]) was measured with a different load: `Engine::Reset` plus an RNG rebase, which
  kills every voice, re-arms the scheduler (`detail/Granular.h:75-84`), zeroes the feedback
  FIFO (`Engine.cpp:210`) and resets the detector. Under that load, feedback-free presets
  became identical 0.25–0.43 s after the load ([preset], [review-num]; the same default
  preset with `Reset` plus the epoch: 0.264 s; the periodic preset: 0.250 s). A feedback-0.6
  preset reconverged only 6.75 s into silence, and a reverb/shimmer preset not within 60 s of
  silence, also under `Reset` semantics ([preset]).
- **Real-time plugin before the fix:** a static preset with mark positioning diverged once its
  last onset mark was about one ring old (from 87.5 s at the canonical ring), and a freeze
  held past the re-anchor point (65.5 s at the canonical ring) diverged with live positioning
  too (**measured** [bugcheck]).
- **DAW automation:** JUCE 9.0.3 applies only the last automation point per parameter per
  block, at block start, as a normalized float (`juce_audio_plugin_client_VST3.cpp:3495-3540`,
  read in [juce]). Automation timing therefore depends on the host's buffer size and value
  rounding. Static presets in a 48 kHz session are covered when the session starts from the
  exact-restart state and, until the fix lands, only on the 48-frame grid. A CLAP build whose
  processor receives raw CLAP events (the direct-process path in `clap-juce-extensions`) could
  deliver sample-accurate exact values in future ([juce]).
- **Plugin bounces:** two bounces separated by `Reset` + `ClearHistory` differed in 265,908
  samples (**measured** [challenge]).

### 2.5 Label rules: detail

- JUCE's default standalone restores whatever sample rate was saved last, so the custom
  Standalone opens the device at 48 kHz itself and checks the rate it got ([juce]; companion
  §2.2 specifies it).
- Emulation detection: on macOS `sysctlbyname("sysctl.proc_translated")`; on Windows
  `IsWow64Process2` or `GetMachineTypeAttributes` reporting a native ARM64 machine.

## 3. Numerics requirements

### 3.1 Operation set: detail

- Default NaN bits (`0xFFC00000` on x86, `0x7FC00000` on Arm) and the different two-NaN
  propagation rules: **measured** on x86, Arm from QEMU source ([fp-isa]).
- Out-of-range float → int: x86 returns `0x80000000`, Arm saturates, and MSVC constant-folds
  a third way (**measured** [fp-isa]).
- Vectorization: GCC 14 `-O3` and Clang 14 AArch64 kept sum reductions as sequential chains,
  and GCC `-O3 -march=x86-64-v3` (autovectorized; x86-64-v3 includes AVX2 and hardware FMA)
  reproduced the golden hash (**measured** [fp-isa], [oracle]).

### 3.2 Flags per toolchain: evidence

- `arm-none-eabi-g++` 10.3 emitted 152 fused instructions in `dsp/` under both
  `-std=gnu++17` and `-std=c++17`, and 0 with `-ffp-contract=off` (**measured** [parity-v1],
  [prototype], [preset]): Engine 13, Granular 35, OnsetDetector 6, PostChain 98.
- **MSVC.** Since VS 2022 17.0, `/fp:precise` no longer contracts (Microsoft's `/fp`
  documentation); earlier MSVC generated FMAs on ARM64 under `/fp:precise`, hence the version
  floor. Golden: `/fp:precise`, `/fp:strict`, `/arch:AVX2`, AVX2 with strict, and LTCG with
  mixed per-file flags. Divergent: `/fp:contract`, `/fp:fast` (**measured** [prototype]).
  MSVC ARM64 was not measured.
- **GCC.** The C++ default is `fast` (fuses across statements). GCC 14 implements `on` and
  fuses within an expression (129 FMAs, divergent). GCC 10 and 11 treat `on` as `off`, which
  is not something to rely on. Golden: GCC 11.4, 12.2 and 14.2 at `-O2`/`-O3`, baseline
  x86-64 and `-march=x86-64-v3` (**measured** [prototype]). Without `-fno-math-errno`, every
  inline square root keeps an error-handling call to libm's `sqrtf`/`sqrt` ([prototype]).
- **Clang.** The default is `on`; AArch64 always has FMA, so an Apple Silicon build fuses
  unless told not to (**measured**, Clang 14 targeting `arm64-apple-macos11` [fp-isa]).
  `-ffp-model=precise` implies `contract=on`. `-ffp-contract=fast` ignores source pragmas
  (**measured**, 385 FMAs, divergent [prototype]). Golden: Clang 14 `-O2`/`-O3` with `off`.
- **arm-none-eabi-gcc.** Golden under QEMU with 10.3.1 and 12.3.1; `-Os` also golden
  ([oracle]). `-fno-math-errno` removes the `sqrtf` error-handling call behind each inline
  `vsqrt.f32`, so no libm symbol remains ([prototype], [fp-isa]).
- **JUCE wrapper units.** JUCE compiles its module sources inside the shared-code target that
  `juce_add_plugin` creates ([juce]). Each plugin format (VST3, AU, LV2, CLAP, Standalone) is
  a separate target that compiles its own wrapper unit, `juce_audio_plugin_client_<format>`,
  and links the shared-code target `PRIVATE`, so it inherits only interface properties
  (JUCE 9.0.3 `extras/Build/CMake/JUCEUtils.cmake:1480-1497`, read in [review-juce]). With a
  `PRIVATE` link of `brainscape_dsp` the wrapper units would compile without
  `-ffp-contract=off`; on AppleClang arm64 they would contract by default, and their copies of
  JUCE's inline FP functions would compete with the shared-code copies at link time. Every
  definition a wrapper unit must see (for example `JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP`) is
  set `PUBLIC` too. JUCE's own CMake plugin example sets its definitions and recommended flags
  `PUBLIC` for this reason (`examples/CMake/AudioPlugin/CMakeLists.txt:83-88`, `:105-112`,
  [review-juce]); the same example links JUCE's recommended LTO flags, which design §3.4
  forbids.
- The toolchain file's flags go into `BRAINSCAPE_M7_FLAGS`
  (`tools/cmake/arm-none-eabi-toolchain.cmake:14`), so firmware code compiled through
  libDaisy's build gets them too; the firmware and the M7 oracle compile the same
  `dsp/CMakeLists.txt` through that toolchain file.
- JUCE's recommended flags add no fast-math or contraction flags (`-O3`, `/Ox`) ([prototype],
  [juce]). The desktop CPU cost of applying the profile to JUCE code is about zero
  (**estimated** [juce]).

### 3.3 Tripwires: evidence

- MSVC predefines `_M_FP_CONTRACT` under both `/fp:contract` and `/fp:fast`, and
  `_M_FP_PRECISE` under `/fp:precise` (**measured** [juce]).
- The `__FINITE_MATH_ONLY__` check is not in the evidence; the tripwire self-test must
  confirm that GCC and Clang define it as expected.
- `#pragma GCC optimize("fp-contract=off")` does override `-ffp-contract=fast` on GCC
  ([prototype]), but GCC documents the optimize attribute and pragma as unsuitable for
  production code, so it is not adopted.
- GCC ignores `#pragma STDC FP_CONTRACT`: it warns under `-Wall` and fuses anyway
  (**measured** [fp-isa]). Clang honours it, and MSVC's `#pragma fp_contract(off)` overrides
  `/fp:contract` (**measured**).
- What catches what the tripwires cannot see: GCC's missing contraction macro → the
  fused-instruction audit and golden hashes; Clang's `-ffp-contract=fast` ignoring the pragma
  → the configure-time flag check and the fused-instruction audit; LTO and ODR merging → the
  no-LTO rule, the no-FP-bodies rule and the link-order-swap test; a libm call → the
  undefined-symbol audit; the host FP environment → the guard and the perturbation job;
  silicon errata → hardware-in-the-loop.
- A suggestion to place the pragmas in the public header as well was declined: a pragma in a
  public header changes code generation in every consumer unit after the include point, and
  protects nothing there, because public headers carry no FP function bodies.

### 3.4 LTO: where the evidence disagreed

- The prototype found that LTO with mixed per-file flags **kept** the golden hash on GCC,
  Clang and MSVC (`/GL` + `/LTCG`); GCC kept contraction decisions per function, with 168
  FMAs, all outside the engine ([prototype]).
- The JUCE investigation found that LTO **broke** it: under GCC `-flto`, a strict engine
  function was inlined into a contracting caller and took on the caller's contraction (in both
  link orders); under MSVC LTCG the result depended on link order, because the linker kept the
  plugin unit's copy of an inline header function (**measured**, both, [juce]).
- Both are correct. They differ in whether engine code was inlinable into, or shared with, a
  contracting unit, which under LTO depends on inlining heuristics that nothing guarantees.
  Resolution: no LTO on `brainscape_dsp`, and none in v1 app and plugin targets. The
  no-FP-bodies rule of design §3.5 removes the known ODR mechanism, but the LTO rule does not
  rely on that alone.

### 3.5 ODR and PIC: evidence

- **ODR copy selection.** When an inline function defined in a header is compiled by two units
  with different FP flags, the linker keeps one copy, chosen by link order, and both call
  sites use it. With the plugin object first, the engine unit's call returned the fused result.
  Reproduced on MSVC, GCC, and GCC with LTO (**measured** [fp-isa], [juce]). A JUCE unit that
  calls an engine helper, for example to draw a grain envelope or display a pitch ratio, would
  emit exactly such a competing copy.
- **Clean headers.** `detail/Granular.h`, `detail/OnsetDetector.h`, `Params.h`, `Memory.h` and
  `HostArenas.h` contain only integer code, constants or assignments.
- `sizeof(Engine)` is 6,032 B on the M7 ([oracle]) and 6,168 B on x64 ([host]) (**measured**).
- **`STB_GNU_UNIQUE`.** On Linux a non-hidden `kParamTable` becomes a process-wide
  `STB_GNU_UNIQUE` symbol; with two builds of the engine loaded in one process, the second read
  the first build's parameter defaults (**measured** [host]).
- **`-fPIC` on the M7.** With the PIC and visibility target properties on `brainscape_dsp`,
  `arm-none-eabi-g++` 10.3 compiled `dsp/` with `-fPIC -fvisibility=hidden
  -fvisibility-inlines-hidden`. Global accesses became PC-relative through the global offset
  table (GOT): `Engine.cpp` gained 8 GOT relocations, its `.text` went from 6,379 to 4,683 B
  and its `.data` grew by 672 B (**measured** [review-juce], [review-pc]). That is no longer
  the archive the M7 measurements used.
- **Where PIC and visibility are set: resolved disagreement.** The host-readiness audit filed
  them as an engine target change; the challenge showed the superbuild can set them before
  `add_subdirectory(dsp)`; an earlier draft of the profile put them on `brainscape_dsp`, and
  the review measured the `-fPIC` effect above. Resolution: the superbuild sets
  `CMAKE_POSITION_INDEPENDENT_CODE ON`, `CMAKE_CXX_VISIBILITY_PRESET hidden` and
  `CMAKE_VISIBILITY_INLINES_HIDDEN ON` before `add_subdirectory(dsp)` when
  `BRAINSCAPE_BUILD_PLUGIN` is on, as companion §3.2 specifies, with a firmware-archive GOT
  audit. A suggestion to
  set hidden visibility on `brainscape_dsp` unconditionally was declined: no evidence shows
  hidden visibility is needed outside plugin builds, and plugin-only keeps the firmware and
  oracle archive exactly as measured.

### 3.6 Evaluation method: evidence

- `FLT_EVAL_METHOD` is 0 on MSVC x64 at run time and on `arm-none-eabi-g++` 10.3
  (`__FLT_EVAL_METHOD__ 0`); Clang defaults to source-precision evaluation on 64-bit targets
  (**measured** [fp-isa]). The M7's `__ARM_FP` is 14: hardware single and double precision.
- JUCE lists Windows x86 as a deployment target, so refusing 32-bit x86 is a real exclusion
  ([juce]).
- All 123 distinct floating-point literals in `dsp/` convert to identical bits on MSVC 17.10
  and GCC 14 (**measured** [fp-isa]).
- `long` is 4 bytes on Windows and the M7 and 8 bytes on Linux and macOS; plain `char` is
  unsigned on Arm EABI; `dsp/` has no arithmetic that depends on either (**measured**
  [fp-isa]). Serialized data is written field by field because padding and pointer sizes
  differ ([parity-v1], [preset]).

### 3.7 The NaN-free boundary: evidence

- **Non-finite input.** The dry path copies it to the output (`Engine.cpp:529-530`), and
  `QuantizeS16` maps NaN to +32767 full scale in the ring (`Engine.cpp:44-46`) ([fp-isa]).
- **The subnormal attack leg.** `a = legs * skew` and `attackInv = a > 0 ? 1/a : 0`
  (`GrainMath.h:121,126`). A subnormal `skew` such as 1e-40 from a preset file passes
  `SetParam`'s clamp (`Engine.cpp:364-365`), and when the leg length is small, `a` is itself
  subnormal. Under today's FTZ/FZ guard a subnormal `a` flushes to 0, which is harmless. Under
  gradual underflow it stays subnormal, and once it is below about 2.9e-39, `1/a` overflows to
  +inf. Only output index 0 then lies in the attack leg (`GrainMath.h:134`), and `EnvValue(0)`
  computes `0 * inf = NaN`. The window-table cast at `Granular.cpp:46` converts that NaN to
  index 0 on both ISAs, so the table is never read out of bounds. The real cross-target effect
  is the NaN itself: it propagates through the wet path to the output, where the default NaN
  differs by ISA, and through the feedback path it poisons the tamer's filter states and
  reaches the ring, where `QuantizeS16` writes +32767 on both targets (derived from the code
  and the **measured** cast and NaN behaviour, [fp-isa], [review-num]).
- **The `MakeEnv` guard's derivation.** When the attack is shorter than one frame, only output
  index 0 lies in the attack leg, and it evaluates to 0 either way; when the decay is shorter
  than one frame, no index lies in the decay leg.
- **DAZ and canonicalization** (**measured** [review-num], MSVC 19.40 `/fp:precise`):
  `1e-40f == 0.0f` is false in IEEE mode and true under FTZ|DAZ. A canonicalizer written as a
  sign-bit test for −0 plus `fabs(v) < FLT_MIN && v != 0` returned `0x00000000` in IEEE mode
  but left the subnormal `0x000116C2` unchanged under DAZ. The stored bits matter: with
  `WindowSkew` stored as 1e-40, `a = legs * skew` (about 3e-37) is a normal number, the attack
  leg is nonzero and `EnvValue(0)` returns 0; with +0 it returns 1. Every grain's first sample
  would then differ.
- **No grid: resolved proposal.** The FP-semantics investigation proposed quantizing
  parameters to a grid so that the domain would be exhaustively testable ([fp-isa]). Not
  adopted: the binary32 domain is already finite (at most 2³² values per parameter) and can be
  enumerated directly, and a grid would silently change authored values. (Companion §5.5 had
  treated the grid as open in an earlier draft; it is closed.)
- **Parameter text** (**measured**, exhaustively over all 4.28 × 10⁹ finite floats [preset]):
  shortest round-trip strings read back exactly through a correctly rounded float parser.
  Through a double-based parser — the path that JavaScript, Python and newlib's `strtof` all
  take — exactly two values come back wrong (±7.038531e-26). The writer therefore emits
  shortest round-trip digits, falling back to 9 significant digits for those two, and readers
  use a correctly rounded parser such as a vendored `fast_float` (MIT/Apache-2.0/Boost).
- **`SanitizeInput` at the wrapper.** The dry path stays a pure pass-through: bit-exact at
  Mix = 0 (**measured** [host]), and not clipped above 0 dBFS, which DAW users rely on for
  headroom. The wet path still clips at ±1.0 in the int16 ring (`Engine.cpp:44-45`),
  identically on every target (**measured** [challenge]).
- **Input handling: resolved disagreement.** One draft put every live input on the 24-bit
  grid; another only sanitized. The grid would clip a DAW's dry signal at 0 dBFS, re-quantize
  it and break dry bit-exactness at Mix = 0. Resolution: `SanitizeInput` on live paths,
  `ConditionInput24` only for pedal-faithful renders, both defined once in
  `InputCondition.h`. The exact-binary64 implementation of `ConditionInput24`, in the style of
  `QuantizeS16` (`Engine.cpp:40-47`), was proposed by the companion design during review and
  adopted. It never uses `lrintf` or `nearbyint`, which follow the rounding mode and may call
  libm.

### 3.8 Targets: evidence

- ARM64EC defines `_M_X64`, so the guard would take the x86 branch through an emulated
  `MXCSR`; whether that reaches the real `FPCR` is unverified ([fp-isa], [challenge]).
- AArch64 conformance is expected but was not measured, because no AArch64 compiler was
  available to the investigations.
- With gradual underflow, an emulator (Rosetta 2, Prism, QEMU) only needs correct IEEE basic
  operations.

### 3.9 libm and DetMath: evidence

- The libm inventory was verified by grep against the evidence lists ([prototype], [fp-isa]).
- **The libraries really differ.** With contraction off on both sides, MSVC and GCC x64 builds
  of today's code diverge from 0.33 s, nulling at −119 dBFS ([juce]). With contraction off on
  the M7 but newlib kept, 6 of 8 presets still differ from x86 ([oracle]) (**measured**).
- **Compile-time folding differs even with one library.** GCC folds constant libm calls
  through MPFR, LLVM through the build machine's libm, and MSVC does not fold. GCC 14 + glibc
  folded results differ from the same library at run time on 11 of 1,000 `cosf` and 18 of
  1,000 `sinf` engine-shaped arguments (**measured** [fp-isa]). A firmware build that made
  48 kHz a `constexpr` would silently fold its Init tables differently from the app.
- Today's agreement between glibc and newlib is accidental: both use Arm's optimized
  routines. UCRT differs, and newlib's own libm contains contracted FMAs that change with
  toolchain upgrades ([oracle], [prototype]).
- **The prototype library** is `dsp_det/include/brainscape/DetMath.h`, 301 lines
  ([prototype]). Under MSVC `/fp:precise`, a plain `sqrtf` compiles to `sqrtss` plus a guarded
  call to the C runtime's `sqrtf` for negative inputs (**measured** by disassembly
  [review-num]); the values are the same, but the reference fails the symbol audit. On GCC and
  Clang, `__builtin_sqrtf` with the required `-fno-math-errno` compiles to the bare
  instruction.
- **Domain defects in the prototype** (**measured** [review-num]): `LogD(0)` returns −709.09
  instead of failing, and `LogD(−1)` returns 0; `Exp2D` builds its result exponent with an
  unchecked scale step that silently wraps outside [−1022, 1023] (`Exp2D(−1100)` ≈ −2.4e285,
  `Exp2D(1030)` ≈ −3.6e-307); `RoundHalfAwayI32` is undefined for |x| ≥ 2³¹. Combined,
  `PowF(0, y)` gives 0 for y ≤ 1, −inf for 1 < y < 2, −4 at y = 2, −0 at y = 3 and +16 at
  y = 4. The macro curves of `grain-engine.md` §5's example mode use exponents 1.5 and 3.0, and
  a macro at its minimum or an expression pedal heel-down evaluates the curve at 0. The
  results are wrong identically on every target, so golden hashes would never catch them.
- **Accuracy** (**measured** against `__float128` [prototype]): `exp2f` maximum 0.5000 ULP
  over 16.8 M arguments, none incorrectly rounded; `logf` exhaustive over all 2²⁴ values the
  RNG can feed it, 0.5000 ULP; pan sin/cos over 4.2 M arguments each; `powf` (normalization)
  over 1.05 M; window and Hann tables 0.5 ULP; `LpCoef` rounded one near-tie in 1 M the wrong
  way. For comparison, glibc `exp2f` is 0.5015 ULP with 10,500 incorrectly rounded results,
  glibc `logf` 0.787 ULP, and UCRT `exp2f` 2.08 ULP, incorrectly rounded on 38 % of
  arguments.
- **Identity:** 0 function-level mismatches between MSVC and GCC (**measured** [prototype]).
- **Sound impact:** the DetMath build nulls against today's MSVC build at −78 to −186 dB,
  correlation 1.000000, with the clean preset bit-identical (**measured** [prototype]).
- **Why DetMath builds the Init tables** rather than committed hex-float tables: both are
  deterministic; DetMath adds no table data and needs no generator step.
- **Per-birth cost: the birth-rate correction.** The prototype judged the kernels' cost
  negligible at "530 births/s"; that number is the pessimistic ScheduleGrain cost in *cycles
  per sample* from the CPU budget table of `grain-engine.md` §8, not a birth rate (the row
  now says so). One review computed
  64,000 births/s from the grain-engine design's voices-per-size formula; the code caps scheduler births at
  one per sample — `Engine.cpp:309-314` caps the voice target at the grain length (48 frames
  at 1 ms), and `Granular.cpp:321` floors every inter-arrival at 1 — so the maximum is
  48,000 births/s, plus onset and manual births. That correction was applied; the 64,000
  figure was not.
- **Kernel cost** (**estimated** from **measured** instruction counts [prototype]): about
  500–550 cycles per birth with jitter, against about 150–250 with newlib (Exp2F 89, LogF 95,
  pan SinCos 136, PowF 186 instructions). At nominal 20 ms grains and 64 voices (about 3,200
  births/s) that is about +20 cycles/sample; at one birth per sample it is about +300
  cycles/sample, with about 500–550 cycles/sample of transcendental birth math in total.

### 3.10 Casts: evidence

- The cast inventory is from [fp-isa], checked against the code.
- The 32-bit fractional-position conversion in `ReadLinear` and `ReadHermite`
  (`Granular.cpp:18,26`) is correctly rounded on x86 for all 2³² inputs, vectorized code
  included (**measured** [fp-isa]).

## 4. Floating-point environment and denormal policy

### 4.1 The guard: evidence

- **Today's guard** ORs FTZ|DAZ into `MXCSR` (`DenormalGuard.h:32`) and FZ into `FPCR` (`:39`)
  or `FPSCR` (`:48`), leaving rounding mode, default-NaN mode and AArch64's `AH` and `FIZ` as
  the host set them. Only `Process` is guarded (`Engine.cpp:394`); `Init`, `Reset` and
  `ClearHistory` (`Engine.cpp:111-234`) compute the window table, smoother coefficients and
  detector tables in whatever environment the calling thread has. The comment at
  `Engine.cpp:36-38` records that the guard "deliberately does not pin" the rounding mode.
- **Consequence:** a host thread in round-toward-zero changed all eight test hashes on x86 and
  on the emulated M7 ([oracle]) and nulled at −26 dBFS against round-to-nearest ([juce])
  (**measured**).
- **The pedal's environment.** libDaisy enables the FPU and configures nothing else. `FPDSCR`
  resets to 0 (round-to-nearest, FZ off, default-NaN off), and the audio DMA interrupt
  handler's FP context is initialized from `FPDSCR`, so an FZ bit set in `main()` never
  reaches the audio callback (libDaisy's sources, the STM32 programming manual PM0253 and
  QEMU's model, [fp-isa]).
- **Branch order.** Today's order (`DenormalGuard.h:9-24`) tests x86 first. MSVC defines
  `_M_X64` under ARM64EC, and a native MSVC ARM64 build reaches the `#error` because MSVC
  defines `_M_ARM64`, not `__aarch64__` ([fp-isa], [host]). In the new order the x86 branch is
  selected by `__SSE2__` or `_M_X64`, after the Arm branches.
- **Ordering.** GCC documents that it may move even `asm volatile` relative to surrounding
  code, and a register write has no data dependence on FP arithmetic ([fp-isa]).
- **`FEAT_AFP`.** Setting `FPCR.AH = 1` switches FZ to x86-style after-rounding detection and
  changes the default-NaN pattern; an OR-style guard inherits whatever `AH` the host set
  ([fp-isa]). Whether Apple cores implement `FEAT_AFP` was not checked; with whole-word writes
  it no longer matters.
- **JUCE.** `juce::ScopedNoDenormals` ORs FTZ|DAZ (`0x8040`) into `MXCSR` on x86, or FZ (bit
  24) into the Arm register, and JUCE's plugin template places it at the top of
  `processBlock`. Hosts may set FTZ as well. No JUCE wrapper calls it on its own behalf
  ([fp-isa], [juce]).

### 4.2 The denormal decision: where the evidence disagreed

Three sources pinned flushing **on**: the prototype's guard kept FTZ/FZ on and forced
round-to-nearest, and its golden hashes were produced that way ([prototype]); the oracle
investigation specified a guard that "pins round-to-nearest and FZ" ([oracle]); the JUCE
investigation specified `MXCSR = 0x9FC0`, FTZ and DAZ on ([juce]). The FP-semantics
investigation recommended flushing **off** everywhere ([fp-isa]). All measured real things;
the resolution follows from the measurements.

1. **FTZ and FZ are different functions.** In 20 M multiplications aimed at the `FLT_MIN`
   boundary, x86 FTZ hardware and an Arm FZ model disagreed on 769,492, every one in the band
   [`FLT_MIN` − 2⁻¹⁵¹, `FLT_MIN`) (**measured** [fp-isa]). Confirmed on the emulated Cortex-M7
   for one constructed product, 2⁻¹²⁶·(1 − 2⁻²⁶): x86 FTZ gives `0x00800000` and the M7 under
   FZ gives 0 (**measured** [prototype]). Only multiply, divide and FMA can land in the band;
   a sum of normal numbers cannot.
2. **The divergence is invisible from x86**, which raises no underflow flag for band results
   ([fp-isa]).
3. **FTZ conflicts with constant folding.** Microsoft documents the default-environment
   assumption, and LLVM folds in IEEE denormal mode. Under FTZ|DAZ, `1e-38f * 0.5f` computed
   at run time gives 0, but the folded constant is `0x003671F7`, a subnormal (**measured**
   [fp-isa]). Which expressions get folded depends on the compiler and its inlining.
4. **Gradual underflow makes the before/after difference a status flag only**, so identity
   holds by construction and emulators need only correct IEEE basic operations.
5. **Rare, not absent.** The flush-on batteries never hit the band: the prototype's 60 s
   tails, the oracle's 50 s tail, and a targeted test in which 0 of 16,384 smoother ramps
   diverged. But the band analysis found exposed states for two real engine coefficients, the
   10 ms smoother and the 8 Hz DC blocker, and signal-dependent products such as
   `c * (x − lp)` with a tiny nonzero `x` cannot be ruled out by enumeration ([fp-isa]). One
   divergence would snap a smoother one sample early, which flips a per-sample bypass gate
   (`PostChain.cpp:263,286,314,364`) and permanently shifts LFO phase and buffer contents.
6. **The x86 CPU argument** (**measured** [fp-isa]): without a flush, IEEE mode is 6–14×
   slower than FTZ in silent tails (default preset: 450.9 against 68.8 ns/sample); x86 pays
   about 18× per dependent operation on subnormals (26.87 against 1.46 ns); with the flush,
   the IEEE tail cost equals FTZ (69.1 against 70.5 ns/sample), and only 0.13–0.25 % of
   silent-tail blocks raise a denormal flag.

**Where the sources agree.** The oracle investigation proposed a CI invariant: on long-tail
vectors, the hash with flushing on must equal the hash with flushing off. The FP-semantics
investigation measured that, with the flush in place, FTZ-forced and IEEE renders are
bit-identical. The profile keeps both: gradual underflow is the shipping environment, and the
invariant is a CI test with flushing forced on through a test-only build option.

**M7 timing sources.** The gating measurement uses the DWT cycle counter (`CYCCNT`). PM0253
states that the FPU is IEEE-754 compliant and that FZ = 0 is "fully compliant"; Arm's FPv4 documentation says no support code is required. No source found
gives the Cortex-M7's cycle cost for subnormal operands ([fp-isa]). The acceptance thresholds
(1.2× silent-tail CPU, 0.5 % of blocks flagged) are the FP-semantics investigation's.

**The fallback: resolved disagreement.** An earlier companion draft named "FZ on the pedal with
the same flush" as the fallback if the M7 proves too slow. The profile rules that out and
widens the flush instead, because "FTZ forced equals IEEE" is empirical, not structural:
subnormal flags still occurred in 49–96 of 38,000 silent-tail blocks with the flush in place,
and the equality was shown on 5 presets × 38 s (**measured** [fp-isa]). Companion §9.2 item 1
now follows the profile's rule.

### 4.3 The flush: evidence

- The feedback tamer's 8 Hz DC blocker first goes subnormal at sample 83,400 and from sample
  92,623 sits at `0x000001DD` (about 6.7e-43) forever; under FTZ the same filter sticks at a
  different *normal* value, `0x056EB438` (about 1.1e-35) (**measured** [fp-isa]).
- The flush sites come from the FP-semantics investigation's working patch ([fp-isa]). The
  allpass sites are the tamer's diffusers and the reverb's input and decay diffusers.
- Without the smoother's epsilon snap, a ramp to 0 crawls through about 5,000 subnormal
  samples. The existing stall snap (`next == value`) stays.
- **Site count** (derived from the code, with every stage active): 6 tamer filter states, 12
  allpass state writes (4 in the tamer, 8 in the reverb), 6 delay-line writes, 4 post-delay
  filter states, 3 reverb filters, up to 8 SVF integrator updates (2 states × 2 channels × 2
  passes of the double-sampled filter, `PostChain.cpp:375-385`) and 8 smoothers: about 45 per
  sample. An earlier count of "about 30" missed the stereo and double-sampled sites.
- **Measured effect** (x86 [fp-isa]): denormal flags in 49–96 of 38,000 silent-tail blocks,
  IEEE cost equal to FTZ cost, FTZ and IEEE renders bit-identical. The patch changed one
  preset's hash (shimmer).

## 5. Engine changes

### 5.6 Output-changing fixes: evidence

- **Mono aliasing:** with mono input (`stereoInput = false`, or a mono bus duplicated as
  `{x, x}`) and the input buffer shared with output 0, 48,000 of 48,000 right-channel samples
  were wrong (**measured** [host], [challenge]).
- **Dither key:** `Tpdf` truncates the counter to 32 bits before scaling (`Engine.cpp:56-58`);
  `RandUnit` was already fixed for exactly this (`GrainMath.h:37-44`). For counters below 2²⁹
  both key formulas give the same 32-bit key, so every golden vector shorter than about 3.1 h
  keeps its hash (derived).
- **Time-parameter clicks:** `post.delay.time_ms` moves the post-delay tap by whole frames, and
  `DelayMs` makes hard splices on clean-delay presets; maximum second difference 0.115 and
  0.68 respectively, against 0.0018 static (**measured** [challenge]).

### 5.7 The block-split bug: the verification in full

**How it was found.** The prototype noticed that its dense-automation preset hashed
differently at block sizes 48, 512 and 7 *within one build*, in both today's code and the
DetMath build, and bisected a trigger of spray + reverse + mark positioning + freeze, inferring
one mechanism ([prototype]). The block-split verification then reproduced the bug on
unmodified `main` at `e86e971`, found three independent causes, and verified a fix
([bugcheck]). Earlier drafts of both design documents prescribed only the first fix part.

**Harness** (`bugcheck/bugcheck.cpp`, built by `bugcheck/CMakeLists.txt` with
`add_subdirectory` of the repository's `dsp/`). It compares the float32 output bits (`memcmp`)
of block sizes 1, 7, 48, 64, 127 and 512 and the mixed pattern 48, 1, 127, 32 against the
block-1 render; every block is clamped to `min(block, remaining)` and split at event frames.
Input: a pluck every 0.3 s from 0.25 s over a −60 dB noise floor. Base preset: delay 250 ms,
grain 90 ms, overlap 0.55, jitter 0.2, mix 1. Freeze runs from frame 97,000 (2.02 s) to 6.0 s;
event frames are on no block grid. MSVC 19.4x through the VS 2022 generator, Release, unless
stated.

**Today's code: 10 of 13 named cases broke** (all **measured**; `out-repo-all.txt`,
`out-repo-matrix.txt`, `out-repo-extra.txt`):

| Case | Result |
|---|---|
| Spray 20 ms + reverse 0.5 + mark + freeze | Every block size and the mixed pattern differ; first differing frame 109,640 (block 7) and 109,340 (block 512); maximum \|diff\| 0.0345 |
| Mark + reverse (no spray, no pitch) | Broken from frame 102,120 to 102,318 depending on block size; 3,070 samples differ at block 512 |
| Mark + pitch +12 st (no spray, no reverse) | Broken from frame 100,475 at every block size; 40,271 samples differ at block 512 |
| Mark + spray 200 ms | Broken from frame 118,913 |
| Mark alone; mark + spray 20 ms | Invariant |
| Control (same parameters, no freeze) | Invariant |
| Contract-#1 parameters (`test_engine.cpp:386-398`) plus freeze, 2¹⁵ ring | Blocks 64, 127 and 512 differ |
| Far margin (2¹⁵ ring, base 678 ms, no freeze) | Only block 512 breaks, from frame 1 |
| Stale mark (default ring, no freeze, one pluck then 90 s) | Blocks 127, 512 and mixed break from 87.5057 s; 1, 7, 48 and 64 agree |
| Re-anchor at the default ring (freeze held 67 s) | Every block size breaks from 66.5423 s (frame 3,194,032), about 139,934 samples |
| Re-anchor on a 2¹³ ring | Breaks from frame 16,165 |
| Live positioning + freeze on a 2¹⁵ ring | Breaks |

**The 32-subset matrix** over {S = spray 20 ms, R = reverse 0.5, M = mark positioning, F =
freeze, P = pitch +12 st}: exactly RMF, SRMF, MFP, SMFP, RMFP and SRMFP broke; the other 26
were bit-identical at every block size. Spray is therefore not required (correcting the
prototype's bisection); a 200 ms spray also triggers the bug, 20 ms did not on this input; and
an onset must arrive after freeze engages.

**Mechanism, from an instrumented copy** (`bugcheck/dsp_probe`; `out-probe.txt`,
`out-probe-extra.txt`) that counts reads where any interpolation tap lands 1 to 512 frames
ahead of the live head:

- **D1, mark + pitch:** birth at frame 98,769 with the pin at 97,000 and the mark at 98,304,
  1,304 frames after the pin. The wrapped distance is 4,194,512, larger than the
  4,194,304-frame ring; it is clamped to the far rail hi = 4,194,240, so the start is 1,705
  frames behind the live head and the first ahead read comes at grain sample 1,704. 39,868
  ahead reads in total.
- **D1, mark + reverse:** the distance is clamped to the reverse far rail of 4,185,600, putting
  the start 6,771 frames **ahead** of the live head. 3,187 ahead reads.
- **D1, spray + reverse + mark + freeze:** 2,961 ahead reads.
- **D2, stale mark:** reverse grains clamp to the far rail and reach the window at grain sample
  4,096.
- **D3, re-anchor cases:** 0 ahead reads but still divergent, which marks D3 as a separate
  mechanism.

**D1 in the code.** `ScheduleGrain` receives only the position anchor (`Granular.cpp:56-57`),
which is the pin while frozen (`Granular.cpp:261`, `:273-274`, `:309`; set at
`Engine.cpp:402`). The ring keeps recording, so the live head is *age* = (W − pin) frames past
the pin. The mark distance is `d = (anchorFrame − m.frame) & mask_` (`Granular.cpp:82`); the
onset detector keeps listening during freeze, so a mark can be recorded after the pin, and its
distance wraps to about one ring length. The attack length is added after the wrap
(`:86-88`), which can push it past the ring. The bounds are computed from the pin with the full
ring length (`:102-104`), and the wrapped distance is clamped or reflected onto the far bound
(`:106-111`), so the start frame `anchorFrame − d` (`:114`) lands just after the pin:

- forward grains above unity pitch start 64 frames after the pin, 1,705 behind the live head;
  the near guard and the grain-length cap (`:92-97`) see the huge wrapped distance and do not
  engage, so the grain overtakes the live head 1,704 samples into its life;
- reverse grains: the far rail is ring − L·(1 + r) − margin (L the grain length, r its rate),
  so the start lands L·(1 + r) + 64 − *age* frames ahead of the live head and the grain reads
  backward through it;
- a large spray: a negative draw moves an unclamped mark position 5 to 407 frames ahead of the
  live head at birth.

Without freeze the pin equals the live head and none of this happens.

**D2 in the code.** `kGuardMarginFrames` = 64 (`detail/Granular.h:14`) is the only far-side
margin, but Pass 1 can be up to 511 frames ahead, because `maxBlockSize` ≤
`kFeedbackDelayFrames` = 512 (`Engine.cpp:118`, `Engine.h:24`). The staleness test
(`Granular.cpp:81`) accepts mark ages up to `mask_`, so a mark nearly one ring old (about 87 s)
positions reverse grains on the far rail. On small rings a base delay near the ring length
breaks block 512 alone.

**D3 in the code.** Re-anchor on wrap is decided once per `Process` call
(`Engine.cpp:405-414`): the pin jumps at the first block boundary after *age* passes three
quarters of the ring, up to B − 1 samples late for block size B, so every later grain
position depends on the grid. Under event splitting every event frame would also become a
decision point, so even an event that changes nothing would change the output.

**Corrected claims.** An earlier version of design §5.7 stated that live positioning alone
cannot reach the bound. That is refuted twice: live positioning + freeze broke on a 2¹⁵ ring,
and D3 breaks any preset whose freeze is held past the re-anchor point at the canonical ring.

**Why the existing tests could not catch it** (`bugcheck/suitecheck.cpp`). The contract-#1 test
claims to run "with the full stochastic feature set active" (`test_engine.cpp:376-378`), but:
no block-split comparison engages freeze — the `Render` helper (`test_engine.cpp:71-110`)
cannot call `SetFreeze`, and the two freeze tests (`:488-535`, `:653-702`) use fixed 256-frame
blocks and check only RMS level; its white-noise render records 0 onsets (**measured**), the
same detector warm-up trap noted at `test_engine.cpp:1094-1096`, so mark positioning and the
onset trigger never activate; and 4,096 frames with a 100 ms delay on a 2¹⁵ ring never
approach the far rail or three quarters of the ring.

**The fix** (`bugcheck/dsp_fix`; `bugcheck/fix.diff`, 354 lines including the new test, about
60 lines of engine change; three parts of about 10 lines each). Part A in the code:

```cpp
if (refFrame != liveFrame) {               // position measured from the pin (frozen)
  bounds.hi -= static_cast<double>(age);   // live-head far rail, in pin coordinates
  if (bounds.hi < bounds.lo) bounds.lo = bounds.hi;  // live rail wins (safety > hold)
}
...
startFrame = (refFrame - lround(d)) & mask_;
```

A frozen live-position grain keeps the near rail measured from the pin, so it stays inside the
pinned window. Part B passes `bufLen > 512 ? bufLen - 512 : 0` as the buffer length; at the
default 87 s ring the lost 10.7 ms never touches the 5 s `DelayMs` range (*calculated*). Part C
makes `frozenAnchor` an in/out pointer and reproduces the old block-1 behaviour exactly
(**measured**: identical hash). Estimated cost: three integer operations per sample while
frozen plus one subtract-and-compare per grain birth, not measured on the M7.

**Results with the fix** (all **measured**):

- All 13 named cases and all 32 matrix combinations are bit-identical across 1, 7, 48, 64,
  127, 512 and the mixed pattern (`out-fix-all.txt`); the same in an MSVC Debug build
  (`out-fix-all-debug.txt`, 30.6 s); the same under GCC 14.2 `-O2` with the CI's library flags
  (`-Wall -Wextra -Werror -fno-exceptions -fno-rtti`), run in Docker `python:3.11` through
  `gcc_check.sh`.
- The instrumented fixed copy (`dsp_fix_probe`) counts 0 ahead reads in every case
  (`out-fixprobe-all.txt`).
- Test suite:

  | Build | MSVC Release | MSVC Debug | GCC 14.2 |
  |---|---|---|---|
  | Repository baseline | 38/38 test cases, 633,228 assertions | 37/37, 632,971 | not run |
  | Fixed copy, with the new regression test | 39/39, 633,297 | 38/38, 633,040 | 38/38, 633,040 |
  | Repository code plus the new test only | fails, 3 assertions in 3 sections | fails, 3 assertions | fails, 3 assertions |

  The Debug build excludes the test that compiles only with `NDEBUG`. The hidden
  `[pending-spsc]` test fails on both, as designed, until the sample-accurate event queue
  lands.
- **Which part fixes what** (variants `dsp_fixA`, `dsp_fixAB`, `dsp_fixAC`): Part A alone fixes
  every mark + freeze case and regression section 1; Part B fixes the far-margin and stale-mark
  cases and section 3; Part C fixes both re-anchor cases and section 2; live positioning +
  freeze on a 2¹⁵ ring needs all three. Part A alone is therefore insufficient.
- **What the fix leaves unchanged** (`hashes-repo.txt` against `hashes-fix.txt`, block 48): all
  26 matrix combinations without both mark positioning and freeze keep the exact same hash,
  including freeze with live positioning at the default ring (F, SF, RF, SRF, FP and others).
  The re-anchor cases now equal the repository's block-1 output.
- **Freeze semantics.** With Part A as delivered (live-head marks, "Variant A1"), frozen
  mark-positioned output equals unfrozen output (MF hash = M hash = `0d723781eadf150c`).
  Pin-eligible marks ("Variant B", `dsp_fixB`, `fixB-over-fix.diff`) give distinct frozen
  output, are invariant everywhere, and pass the suite (MSVC Release 39/39, Debug 38/38, GCC
  38/38).

**The invariant: why an earlier wording was wrong.** An earlier version of design §5.7 stated
the invariant as "no grain born at sample *t* ever reads a frame written at or after *t*". A
forward grain whose read span exceeds its delay legally reads frames written after its birth
(the near guard allows a delay down to L·max(0, r − 1) + margin, `GrainMath.h:71`), and those
reads are block-invariant, because the frame was written at or before the current output sample
under any split. A counter built to the old wording fired 234,036 times on default Strum
settings, 1,768,877 times with a 50 ms delay and 90 ms grains, and 6,128,362 times at +12 st,
while every render was identical across block sizes and the write-ahead counter stayed at 0
(**measured** [review-num]). Strictly, only offsets 1 to 511 can differ between splits;
checking up to 512 matches Part B's exclusion.

**Regression test.** Written and verified in `bugcheck/dsp_fix/tests/test_engine.cpp` after
line 1081. All three sections fail on today's code and pass with the fix, in MSVC Release and
Debug and in GCC 14.2.

**Workaround until the fix lands.** The prototype's battery already used the 48-frame grid
aligned to frame 0, which is why its 32 builds agreed despite the bug.

### 5.8 Restart: evidence

- `Restart`'s scheduler re-arm is `GranularCore::Reset` (`detail/Granular.h:75-84`).
- `Restart` does not exist yet. The equivalent sequence of today's calls, `Reset` +
  `ClearHistory` + counter rebase + tamer buffer clear, reproduced a fresh `Init` bit for bit
  over 1,920,000 frames on all four test presets; without the tamer clear, feedback
  presets nulled at only −51 to −56 dB (**measured** [preset]). The `Reset` code comment
  accepts that "stale content decays through the loop naturally".
- `ClearHistory` takes 0.92 ms on the desktop at 48 kHz (**measured** [host]).
- **The M7 clear time.** The clear covers about 16.7 MiB (17.5 MB): the 16 MiB ring and the
  0.73 MiB post delay. libDaisy runs the SDRAM on a 32-bit bus at 100 MHz (PLL2R 200 MHz
  divided by 2; `src/dev/sdram.cpp:76`, `:80`, and `src/sys/system.cpp`'s PLL2 settings,
  [review-fw]), a 400 MB/s peak, so 17.5 MB takes at least 44 ms even at full bus efficiency
  (*calculated*). The grain-engine design's `ClearHistory` estimate in §9, "~40–80 ms" before
  its 2026-10-05 amendment, was below that floor. Its other figure, about 0.3–0.6 s for the
  whole SDRAM arena (about 63.3 MiB, `grain-engine.md` §7), scales to 79–159 ms for 16.7 MiB.
  The two figures disagreed; the profile's 45–160 ms brackets them, and DWT settles it. On the
  pedal,
  `Restart` runs from the main loop while the audio callback outputs the dry signal with the
  wet path muted.

### 5.9 RNG epoch: evidence

- The FIFO's loop delay is 512 frames whatever the slot phase, because Pass 1 reads slot
  `abs & 511` and Pass 3 writes the same slot. The preset-pipeline probe did reset the counter,
  at frame 480,000; because 480,000 mod 512 = 256, its first FIFO cycle after the reset read
  values only 256 frames old ([preset]). That is why an epoch is used instead of a counter
  reset. Grain lifetimes (`endAbs`) and mark ages also stay absolute, because they measure
  elapsed time. `SaveState`/`LoadState` (`grain-engine.md` §9, contract #8) store `epochStart_`.
- Without restarting the RNG, Spillover renders diverged permanently (−1.3 to −21.6 dB,
  **measured** [preset]). With the epoch alone, and trails, in-flight grains and the scheduler
  phase kept, the output never reconverged within 30 s, even for feedback-free presets
  (**measured** [review-num]; §2.4 above).

### 5.10 LoadPreset: evidence

- `Init → SetParam(all) → Process` (smoothers glide from the `Init` defaults) and
  `Init → SetParam(all) → Reset` (smoothers snap) differ by −56 to −69 dB; with feedback the
  difference never decays (**measured** [preset]).
- A preset applied as a delta over the previous preset's values nulls at only −4.0 to −6.4 dB
  for the whole 40 s render (**measured** [preset]).
- Macro positions stored in a preset are not re-applied at load; the stored leaf values are
  authoritative ([preset]).
- 28 independent `SetParam` stores can straddle a block start and give one block a mix of old
  and new values ([challenge]).

### 5.11 Events: evidence

- Splitting host blocks at event frames is bit-exact across host block sizes: 0 of 576,000
  samples differed for host blocks from 1 to 4,096 frames and for random sizes (**measured**
  [challenge]; also [host], [preset]), for presets the block-split bug does not reach.
- "Any thread" through a single-producer queue is a contradiction ([plan-of-record]).
- ADC noise leaking through as tiny `SetParam` calls would make the pedal drift from the app's
  render ([preset]).
- **Resolved disagreement.** Some sources had wrappers splitting host blocks; others relied on
  the SPSC queue. Resolution: one implementation of splitting inside `dsp/`, with the queue as
  transport.

### 5.12 and 5.13: evidence

- Offering the firmware bundled with the app on a revision mismatch comes from the
  preset-pipeline investigation ([preset]).
- The shared generator comes from the oracle and preset-pipeline investigations ([oracle],
  [preset]). One preset-pipeline probe built its input with `std::sin`/`std::exp`, which would
  not be identical across builds; the integer generator avoids that.

## 6. Verification

### 6.1 Golden suite: evidence

- The existing 10 s test vector fires only 2 onsets for every preset, and only 7 in 60 s
  (**measured** [oracle]).
- Silent tails of at least 120 s exceed the reverb's decay to 1e-38 and so exercise the range
  where subnormals would appear ([oracle]).
- Writing raw output per sample through a WSL `/mnt/c` mount inflated one oracle run to 94 s
  (**measured** [oracle]); hence in-process hashing and WAV files only on mismatch.
- Generator plucks have roughly 3 ms broadband attacks; tonal notes have soft attacks.

### 6.2 CI legs: evidence

- x86 renders at about 60–130× realtime depending on the preset (**measured**, i9-12900K
  [host], [prototype]).
- Runner names: the host matrix uses `macos-latest` (arm64, AppleClang, plus the x86-64 build
  under Rosetta 2), `ubuntu-24.04-arm` (GCC aarch64) and `windows-11-arm` (GitHub's arm64
  Windows image, where the x64 build runs under Prism). The Standalone leg runs the macOS
  universal binary's x86_64 slice with `arch -x86_64`. Companion §3.4 names the two app-side
  jobs `standalone-parity` and `plugin-format-parity`.
- The M7 user-mode leg ran 80 s of audio in 9.8 s, about 8× realtime per core (**measured**
  [oracle]). newlib's own Linux layer does not run under modern QEMU, so the leg links a small
  syscall shim (**measured** [oracle]).
- The headless `--parity-render` mode of the Standalone is from [oracle] and [juce].
- **The plugin parity host's reason.** The Standalone leg never loads the VST3, AU, LV2 or CLAP
  binaries, and JUCE builds each format as its own binary with its own wrapper unit. The
  wrapper obligations live there: 0-sample `process` calls, in-place and mono buffers, the
  exact-equality echo guard against VST3 `setComponentState` (which echoes
  `(double)getValue()` into `setParamNormalized`), state save and restore, and restart on
  transport start ([review-juce]). JUCE's hosting classes cover VST3, AU and LV2; CLAP uses the
  community `clap-validator` plus a minimal CLAP host; `pluginval` at strictness 5 or higher
  runs for VST3 and AU in companion §3.4's plugin job.
- **How far QEMU can be trusted.** QEMU's Arm FP emulation implements the architecture
  reference pseudocode: tininess before rounding, FZ flushing both inputs and outputs, default
  NaN `0x7FC00000`, unfused `VMLA` against fused `VFMA`, `FPDSCR` → `FPSCR` on an exception's
  new FP context, and a `cortex-m7` CPU model with a double-precision FPU (source inspection
  [fp-isa]). The same binary traps with an illegal-instruction signal under
  `QEMU_CPU=cortex-m4` or `cortex-a15` (**measured** [oracle]).
- **QEMU version.** Tested on 10.2.3 through Docker Desktop's binfmt handler ([prototype],
  [oracle]). Ubuntu 24.04's packaged `qemu-user` 8.2.2 is expected to work, because the fix
  for M-profile user-mode programs landed in July 2023 and is inferred to be in 8.1 and later;
  it is untested.
- **One archive, three links.** The firmware links libDaisy startup and the QSPI linker
  script; the user-mode oracle links the shim; the full-system oracle links semihosting startup
  and its own linker script. Object files are byte-reproducible across rebuilds; plain
  `ar rcs` archives are not, because they embed timestamps; `ar rcsD` archives are
  (**measured** [oracle]).

### 6.3 Static audits: evidence

- **MSVC symbols** (**measured** [review-num], `dumpbin /symbols` on the prototype's MSVC
  objects): `Engine` references exactly `_fltused`, `__security_cookie`,
  `__security_check_cookie`, `__GSHandlerCheck` and `__ImageBase`, but `OnsetDetector` and
  `PostChain` also reference the C runtime's `sqrtf` and `sqrt`, because `/fp:precise` guards
  each `sqrtss` with a call for negative inputs.
- **Today's M7 objects** reference `cos`, `sin`, `exp`, `expm1`, `exp2f`, `powf`, `logf`,
  `cosf`, `sinf`, `lround`, `llround` and `sqrtf`; the DetMath build with `-fno-math-errno`
  references none (**measured** [prototype], [fp-isa]).
- The symbol tools are `arm-none-eabi-nm -u`, `nm -u` on Linux and macOS, and
  `dumpbin /symbols` on MSVC.
- **What the fused-instruction patterns match.** Armv7: the fused `vfma`, `vfms`, `vfnma` and
  `vfnms`, while the unfused `vmla`/`vmls` round twice, like separate operations. AArch64: the
  scalar `fmadd`, `fmsub`, `fnmadd`, `fnmsub` and the Advanced SIMD vector and by-element
  `fmla`, `fmls`, `fmlal`, `fmlsl`. x86: every FMA3 form (`vfmadd231ss`, `vfnmsub132pd`,
  `vfmaddsub213ps` and so on) and the older FMA4 forms.
- **Vector FMA on AArch64:** Clang's default contraction emits vector `fmla` in auto-vectorized
  loops (`fp-isa/asm/clang14_apple_default.s:114,116`, `clang14_a64_default.s:151-152`; none
  with contraction off; **measured** [fp-isa]). The regular expressions were tested against
  sample mnemonics. Today's M7 fused-instruction count is 152.
- The `#include <juce` ban under `dsp/` and `firmware/` keeps the engine framework-free
  ([juce]).

### 6.4 Perturbation: evidence

- A hostile host environment changes every hash on today's code ([oracle]).
- **Ring size** (**measured** [oracle]): 10 s renders were identical at 2²², 2²¹, 2²⁰ and 2¹⁹
  for all eight presets; 2¹⁷ changed the strum and freeze presets; at 60 s, 2²¹ differed from
  2²² only for a freeze held 59 s, which re-anchored at 32.8 s.
- A freeze held between 16.4 s and 32.8 s re-anchors at 2²⁰ but not at 2²¹ (*calculated*:
  0.75 × 2²⁰ / 48,000 = 16.4 s; 0.75 × 2²¹ / 48,000 = 32.8 s).
- With 500 ms grains at +24 st in reverse, L·(1 + r) alone is 120,000 frames (2.5 s); with the
  64-frame margin and the 512-frame write-ahead the far-rail strip is 120,576 frames
  (*calculated*).
- **Resolved:** the canonical ring is 2²²; smaller rings are used only for vectors whose
  validity at that exact size a host leg has verified.
- Every negative control diverged in the prototype (**measured** [prototype]).

### 6.5 Full-system emulation: evidence

`mps2-an500` has been QEMU's Cortex-M7 board since QEMU 5.2. `PlanMemory` measured the bulk
arena at 16.73 MiB for a 2²² ring and 8.73 MiB for 2²¹ ([oracle]); a 60 s input buffer would
not fit either, hence on-target generation. The evidence built this ELF but did not run it, so
its run time is unknown ([oracle]).

### 6.6 Hardware-in-the-loop: evidence

- **The scratch-engine alternative** ([oracle]): a scratch `Engine` with a 2²⁰ ring beside the
  live one, so live audio need not stop. Its full cost is about 4.7 MiB of SDRAM (4.73 MiB
  measured by `PlanMemory`) **plus** a second hot arena in DTCM (about 15–21 KiB in the
  design's plan) and a second warm arena in AXI SRAM (129,680 B measured on the desktop, about
  176 KiB in the memory plan of `grain-engine.md` §7), which is significant in 512 KiB of
  AXI SRAM. It would also be valid only for vectors valid at 2²⁰. **Resolved disagreement:**
  the oracle investigation proposed the scratch engine; the preset-pipeline investigation
  muted live audio and restarted the main engine. v1 mutes and reuses, as companion §9.2
  item 6 also decides.
- **libDaisy's USB handlers.** libDaisy's `src/sys/system.cpp`, the object that holds
  `System::Init` and `SysTick_Handler`, also defines the high-speed USB interrupt handlers,
  which reference ST's USB host and device handles from files under ST's SLA0044 licence
  (`src/sys/system.cpp:96-121`, [review-fw]). Every libDaisy firmware, render-mode and HIL
  images included, therefore links ST's USB code until companion §7.2's libDaisy patch lands.
  An earlier version of the profile claimed the render mode needed no USB stack and was
  therefore clear of the licensing question; that was withdrawn.
- The runner hardware estimate (about $60) and erratum 776924 come from [oracle] and [fp-isa].
- Offline rendering at 1 / CPU load gives about 1.3–3.1× realtime (**estimated**), from the
  profile's load in design §7.2: 32–41 % nominal (1 / 0.41 ≈ 2.4, 1 / 0.32 ≈ 3.1) and
  77–78 % pessimistic (1 / 0.78 ≈ 1.3). An earlier version used the 30–68 % load that
  `grain-engine.md` §8 derives without the profile, which gave 1.5–3.3×.

### 6.7 User-facing parity: evidence

- The parity render takes about 3.2–7.8 s for 10 s of audio plus the `Restart` (**estimated**:
  10 s × 0.32 to 10 s × 0.78, from the load in design §7.2; [preset] for the procedure). An
  earlier version gave 3.5–7 s from the pre-profile load.
- The capture log needs 4 channels × 48 kHz × 4 B = 768 KB/s ([oracle]); replay reproduces
  Spillover loads because they are stamped events in the same deterministic stream ([preset]).
- **Resolved disagreement:** an earlier draft had the pedal restart a capture engine. Capture
  now starts at power-on with no `Restart`, because `Init` already leaves the counter at 0 and
  every buffer zero, and a `Restart` would cut the player's sound. Companion §9.2 item 15
  agrees.

### 6.8 Toolchain: evidence

- The local toolchain string (`arm-none-eabi-gcc` 10.3.1 20210824) was checked on the
  development machine; every M7 measurement used it.
- CI today: `.github/workflows/host.yml:69-70` installs the distribution package on
  `ubuntu-latest`, Ubuntu 24.04's GCC 13.2 (package `15:13.2.rel1-2`), and the job only
  compiles (`host.yml:65-77`) ([oracle]).
- GCC 10.3.1 and 12.3.1 for the M7 and GCC 11, 12 and 14 on x86 all produced identical output
  (**measured** [prototype]).
- The toolchain that libDaisy v9 supports was not checked. Naming the pinned toolchain in the
  GPLv3 build instructions comes from [preset].

## 7. Costs

### 7.1 Cost table with sources

| Item | Platform | Cost | Status | Source |
|---|---|---|---|---|
| DetMath | x86 | None measurable: the heavy preset runs at 79–81× realtime against 78–81× with libm | **Measured** | [prototype] |
| Contraction off | x86 | 11–13 % slower than an FMA-contracted AVX2 build; each instance still about 80× realtime | **Measured** | [prototype] |
| Contraction off | M7, static | Inner-loop instruction counts: Hermite read 101 → 111, linear 63 → 68, unity 45 → 49, SVF 99 → 116, post delay 96 → 103, tamer 85 → 88, FFT butterfly 24 → 26, ring write 151 → 157 | **Measured** by disassembly | [prototype] |
| Contraction off | M7, CPU | About +1 % at roughly 16 voices and +4–5 % at 64 voices (worst case about 430 extra FP-issue cycles per sample); part may hide behind SDRAM stalls | **Estimated** from LLVM's Cortex-M7 scheduling model | [prototype] |
| Recursive-state flush | x86 | About zero; a net speed-up in silence against IEEE mode without a flush | **Measured** | [fp-isa] |
| Recursive-state flush | M7 | About 45 sites per sample: 150–250 cycles/sample (1.5–2.5 %) | **Estimated**; sites derived from the code | [fp-isa] |
| DetMath per grain birth | M7 | About +300 cycles per birth (500–550 total against 150–250); +20 cycles/sample nominal, +300 (3 %) at one birth per sample | **Estimated** from **measured** instruction counts | [prototype]; birth rate from the code |
| Code size | M7 | Engine `.text` 27,515 B with kernels out of line (34,791 B inlined) and 0 B of libm, against 19,579 B + 10,130 B of libm today; the firmware executes from QSPI (`grain-engine.md` §7), and libDaisy's QSPI linker script places all `.text` in QSPI and nothing in ITCM (`core/STM32H750IB_qspi.lds:18-50`) | Sizes **measured** (`arm-none-eabi-size`); cache effect unmeasured | [prototype], [review-fw] |
| Full-word guard | All | A few instructions per entry point | **Estimated** | — |
| `Restart` | Desktop / M7 | 0.92 ms / 45–160 ms | **Measured** / **estimated** | [host]; [review-fw], `grain-engine.md` §7 |
| Strict profile on JUCE code | Desktop | About zero CPU | **Estimated** | [juce] |
| CI | — | M7 user-mode leg about 1–2 min per PR; static audits seconds; perturbation job minutes; each app-side parity leg minutes per OS; the whole prototype battery (300 s of audio) took about 5 min locally | Runner **estimated**; local **measured** | [oracle], [prototype] |

**Effort sources** (all **estimated**):

| Work item | Days | Source |
|---|---|---|
| Build profile: target, flags, checks, tripwires, 32-bit refusal, archive | 0.5–1 | [juce] (target and flag), the profile |
| Header hygiene and opaque storage | 1–2 | the profile; the same row is in companion §8.2 |
| Guard rewrite and tests | 1–2 | the profile |
| Flush and smoother snap | 1–2 | the profile; the FP-semantics patch exists |
| DetMath productized | 3–5 | [fp-isa] ("several days"); the prototype exists |
| NaN-free boundary | 2–3 | the profile |
| Block-split fix | 1–2 | the profile; the verified patch exists [bugcheck] |
| Mono aliasing and dither fold | 0.5 | the profile |
| `Restart`, RNG epoch, `LoadPreset` | 3–5 | companion §8.2 (about 1 day plus 2–4 days) |
| Frame-stamped events in `dsp/` | 3–5 | the profile |
| Engine SPSC queue | 2–4 | the profile; the queue is specified in the threading table of `grain-engine.md` §9 |
| Test-signal generator | 1–2 | the profile |
| Golden harness | 1–2 | [oracle] |
| Productizing the M7 oracle | about 1 | [oracle] |
| CI legs | about 1 week | [oracle] |
| Standalone parity and plugin parity host | 2–4 | the profile |

The total, about 28–46 engineer-days, is about 5.5–9 engineer-weeks.

**Overlap with companion §8.2.** An earlier version said about 5–9 days overlapped (the profile
target, opaque storage, `Restart`, `LoadPreset` and the epoch). Companion §8.2 now also marks
the block-split fix, the input functions with the test-signal generator, and the parity legs
as shared, so the overlap is about 10–17 days (*calculated* from companion §8.2's shared rows:
0.5 + 1 + 2–4 + 1–2 + 1–2 + 1–2 + 3–5). Companion §8.2's combined known-work row, about
83–140 engineer-days, counts each item once.

### 7.2 Budget: correction

The CPU budget of `grain-engine.md` §8 puts the nominal case at 2,950–3,700 cycles/sample (30–37 %
of the 10,000-cycle budget) and the pessimistic case at 6,800 (68 %), derived, not measured.
An earlier version of design §7.2 gave about 7,350 cycles for the pessimistic case with the
profile; it omitted the per-birth DetMath cost and undercounted the flush sites. The corrected
figure is about 7,700–7,800 cycles/sample.

### 7.3 Explicit FMA: evidence

- **Prototype placement:** 8 FMAs in the Hermite read and grain accumulation and 30 in the SVF.
  All of these builds produced the same hash (`b76be857…`): MSVC `/arch:AVX2`, inlined; MSVC
  SSE2 calling UCRT's `fmaf`, through both its FMA3 path and its forced software path; GCC 14
  for baseline x86-64, calling glibc's `fmaf`; GCC 14 and Clang 14 for x86-64-v3, inlined; the
  M7 under QEMU, with `vfma` inlined and no libm references (**measured** [prototype]).
- **Oracle placement:** FMAs in the interpolators, the window-table interpolation, the envelope
  morph and the accumulation; seven builds agreed, with 18 inline `vfma.f32` and no calls on
  the M7 (**measured** [oracle]).
- **Throughput recovered on the M7** (static counts): Hermite read 111 → 103, where implicit
  contraction gave 101; SVF 116 → 101, against 99 (**measured** [prototype]).
- **x86 price:** an MSVC SSE2 build ran the all-post-stages preset about 43 % slower (292 ms
  against 204 ms, **measured** [prototype]).
- **Rejected alternative, matching implicit contraction.** GCC happened to fuse the same sites
  on Arm and on x86-64-v3: GCC 10 for the M7 and GCC 11, 12 and 14 for x86-64-v3 all gave hash
  `bd0ea670` ([prototype], [oracle]). Relying on that would tie identity to GCC's internal
  optimization passes, exclude MSVC and AppleClang, and fail with Clang, which fuses different
  sites.

## 8. Resolved disagreements and review corrections

### 8.1 Where the evidence disagreed

Each is set out in the section named; the resolution is the design's.

- **The contract itself** (record §2.1): the preset-pipeline contract, extended.
- **Flush mode** (record §4.2): gradual underflow, with the FTZ-on-equals-off invariant as a
  test and M7 subnormal timing as the gate; FZ on the pedal alone ruled out.
- **LTO** (record §3.4): both measurements right; no LTO on `dsp/` and none in v1 app targets.
- **Pitch-ratio math** (record §3.9): polynomial kernels provisionally, Init-built tables as the
  fallback, decided by DWT at the real maximum of one birth per sample.
- **The block-split bug** (record §5.7): three mechanisms, not one; spray not required.
- **Spillover convergence** (record §2.4): the figure belongs to `Reset` plus the RNG rebase;
  Spillover as specified promises nothing.
- **Input handling** (record §3.7): sanitize live, grid only for pedal-faithful renders.
- **PIC and visibility** (record §3.5): plugin builds only.
- **Event delivery** (record §5.11): one split inside `dsp/`, the queue as transport.
- **Ring size in the oracle** (record §6.4): smaller rings only where validated.
- **Render mode** (record §6.6): mute and reuse, no scratch engine.
- **Capture start** (record §6.7): power-on, no `Restart`.
- **Fused-instruction count.** One feasibility probe counted 148 M7 fused instructions,
  another 152; the challenge confirmed 152 and attributed 148 to a counting difference
  ([challenge]).
- **Onset detection as a cause of divergence** (record §1.2): random grain timing instead.

### 8.2 Corrections made during review

- The block-split section was rewritten from the verification: three mechanisms, the corrected
  minimal trigger, the three-part fix, the write-ahead invariant replacing a wrong one, and the
  three-section regression test (record §5.7).
- DetMath domain holes were found and given edge rules (record §3.9).
- The per-birth cost was corrected from a misread "530 births/s" to one birth per sample, and
  polynomial kernels became provisional (record §3.9).
- The flush site count rose from about 30 to about 45 (record §4.3), and the pessimistic
  budget from about 7,350 to about 7,700–7,800 cycles (record §7.2).
- The Exact-load clear estimate became 45–160 ms, with a 44 ms physical floor (record §5.8).
- The undefined-symbol allow-list became per-toolchain, `-fno-math-errno` became required, and
  DetMath's square roots use intrinsics on MSVC (record §6.3, §3.9).
- The AArch64 fused-instruction pattern gained the vector forms (record §6.3).
- `SetParam` canonicalization became bit tests inside the guard (record §3.7).
- The tripwires were split into a public and a private header (record §3.3).
- The codec conversion question was closed from libDaisy's source (record §2.2).
- The render-mode USB licensing claim was withdrawn (record §6.6).
- The parity legs were extended to every shipped binary, both macOS slices, Prism, and every
  plugin format (record §6.2).
- The implementation plan was placed inside companion §8.1's merged sequence, with the
  *minted* and *published* vocabulary.

### 8.3 Cross-document reconciliation at this revision

- The companion's earlier proposal to align Spillover epochs to 512 frames is withdrawn
  (companion record §5); design §8.3 Q4 now states only that the 256-frame hop grid matters
  and the 512-frame FIFO phase does not (record §5.9).
- Native Windows on Arm is not a v1 target (companion §2.1; phase F of companion §8.1).
  Design §8.3 Q7 is therefore decided for v1, and the guard refuses `_M_ARM64` at compile
  time until phase F adds its branch with a native CI leg (design §3.8, §4.1).
- Rate invariance of the feedback FIFO and the onset detector is deferred to phase F
  (companion §4.2), so design §8.3 Q11 no longer lists it as a candidate for the first
  published revision; design §8.2 risk 7 counts it as a known sound change after v1.
- The companion's wrapper initializes every engine with `maxBlockSize` 512 (companion §4.1),
  and the firmware with 48. Design §2.3 precondition 2 therefore lets `maxBlockSize` differ,
  since it only sizes buffers and bounds `numFrames` (`Engine.cpp:89`, `:117-118`, `:141`,
  `:379-381`); the canonical `EngineConfig` carries 512, which the firmware lowers. An earlier
  wording let it differ only after the block-split fix, which every app render would have
  failed; what must match before the fix is the block grid actually rendered (design §2.1).
- The pedal's offline render speed and the parity-check time are now derived from the
  profile's own load (design §7.2) rather than from `grain-engine.md` §8's pre-profile load
  (record §6.6, §6.7).
- The passages of other documents that the profile made stale were amended in the same change
  (record §1.3), so design §1.3 now records the amendments, and design §8.4 step 10 no longer
  updates those documents.
- The effort overlap with companion §8.2 was updated to about 10–17 days, with the project
  total taken from companion §8.2's combined row (record §7.1).
- The canonical configuration now names the values the companion's wrapper uses
  (`stereoInput` and `ditherRingWrite` true, `looperFrames` 0; companion §4.1) instead of only
  requiring them to be equal on both sides.
- Companion §11 now defers the list of stale passages to design §1.3, and companion §7.6 shows
  exactly the labels of design §2.5, so design §1.3 records the amendments and design §2.5
  says when each label is shown and how emulation is detected.
- Companion §3.3 now requires `-fno-math-errno` on GCC and Clang, as design §3.2 does.
- The source this record calls [challenge] carries the same label in the companion record.
