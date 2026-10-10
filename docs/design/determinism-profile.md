# Determinism Profile — Sample-Identical Engine Output Across Pedal and Desktop

> How `brainscape::Engine` produces **bit-identical float32 output** on the Daisy Seed pedal (any STM32H750 Seed-family module: Seed3, Seed Rev7, Seed2 DFM)
> (Cortex-M7) and every desktop build. This document owns the numeric parity contract, the
> numerics rules, the floating-point (FP) environment, the engine changes identity needs, and
> verification; [companion-app.md](companion-app.md) owns product shape, JUCE build and
> licensing, wrapper hosting, parameters, preset format, device link and the delivery plan.
> Numbers are **measured** (source label, e.g. [prototype]), **estimated** or *calculated*.
> The [evidence record](reviews/determinism-profile-record.md) describes each source (record
> §0) and holds, under this document's section numbers, the measurement detail and the
> evidence conflicts. Code is cited as `path:line` against `main` at `e86e971`; other
> documents are cited by section, because the change that adds this profile also amends them.
> Status: **proposed, v1**; demonstrated on a scratch copy of `dsp/` and emulated Cortex-M7
> code, not on silicon; not implemented.

---

## 1. Purpose and the requirement

### 1.1 The requirement

The owner decided that the companion app is a desktop preset-creation tool whose presets the
engine recreates 1:1 and which uploads them to the pedal; that JUCE is the framework, in one
C++ monorepo; and that "same sound" means sample-identical output between pedal and app. Two
builds of the same C++ do not produce the same bits by default: compilers fuse multiplies and
adds differently, math libraries round differently, CPUs flush tiny values differently, and
host threads can change the rounding mode. This document removes each such source.

### 1.2 Why a tolerance is not enough

Contract #7 of `grain-engine.md` §10 allowed a −120 dBFS null between firmware and plugin
until the 2026-10-05 amendment replaced it with §2.6's text, because no such tolerance can
hold. Random draws are
keyed on the birth sample (`GrainMath.h:40-45`), so moving one grain birth by one sample
decorrelates everything after it: the default preset nulls at −4.8 dB under MSVC
`/fp:contract` against `/fp:precise` [parity-v1], and with today's firmware code under an
emulated M7, jittered presets null against x86 at −1.3 to −5 dB [prototype]. With feedback,
one-LSB ring differences grow (−105 to −111 dBFS and rising [challenge]); only
fixed-schedule presets without feedback pass (all **measured**). Stochastic outputs are
either identical or unrelated, and identity is cheap (§7), so this profile requires
bit-exactness.

### 1.3 Relation to the grain-engine design contracts

- **#7 is superseded:** every conforming build reproduces every golden hash of the current
  sound revision (§5.12, §6.1).
- **#6 is restated:** identity holds across builds, the compiled package is the unit of
  identity, and the exact-restart state (§5.8) replaces the undefined "starting RNG
  counter". §2.6 has the text for #6 and #7.
- **#1 is broken today** (§5.7). Its test must freeze mid-render with the block split at the
  freeze frame, fire onsets (`ConsumeOnsetCount() > 0`), place grains on the far rail (§1.5),
  and hold freeze past the re-anchor point. Other mid-render events arrive with §5.11.
- **#8** stores the RNG epoch (§5.9).

The passages of other documents that this profile made stale were amended on 2026-10-05, in
the same change that adds this document, and now point here. In `grain-engine.md` they
include contracts #1, #6, #7 and #8, the §3 write-head guards and pitch paragraph, the §8
budget, the §9 API listing and threading table, and the §11 Freeze row; `grain-engine.md` §13
lists every amended passage. `STATUS.md` and `README.md` now say that output is identical
only within one build until this profile lands, and that block-size invariance has a known
defect until §5.7's fix lands. Companion §11 lists the passages that the companion design made
stale, which were amended on the same date.

### 1.4 Scope

Out of scope: what companion-app.md owns, and the looper (not designed). Where those touch the
contract, this document states only the requirement.

### 1.5 Terms

| Term | Meaning |
|---|---|
| Contraction | The compiler fusing a multiply and an add into one fused multiply-add (FMA) with one rounding. An explicit `fma` call is not contraction. |
| Subnormal | A nonzero binary32 below `FLT_MIN` ≈ 1.18e-38; keeping them is IEEE *gradual underflow*. |
| FTZ, DAZ, FZ | Modes that flush subnormals to zero: x86 FTZ (results) and DAZ (inputs) in `MXCSR`; Arm FZ in `FPSCR` (M7) or `FPCR` (AArch64). |
| Control word | The per-thread FP mode register; on the M7, `FPDSCR` seeds `FPSCR` for each new interrupt FP context. |
| DWT, HIL | The M7's cycle counter; hardware-in-the-loop CI on a real Seed-family module (the prototype is a Seed Rev7). |
| Golden hash | The checked-in SHA-256 of a render of a fixed test vector. |
| Oracle | A reference run of the engine under emulation whose hashes every other build must match. QEMU *user mode* (`qemu-arm`) runs one Linux-style executable by translating its instructions; *system mode* (`qemu-system-arm`) emulates a whole board, interrupts included. |
| Sound revision; minted, published | `kSoundRevision` (§5.12). *Minted* when its golden file is created, *published* when a build carrying it first leaves the project, *internal* until then. |
| Exact / Spillover load | Loading a preset into a cleared engine, or over the running one, keeping trails. |
| FastCut, Trails | The two mode-switch styles of `grain-engine.md` §5: Trails (the default) lets the old mode's grains finish; FastCut does not. |
| History ring, bulk arena | The engine's 2²²-frame stereo int16 circular buffer of recent input plus feedback, from which grains read (`grain-engine.md` §7). It lives, with the post delay and later the looper, in the *bulk arena*, the SDRAM memory tier. |
| W(n), Pass 1, Pass 2 | `Process` writes the input block into the history ring (`Engine.cpp:457-494`), then renders grains (`:497-498`); W(n) is the ring frame written for output sample *n*. |
| Write-ahead window | Frames Pass 1 has written ahead of W(n) when Pass 2 renders *n*: up to 511. |
| Near rail, far rail | The smallest and largest delays the write-head guards allow: `lo` and `hi` of `ComputeDelayBounds` (`GrainMath.h:64-77`). A grain on the near rail reads just behind the live write head; one on the far rail reads the oldest audio in the ring, which the write head overwrites next as it wraps. |
| Pin, re-anchor | Freeze measures positions from the ring frame where it engaged, the pin; once the live head is ¾ of the ring past it, the pin jumps to the live head. |
| Mark positioning | `POS_MARK`: grains start at the latest onset mark (the Strum modes). |
| Feedback tamer | The filters, saturator and diffusers inside the feedback loop (`PostChain.cpp:88-104`). |
| Abbreviations | Rosetta 2, Prism: Apple's and Microsoft's x86-64 emulators. LTO: link-time optimization. ODR: C++'s one-definition rule; COMDAT: a per-function section of which the linker keeps one copy. GOT: global offset table. SPSC: single-producer, single-consumer. SVF: state-variable filter. ULP: unit in the last place. DTCM, ITCM: the STM32H750's tightly coupled memories; SDRAM: the Seed's 64 MiB external RAM. UART, SWD, semihosting: serial port, Arm debug port, target I/O through the debugger. PendSV, SysTick: two Cortex-M exception handlers, a software-triggered one and the system timer's. x86-64-v3: the x86-64 feature level that includes AVX2 and hardware FMA. CODEOWNERS: GitHub's file that requires named reviewers to approve changes to given paths. |

## 2. The parity contract

### 2.1 Statement

> **Parity contract.** Let two *conforming builds* (§3.8) with the same sound revision each
> restart into the exact-restart state, load the same compiled preset package with
> `LoadPreset(…, Exact)`, and receive the same float32 input frames at 48 kHz and the same
> frame-stamped event stream. They then write **identical float32 bit patterns** to their
> outputs, for every frame, whatever block sizes each side uses.

