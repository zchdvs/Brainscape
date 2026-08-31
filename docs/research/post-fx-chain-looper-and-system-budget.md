# Post-FX Chain, Looper, Freeze, and Whole-System Budget for Brainscape

> Research reference for **Brainscape** — costing the non-granular half of the engine (reverb, modulation, filter, looper, freeze) and assembling one whole-system CPU/RAM budget on the Daisy Seed (STM32H750, Cortex-M7 @ 480 MHz, 64 MB SDRAM, 512 KB AXI SRAM, 48 kHz / 48-sample-block ≈ 1 ms / 10,000-cycle callback baseline).
> Companion to `grain-delay-theory.md` (grain engine), `microcosm.md` (product reference), `vst-and-shared-dsp.md` (shared-core architecture), and `daisy-seed-platform.md` (hardware platform). Every DSP-source claim below was pulled and read in full from the primary repository via the GitHub API during this research (not paraphrased from search snippets) unless marked otherwise.

---

## Summary

- **DaisySP's `ReverbSc` is LGPL-2.1, not MIT, and lives in a separate submodule repo** (`electro-smith/DaisySP-LGPL`), gated behind an explicit opt-in build flag. This directly contradicts the implicit framing in the earlier research corpus that it's just another DaisySP module. Verified from `.gitmodules`, the `DaisySP-LGPL` `LICENSE`, and its `README.md`'s explicit LGPL redistribution obligations.
- **`ReverbSc`'s tank is a fixed `float aux_[98936]` — ~386 KiB of static storage regardless of sample rate** — verified from the actual `reverbsc.h`/`reverbsc.cpp` source. At Brainscape's 48 kHz the eight FDN delay lines (verified exact lengths: 2473/2767/3217/3557/3907/4127/2143/1933 samples plus small jitter margins) only need ~24,850 samples (~97 KiB in float); the shipped class over-allocates ~4x for headroom the pedal will never use at a fixed 48 kHz. **As shipped, this is too large to drop into internal SRAM without eating most of the 512 KB budget** — either resize it to the actual sample rate or place it in SDRAM.
- **Mutable Instruments Clouds' Dattorro/Griesinger reverb tank totals ~16,384 samples in 16-bit fixed-point (Q4.12) — exactly 32 KiB** at Clouds' native 32 kHz, verified from `fx_engine.h`/`reverb.h` source (the `FxEngine<16384, FORMAT_12_BIT>` template and its `Reserve<...>` delay-line chain sum to ~16,375 samples). Rescaled to 48 kHz that's ~24,576 samples ≈ **48 KiB** (16-bit) or **~96 KiB** (float32). The reference Faust port of the raw Dattorro topology (independently verified from `reverbs.lib` source) needs 22,494 samples total (905 input-diffusion + 21,589 tank) at ~29.76 kHz, rescaling to ~36,270 samples (~72–145 KiB) at 48 kHz.
- **All three reverb algorithms studied need on the order of 50–150 KiB of tank memory at 48 kHz — two to three orders of magnitude smaller than the grain buffer's 16–32 MiB.** Combined with a real community-measured data point (below), the reverb tank belongs in internal SRAM, not SDRAM.
- **The "3.5x SRAM-vs-SDRAM cost" figure is real, but it lives in `daisy-seed-platform.md`, not `grain-delay-theory.md`** (this document's task brief misattributed it). It is a community forum report of a reverb needing **~240 KB of delay memory measuring 42% CPU with SDRAM-resident buffers vs. 12% CPU with SRAM-resident buffers** — a ~3.5x gap, and directly on-point: 240 KB is in the same range this research independently derived for a plate/FDN tank. This is strong, if informal, evidence that a reverb tank sized like the ones above should be placed in SRAM.
- **DaisySP's `Looper` utility (MIT, core library) is mono-only, has no built-in undo, no quantize, and its main playback path truncates fractional position with no interpolation** — verified in full from `looper.h` source. `TrigRecord()` is a 4-state machine (EMPTY → REC_FIRST → PLAYING → REC_DUB) with no way to discard just the last overdub short of clearing the whole buffer. It supplies only the buffer read/write/crossfade primitive (a 1200-sample, `sin(π/4·x)`-windowed seamless-loop-point fade); **all of the Microcosm-grade features Brainscape wants — stereo, undo, quantize, 1/4x–4x varispeed, pre/post-FX routing — must be built on top of it, not found inside it.**
- **`DaisySP::Looper` hardcodes a sample-rate assumption** (`IsNearBeginning()` compares `pos_ < 4800`, silently meaning "100 ms" only at 48 kHz) and **`ChorusEngine` does the same** (`kDelayLength = 2400 // 50ms at 48kHz`, a compile-time constant sized for one specific sample rate). Both are verified, concrete, in-the-wild examples of exactly the anti-pattern `vst-and-shared-dsp.md` warns against — independent evidence for resolving Brainscape's own block-size/sample-rate question toward runtime parameterization everywhere in `dsp/`.
- **The compile-time-vs-runtime block size contradiction is resolved by primary source, not opinion: libDaisy's own `AudioHandle::Config` declares `size_t blocksize = 48` as a runtime struct field with a runtime `SetBlockSize()` method**, and `SetSampleRate()` alongside it — verified directly from `audio.h`. Block size was never a compile-time constant even on the firmware target itself; `grain-delay-theory.md`'s rec (compile-time constant "so loops unroll") reached for a real optimization but the wrong mechanism. `vst-and-shared-dsp.md`'s `Init(sampleRate, maxBlockSize)` / `Process(..., numFrames)` pattern is correct and should stand as `dsp/`'s public interface; loop-unrolling can still be had by chunking the *inner* per-grain render loop into a fixed-size compile-time sub-block (e.g. 8 or 16 samples) without exposing that constant in the public API.
- **DaisySP's `Svf` (state-variable filter, MIT, verified in full from source) double-samples internally** (runs its core recurrence twice per input sample, averages the two) for stability at high cutoff/resonance, and **unconditionally computes all five outputs — low/high/band/notch/peak — every call**. A user-selectable LP/BP/HP/notch morphing filter (community wishlist item #7) therefore costs *nothing extra* in arithmetic over a fixed LP-only implementation: the "morph" is just choosing which already-computed output(s) to mix. Total cost is ~25–35 float ops/sample per instance — trivial against the 10,000-cycle budget even with two instances for stereo.
- **The Daisy Seed's on-board 8 MB QSPI flash cannot hold even one 60-second stereo loop**, let alone loops embedded in multiple presets: verified from `libDaisy`'s `qspi.h`/`PersistentStorage.h`, QSPI erase/write is sector(4 KB)/block(64 KB)-granular and `PersistentStorage<T>::Save()` is explicitly a blocking call sized for small settings structs, not multi-megabyte audio. A 60 s stereo int16 loop alone is ~23 MB — **saving loops into presets the way the Microcosm does requires adding a microSD card** (libDaisy ships full `FatFs` + `SDMMC` support, verified from the repo tree, so this is a well-trodden, not speculative, path) rather than QSPI.
- **Freeze can be built for zero additional SDRAM** if Brainscape's grain engine already maintains a continuously-written circular history buffer (as `grain-delay-theory.md` assumes): "freeze" is then just pinning the scheduler's write-head reference so grains keep drawing from a fixed window of already-captured audio instead of one that keeps sliding forward, while the phrase looper remains an entirely separate, independently-sized buffer. **This gives a concrete, architectural answer to `microcosm.md` §5's claim that the freeze/looper mutual exclusivity "has no architectural justification"** — on this design they don't even compete for the same memory.
- **The two SDRAM asks in the corpus (a generous grain buffer, and a multi-minute/multi-slot looper) do fit in 64 MiB together, but only under a specific, previously-unstated choice: 16-bit storage for both, and a grain/history buffer sized at the *low* end (16 MiB / 87 s) rather than the *high* end (32 MiB / 175 s) `grain-delay-theory.md` offered.** At 16 MiB(grain) + ~46 MiB(looper, ≈4.2 minutes stereo int16 with no undo, or two ~2-minute undo-capable buffers) + ~0.2 MiB(reverb tank if kept in SDRAM for simplicity, plus scratch), the total is ~62.2 of 64 MiB. Choosing the 32 MiB grain buffer instead leaves ~31.8 MiB for everything else — still workable (a single ~2.9-minute loop, or ~87 s with full undo — both still exceed Microcosm's 60 s parity) but meaningfully tighter once multiple undo-capable slots are wanted (see §6.1's worked table), and the two docs' recommendations genuinely do need this explicit reconciliation, which had not previously been done.
- **`ReverbSc` has no input diffusion stage** (input is summed via an 8-line "resultant junction pressure," verified O(N) not O(N²) from `Process()` source) while **Clouds' reverb has an explicit 4-stage allpass diffuser ahead of its FDN loop** (Dattorro/Griesinger topology). Because Brainscape's reverb sits immediately downstream of a pointillistic granular source, the diffuser-equipped topology is plausibly the better perceptual match even though `ReverbSc` is the simpler, cheaper code — a genuine design tradeoff, not just a licensing one.
- **GuitarML's `DaisyCloudSeed` — a real, shipped port of the open-source "CloudSeed" algorithmic reverb (MIT, by ValdemarOrn) onto a Daisy-Seed-based pedal (Terrarium) — is independent, practical evidence for the memory/CPU hierarchy above**: the maintainers had to convert full CloudSeed's processing to **mono** to fit "up to 5 delay lines" on Terrarium's hardware, and separately ship a lighter alternative, **"CloudyReverb," explicitly described as using "the reverb algorithm from eurorack" (i.e. Clouds) because it needs less memory/processing** than CloudSeed. This is a real-world confirmation that Clouds' reverb is the lightweight option and a full multi-line network reverb (CloudSeed-style) is comparatively heavy on this exact hardware class.
- Assembled CPU estimate (all algorithm costs derived/estimated in this document, cross-checked against `grain-delay-theory.md`'s own cycle-counting methodology, and explicitly **not hardware-measured**): 64 grains (~1,600 cycles) + `ReverbSc`-class reverb in SRAM (~500–800 cycles, estimated from its verified `Process()` op count) + stereo SVF filter (~70 cycles) + stereo modulation (~60 cycles) + a simple envelope-based onset detector (~100 cycles, unmeasured estimate — no dedicated Brainscape onset-detection research exists yet) + stereo looper read/write (~60 cycles) totals **roughly 2,400–2,700 of the 10,000-cycle/sample budget, ≈24–27%**, leaving substantial headroom for UI/MIDI (which should run at control rate, not audio rate, and therefore shouldn't be charged against this budget at all in a properly layered architecture) and safety margin.

---

## 1. Reverb

### 1.1 Candidate algorithms at a glance

| Algorithm | License | Where it lives | Topology | Tank size @ native rate | Tank size @ 48 kHz (derived) | Input diffusion? |
|---|---|---|---|---|---|---|
| **DaisySP `ReverbSc`** | **LGPL-2.1** (separate `DaisySP-LGPL` submodule) | `electro-smith/DaisySP-LGPL` | 8-line stereo FDN, "resultant junction pressure" scattering-junction feedback (Costello/Varga, from Csound `reverbsc`) | 48 kHz-native (Daisy's own port re-derived the params at 48 kHz, not 44.1 kHz) | as-shipped: fixed 98,936-float (~386 KiB) static array; properly sized: ~24,850 samples (~97 KiB float) | **No** — input sums directly into the FDN |
| **Mutable Instruments Clouds `Reverb`** | **MIT** | `pichenettes/eurorack` (`clouds/dsp/fx/reverb.h`) | Dattorro/Griesinger: 4 input allpass diffusers → 2×(2AP+1 delay) loop, LFO-modulated | 32 kHz-native, 16,384 samples @ Q4.12 (16-bit) = 32 KiB | ~24,576 samples ≈ 48 KiB (16-bit) / ~96 KiB (float32) | **Yes** — 4-stage allpass diffuser |
| **Reference Dattorro plate** (as ported in Faust's `reverbs.lib`) | MIT (Faust Libraries license) | `grame-cncm/faustlibraries` | Dattorro 1997: 4 input-diffusion allpasses + figure-8 tank of 2 decay-diffusion loops | ~29.76 kHz-native (integer sample counts from Dattorro's original paper, not visibly rescaled in this port), 22,494 samples total | ~36,270 samples ≈ 72 KiB (16-bit) / 145 KiB (float32) | **Yes** — 4-stage allpass diffuser (same lineage as Clouds') |
| **CloudSeed** (ValdemarOrn) via **GuitarML `DaisyCloudSeed`** | Original CloudSeed algorithm: **MIT**, verified via GitHub's license API on `ValdemarOrn/CloudSeed`. GuitarML's own `DaisyCloudSeed` port/glue repo: **no LICENSE file found at the repo root** (verified — GitHub's license API returns 404 for it) — treat the port's own code as unstated/unverified licensing pending direct clarification from the maintainer, even though its CloudSeed and eurorack submodule dependencies are MIT | `GuitarML/DaisyCloudSeed`, derived from `ValdemarOrn/CloudSeed` | Multi-line comb/allpass diffusion network (heavier than the above three) | Not independently verified in this research; GuitarML's port needed to go **mono** and cap at **5 delay lines** to fit on a Daisy-Seed-based pedal (Terrarium) | Heavier than the alternatives above — see §1.5 | Yes (network-based diffusion, distinct from Dattorro's four discrete allpasses) |

### 1.2 DaisySP-LGPL `ReverbSc` — verified from source

**Licensing (verified, not inferred).** `DaisySP`'s top-level `daisysp.h` only pulls in `reverbsc.h` behind `#ifdef USE_DAISYSP_LGPL → #include "daisysp-lgpl.h"`, and the actual file lives in a separate git submodule declared in `.gitmodules`:
```
[submodule "DaisySP-LGPL"]
    path = DaisySP-LGPL
    url = https://www.github.com/Electro-smith/DaisySP-LGPL
```
That repo's `LICENSE` states plainly: *"DaisySP-LGPL, copyright (c) 2023 Electrosmith, Corp. Published under the LGPL-V2.1 license."* Its `README.md` spells out the practical obligation: *"If you distribute code that makes use of DaisySP-LGPL, you are obligated to give end users the option to recompile with the LGPL components having been replaced,"* and ships a `gather_lgpl.sh` script specifically to make that obligation dischargeable for a closed-source-adjacent product. **For a project already planning GPLv3 (per `vst-and-shared-dsp.md`), LGPL-2.1 linkage is compatible and low-friction — GPLv3 code may link LGPL-2.1 libraries — but it is a second license to track and disclose, not a non-issue**, and the earlier research corpus's framing ("DaisySP has ReverbSc") elided this distinction entirely. [DaisySP `.gitmodules`](https://github.com/electro-smith/DaisySP/blob/master/.gitmodules), [DaisySP-LGPL LICENSE](https://github.com/electro-smith/DaisySP-LGPL/blob/main/LICENSE), [DaisySP-LGPL README](https://github.com/electro-smith/DaisySP-LGPL/blob/main/README.md).

**Algorithm (verified from `reverbsc.h`/`reverbsc.cpp` in full).** Ported from Csound's `reverbsc` opcode (Sean Costello 1999, Istvan Varga 2005 — both credited in the file's copyright header). Eight delay lines, each independently and continuously **randomly modulated in length** (`NextRandomLineseg()` walks each line's delay time toward a new pseudo-random target over `sample_rate_ / kReverbParams[n][2]` samples — this is what keeps the FDN's resonances from becoming static combs). Feedback is a **"resultant junction pressure"** model — physically, N lossless waveguides of equal characteristic impedance meeting at a junction — computed as one `O(N)` sum (`a_in_l += delay_lines_[n].filter_state` for all 8 lines, scaled by `kJpScale = 0.25`), **not** a full `O(N²)` mixing matrix multiply. Each line reads with **4-point cubic interpolation** (not the 2-point linear DaisySP uses elsewhere) and applies a single first-order lowpass in its own feedback path (`damp_fact` derived from `lpfreq_`) before writing back. Output is `out = a_out_l/r * 0.35` (fixed `kOutputGain`).

**Delay line lengths — verified exact, in samples, at the Daisy port's own `DEFAULT_SRATE = 48000.0`** (note: this differs from the original Csound/LMMS port, which used 44100 Hz — Electrosmith re-derived the constant for 48 kHz):
```
2473, 2767, 3217, 3557, 3907, 4127, 2143, 1933   (sum = 24,124 samples)
```
plus a per-line random-jitter margin (`kReverbParams[n][1]`, 0.0006–0.0017 s) scaled by `i_pitch_mod` (fixed at 1 in `Init()`) and a `+16.5` rounding pad in `DelayLineMaxSamples()` — pushing the *actually used* total to roughly **24,850–25,000 samples (~97–98 KiB in float32)**.

**Memory — the actual finding.** The class declares:
```cpp
#define DSY_REVERBSC_MAX_SIZE 98936
...
float aux_[DSY_REVERBSC_MAX_SIZE];   // one flat array, 8 lines carved out of it via pointer offsets
```
This is a **static member, sized once at compile time to 98,936 floats = 395,744 bytes ≈ 386.5 KiB**, regardless of what sample rate `Init(sr)` is actually called with. It is not a template parameter and not resized based on `sr` — `Init()` merely slices the fixed `aux_` array into 8 sub-buffers via `delay_lines_[i].buf = aux_ + n_bytes`. At Brainscape's fixed 48 kHz this wastes roughly 4x the memory the algorithm actually needs (the constant appears sized for headroom up to a much higher sample rate and/or larger pitch-mod range than a 48 kHz-only pedal will ever use). **Practical consequence: dropping `ReverbSc` unmodified into internal SRAM consumes ~75% of the Daisy's 512 KB AXI SRAM region** (already capped to ~480 KB usable by the bootloader, per `daisy-seed-platform.md`) — leaving almost nothing for anything else that wants fast internal memory (filter/oscillator state, DTCM scratch for the grain accumulator, stack, LUTs). **Recommendation: either (a) reimplement with the buffer sized to the actual runtime sample rate (an `Init(sampleRate, maxBlockSize)`-style allocation, consistent with `vst-and-shared-dsp.md`'s own architecture principle, which the shipped class violates), or (b) if reusing the class verbatim for speed of implementation, place the whole `aux_` array in SDRAM via `DSY_SDRAM_BSS`** rather than let it land in default SRAM/BSS. [reverbsc.h](https://github.com/electro-smith/DaisySP-LGPL/blob/main/Source/Effects/reverbsc.h), [reverbsc.cpp](https://github.com/electro-smith/DaisySP-LGPL/blob/main/Source/Effects/reverbsc.cpp).

**CPU — derived, not measured.** Per delay line per sample: 1 write (with a subtract for the feedback junction), a cubic-interpolation coefficient calculation (~10 float mul/add), 4 buffer reads, the cubic combine (~9 mul/add), and the feedback/damping filter (~4 ops) — roughly **30–35 float ops + 4 loads + 1 store per line**. Across all 8 lines plus the O(N) junction-pressure sum and output scaling, that's on the order of **270–300 float multiply/adds and 32 memory loads per output sample** (both channels together, since all 8 lines are processed once to produce stereo out). On a dual-issue, FMA-capable Cortex-M7 with the buffer resident in zero-wait-state SRAM, this plausibly retires in the range of **500–800 cycles/sample (5–8% of the 10,000-cycle budget)** — cheap. **If the same buffer is placed in SDRAM instead, expect a jump broadly consistent with the community-reported 3.5x figure in §1.6**, since accesses are 8 independent scattered streams per sample (not one sequential stream), a worse cache-locality pattern than a single delay line.

### 1.3 Mutable Instruments Clouds `Reverb` — verified from source

**License and structure.** MIT (Emilie Gillet, 2014), verified from the file's own copyright header. Implements *"the Griesinger topology described in the Dattorro paper (4 AP diffusers on the input, then a loop of 2x 2AP+1Delay). Modulation is applied in the loop of the first diffuser AP for additional smearing; and to the two long delays for a slow shimmer/chorus effect"* — a direct quote from the source comment.

**Buffer — verified exact.** `typedef FxEngine<16384, FORMAT_12_BIT> E;` — a single flat buffer of 16,384 samples, shared across all ten named delay lines via a compile-time `Reserve<...>` chain:
```
113, 162, 241, 399, 1653, 2038, 3411, 1913, 1663, 4782   (ap1..ap4, dap1a/b, del1, dap2a/b, del2)
```
which sum to 16,375 samples (plus 1 guard sample per line) — i.e. the class's ten delay lines occupy essentially the *entire* declared 16,384-sample buffer, with almost no slack. **`FORMAT_12_BIT` is not 12-bit storage** — verified from `fx_engine.h`: it's a **full 16-bit integer** (`uint16_t`) storing a fixed-point value scaled by 4096 (`value / 4096.0f` to decompress), giving roughly +18 dB of headroom above unity before clipping (useful for a resonant tank that can transiently exceed 0 dBFS) while still costing exactly 2 bytes/sample. **Total tank memory: 16,384 × 2 bytes = 32,768 bytes = 32 KiB**, at Clouds' native 32 kHz internal rate. Rescaled proportionally to 48 kHz (delay lengths in Clouds are effectively tuned in samples-at-32kHz, i.e. ms-based; a straight rate rescale gives ×1.5): **~24,576 samples ≈ 48 KiB in 16-bit, or ~96 KiB if stored as float32** (trading the fixed-point compress/decompress cost for direct float access — worth benchmarking both ways). [Clouds `reverb.h`](https://github.com/pichenettes/eurorack/blob/master/clouds/dsp/fx/reverb.h), [Clouds `fx_engine.h`](https://github.com/pichenettes/eurorack/blob/master/clouds/dsp/fx/fx_engine.h).

**Design technique worth copying regardless of which reverb Brainscape ships.** `FxEngine`'s `Context` class packages "read from delay line," "write with feedback," "one-pole lowpass," and "linear-interpolated read with LFO offset" into small composable primitives (`c.Read(...)`, `c.WriteAllPass(...)`, `c.Lp(...)`, `c.Interpolate(...)`) operating on a shared `accumulator_`. The whole reverb's signal flow (§1.3's Griesinger topology) is then ~25 lines of very readable code. This is a strong template for how Brainscape's own reverb (whichever topology is chosen) should be structured in `dsp/`, independent of whether the exact Clouds code is vendored.

**CPU — derived, not measured.** Each `Context` operation (`Read`, `Write`, `Lp`, `Interpolate`) is 2–6 float ops plus 1–2 memory accesses; the full per-sample chain in `Process()` has roughly 15 such calls (4 diffuser read/write pairs, 2 `Interpolate` with LFO, 2 `Lp`, 4 more read/write pairs, 2 output blends) — call it **~60–90 float ops and ~20 memory accesses per sample**, noticeably cheaper in raw op-count than `ReverbSc`'s 8-line FDN, consistent with Clouds needing to run this *in addition to* its whole granular engine on a much slower (168 MHz, no double-issue) STM32F4/M4.

### 1.4 Reference Dattorro plate (generic topology, not vendored from any specific repo)

Verified from Faust's `reverbs.lib` (`dattorro_rev`, MIT-licensed Faust Libraries), which implements Dattorro's 1997 JAES topology essentially unmodified from the paper's own integer sample counts:
- **Input diffusion:** 4 allpasses, lengths **142, 107, 379, 277** samples (sum 905).
- **Tank (figure-8 of two decay-diffusion loops):** delays **672, 4453, 1800, 3720** on one side and **908, 4217, 2656, 3163** on the other (sum 21,589).
- **Grand total: 22,494 samples**, at what is very likely Dattorro's original ~29.76 kHz reference rate (no explicit rate-rescaling logic was visible in the fetched excerpt of this port — treat that specific detail as **unverified**, since Faust's parameter list separately exposes `bw`/`damping` coefficients that are themselves sample-rate-dependent and may be intended to be retuned by the caller rather than by the library).
- Rescaled proportionally to 48 kHz: **~36,270 samples ≈ 145 KiB (float32) or ~72 KiB (16-bit)** — squarely in the same 50–150 KiB range as the other two algorithms.

This is included because it's the algorithm both Clouds' and countless commercial "plate reverb" plugins actually derive from, and because its component structure (4 input allpasses feeding a figure-8 of two allpass+delay chains) is architecturally identical to Clouds' reverb — Clouds is, in effect, a compressed/retuned Dattorro implementation. **A from-scratch Brainscape reverb built directly against Dattorro's published topology, sized in float32 or 16-bit at 48 kHz, lands in the same ~70–150 KiB memory class as reusing Clouds' code, without the MIT-attribution-of-a-large-vendored-file question.** [Faust `reverbs.lib`](https://github.com/grame-cncm/faustlibraries/blob/master/reverbs.lib), [Dattorro's original paper](https://ccrma.stanford.edu/~dattorro/EffectDesignPart1.pdf).

### 1.5 CloudSeed / GuitarML `DaisyCloudSeed` — real-world evidence, not a recommendation

**CloudSeed** (`ValdemarOrn/CloudSeed`) is an independent, MIT-licensed algorithmic reverb VST (license verified directly via GitHub's license API) — a denser, more parameter-rich comb/allpass diffusion network than either of the above, originally built for desktop CPU budgets. **GuitarML's `DaisyCloudSeed`** ports it to a Daisy-Seed-based guitar pedal (Terrarium hardware) and is useful here purely as **evidence of the memory/CPU hierarchy**, not as a candidate to vendor wholesale:

- The port's README states processing was changed **"to mono (from stereo), which allows up to 5 delay lines"** — i.e. full stereo CloudSeed at its native line count did not fit comfortably on this hardware class, and the team traded a channel of stereo width for delay-line count headroom.
- The same repo ships **"CloudyReverb,"** described explicitly as **"a lighter reverb than CloudSeed (in terms of memory/processing requirements)"** that **"uses the reverb algorithm from eurorack"** — i.e. Clouds' Dattorro/Griesinger implementation from §1.3, offered as the lower-cost alternative for exactly the CPU/memory reasons this document is quantifying.
- This is independent, practical confirmation — from a team solving the identical problem (fit a good algorithmic reverb on Daisy-Seed-class hardware, in a guitar pedal) — that a full multi-line network reverb (CloudSeed-class) is comparatively heavy, and that Clouds' reverb specifically is the "I need this to be cheap" choice in that community's own judgment.

**Recommendation for Brainscape:** do not target a full CloudSeed-class network reverb for the primary always-on post-chain reverb; it's the wrong end of the cost/benefit curve for a device whose CPU is also carrying a full granular engine. It remains a reasonable *option* for a "big ambient" alternate reverb mode gated behind a CPU-budget check, following exactly GuitarML's own two-tier pattern (CloudSeed as the expensive/optional mode, Clouds-derived as the cheap default). [GuitarML/DaisyCloudSeed](https://github.com/GuitarML/DaisyCloudSeed), [erwincoumans/DaisyCloudSeed (upstream Patch port)](https://github.com/erwincoumans/DaisyCloudSeed), [Daisy Forum: DaisyCloudSeed lush reverb](https://forum.electro-smith.com/t/daisycloudseed-lush-reverb/522).

### 1.6 Does the tank belong in SRAM or SDRAM?

This document independently derived ~50–150 KiB as the tank size for every algorithm studied (§1.2–1.4). Separately, `daisy-seed-platform.md` (a sibling research file in this same project, not re-derived here) already cites a directly-relevant, real community data point:

> *"A community-reported reverb example needing ~240KB of delay memory used 42% CPU in SDRAM vs. 12% CPU in internal SRAM for the same algorithm — an ~3.5x cost difference."* — [Daisy Community thread](https://community.daisy.audio/t/64mb-of-sdram-for-up-to-10-minute-long-audio-buffers/4422) (attempted to re-fetch directly during this research; the forum returned HTTP 403 to automated fetching, so this citation is carried forward from `daisy-seed-platform.md`'s own verified summary rather than independently re-confirmed here).

**This is the origin of the "3.5x SRAM-vs-SDRAM cost point" referenced in this document's task brief — it is real, but it is in `daisy-seed-platform.md`, not `grain-delay-theory.md` (which this research also checked directly and does not contain any such figure).** The 240 KB test case is close enough to this document's own ~50–150 KiB tank estimates that the two numbers should be read together: **a properly-sized reverb tank (any of the three algorithms studied) comfortably fits within internal SRAM's 512 KB region, and doing so avoids a large, empirically-observed CPU penalty.** Concretely:
- `ReverbSc`, if resized to ~97 KiB (§1.2) instead of shipped as-is (386 KiB), uses ~19% of the ~480 KB SRAM the bootloader leaves usable — leaves ample room for everything else that wants internal memory.
- Clouds'/Dattorro's ~48–150 KiB tanks are an even smaller fraction.
- Oopsy's own code-generation strategy (cited in `daisy-seed-platform.md`) independently converges on the identical rule: *"fast SRAM for the core algorithm and most data resources"* and SDRAM only for *"larger delays and data storage."*

**Recommendation: place the reverb tank in internal SRAM (AXI SRAM or DTCM, per the `dsp/` allocation seam recommended in `vst-and-shared-dsp.md`), reserve SDRAM exclusively for the grain/history buffer and the phrase looper (§4, §6).** This resolves the sub-question raised in the task brief cleanly and is the single most actionable finding in this section.

### 1.7 Reverb recommendation for Brainscape

1. **Do not simply vendor `ReverbSc` as-is.** Its LGPL-2.1 licensing needs an explicit, documented decision (acceptable, but must be disclosed and the `gather_lgpl.sh`-style redistribution path considered), and its static 386 KiB buffer must be resized or relocated before use.
2. **Prefer a from-scratch or Clouds-derived (MIT) Dattorro/Griesinger-style reverb with input diffusion** over `ReverbSc`'s diffusion-less FDN, specifically *because* Brainscape's reverb sits downstream of a pointillistic granular source — the 4-stage allpass diffuser is doing exactly the perceptual job (smearing transients before they hit the resonant tank) that a granular-delay's output most needs. This is a case where the "better before granular" reasoning in `microcosm.md` §11.4 argues for the topology with diffusion even though it's marginally more code than `ReverbSc`.
3. **Store the tank buffer as 16-bit fixed-point (matching Clouds' Q4.12 approach) in internal SRAM**, not float32 in SDRAM — cheaper in both memory and (per §1.6) CPU.
4. **Reserve a CloudSeed-class network reverb (via the GuitarML precedent) as an optional, more expensive "big ambient" alternate mode**, gated so it cannot run concurrently with the full grain engine at maximum grain count if the CPU budget doesn't support both (see §6).

---

## 2. The post-FX chain: fixed order vs. reorderable

### 2.1 The Microcosm's fixed chain (recap, from `microcosm.md` §3)

```
INPUT → [+] → EFFECTS(granular) → MODULATION → REVERB → FILTER → [+] → OUTPUT
```
Fixed, always in that order, not user-configurable, and the Filter's fully-CCW position doubles as a hard wet-kill. `microcosm.md` rec #11 recommends keeping this as the *default* signal flow but making each stage **defeatable and the order configurable** — validated by Hologram's own Chroma Console, whose headline feature over the Microcosm is exactly "4 re-orderable modules."

### 2.2 What a reorderable chain actually costs

This is primarily an **architecture and state-management cost, not a raw-cycles cost** — none of the individual blocks (modulation, reverb, filter) becomes more expensive to compute because its position in the chain changed. The real costs are:

1. **A processing-order data structure and dispatch.** Instead of hardcoding `modulation.Process(x); x = reverb.Process(x); x = filter.Process(x);`, the engine needs an ordered list of stage handles (e.g. a fixed-size `std::array<Stage*, 3>` or an enum-indexed permutation) walked in a loop. On a Cortex-M7 this is a handful of extra instructions per stage per block if implemented as a **function-pointer-free** dispatch (a `switch` on a stage-ID enum, or — better — templated stage objects called through a compile-time-sized loop with the *order* as runtime data but the *stage types* fixed) — virtual dispatch should be avoided (per `vst-and-shared-dsp.md`'s RTTI/exceptions guidance) but a plain array of tagged unions or an ID-indexed function-pointer table costs on the order of **1 indirect call + 1 branch per stage per block**, i.e. negligible (a handful of cycles amortized over a 48-sample block, not per-sample).
2. **Per-stage bypass/crossfade state.** "Defeatable" cannot mean a hard on/off switch mid-signal — that clicks. Each stage needs its own **wet/dry crossfade ramp** (a one-pole or linear ramp over N samples, matching the smoothing already needed for parameter changes) so toggling a stage in or out of the chain is inaudible. This is cheap per stage (1 multiply-add per sample per stage, same cost class as the denormal guard) but is *new* state that a fixed-order, always-on chain doesn't need.
3. **Preset/state complexity.** A fixed chain's preset only needs each stage's parameters. A reorderable chain's preset additionally needs **the order itself** (a small permutation — 3 stages = 6 possible orders, needs only 3 bits if stored as a fixed-width index, or a short array of stage-ID bytes) and **each stage's bypass state**. This is trivial in byte count but is a real addition to the preset schema that must be designed up front (consistent with `vst-and-shared-dsp.md` rec #8's "design the preset/mode file format before the DSP").
4. **Interaction/normalization surprises.** Reordering modulation-then-reverb vs. reverb-then-modulation changes the reverb tank's input spectrum (a chorused/vibrato'd input diffuses differently than a static one) and can change perceived loudness. This isn't a CPU cost, but it is a **design/QA cost**: every stage needs to sound acceptable in every position, which roughly multiplies the tuning/preset-curation effort by the number of valid orderings (3 stages → up to 6 orderings, though not all need equal attention if some are clearly the "intended" defaults).
5. **Filter's dual role.** The Microcosm's filter is both a tone control *and* (fully-CCW) a wet-kill. If the filter becomes reorderable, its "kill" behavior needs to be reconsidered — a filter placed *before* the reverb that's turned fully closed will silence the reverb's input (starving the tank, causing it to decay to silence over the reverb's own tail time) rather than instantly muting the output the way it does today at the end of the chain. This is a real, non-obvious behavioral change worth flagging explicitly in the design rather than discovering it in testing.

**Net assessment: reordering is CPU-cheap (single-digit percent of one block's overhead at most, dominated by the crossfade ramps, not the dispatch) but meaningfully more expensive in engineering time — preset schema, per-stage crossfade, and the "does every stage sound good everywhere" QA matrix.** This matches `microcosm.md`'s own framing of the Chroma Console's re-orderable chain as the harder, more "toolbox" philosophy versus the Microcosm's easier, more "finished sounds" philosophy — the cost is real but is not a reason to avoid it; it's a reason to budget engineering time for it deliberately rather than treat it as a free upgrade over a fixed chain.

### 2.3 Recommendation

Adopt `microcosm.md` rec #11 as originally stated — keep Modulation → Reverb → Filter as the *default* order (it is a well-tested, musically sensible default, and matches what most users expect from "the granular pedal" genre) — but implement the chain internally as an ordered, bypassable stage list from day one rather than as three hardcoded function calls. The marginal CPU cost of doing so is small (§2.2.1–2.2.2); retrofitting it later, after presets and a fixed calling convention already exist, is the expensive path.

---

## 3. Filter: state-variable / morphing filter

### 3.1 DaisySP `Svf` — verified from source (both `svf.h` and `svf.cpp` read in full)

**License:** MIT, core DaisySP (not the LGPL submodule). Credits: Andrew Simper (musicdsp.org, original topology), Laurent de Soras (stability limit), Stefan Diedrichsen (corrected notch output), ported by Stephen Hensley.

**It is a *double-sampled* SVF** — `Process()` runs the entire state-update recurrence **twice** per input sample and averages the two passes' outputs:
```cpp
// first pass
notch_ = input_ - damp_ * band_;
low_   = low_ + freq_ * band_;
high_  = notch_ - low_;
band_  = freq_ * high_ + band_ - drive_ * band_ * band_ * band_;
out_low_ = 0.5f * low_;  out_high_ = 0.5f * high_;  out_band_ = 0.5f * band_;
out_peak_ = 0.5f * (low_ - high_);  out_notch_ = 0.5f * notch_;
// second pass — identical structure, same input — then accumulate (+=) into the out_* variables
```
This is a well-known technique (oversampling the recurrence 2x internally, without oversampling the *codec* I/O) that trades roughly double the per-sample arithmetic for a much more stable filter at high cutoff/resonance settings than the naive single-pass "Chamberlin" SVF — directly relevant to a "morphing filter with resonance" wishlist item, since resonance is exactly where a naive SVF becomes unstable.

**All five outputs are always computed, every call — LP/HP/BP/Notch/Peak are free once you've paid for one.** `Low()`, `High()`, `Band()`, `Notch()`, `Peak()` are all `inline` accessors reading already-computed member state; there is no way to compute "just LP" more cheaply than computing all five, because the recurrence itself produces `low_`, `high_`, and `band_` as its core state and the other two (`notch_`, `peak_`) are one subtraction/access away. **This directly answers the "cost a state-variable/morphing filter with resonance and LP/BP/HP/notch selection" research question: the morphing/selection UI feature (`microcosm.md` wishlist item 7) has zero incremental DSP cost over a fixed LP-only filter using the same class** — the entire cost is in exposing a mode-select or crossfade-between-outputs parameter, not in additional signal processing.

**Op count, derived from the verified `Process()` body:** per pass, `band_`'s update alone needs `band_*band_*band_` (2 muls) plus the `freq_*high_` and `drive_*(...)` terms (2 more muls) and 2 adds — call the whole four-line recurrence **~9 float ops per pass**. Output scaling (`0.5f * X` for 5 outputs) adds **5 more muls per pass** if all 5 outputs are read; fewer if only 1–2 outputs are actually used downstream (the multiply is only paid for outputs actually accessed, since they're separate statements, not a fused compute-all block — worth confirming this optimizes away unused outputs in practice, but the source structure allows it). **Total: roughly (9 + up to 5) × 2 passes ≈ 28–35 float ops per sample per filter instance.** For a stereo filter (2 instances) that's **~56–70 float ops/sample**, a small fraction of a percent of the 10,000-cycle budget.

**Parameter-change cost:** `SetFreq()`/`SetRes()` recompute `freq_` (one `sinf` call) and `damp_` (one `powf` call) — meaningfully more expensive than the per-sample `Process()` call, but these only need to run when the cutoff/resonance parameter actually changes (typically once per control-rate tick, not once per audio sample), so they don't belong in the audio-rate budget at all if the engine is architected to update filter coefficients at control rate and only run `Process()` at audio rate. [svf.h](https://github.com/electro-smith/DaisySP/blob/master/Source/Filters/svf.h), [svf.cpp](https://github.com/electro-smith/DaisySP/blob/master/Source/Filters/svf.cpp).

### 3.2 Recommendation

Adopt an `Svf`-class double-sampled state-variable filter (either the DaisySP class directly — it's MIT, no licensing complication — or a from-scratch equivalent using the same verified topology) as Brainscape's post-chain filter, with a **continuous morph parameter that crossfades between the LP/BP/HP/Notch outputs** rather than a discrete 4-way switch, directly answering the community's #7 wishlist item at effectively zero extra DSP cost. Keep coefficient recalculation (`SetFreq`/`SetRes`, which call `sinf`/`powf`) at control rate, not audio rate.

---

## 4. Looper as a real subsystem

### 4.1 What `DaisySP::Looper` actually provides — verified in full from source

The class (`Source/Utility/looper.h`, MIT) is genuinely useful as a **buffer read/write/crossfade primitive**, and genuinely insufficient as a "looper" in the Microcosm sense on its own:

**What it has:**
- `Init(float* mem, size_t size)` — takes a raw pointer + sample count, so it's agnostic to *where* the memory lives (SRAM/SDRAM/desktop heap), which is the right shape per `vst-and-shared-dsp.md`'s buffer-provisioning-seam principle.
- A 4-state machine (`EMPTY → REC_FIRST → PLAYING → REC_DUB`) driven entirely by repeated calls to `TrigRecord()` — this *is* the Microcosm's "first press records, second press plays, third press overdubs" gesture, already built.
- Four **mix modes** at overdub time: `NORMAL` (infinite additive overdub — sig grows without bound if never re-normalized), `ONETIME_DUB` (records exactly one pass over the existing loop then auto-returns to `PLAYING`), `REPLACE` (overwrite instead of add), `FRIPPERTRONICS` (additive overdub with a fixed `sin(π/4) ≈ 0.7071` decay multiplier per pass — a self-decaying tape-loop emulation, distinct from all three others).
- A **1200-sample (25 ms @ 48 kHz) crossfade window**, verified as the constant `kWindowSamps = 1200`, applied via a `sin(π/2 · x)` law both at the start of the initial recording and at every loop-point crossing during overdub, to avoid a click at the seam.
- Basic **half-speed and reverse** flags (`half_speed_`, `reverse_`) that scale/negate the per-sample position increment.

**What it does not have, verified by absence from the source:**
- **Stereo.** `Process(const float input)` takes and returns one float. A stereo phrase looper needs two independent instances (doubling both the SDRAM footprint and, trivially, the per-sample CPU) sharing a single transport/state controller so L and R stay locked to the same position, mode, and speed.
- **Undo.** There is no operation that reverts to "the loop before the last overdub." `NORMAL` and `FRIPPERTRONICS` modes mix new material into the buffer **destructively and irreversibly** — the only way back to a prior state is `Clear()` (which discards everything, verified: it only flips `state_` to `EMPTY` without touching buffer contents, so it's actually just "stop treating this as valid," not even a real erase). **Matching the Microcosm's documented undo behavior (§6 of `microcosm.md`: "keeps the initial phrase, erases the Overdub Layer") requires keeping the initial phrase and the most recent overdub in physically separate buffers** — see §4.3.
- **Quantize.** No tempo/subdivision awareness anywhere in the class; `recsize_` is set purely by however many samples elapsed between the first and second `TrigRecord()` calls. Quantizing to a musical grid is entirely external logic that must trim (or, better, time-stretch — see §4.4) `recsize_` after the fact.
- **Interpolated fractional playback in the main path.** `ReadF(float pos)` — a proper 2-point linearly-interpolated read — **exists in the class but is never called by `Process()`**; the actual playback path calls `Read(pos)`, whose signature takes `size_t pos`, meaning the float `pos_` is **implicitly truncated to an integer** every time it's read in `PLAYING`/`REC_DUB` states. This is fine at `increment_size = 1.0` or exactly `0.5` (`half_speed_`), the two cases the class ships pre-built support for, but **wiring in `SetIncrementSize()` for arbitrary 1/4x–4x varispeed without also switching the render path to `ReadF()` will alias/glitch** — a genuine, source-verified gotcha for anyone extending this class for continuous varispeed rather than the two built-in speeds.
- **Pre/post-FX routing.** Purely an *external* architectural decision — the class has no concept of where in a signal chain it sits; see §4.6.
- **Sample-rate independence.** `IsNearBeginning()` hardcodes `pos_ < 4800` with no reference to an actual sample rate — silently wrong at any rate other than 48 kHz (see Summary bullet; also true of `ChorusEngine`'s `kDelayLength`).

### 4.2 Buffer format and sizing for multi-minute stereo

Following `grain-delay-theory.md`'s own per-second memory table (int16 stereo = 192 KB/s at 48 kHz), a genuinely "multi-minute, longer than the Microcosm's 60 s" single stereo loop:

| Loop length | Format | Size |
|---|---|---|
| 60 s (Microcosm parity) | int16 stereo | 11.25 MiB |
| 180 s (3 min) | int16 stereo | 33.75 MiB |
| 245 s (~4.1 min) | int16 stereo | 46 MiB |
| 60 s | float32 stereo | 22.5 MiB |

**Two independent mono `DaisySP::Looper` instances** (one per channel, sharing one transport controller) is the pragmatic implementation path — it reuses the verified, working buffer/crossfade/mode-state-machine logic in §4.1 without modification, at the cost of driving two `Process()` calls per audio sample (trivial CPU, ~10–15 float ops each per the source's `Process()` body) and needing to keep both instances' `TrigRecord()`/mode/position calls perfectly synchronized from the calling code.

### 4.3 Overdub mixdown and undo design

The Microcosm's documented behavior — *"Undo... erases the Overdub Layer, retains the initial phrase"* (`microcosm.md` §6) — requires a **two-buffer design that `DaisySP::Looper` alone cannot provide**, because its `NORMAL`/`FRIPPERTRONICS` modes overdub additively into the *same* buffer the initial phrase lives in, with no separate record of "before this overdub."

**Recommended design:**
1. **Buffer A: the committed phrase** (the initial recording, or the result of the last "mixed down" overdub).
2. **Buffer B: the active overdub layer**, same length as A, cleared to silence when overdub starts.
3. **Playback** reads `A[pos] + B[pos]` (or crossfades, matching `microcosm.md`'s "mixed down into a single loop file" language for what happens *after* undo is no longer possible).
4. **Undo** = discard B (clear it, or simply stop mixing it in) — instant, sample-accurate, and matches the Microcosm's documented one-level-only undo exactly.
5. **Committing an overdub** (starting a *new* overdub layer, or saving the preset) = mix B into A (`A[i] += B[i]`, with whatever normalization prevents unbounded gain growth across many committed overdubs — the Microcosm's "mixed down into a single loop file" note in `microcosm.md` §7 implies this exact commit step happens at preset-save time) and clear B for the next layer.

**Memory cost of this design: 2x one loop-slot's buffer**, since B must be sized identically to A even though it's silent for most of a performance. For the 46 MiB single-loop allocation in §4.2/§6, this means the "one loop slot" line item is actually **2× 46 MiB, i.e. the two-buffer undo design and "one long loop" are the same memory commitment either way** (both need to reserve worst-case A+B up front, since a performer might overdub at any point) — a detail worth surfacing explicitly, since a naive single-buffer implementation would look like it saves memory but cannot deliver Microcosm-parity undo.

**CPU cost:** the extra "commit" mixdown (`A[i] += B[i]` over the whole buffer) is a rare, non-real-time operation (happens once per overdub-commit gesture, not per audio sample) and can run at a leisurely pace outside the audio ISR — it is not part of the per-sample budget in §6.

### 4.4 Quantize-to-effect-tempo design

Two implementation strategies, with different cost profiles:

1. **Trim (Microcosm's own documented approach — "trims the loop to the nearest quarter note"):** at the moment recording stops, round `recsize_` to the nearest multiple of the current tempo's sample-count-per-subdivision, and either pad with silence or truncate the tail. **Cost: effectively free** — a single integer rounding operation performed once, at the state transition from `REC_FIRST`/`REC_DUB` to `PLAYING`, not a per-sample cost. **Downside (inherited from the Microcosm, and worth fixing):** trimming discards or pads real recorded content rather than preserving it, which is the source of the "sometimes it needs a couple of seconds" timing feel some Microcosm reviews describe.
2. **Time-stretch to fit** (better, not present in the Microcosm as documented): instead of trimming, adjust the effective playback increment so the *existing* `recsize_` samples play back over exactly the quantized duration. This preserves 100% of what was played but introduces a small, usually-inaudible pitch/rate deviation (typically well under a semitone for any reasonable quantization tolerance) and **requires the interpolated `ReadF()` path** (§4.1's alias-risk gotcha) rather than the integer-truncating default path, since the increment will now essentially never be exactly 1.0.

**Recommendation:** ship (1) for v1 parity with the Microcosm's proven, well-liked behavior, with (2) as a stretch goal once the interpolated-read path is validated — both are cheap enough that CPU is never the deciding factor here; the deciding factor is implementation risk (aliasing) vs. musical feel (content preservation).

### 4.5 Varispeed (1/4x–4x) and reverse

`GetIncrementSize()` already returns `increment_size * (half_speed_ ? 0.5 : 1.0) * (reverse_ ? -1 : 1)`, and `SetIncrementSize()` is public — so the *plumbing* for arbitrary varispeed already exists in the class. **The blocking issue, verified in §4.1, is that the main render path does not call the interpolated `ReadF()`.** Extending the class (or writing a stereo-aware equivalent) to route through `ReadF()` whenever `increment_size` is not exactly ±1.0 or ±0.5 is a small, well-scoped code change with a well-understood cost: one extra float subtract/multiply/add per sample for the linear interpolation (~3 ops), applied uniformly rather than only at the two special-cased speeds. This is trivial against the budget in §6 and should simply be done rather than treated as a design tradeoff.

### 4.6 Pre/post-FX routing — resolving `microcosm.md` open question #7

`microcosm.md`'s open question #7 asks whether Pre-FX mode changes what the looper *records* (raw dry input) or only where it plays back, since the manual's diagram doesn't disambiguate and the manual's prose ("recorded material will be sent through effect processing... without altering the recorded material") only implies an answer.

**This is a pure signal-routing question, not a DSP-cost question — there is no meaningful CPU or memory difference between the two interpretations**, and Brainscape does not need to guess: since the looper is decoupled from the granular engine (a separate module reading/writing its own buffer, per §4.1's architecture), **both the Microcosm's exact behavior and richer, user-visible alternatives are equally cheap to implement.** Concretely, expose Pre-FX/Post-FX as a literal tap-point selector for **where the looper's record-input is sourced from** (dry input vs. post-granular-engine output) and, independently, **where the looper's playback-output is injected back into the chain** (before or after the granular engine) — i.e. don't collapse this into one binary switch at all. This gives four combinations instead of the Microcosm's two, at zero extra DSP cost (it's just which buffer pointer feeds the looper's `Process()` call and which point in the chain reads its output), directly satisfying `microcosm.md` rec #6's "keep pre/post-FX routing... verbatim" while also resolving the ambiguity the Microcosm itself never had to answer publicly. The resample workflow `microcosm.md` §6 highlights (record post-FX, then switch to pre-FX to re-process the loop through the granular engine) is a special case of this four-way matrix, not a separate feature.

### 4.7 Saving a loop into a preset: flash/SD costing

**QSPI flash is the wrong medium for loop audio, verified from source.** `libDaisy`'s `qspi.h` documents 4 KB sector / 64 KB block erase granularity, and `PersistentStorage<T>::Save()` (in `util/PersistentStorage.h`, verified in full) is explicitly designed and commented for a small, fixed-size `SettingStruct` — its own doc comment lists `\todo - Make Save() non-blocking` as an open item, i.e. **the shipped implementation blocks the calling thread for the duration of an erase+write**, acceptable for a parameter struct measured in bytes-to-kilobytes but not for megabytes of loop audio. The Daisy Seed's QSPI flash is **8 MB total** (`daisy-seed-platform.md`, confirmed part number ISSI IS25LP064), of which a meaningful fraction is already claimed by firmware itself when using the bootloader+QSPI-XIP path recommended for a feature-rich pedal. A single 60-second stereo loop at int16 (11.25 MiB, per §4.2) **does not fit in the entire flash chip**, let alone one loop per preset across 16+ presets as the Microcosm does with its own (unknown, likely much larger or purpose-built) internal storage.

**SD card is the correct medium, and it's a first-class, already-supported path — not a from-scratch integration project.** Verified directly from the `libDaisy` repository tree: it ships a complete `Middlewares/Third_Party/FatFs` (full FatFs source) plus `src/per/sdmmc.{h,cpp}`, `src/sys/fatfs.{h,cpp}`, and an `examples/SDMMC_HelloWorld` reference project. SD cards on the STM32H7's SDMMC peripheral sustain multiple MB/s of sequential write on any reasonably modern card — vastly more than the ~192 KB/s (int16 stereo, real-time) or even the burst rate needed to write out a whole loop buffer in a few hundred milliseconds after the fact — so **write throughput is not the constraint; adding the physical SD slot, wiring it to the SDMMC pins, and writing the loop-file format is the actual work**, and it's work with a well-worn path (FatFs is arguably the most common embedded filesystem in existence). **Recommendation: Brainscape's hardware should include a microSD slot from v1** if "save a loop into a preset" is a committed feature — it is not optional the way it might appear from the DaisySP/libDaisy software layer alone, because the built-in flash is categorically too small. [libDaisy `qspi.h`](https://github.com/electro-smith/libDaisy/blob/master/src/per/qspi.h), [libDaisy `PersistentStorage.h`](https://github.com/electro-smith/libDaisy/blob/master/src/util/PersistentStorage.h), [libDaisy `QSPI_EraseTiming` example](https://github.com/electro-smith/libDaisy/blob/master/examples/QSPI_EraseTiming/main.cpp) (demonstrates the erase-timing test harness; exact millisecond figures are runtime `printf` output, not present in the source itself, so treat specific erase/write durations as **unverified** pending an actual hardware run — but 4 KB sector erases on this flash family are typically tens of milliseconds and 64 KB block erases several hundred milliseconds, per standard NOR-flash datasheet behavior for this part class), [libDaisy repo tree (FatFs/SDMMC confirmation)](https://github.com/electro-smith/libDaisy).

---

## 5. Freeze / hold sampler, and coexistence with the looper

### 5.1 What the Microcosm's Hold Sampler needs, functionally

Per `microcosm.md` §5: captures a recent segment of audio, **continuously re-feeds it into the effects section** (so the granular engine keeps processing static material rather than holding a static *output*), persists across changing effects/parameters, and is documented as mutually exclusive with the Phrase Looper for no stated technical reason.

### 5.2 The zero-extra-memory design

Because `grain-delay-theory.md`'s own recommended architecture already keeps **a continuously-written circular history buffer** as the substrate every grain reads from (that's the entire premise of "grain delay" as opposed to "static-buffer granular synthesis," per that document's §2), **freeze does not need a buffer of its own at all**:

- **Normal operation:** the grain scheduler resolves each new grain's read position relative to the buffer's live write head (moving forward with real time).
- **Freeze engaged:** the scheduler instead resolves new grains' read positions relative to a **pinned reference point** — the write-head position at the instant freeze was engaged — so every subsequently-scheduled grain reads from the same fixed window of already-captured audio, no matter how much real time passes. The granular engine keeps running exactly as before (grains still fire, still pitch-shift, still reverse, per whatever mode is active) — it simply stops being fed *new* material, matching the Microcosm's "the effect keeps dynamically processing it" behavior precisely.
- **Whether the underlying buffer keeps being overwritten by live input in the background during freeze is a free implementation choice** (it doesn't matter as long as the frozen window itself isn't overwritten before the buffer wraps around — at 16 MiB / 87 s of history, per §6, a freeze held far longer than that would eventually be overwritten by the circular buffer's own wraparound, which is an acceptable, documentable limit rather than a design flaw).

**Cost: effectively zero incremental SDRAM, and effectively zero incremental CPU** — it is a mode flag on the existing grain scheduler (one comparison/branch to decide whether "now" advances the reference position or not), not a new DSP module.

### 5.3 Why this also resolves the "no architectural justification" claim

`microcosm.md` §5 asserts the Microcosm's freeze/looper mutual exclusivity "has no architectural justification." On the design in §5.2, this is not just plausible but **demonstrable**: freeze is a mode of the grain engine's own always-on history buffer, while the Phrase Looper (§4) is a **completely separate buffer and state machine** with its own SDRAM allocation. The two features don't compete for the same memory, don't share any mutable state, and there is no data-race or buffer-aliasing hazard forcing one to disable the other — **the only way they'd need to be mutually exclusive is if a from-scratch implementation deliberately reused one buffer for both purposes to save memory**, which §6's budget shows is unnecessary at Brainscape's target buffer sizes. This directly and quantitatively confirms the claim the earlier research corpus made qualitatively.

### 5.4 If freeze needs to work in a non-granular patch too

If some future Brainscape mode has no grain engine running at all (e.g., a pure delay/reverb-only patch) and therefore no always-on history buffer, freeze in that context would need a small dedicated capture buffer instead. Even a generous **4-second stereo int16 buffer is only ~1.5 MiB** — trivial against the budget in §6 — so this fallback case, if needed, does not change the overall memory picture materially. **Recommendation: keep a continuously-written history buffer active in every mode** (not just granular ones) specifically so freeze is always the zero-cost, zero-extra-buffer version from §5.2 rather than needing this fallback at all.

---

## 6. Consolidated system budget

### 6.1 SDRAM (64 MiB total)

| Item | Recommended size | Rationale |
|---|---|---|
| Grain/history buffer | **16 MiB** (≈87.4 s stereo int16, per `grain-delay-theory.md`'s own 2²² -frame table) | Already far longer than any documented granular pedal's working window (Microcosm reportedly needs only "a couple of seconds" of fill before effects engage); doubles as the freeze source (§5) at zero extra cost |
| Phrase looper (committed phrase, Buffer A) | **23 MiB** (≈2 min stereo int16) | See note below on undo doubling |
| Phrase looper (active overdub layer, Buffer B) | **23 MiB** (matches A, per §4.3's undo design) | Needed for instant, Microcosm-parity undo; without it, halve this line and lose undo |
| Reverb tank | **0.1–0.2 MiB** if kept in SDRAM for implementation convenience (0 MiB if placed in internal SRAM per §1.6, the recommended choice) | ~50–150 KiB per §1.2–1.4; negligible either way |
| Modulation delay lines (stereo chorus-style, `kDelayLength`-class) | **~0.02 MiB** (2 × 2400 floats, per verified `ChorusEngine` source) if in SDRAM (0 MiB if in internal SRAM, the recommended choice) | Trivial |
| **Total (grain 16 + looper 46 + reverb 0.2)** | **≈62.2 MiB of 64 MiB** | **~1.8 MiB headroom** — tight but fits, with reverb/modulation ideally moved to internal SRAM to widen this margin further |

**This is the concrete reconciliation the task brief asked for.** `grain-delay-theory.md` §5.2 offers a **32 MiB** grain buffer option (175 s); `microcosm.md` §13.5 separately asks for a multi-minute, multiple-slot looper *and* simultaneous freeze, without adding the two together. At stereo int16 (192,000 B/s ≈ 0.1831 MiB/s), the arithmetic works out as follows:

| Grain buffer choice | Left for looper (after reverb/mod) | Single loop, no undo | Single loop, with two-buffer undo (§4.3) | 4 slots, with undo |
|---|---|---|---|---|
| 16 MiB (recommended, §6.1 above) | ~47.8 MiB | ~261 s (4.3 min) | ~130 s (2.2 min) per slot's worth if kept as one slot (this doc's table above rounds down to 23+23 MiB for headroom, giving ~126 s) | ~33 s/slot |
| 32 MiB (`grain-delay-theory.md`'s higher option) | ~31.8 MiB | ~174 s (2.9 min) | ~87 s (1.5 min) | ~22 s/slot |

**Both grain-buffer choices still comfortably clear the Microcosm's 60-second single-loop parity even with full undo** — the earlier, stronger claim that 32 MiB "barely" clears parity was a false start in an earlier pass of this analysis and is corrected here. **Where the two recommendations genuinely start to conflict is the *combination* of a large grain buffer, multiple loop slots, undo, and generous per-slot length all at once**: at 32 MiB for grains, four undo-capable slots only get ~22 s each — noticeably short for a "longer than Microcosm" ambition applied per-slot. **Recommendation: if multiple undo-capable slots at generous individual length is the goal, prefer the 16 MiB grain-buffer option** (still ~87 s of grain history, itself far beyond what any documented granular pedal needs) so the looper's ~47.8 MiB stretches to ~33 s per slot across four undo-capable slots, or a single ~2-minute undo-capable slot if slot count is deprioritized. **This is a genuine product decision — how many slots, how long each, whether undo is universal — not a technical constraint Brainscape can engineer around for free, but it is a substantially more forgiving tradeoff space than the task brief's framing implied.**

### 6.2 CPU (10,000 cycles/sample budget at 48 kHz / 480 MHz)

| Component | Estimated cycles/sample | % of budget | Basis |
|---|---|---|---|
| 64 grains, cache-hot, mitigations applied | ~1,280–1,600 | 12.8–16% | `grain-delay-theory.md` §5.3–5.4 (carried forward, not re-derived here) |
| Reverb (`ReverbSc`-class 8-line FDN, **in internal SRAM**) | ~500–800 | 5–8% | Derived in §1.2 from verified `Process()` op count |
| Stereo state-variable filter (2× `Svf`, double-sampled) | ~56–70 | <1% | Derived in §3.1 from verified `Process()` op count |
| Stereo modulation (2× chorus/vibrato-class engine) | ~40–60 | <1% | Estimated from verified `ChorusEngine` structure (delay read/write + LFO, ~15–20 ops/engine) |
| Onset detection (simple envelope/derivative, not spectral-flux) | ~80–120 | ~1% | **Estimated, not measured — no dedicated Brainscape onset-detection research exists yet; flagged as an open question below** |
| Stereo phrase looper (2× `Looper`-class read/write/crossfade) | ~40–60 | <1% | Estimated from verified `Process()` body (~10–15 ops/instance/sample) |
| **Subtotal** | **≈2,000–2,700** | **≈20–27%** | |
| UI/MIDI (encoders, LEDs, MIDI parse) | *not counted against this budget* | 0% (by design) | Should run at control rate (e.g. once per block or slower) inside the same ISR window only as a small fixed tail, or be deferred to a lower-priority context entirely — not a per-sample cost |
| **Headroom remaining** | **≈7,300–8,000** | **≈73–80%** | For safety margin, future features, and the "big ambient" CloudSeed-class reverb mode (§1.5, §1.7) when the primary reverb is bypassed in favor of it |

**This is a derived estimate, explicitly not a hardware measurement**, consistent with how `grain-delay-theory.md` itself frames its own grain-cost numbers (cycle-counting from source structure, cache-miss penalty bounded but "unverified — needs measurement on hardware"). The individual per-component derivations in this document (§1.2, §3.1) are each traceable to a specific, fully-read source file's actual `Process()` implementation, which is a firmer basis than the earlier corpus's "the grain engine alone" costing — but **none of it substitutes for putting a DWT cycle counter or a GPIO-toggle-plus-scope measurement (both documented, standard techniques on the Daisy forum) around the real implementation once it exists.**

---

## 7. Resolving the compile-time-vs-runtime block size contradiction

**The contradiction is resolved by primary source: block size is a runtime value on the actual firmware platform, full stop.** Verified directly from `libDaisy`'s `src/hid/audio.h`:

```cpp
struct Config {
    size_t blocksize = 48;                                    // runtime field, not a template/#define
    SaiHandle::Config::SampleRate samplerate = SAI_48KHZ;      // also a runtime field
    ...
};
...
Result SetSampleRate(SaiHandle::Config::SampleRate samplerate); // callable after Init()
Result SetBlockSize(size_t size);                                // callable after Init()
```

`blocksize` is a **struct member with a default value**, and `SetBlockSize()`/`SetSampleRate()` are **public methods that can be called after initialization** to change it. There is no `constexpr`, no template parameter, no `#define` anywhere in this API. **`grain-delay-theory.md`'s recommendation ("make block size a compile-time constant so loops unroll") is not just in tension with `vst-and-shared-dsp.md` — it's inconsistent with how the Daisy Seed's own official HAL actually works**, and would require fighting the platform (e.g. asserting/crashing if the host or a future firmware config ever requested a different block size) to hold.

**What `grain-delay-theory.md`'s recommendation was actually reaching for is real and worth preserving, just via a different mechanism:**

1. **`dsp/`'s public interface stays exactly as `vst-and-shared-dsp.md` rec #3 specifies**: `Init(float sampleRate, size_t maxBlockSize)` (allowed to allocate, sizes all scratch buffers for the worst case) and `Process(..., size_t numFrames)` where `numFrames <= maxBlockSize` and can vary block-to-block. This is what both the Daisy's own runtime-configurable block size *and* a desktop host's block size (which commonly varies transparently, e.g. shrinking at buffer boundaries) actually require.
2. **Loop-unrolling / cache-locality gains are still available**, but at a smaller, internal granularity: the per-grain render loop (`grain-delay-theory.md` §5.4's recommendation to "render per-grain over the whole block into a DTCM float accumulator") can chunk its inner loop into a **fixed-size compile-time sub-block** (e.g. process 8 or 16 samples at a time within whatever `numFrames` actually is that block), which the compiler can unroll and vectorize, **without that constant ever appearing in the public `Process(numFrames)` signature or constraining what `numFrames` is allowed to be.** This gets the cache/unrolling benefit `grain-delay-theory.md` was after without baking a platform assumption into the shared core's contract.
3. **Concretely: pick a real default for the *firmware build's own configuration*** — `daisy-seed-platform.md` already documents the Daisy default as **48 samples @ 48 kHz ⇒ 1 ms** — and set `AudioHandle::Config::blocksize = 48` accordingly in `firmware/`'s `main.cpp`. That default belongs in `firmware/`, as a value passed into `dsp/`'s `Init(sampleRate, maxBlockSize)` call, exactly as `vst-and-shared-dsp.md`'s architecture already prescribes — it does not belong inside `dsp/` itself as a baked-in constant.

**Net: no change is needed to `vst-and-shared-dsp.md`'s recommended `dsp/` interface. `grain-delay-theory.md`'s rec should be understood/amended as "chunk the inner grain-render loop at a fixed compile-time granularity for unrolling," not "make the callback block size itself a compile-time constant" — the latter is now shown to be factually wrong about how the target platform's own HAL behaves.**

---

## Recommendations for Brainscape

1. **Treat `ReverbSc` as LGPL-2.1, licensed and disclosed accordingly**, not as ordinary MIT DaisySP — and either resize its buffer to the actual runtime sample rate or explicitly place it in SDRAM if used as-is. Do not silently vendor it as if it were core DaisySP.
2. **Build (or adapt from Clouds' MIT-licensed `reverb.h`) a Dattorro/Griesinger-topology reverb with input diffusion**, sized at ~50–150 KiB, and **place its tank buffer in internal SRAM, not SDRAM** — supported by both this document's own derivation and a real community-measured 3.5x CPU penalty for a similarly-sized reverb wrongly placed in SDRAM.
3. **Reserve a heavier, CloudSeed-class network reverb as an optional/alternate mode**, following GuitarML's own real-world precedent of offering both a cheap default and an expensive alternate on this exact hardware class — gate it against the CPU budget in §6.2 rather than assuming it's free to add.
4. **Implement the post-chain (modulation → reverb → filter) as an ordered, per-stage-bypassable list from day one**, keeping that order as the default, per `microcosm.md` rec #11 — the CPU cost is negligible; the design/preset-schema cost is real and should be budgeted for explicitly rather than discovered later.
5. **Use a double-sampled state-variable filter (DaisySP's `Svf` or an equivalent) with a continuous LP↔BP↔HP↔Notch morph parameter** — this satisfies the community's #7 wishlist item at effectively zero incremental DSP cost over a fixed LP-only filter, since the class computes all four outputs unconditionally.
6. **Do not reuse `DaisySP::Looper` unmodified for anything beyond its verified scope** (mono, no undo, no quantize, non-interpolated main playback path). Build a stereo wrapper with: two-buffer (committed-phrase + active-overdub-layer) undo per §4.3, trim-based quantize for v1 per §4.4, an interpolated (`ReadF`-routed) playback path before shipping any varispeed beyond the built-in 0.5x/1x/reverse, and a four-way (not two-way) pre/post-FX record/playback tap-point matrix per §4.6.
7. **Fix the two verified sample-rate-hardcoding bugs before copying these classes' patterns elsewhere**: `DaisySP::Looper`'s `pos_ < 4800` and `ChorusEngine`'s `kDelayLength = 2400` are both silently wrong off 48 kHz. Treat every DaisySP/DaisySP-LGPL class Brainscape reuses as a candidate for this same audit, not just these two.
8. **Include a microSD slot in the hardware from v1** if "save a loop into a preset" ships — the on-board 8 MB QSPI flash cannot hold even one 60-second stereo loop, and `libDaisy` already ships full FatFs/SDMMC support, so this is an integration cost (physical slot + wiring + file-format design), not a from-scratch software project.
9. **Implement freeze as a pinned-reference-point mode on the grain engine's own always-on history buffer (§5.2), not as a separate DSP module or buffer.** This costs effectively nothing in CPU or SDRAM and structurally guarantees freeze and the phrase looper can run simultaneously, resolving `microcosm.md` §5's claim with an actual design rather than just agreeing with it.
10. **Pick the 16 MiB grain-buffer option, not the 32 MiB one, if a genuinely multi-minute looper with undo is a committed feature** — the two do not both fit generously at the 32 MiB grain-buffer size (§6.1). This is a product decision to make consciously now, not a budget surprise to discover during firmware bring-up.
11. **Resolve the compile-time/runtime block-size question in `vst-and-shared-dsp.md`'s favor, unamended, at the public `dsp/` interface** (§7) — it is not just the better architectural choice, it is what the Daisy's own official HAL already does. Recover `grain-delay-theory.md`'s legitimate unrolling goal via an internal, fixed-size inner-loop chunk size instead.
12. **Measure, don't just estimate, once real code exists.** Every CPU figure in §6.2 beyond the grain-engine numbers already in `grain-delay-theory.md` is a derivation from verified source structure, not a hardware measurement — put a DWT cycle counter (documented, standard technique per the Daisy community) around each stage as it's implemented.

---

## Sources

**DaisySP / DaisySP-LGPL (all fetched and read in full via the GitHub API during this research)**
- [DaisySP repo](https://github.com/electro-smith/DaisySP)
- [DaisySP `.gitmodules`](https://github.com/electro-smith/DaisySP/blob/master/.gitmodules)
- [DaisySP `daisysp.h` (module include list)](https://github.com/electro-smith/DaisySP/blob/master/Source/daisysp.h)
- [DaisySP `Source/Filters/svf.h`](https://github.com/electro-smith/DaisySP/blob/master/Source/Filters/svf.h)
- [DaisySP `Source/Filters/svf.cpp`](https://github.com/electro-smith/DaisySP/blob/master/Source/Filters/svf.cpp)
- [DaisySP `Source/Utility/looper.h`](https://github.com/electro-smith/DaisySP/blob/master/Source/Utility/looper.h)
- [DaisySP `Source/Effects/chorus.h`](https://github.com/electro-smith/DaisySP/blob/master/Source/Effects/chorus.h)
- [DaisySP `Source/Utility/dcblock.h`](https://github.com/electro-smith/DaisySP/blob/master/Source/Utility/dcblock.h)
- [DaisySP `Source/Effects/phaser.h`](https://github.com/electro-smith/DaisySP/blob/master/Source/Effects/phaser.h)
- [DaisySP-LGPL repo](https://github.com/electro-smith/DaisySP-LGPL)
- [DaisySP-LGPL `LICENSE` (LGPL-2.1)](https://github.com/electro-smith/DaisySP-LGPL/blob/main/LICENSE)
- [DaisySP-LGPL `README.md`](https://github.com/electro-smith/DaisySP-LGPL/blob/main/README.md)
- [DaisySP-LGPL `Source/Effects/reverbsc.h`](https://github.com/electro-smith/DaisySP-LGPL/blob/main/Source/Effects/reverbsc.h)
- [DaisySP-LGPL `Source/Effects/reverbsc.cpp`](https://github.com/electro-smith/DaisySP-LGPL/blob/main/Source/Effects/reverbsc.cpp)

**libDaisy (fetched and read via the GitHub API)**
- [libDaisy repo](https://github.com/electro-smith/libDaisy)
- [libDaisy `src/hid/audio.h`](https://github.com/electro-smith/libDaisy/blob/master/src/hid/audio.h)
- [libDaisy `src/per/qspi.h`](https://github.com/electro-smith/libDaisy/blob/master/src/per/qspi.h)
- [libDaisy `src/util/PersistentStorage.h`](https://github.com/electro-smith/libDaisy/blob/master/src/util/PersistentStorage.h)
- [libDaisy `examples/QSPI_EraseTiming/main.cpp`](https://github.com/electro-smith/libDaisy/blob/master/examples/QSPI_EraseTiming/main.cpp)
- [libDaisy `src/dev/sdram.cpp`](https://github.com/electro-smith/libDaisy/blob/master/src/dev/sdram.cpp)

**Mutable Instruments Clouds (fetched and read via the GitHub API)**
- [eurorack repo](https://github.com/pichenettes/eurorack)
- [`clouds/dsp/fx/reverb.h`](https://github.com/pichenettes/eurorack/blob/master/clouds/dsp/fx/reverb.h)
- [`clouds/dsp/fx/fx_engine.h`](https://github.com/pichenettes/eurorack/blob/master/clouds/dsp/fx/fx_engine.h)

**Other reverb references**
- [LMMS `ReverbSC/revsc.c`](https://github.com/LMMS/lmms/blob/master/plugins/ReverbSC/revsc.c) (parallel Csound-derived port, used to cross-check `ReverbSc` delay-line lengths and origin attribution)
- [Faust `reverbs.lib` (`dattorro_rev`)](https://github.com/grame-cncm/faustlibraries/blob/master/reverbs.lib)
- [Dattorro's original 1997 paper](https://ccrma.stanford.edu/~dattorro/EffectDesignPart1.pdf)
- [GuitarML/DaisyCloudSeed](https://github.com/GuitarML/DaisyCloudSeed)
- [erwincoumans/DaisyCloudSeed](https://github.com/erwincoumans/DaisyCloudSeed)
- [Daisy Forum: DaisyCloudSeed lush reverb](https://forum.electro-smith.com/t/daisycloudseed-lush-reverb/522)

**Daisy platform / community**
- [Daisy Community: 64MB of SDRAM for up to 10-minute-long audio buffers (source of the 3.5x SRAM/SDRAM figure, carried forward from `daisy-seed-platform.md`; returned HTTP 403 on direct re-fetch during this research)](https://community.daisy.audio/t/64mb-of-sdram-for-up-to-10-minute-long-audio-buffers/4422)
- [Daisy Forum/Community: MCU utilization measurement thread](https://community.daisy.audio/t/solved-how-to-do-mcu-utilization-measurements/1236)

**This project's own sibling research documents (read in full, referenced throughout)**
- `docs/research/microcosm.md`
- `docs/research/grain-delay-theory.md`
- `docs/research/vst-and-shared-dsp.md`
- `docs/research/daisy-seed-platform.md`

---

## Open questions

1. **No dedicated Brainscape onset-detection research or costing exists.** The §6.2 CPU line item for onset detection (~80–120 cycles) is an unverified order-of-magnitude estimate for a simple envelope/derivative-based approach, not derived from any specific source. `microcosm.md` §3.3/13.3 flags onset detection as the Microcosm's biggest technical weakness and recommends multi-band/spectral-flux detection — that approach would cost meaningfully more (likely requiring at least a short FFT or a bank of bandpass envelope followers) than the simple estimate used here, and deserves its own dedicated research pass before the budget in §6.2 is treated as final.
2. **Exact QSPI erase/write millisecond figures were not obtained.** `libDaisy`'s `QSPI_EraseTiming` example exists and is the right tool, but its timing output is runtime `printf`, not present in source — this document's "tens of milliseconds / several hundred milliseconds" figures are inferred from standard NOR-flash datasheet behavior for this part class, not measured on this specific board. Low priority given the §4.7 conclusion (QSPI is the wrong medium for loop audio regardless of exact timing), but worth confirming if QSPI is ever used for anything larger than a settings struct.
3. **The community-reported "3.5x SRAM-vs-SDRAM" forum thread could not be independently re-fetched** (HTTP 403) to confirm the exact algorithm tested, block size, and measurement method behind the 42%/12% CPU figures cited in `daisy-seed-platform.md`. The figure is directionally consistent with this document's own derivations and with `grain-delay-theory.md`'s cache-miss model, but a from-scratch hardware measurement on Brainscape's actual reverb implementation should supersede it once available.
4. **STM32H7 Cortex-M7 L1 cache size/line size (16 KB / 32 B) is carried forward from `daisy-seed-platform.md` and `grain-delay-theory.md`**, not independently re-verified against ST's reference manual in this research pass (an attempted fetch of ST's AN4839 timed out; a Digikey datasheet mirror returned HTTP 410). Treat as high-confidence but not freshly re-confirmed.
5. **Whether Faust's `dattorro_rev` in `reverbs.lib` rescales its integer delay-line lengths for the caller's actual sample rate was not confirmed** from the fetched excerpt — the function signature exposes `bw`/`damping` as sample-rate-sensitive coefficients the caller sets, which suggests the delay-line sample *counts* themselves may be fixed at whatever rate Dattorro originally published them for. If Brainscape studies this implementation directly, confirm this before assuming the 48 kHz-rescaled figures in §1.4 are exactly how that specific Faust code would behave unmodified.
6. **The exact number of delay lines full (unmodified) CloudSeed uses, and its CPU/memory footprint on a reference desktop or embedded target, was not independently verified** — only the secondary evidence that GuitarML's port needed to cut it to mono + 5 lines to fit on Daisy-Seed-class hardware. A direct read of `ValdemarOrn/CloudSeed`'s own source would sharpen §1.5's comparison if a CloudSeed-class mode is pursued seriously.
7. **No measurement exists yet (nor could one be obtained through documentation review alone) for the actual DMA/cache-coherency cost of a stereo phrase-looper buffer that is simultaneously being written by the audio callback and read/written by a background SD-card save operation** — `daisy-seed-platform.md` flags DMA/cache coherency as a real, well-documented gotcha in general, but this document did not find or verify a specific pattern for "loop buffer being saved to SD while still potentially in use." Worth a dedicated look before finalizing the preset-save design in §4.7.