Tests compare SHA-256 digests of both renders (§6). "Whatever block sizes" holds only after
all three parts of §5.7's fix land. Until then pedal-exact renders use the pedal's grid —
48-frame blocks aligned to frame 0, events only at multiples of 48 — which a real-time plugin
on arbitrary host buffers can keep only by buffering at least 47 frames (*calculated*;
companion §2.3's "pedal grid" option reports 48).

### 2.2 Boundary

The contract covers the float32 samples in `ProcessContext::in` and `out` (`Engine.h:74-82`)
at exactly 48 kHz. Constants counted in frames (the 512-frame FIFO, `Engine.h:24`; the
512/256 onset window and hop, `detail/OnsetDetector.h:30-31`; the guard margin,
`detail/Granular.h:14`) make any other rate a different sound (a 100 ms preset repeats every
110.667 ms at 48 kHz and 105.333 ms at 96 kHz, **measured** [host]).

Outside: the pedal's analog path; host-rate DAW sessions, where only the internal 48 kHz
stream can conform; and the DAW's own processing. On the codec's digital side, libDaisy v9
[review-fw] feeds the engine exactly *i* × 2⁻²³ (24-bit SAI, `postgain` 1 on every Seed;
`src/daisy_seed.cpp:259`, `src/hid/audio.cpp:306`, `src/daisy_core.h:42`, `:138-141`), as
`ConditionInput24` (§3.7) does, while `postgain` stays 1. Its `f2s24` output clamps to
±0.999985 and truncates (`src/daisy_core.h:33`, `:146-151`), so captures log the engine's
float32 output (§6.7).

### 2.3 Preconditions

All are required.

| # | Precondition | Meaning and reason |
|---|---|---|
| 1 | Same sound revision | Same `kSoundRevision` (§5.12) on conforming builds. |
| 2 | Canonical configuration | One `constexpr EngineConfig` in `dsp/` for the firmware and every pedal-exact path: 48000 Hz, `historyFrames` 2²², `stereoInput` and `ditherRingWrite` true, `looperFrames` 0 until the looper exists (companion §4.1), and `maxBlockSize` 512, which the firmware lowers to 48. `maxBlockSize` (≤ 512) may differ because it only sizes buffers and bounds `numFrames` (`Engine.cpp:89`, `:117-118`, `:141`, `:379-381`); until §5.7's fix the blocks actually rendered follow §2.1's 48-frame grid. Ring length reaches the output (§6.4). |
| 3 | Exact-restart start state | `Init` or `Restart` (§5.8), then `LoadPreset(P, Exact)`: counter and epoch 0, every buffer and state zero. Draws are keyed on the counter (`Engine.cpp:56-65`); stale tamer diffusers alone left feedback presets nulling at −51 to −56 dB (**measured** [preset]). |
| 4 | Identical input bits | The same finite float32 values per frame and channel, from the same mono/stereo input-mode rule (companion §4.8); subnormals allowed. |
| 5 | Identical frame-stamped events | Every parameter change, freeze toggle, trigger, tap, mode switch and preset load is an event `(absolute frame, sequence number, type, id, value)` applied at exactly its frame (§5.11); one change applied at block start made 48- and 512-frame renders differ (**measured** [preset]). |
| 6 | Exact plain parameter values | `SetParam` gets the package's exact binary32 values, never via a normalized float or decimal text on the pedal. JUCE's normalize round trip changes 8–20 % of float values in most parameter ranges; in one preset two values moved 1–2 ULP each and broke identity (null −118.5 dBFS; **measured** [juce]; record §2.3). |
| 7 | Profile-conforming execution | Every entry point inside the guard (§4.1), in a build meeting §3. |

The guarantee therefore covers *renders* (preset, input, event script), not arbitrary moments
of a live session.

### 2.4 What is not guaranteed

- **Live playing on the pedal, in either load mode:** analog input and knob timing differ; an
  Exact load only defines the start state. SD capture could later replay a session (§6.7).
- **Spillover loads:** history, grains and scheduler phase carry over; as specified (§5.10),
  output had not reconverged after 30 s even without feedback (**measured** [review-num];
  record §2.4; §8.3 Q10).
- **Real-time plugins on arbitrary buffers before §5.7's fix**, and **DAW automation**, which
  JUCE's VST3 wrapper applies once per block (`juce_audio_plugin_client_VST3.cpp:3495-3540`);
  the app's scripted renders are covered.
- **Plugin bounces without the restart option** (companion §4.9): `Reset` keeps the counter
  (`Engine.cpp:203-228`), and two bounces differed in 265,908 samples (**measured**
  [challenge]).
- **Host-rate sessions** (§2.2), **non-conforming builds** (§3) and **silicon defects** (§6.6).

### 2.5 What the user is promised

One vocabulary, which companion §7.6 shows exactly; there is never a bare "1:1".

- **Same engine (rN):** on connect, the pedal reports the app's revision, so both run the same
  engine under the same profile; nothing has been checked on this pedal yet.
- **Verified 1:1 on rN · firmware ‹git hash›:** a parity check (§6.7) of this slot on this
  firmware matched — the only end-to-end proof on hardware, and what "verified upload" means.
- **1:1 render:** a render from the exact-restart state with an Exact load, at 48 kHz without
  resampling (on the 48-frame grid until §5.7's fix), with the wrapper's reproducible flag
  (companion §4.9), on a build and platform a CI leg covers.
- **Resampled — same preset, not sample-identical:** a 44.1 or 96 kHz session (§2.2).
- **Not identical — update the pedal to rN?** when the pedal's revision is older (the app
  offers its bundled firmware); **Not guaranteed identical — update the app** when it is
  newer. Nothing is certified.

An x86-64 build that detects Rosetta 2 or Prism (macOS `sysctl.proc_translated`; Windows
`IsWow64Process2` or `GetMachineTypeAttributes` reporting native ARM64), on a combination no
CI leg covers, adds "(emulated — verify with PARITY)". Each label is shown with what it does
not cover: live playing, Spillover loads, the analog path.

### 2.6 Replacement text for grain-engine.md §10 #6 and #7

> **6. Preset round-trip and cross-target identity.** `Compile → Serialize → Compile` is
> idempotent. The compiled preset package is the unit of identity. It is produced only on the
> desktop, by `dsp/`'s compiler running under the determinism profile, and compiling the same
> JSON on every desktop CI leg yields byte-identical packages. Loaded with
> `LoadPreset(P, Exact)` from the exact-restart state, one package drives the firmware and
> every desktop build to bit-identical output under the parity contract
> (`docs/design/determinism-profile.md` §2). This holds across builds, not merely within one.
>
> **7. Cross-build equivalence (bit-exact).** Every conforming build (determinism profile §3)
> reproduces the golden SHA-256 of every golden vector for the current `kSoundRevision`.
> Verified per pull request on the host matrix and on the emulated Cortex-M7, nightly under
> full-system emulation, and on hardware before every release. The earlier −120 dBFS
> tolerance is withdrawn.

## 3. Numerics requirements

### 3.1 The operation set

The engine may rely only on what IEEE-754 guarantees bit for bit on x86-64, AArch64 and the
Cortex-M7: `+ − × ÷` and `sqrt` in binary32 and binary64, round-to-nearest-even; comparisons;
integer and bit operations; exact or correctly rounded conversions (float → int only when
proven in range, §3.10); and an explicit `fma` if §7.3 adopts it — in source order, with
gradual underflow (§4) and no compiler fusion or reassociation. Everything else differs
between targets: contraction (§3.2), libm (§3.9), flush modes (§4.2), NaN bits and
propagation (default NaN `0xFFC00000` on x86, `0x7FC00000` on Arm), out-of-range float → int
(**measured** [fp-isa]) and x87 precision (§3.6). Auto-vectorization stays allowed: without
fast-math, lanes do the same IEEE operations and reductions keep their order (**measured**
[fp-isa], [oracle]).

### 3.2 Contraction off, fast-math off: flags per toolchain

Contraction is off in **every translation unit that compiles engine code**. The C++ standard
mode does not achieve this: `arm-none-eabi-g++` 10.3 emitted 152 fused instructions in
`dsp/` under `-std=c++17` and none with `-ffp-contract=off` (**measured** [prototype]).

| Toolchain | Required | Forbidden | Note |
|---|---|---|---|
| MSVC, VS 2022 17.0+ (x64; native ARM64 is not a v1 target, §3.8) | `/fp:precise` or `/fp:strict`; `/arch:AVX2` allowed | `/fp:contract`, `/fp:fast` | Older `/fp:precise` fused on ARM64. |
| GCC (x86-64, AArch64) | `-ffp-contract=off -fno-fast-math -fno-math-errno` | `-ffast-math`, `-Ofast`, `-funsafe-math-optimizations`, `-fassociative-math`, `-freciprocal-math`, `-ffinite-math-only`, `-fno-signed-zeros`, `-ffp-contract=fast`/`on` | Default `fast`. `-fno-math-errno` drops the libm `sqrtf` call behind inline square roots (§6.3). |
| Clang, AppleClang | `-ffp-contract=off -fno-math-errno`, after any `-ffp-model=` | `-ffp-contract=fast`/`on`, `-ffp-model=fast`, `-ffast-math`, `-Ofast` | Default `on`, so Apple Silicon fuses (**measured** [fp-isa]); `-ffp-model=precise` implies `on`. |
| arm-none-eabi-gcc | `-mcpu=cortex-m7 -mthumb -mfpu=fpv5-d16 -mfloat-abi=hard -O3 -ffp-contract=off -fno-math-errno -fno-exceptions -fno-rtti` | As for GCC | Golden under QEMU with 10.3.1 and 12.3.1 (**measured** [prototype]). |

- `cmake/BrainscapeFpProfile.cmake` defines the `INTERFACE` target `brainscape::fp_profile`;
  `brainscape_dsp` links it `PUBLIC`, so every consumer compiles under it.
- JUCE's per-format wrapper units inherit only the shared-code target's interface properties
  (JUCE 9.0.3 `JUCEUtils.cmake:1480-1497`), so `plugin/` links the engine `PUBLIC` there
  (companion §3.3); §6.3 compares every format target's flags.
- `tools/cmake/arm-none-eabi-toolchain.cmake:14` adds `-ffp-contract=off -fno-math-errno`.
- A configure-time check fails if `CMAKE_CXX_FLAGS*` or any target linking `brainscape_dsp`
  carries a forbidden flag. JUCE's recommended LTO flags fall under §3.4.

### 3.3 Source tripwires

The public `dsp/include/brainscape/FpProfile.h`, included by every public `dsp/` header,
holds the `#error` checks, so a consumer built with wrong flags fails to compile. The private
`dsp/src/detail/FpProfilePrivate.h`, included first by every `dsp/` source, adds the
pragmas; in a public header they would change consumers' code generation and protect
nothing, since public headers carry no FP bodies (§3.5).

```cpp
#include <cfloat>
#if defined(__FAST_MATH__)                       // GCC/Clang -ffast-math, -Ofast
#error "brainscape determinism profile: fast-math is forbidden"
#endif
#if defined(__FINITE_MATH_ONLY__) && __FINITE_MATH_ONLY__   // -ffinite-math-only
#error "brainscape determinism profile: finite-math-only is forbidden"
#endif
#if defined(_MSC_VER) && !defined(__clang__) && (defined(_M_FP_FAST) || defined(_M_FP_CONTRACT))
#error "brainscape determinism profile: /fp:fast and /fp:contract are forbidden"
#endif
#if !defined(FLT_EVAL_METHOD) || FLT_EVAL_METHOD != 0
#error "brainscape determinism profile: FLT_EVAL_METHOD must be 0 (no x87)"
#endif
// ---- private header only from here ----
#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF                     // honoured by Clang (measured)
#elif defined(_MSC_VER)
#pragma fp_contract(off)                         // honoured by MSVC, overrides /fp:contract (measured)
#endif
```

MSVC predefines `_M_FP_CONTRACT` under `/fp:contract` and `/fp:fast` (**measured** [juce]); a
self-test (§6.3) confirms the GCC and Clang macros. GCC ignores `#pragma STDC FP_CONTRACT`
and calls its `optimize` pragma unfit for production, so GCC relies on the flag and the CI
audit. The audits, goldens, guard and perturbation job (§6, §4.1) catch what tripwires cannot
see: GCC's contraction mode, Clang's `-ffp-contract=fast`, LTO, libm calls, the host state.

### 3.4 No link-time optimization across the engine boundary

Whether link-time optimization (LTO) inlines strict engine code into a contracting caller
depends on heuristics, and the probes found both outcomes (record §3.4). `brainscape_dsp` is
never built with `-flto` or `/GL` (CMake forbids `INTERPROCEDURAL_OPTIMIZATION` on it), and
v1 app and plugin targets linking it build without LTO; LTO for their own code later requires
the standalone and plugin parity legs (§6.2) to pass.

### 3.5 Engine floating-point code lives only in `dsp/` translation units

When two units compile an inline header function under different flags, the linker keeps one
copy for both callers; with the plugin object first, the engine got the fused result
(**measured** [fp-isa], [juce]). So **no public header contains a floating-point function
body.** To move:

| Header | Inline FP bodies |
|---|---|
| `dsp/include/brainscape/GrainMath.h` | `RandUnit` (40-45), `SemitonesToRatio` (47-53), `ComputeDelayBounds` (64-77), `ClampDelayFrames` (79-85), `ReflectIntoBounds` (92-98), `MakeEnv` (114-130), `EnvValue` (133-140) |
| `dsp/include/brainscape/detail/Smoother.h` | `Smoother::SetTau` (13-16), `Smoother::Next` (18-22) |
| `dsp/include/brainscape/detail/PostChain.h` | `CheapSine` (19-22), `SoftSat` (26-31), `Allpass::Process` (46-52), `DelaySlice::ReadBackLerp` (83-89) |
| `dsp/include/brainscape/DenormalGuard.h` | The guard (inline asm), which becomes private (§4.1) |
| `dsp/include/brainscape/Engine.h:7-10` | Includes the above so `Engine` can hold members by value |

They move to a private `dsp/src/detail/` (the tests that call them,
`dsp/tests/test_engine.cpp:116-187`, get it as a private include path); the other public
headers are clean. `Engine` gets opaque aligned storage
(`alignas(16) unsigned char impl_[kEngineImplBytes]`, `static_assert`ed in `Engine.cpp`), so
it never allocates and still fits DTCM (6,032 B on the M7, **measured** [oracle]). Display
helpers for the app are exported, non-inline `dsp/` functions inside the guard. CI rejects
weak or COMDAT `brainscape::` definitions outside `dsp/` objects and renders in both link
orders (§6.3).

**PIC and visibility.** Plugin builds need both: on Linux a non-hidden `kParamTable` was
shared between two loaded builds (**measured** [host]). The superbuild sets them before
`add_subdirectory(dsp)` in plugin builds only (companion §3.2), never on `brainscape_dsp`,
where they change the M7 archive's code (**measured** [review-pc]). An audit rejects GOT
relocations in the firmware archive (§6.3).

### 3.6 Evaluation method, targets and literals

- `FLT_EVAL_METHOD == 0`, enforced by the tripwire. The M7 (`__ARM_FP` 14) runs binary64 as
  IEEE hardware operations.
- No 32-bit x86 (x87 evaluates in 80 bits): CMake refuses x86 targets with 4-byte pointers.
- All 123 floating literals in `dsp/` convert identically on MSVC and GCC (**measured**
  [fp-isa]). New constants with more digits than their type holds are hex-float literals,
  and CI compares every literal's bits across toolchains.
- Anything serialized or hashed is written field by field, little-endian, never as a struct.

### 3.7 The input and parameter boundary: the engine never sees or makes NaN

NaN bits and propagation differ by ISA, so the rule is **no NaN and no infinity, ever**. They
enter today through non-finite host input, which the dry path copies to the output
(`Engine.cpp:529-530`) and the ring stores as +32767 (`:44-46`); through a subnormal `skew`
such as 1e-40, which passes `SetParam`'s clamp (`:364-365`) and, under gradual underflow,
makes `MakeEnv`'s reciprocal infinite and `EnvValue(0)` NaN (`GrainMath.h:121,126`), reaching
the output with ISA-dependent bits; and through DetMath domain edges (§3.9).

- **`MakeEnv` guard**, mandatory under §4: a leg shorter than 2⁻²⁰ frames is absent,
  `if (!(a >= 0x1p-20f)) a = 0.f`, likewise `dcy`, before `attackEnd`, `decayStart` and the
  reciprocals are derived. Guarding only the reciprocal left a subnormal `attackEnd`, which put
  index 0 inside the attack leg (0) under gradual underflow and past it (1) with flushing
  forced on (**measured**, engine review). The guard changes output only for grains with a leg
  in (0, 2⁻²⁰): their index 0, and a mark-positioned start shifted by under 2⁻²⁰ frames. An
  unchanged golden hash must confirm that the battery has none.
- **`SetParam` canonicalization** (`Engine.cpp:358-367`, shared with `LoadPreset`), written as
  **integer tests on the bit pattern**: exponent all ones (NaN, ±inf) → the descriptor
  minimum; exponent zero (±0, subnormals) → `+0.0f`; then clamp to `[min, max]`. Under DAZ,
  which JUCE's `ScopedNoDenormals` sets, x86 comparisons treat subnormals as zero, and a
  comparison-based rule stored different bits, changing every grain's first sample
  (**measured** [review-num]; record §3.7). `SetParam` also runs inside the guard, and a fuzz
  test requires identical bits under `MXCSR = 0x9FC0` and `0x1F80`. Packages hold canonical
  values, so on the pedal this is a no-op.
- **No value grid:** binary32 is already finite (§3.9), and a grid would change authored
  values.
- **Text** is parsed only on the desktop, by a correctly rounded binary32 parser (companion
  §6.4); the pedal never parses decimal text.
- **Input functions,** exported and non-inline in `dsp/include/brainscape/InputCondition.h`
  and referenced, not restated, by companion-app.md. They work in integers on the bit
  pattern, plus exact conversions, so no flag or FP environment changes them, unmasked
  exceptions included, and they need no guard.
  1. **`SanitizeInput`** maps an all-ones exponent (NaN, ±inf) to `+0.0f` and passes every
     finite value, subnormals included. Every desktop wrapper applies it to every live input
     sample (each plugin format, the Standalone's live monitoring, the oracle harness); the
     firmware does not need it. It sits in the wrapper so the dry path stays a pass-through,
     bit-exact at Mix = 0 and unclipped above 0 dBFS.
  2. **`ConditionInput24`** puts input on the codec's 24-bit grid (§2.2): NaN → 0;
     `d = (double)x * 0x1p23` (exact); clamp `d` to [−2²³, 2²³ − 1], which saturates ±inf;
     `i = (int32_t)(d >= 0 ? d + 0.5 : d - 0.5)`, ties away from zero; return *i* × 2⁻²³.
     Every step is exact and the truncation ignores the rounding mode (never `lrintf`). The
     code computes the same result in integers on the bit pattern: the binary64 steps trap
     where a host has unmasked the denormal-operand or inexact exception (**measured**,
     engine review). It serves offline DI renders, golden vectors from recordings, the
     preset tool's render command and the app's optional pedal-faithful monitoring, never
     the default live path. It is a no-op for §5.13's generator vectors, which are already on
     the grid. The firmware relies instead on `postgain` staying 1 and on bring-up confirming
     the Seed3's 24-bit codec branch. Identity needs only identical bits; the grid adds
     realism.
- **Finite in, finite out:** every finite input, ±`FLT_MAX` included, gives finite output
  and finite state. The final mix saturates at ±`FLT_MAX` (one-sided compares, so NaN still
  reaches the Debug assertion); unsaturated, a dry sample near `FLT_MAX` under a positive trim
  overflowed to ±inf. The onset detector's input is bounded at ±2¹⁶, where its FFT and hop
  energy cannot overflow; unbounded, one sample past about 2e19 latched +inf into the whitening
  memory and stopped onset detection until `Reset` (both **measured**, engine review).
- **Debug assertions:** output samples and smoother states finite once per block; macro and
  expression leaves finite before canonicalization. A CI fuzz test feeds NaN, ±inf and
  subnormal inputs and parameters and requires finite output equal to the sanitized run, over
  several seeds: with one, the overflow above went unnoticed.

> **Update (2026-10-09, the output limiter).** [output-limiter.md](output-limiter.md), a draft for
> the owner's decisions and not built, adds a last step after the final mix's ±`FLT_MAX`
> saturation: a peak limiter with a ceiling of exactly 1.0. Today the pedal's codec clamps every
> over at ±0.999985 (§2.2) and the plugin passes it on, so the two differ above full scale. With
> the limiter the engine's own output stops at 1.0, the same on every target.
>
> It works within this profile's rules:
> - only +, −, ×, ÷, compares and bit operations, with no transcendental per sample;
> - its two release steps are computed once at `Init` with `Exp2D`;
> - its gain never decays toward 0, so §4.3 gains no site;
> - per-sample state only;
> - one-sided compares, so NaN still reaches the Debug assertion, and a final clamp that bounds
>   every limited sample;
> - each channel's ceiling is max(1, |dry term|), and the wet gives way first, by up to 12 dB, so
>   this section's sanitized live dry still passes above 0 dBFS unclipped, and Mix 0 stays the
>   input on hostile input whenever the wet's gain is at −12 dB or above.
>
> A prototype built into revision 7's engine kept 37 of the 45 golden hashes, each identical under
> every block pattern, delivery and the hostile environment, and passed the forced-flush control.
> Its §5 gives the arithmetic.
>
> *2026-10-10:* the owner's answers add a per-preset switch (row 87, `output.limiter`). With it
> Off the over is clamped at the same ceiling, and a switch to Off mid-limiting releases the gain
> to exactly 1 at the fast rate. It is applied through a seventh parameter domain at its event's
> frame, so the output still stops at 1.0 on every target and every rule above still holds (its
> §4.7). *Draft 4, corrected:* the switch is the owner's choice for that decision, made in the
> session from four options (draft 4 first called it a proposal). Whether Off clamps, as above,
> or passes overs is still the owner's question (its §11.5 Q1), with the clamp recommended, which
> keeps the output at the same ceiling on every target; `Reset` primes the limiter before its
> own rebuild, so the switch always sees unity gain after a restart (its §6.1).

### 3.8 Conforming builds and supported targets

> **Update (2026-10-07, Rev7 silicon record).** The Cortex-M7 conforms on silicon too: on the
> owner's Daisy Seed Rev7 the parity image rendered the golden corpus bit for bit at sound
> revision 1 (28 presets) and revision 3 (33 presets, its 18 packages matching `MANIFEST`), each
> at `maxBlockSize` 512 and 48 and with FZ, DN and round toward zero in the caller's FPSCR
> ([the silicon record](reviews/rev7-silicon-record.md) §2).

x86-64 (SSE2 or AVX2; MSVC, GCC, Clang) conforms (**measured** [prototype]), as does the
Cortex-M7 under QEMU (**measured**; silicon pending, §6.6). AArch64 (AppleClang, GCC) is
expected to; the first CI run settles it. Native Windows ARM64 is not a v1 target (companion
§2.1; phase F of companion §8.1), so in v1 the guard refuses MSVC ARM64 at compile time (§4.1).
x86-64 under Rosetta 2 or Prism is expected to conform and is labelled emulated until
a §6.2 leg covers it. Out of profile: 32-bit x86, ARM64EC (its emulated `MXCSR` may not reach
`FPCR`), forbidden flags and LTO across the boundary. A build is **conforming** when it is
compiled per §3.2–§3.6, links no libm transcendental (§3.9), passes the audits (§6.3) and
reproduces every golden hash (§6.1).

### 3.9 DetMath replaces every libm transcendental

| When | Sites and calls |
|---|---|
| Init | `Engine.cpp:174` `cos`; `OnsetDetector.cpp:38,44,47-48` `exp`, `cos`, `sin` |
| Init and Feedback change | `PostChain.cpp:11` (`LpCoef`, also from `:85`) and `detail/Smoother.h:15` `expm1` |
| Parameter change | `Engine.cpp:253`, and `:337,343` → `GrainMath.h:52`, `exp2`; `Engine.cpp:355` `pow`; `PostChain.cpp:215,222,233-234` `sin`, `pow(r, 0.25f)`, `cos`, `sin` |
| Grain birth | `Granular.cpp:69` → `GrainMath.h:52` `exp2`; `:129-130` `cos`, `sin` (pan); `:318` `log` (jitter) |
| Exact, replaced too | `OnsetDetector.cpp:34` `ceil`; `PostChain.cpp:15,112,122,128,151` and `Granular.cpp:114,116` `lround`/`llround`; `Engine.cpp:304,341,343` `lround`, `fabs` |
| IEEE, allowed | `OnsetDetector.cpp:136`, `PostChain.cpp:290-291,315-316` `sqrt` |

Init-time calls go too: with contraction off, today's MSVC and GCC builds still diverge
[juce], an M7 build keeping newlib differs from x86 on 6 of 8 presets [oracle], and compilers
fold constants differently [fp-isa] (all **measured**).

**DetMath** (from the prototype) becomes the private `dsp/src/detail/DetMath.h`: `Exp2D`,
`ExpD`, `Expm1D` (Cody–Waite reduction, binary64 Taylor polynomial), `LogD` (atanh series),
`SinCosD` (fdlibm's π/2 reduction), `SinCosPi`, `PowF(x, y) = exp2(y · log2 x)` (the SVF's
`r^0.25` becomes `sqrt(sqrt((double)r))`), `RoundHalfAwayI32/I64`, `CeilSmall`, `Abs`, and
`SqrtF`/`SqrtD` (`_mm_sqrt_ss`/`_mm_sqrt_sd` on MSVC x64, whose `sqrtf` calls the C runtime). Kernels
compute in binary64 and round once, use basic operations and hex-float coefficients only, and
put **one FP operation per statement**, fixing the order even under a wrong flag. `Init`
builds the window, Hann and twiddle tables with DetMath rather than committing tables.

**Domains.** A domain error is identical everywhere, so goldens cannot catch it; the
prototype's `PowF(0, y)` returned −inf for 1 < y < 2 (record §3.9), and the macro curves of
`grain-engine.md` §5's example mode (exponents 1.5 and 3.0) evaluate at 0. Each kernel has a
stated domain and edge rules:

| Kernel | Domain and edge rules |
|---|---|
| `PowF(x, y)` | Domain: x ≥ 0 and finite. Edge rules: x = 0 gives 0 for y > 0 and 1 for y = 0; x = 1 gives 1. |
| `Exp2D`, `ExpD`, `Expm1D` | Edge rule: the integer exponent is clamped to [−1022, 1023] before scaling; below, the result is 0 (−1 for `Expm1D`); above, the largest finite binary64. |
| `LogD` | Domain: x > 0 and finite. |
| `RoundHalfAwayI32` / `I64` | Domain: −2³¹ < x < 2³¹ (±2⁶² for `I64`), per §3.10. |

The edge rules are part of the function in every build; Debug builds also assert the stated
domains (x ≥ 0 and finite for `PowF`, x > 0 and finite for `LogD`, the `RoundHalfAway`
ranges). A clamp built only into Debug would let Release builds wrap silently and wrongly,
identically on every target, so no golden hash would catch it.

**Accuracy.** Required: identical bits on every conforming target and a maximum error of
1 ULP of the float result over the reachable domain. Target: correctly rounded (0.5 ULP
**measured** for `exp2f` and `logf` [prototype]). Reference tests run exhaustively over
domains of at most 2³² binary32 inputs, otherwise by dense sampling plus endpoints (`PowF`
over the curve-exponent grid, x at 0, 1 and across (0, 1)); they are sampled per pull request
and exhaustive nightly. Identity is a function-level hash on every CI leg. The change
nulls at −78 to −186 dB against today's build (**measured** [prototype]).

**Polynomial kernels or LUT+lerp.** The pitch paragraph of `grain-engine.md` §3 specified a
lookup table for `SemitonesToRatio` before it was amended to follow this section; both are
deterministic, so the per-birth cost decides. The code allows one scheduler birth per sample (1 ms grains are 48 frames, `Engine.cpp:304`; `Overlap` 1 caps
voices at 48, `:309-314`; the inter-arrival floor is 1, `Granular.cpp:236`, `:321`):
**48,000 births/s**. At about +300 cycles per birth over newlib (**estimated** from
**measured** instruction counts [prototype]) that is +20 cycles/sample at nominal 20 ms grains
and **+300 cycles/sample (3 %)** at one birth per sample. *Decided, provisionally:* polynomial
kernels, confirmed or reversed by DWT of `ScheduleGrain` at the maximum birth rate (§8.3 Q2).
The fallback, DetMath-built tables for `SemitonesToRatio` and the pan law, changes output and
would land before the first published revision.

### 3.10 Float-to-int conversions are range-proven

Every float → int conversion in `dsp/` has a documented range and a Debug assertion; all are
in range once §3.7 holds:

| Site | Conversion | Proven range |
|---|---|---|
| `Engine.cpp:46` | `QuantizeS16` | Clamped to ±32767 first; NaN mapped by negated compares |
| `Engine.cpp:304` | `lround(GrainSizeMs·sr/1000)` | [48, 24000] at 48 kHz |
| `Granular.cpp:46` | `env × 4095 → uint32` | Needs finite `env` in [0, 1] (§3.7) |
| `Granular.cpp:95` | `maxOut → uint32` | ≥ 16 and < `total` |
| `Granular.cpp:114` | `lround(d)` | d in [64, 2²⁶] |
| `Granular.cpp:116` | `llround(ratio · 2³²)` | ratio ≤ 4 |
| `Granular.cpp:237` | `target → uint32` | [1, 64] |
| `PostChain.cpp:15,112,122,128,151` | lengths from the sample rate | Bounded by `Init`'s rate check |
| `PostChain.cpp:231` | morph segment | [0, 3) |
| `PostChain.cpp:280` | post-delay tap | [480, 96000] at 48 kHz |
| `detail/PostChain.h:84` | `ReadBackLerp` integer part | Bounded LFO excursion |
| `OnsetDetector.cpp:34` | `ceil(...) → int64` | Small positive |

Converting the 32-bit fractional read position to float (`Granular.cpp:18,26`) follows the
rounding mode on Arm (`vcvt.f32.u32` reads `FPSCR.RMode`), one reason the guard pins
round-to-nearest.

## 4. Floating-point environment and denormal policy

### 4.1 A full control-word guard on every engine entry point

Today `ScopedDenormalGuard` ORs flush bits into the host's word (`DenormalGuard.h:32`, `:39`,
`:48`), keeps its rounding mode (`Engine.cpp:36-38`) and guards only `Process` (`:394`), so
`Init`, `Reset` and `ClearHistory` (`:111-234`) build tables in the caller's environment; a
host round-toward-zero mode changed every test hash (**measured** [oracle]). On the pedal,
`FPDSCR` resets to 0 and seeds the audio interrupt's FP context: already the target [fp-isa].

**Requirement.** Every engine entry point saves the control register, **writes the complete
word**, works, and restores the saved word: x86-64 `MXCSR = 0x1F80` (round-to-nearest,
FTZ = DAZ = 0, exceptions masked, flags clear); AArch64 `FPCR = 0` (round-to-nearest; FZ,
FZ16, DN, AH, FIZ, NEP, AHP clear; traps off); Cortex-M7 `FPSCR = 0` (round-to-nearest,
FZ = DN = AHP = 0, flags clear). The firmware also writes `FPDSCR = 0` at boot, documenting
the dependency.

- **Branch order:** `_M_ARM64EC` (refuse, §3.8); `_M_ARM64` (refuse in v1 with `#error`,
  §3.8; phase F adds `_ReadStatusReg` / `_WriteStatusReg(ARM64_FPCR)` together with a
  native CI leg); `__aarch64__` (`mrs`/`msr fpcr`); `__arm__ && __ARM_FP`
  (`vmrs`/`vmsr fpscr`); x86; otherwise `#error`. Today's order (`DenormalGuard.h:9-24`)
  tests x86 first, which ARM64EC matches.
- **Ordering:** GCC may move `asm volatile` across FP code, so each entry point is a thin
  function that sets the guard and calls a separate `noinline` body, with a `"memory"`
  clobber on the asm and no FP arithmetic of its own.
- **Wrapped:** `Init`, `Reset`, `Restart`, `ClearHistory`, `Process`, `LoadPreset`,
  `SetParam`, every exported helper whose result reaches the engine (tapers, macro fan-out,
  expression curves), and on the desktop the mode and preset compilers.
- A whole-word write also neutralizes host bits such as `FPCR.AH` and JUCE's
  `ScopedNoDenormals`, which is allowed but redundant (companion §4.6 omits it as a style
  rule); an OR-style guard cannot clear bits others set. A hostile environment at every entry
  must reproduce the golden hash (§6.4).

### 4.2 The denormal decision: gradual underflow everywhere

> **Update (2026-10-07, Rev7 bench, sound revision 1).** Both measurements are taken
> ([the silicon record](reviews/rev7-silicon-record.md) §3.7). (a) At FZ = 0 a subnormal costs the
> M7 nothing: dependent `vmul` and `vadd`/`vsub` chains with subnormal operands and results run at
> 3.00–3.01 cycles per instruction, as normal ones do (`vdiv` 18.00 and `vsqrt` 14.00 either way).
> (b) The rule passes on both silent tails: the worst silent-tail block at FZ = 0 is 1.002× that
> at FZ = 1 on the golden tail and 0.999× on 2 s of noise then 120 s of silence through the
> pessimistic configuration at feedback 0.95 (rule: at most 1.2×); 0 of 120,000 blocks raise IDC
> or UFC at FZ = 1 (rule: at most 0.5 %); both golden-tail renders equal the golden hash, and the
> noise tail's FZ = 1 render equals its FZ = 0 render. **Keep gradual underflow**, as decided; no
> wider flush is needed. Measured on revision 1's engine; the bench has not run at a later
> revision.

**Decision:** IEEE gradual underflow on every target (FTZ, DAZ and FZ off), plus a
deterministic in-code flush of recursive state (§4.3). Renders with flushing forced on must
still equal the golden hash (§6.4). The evidence disagreed (record §4.2). Why (**measured**
[fp-isa] unless noted):

1. **x86 FTZ and Arm FZ differ.** x86 decides tininess after rounding and Arm before, so
   results rounding up to `FLT_MIN` stay `FLT_MIN` on x86 and become 0 on Arm (769,492 of
   20 M boundary-aimed products, all in [`FLT_MIN` − 2⁻¹⁵¹, `FLT_MIN`); confirmed on the
   emulated M7 [prototype]), and x86 flags none of them.
2. **FTZ conflicts with constant folding,** which assumes gradual underflow.
3. **Under gradual underflow the difference is only a status flag** (derived from IEEE-754,
   not measured), so identity holds by construction and emulators need only correct IEEE
   basics.
4. **The hazard is rare, not absent:** no battery hit the band, but smoother and DC-blocker
   states are exposed, and one hit would flip a per-sample bypass gate
   (`PostChain.cpp:263,286,314,364`).
5. **The flush removes FTZ's advantage:** x86 silent tails run 6–14× slower in IEEE mode
   without it and cost the same as FTZ with it (0.13–0.25 % of blocks still flag; with
   `SoftSat`'s square flushed too, none do, §4.3).

**Gating measurement.** No source gives the M7's subnormal cycle cost. Measure with DWT on a
Seed (the prototype Rev7 first): (a) a dependent `vmul.f32`/`vadd.f32` chain with subnormal operands and results at
FZ = 0 and 1, against a normal-operand baseline; (b) firmware silent tails (2 s of noise, then
at least 120 s of silence, through feedback presets and every post stage), recording
worst-block cycles at FZ = 0 and 1 and the blocks whose `FPSCR` input-denormal or underflow
flags are set.

**Decision rule** (the single acceptance criterion; companion-app.md refers to it):

- **Keep gradual underflow** if, in (b), worst-block silent-tail CPU at FZ = 0 is at most
  1.2× that at FZ = 1, at most 0.5 % of blocks raise a subnormal flag, and the FTZ-forced
  render equals the golden hash. Total tail time hides the slow blocks: with flags in 0.05 %
  of x86 blocks, the totals matched while the worst block ran 1.16–1.59× slower (**measured**,
  engine review). The absolute budget is a separate gate (§7.2).
- **Otherwise widen the deterministic flush** (more sites or a higher threshold) and measure
  again: a sound-revision bump that keeps identity.
- **FZ on the M7 alone is ruled out** (point 1). FTZ/FZ everywhere would make identity depend
  on coverage and needs this decision reopened.

### 4.3 Deterministic flush of recursive state

> **Update (2026-10-07, Rev7 bench, sound revision 1).** The engine flushes with a bit test
> (`detail/FlushTiny.h`: `|x| < 1e-20f` decided on the bit pattern, the same answer as the two
> compares below for every input). On the M7 it costs 2.02 cycles per site over eight independent
> one-poles and 4.03 on one recursive one-pole, so 91–181 cycles per sample (0.9–1.8 %) at 45
> sites, against the 150–250 estimated below; the two compares cost 7.99–10.15 cycles per site,
> 359–457 per sample (3.6–4.6 %). The bit test is the cheaper form on the M7 too (§8.3 Q5;
> [the silicon record](reviews/rev7-silicon-record.md) §3.8).

Under gradual underflow a decaying one-pole locks onto a permanent subnormal: the tamer's DC
blocker sits at `0x000001DD` from sample 92,623, and at a different normal value under FTZ
(**measured** [fp-isa]). The trajectory depends on the mode, and x86 would pay the subnormal
penalty forever. So, at each state-update site, per sample:

```cpp
if (x < kTiny && x > -kTiny) x = 0.f;   // kTiny = 1e-20f, written as a hex-float literal
```

1e-20 (about −400 dBFS) is far below any LSB here, and the comparisons are exact. The flush
runs **per sample, never per block**, or state would depend on block boundaries (contract #1).

| State | Location |
|---|---|
| Tamer `dcL_/dcR_`, `hpL_/hpR_`, `lpL_/lpR_` | `PostChain.cpp:89-98` |
| `Allpass::Process` state (tamer and reverb diffusers) | `detail/PostChain.h:49` |
| `DelaySlice::Write` (post delay, reverb tank, mod lines) | `detail/PostChain.h:74-77` |
| Post-delay filters `pdDcL_/pdDcR_`, `pdLpL_/pdLpR_` | `PostChain.cpp:296-299` |
| Reverb `rvBandwidth_`, `rvLp_[0..1]` | `PostChain.cpp:317`, `:329-330` |
| SVF integrators `s[0]`, `s[1]` | `PostChain.cpp:380,382` |
| `Smoother::Next`: snap to target when `|next − target| < kTiny` (the stall snap stays) | `detail/Smoother.h:18-22` |
| `SoftSat`'s square of the flushed `lpL_/lpR_`, an intermediate: below 2⁻⁶³ it squares 0, which 27 absorbs to the same bits | `detail/PostChain.h:30-43` |

No flush is needed in the int16 ring, the feedback FIFO (fed by the flushed tamer), the onset
detector (peak floored at 1 % of the frame maximum, `OnsetDetector.cpp:140-146`; the rest
rewritten every hop) or the granular core. With the flush, IEEE-mode silent tails cost the
same as FTZ and match it bit for bit (**measured** [fp-isa]); it changed one preset's hash.
The flushed values themselves reach down to 1e-20, so squaring one can go subnormal:
`SoftSat`'s square flagged every remaining x86 tail block until it was flushed too, and since
then no battery tail block raises a subnormal flag on x86 or on the emulated M7 (**measured**,
engine review), which the forced-flush test requires.
About **45 sites per sample** (derived; record §4.3) cost an **estimated** 150–250
cycles/sample (1.5–2.5 %) on the M7; a cheaper deterministic form is open (§8.3 Q5).

## 5. Engine changes required

**Sound-changing** items alter golden hashes and land before internal revision 1 is minted.

### 5.1 DetMath — sound-changing

Add `dsp/src/detail/DetMath.h` and replace every call in §3.9's table but `sqrt`, so the symbol audit can
require zero libm symbols (−78 to −186 dB null depth, **measured** [prototype]).

### 5.2 Build profile, tripwires and header hygiene

The items of §3.2–§3.6: `fp_profile` and toolchain-file flags, the forbidden-flag check, the
32-bit x86 refusal, no `INTERPROCEDURAL_OPTIMIZATION`, a deterministic `ar D` archive,
plugin-only PIC, the tripwire headers, private FP bodies and opaque `Engine` storage. They
change only builds that contract today (the M7 and Apple arm64, which have no golden yet).

### 5.3 Guard rewrite

Replace `DenormalGuard.h` with private `dsp/src/detail/FpEnvGuard.h` (§4.1); wrap `Init`
(`Engine.cpp:111`), `Reset` (`:203`), `ClearHistory` (`:230`), `Process` (`:375`) and the
entry points of §5.8–§5.11; update `Engine.cpp:36-38` and the guard tests
(`test_engine.cpp:1225-1246`), which check that flush bits are *set*. `QuantizeS16` stays
independent of the rounding mode regardless.

### 5.4 Recursive-state flush and Smoother snap — sound-changing

Add the flush at §4.3's sites. It lands together with §5.3, so that x86 never runs the engine
under gradual underflow without the flush, which would make silent tails 6–14× slower
(**measured** [fp-isa]).

### 5.5 A NaN-free boundary

Add the `MakeEnv` guard (`GrainMath.h:126-127`), the bit-test canonicalization in `SetParam`
inside the guard, and the input functions (§3.7); the DetMath domains and edge rules (§3.9);
the Debug finiteness assertions (§3.7) and the cast range assertions (§3.10); and the CI fuzz
test (§3.7).

### 5.6 Output-changing fixes outside numerics

- **Mono in-place aliasing** (`Engine.cpp:529-530`): the mix writes `outL[n]` before reading
  `inR[n]`, so mono input sharing output 0's buffer corrupts every right sample (**measured**
  [host]). JUCE processes in place; the pedal never aliases. The fix reads both inputs first.
  It is queued separately and must land before the app's parity leg.
- **Dither key truncation** (`Engine.cpp:56-58`): the counter is truncated to 32 bits, so the
  dither repeats every 2²⁹ samples (3.1 h). Fold the 64-bit counter as `RandUnit` does
  (`GrainMath.h:37-44`). Golden vectors keep their hashes (derived; to confirm), but longer
  renders change, so it needs a bump (§5.12), free before internal revision 1.
- **Time-parameter clicks** (the post-delay tap and `DelayMs` splices) are identical on both
  sides, so not a parity defect. *Decided in §8.4 step 8 and landed before revision 1:* the
  post-delay tap glides (`TapGlide`, `detail/PostChain.h`). Two cascaded 50 ms one-poles, a
  critically damped pair, carry a cubic-interpolated (Catmull-Rom) head to the new integer
  target, its speed continuous and capped at 0.5 frames per frame (0.5–1.5× playback), so a
  change bends pitch like tape instead of splicing; a settled head reads the integer tap
  exactly, and a silent stage jumps, also when it re-engages on the frame the time changes. A
  100 ms step that spliced at 190× the static second difference now measures at most the
  glide's own pitch, s² at read speed s (2.25× at the cap), and static settings keep their
  golden hashes (**measured**, record §5.6). Two colourings remain for the owner to judge by
  ear: a moving head loses up to 0.54 dB at 10 kHz (2.5 dB at 15 kHz) between frames, and at
  high feedback every pass repitches the repeats again, so a 2 s → 10 ms throw at 0.9 swoops
  the repeats up nearly three octaves over 4 s before it lands (**measured**, record §5.6).
  `DelayMs` still splices (86× for a 100 ms step, **measured**): that is the grain engine's
  glide feature, not built yet.

### 5.7 The block-split bug: three verified mechanisms — sound-changing

Reproduced on `main` at `e86e971` and fixed on a scratch copy of `dsp/`, comparing output bits
across block sizes 1–512 with every render split at its event frames [bugcheck]; cases and
data are in record §5.7. The `bugcheck/` paths below are under
[`tools/parity/bugcheck/`](../../tools/parity/bugcheck/), committed with this design; §8.4 step 8
applies the fix to `dsp/`.

**The rule the engine never wrote down.** Pass 1 writes the whole block into the ring before
Pass 2 renders grains (`Engine.cpp:457-498`), so the frames just ahead of W(*n*) hold new input
in a large block and one-ring-old audio in a small one. Invariance requires that **no
interpolation tap reads inside the write-ahead window**. The write-head guards as the code
implements them (`GrainMath.h:64-77`, following `grain-engine.md` §3 as it stood before its
2026-10-05 amendment) fail to ensure it three ways:

- **D1, guards measured from the pin** (freeze + mark positioning). While frozen,
  `ScheduleGrain` sees only the pin (`Granular.cpp:56-57`), so a mark recorded after the pin
  wraps to about one ring (`:82`), clamps to the far rail (`:102-111`) and starts just after
  the pin (`:114`): forward grains above unity pitch overtake the live head, reverse grains
  start ahead of it. Minimal trigger: freeze + mark + (reverse or pitch above 0 st), with an
  onset after freeze engages.
- **D2, a far margin smaller than the window** (no freeze). The 64-frame `kGuardMarginFrames`
  (`detail/Granular.h:14`) is less than Pass 1's lead of up to 511 frames. A mark nearly one
  ring old (`Granular.cpp:81`) puts reverse grains on the far rail, and blocks of 127 and 512
  frames diverged from those of 48 after 87.5 s (**measured**): pedal and plugin disagree on
  a static preset.
- **D3, re-anchor at block start** (long freeze, any position source). `Engine.cpp:405-414`
  re-anchors once per call, up to B − 1 samples late; with freeze held 67 s every block size
  diverged from 66.54 s (**measured**).

The contract-#1 test (`test_engine.cpp:376-378`) misses all three: its `Render` helper
(`:71-110`) cannot freeze, its input fires 0 onsets (**measured**), and it is too short.

**The fix: three parts, all required** (about 60 lines, `bugcheck/fix.diff`), using
`liveFrame` (the ring frame Pass 1 wrote at the birth sample), *age* =
`(liveFrame − pin) & mask_`, and a shared constant `kBlockWriteAheadFrames` = 512:

- **A (D1):** `GranularCore::Process` computes the live frame per sample and passes it to
  `ScheduleGrain` and `FireExternal` beside the anchor (`Granular.cpp:56`, `:201`; call sites
  `:261`, `:273-275`, `:309-310`). Bounds from `ComputeDelayBounds` are shifted into the
  anchor's frame: when it is the pin, `hi -= age`, and if `hi < lo` then `lo = hi`. The
  live-head rail thus wins over the near rail (the reverse of `GrainMath.h:75`), because a
  write-ahead read breaks identity while a clamp only changes the sound. Mark distances never
  wrap across the pin. Unfrozen, nothing changes.
- **B (D2):** `inline constexpr uint32_t kBlockWriteAheadFrames = 512;` beside
  `kGuardMarginFrames`; `ComputeDelayBounds` gets the ring length minus 512;
  `static_assert(kFeedbackDelayFrames <= kBlockWriteAheadFrames)` in `Engine.cpp`. Never
  derived from `cfg.maxBlockSize`, because pedal (48) and plugin (512) must clamp alike. It
  costs 10.7 ms of ring, outside the 5 s `DelayMs` range (*calculated*).
- **C (D3):** move `Engine.cpp:405-414` into the sample loop of `GranularCore::Process`:
  `if (frozen && ((live - pin) & mask_) > reanchorAge) pin = live;`, with the anchor passed by
  pointer. It reproduces the old block-1 output exactly (**measured**).

**Freeze and marks** (product decision, §8.3 Q3; both variants verified invariant):
*pin-eligible marks* (recommended) admit only marks at or before the pin — the newest mark in
the 16-entry ring with `((pin - m.frame) & mask_) <= ((live - m.frame) & mask_)`, measured
from the pin, else the base position — so freeze holds the Strum position as the freeze
commitment of `grain-engine.md` §2 intends (`bugcheck/fixB-over-fix.diff`, about 10 lines). *Live-head
marks* (the fix as delivered) leave freeze without effect on mark-positioned grains
(**measured**).

**Results** (**measured** [bugcheck]): every case is bit-identical at every tested block size
on MSVC and GCC 14.2, no write-ahead read remains, the suite passes, and each part is
necessary. Cost: a few integer operations per sample while frozen and per birth
(**estimated**). Parts A and B change output. Not covered: freeze and `SetParam` apply at
block start (`Engine.cpp:399-404`, `:415-421`) until §5.11, and no ring smaller than one block
plus a grain span (`Init` accepts 8 frames, `Engine.cpp:124`) can be invariant, so test rings
stay well above that.

**Invariant and tests:**

1. For every output sample *n* and every tap *f* of every live grain (Hermite *f*−1 … *f*+2,
   linear *f* and *f*+1, unity *f*), `((f − W(n)) & mask)` is **not** in
   [1, `kBlockWriteAheadFrames`]: a Debug assertion in `RenderSpan`, plus an instrumented
   counter that must stay 0 over the golden corpus (the instrumented copy that modelled it is
   described in record §5.7). Reading frames written after a grain's birth
   but by sample *n* is legal (record §5.7).
2. The regression test "block-splitting bit-exactness with freeze and far-rail positions"
   (part of `bugcheck/fix.diff`) splits at the freeze frame and compares {7},
   {32}, {48}, {64}, {127}, {512} and {48, 1, 127, 32} with block 1 by `memcmp`, for (i) the
   contract-#1 parameters (pitch 7 st, reverse 0.3, spray 50 ms, mark positioning, onset
   trigger), freeze at frame 6,001, plucks every 4,800 frames, 24,000 frames on a 2¹⁵ ring;
   (ii) freeze held past the re-anchor point on a 2¹³ ring; (iii) no freeze, base delay
   678 ms on a 2¹⁵ ring.
3. The contract-#1 test requires `ConsumeOnsetCount() > 0` after the detector's warm-up
   (`test_engine.cpp:1094-1096`).
4. Golden vectors cover all three mechanisms under §6.4's block perturbations.

**Interim rule:** until all three parts land, every pedal-exact render uses the 48-frame grid
of §2.1, for every preset (D2 needs no freeze), so both sides read the same ring contents.

### 5.8 Exact restart API

> **Update (2026-10-07, Rev7 bench, sound revision 1).** `Restart` after 2 s of rendering takes
> 22,645,866 cycles, 47.18 ms (an Exact load 47.17 ms, `ClearHistory` 47.13 ms); clearing 16 MiB
> of SDRAM alone takes 44.97 ms, with the firmware's `memset` and with an 8-register `STM` loop,
> the core's floor without DMA. The measurement sits at the estimate's low end: an Exact load
> mutes the wet path for 47 audio blocks of 1 ms. On an engine that has rendered nothing since,
> `Restart` takes 9,279 cycles, less than `Reset`'s 20,487. The watermark is not built, so Q9
> stays open ([the silicon record](reviews/rev7-silicon-record.md) §3.6).

Add `void Engine::Restart() noexcept`, not real-time safe (like `Init`, with `Process`
stopped). It returns a running engine to the post-`Init` state but **keeps parameter values**:
voices, marks and scheduler phase reset (`detail/Granular.h:75-84`); detector reset
(`OnsetDetector.cpp:53-70`); FIFO zeroed (`Engine.cpp:210`); tamer states zeroed
(`PostChain.cpp:75-79`) **and its diffuser buffers cleared**, which today only `Init` does
(`:71-72`); ring and post buffers cleared (`Engine.cpp:230-234`), post-chain state and LFO
phases reset (`PostChain.cpp:195-209`); `sampleCounter_`, epoch and `writeFrame_` zeroed;
freeze off (`frozen_`, `frozenAnchor_`, `freezePending_`); onset and trigger counters zeroed;
parameters drained and smoothers snapped. `Restart` does not exist yet; the equivalent
sequence of today's calls (`Reset`, `ClearHistory`, counter rebase, tamer buffer clear)
reproduced a fresh `Init` bit for bit over 1,920,000 frames on four presets (**measured**
[preset]). `Reset` stays real-time safe and keeps the counter; a DAW's `reset()` maps to it.
On an engine that has rendered no frame since `Init`, `Restart` or `ClearHistory`, `Restart`
skips the ring and post-buffer clears, which would only rewrite zeros: an Exact load before
the first block costs what `Reset` does, so a wrapper may run it on its audio thread.

**Cost.** Clearing about 17.5 MB of SDRAM takes 0.92 ms on the desktop (**measured** [host])
and an **estimated 45–160 ms** on the M7 (floor 44 ms at the 400 MB/s peak of libDaisy's
SDRAM setup, `src/dev/sdram.cpp:76`, `:80`, *calculated*), during which the pedal mutes the
wet path; DWT settles it. **To evaluate before §8.3 Q9, a restart watermark:** record the
restart frame instead of clearing and read 0 for any frame written before it, exactly a
cleared ring's bits; only grains crossing it in the first ring length take a slow path (as
the looper-undo watermark of `grain-engine.md` §7 does), the small post delay (about 2–4 ms,
**estimated**) is cleared or watermarked, and goldens must match the clearing version. A
DMA2D fill would free the CPU, not the bus.

### 5.9 RNG epoch for Spillover loads

Add `int64_t epochStart_`; random keys use `absoluteFrame − epochStart_` at the `RandUnit`
sites (`Granular.cpp:63,70,107,128,301,317`) and in the ring dither (`Engine.cpp:488-489`).
Structural grids stay absolute: the FIFO slot (`Engine.cpp:459`, `:506`), whose phase never
reaches the output but which a counter rewrite would break (hence an epoch, not a reset);
grain lifetimes and mark ages; and the onset hop grid (`OnsetDetector.cpp:165`, counter mod
256), whose phase does reach the output (§8.3 Q4). `Restart` zeroes both, so Exact renders are
unchanged (derived); a Spillover load sets the epoch to its frame. Without it, Spillover
renders diverged permanently (**measured** [preset]). `SaveState` stores it (contract #8).

### 5.10 A single `LoadPreset` entry point with a fixed order

> **Update (2026-10-06, mode-compiler lane 0).** Built with [mode-compiler.md](mode-compiler.md)
> §4.1's per-kind rules, which amend this section when that design is accepted (its §12.5):
> step 1 applies the defaults of the `Leaf` rows only, step 2 counts an id that is not a `Leaf`
> row as unknown, and `Global` rows (device settings) keep their values across every load and
> `Restart`.
>
> **Update (2026-10-07, mode-compiler lane B).** `PresetState` is now the whole decoded
> package (mode-compiler.md §5.1): `soundRev`, the leaves, the `ModeBlob`, CTRL and the stored
> performance state, from `DecodePreset`. Until sound revision 2 (lane C) `LoadPreset` still
> reads only the leaves; validating the mode at load (step 0), the `sinceRev` rule and the
> mode's structure arrive with it (mode-compiler.md §7.3).

> **Update (2026-10-07, mode-compiler lane C, sound revision 2).** `LoadPreset` now reads the
> whole package ([mode-compiler.md](mode-compiler.md) §7.3): a step 0 validates the mode and CTRL
> (`ValidateMode`'s rules, not STAT's, which step 2 canonicalizes and counts) and applies
> nothing when they fail (`LoadReport.invalidMode`); step 2 counts a missing leaf only if its row
> existed at the package's revision (`sinceRev`; 0 or above this build's counts as this build's);
> step 3 copies the mode and CTRL's expression table in at the load frame, one active mode in the
> Warm arena instead of a published ring, and rebuilds every domain; step 4 counts stored
> performance fields this build cannot play (`LoadReport.unsupported`). Rows 27 and 28 are
> retired into the mode, and a change to any row rebuilds what its domain names (R1).

Add `bool Engine::LoadPreset(const PresetState&, LoadMode)`, with `LoadMode` `Exact` or
`Spillover` and `PresetState` the decoded package (companion §6.3). The firmware and every
desktop path call only this, because order matters (gliding and snapped smoothers differ by
−56 to −69 dB, forever with feedback) and delta loads fail (−4.0 to −6.4 dB) (**measured**
[preset]). The order:

1. Every descriptor default.
2. Every stored leaf, canonicalized (§3.7), in ascending ID order. Unknown or missing IDs mark
   the load *inexact*; the caller is told, and the app then shows no identity label.
3. Publish the mode, once modes exist: FastCut for Exact, Trails for Spillover
   (`grain-engine.md` §5); a Spillover blob is staged before its event is stamped (companion
   §6.1).
4. The stored performance state (global reverse, tempo, subdivision). Freeze loads off.
5. *Exact:* `Restart`. *Spillover:* the epoch restarts at the load frame; grains, scheduler
   phase (`intervalRemaining_`) and smoothers are kept, so output never reconverges (§2.4,
   §8.3 Q10).

Stored macro positions are not re-applied; leaves are authoritative. An Exact load is not
real-time. A Spillover load is one event applied at its frame (§5.11), never 28 `SetParam`
stores that could straddle a block start.

### 5.11 Frame-stamped event delivery

An event is `(absolute frame f, sequence number, type, id, value)`. Types: parameter set
(exact binary32), freeze on/off, trigger (source, velocity), tap or tempo, mode switch,
Spillover load, macro move (fanned out to leaves in target-list order through DetMath curves)
and expression. Frames before *f* use the old state, the event applies from *f*, and
same-frame events apply in sequence order: at a block start, exactly today's "`SetParam`,
then `Process`".

> **Update (2026-10-07, mode-compiler lane C, sound revision 2).** The mode switch is a
> Spillover load with a style ([mode-compiler.md](mode-compiler.md) §7.3): the `SpilloverLoad`
> event's `id` is Trails (0, grains finish as resolved) or FastCut (1, grains sounding at the load
> fade to zero over 128 frames), and `LoadPreset` takes the style for a wrapper that splits its
> own blocks. Macro moves (event 4) and expression moves (event 5) are built, fanned out through
> the exported evaluator inside the guard. The corpus checks all three for block-split invariance
> on every leg, the M7 included.

`Process` takes the block's events as in-block offsets and splits internally at each, once, in
`dsp/`; the same split done in a wrapper is **measured** bit-exact across host blocks of
1–4,096 frames [challenge] for presets §5.7 does not reach. This closes three gaps:
`SetParam` accepts a `sampleOffset` and ignores it (`Engine.h:90-96`, `Engine.cpp:358`);
`Trigger` ignores its offset too, and queued manual triggers fire at offsets 0, 1, 2… of the
next block (`Granular.cpp:260-263`); and freeze applies at the next block
(`Engine.cpp:399-404`).

The planned SPSC queue of `grain-engine.md` §9 becomes the transport, carrying the stamp, as
amended in that section's threading table: a single-producer queue cannot serve "any
thread", so each engine has one producer — the pedal's control loop, or the desktop wrapper's
audio thread fed by the wrapper's own queue (companion §4.7) [plan-of-record]; and an
overflow is never coalesced but counted, and a render with a nonzero count is outside the
contract, so scripted renders size queues to avoid it. A stamp below the last one accepted
is refused and counted the same way, since it would apply out of order. `Restart` begins a
new timeline at frame 0, so the queue is cleared with it and producers stamp from the
restarted counter; and a Spillover event's staged preset is reused only once the queue
reports the event retired (companion §6.1).

Freeze is a level, not an edge: the events at one frame leave it on or off, so a release and
a re-engage at one frame keep the pin, as `SetFreeze` between split blocks does. A Spillover
load turns it off at once, so a freeze after the load at its frame pins anew.

On the pedal, pot (after soft takeover), footswitch and MIDI events are stamped with the
48-frame block start where they apply, and can be logged (§6.7); untouched pots emit nothing
(hysteresis). Macro fan-out and expression curves run in `dsp/` on DetMath inside the guard,
relying on `PowF`'s edge rules at 0 and 1 and asserting finite leaves.

> **Update (2026-10-08, CLOCK design pass).** [clock.md](clock.md) specifies the tempo events
> (Tap 6, Tempo 7, ClockTick 8, Transport 9, Subdivision 10) and keeps this section's rules:
> tap averaging and MIDI-clock following run in `dsp/` in integer arithmetic on the frames where
> events applied, and change state only at event frames, so they are block-split invariant
> without internal deadlines: a second without a clock tick is applied before the next event of
> any type, never only before tempo events, which gives the same bits because nothing between
> events reads the clock state, and the snapshots evaluate it without changing it (clock.md §3.5).
> CLOCK grid hits fire at the first frame at or after their boundary, a boundary inside the frame
> before a span's start included (clock.md §6.3). On the pedal every event is stamped a fixed
> number of 48-frame blocks after the block it was captured in (two; five for the tap), still on
> the block grid; the control loop alone pushes, and ring entries from before an Exact load's
> reset are never stamped into the new timeline; a log records each event's applied frame,
> because a late stamp applies at the next block's start and the engine only ever sees that frame
> (clock.md §4.5). The owner confirmed clock.md's decisions on 2026-10-08 (its §11.5), among them
> the gap rule (D2) and the pedal's stamps (D20).

### 5.12 Sound revision constant

Add `constexpr uint32_t brainscape::kSoundRevision` in `dsp/include/brainscape/SoundRevision.h`.

- **It certifies** the `dsp/` sources, the shared constants (`kMaxGrains`,
  `kFeedbackDelayFrames`, `kBlockWriteAheadFrames`, the tier split), the canonical
  `EngineConfig` and this profile's rules and flags, for **every** input.
- **Rule:** any change that *can* change output bumps it; golden coverage is not the
  definition (§5.6's dither fold changes output only after 3.1 h). A **hard trigger** fails a
  pull request that changes a golden hash without a bump (§6.1); a **path trigger**
  (companion §3.4's `sound-rev-gate`) requires a pull request touching `dsp/src/`,
  `dsp/include/`, the CMake files that build the engine (`dsp/CMakeLists.txt`, the root
  `CMakeLists.txt`, `cmake/BrainscapeFpProfile.cmake`), `cmake/fp-forbidden-flags.txt` or the
  arm toolchain file to bump or to carry a CODEOWNERS-approved, justified "sound-neutral"
  label (refactors, comments, tests).
  Firmware-only and app-only changes never touch it.
- **Uses:** it keys the golden file, is recorded in every package and the pedal's handshake,
  and gates "Same engine (rN)" (§2.5). Bumps are cheap before publication (regenerate goldens,
  re-stamp factory packages) and cost users after it, so sound changes are batched.
- **Toolchain ID:** each build embeds its compiler, version, target and a hash of the FP
  flags; the parity reply carries it for triage only (companion §7.4).

> **Update (2026-10-07, mode-compiler lane G).** The gate also holds
> [mode-compiler.md](mode-compiler.md) §8.3's package rule: a golden preset that plays a
> committed package records its `soundHash` and `controlHash`, and a changed render of such a
> preset is excused from the hard trigger only when those changed too and the pull request
> touches none of this section's trigger paths without a bump. A changed package (there, or a
> corpus or factory package's in `dsp/tests/golden/presets/MANIFEST` or
> `firmware/factory/MANIFEST`) needs a code-owner-approved "package-change" label and a
> `Package-change: <cause>` line in the description, bump or not: a compiler or document change
> alters what a document means, not what the engine plays.
> That design amends this section and §6.1 when it is accepted (its §12.5).

> **Update (2026-10-07, revisions per commit).** The gate holds the bump rule per commit, not
> per pull request: each revision is one commit that raises `kSoundRevision` by exactly one,
> and its golden file is minted at that revision, by that commit or a later one before the next
> bump; a pull request may carry several consecutive revisions (step 3 carries r2 and r3, and
> wave 1 one per feature). The gate walks every commit the pull request adds and CI's merge
> commit, and reads each one's revision from the header's one `kSoundRevision = N;` outside
> comments (a commit without the header keeps its parents'). A commit below the highest of its
> parents' revisions, or more than one above it, fails. The revisions introduced above the
> base's must run from the base's + 1 to the head's, each introduced by one commit (two would
> be two sounds sharing a number) and each minted by a commit at that revision: the harness
> keys the file to the revision it was built with, so a file keyed to another revision than its
> commit's mints nothing. A commit that introduces a number the base already has (a parallel
> line's claim) fails until the line is renumbered on top of the base. The gate checks a lower
> revision's golden file by its key and the commit that wrote it, and parity renders only the
> head's; `sound-rev-render.yml` renders every lower revision on every parity leg. For each
> revision below the head's, the gate's `--list-revisions` names every commit at that revision
> that a commit at another revision has as a parent (the next revision's bump, or a merge into
> a later one, so a side line merged after the next bump counts). One job per leg of the
> head's `parity-host` (GCC and Clang on Linux x64, GCC on Linux arm64, MSVC with SSE2 and with
> AVX2, AppleClang on the pinned and the latest macOS), rendering every listed commit, and one
> job per listed commit on `parity-m7`'s runner (the Cortex-M7 under the pinned qemu-arm) build
> each commit's own harness as that commit's own `parity.yml` builds it on the leg and make
> every render that `parity.yml` makes there, against that commit's golden file: what parity
> would have checked had the commit been the head. `tools/ci/parity_plan.py` reads `parity.yml`
> and fails on any shape it does not know, and a commit whose `parity.yml` does not run a leg
> gets the head's renders there. At present that is 48 and 512-frame blocks and the hostile FP
> environment on every host leg, and on Linux GCC also the other block sizes and patterns,
> random sizes, split event delivery and fresh engines; on the M7, 48 and 512-frame blocks,
> {48, 1, 127, 32}, random sizes, the hostile FPSCR, the parity image's stream and the
> forced-flush control (§6.1 asks every x86 leg and the emulated M7 to agree with a golden
> file). The runners and the pinned M7 toolchain are the head's: every golden file must
> reproduce on them too. One job named `sound-rev-render` waits for the listing and every
> render job: it passes when the pull request introduces at most one revision, and fails when
> a listed commit cannot be built or checked on any leg or its golden file is not keyed to its
> revision. It has no `needs`, so its check is pending from the start of every run and a base
> change or a reopen (which keep the head commit) cannot leave an older run's result standing;
> it is the one to require, not the listing or a leg, which a pull request with at most one
> revision skips. While branch protection requires `sound-rev-render`, a pull request may push
> several revisions at once; until the owner requires it, each revision commit is pushed and
> passes parity and host as the pull request's head before the next revision's commit is
> pushed. The head alone still gets `parity-negative-control` (a contracting build must miss
> the golden file), `host.yml`'s tests, and the checks that read no golden file (the static
> audits, the decoder fuzzers, `bspc-roundtrip`), which check the code the pull request lands;
> `parity-summary`'s comparison of the legs gates nothing that each leg's check against the
> same golden file does not, but the negative control. The walk needs the whole history:
> `sound-rev.yml` checks out with `fetch-depth: 0`, and the gate fails on a shallow clone or a
> missing object rather than pass. The rule holds on `main` only for pull requests merged with
> a merge commit; a squash merge would land several revisions as one commit that skips numbers.

### 5.13 Shared deterministic test-signal generator

Add `dsp/include/brainscape/TestSignal.h`: an integer-only, versioned generator of noise bursts
keyed by SplitMix32 (a small integer hash) and of fixed-point plucks and chords, scaled exactly
by 2⁻²³. CI, the firmware render mode and the app's parity check use it, so every platform
derives the same input bits without file transfer.

## 6. Verification

Tests, not reviews, enforce the profile: a change that breaks identity fails CI on its own
pull request.

### 6.1 The golden-hash suite

**Inputs:** generator vectors (plucks, soft notes, chords, dense onset bursts, silence,
full-scale saturation) and a few committed guitar DI recordings on the 24-bit grid, and one
generator vector whose silent samples are subnormals instead (§6.4).

**Presets:** every factory preset, plus coverage presets — maximum delay and spray; ±24 st;
reverse 1.0; 1 ms grains at 64 voices; jitter 1; feedback 1.05–1.1 and feedback decaying to
zero; every post stage at its extremes and a filter-morph sweep; onset triggering with mark
positioning; freezes shorter and longer than 0.75 × ring with onsets during them, with
reverse and with pitch above 0 st; a reverse mark aging past one ring, and far-rail
positions; dense automation off the 48-frame grid, including freeze, triggers and macros;
Exact and Spillover loads mid-render; silent tails of at least 120 s.

> **Update (2026-10-07, mode-compiler lane C, sound revision 2).** The corpus now loads its
> structured presets as committed packages, compiled by `bspc` (`dsp/tests/golden/presets/`), and
> records each package preset's `soundHash` and `controlHash`, which check mode compares (the
> package rule's tie, [mode-compiler.md](mode-compiler.md) §8.3); revision 2 adds macro and
> expression moves, mode switches in both styles and the wet kill, with their counters and
> ablations (that design's §10.3 as built).

**Coverage counters** per vector and preset: births, onsets, steals, reverse and
mark-positioned births, re-anchors, far-rail clamps, frames with feedback above 1, blocks with
an underflow flag, events applied, and the write-ahead counter (which must be 0). Lost coverage
fails CI even when hashes match (today's 10 s vector fires 2 onsets, **measured** [oracle]).
The Linux Debug leg also enforces gcov coverage of `Granular.cpp`, `OnsetDetector.cpp` and
`PostChain.cpp`. *Revision 1 (§8.4) has neither the engine-side counters nor the gcov
thresholds.* Its harness counts what it sees from outside the engine (18 counters: events,
onsets, frozen onsets and frames, frames with feedback above 1, triggers, loads, restarts,
active and subnormal output, among others), requires per-preset minimums of them, and proves
each preset's features with ablations, which must change its output and not before the
feature acts. The write-ahead invariant is a Debug assertion instead of a counter, run over
the whole corpus by every Debug `golden_check`. Births, steals, reverse and mark-positioned
births, re-anchors, far-rail clamps and blocks with an underflow flag are not counted.

**Hash:** SHA-256 of the interleaved little-endian float32 output plus one per second, computed
in process; WAV files are written only on mismatch.

**`dsp/tests/golden/golden.json`:** `soundRevision`; `sampleRate`, `stereoInput`,
`ditherRingWrite`, `historyFrames`; per vector, `generatorVersion`, `inputHash` and its valid
ring sizes (§6.4); per preset, the hash, per-second hashes and counters.

**Rules:** one golden for all targets; a hash change bumps `kSoundRevision` and regenerates
through the script; a golden is minted only when the x86 and emulated-M7 legs agree in one CI
run; the file has CODEOWNERS review; goldens minted before revision 1 is published are
internal. *Revision 1 deviates:* there is no mint job, so it was minted locally on MSVC (GCC 11
and 14, Clang 14 and the emulated M7 minted byte-identical files), and the pull request that
carries it must pass every x86 leg and the emulated M7 in check mode against the committed
file in one CI run: the same agreement, checked after minting instead of before.

### 6.2 CI legs

| Leg | When | What |
|---|---|---|
| Host matrix | Every PR | Ubuntu x64 GCC and Clang; MSVC x64, SSE2 and AVX2; macOS arm64 AppleClang, and the x86-64 build under Rosetta 2; Ubuntu arm64 GCC; the x64 build under Prism on `windows-11-arm`. |
| M7 user mode | Every PR | The archive with the pinned toolchain and firmware flags, a syscall shim, `qemu-arm` with `QEMU_CPU=cortex-m7`: about 8× realtime per core (**measured** [oracle]), 1–2 min per PR (**estimated**). |
| Audits; perturbation and negative controls | Every PR | §6.3, §6.4 |
| `standalone-parity` | Every PR, once the app exists | The shipping Standalone renders the golden set headless on Linux x64, Windows x64 (native and Prism) and both macOS slices. |
| `plugin-format-parity` | Every PR, once the plugin exists | `tools/plugin-parity-host` loads each format binary, restores a golden package via `setStateInformation`, and runs `processBlock` in place at 48 kHz with blocks {0, 1, 37, 441, 513, 1024, 4096, random}, mono→stereo and stereo, toggling the transport; §5.7's cases included. |
| Full-system M7; exhaustive DetMath accuracy | Nightly | §6.5; §3.9 |
| Toolchain drift (non-blocking) | Nightly | Distribution GCC 13.2 and the newest host compilers; a mismatch is a profile bug, not a golden change. |
| HIL | Tags and nightly, after bring-up | §6.6 |

The static audits take seconds per pull request, the perturbation job minutes, and each
app-side parity leg minutes per OS (all **estimated**); the full-system leg's run time is not
yet measured (§6.5). Companion §3.4 names the jobs. QEMU follows Arm's FP pseudocode (source
inspection [fp-isa]) and traps instructions outside the M7's feature set (**measured**
[oracle]), but models no timing, errata or caches; a QEMU arithmetic bug slips through only
if it matches x86. QEMU was tested at 10.2.3; Ubuntu's 8.2.2 is untested (§8.3 Q6), with a
pinned `qemu-user-static` as fallback.

**One archive, three links.** `libbrainscape_dsp.a`, built once per toolchain with `ar rcsD`
(plain `ar rcs` embeds timestamps, **measured** [oracle]), links into the firmware and both
oracles; CI asserts one SHA-256 across all three. Builds with `-g` use `-ffile-prefix-map`.

### 6.3 Static audits

- **Undefined symbols** in `dsp/` objects, per toolchain (`nm -u`, `dumpbin /symbols`), never
  libm. Allowed: `memset`, `memmove`, `memcpy`; on arm-none-eabi, `__aeabi_*` integer helpers
  (including `__aeabi_d2lz`, `__aeabi_l2d`) and `__assert_func` in Debug; on MSVC, `_fltused`,
  `__security_cookie`, `__security_check_cookie`, `__GSHandlerCheck`, `__ImageBase` and, in
  Debug, `_wassert` (**measured** [review-num]); on GCC and Clang, nothing more.
- **Fused instructions** in `dsp/` objects: 0, or a checked-in count per object if §7.3 is
  adopted, matched over the disassembly by Armv7 `\bvfn?m[as]\b` (unfused `vmla`/`vmls` are
  allowed), AArch64 `\bfn?m(add|sub|la|ls)\b` plus `\bfml[as]l2?\b` (Clang emits vector
  `fmla` in vectorized loops, **measured** [fp-isa]), and x86
  `\bvfn?m(add|sub|addsub|subadd)(132|213|231)?[ps][sd]\b`. Each pattern's self-test requires
  a nonzero count on a known contracting kernel.
- **Flags:** the configure check; a self-test compiling with each forbidden flag and
  expecting failure; every format target's flags compared with the shared-code target's
  through `compile_commands.json` (companion `symbol-scan`).
- **Literal bits** compared across toolchains (§3.6); `#include <juce` forbidden under `dsp/`
  and `firmware/`.
- **ODR:** no weak or COMDAT `brainscape::` symbols outside `dsp/` objects; identical renders
  in both link orders.
- **No GOT relocations** in the firmware's `dsp/` archive (`arm-none-eabi-objdump -r`; run in
  companion's `firmware-elf-audit`).

### 6.4 Perturbation and negative controls

These must still reproduce the golden hashes:

- a hostile host environment before every entry, `SetParam` included (x86 FTZ|DAZ, Arm FZ|DN,
  each with round-toward-zero);
- flushing forced on inside the guard by a test-only option (FTZ|DAZ, or FZ on the M7 leg;
  on the host, `brainscape_golden_flush --force-flush-control`, the `golden_forced_flush`
  test). This control holds for golden vectors on the 24-bit grid, not for every canonical
  input: DAZ zeroes a subnormal dry sample, and the golden script rejects a preset with
  any nonzero parameter value below 2⁻²⁴ in magnitude. A tiny gain times a tail-level signal
  is subnormal: Mix = 1e-30 flagged 21.6 % of tail blocks and changed the forced render, while
  every gain at 2⁻²⁴ left none flagged and the renders identical (**measured**, engine
  review). Parity itself holds for any canonical value, since every conforming build runs
  gradual underflow. So one vector, `plucks_subnormal_6s`, is off the grid on purpose: its
  silent input samples are subnormals and its presets must output subnormals, which no
  flushing mode can produce. Forced flushing must change it, and its hostile-environment
  renders, the M7's included, fail when the guard lets a caller's flush bit through; without
  it a guard that kept the caller's FTZ|DAZ reproduced every preset (**measured**, adoption
  review);
- block sizes {1, 7, 32, 37, 48, 64, 127, 512}, the pattern {48, 1, 127, 32}, random sizes
  1–512 and host blocks up to 8,192 through the wrapper's chunker, split at event frames —
  after §5.7's fix; until then the 48-frame grid only;
- ring sizes in each vector's validity set. Candidates: 2²² (canonical), 2²¹ (§6.5), and
  2²⁰ only if a scratch render engine is ever adopted. A size joins a vector's set only when
  its hash equals the 2²² hash, never by inference (a freeze held 16.4–32.8 s re-anchors at
  2²⁰ but not at 2²¹). Ring length reaches the output only through the re-anchor, mark
  staleness (`Granular.cpp:81`) and the far guard (`:102-104`, with Part B), so a vector is
  valid at R when every freeze ends before age 0.75 × R, every positioning mark is younger
  than R − 1 frames, and every grain keeps *d* + *age* + L·(1 + r) + `kGuardMarginFrames` +
  `kBlockWriteAheadFrames` < R (*d*: base delay, or mark age plus attack offset, plus maximum
  spray; *age*: freeze age; L: grain length; r: rate). The derivation explains; the render
  decides (record §6.4);
- `dsp/` and plugin objects linked in swapped order.

These must fail (each diverged in the prototype, **measured** [prototype]): a contracting build
(GCC `-ffp-contract=fast -march=x86-64-v3`, or MSVC `/fp:contract /arch:AVX2`), and one DetMath
function swapped back to libm. A control that passes means lost coverage, and CI fails.

### 6.5 Nightly full-system emulation

`qemu-system-arm -M mps2-an500`, QEMU's Cortex-M7 board, has 16 MiB of PSRAM: too little for
the 16.73 MiB bulk arena at a 2²² ring, enough at 2²¹ (**measured** with `PlanMemory`
[oracle]). It runs 2²¹-valid vectors with input generated on the target, calls `Process` from
an exception handler (PendSV or SysTick) with `FPDSCR` set to round-toward-zero, FZ and DN, as
the pedal's audio interrupt starts each FP context from `FPDSCR`, and reports hashes over
semihosting. The image is built but not yet run.

### 6.6 Hardware-in-the-loop

> **Update (2026-10-07, Rev7 silicon record).** The first sessions ran on the owner's Rev7 over
> USB serial, not yet on a HIL runner. The parity image matched the golden corpus at revisions 1
> and 3 in three configurations each, rendering at 2.66× realtime at revision 1 and 2.57× at
> revision 3 (estimated below at 1.3–3.1×), and the DWT bench ran at revision 1. The renders run
> in the main loop, so the interrupt `FPDSCR` (§6.5) is still untested on silicon, as are the
> SDRAM march test, the hot soak and the errata review
> ([the silicon record](reviews/rev7-silicon-record.md) §2).

This starts after bring-up. The **firmware render mode** ships in the production image; in
v1 it mutes live audio and reuses the live engine with the full 2²² ring (companion §9.2 item
6), since a scratch engine costs about 4.7 MiB of SDRAM plus a second DTCM and AXI SRAM arena,
significant in 512 KiB of AXI SRAM (record §6.6). It renders generator input and reports
hashes over UART or semihosting through SWD, needing no USB class code, though like
every libDaisy image it links ST's SLA0044 USB code until companion §7.2's patch lands
(`firmware-elf-audit` runs on every image). A self-hosted runner with a Seed-family module (a Rev7 first; every module model that ships needs its own leg) and an ST-Link
V3 (about $60, **estimated**) flashes CI-built release firmware, compares hashes on tags and
nightly, and runs the DWT measurements. Only silicon shows FPU errata (for example
`VDIV`/`VSQRT` loss under lazy stacking, Arm erratum 776924 on Cortex-M4F; check the STM32H750
and Cortex-M7 errata), cache and DMA coherency, and the real interrupt `FPDSCR`. Silicon also
owns SDRAM retention: the 16 MiB history ring lives there, so a refresh or retention error at
enclosure temperature would silently break identity. The HIL plan adds an SDRAM march test and
a hot soak (the parity image rendered with the board at its rated temperature), and bring-up
decides whether Brainscape overrides libDaisy's SDRAM refresh count
([hardware-supply-2026-10.md](../research/hardware-supply-2026-10.md)). The pedal
renders offline at about 1.3–3.1× realtime (**estimated** as 1 / load from §7.2's 32–41 %
nominal and 77–78 % pessimistic load).

### 6.7 User-facing parity

**Parity check** (transport and UI: companion §7.4). The pedal mutes live processing, runs
`Restart` and `LoadPreset(…, Exact)`, renders a generator vector through the requested input
mode with a scripted event stream, and returns SHA-256 per segment; the app renders the same
offline at 48 kHz on a fresh engine and compares, and the first differing segment locates the
divergence. It takes about 3.2–7.8 s per 10 s of audio plus `Restart` (**estimated** from
§6.6's render speed); the user's history is lost. A pass earns "Verified 1:1".

**Later, capture and replay.** After v1 and the event queue, the pedal logs post-codec input,
float32 engine output and stamped events to microSD from power-on (`Init` already is the
exact-restart state; 768 KB/s, *calculated*), and the app replays the session from `Init`,
Spillover loads included: the only way to verify live use. The load mode never makes live
playing comparable (§8.3 Q9).

### 6.8 Toolchain pinning

Every M7 measurement used GNU Arm Embedded Toolchain 10.3-2021.10, while CI today installs
Ubuntu's GCC 13.2 (`.github/workflows/host.yml:69-70`). Other GCC versions gave identical
output (**measured** [prototype]), but that is tested, not proven. **Decision:** pin
10.3-2021.10, checksum-verified, for release firmware, oracle and golden minting; revisit at
bring-up (§8.3 Q6); GCC 13.2 becomes the drift leg; pin QEMU likewise; require MSVC ≥ VS 2022
17.0 and log host compiler versions. The GPLv3 build instructions name the pinned toolchain.

## 7. Costs and the explicit-FMA option

### 7.1 Costs and effort

> **Update (2026-10-07, Rev7 bench, sound revision 1).** Code placement is measured: with the
> engine's code (and `mem*`) executing in place from QSPI instead of ITCM, the mean block costs
> 1.045–1.210× as much with warm caches and 1.259–1.697× with cold ones, and the nominal row's
> worst warm block reaches 112.5 % of the budget (99.1 % from ITCM). The requirement below stands,
> and the images meet it ([the silicon record](reviews/rev7-silicon-record.md) §3.4).

On x86, DetMath costs nothing measurable and contraction-off 11–13 % against a contracted
AVX2 build, still about 80× realtime (**measured** [prototype]). On the M7, contraction-off
adds 3–17 % to inner-loop instruction counts (**measured**) and an **estimated** 1 % CPU at 16
voices, 4–5 % at 64 [prototype]; the flush (§4.3), DetMath per birth (§3.9) and `Restart`
(§5.8) are costed in their sections; the guard is a few instructions per entry
(**estimated**). Engine `.text` is 27,515 B with no libm, against 19,579 B plus 10,130 B of
libm today (**measured** [prototype]).

**Code placement.** libDaisy's linker script puts all `.text` in QSPI flash and none in the
64 KiB ITCM (`core/STM32H750IB_qspi.lds:18-50` [review-fw]), so code size competes for the
16 KiB instruction cache. **Requirement:** the inner loops (grain render, tamer, post chain,
flush sites) go in ITCM through a custom linker section, and per-birth kernels are sized
against the cache (effect unmeasured; DWT).

**Effort** in engineer-days, all **estimated** (sources in record §7.1): build profile 0.5–1;
header hygiene 1–2; guard 1–2; flush 1–2; DetMath 3–5; NaN-free boundary 2–3; block-split fix
1–2; aliasing and dither 0.5; `Restart`, epoch and `LoadPreset` 3–5; events in `dsp/` 3–5;
engine SPSC queue 2–4; generator 1–2; golden harness 1–2; M7 oracle about 1; CI legs about 5;
standalone and plugin parity legs 2–4. **Total about 28–46.** About 10–17 of these days
(profile target, opaque storage, `Restart`, `LoadPreset` and epoch, input functions and
generator, block-split fix, parity legs) are also rows of companion §8.2; for the project use
its combined row (about 83–140 days), not the sum. Unsized: bring-up and the DWT pass, the
mode compiler, factory curation, the GUI.

### 7.2 Effect on the grain-engine CPU budget

> **Update (2026-10-07, Rev7 bench, sound revision 1).** Measured, and **the budget is not met**.
> With the engine's code in ITCM and warm caches, the nominal row costs 7,774 cycles/sample on
> average (77.7 %) and 9,915 in its worst block (99.1 %; 10,028, 100.3 %, with cold caches); the
> pessimistic configuration costs 9,062 on average and 13,554 at worst with 20 ms grains (90.6 %,
> 135.5 %) and 11,958 and 16,847 with 1 ms grains (119.6 %, 168.5 %), against the 32–41 % and
> 77–78 % estimated below; the corpus's `dense_1ms` peaks at 118.6 %. Of the costs this section
> adds, the flush is 0.9–1.8 % (§4.3) and subnormals cost nothing extra (§4.2); contraction off is
> inside every figure and not isolated. A fix is under design, with owner decisions pending
> ([the silicon record](reviews/rev7-silicon-record.md) §3.2–§3.5). *(2026-10-08: the fix is
> [cpu-budget.md](cpu-budget.md), owner-approved, with its decisions D1–D13; it replaces the
> estimate below with a cost model and an 85 % ceiling for any input, and its steps 1–2,
> bit-exact, are built.)*

The CPU budget of `grain-engine.md` §8 is derived and assumes contracted FMAs. With this profile
(**estimated**): nominal 2,950–3,700 → about 3,200–4,050 cycles/sample (+≈100 contraction,
+150–250 flush, +≈20 DetMath), 32–41 % of the 10,000-cycle budget (480 MHz / 48 kHz);
pessimistic 6,800 → about 7,700–7,800 (+≈430, +150–250, +≈300), 77–78 %, about 22 % headroom.
DWT gates every number.

### 7.3 The explicit-FMA option

> **Update (2026-10-07, Rev7 bench, sound revision 1).** The rule's condition is met: contraction
> off threatens the budget at 64 voices, the worst warm pessimistic block at 168.5 % (p99.9 160.9
> %), 169.0 % with cold caches. Explicit FMA is therefore a candidate, to build and measure before
> adopting; no build with contraction on or with explicit FMA exists yet, so the bench measures
> contraction off only, inside every figure. On §7.2's estimate of about +430 cycles/sample for
> contraction off at the pessimistic row, FMA alone cannot bring those blocks under the deadline;
> it is weighed with the CPU budget's fix, which is under design
> ([the silicon record](reviews/rev7-silicon-record.md) §3.9). *(2026-10-08: weighed and
> deferred. [cpu-budget.md](cpu-budget.md)'s D10, owner-approved, keeps explicit FMA out for now:
> it gains little on the M7 and costs the x86 plugin; it stays in reserve with the other
> sound-changing levers.)*

`detmath::Fma(a, b, c)` at chosen hot sites (`__builtin_fmaf`, `std::fma`), with contraction
off elsewhere, is bit-exact everywhere because IEEE-754 defines fused multiply-add exactly:
the prototype's placement gave one hash across MSVC, GCC, Clang and the emulated M7, and an
independent placement agreed across seven builds (**measured** [prototype], [oracle]). It
recovers roughly 80–90 % of the M7's
contraction-off loss in converted loops [prototype]. Without hardware FMA each call goes to a
library (an SSE2 MSVC build ran 43 % slower, **measured** [prototype]), so the x86 plugin would
require FMA3 or dispatch at run time to an identical software path. Relying on implicit
contraction agreeing is rejected: it fails across compilers (record §7.3).

**Decision: deferred to DWT.** Adopt in inner loops only if contraction-off threatens the
budget (§7.2's pessimistic row, or a target polyphony). Adoption bumps the revision, adds FMA3
or dispatch to the x86 plugin, and switches the fused-instruction audit to expected counts. It
is decided in merged step 5 (§8.4), before the first public revision.

## 8. Decisions, risks, open questions, plan

### 8.1 Decisions taken

Each decision is stated with its reason where it is made: bit-identity (§1.2), 48 kHz only
and the labels (§2); contraction off, no fast-math, `PUBLIC` profile, no LTO, no FP in public
headers, plugin-only PIC, no x87 or ARM64EC (§3.2–§3.8); no NaN and bit-test
canonicalization (§3.7); DetMath (§3.9); the full control word (§4.1); gradual underflow with a
per-sample flush, never FZ on the M7 alone (§4.2, §4.3); `Restart`, `LoadPreset`, the epoch and
stamped events (§5.8–§5.11); the sound revision (§5.12); one golden (§6.1); the CI legs
(§6.2); mute-and-reuse render mode (§6.6); the pinned toolchain (§6.8); no native Windows
ARM64 in v1 (§3.8). Polynomial kernels (§3.9), Spillover as the pedal's default load
(companion §6.7; §8.3 Q9) and deferring explicit FMA (§7.3) are provisional, settled by
§8.3's measurements.

### 8.2 Risks

Each risk is stated with its mitigation.

1. **The M7's subnormal cost is unknown.** If subnormal operations are slow on the M7, any
   silent-tail block that still raises a flag could cause a CPU spike: none in the battery
   since §4.3's `SoftSat` flush, but presets with tiny nonzero values still raise them (§6.4).
   Mitigation: §4.2's DWT gate, then a wider flush.
2. **The profile's M7 cost is unmeasured**; the pessimistic total is 77–78 % of the budget
   (**estimated**, §7.2). Mitigation: explicit FMA (§7.3), Init-built tables (§3.9) and ITCM
   placement of the inner loops (§7.1), all gated by DWT.
3. **Emulation is not silicon.** QEMU shows no errata, lazy-stacking effects, or cache and
   DMA coherency. Mitigation: HIL before any release, and a review of ST's and Arm's errata.
4. **Thin vectors prove little.** Today's vector fires 2 onsets in 10 s, and the contract-#1
   test fires none (**measured**). Mitigation: coverage counters, gcov thresholds and
   negative controls (§6.1, §6.4), `ConsumeOnsetCount() > 0` in the contract-#1 test, and a
   sound-revision rule that does not depend on golden coverage (§5.12).
5. **Some toolchains are untested.** AppleClang arm64, aarch64 GCC and future compiler
   versions are expected to conform but have not been measured. Mitigation: CI legs from day
   one, and the drift leg.
6. **Wrapper discipline.** JUCE's layers can silently break preconditions through normalized
   values, block-start automation, in-place buffers, 0-sample calls and `PRIVATE` linking.
   Mitigation: companion §5's plain-value parameters, `PUBLIC` linking and the flag
   comparison (§3.2, §6.3), the two parity legs (§6.2), and the aliasing fix (§5.6).
7. **Sound revisions can churn after release.** Every change that touches output bumps the
   revision: for example time-parameter smoothing, new mode-compiler features, explicit FMA,
   LUT+lerp kernels and, after v1, the rate-invariant constants of companion phase F. Users
   then see "not identical" until they update the pedal. Mitigation: batch sound changes, and
   land the known ones, including the hardware-gated decisions, before the first published
   revision (§8.4).
8. **A DetMath kernel can be wrong in a corner of its domain**, identically on every target,
   so identity does not catch it. Mitigation: stated domains with edge rules in every build,
   Debug assertions, endpoint tests and the nightly exhaustive accuracy job (§3.9).
9. **The block-split fix changes behaviour.** Strum-family modes behave differently under
   freeze, and Part B removes 10.7 ms at the far end of the ring. Mitigation: the Q3
   listening test; the lost ring is outside the `DelayMs` range.
10. **Exact loads mute the pedal's wet path** during `Restart` (45–160 ms, **estimated**).
    This is a user-experience risk, not a parity risk. Mitigation: the restart watermark
    (§5.8), and Spillover as the recommended default (§8.3 Q9).
11. **CI depends on emulators.** It needs particular QEMU and Docker versions, and Ubuntu's
    QEMU 8.2.2 is untested. Mitigation: pinned versions, with `qemu-user-static` as the
    fallback.
12. **Every libDaisy image links ST's SLA0044 USB code**, test images included (§6.6).
    Mitigation: companion §7.2's patch and the ELF gate from the first build.
13. **Emulated x86 builds look native** to users of arm64 machines. Mitigation: the Rosetta
    and Prism legs (§6.2) and the emulation label (§2.5).

### 8.3 Open questions

> **Update (2026-10-07, Rev7 bench, sound revision 1).** From
> [the silicon record](reviews/rev7-silicon-record.md) §4. Q1 is settled: no subnormal penalty at
> FZ = 0, and §4.2's rule passes (keep gradual underflow). Q5 is answered for the forms measured:
> the engine's bit test, 0.9–1.8 % of the budget, is cheaper than the two compares. Q9's `Restart`
> half is measured (47.18 ms); the watermark and the default load mode remain. Q2 remains: there
> is no contraction-on or explicit-FMA build, and the births suite bounds a birth at 6,203 cycles
> at most (48 voices at 1 ms grains, ring locality included), which does not decide kernels
> against tables. In §8.2, risk 1 is retired, risk 10's mute is 47.2 ms, and risk 2 came true: the
> measured cost exceeds the budget (§7.2). *(2026-10-08: Q2's explicit-FMA half is deferred by
> [cpu-budget.md](cpu-budget.md)'s D10; kernels against tables stay open.)*

| # | Question | Settled by |
|---|---|---|
| 1 | What does a subnormal operation cost on the M7? (Gates §4.2.) | DWT tests (a) and (b) of §4.2 |
| 2 | Adopt explicit FMA (§7.3)? Polynomial kernels or Init-built tables (§3.9)? | DWT with contraction on, off and explicit FMA in `RenderSpan`, the SVF, `FeedbackTamer` and the FFT; DWT of `ScheduleGrain` at 1 ms grains and overlap 1 |
| 3 | While frozen, pin-eligible marks (recommended) or live-head marks, under which freeze does nothing to mark-positioned grains? | A Strum listening test (§5.7) |
| 4 | Spillover loads: make the onset hop grid epoch-relative with a detector reset, or stamp loads at multiples of 256 frames? Only the 256-frame hop grid matters; the 512-frame FIFO phase does not (§5.9; record §5.9), so the companion's earlier 512-frame alignment proposal is withdrawn (companion record §5). If aligned, the producer stamps the aligned frame; the engine never defers events. | Whether Spillover convergence with onset modes matters |
| 5 | A cheaper deterministic per-sample flush? | DWT of the flush |
| 6 | Toolchain 10.3-2021.10 or libDaisy v9's? *QEMU closed:* pinned 10.2.3 at commit `2e7e8b7e…`, built from source; its first CI run matched the golden file and the local M7 archive byte for byte. | libDaisy v9's docs |
| 7 | *Decided for v1:* native Windows on Arm is not a v1 target (companion §2.1); the guard refuses `_M_ARM64` until phase F adds its branch (§4.1), and the x64 build under Prism is covered by §6.2's leg. ARM64EC stays out of profile unless a test shows that its `_mm_setcsr` reaches the real `FPCR`. | Product decision, taken; phase F needs a Windows-on-Arm test machine |
| 8 | *Closed:* libDaisy's int24 → float is exactly *i* × 2⁻²³ while `postgain` is 1 (§2.2). | — |
| 9 | Pedal default load: Spillover (recommended) or Exact? A user-experience choice. | DWT of the `Restart` clear; the watermark (§5.8) |
| 10 | Should Spillover re-arm the scheduler, and what happens to in-flight voices? Does the flush bring reverb and shimmer tails to exact zero? | Product decision, then re-measuring with the real `LoadPreset(…, Spillover)` after §5.4 |
| 11 | Which sound-changing work, besides the hardware-gated decisions of Q1 and Q2, lands before the first published revision? Rate invariance of the FIFO and the onset detector does not: companion §4.2 defers it to phase F, after v1. Time-parameter smoothing is decided in §8.4 step 8 (§5.6). | Roadmap |
| 12 | Embed historical engine revisions in the app? Recommended: no; prompt a firmware update. | Product decision |

### 8.4 Implementation plan

> **Update (2026-10-07, Rev7 silicon record).** Part of step 13's hardware-gated work is done on
> the owner's Daisy Seed Rev7 ([the silicon record](reviews/rev7-silicon-record.md) §4):
> `FPDSCR` is 0 at boot in every image's hello; the DWT pass measured subnormals, the flush, a
> bound on the cost of a birth at the maximum birth rate, the `Restart` clear and ITCM
> placement, with contraction off inside every figure; and §4.2 is decided (keep gradual
> underflow). Still open in step 13: render mode, the HIL runner, the engine SPSC queue, builds
> with contraction on and with explicit FMA (§7.3), kernels against tables, the watermark and the
> default load mode, and the CPU budget's fix, which is under design with owner decisions
> pending. *(2026-10-08: the fix is [cpu-budget.md](cpu-budget.md), owner-approved; its steps 1–2,
> bit-exact, are built, and its D10 defers explicit FMA.)*

The merged milestone sequence is companion §8.1; the profile's steps fall into it as below.
Every sound-changing change lands before revision 1 is **published** (§1.5).

**Status, 2026-10-06: steps 1–9 are done, and step 10 but for the pieces listed below**, in
`a30aa9d` through `d10c499` (`git log ce005eb..d10c499`): merged step 1 up to
`b114b07`, which merged the state API (step 9) and the post-delay glide (step 8's smoothing
decision); internal revision 1 minted in `06557f3`, its gates turned on in `26917d3`, and in
`d10c499` check mode made to compare every field of the golden file, and random block
sizes, the M7's forced-flush control (FZ) and the JUCE-include audit added to the gating legs
(STATUS.md, "Internal sound revision 1"). Step 10 still lacks: (a) the engine-side coverage counters and the gcov
thresholds of §6.1 (revision 1's substitutes are in §6.1); (b) host blocks up to 8,192 frames
through the wrapper's chunker (§6.4): the plugin's wrapper tests reach 4,096-frame host blocks
against the engine, not against the golden corpus; (c) the ODR controls, renders and the audit
with `dsp/` and plugin objects linked in swapped order (§6.3, §6.4); (d) the literal-bit audit
(§6.3); (e) the plugin format targets' flags compared with the engine's through
`compile_commands.json` (§6.3; the configure check already rejects forbidden flags on them);
(f) the second negative control, a DetMath function swapped back to libm, as a hash control
(§6.4; its symbol-audit form runs); (g) the Rosetta 2 and Prism host legs (§6.2); (h) a mint
job (§6.1 records the deviation). The branch protection that makes the gates binding
(§5.12, §6.1) is in place since 2026-10-06, with `.github/CODEOWNERS`; STATUS.md lists its
caveats. One finding for §5.12: a one-ULP binary64 change to a DetMath
coefficient changed no corpus output bit, because every DetMath result is rounded to binary32
first, so only the path trigger sees it. The rest of step 10, step 11 and the hardware-gated
work of step 13 remain.

- **Merged step 1, no hardware (profile steps 1–9; the JUCE skeleton runs in parallel):**
  (1) build profile (§5.2); (2) header hygiene (§3.5); (3) parity harness — generator,
  hashes, counters, host matrix report-only — grown from the evidence probes already committed
  under [`tools/parity/`](../../tools/parity/) (battery harness, DetMath accuracy test, oracle
  shim, FTZ boundary probe, and under `bugcheck/` the block-split harness, the two fix diffs
  and the regression test of §5.7); (4) M7 leg, report-only; (5) guard, refusing MSVC ARM64 in v1
  (§4.1), with flush and snap (§5.3, §5.4);
  (6) DetMath, with the symbol audit gating (§5.1); (7) NaN-free boundary (§5.5); (8) the
  block-split fix with pin-eligible marks unless Q3 decides otherwise, its assertion and
  tests, mono aliasing, dither fold, and the time-parameter smoothing decision (§5.6, §5.7);
  (9) `Restart`, epoch, `LoadPreset` and stamped events with the overflow counter
  (§5.8–§5.11), before minting because golden scripts place events at odd frames.
- **Merged step 2:** (10) mint internal revision 1 — `kSoundRevision` with both triggers,
  `golden.json` with x86 and M7 agreeing, host and M7 legs gating, audits, the full
  perturbation job and negative controls; (11) nightly full-system QEMU with the interrupt
  `FPDSCR` test, exhaustive DetMath, toolchain drift.
- **Merged steps 3–4, mode compiler and curation:** macro and expression curves land in
  `dsp/` on DetMath; every output change bumps the internal revision and re-stamps packages.
- **Companion phase B, in parallel:** (12) app integration — plain-value parameters, `PUBLIC`
  linking, chunks of at most 512 frames, 0-frame calls skipped, `SanitizeInput`, no mono
  aliasing, parameters re-pushed after every `Init` (which resets them, **measured** [host]),
  48 kHz pedal-exact mode, renders from `Restart`, labels, and the two parity legs.
- **Merged step 5, bring-up (companion phases D, E):** (13) `FPDSCR = 0` at boot, the ELF gate
  from the first image, render mode, HIL runner, the engine SPSC queue as pedal transport;
  DWT of subnormals, contraction, explicit FMA, the flush, `ScheduleGrain` at the maximum
  birth rate, the `Restart` clear and watermark, and ITCM placement; then decide §4.2, §7.3,
  kernels versus tables and the default load mode, each adopted change bumping the internal
  revision; (14) the parity check and verified upload (§6.7), which can start on the
  companion's desktop pedal simulator.
- **Merged step 6:** the first public sound revision fixes IDs, names, tapers and sound
  together; only then do the public plugin, app and firmware ship. After v1: capture and
  replay (§6.7).

## 9. Evidence record

[reviews/determinism-profile-record.md](reviews/determinism-profile-record.md), numbered like
this document in its §1–§7 (its §0 describes the sources and its §8 holds cross-cutting
material), keeps the measurement detail, the evidence sources and where their probes live
(under [`tools/parity/`](../../tools/parity/)), the evidence conflicts and how
each was resolved, and the corrections made during review.
