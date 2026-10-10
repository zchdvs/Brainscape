# Output limiter: a safety ceiling at full scale

> The design pass for the output safety limiter. The audition page asked "At extreme knob corners,
> a sustained chord into an attack mode can reach +2.5 dBFS and clip the codec. Add an output
> safety limiter?", and the owner answered yes, as a sound revision of its own
> (`firmware/factory/AUDITION.md:43`, 2026-10-08). On 2026-10-09 the owner asked for the design to
> start now. This document specifies, precisely enough to build and test, a limiter that is
> inside the engine, so the pedal and the plugin play the same samples. It covers the
> algorithm, its arithmetic, where it sits, its control and display, its cost and code
> placement, its effect on the goldens and the audition, and its sound revision and order
> against CLOCK's tempo core and the cost governor.
>
> It extends [grain-engine.md](grain-engine.md) ("engine §N"),
> [determinism-profile.md](determinism-profile.md) ("profile §N"),
> [mode-compiler.md](mode-compiler.md) ("compiler §N"), [companion-app.md](companion-app.md)
> ("companion §N"), [cpu-budget.md](cpu-budget.md) ("budget §N") and [clock.md](clock.md)
> ("clock §N"). Its evidence, the three reviews of draft 1 and how each finding was disposed of
> are in [reviews/output-limiter-record.md](reviews/output-limiter-record.md) ("record §N").
>
> The evidence comes from three sources, summarised in §12:
> - three research lanes: practice in comparable products, the DSP options with a prototype,
>   and the tree's constraints;
> - draft 1's prototype, patched into `main`;
> - draft 2's prototype, the limiter of §4.3 built into the engine with statistics read from it.
>   MSVC ran the unit tests and the golden harness under eight deliveries and the forced-flush
>   control. arm-none-eabi-gcc 10.3 built the live image, whose map and disassembly give the ITCM
>   and instruction figures.
>
> The prototypes' code, scripts and logs are scratch work and are not committed (record §1.3).
> Code is cited as `path:line` at `main` `011b294` (sound revision 7), except where a citation
> names `claude/tempo-core` `b91b34d` (CLOCK's tempo core, sound revision 8, not yet merged). "LD"
> is the pinned libDaisy v9.0.0.
>
> Status: **draft 4, corrected, with the owner's last answers** (2026-10-10): draft 3, the
> owner's answers, revised after two reviews of it, then corrected for D5's form. Draft 2
> (2026-10-09) was revised after three reviews of draft 1. On 2026-10-10 the owner confirmed
> twelve of the thirteen decisions in §11.2 as recommended and **changed D5**: marked "Change" on
> the decisions page with no note, then, asked in the session what change D5 should be, chose from
> four options **a per-preset switch**, a stored Leaf (row 87, `output.limiter`, default On), so
> a mode can turn the limiter off on purpose, for example for deliberate clipping. §7.1 specifies
> it. The same day the owner answered, in the session, the three questions the corrected draft 4
> left open, each as recommended (record §7.6): **Off hard-clips** at the ceiling (§11.5 Q1),
> **factory presets keep the switch On** (D14, §11.5 Q8), and a Limit preset loaded with Trails
> after a Clip one **drops at once** (§11.5 Q9). No owner question is open; L1b's DAC test, L2's
> A/B and bench session 2 settle what remains (§11.5). Nothing is built.

**Evidence labels.**

- **Measured:** run on the host (MSVC, `/fp:precise`), or bytes and instructions from a firmware
  build's map and disassembly.
- **Model:** an estimate from the disassembly and the Seed's measured instruction latencies.
  Treat it as ±25 %.
- **Calculated:** derived from measured numbers.
- **Reported:** taken from a research lane's sources and not re-checked here.

**What changed from draft 1.** The reviews' findings are in record §6. In short:

- When the mix would pass full scale, **the wet gives way first**, down to −12 dB, and only then
  the whole mix. Draft 1 scaled the whole mix, so raising the effect volume or the Mix knob
  ducked the player's dry on most factory presets (§3.3, D11).
- **The release depends on the program:** 40 dB/s after an isolated over, 10 dB/s once an over
  returns during a release. Draft 1's single 40 dB/s rate fluttered on sustained material (§4.6,
  D3).
- The DAC's inter-sample test moves before the revision is minted (§6.4, L1b). An input-staging
  target and a bypass requirement are added (§6.4, §6.5).
- The cost is re-derived from the live image's own code, the statistics leave the hot path, and
  the console reads them safely (§7.4, §8).
- Lifecycle coverage, the extreme-input bound, the stored-position claims and the order-aware
  re-mint check are corrected (§4.4, §5.4, §9).

**What changed in draft 3 (the owner's answers, 2026-10-10).** Record §7 holds the answers and
the choices this revision made for the switch. In short:

- **D1–D4 and D6–D13 are confirmed** as recommended, D7 in its amended form: the limiter is the
  next free sound revision after CLOCK's tempo core (8) and synced times (9), **expected 10**,
  and the governor follows (11). This document writes the limiter's revision as 10 and the one
  before it as 9 (§10).
- **D5 is changed: the owner chose a per-preset switch** (in the session, from four options;
  record §7.5). Draft 3 specifies it as row 87, `output.limiter`, a Leaf, default On (§7.1).
  - **Off clips** each channel at the same ceiling instead of scaling the gain. On the pedal that
    gives the codec, code for code, what the engine without a limiter gives it. The plugin now
    clips identically, where an Off that passed overs would split it from the pedal again
    (§4.7). What Off does was then the owner's open question, §11.5 Q1, with the clip
    recommended; the owner chose the clip later the same day (record §7.6).
  - **Turning Off while limiting drains** the gain to unity at 40 dB/s with no attack, then
    clips. Turning On starts from unity, or from the drain's gain if a drain is running
    (§4.7).
  - **No render changes for the switch** but `limit_off_hot`'s, which L2 sets Off: every other
    preset plays it at its default. Every committed package's `sound_hash` changes, because the
    compiler writes the new leaf: revision 10's pull request needs the package-change label
    (§10).
  - **Factory presets keep it On**, enforced by a new lint, L15 (§9.5). Draft 4 makes this the
    owner's D14, which the owner confirmed the same day (record §7.6).
- **After T1, the live image has 792 bytes of ITCM spare** (clock §11.11 item 22 on
  `claude/tempo-core`), so L2 must first move cold code out of ITCM (§8.3).

**What changed in draft 4 (two reviews of draft 3).** Record §8 holds the findings and their
disposition. In short:

- **D5's form, corrected after draft 4.** Draft 4 labelled the per-preset switch as proposed and
  asked the owner, in §11.5 Q1, to choose among four readings: its reviewers saw only the
  decisions page, which records "Change" with an empty note. The owner had chosen the switch that
  day in the session, from four options (§11.2, record §7.5). The correction restates it as the
  owner's decision; §11.5 Q1 now asks only what Off does, and row 87 is the switch's.
- **The factory policy becomes an owner decision, D14**, recommended On for now, and confirmed by
  the owner the same day (record §7.6). The switch's clauses leave the confirmed answers of D6
  and D8 and move into their consequences. The declaration that would let a factory mode clip is
  specified so it can be built (§9.5).
- **The row reads as any integer-valued leaf:** a fraction is accepted and plays by the
  threshold, as compiler §2.2 requires (§7.1). The user-facing states are **Limit** and **Clip**.
- **Reset primes the limiter before its rebuild,** so `Switch` sees G = 1 after a restart (§6.1).
- **Tests and goldens.** The same-frame test is split into a limiter test and an engine test
  (§9.4). `limit_switch` gets a wet hot enough to pass F, and two per-call counts pin the drain
  below F. A new preset, `limit_clip_trails`, pins the duck on leaving a clipping preset with
  Trails, which §4.5 and risk 12 now state (§9.3).
- **Corrections:** the floor row of §4.4 counts as the other rows do (173,843); row 86 must exist
  before row 87 (§7.1); pre-revision-10 sessions load marked inexact; the `Package-change:` line
  names `limit_off_hot` and the gate's attribution of package presets' renders.
- **Visibility:** the pedal's control surface must show a stored Clip from the moment it loads, the
  Rev7 LED cues it, and the plugin marks Clip presets in its lists (D6's consequence, §7.4).

**The owner's answers to draft 4's open questions (2026-10-10).** Record §7.6 holds the questions
as asked and the answers. The owner gave each the answer this design recommended, so no technical
content changes:

- **What Off does (§11.5 Q1): "Hard-clip".** Off clamps each channel at the limiter's ceiling,
  as §4.7 specifies.
- **Factory presets and the switch (D14, §11.5 Q8): "Keep factory On".** L15 is an error under
  `--factory`, and the `--clips` declaration waits until a factory mode asks for it (§9.5).
- **Leaving a Clip preset with Trails (§11.5 Q9): "Instant".** The limiter attacks at once, as
  §4.7 specifies. The entry ramp is not built, and L2's A/B plays `limit_clip_trails` with the
  instant attack only (§11.1).

No owner question is left open. L1b's DAC test, L2's A/B and bench session 2 settle the rest
(§11.5).

---

## Summary

- **What.** A zero-latency peak limiter, linked across the two channels. It is the last step of
  the engine's output pass (Pass 3c), in `dsp/`, so the pedal and the plugin play the same
  limited samples.
- **When it acts.** Only when a sample of the mix would pass full scale. The threshold and the
  ceiling are both exactly 1.0: a hard knee at 0 dBFS. While no sample passes 1.0, the
  limiter's output is the input's bits, so a render that never clips keeps its hash.
  - 37 of the 45 golden renders keep their hashes; the same 8 as draft 1 change (measured).
  - All 45 are identical under eight block, delivery and floating-point variants, and the
    forced-flush control passes (measured).
  - The unit suite passes, apart from one click test whose renders run 0.9–3.2 dB over full
    scale (§9.4).
  - Every factory preset's engaged renders at its stored positions on Plucks, Strums and
    SoftNotes keep their hashes. Its S0 Saturation render changes, and so do two other S0 renders
    that pass 0 dBFS today: Echolalia's engaged OnsetBursts render and Déjà Vu's wet (Mix 1)
    SoftNotes render (§9.5).
- **What gives way.** The wet first: while the wet alone can bring the sum under full scale with
  a gain of −12 dB or more, the dry is not touched. Past that, the wet stays at −12 dB and the
  whole mix is scaled.
  - With the effect volume raised to +12 dB on every factory preset at its stored positions, the
    dry is never scaled, on any of the three class inputs; the wet's lowest gain is −10.1 dB
    (measured on reconstructed renders, §3.3).
  - On a full-scale dry with a −6 dBFS wet, the dry dips 1.0 dB, where draft 1 dipped it 3.5 dB
    (measured, synthetic).
- **How it acts.**
  - **Attack:** instant.
  - **Hold:** 10 ms, restarted while the over keeps coming back within 0.25 dB.
  - **Release:** 40 dB/s after an isolated over; 10 dB/s once an over returns during a release.
    It ends at exactly 1.0f.
  - **Safety:** a gain floor of 2⁻²⁴ and a final clamp.
  - **No look-ahead.** The dry is never delayed, and `LatencySamples()` stays 0.
- **The plugin's hot dry.** Each channel's ceiling is the larger of 1.0 and the dry term's own
  level, which is always 1.0 on the pedal. The output never exceeds that ceiling, and the dry
  passes untouched while the wet alone gives way. Mix 0 plays the input on any input whenever the
  wet's gain is at −12 dB or above.
- **Arithmetic.**
  - Only +, −, ×, ÷, compares and bit operations.
  - One division per channel over full scale, and a second when its wet would need more than
    12 dB.
  - No transcendental per sample. Both release steps are computed once, at `Init`.
  - No flush site is needed.
- **Cost.**
  - Counted in the live image's disassembly, from the saturation to the stores, where today's
    code is 18 instructions: idle 33; limiting 94–128 on the common paths, with at most one
    division; the worst path 163, with four divisions. The placeholder is 260 cycles per frame,
    12.5k per 48-frame block (2.6 %), in the governor's static reserve S until bench session 2
    times each path (model).
  - The live image grows by 1,280 bytes of ITCM (measured on revision 7's image, from 3,208
    bytes spare to 1,928). The switch adds an estimated 100–200 bytes. After CLOCK's T1 only
    792 bytes are spare, so the limiter lands after a cold-code move (§8.3).
- **Control: a per-preset switch, the owner's D5** (2026-10-10: chosen in the session from four
  options, after a "Change" with no note on the decisions page).
  - Row 87, `output.limiter`, is a Leaf stored in every package. It is On by default and is
    written `"output": { "limiter": 1 }`. Users see its two states as **Limit** (1) and **Clip**
    (0).
  - **On** limits as above. **Off** clips each channel at the same ceiling: the owner's answer
    to §11.5 Q1 (2026-10-10), as the design recommended. So no sample leaves the engine over
    full scale on the pedal either way, and the plugin plays what the pedal plays.
  - Turning it Off mid-limiting drains the gain to unity at 40 dB/s, then clips. Turning it On
    starts from unity, so the next over attacks at once, also when a Limit preset loads with
    Trails after a Clip one, as the owner chose (§11.5 Q9).
  - Macros, expression and host automation cannot reach it. Factory presets keep it On (lint
    L15): the owner's D14, confirmed on 2026-10-10.
  - The plugin shows a limiter lamp captioned LIM or CLIP, a dry indication and the gain
    reduction, and marks Clip presets in its lists. On the pedal, the console reports the switch
    and the counts, and the Rev7's user LED shows limiting and cues a Clip preset at load.
- **Revision.** The next free sound revision after CLOCK's tempo core (8) and synced times (9):
  expected **10**, confirmed by the owner (D7). It lands before the governor (11) and before the
  owner's knob-rating rows are written, so no rating needs a re-listen. The DAC's inter-sample
  test runs first (L1b), so the ceiling is fixed once.
  - The new leaf changes every committed package's `sound_hash`, and no render but that of
    `limit_off_hot`, which L2 sets Off. Revision 10's pull request carries the package-change
    label and a `Package-change:` line (§7.1, §10).
- **Work.** About 11–15 engineer-days in seven lanes (§11.1).

---

## 1. Scope

### 1.1 The request, and what this pass delivers

The engine forms its output as a + b per channel, where a = dry·gd is the dry term, b =
wet·g·gw is the wet term, gd and gw are the Mix law's gains and g is the wet gain. Its only
conditioning is a saturation at ±`FLT_MAX` (`dsp/src/Engine.cpp:1231-1260`, `:193`). So any
over is clipped by whatever comes after the engine:

- **The pedal.** The codec's conversion clamps every over to just under full scale.
- **The plugin.** The host receives values above 1.0.

The pre-screen measured how far over the factory set goes (`firmware/factory/AUDITION.md:43`;
`docs/STATUS.md:160`):

- **Stored positions on the class inputs:** never above −1 dBFS.
- **Attack modes' sweeps and S11 corners on SoftNotes:** up to +2.5 dBFS (Echolalia +2.5,
  Updraft +2.4, Callback +1.7, Déjà Vu +1.7, Kaleido +1.3, Pinhole +1.2, Refrain +1.0,
  Retrograde +0.8, Engram +0.7).
- **S0 on the Saturation vector:** +5.5 to +10.7 dBFS on all 18 modes.
- **Lull's S10 and Runaway's S8:** just over 0 dBFS.
- **Two other stored-position renders** that the pre-screen's "S0 highest" line hides behind the
  Saturation render: Echolalia's S0 OnsetBursts render at +0.14 dBFS and Déjà Vu's S0 wet
  (Mix 1) SoftNotes render at +0.52 dBFS (measured, `bspc --metrics`).

This pass designs:

1. the algorithm and its constants (§4);
2. its arithmetic under the determinism profile (§5);
3. where it sits, why the plugin gets it too, the analog side and bypass (§6);
4. its control and display (§7);
5. its cost, how the governor charges it, and where its code lives (§8);
6. its effect on the goldens, the tests and the audition (§9);
7. its sound revision and order (§10);
8. the plan and the decisions (§11).

### 1.2 Goals

1. **A safety ceiling.** No sample leaves the engine over full scale on the pedal, for any preset,
   knob position, event stream or input the codec can deliver, with the preset's switch On
   (limited) or Off (clipped at the same ceiling, §4.7).
2. **Nothing changes where nothing clipped.** While no sample of the mix exceeds 1.0, the output
   is today's output, bit for bit: every golden render and factory render that never passes full
   scale keeps its hash.
3. **The dry gives way last.** The dry is the player's instrument. The wet takes the reduction
   first; the dry is scaled only when the wet alone would need more than 12 dB.
4. **One sound.** The plugin plays exactly what the pedal plays, within the parity contract
   (profile §2).
5. **Sample-identical** on MSVC, GCC, Clang and the M7, for any block split, delivery and
   caller floating-point environment.
6. **Bounded and charged.** Its worst cost per block is a constant the governor can reserve.
7. **Quiet when it acts.** It acts with a gain, not a waveshape. Overs of a few dB on sustained
   material become a level dip that holds still. The waveform bends only at the sample where a new
   maximum arrives, and while the wet alone gives way, only the wet bends (§4.6).

### 1.3 Non-goals

| Not a goal | Why, and where it goes |
|---|---|
| A creative compressor, a "glue" or a loudness maximiser | It never acts below full scale. Character belongs to modes; a drive or compressor would be a stage of its own, with its own design |
| A fix for hot inputs | The Saturation vector's +5.5 to +10.7 dBFS is an input-staging problem. §6.4 sets an input-staging target for the hardware design (D12). The limiter keeps those renders from clipping, with the dry dipping only once the wet is down 12 dB |
| Protection of the feedback loop | The tamer's saturator already bounds it (engine §2.3, `dsp/src/detail/PostChain.h:32`). The limiter is outside the loop |
| True-peak (inter-sample) limiting | It needs a 4× oversampled side chain, which is BS.1770-4 Annex 2 practice, or a ceiling below 1.0, which changes the full-scale null (§3.4). Decided once, before revision 10 is minted, by the Rev7 DAC test of lane L1b (D10) |
| The plugin's output trim and the pedal's analog output level | Both act after the engine, outside parity, at the user's choice (§6.3) |
| The bypass topology | It belongs to the bypass design (engine §1 defers it). §6.5 hands it one requirement (D13) |
| A limiter in the looper | The looper has its own design |
| A switch that removes the ceiling | The owner's per-preset switch (D5) chooses how an over is held, by limiting (Limit, 1) or by clipping (Clip, 0), never whether. An Off that passed overs would split the plugin from the pedal above full scale again, and on the pedal it would sound the same as clipping (§4.7). The owner chose the clip for Off on 2026-10-10, as recommended (§11.5 Q1) |

### 1.4 Terms

| Term | Meaning |
|---|---|
| a | The dry term of the mix per channel, `dry·mix.dry` |
| b | The wet term of the mix per channel, `wet·g·mix.wet` |
| s | The mix sample per channel: today's `oL`, `oR` = a + b after the ±`FLT_MAX` saturation (`Engine.cpp:1250-1258`) |
| p | max(\|s_L\|, \|s_R\|): the linked detector |
| C | The ceiling, 1.0f. Each channel's ceiling is c = max(C, \|a\|), which is C on the pedal |
| T | The threshold. In this design T = C |
| G | The limiter's gain state, in [2⁻²⁴, 1]: the wet's gain. "Idle" means G = 1 with no sample over C |
| F | The wet floor, 2⁻² (−12.04 dB). While G ≥ F the wet alone is scaled by G; below it the wet sits at F and the whole mix is scaled by G/F |
| r | The gain the frame requires: 1 while no channel passes its ceiling, else the smallest channel requirement (§4.3), floored |
| Demand | A frame on which some channel passes its ceiling (equivalently r < 1) |
| H | The hold, in frames: 480 at 48 kHz |
| k, k_s | The per-frame release factors: 40 dB/s (about 1.0000960) and 10 dB/s (about 1.0000240) at 48 kHz |
| Over | A sample of s with \|s\| > 1.0 |
| The switch | Row 87, `output.limiter`, a per-preset Leaf, the owner's answer to D5: On (1, the default) limits; Off (0) clips at the ceiling, the owner's answer to §11.5 Q1 (§4.7, §7.1). Users see On as **Limit** and Off as **Clip**; this document says On and Off |
| Drain | After a switch to Off with G < 1: the gain rises at k per frame, with no attack and no hold, to exactly 1 (§4.7) |

### 1.5 Principles

1. **Engage only where clipping would occur.** Full scale is the one threshold at which "nothing
   changes where nothing clipped" holds by construction (§3.4).
2. **The dry gives way last.** The wet takes the first 12 dB of any reduction (§3.3).
3. **Gain, not shape.** The output is the dry plus the wet times a slowly varying gain. The only
   waveform bending is at the sample where a new maximum arrives (§4.6).
4. **Zero latency.** Instant attack and no look-ahead: the dry path stays undelayed
   (`dsp/include/brainscape/Engine.h:289-290`; grain-delay theory §3.11).
5. **Exact unity, by comparison, not convergence.** The idle path returns before any
   arithmetic, and the release is capped at exactly 1.0f.
6. **In the engine, after everything that sounds.** It is the last step of Pass 3c, outside the
   feedback loop, so it never changes the engine's state.

---

## 2. The problem in numbers

### 2.1 How the output is formed

Pass 3c (`Engine.cpp:1231-1261`) runs per sample, after the post chain.

1. It reads the smoothed Mix through the Mix law, dry = min(1, 2(1 − m)) and wet = min(1, 2m)
   (`dsp/src/detail/MixLaw.h:35-39`).
2. It reads the smoothed wet gain (`Engine.cpp:162-168`). That gain is either exactly 0 under
   the cutoff kill, or `Exp2F` of the mode's trim plus the player's effect volume.
3. It forms `dry·mix.dry + wet·g·mix.wet`. This two-multiply form keeps the endpoints exact.
4. It saturates at ±`FLT_MAX` with one-sided compares, so NaN still reaches the Debug assertion
   (`:1252-1258`).
5. It stores the result.

Reachable levels:

- The wet trim spans −24 to +24 dB (`Params.h:189`) and the effect volume −24 to +12 dB
  (`Params.h:267`), so the wet can be raised by up to 36 dB. The effect volume is the player's
  wet level on the panel (Shift+Mix, compiler §3.8). The research's model for it, the
  Microcosm's, "does not affect dry" (`docs/research/microcosm.md:108`).
- At Mix 0.5 both gains are 1, so a full-scale input reaches +6 dBFS before any wet gain.
- The feedback tap is written in Pass 3a (`Engine.cpp:1209-1225`), before Pass 3c, so nothing
  in Pass 3c reaches the engine's state.

### 2.2 How it reaches the codec and the host

**The pedal.**
- `AudioCallback` hands libDaisy's buffers to `Process` and adds nothing
  (`firmware/live/main.cpp:100-106`).
- LD converts with `f2s24(out × output_adjust_)`, where `output_adjust_` is 1 because
  `postgain` is 1 (LD `src/hid/audio.cpp:447-453`, `:121`).
- `f2s24` clamps to ±`FBIPMAX` = ±0.999985, multiplies by 2²³ and truncates, so its largest code
  is 8,388,482 (LD `src/daisy_core.h:33-34`, `:146-151`; profile §2.2).
- Today, then, the pedal hard-clips every over, about 0.00013 dB under full scale.

**The plugin.**
- `RenderChunk` runs the engine, then applies the wrapper's output level, then meters. It never
  clamps (`plugin/src/PluginProcessor.cpp:1061-1108`).
- A DAW therefore receives the overs as floats.

**So the two outputs already differ above full scale.** Once the engine stops at 1.0, the codec
clamp only ever touches samples in (0.999985, 1.0]. That is a 125-code band, 1.3·10⁻⁴ dB. It
lies outside the parity contract, whose boundary is the engine's float output (profile §2.2).

### 2.3 How far over it goes: the revision-7 goldens

Eight golden renders cross full scale at revision 7. The limiting figures below were measured
with draft 2's limiter built into the engine, reading its statistics per render (§12). An
offline model applied to the dumped output no longer works for this design: the wet-first rule
needs the dry and wet terms separately, which the output alone does not carry. The offline model
of draft 1 also never reset its gain at a `Reset` frame, which was harmless only because no
restart in the corpus lands while limiting (record §6, E1).

| Preset | Peak today | Frames over today | Frames limited | Of which the whole mix | Engagements | Lowest wet gain | Frames at the ceiling |
|---|---|---|---|---|---|---|---|
| `saturation_6s/hot_out` | +27.73 dBFS | 259,739 | 275,993 | 275,629 | 1 | −27.73 dB | 542 |
| `plucks_automation_12s/automation_offgrid` | +11.41 | 5,752 | 177,643 | 0 | 6 | −11.63 | 65 |
| `plucks_state_14s/spillover_chain` | +1.77 | 3 | 3,072 | 0 | 1 | −2.16 | 2 |
| `plucks_state_14s/exact_load_mid` | +1.32 | 8 | 9,776 | 0 | 4 | −2.91 | 5 |
| `plucks_wave1_12s/midi_gate` | +1.22 | 8 | 3,629 | 0 | 2 | −1.68 | 3 |
| `plucks_12s/strum_marks` | +0.40 | 3 | 1,636 | 0 | 1 | −0.69 | 2 |
| `soft_notes_10s/post_max` | +0.37 | 14 | 1,072 | 0 | 1 | −0.48 | 8 |
| `plucks_modes_14s/lone_changes` | +0.08 | 1 | 673 | 0 | 1 | −0.16 | 1 |

- **Only `hot_out` scales the whole mix.** It plays at Mix 1, so its dry term is 0, and its wet
  is scaled exactly as draft 1 scaled the whole sum: its hash is the same under both rules
  (measured).
- **The other seven take the reduction on the wet alone.** Their wet dips further than draft 1
  dipped the whole mix (`exact_load_mid` −2.91 dB against −1.32 dB), and they limit for longer
  (9,776 frames against 5,770), because the wet carries the whole reduction and the release
  slows when overs return. Draft 1's figures are in record §2.4.

The other 37 peak at or below −0.60 dBFS. In order: `selfosc` −0.60, `onset_burst` −0.84,
`dense_1ms` −1.24, `freeze_long` −1.46, `mono_stutter` −1.66, `restart_kept_params` −1.70, and
the rest at or below −2.14. `default_silence` is silent.

### 2.4 Contracts the limiter must keep

| Contract | Where | What a limiter must do |
|---|---|---|
| Mix 0 plays the dry input bit for bit, up to the sign of a zero, on hostile input up to ±`FLT_MAX` | `MixLaw.h:15-27`; `dsp/tests/test_modes.cpp:506-558` | Leave a mix with no wet term untouched at any level |
| Every Mix plays dry·LawDry + wet·LawWet bit for bit | `test_modes.cpp:469-504` | Not engage on that test's renders (§9.4) |
| Contract #2: the unity-rate Tu path nulls to the bit, including a 1.0 impulse at Mix 1 | engine §10; `dsp/tests/test_engine.cpp:320-347` | Leave a sample of exactly 1.0 untouched |
| The trim and effect volume never scale the dry | `Engine.cpp:1236-1238`; `test_modes.cpp:356-368` | Scale the wet before the dry. Kept whenever the wet alone can absorb the over by up to 12 dB: measured on every factory preset at its stored positions with the effect volume up to +12 dB (§3.3). Past that, the dry dips (§4.5) |
| The dry is never delayed; the plugin reports 0 latency | `Engine.h:289-290`; engine §2 item 5 | No look-ahead |
| Block-split invariance; no per-block decision | engine §10 #1; profile §5.7 | Per-sample state only |
| Finite in, finite out; NaN reaches the Debug check | profile §3.7; `Engine.cpp:1252-1272` | One-sided compares and a bounded gain |
| Recursive state is flushed per sample | profile §4.3 | No state that decays toward 0, or a flush site |
| The plugin's live dry passes above 0 dBFS unclipped | profile §3.7; companion §4.8 | The output never exceeds max(1, \|a\|) per channel, and the dry is untouched while the wet alone gives way. With the wet past −12 dB, the linked gain can pull a hot dry below its own level (§4.4 item 7) |
| `Reset` and `Restart` give the exact-restart state; Spillover keeps smoothers | `Engine.h:97-112`; `Engine.cpp:667-705`, `:754-770` | Prime on `Reset`; carry over on Spillover; both pinned by goldens (§5.4) |

---

## 3. Options considered

### 3.1 What comparable products do (reported)

| Product | Output protection | Control |
|---|---|---|
| Strymon BigSky MX | An "adaptive limiter" for reverb build-up from hot input, added in firmware v2.02 (July 2024). No details are published | none found |
| Strymon TimeLine/BigSky, Chase Bliss MOOD | Analog dry path: only the wet reaches the DAC, so an over clips the wet and never the dry | Kill Dry |
| Mutable Clouds | A static Padé soft clip on dry plus wet. Nominal level already sits 0.6 dB into the curve | none |
| Mutable Rings | A linked peak follower (attack 0.05 per sample, release 2·10⁻⁵ per sample, about 8.3 dB/s), gain 1/peak above 1, then ×0.8 into the same Padé soft clip | none |
| DaisySP `Limiter` | Rings' follower, then ×0.7 into the soft clip | library |
| Fractal Axe-Fx | A hard limit at 0 dBFS before the D/A, which users call harsh (forum) | none |
| Eventide H90, EHX 45000, Red Panda Particle | No output limiter documented. The H90's clip LEDs sit before its gain stages and miss real clipping | per-algorithm gain |
| Airwindows ClipOnly2 | A safety clipper: unclipped samples pass untouched | none |
| Chase Bliss Lossy, Meris LVX, Beads | Limiters as creative effects | user |
| Ableton Limiter | 1.5–6 ms look-ahead, so it adds latency | full |

Four patterns stand out:

- Limiters meant as protection are always on and have no control. Limiters a user can adjust
  are creative effects.
- The products with a digital dry either soft-clip the whole sum or hard-clip it. None documents
  look-ahead.
- In the products with an analog dry, only the wet can clip, so the dry never gives way. That is
  the behaviour §3.3 adopts in the digital domain.
- Limiting inside the feedback loop is universal. Brainscape already has it, in the tamer's
  saturator.

### 3.2 The candidates for the gain computer

Each candidate was prototyped by the DSP lane and run on sine bursts with a 0.5 ms onset, 2.5 dB
and 10.7 dB over full scale (host-run). The residual is measured against the best linear gain,
in dB relative to the output. "Onset" covers 0–20 ms and "steady" 0.3–1.3 s. These were measured
on the whole sum; §3.3 then chooses what the gain applies to.

| | Algorithm | Unity below threshold | Latency | Onset residual, +2.5 dB (110 Hz / 1 kHz / 5 kHz) | Steady state | Cost | Verdict |
|---|---|---|---|---|---|---|---|
| A | Static soft clipper (Clouds-like) with exact identity below T | yes | 0 | −19.1 / −19.0 / −19.0 dB | −19.0 dB residual, aliasing −29.6 dB at 5 kHz; at +10.7 dB −9.6 dB, a fuzz | 10–40 cycles per channel | Rejected: it is a distortion |
| **B** | **Peak limiter, linear gain, hard knee at full scale** | **yes, exact** | **0** | **−24.2 / −29.1 / −31.9 dB** | **−153 dB from 82 Hz up; −67 dB at 41 Hz** | **§8.1** | **Chosen** |
| C | B with a quadratic soft knee from −1 dBFS to 1.0 | yes, below −1 dBFS | 0 | −24.4 / −29.1 / −32.0 dB | as B | B plus four operations; a second division on frames where the dry passes −1 dBFS | The owner's alternative (D2) |
| D | B with a dB-domain gain computer (`LogF`, `Exp2F`) | yes | 0 | within 0.1 dB of B and C | as B | +300–450 cycles per limited frame | Rejected: cost |
| E | 1 ms look-ahead brickwall | yes | 48 frames | −29.5 / −39.5 / −46.5 dB | −51 dB at 110 Hz | 0.6–1 KiB state, 40–80 cycles | Rejected: delays the dry, moves every golden, needs a reported latency |
| F | Finite attack (0.5 ms) then a soft clipper (the Rings and DaisySP topology, adapted to unity) | yes, once adapted | 0 | −21.0 / −22.9 / −23.2 dB | −57 dB at 110 Hz | two stages, about 1 KB | Rejected: a slower attack moves the onset distortion into the clipper |
| G | No limiter (the codec clips), or a clamp in the firmware only | — | — | — | hard clip on every over | 0 | Rejected: the owner asked for one, and pedal and plugin keep differing |
| H | A limiter in the plugin only, or in the firmware only | — | — | — | — | — | Rejected: breaks "the plugin plays what the pedal plays" (§6.3) |

The originals of F scale every sample by 0.8 or 0.7 and let their envelopes decay toward 0. With
those, nothing is bit-exact below the threshold, and the envelopes would need a flush site.

### 3.3 What gives way: the whole mix, the wet, or the wet first

One gain state G, with B's time law, can be applied in four ways. A single constant F spans them:
while G ≥ F the wet alone is scaled by G, and below F the wet sits at F and the whole mix is
scaled by G/F.

| Variant | F | What it does | Verdict |
|---|---|---|---|
| The whole mix (draft 1) | 1 | The dry dips with the wet whenever the sum is over | The alternative of D11: cheaper, but the player's dry pumps when the wet is raised |
| The wet only, from the instantaneous dry, without a hold | — | The wet's gain follows the dry sample by sample: with a dry at full scale and a wet at −6 dBFS, the wet drops 38.6 dB, and it intermodulates with the dry (host-run) | Rejected for its sound |
| The wet only, with B's hold and release ("dry priority") | 2⁻²⁴ | The dry is never scaled. Under a full-scale dry the wet has no room: it falls 118–132 dB and is still not back 2 s after the dry stops (measured, synthetic) | Rejected: the effect vanishes on hot input and takes seconds to return |
| **The wet first, then the whole mix** | **2⁻² (−12 dB)** | **The dry is untouched while the wet alone can absorb the over by up to 12 dB. Past that, the dry dips by G/F** | **Chosen (D11)** |

**The measurements (reconstructed renders).** For each factory preset at its stored positions,
the dry and wet terms were rebuilt from `bspc`'s S0 render and its Mix-1 wet render (the review's
method; the reconstruction's residual is at most 1.8·10⁻⁸). The wet term was raised by the
effect volume, and each variant ran over all 18 presets on the three class inputs. "Wobble"
counts the 50 ms windows, at a 10 ms hop, in which the gain rose and fell back by 0.5 dB or more,
summed over the 18 presets.

| Case | Whole mix, 40 dB/s (draft 1) | Whole mix, dual release | Wet first, F = −12 dB, dual release |
|---|---|---|---|
| Mix 0.5, SoftNotes | dry scaled on 10 of 18, down to −1.65 dB | as draft 1 | dry never scaled; wet down to −2.50 dB |
| Effect volume +6 dB, SoftNotes | dry scaled on 16 of 18, down to −2.97 dB; wobble 193 | dry as draft 1; wobble 57 | dry never scaled; wet down to −4.09 dB; wet wobble 84 |
| Effect volume +6 dB, Plucks / Strums | dry scaled on 2 / 1, down to −1.79 / −0.66 dB | — | dry never scaled; wet down to −3.31 / −1.73 dB |
| Effect volume +12 dB, SoftNotes | dry scaled on 17 of 18, down to −8.51 dB; wobble 3,399 | wobble 49 | dry never scaled; wet down to −10.09 dB; wet wobble 41 |
| Effect volume +12 dB, Plucks | dry scaled on 17, down to −6.47 dB; wobble 117 | wobble 58 | dry never scaled; wet down to −9.31 dB; wet wobble 57 |
| Effect volume +12 dB, Strums | dry scaled on 16, down to −4.95 dB; wobble 108 | wobble 44 | dry never scaled; wet down to −7.73 dB; wet wobble 39 |

Plucks and Strums do not engage at Mix 0.5. No variant leaves a sample over full scale.

**On hot input (measured, synthetic).** A full-scale dry, a 110 Hz sine at twice full scale
clipped to the codec's 1 − 2⁻²³, lasts 2 s under a 220 Hz wet that lasts 4 s:

| Wet level | Variant | Dry's lowest gain | Wet's lowest gain | Back to unity after the dry stops |
|---|---|---|---|---|
| −6 dBFS | whole mix | −3.53 dB | −3.53 dB | 0.097 s |
| −6 dBFS | wet first, F = −6 dB | −1.94 dB | −7.96 dB | 0.208 s |
| −6 dBFS | **wet first, F = −12 dB** | **−1.03 dB** | **−13.07 dB** | **0.336 s** |
| −6 dBFS | wet first, F = −18 dB | −0.53 dB | −18.59 dB | 0.474 s |
| −6 dBFS | wet only | 0 | −132.5 dB | not within 2 s |
| −20 dBFS | whole mix | −0.83 dB | −0.83 dB | 0.030 s |
| −20 dBFS | **wet first, F = −12 dB** | **−0.21 dB** | **−12.26 dB** | **0.316 s** |
| −20 dBFS | wet only | 0 | −118.5 dB | not within 2 s |

**Why F = −12 dB.** It is the largest power of two below the lowest wet gain the factory set
needs at its stored positions with the effect volume at its +12 dB maximum (−10.09 dB, 0.31), so
the dry is never scaled there, with 2 dB to spare. A lower F keeps the dry steadier on hot input but
lets the wet fall further and recover more slowly. A power of two makes G/F and F·b exact.

**The owner's case.** The S11 corner's sum peaks at +2.5 dBFS (1.33) on SoftNotes, whose dry
peaks at −7.18 dBFS (0.44). Any same-signed sample then has |a| ≤ 0.44 and |a| + |b| ≤ 1.33, so
its wet needs at most (1 − 0.44)/(1.33 − 0.44): about −4 dB. The dry is not touched
(calculated).

**F = 1 is draft 1.** With F = 1 and a single 40 dB/s release, draft 2's code reproduces draft
1's prototype bit for bit on all 45 golden renders (measured). Draft 1 is one member of this
family, and D11 can return to it by changing one constant.

### 3.4 The choice: B with a hard knee at full scale, wet first

**Why a hard knee at full scale.** It is the only threshold at which three properties hold by
construction, not by measurement:

1. **A 1.0 sample is never touched.** Contract #2's full-scale impulse and its test are
   unchanged.
2. **On the pedal, the dry alone can never engage it.** The codec's input is under 1.0
   (`InputCondition.h:21-27`; LD `s242f`), and the dry gain is at most 1.
3. **Only renders that clip today change.** That is 8 of 45 goldens, and on the factory set
   only the renders that already pass 0 dBFS.

**What a soft knee from −1 dBFS (C) would cost.** It changes renders that never clipped:

- 10 goldens instead of 8: `selfosc` at −0.60 dBFS and `onset_burst` at −0.84 join.
- Every factory sweep and S11 render between −1 and 0 dBFS, including the tightest at −0.01
  dBFS (Halation at Pinhole's positions).
- Contract #2 must be restated "below −1 dBFS", because the knee turns the 1.0 impulse into
  0.973.
- The Mix-law test holds only while its sums stay under −1 dBFS.

**What the soft knee buys.** Less bending of the attack edge on large, sudden, low overs
(measured on the whole sum):

| Over | Frequency | HF-weighted onset residual, hard knee | Soft knee |
|---|---|---|---|
| +2.5 dB | 110 Hz | −4.9 dB | −21.8 dB |
| +10.7 dB | 110 Hz | +14.4 dB | +5.7 dB |
| either | 1 kHz and above | within 2 dB of the soft knee | |

The residual is weighted by the third difference (host-run). On program material the difference
shrinks: on `strum_marks`, the HF splatter index is −1.45 dB with the hard knee and −1.46 dB with
the soft knee (host-run).

The engine's click test, `post-delay time automation is click-free`, shows the same. Its
feedback-0.9 render plays at Mix 1 and runs 0.9 dB over full scale. The largest local outlier of
its second difference is 5.84 times its neighbourhood with the hard knee, and 4.19 times with the
soft knee, against the test's 2.0 (measured; draft 2 measures the same 5.84, since at Mix 1 the
wet is the whole mix). Both knees fail the same way: when a new maximum arrives from a held gain,
the knee's slope is below the held gain, so the waveform bends (§4.6). Only look-ahead avoids
that bend.

**With the wet first, the bend lands on the wet.** While G ≥ F the dry passes untouched, so the
bend at a new maximum is in the wet alone. That weakens the case for the soft knee, whose
measurements were all on the whole sum.

**Recommendation, confirmed by the owner on 2026-10-10.** B with the hard knee (D2), the wet
first (D11). Lane L2 renders the owner's confirming A/B before the revision is minted: what
gives way, and the release (§11.1). The knee is two
constants and one branch; it is auditioned only if the owner asks, and would be re-derived for
the wet-first form.

---

## 4. The algorithm

### 4.1 Constants

| Name | Value | binary32 | Source |
|---|---|---|---|
| `kLimCeiling` (C, also T) | 1.0 | `0x3F800000` | literal |
| `kLimFloor` | 2⁻²⁴ (−144.5 dB) | `0x33800000` | literal `0x1p-24f` |
| `kLimWetFloor` (F) | 2⁻² (−12.04 dB) | `0x3E800000` | literal `0x1p-2f` |
| `kLimWetFloorInv` (1/F) | 4 | `0x40800000` | literal `0x1p+2f`; exact |
| `kLimHoldTol` | 10^(0.25/20) ≈ 1.0292005 | `0x3F83BCD8` (1.02920055) | hex-float literal `0x1.0779b0p+0f` |
| `kLimReleaseDbPerS` | 40 dB/s | — | literal, binary64 |
| `kLimReleaseSlowDbPerS` | 10 dB/s | — | literal, binary64 |
| `kLimHoldSeconds` | 0.010 s | — | literal, binary64 |
| `release` (k) | 2^(40·log₂10/20 / R) | at 48 kHz `0x3F800325` = 1 + 805·2⁻²³ ≈ 1.0000960, which is 40.0074 dB/s | `Init`: `(float)Exp2D(40.0 * 0x1.542a5a12e1c5ap-3 / sampleRate)` |
| `releaseSlow` (k_s) | 2^(10·log₂10/20 / R) | at 48 kHz `0x3F8000C9` = 1 + 201·2⁻²³ ≈ 1.0000240, which is 9.9898 dB/s | `Init`: `(float)Exp2D(10.0 * 0x1.542a5a12e1c5ap-3 / sampleRate)` |
| `holdFrames` (H) | round(0.010·R) | 480 at 48 kHz | `Init`: `RoundHalfAwayI32(0.010 * sampleRate)` |

`0x1.542a5a12e1c5ap-3` is log₂10/20 in binary64 exactly, the same value as `WetGainTarget`'s
`0.16609640474436813f` (`Engine.cpp:167`), carried at double precision. At 44.1 kHz the three
computed values are `0x3F80036C`, `0x3F8000DB` and 441; at 96 kHz `0x3F800192`, `0x3F800065` and
960 (measured).

### 4.2 State

| Field | Type | Range | Reset |
|---|---|---|---|
| `gain` (G) | float | [2⁻²⁴, 1] | 1 |
| `hold` | uint32 | [0, H] | 0 |
| `releasing` | bool | a release step has run since the last attack | false |
| `slow` | bool | an over came back during a release: release at k_s | false |
| `on` | bool | the switch as the engine plays it: true limits, false clips (§4.7) | `Init`: true; then the rebuild of row 87 sets it; `Reset` keeps it |
| `release`, `releaseSlow`, `holdFrames` | float, float, uint32 | from `Init` | kept |
| `limitedFrames`, `dryFrames`, `engagements` | uint64 | counts, not sound | `Init` only |
| `restartsWhileLimiting`, `loadsWhileLimiting` | uint64 | counts, not sound (§5.4) | `Init` only |
| `offFrames`, `clippedFrames`, `offWhileLimiting`, `onWhileDraining` | uint64 | counts, not sound (§4.7) | `Init` only |
| `drainFrames`, `drainDryFrames` | uint64 | counts, not sound: of `limitedFrames` and `dryFrames`, those rendered with the switch Off, so a drain's (§6.1) | `Init` only |
| `lowestGain`, `peak` | float | statistics, not sound | `Init`; restarted by their `Consume` calls |

The prototype's fields, all but the two lifecycle counts, add 56 bytes to `Engine::Impl`
(measured: the next member moved from offset 104 to 160); the two counts add 16 more. So 7,944
bytes become 8,016 of `kEngineImplBytes`' 8,192 on the M7 (compiler §7.1). The firmware's 9 KiB
DTCM slot is unchanged, and the prototype's live image linked with the same DTCM use (measured).
The switch adds the flag and six counts, about 52 bytes (*estimated*), and CLOCK's tempo core
adds its own state (clock §2.6), so L2 re-measures `Engine::Impl` against 8,192 on top of
revision 9 and raises `kEngineImplBytes` if it must (§8.4).

### 4.3 Per frame (normative)

`detail/OutputLimiter.h`, header-only, called once per frame from Pass 3c (§6.1). The order of
operations is as written. Each line is one IEEE operation, a comparison, a bit operation or a
selection.

```cpp
// a: the dry terms (dry * mix.dry); b: the wet terms (wet * g * mix.wet);
// s: the mix a + b after the ±FLT_MAX saturation.
// Returns a bit set: 0 idle (the mix's own bits); bit 0 a gain below 1 was applied; bit 1 the
// whole mix was scaled (G < F); bit 2 the switch is Off and the clamp clipped a channel (§4.7).
inline uint32_t OutputLimiter::Frame(float aL, float aR, float bL, float bR,
                                     float& sL, float& sR) noexcept {
  const float pL = detmath::Abs(sL);                   // sign bit cleared
  const float pR = detmath::Abs(sR);
  const float p  = pL > pR ? pL : pR;                  // linked detector
  if (gain == 1.0f && !(p > kLimCeiling)) return 0u;   // idle, On or Off: the mix's own bits
  const float cL = Ceiling(aL);                        // max(1, |a|): 1 on the pedal
  const float cR = Ceiling(aR);
  if (!on) return Off(aL, aR, bL, bR, cL, cR, pL, pR, sL, sR);  // the switch is Off: §4.7
  float r = 1.0f;                                      // the gain this frame requires
  if (p > kLimCeiling) {
    if (p > peak) peak = p;                            // statistics, not sound
    if (pL > cL) { const float n = Need(aL, bL, cL); if (n < r) r = n; }
    if (pR > cR) { const float n = Need(aR, bR, cR); if (n < r) r = n; }
    if (r < kLimFloor) r = kLimFloor;
  }
  if (r < gain) {                                      // attack: instant
    if (gain == 1.0f) ++engagements;                   // statistics, not sound
    if (r < lowestGain) lowestGain = r;                // G only ever falls here
    if (releasing) slow = true;                        // an over came back during a release
    releasing = false;
    gain = r;
    hold = holdFrames;
  } else if (r < 1.0f && r <= gain * kLimHoldTol) {    // demand within 0.25 dB: keep holding
    hold = holdFrames;
  } else if (hold != 0u) {
    --hold;
  } else {                                             // release, never past r <= 1
    releasing = true;
    const float up = gain * (slow ? releaseSlow : release);
    gain = up < r ? up : r;
    if (gain == 1.0f) {                                // released this frame: no sample was over
      releasing = false;
      slow = false;
      return 0u;
    }
  }
  if (gain >= kLimWetFloor) {                          // the wet gives way; the dry is untouched
    sL = Clamp(aL + gain * bL, cL);
    sR = Clamp(aR + gain * bR, cR);
    return 1u;
  }
  const float t = gain * kLimWetFloorInv;              // exact: F is a power of two
  sL = Clamp(t * (aL + kLimWetFloor * bL), cL);        // the wet at F, the whole mix scaled
  sR = Clamp(t * (aR + kLimWetFloor * bR), cR);
  return 3u;
}

// The largest gain x that keeps this channel's output within c.
inline float OutputLimiter::Need(float a, float b, float c) noexcept {
  const float ma   = detmath::Abs(a);
  const float head = SameSign(a, b) ? c - ma : c + ma; // the room the wet may fill
  const float x    = head / detmath::Abs(b);
  if (x >= kLimWetFloor) return x;                     // the wet alone gives way
  return (kLimWetFloor * c) / detmath::Abs(a + kLimWetFloor * b);
}
```

- `Ceiling(a)` returns `Abs(a) > 1 ? Abs(a) : 1`.
- `SameSign(a, b)` compares the two sign bits (a bit operation, so a zero of either sign gives
  the same `head`, c).
- `Clamp(x, c)` is `if (x > c) x = c; if (x < -c) x = -c;`.
- `Need` is called only for a channel over its ceiling, so |b| > 0 there: with b = 0 the mix is
  a, which is within c.

**Why the two forms join.** At G = F exactly, t = 1 and both forms compute fl(a + fl(F·b)), so
the output is continuous across F to the bit.

Draft 2 returned 2 for the whole-mix form. Draft 3 returns a bit set, 3 for that form, so that
the Off path's clip can be counted beside it (§6.1). The On path is otherwise draft 2's,
operation for operation: the one addition is the `on` test, which sits after the idle return.

### 4.4 Properties

1. **Identity.** With G = 1 and every sample at or below 1.0 in magnitude, the function returns
   before any arithmetic. The output is the input's bits, −0 and subnormals included, and the
   state is unchanged.
   - Checked exhaustively at G = 1 on all 1,065,353,217 magnitudes from 0 to 1.0, with both
     signs: 0 changed (measured, draft 1; the idle test is unchanged in draft 2).
   - G = 1 implies `hold` = 0, `releasing` = false and `slow` = false: G reaches 1 only in the
     release branch, which runs only at `hold` = 0 and clears both flags. So the early return is
     an optimisation and not a rule of its own.
2. **Ceiling.** For every finite input, |out_c| ≤ c_c = max(1, |a_c|). **The final clamp is the
   bound**, and it runs on every limited frame.
   - After the update, G ≤ r: the attack sets G = r, the hold branches run only when r ≥ G, and
     the release takes min(G·k, r). Mathematically, G ≤ r keeps each channel within its ceiling.
   - In floating point the unclamped result can overshoot. On 4,750,206 random pedal attack frames
     whose requirement is above the floor (a on the codec's grid, wet terms up to 2²⁴), 11,178
     (0.24 %) exceeded 1.0 before the clamp, each by 1 ulp (measured).
   - Wet terms beyond about 2²⁴ (+144 dBFS, reachable only by hostile plugin input) need a gain
     below 2⁻²⁴. The floor binds, every such frame exceeds the ceiling before the clamp
     (3,199,999 of 3,199,999 sampled), and the clamp acts as a hard clip at that sample.
3. **Return to unity.** After the last frame with demand, G is exactly 1.0f within H frames plus
   the release. From that frame on, the output is bit-identical to the unlimited engine's,
   because the limiter never fed back. Times to exact unity at 48 kHz, from a single over (the
   fast rate, since no over returns during the release). Each count runs from the over's frame,
   the attack, to the frame at which G reaches 1.0f, both included: 481 frames of attack and
   hold, then the release.

   | Gain reduction | Frames to exact unity | Time | Of which hold | Of which release |
   |---|---|---|---|---|
   | 0.1 dB | 601 | 12.5 ms | 10 ms | 2.5 ms |
   | 1.0 dB | 1,681 | 35.0 ms | 10 ms | 25.0 ms |
   | 2.5 dB | 3,481 | 72.5 ms | 10 ms | 62.5 ms |
   | 6.0 dB | 7,680 | 160.0 ms | 10 ms | 150.0 ms |
   | 11.4 dB | 14,159 | 295.0 ms | 10 ms | 285.0 ms |
   | 27.7 dB | 33,715 | 702.4 ms | 10 ms | 692.5 ms |
   | From the floor, 2⁻²⁴ (a ±`FLT_MAX` burst) | 173,843 | 3.622 s | 10 ms | 3.612 s |

   The first six were measured. The floor row counts from the burst's last over frame, as the
   others count from their over. The prototype measured 173,842 counting from the first frame
   after the burst; §4.3's code, simulated, gives 173,843 by this table's convention, which is
   481 plus §4.7's 173,362 (record §8.3).

   Once an over has returned during a release, the rest of that release runs at 10 dB/s: 1 dB
   takes 100 ms, and the floor about 14.5 s (calculated). The flag clears when G reaches 1.
4. **Liveness.** For every G in [2⁻²⁴, 1), G·k > G and G·k_s > G after rounding: k − 1 =
   805·2⁻²³ and k_s − 1 = 201·2⁻²³, at least 201 ulps of G. So the release rises strictly every
   frame and never stalls short of 1.
5. **The hold is restarted only under demand.** A variant that restarted the hold on any frame
   whose required gain sat within 0.25 dB of G, demand or not, never released. After the over
   ended it stayed at −0.25 dB, or −0.20 dB after a 0.2 dB over (measured). The `r < 1` term is
   normative.
6. **Linked channels.** Both channels share one gain, so the stereo image and the level
   difference between channels never move. On Plucks' alternating pan, an over in one channel
   lowers both wets.
7. **The dry contracts.**
   - **Mix 0, or the cutoff kill**, gives b = 0, so s = a exactly (up to the sign of a zero). On
     the pedal |a| < 1, so the limiter is idle at G = 1. With G ≥ F the output is fl(a + G·0) =
     a: Mix 0 plays the input even while a release is running (measured). Only with G < F, a
     Mix that reaches 0 while the whole mix is still scaled, is the dry scaled by G/F until the
     release brings G back to F.
   - **The plugin's hot dry.** No channel's output exceeds max(1, |a|). While G ≥ F the dry is
     untouched. Below F the linked gain scales both channels' dry by G/F, so a hot dry can sit
     below its own level: with aL = 3.0 (+9.5 dBFS) and a wet on the right channel that needs
     G = 0.248, the left output is 3.0, its own level, and the right channel's dry is scaled by
     0.99 (measured).
   - The hostile Mix-0 test loads at Mix 0, so its renders never limit.
8. **Split and environment invariance.** Every decision is per sample and keyed on nothing but
   the stream, and the code runs inside `Process`' guard. With draft 2's limiter built in, all 45
   golden hashes are identical in eight cases (measured):
   - blocks of 48, of 1, of 512 and the pattern {48, 1, 127, 32};
   - random block sizes (seed 7);
   - a caller environment with FTZ|DAZ and round toward zero;
   - a fresh engine per render;
   - split event delivery.

   Every invariance check passes in each, and the forced-flush control
   (`brainscape_golden_flush --force-flush-control`) passes on all 45 (measured).
9. **Finite.** Finite in, finite out. G stays within [2⁻²⁴, 1], and every output passes the
   clamp, which maps even an infinite intermediate to ±c. NaN, which the wrappers' sanitizing
   excludes, passes through unchanged and reaches the Debug assertion, as today.
10. **Either setting of the switch.** Items 1, 2, 8 and 9 hold with the switch Off as well. The
    idle return is the same code, and with the switch Off the clamp is the ceiling's whole
    mechanism. Items 3–6 describe the On setting; §4.7 gives the Off setting's own timing.

### 4.5 Behaviour by case

| Case | What happens |
|---|---|
| A preset that never passes full scale | Nothing, bit for bit |
| A transient over (a pluck's attack 1.3 dB over) | The wet is scaled so the over's sample sits at the ceiling; the dry is untouched. 1–8 frames per render sit exactly at the ceiling on the goldens with moderate overs. Then 10 ms of constant gain, then a 40 dB/s rise |
| A sustained over on program material (the +2.5 dBFS chord, feedback build-up) | Peaks vary by more than the 0.25 dB tolerance, so the hold expires and overs return during the release. The release then slows to 10 dB/s and the gain stays nearly still. On the owner's corner (Echolalia S11 a1r0s1t0, SoftNotes), the 50 ms windows with a rise and fall of 0.5 dB or more fall from 26 at 40 dB/s to 3; on S0 Saturation from 97 to 0 (Echolalia) and 191 to 0 (Updraft) (measured on the whole sum, §4.6) |
| A sustained over on a steady tone | The gain settles to the requirement and holds still: ripple 0.012 dB at 41 Hz and 0 from 82 Hz up (measured, sines) |
| A level that keeps rising | Each new maximum lowers the gain on its sample and bends the waveform there, in the wet alone while G ≥ F (§4.6) |
| The effect volume raised (Shift+Mix) | The wet gives way; the dry stays at its level. At +12 dB, every factory preset at its stored positions keeps its dry untouched on the three class inputs, with the wet down at most 10.1 dB (§3.3) |
| The Mix knob toward 0.5 | As the effect volume: at Mix 0.5, 10 of 18 presets limit on SoftNotes, the dry untouched and the wet down at most 2.5 dB (§3.3) |
| A hot input (the Saturation vector, a boosted guitar) | The wet gives way first, by up to 12 dB; past that the whole mix dips. With a full-scale dry and a −6 dBFS wet, the dry dips 1.0 dB and the wet 13.1 dB, back to unity 0.34 s after the dry stops (measured, synthetic) |
| Self-oscillation (feedback above 1) | Unchanged inside the loop. The tamer's saturator still bounds it, and the limiter caps what is heard. When the loop decays the gain follows it back up |
| DC | A steady gain reduction |
| The input stops | Tails decay through the release. The gain is 1 again within §4.4's times, and the output is today's from then on |
| The cutoff kill | b = 0: idle on the pedal; Mix 0's rule in the plugin |
| Freeze, triggers, bursts | No interaction: the limiter sees only the two terms of the mix |
| Spillover load (Trails, FastCut) | The gain, hold and both flags carry over, as the smoothers do (`Engine.cpp:754-770`), so a load mid-limiting does not jump the level. The exception is a load of an On preset from a settled Off over a clipping texture: see "The switch turned On" below |
| Exact load, `Restart`, `Reset` | G = 1, hold = 0, both flags cleared (§5.4) |
| A plugin dry over 0 dBFS | The output never exceeds the dry's own level on that channel; a wet that would add is held to it. While G ≥ F the dry is untouched |
| ±`FLT_MAX` input (plugin, hostile) | Output at most max(1, \|a\|) and finite. The gain can reach the floor; recovery takes 3.6 s, or about 14.5 s if an over returned during the release |
| Bypass, and trails while bypassed | Not the limiter's to decide: §6.5's requirement keeps the bypassed dry out of its gain |
| A preset with the switch Off | Below full scale, nothing. An over is clipped at its channel's ceiling. On the pedal the codec receives the same codes as from an engine without a limiter (§4.7) |
| The switch turned Off while the gain is below 1 (a `SetParam`, or a load of a preset with it Off) | No step: the gain drains to exactly 1 at 40 dB/s with no attack, and whatever then passes the ceiling is clipped. That takes 301 ms from F and 3.61 s from the floor (§4.7) |
| The switch turned On | From a settled Off, G is 1: the next over attacks as any first over does. During a drain, the gain continues from where the drain left it |
| The switch turned On over a clipping texture (a `SetParam`, or a Spillover load of an On preset with Trails from a clipping Off preset) | A step. While Off and settled, the clamp hides overs of any depth at G = 1. The first over after the switch attacks to its whole requirement in one frame, so the old preset's trails and the new grains drop by the depth of the clipped over, and when r < F the dry dips too. Overs keep returning, so the release then runs at 10 dB/s. Example: a dry at −1 dBFS (0.891) and a same-signed wet of 2.0 give r = 0.25/(0.891 + 0.5) ≈ 0.18, so the dry steps down by G/F ≈ 0.72 (−2.9 dB) at the load's frame (calculated). The owner chose this instant step over §4.7's entry ramp on 2026-10-10 (§11.5 Q9) |
| `Restart` or an Exact load with the switch Off | G = 1 and the switch as stored: a settled Off, as a fresh engine with that preset |

### 4.6 What it sounds like

- **Sustained material.**
  - On steady tones, from 82 Hz up the gain is exactly constant: the residual against a linear
    gain is about −153 dB at 82, 110 and 220 Hz (measured). The DSP lane measured the aliasing of
    a 5 kHz sine at −152.5 dB. At 41 Hz with the hold restart, the residual is −67 dB and the
    ripple 0.012 dB; without the restart, a plain 10 ms hold gives −49 to −61 dB at 41–220 Hz and
    0.05–0.135 dB of ripple.
  - On program material the hold cannot carry the gain, because a granular texture's peaks vary
    by more than 0.25 dB. With one 40 dB/s release, the gain left 1.0 thirty times on the owner's
    corner, with 476 new-maximum attacks in 1.54 s of limiting, and rose and fell by 0.5–1.1 dB
    within 50 ms windows at about 10–40 Hz. The whole mix fluttered, the dry included.
  - Releases compared on that render and on two S0 Saturation renders (whole sum, measured;
    record §2.5):

    | Release | Owner's corner: limited time, windows ≥ 0.5 dB (worst) | Echolalia S0 Saturation | Updraft S0 Saturation |
    |---|---|---|---|
    | 40 dB/s (draft 1) | 1.54 s, 26 (1.14 dB) | 97 | 191 |
    | 20 dB/s | 1.81 s, 10 (0.73 dB) | 72 | 71 |
    | 10 dB/s | 2.27 s, 0 (0.46 dB) | 0 | 0 |
    | **Dual: 40, then 10 once an over returns** | **1.85 s, 3 (1.14 dB)** | **0** | **0** |

  - A plain 10 dB/s release also removes the flutter, but it lengthens every isolated duck:
    `strum_marks` limits 2,750 frames instead of 1,293, and `midi_gate` 8,814 instead of 2,930
    (whole sum, measured by the sound review). The dual release keeps isolated transients exactly
    as at 40 dB/s and slows only once overs keep returning. Its constant is the Rings and DaisySP
    limiters' order of magnitude (about 8.3 dB/s, §3.1).
  - With the wet first, the gain moves the wet alone while G ≥ F. The dry, which carries the
    articulation, holds still.
- **Onsets.** The onset residual of a +2.5 dB burst is −24.2, −29.1 and −31.9 dB at 110 Hz,
  1 kHz and 5 kHz (whole sum). Today the codec clips the same burst on every cycle, so it is never
  clean there.
- **The bend at a new maximum.** Zero latency means the gain can only drop at the sample that
  needs it. The rising edge of a new maximum is held at the ceiling up to its peak, and the
  waveform bends there; with the wet first, the bend is in the wet.
  - On the goldens this means 1–8 samples per render at the ceiling (65 on `automation_offgrid`,
    542 on `hot_out`), where today the codec clips 1–5,752.
  - The engine's click test measures the bend at 5.84 times its neighbourhood's second
    difference on a 0.9 dB over at Mix 1 (§3.4). A soft knee lowers that to 4.19 times, and only
    look-ahead removes it.
  - This is what the owner's A/B (D11, D3) and the audition renders (§9.5) should be heard for.
- **The wet pumps more than the whole mix would.** The wet carries the whole reduction, so it
  dips deeper (at effect volume +6 dB on SoftNotes, −4.1 dB against the whole mix's −3.0 dB) and
  its wobble count is higher than the whole mix's under the same release (84 against 57). The dry
  does not move at all.
- **Hot inputs.** The S0 Saturation renders' sums reach +5.5 to +10.7 dBFS over a dry at full
  scale. The wet gives way by 12 dB and the whole mix dips by the rest: at the renders' peaks,
  with the dry at full scale and a same-signed wet, by 1.7 to 4.1 dB, where draft 1 pulled
  everything down by 5.5 to 10.7 dB (calculated: the dry's gain is 1/(1 + F·(S − 1)) at a sum
  peak S). The fix is input staging (§6.4, D12).

### 4.7 The switch: Off, the drain and On (normative)

D5, in the form the owner chose, gives each preset a switch, row 87 (§7.1). The engine holds it
as the flag `on`, set only through the row's rebuild. Off does not remove the ceiling: it holds
an over by clipping it instead of by lowering the gain.

**What Off means, and why it clips.** With the switch Off, an over is clamped at its channel's
ceiling c = max(1, |a|). This was the design's recommendation, and on 2026-10-10 the owner chose
it: asked what should happen to a sample over full scale with the switch Off, the owner answered
"Hard-clip" (§11.5 Q1, record §7.6). The alternative, an Off that passed s unchanged, was not
recommended:

- **On the pedal the two are the same sound.** The codec's `f2s24` clamps every sample above
  `FBIPMAX` = 0.999985 to code 8,388,482, and below −`FBIPMAX` to −8,388,482 (§2.2). A clamp at
  ±1.0 before it therefore changes no code: for any s, `f2s24(Clamp(s, 1))` = `f2s24(s)`
  (*calculated*: both sides clamp to ±`FBIPMAX` whenever |s| > 0.999985, and pass the same value
  otherwise). Off reproduces on the pedal, code for code, what revision 9's engine plays, which is
  what "limiter off" means on the pedal. This holds with C = 1.0. If L1b lowers C (D10), Off
  clips at the lowered ceiling, which is the reason for lowering it.
- **In the plugin only the clamp matches the pedal.** An Off that passed overs would hand the DAW
  floats above 1.0, which a floating-point mix bus plays unclipped. The preset's deliberate
  clipping would then exist on the pedal and not in the plugin, the divergence §2.2 describes and
  Goal 4 removes.
- **Goal 1 and the audition's guard survive.** No sample leaves the engine over full scale on
  either setting, so "no sample over full scale" (§9.5) holds for every preset.
- **The plugin's hot dry keeps its headroom.** The clamp uses the same ceiling as the limiter,
  max(1, |a|), so a live dry above 0 dBFS still passes.

**The Off path.** It is called from `Frame` (§4.3) only past the idle return, so a frame with
G = 1 and no over costs the same whatever the setting.

```cpp
// Off: no attack and no hold. A gain below 1 drains at k to exactly 1; from 1 on, the frame is
// the mix clamped at each channel's ceiling. Returns §4.3's bit set.
inline uint32_t OutputLimiter::Off(float aL, float aR, float bL, float bR, float cL, float cR,
                                   float pL, float pR, float& sL, float& sR) noexcept {
  const float p = pL > pR ? pL : pR;
  if (p > kLimCeiling && p > peak) peak = p;           // statistics, not sound: the would-be peak
  uint32_t lim = 0u;
  float    xL  = sL;                                   // settled: the mix itself
  float    xR  = sR;
  if (gain != 1.0f) {                                  // draining
    const float up = gain * release;                   // always the fast rate, k
    gain = up < 1.0f ? up : 1.0f;                      // exactly 1.0f at the end, by comparison
    if (gain != 1.0f) {                                // still below 1: §4.3's two forms
      if (gain >= kLimWetFloor) {
        xL = aL + gain * bL;
        xR = aR + gain * bR;
        lim = 1u;
      } else {
        const float t = gain * kLimWetFloorInv;
        xL = t * (aL + kLimWetFloor * bL);
        xR = t * (aR + kLimWetFloor * bR);
        lim = 3u;
      }
    }
  }
  const bool hit = detmath::Abs(xL) > cL || detmath::Abs(xR) > cR;  // one-sided: NaN never hits
  sL = Clamp(xL, cL);
  sR = Clamp(xR, cR);
  return hit ? (lim | 4u) : lim;
}
```

**The switch.** `RebuildDirty` calls `Switch` with the row's value, through a seventh parameter
domain (§7.1). The rebuild runs once per span, after the frame's events and before the frame
renders (`Engine.cpp:1085-1092` at `claude/tempo-core` `b91b34d`). So the switch takes effect on
the frame of its event or load, and only the frame's final value acts: an Off and an On at the
same frame change nothing.

That coalescing is the engine's, not `Switch`'s. Called twice at G < 1, Off then On, `Switch`
would clear the hold and both flags, set `releasing` and count twice, which changes the sound.
So the limiter's own test checks only that a call with the current setting changes nothing, and
an engine test checks the same-frame pair through `SetParam` (§9.4: test 15, and "The switch in
the engine").

```cpp
// value >= 0.5 is On: the threshold of ParamDisplay's two-state kinds (OffOn, LiveMark,
// and this row's LimitClip, §7.1).
inline void OutputLimiter::Switch(bool wantOn) noexcept {
  if (wantOn == on) return;                            // unchanged: no state is touched
  on = wantOn;
  if (!on) {                                           // to Off: drain from the current gain
    if (gain != 1.0f) ++offWhileLimiting;              // statistics, not sound
    hold      = 0u;                                    // no hold: the drain starts this frame
    releasing = false;
    slow      = false;
  } else if (gain != 1.0f) {                           // to On during a drain: continue from G
    ++onWhileDraining;                                 // statistics, not sound
    releasing = true;                                  // the drain was a release at k: an over
  }                                                    // that returns re-attacks and sets slow
}
```

**Turning Off while the gain is below 1: the drain.** The gain rises by k = 1 + 805·2⁻²³ per frame,
0.00083 dB, which is 40.0074 dB/s at 48 kHz. Then it is capped at exactly 1.0f, and from that
frame on the output is `Clamp(s, c)`. In detail:

- **No step anywhere.** On the switch frame G is the previous frame's G times k. Each later frame
  changes G by the same factor, which is the slope of the release after every isolated over (D3).
  The last drain frame and the first settled frame differ by at most that one step, since at
  G = 1 the wet-first form fl(a + 1·b) is s itself.
- **Clipping enters gradually.** No attack runs, so as G rises the part of the waveform past the
  ceiling grows from nothing at 40 dB/s, and the clamp clips it.
- **The fast rate, always.** The slow rate exists to stop the flutter of re-attacks during a
  release (§4.6). A drain has no attack, so nothing can flutter, and k bounds the drain at
  3.6 s from the floor where k_s would take 14.5 s.
- **Why not a crossfade.** A fixed crossfade to the clipped mix over `kFastCutFrames` (128 frames,
  2.67 ms) would lift a −12 dB gain to unity at about 4.5 dB per ms, a swell on exactly the
  material that was being held down. It would also compute both outputs while it ran. A crossfade
  long enough to be smooth is a release in all but name, so the drain reuses the release instead,
  with no new state.

Frames from the switch frame to exact unity, both included (*calculated* exactly, by iterating
the binary32 product G·k from the starting gain's bits; record §7.3). Each row that §4.4 item 3
also gives is its figure less 481 frames, the attack frame and the 480-frame hold, the floor row
included.

| Gain at the switch | Starting bits | Frames to exact unity | Time |
|---|---|---|---|
| −0.1 dB | `0x3F7D11D1` | 120 | 2.5 ms |
| −1.0 dB | `0x3F642905` | 1,200 | 25.0 ms |
| −2.5 dB | `0x3F3FF911` | 3,000 | 62.5 ms |
| −6.0 dB | `0x3F004DCE` | 7,199 | 150.0 ms |
| −11.4 dB | `0x3E89CE7C` | 13,678 | 285.0 ms |
| F, −12.04 dB | `0x3E800000` | 14,447 | 301.0 ms |
| −27.7 dB | `0x3D28CB8F` | 33,234 | 692.4 ms |
| The floor, 2⁻²⁴ | `0x33800000` | 173,362 | 3.612 s |

The starting bits of the dB rows are the binary32 rounding of 10^(−dB/20).

**Turning On.**

- **From a settled Off,** G is already 1, with the hold at 0 and both flags clear, which is the
  reset state. The limiter starts at unity gain, and the next over attacks instantly as any
  first over does (D3).
- **Over a clipping texture, that attack is a step.** While Off and settled, the clamp hides overs
  of any depth at G = 1. The first over after the switch sets G to its whole requirement in one
  frame. The trails a Trails load carries and the new preset's grains drop together by the depth
  of the clipped over, and when that requirement is below F the dry dips by G/F too (§4.5's
  example: −2.9 dB). On a clipping texture the overs keep returning during the release, which
  slows it to 10 dB/s (§4.3), so the recovery takes seconds. A `SetParam` to On does the same. This breaks §4.5's promise that a Spillover load does not jump
  the level, for this one case; `limit_clip_trails` pins it (§9.3) and risk 12 states it.
- **During a drain,** G continues from where the drain left it. The drain was a release at k, so
  without demand the gain keeps the same trajectory, and an over that returns re-attacks and
  switches the rest of that release to k_s, as in any release. Setting G to 1 here would step the
  level up; setting it to the requirement would step it down.

**The entry ramp, an alternative the owner did not choose (§11.5 Q9).** It mirrors the drain.
On a switch to On from G = 1, a flag `entering` is set. While it is set and the frame has demand,
G falls by one step of k per frame toward the requirement, never below it:
G ← max(r, fl(G·k⁻¹)), with k⁻¹ computed at `Init` as k is. The hold restarts, and the
clamp holds the ceiling for whatever the gain does not yet cover. The flag clears when G reaches
r, from which limiting is ordinary, or at the first frame without demand. Clipping then fades
into limiting within about 0.3 s (about 14,400 frames from 1 to F at 40 dB/s), instead of
stepping.

- *For it:* a Trails load of an On preset from a clipping one stays continuous, as every other
  Spillover load does, and the dry never steps.
- *Against it:* the incoming On preset clips, its own grains included, for up to 0.3 s after the
  switch, which an On preset is meant never to do. It adds a flag, a constant and a path to the
  limiter, which are not prototyped.
- *Recommended, and the owner's answer (2026-10-10):* the instant attack as designed, since it is
  D3's confirmed attack unchanged and the step occurs only on leaving a preset that clips by
  design. Asked how the level should drop when a limited preset loads with trails right after a
  clipping one, the owner answered "Instant" (record §7.6). The ramp is not built and stays
  specified here for reference; L2's A/B plays `limit_clip_trails` with the instant attack
  (§11.1).

**Lifecycle.**

- `Init` sets `on` = true, the row's default, before its own `dirty_ = kAllParamDomains;
  RebuildDirty();` (`Engine.cpp:710-711` at `claude/tempo-core`), so that rebuild sets `on` from
  the default with G = 1.
- `Reset`, and so `Restart` and an Exact load, primes G, the hold and the flags as in §5.4 and
  leaves `on` alone. `Reset` calls `RebuildDirty` itself (`:737-738` there), before the
  smoothers' priming, so `limiter_.Prime()` runs **before** that rebuild (§6.1). The rebuild then
  sets `on` from the leaf with G = 1, so `Switch` starts no drain and counts nothing. A preset
  with the switch Off restarts as a settled Off, the state of a fresh engine given that preset.
- A Spillover load carries G, the hold and the flags, and its rebuild applies the incoming
  preset's switch at the load's frame. On to Off mid-limiting starts a drain there; Off during a
  drain to On continues from G.

**Split and environment invariance.** The switch acts only at an event's or a load's frame, which
are stamped, through the rebuild that already runs per span. The drain is one product, one
compare and one selection per frame, with nothing decided per block. So every case of §4.4 item
8 holds with toggles at any frame. The golden preset `limit_switch` and the block-split test pin
it (§9.3, §9.4).

**What the Off setting costs.** The idle return is unchanged, and the On paths gain only the `on`
test. The Off path has no division: a settled clip is the ceilings, two compares and two clamps;
a drain frame is the release's product and §4.3's output forms. Neither exceeds the worst On path
(§8.1).

---

## 5. Arithmetic and determinism

### 5.1 The operations

All of these are in profile §3.1's set:

- sign-bit clears (`detmath::Abs`) and a sign-bit comparison (`SameSign`);
- compares and selections;
- per channel over its ceiling, one subtraction or addition and one binary32 division, and, when
  its wet would need more than 12 dB, one multiply, one addition and a second division;
- one multiply each for the hold test and the release step;
- per channel, one multiply and one addition (G ≥ F), or two multiplies, one addition and the
  multiply by t (G < F);
- integer counters.

There is no transcendental per sample. Each product is rounded before the sum that uses it:
the firmware builds with `-ffp-contract=off` and the host with `/fp:precise` or its equivalent
(profile §3.2), so `a + gain * b` never becomes a fused multiply-add.

The mix's own operations do not change. The engine computes `aL = dryL * mix.dry`, `bL = wl *
mix.wet` and `oL = aL + bL`: the same two products and one sum in the same order, so the mix's
bits do not change (§9.1).

The M7's `VDIV.F32` and the x86 `DIVSS` are both correctly rounded, so they agree. The division
costs 18 cycles on the Seed (`firmware/records/rev7-2026-10-07/session-1/bench-all.md:64`).

Multiplying by F and by 1/F = 4 is exact, except where a product underflows or overflows, which
the clamp then bounds.

The Off path and the switch (§4.7) use a subset of the same operations, with no division: the
release's one product, compares, selections and the clamp. The row's value is read with one
compare, `value >= 0.5f`, at the rebuild.

### 5.2 Constants computed at `Init`

`release`, `releaseSlow` and `holdFrames` are computed once, in `Init`, as `Smoother::SetTau`
computes its coefficient (`dsp/src/detail/Smoother.h:18-21`). Each release uses `Exp2D` in
binary64 with one rounding to binary32 (profile §3.9), and the hold uses the exact
`RoundHalfAwayI32` (`DetMath.h:51-57`).

At 48 kHz the results are `0x3F800325`, `0x3F8000C9` and 480. A unit test pins all three. Every
pedal-exact path runs at 48 kHz (profile §2.2), and other rates compute their own k, k_s and H in
the same way.

### 5.3 No flush site; subnormal outputs

**No flush site.** The gain never decays toward 0. It lies in [2⁻²⁴, 1] and moves up
multiplicatively, the hold is an integer and the flags are booleans. So profile §4.3 adds no
site. This is why the design rejects an envelope follower that decays to 0: as in the Rings and
DaisySP limiters, that would need a per-sample `FlushTiny`.

**Subnormal outputs.** A limited output is subnormal only when its dry and scaled wet nearly
cancel, or when |s| is below 2⁻¹²⁶ divided by the gain.

- On pedal-faithful inputs such a sample is not expected. The dry is on the 2⁻²³ grid, the
  wet's recursive states are flushed below 10⁻²⁰, and G ≥ 2⁻²⁴. With draft 2's limiter,
  `SubnormalOutFrames` was unchanged on all 45 golden presets (measured).
- Elsewhere the result is deterministic under the profile's gradual underflow on every target
  (profile §4.2).
- `Corpus.h:71-73` says arithmetic "cannot produce" a subnormal output. Its comment is amended in
  L2 to name the limiter as the one exception. The `subnormal_*` goldens peak at −4.9 and
  −10.5 dBFS and never engage it.

### 5.4 Lifecycle and invariance

| Entry point | Limiter |
|---|---|
| `Init` | computes `release`, `releaseSlow` and `holdFrames`; G = 1, hold = 0, flags cleared; `on` = true; counts zeroed |
| `Reset` (and so `Restart` and an Exact load) | G = 1, hold = 0, flags cleared, by a `Prime` placed before `Reset`'s own `dirty_ = kAllParamDomains; RebuildDirty();` (`Engine.cpp:681-682`; `:737-738` at `claude/tempo-core`), not beside the smoothers' priming after it; `on` kept, then set from row 87 by that rebuild, with G = 1. Counts kept, as `Stats()` keeps them |
| Spillover load (event or direct) | carried over: gain, hold and both flags; the load's rebuild applies the incoming switch at its frame (§4.7) |
| A change of row 87 (`SetParam`, or a load) | `Switch` at the rebuild before the frame renders (§4.7) |
| `Process` | per frame, inside the guard |

**What pins these rules.** Nothing in the revision-7 corpus does. Its two restarts, in
`restart_kept_params` at frame 288,010 and in `exact_load_mid` at 348,345, both find the gain at
1, and `spillover_chain`'s three loads all land while the limiter is idle (measured, draft 2 and
the determinism review's trace). Draft 1's claim that `exact_load_mid` "restarts while the
limiter is active" was wrong, and its RestartTail invariance proved nothing about priming.

L1's and L2's corpus versions therefore add (draft 2 numbered them 13 and 14; CLOCK's T1 has
since taken 13 and T2 is to take 14, so they are the next free, expected 15 and 16):

- loads and restarts placed inside held overs (`limit_sustain`, `limit_restart`, §9.3), and
  switches and loads that turn the limiter Off and On inside them (`limit_switch`);
- counters `LoadsWhileLimiting` and `RestartsWhileLimiting`, with `require` at least 1 each, so
  the coverage cannot silently vanish (§9.2);
- two engine tests: render into limiting, `Restart()`, and require the rest to equal a fresh
  engine bit for bit; render into limiting, apply a Spillover load, and require
  `OutStats().limitedFrames` to keep advancing across the load's frame (§9.4).

---

## 6. Where it sits, and why the plugin gets it too

### 6.1 In Pass 3c, last

```
 … ─► POST CHAIN ─► MIX (a = dry·gd, b = wet·g·gw, s = a + b) ─► ±FLT_MAX ─► LIMIT(a, b, s) ─► OUT
                     ▲
        dry (never delayed) ┘          feedback tap: Pass 3a, before all of this
```

The change to `Engine.cpp:1246-1260` (draft 2's prototype, measured):

```cpp
    const float aL = dryL * mix.dry;   // the dry terms
    const float aR = dryR * mix.dry;
    const float bL = wl * mix.wet;     // the wet terms
    const float bR = wr * mix.wet;
    float       oL = aL + bL;          // the same products and sum as today: the same bits
    float       oR = aR + bR;
    // ... the ±kMaxFinite saturation, unchanged ...
    const uint32_t lim = limiter_.Frame(aL, aR, bL, bR, oL, oR);  // output-limiter.md §4.3
    limited   += lim & 1u;             // 32-bit locals, folded into the 64-bit counts
    dryScaled += (lim >> 1) & 1u;      // once per RenderFrames call
    clipped   += lim >> 2;             // the switch Off and a channel clipped (§4.7)
    outL[n] = oL;
    outR[n] = oR;
```

The rest of the engine change:

- `Engine::Impl` gains `detail::OutputLimiter limiter_` next to the smoothers (`Engine.cpp:422`).
- `Init` calls `limiter_.Init(cfg.sampleRate)` next to the `SetTau` calls (`:642-645`), which
  come before `Init`'s own rebuild.
- `Reset` calls `limiter_.Prime()` **before** its `dirty_ = kAllParamDomains; RebuildDirty();`
  (`:681-682`; `:737-738` at `claude/tempo-core`), not next to the smoothers' priming, which
  follows that rebuild. So the rebuild's `Switch` always sees G = 1 after a restart, starts no
  drain and counts nothing (§4.7). `Prime` counts a priming that finds G < 1, and the Spillover
  path counts a load that finds G < 1 (§9.2).
- `RenderFrames` adds the three locals into `limitedFrames`, `dryFrames` and `clippedFrames`
  after its loop. When `on` is false it also adds its frame count to `offFrames`, and the first
  two locals to `drainFrames` and `drainDryFrames`: a frame limited with the switch Off is a
  drain frame. The switch is constant within one call, since every event and load ends a span,
  so these counts cost nothing per frame.
- The switch's row and domain (§7.1): `ParamDomain` gains `kDomainOutput = 1u << 6`, so
  `kAllParamDomains` becomes `0x7F` and the static assert (`Engine.cpp:127` at
  `claude/tempo-core`) follows. `RebuildDirty`'s loop runs to seven, and its switch gains
  `case kDomainOutput: limiter_.Switch(Active(ParamId::OutputLimiter) >= 0.5f); break;`
  (`:1190-1215` there). `RebuildDirty` also runs inside `Init` and `Reset`, not only once per
  span, which is why the two calls above sit before it.
- `Engine.h:97-112`'s list of what `Restart` and `Reset` do gains "the output limiter's gain
  returns to 1".

### 6.2 Outside the feedback loop

- **The loop is not changed.** The tamed wet is written to the feedback FIFO in Pass 3a
  (`Engine.cpp:1209-1225`), before the post chain and the mix. So the limiter never changes
  regeneration, the tamer's saturation, tails or anything the engine remembers.
- **The changed renders are known before minting.** The prototype's harness reads the
  limiter's statistics per render (§2.3). A model of the limiter run outside the engine must have
  the dry and wet terms separately, and must reset its gain at every `Reset` frame.
- **Unity returns exactly.** Once the gain is back at 1, the output equals the unlimited
  output.

### 6.3 The plugin plays it too

The engine is the one place both targets share. Putting the limiter there has three effects:

1. **The pedal and the plugin agree above full scale, which they do not today (§2.2).** Both
   targets check the same goldens, so a limited render is one sound on both.
2. **The plugin needs no wrapper code.** A limiter in the plugin alone would play a sound the
   pedal cannot. A limiter in the firmware alone would make every limited render a parity
   failure.
3. **What stays outside is chosen.** The wrapper's output level and Curation monitor trim
   (`PluginProcessor.cpp:1098-1107`, ±24 dB) act after the engine, as the pedal's analog output
   level does. A user who raises them can exceed 0 dBFS in the DAW by choice.

`LatencySamples()` stays 0. The plugin's live dry keeps its headroom above 0 dBFS through the
dry-referenced ceiling (companion §4.8), with §4.4 item 7's limits.

### 6.4 The pedal's analog side

- **The codec clamp.** With the limiter, LD's `f2s24` clamp touches only the 125 codes between
  0.999985 and 1.0 (§2.2).
- **Inter-sample peaks.** A sample-peak ceiling does not bound the peaks between samples at the
  DAC. The PCM3060's datasheet says nothing about headroom in its 8× interpolation filter
  (reported). Benchmark Media reports that every D/A chip it tested clips inter-sample overs,
  which reach 3.01 dB in theory and 1.5–2 dB in practice.
  - They exist today, without any limiter. With a 4× Hann-windowed sinc interpolator (32 taps a
    side, +3.00 dBTP on an fs/4 sine at 45°), stored-position renders on OnsetBursts reach +0.35
    to +1.51 dBTP at sample peaks of −1.0 to −2.7 dBFS, and Shards' stored Plucks render −0.16
    dBTP at a sample peak of −3.27 dBFS (measured by the sound review, spot-checked).
  - A limiter holds limited material at sample peaks of exactly 1.0, which maximises them: on
    the whole-sum limited goldens, `strum_marks` +0.91, `automation_offgrid` +1.33 and `hot_out`
    +5.51 dBTP (the sound review).
- **When to decide.** If the PCM3060 clips these, the ceiling must drop below 1.0, or the
  detector must be oversampled. Either changes every rated render whose peak lies between the new
  threshold and 0 dBFS, so it must be decided before revision 10 is minted, not in bench session 2.
  Lane L1b measures it first (D10):
  - the 0 dBFS output level, from a 1 kHz sine at sample peaks of 1.0, read on a scope or an
    audio interface;
  - inter-sample behaviour, from a 12 kHz sine (fs/4) at 45° phase with sample peaks at 1.0,
    whose true peak is +3.01 dB, captured to see whether the codec clips it.
  - **The firmware path.** No image can send exact samples at ±1.0 today: the bench image never
    starts audio (`firmware/bench/main.cpp`, no `StartAudio`), and the live image plays only
    `Engine::Process` fed by the codec input (`firmware/live/main.cpp:86-121`, `:607-610`). L1b
    adds a live-image console verb, `tone <hz> <phase> <amplitude bits>`. It mutes the engine
    through the existing `g_muted` path and has `AudioCallback` write a generated sine straight
    into `out`. It sits outside the engine, so it is not a sound revision. At amplitude 1.0 the
    expected codec sample is `f2s24(1.0)` = 8,388,482.
- **Output requirement.** The pedal's output stage must swing the DAC's full scale plus about
  3 dB. Today the Seed is passive after the codec.
- **Full-scale level.** TI's formula gives 0.8 × 4.5 V = 3.6 Vp-p, about 1.27 Vrms. Electrosmith's
  datasheet says 1 Vrms (reported). L1b measures it.
- **Input staging (D12).** The limiter's largest effect is on hot inputs, and the research's
  level table makes hot inputs normal. A humbucker's hard strum is 0.5–1.5 Vpp, −17 to
  −7.6 dBFS of 3.6 Vpp; active pickups, a boosted guitar or another pedal's output are 2–6 Vpp,
  −5 dBFS to clipping (`docs/research/daisy-pedal-platforms.md` §1.5, reported). At stored
  positions on SoftNotes the mix peaks up to 6.1 dB above the dry's own peak (−1.07 to −1.91 dBFS
  against a dry at −7.18), so a source about 2 dB hotter engages the limiter on 16 of 18 presets
  (calculated by the sound review). With the wet first, that engagement dips the wet, not the dry.
  The requirement handed to the hardware design: **the Instrument/Line pad puts the hottest
  supported source at or below about −7 dBFS peak at the codec.**
- These belong to the pedal's output-stage and input-stage designs (`hardware/` is empty) and to
  lane L1b (D10, D12).

### 6.5 Bypass and trails

The bypass topology belongs to its own design (engine §1 defers "footswitch assignment and
bypass topology"). The research's bar for the pedal includes buffered bypass with trails
(`docs/research/daisy-pedal-platforms.md` §3), the audition defines bypass as the dry input, and
the plugin's bypass crossfades to the sanitized dry while the engine keeps running (companion §5).

A trails bypass built on `Engine::Process`'s output would carry the bypassed dry through the
limiter's gain: a hot tail would duck it, and a bypass pressed mid-limiting would leave it low
through the release (up to 0.7 s from `hot_out`'s −27.7 dB, longer at the slow rate).

**The requirement handed to the bypass design (D13, confirmed):** the bypassed dry never passes
through the limiter's gain. Either the firmware outputs the input plus a wet-only tail, which it
must then keep within full scale itself, or the engine renders the tail with the dry term as the
input at unity, so the wet-first rule scales only the tail while the tail alone needs 12 dB or
less. On re-engaging, the limiter's state simply continues: it is never reset by a bypass. With
the switch Off, the same requirement keeps the bypassed dry out of the clamp: a bypassed dry is
never clipped for the preset's sake.

---

## 7. Control and display

### 7.1 The per-preset switch (D5, the owner's)

Draft 2 recommended no control at all, for three reasons:

- the limiter changes nothing below full scale;
- protective limiters are always on (§3.1);
- a per-preset switch would change every package's `sound_hash` and let a preset opt out of
  safety.

On 2026-10-10 the owner marked D5 "Change" on the decisions page, with no note. Asked in the
session "For limiter decision D5 (user control), what change do you want?", with four options (a
device setting, on or off; off in the plugin only; a per-preset switch; an adjustable ceiling),
the owner answered **"Per-preset switch"**. That option read: "A stored preset parameter, so a
mode can turn the limiter off on purpose (e.g. for deliberate clipping). Adds a new leaf, so
every package hash changes (package-change label)". **The per-preset switch is therefore the
owner's decision**, On by default. (Draft 4 labelled it a proposal, because its reviewers saw
only the decisions page; record §7.5.)

This design keeps draft 2's safety reasons by making Off a clip at the same ceiling (§4.7): a
preset chooses how an over is held, never whether. That was the design's recommendation for what
Off does, and the owner chose it on 2026-10-10 ("Hard-clip", §11.5 Q1). The package cost is paid
once, at revision 10 (§10). The rest of this section specifies the switch.

**The row.**

| Field | Value |
|---|---|
| ID | **87**, appended after 86 as host indices require (compiler §4.5): rows 1–82 are `main`'s (`dsp/include/brainscape/Params.h`); 83–85 are the tempo core's (`Params.h` at `claude/tempo-core`, revision 8); 86 is `global.tempo_glide`, claimed by clock §10.4 for T2 (revision 9). No other branch or design claims a row above 86. Any lane that appends a row before L2 lands starts at 88. With D5's form the owner's, 87 is **claimed for the limiter's switch**, `output.limiter` |
| Row 86 first | The table must stay contiguous from 1 (`static_assert(TableIsContiguous())`, `Engine.cpp:36` at `claude/tempo-core`), so row 87 cannot exist without row 86. D7 expects T2 to land first, with 86 as a `Global` row. If L2 lands first, it adds 86 as a `Reserved` row under its final name, `global.tempo_glide` (0–1, default 0, no domain, `sinceRev` 0), with a display row like row 85's (group `Device`, "Tempo glide", `OffOn`), for T2 to turn into a `Global` row; a Reserved row is never stored and changes no hash (`Params.h:141-155` there). The limiter's revision is then 9, not 10, and its `sinceRev`, `Package-change:` line and every "10" here take the actual number (§10) |
| Name | `output.limiter`: the leaf's path in the document (compiler §2.2), in a new top-level object `output` |
| C++ | `ParamId::OutputLimiter = 87`, in a new section of the enum after the tempo core's rows |
| Kind | `Leaf`: stored in every package, `SetParam` stores it, `LoadPreset` gives it its default and then the stored value (compiler §4.1) |
| Range, default | 0 to 1, default **1 (On, Limit)**, unit `""`. Integer-valued: 0 is Off (Clip), 1 is On (Limit). Read as any integer-valued leaf (compiler §2.2, §3.7): a fraction is accepted and plays by the threshold below |
| How the engine reads it | `value >= 0.5f` is On, the threshold of `ParamDisplay`'s two-state kinds (`dsp/src/ParamDisplay.cpp:429-433` at `claude/tempo-core`). On [0, 1] it equals compiler §3.7's `RoundHalfAwayI32(value) != 0`, so a fraction plays as the integer the UI shows |
| Domain | `kDomainOutput`, a new seventh bit, `1u << 6` (§6.1). The rebuild calls `Switch` (§4.7) |
| `sinceRev` | 10, the limiter's revision (compiler §7.3's missing-leaf rule) |
| Display row (`ParamDisplay.cpp`) | group `GrainDelay`, beside Mix and the wet trim, the mix stage's rows; title **"Output ceiling"**; short title "Ceiling"; `Linear` taper; a new kind **`LimitClip`**, shown **"Limit"** (≥ 0.5) or **"Clip"**, beside `OffOn` and `LiveMark` ("Mark"/"Live"); 2 steps; flags `kLeafStep` (discrete, not automatable) |
| Typed text in the plugin | "limit", "clip", "on", "off" and the numbers: `NamedValue`'s table (`plugin/src/BrainscapeParam.cpp:176-200` at `claude/tempo-core`) gains the four names for `LimitClip`, as `LiveMark` has "live" and "mark". `isBoolean()` (`:310`) returns true for `LimitClip` as for `OffOn`, so hosts show a two-state control that reads Limit or Clip |

**In the package (STAT).** The leaf is one entry, `{u32 87, u32 bits}`, last in ascending order:
bits `0x3F800000` for On, `0x00000000` for Off (compiler §6.2).

- **Every package written at revision 10 or later carries it.** The compiler writes every `Leaf`
  row of a present element, and `output` is always present. STAT grows by 8 bytes, to 34
  leaves if T2 has made row 63 the 33rd.
- **An older package lacks it and still loads exact,** with the switch at its default On: its
  `sound_rev` is below the row's `sinceRev` (compiler §7.3 step 2). A package of revision 10 or
  later that lacks it loads inexact, as with any missing leaf.
- **A build before revision 10 ignores STAT ID 87** and reports the load inexact, as with any
  unknown leaf.

**In the document (compiler).**

- **Schema.** `v.Object("output", false, [&] { v.Leaf("limiter", ParamId::OutputLimiter); });`
  goes in `Visit` after `wet_trim_db` and before `trigger` (`compiler/src/Schema.cpp:459-460` at
  `claude/tempo-core`). That is signal order: post chain, dry duck, wet trim, then the output.
- **Canonical form.** `"output": { "limiter": 1 }`, written in every document, default included
  (compiler §6.4). L2's pull request re-formats every committed document with `bspc fmt`: the
  corpus's, the compiler's examples and the factory set.
- **Reading.** As every integer-valued leaf (compiler §2.2, §3.7): any JSON number, read as
  correctly rounded binary32, in [0, 1]. `0`, `1`, `1.0`, `1e0` and `0.3` are all accepted; a
  fraction plays by the threshold (0.3 is Clip). A value outside 0–1 is E4, as for any leaf.
  `true` and `false` are E3, a type mismatch, because leaves are numbers. The compiler writes
  whatever value the document holds, so `fmt` and decompile round-trip any value a package or the
  engine can hold: `DecodePreset` accepts any canonical STAT value and `SetParam` stores any value
  in [0, 1] (below), so a decompiled `"limiter": 0.3` must compile back (compiler §10.1), and the
  random-document property test, which writes every leaf with a random value in its range
  (`compiler/tests/test_property.cpp:127` at `claude/tempo-core`), needs no exception. The app's
  toggle writes 0 or 1; typed text and the console's `set` can store a fraction, which plays by
  the threshold.
- **References (E8).** A macro target on `output.limiter` is E8, as `global.mix`'s is
  (`Schema.cpp:1575`), and so is an expression assignment on it, which `global.mix` allows. The
  message: "output.limiter is no macro's or expression's target: the preset sets it".
- **Lint L15** (the next free: L1–L9 are compiler §2.7's, L10–L11 cpu-budget §7.4's, L12–L14
  clock's): "the output ceiling clips (output.limiter below 0.5): an over is clipped at full
  scale, not limited". It is a note for a user preset and, under the owner's D14, an error under
  `--factory`; once the `--clips` declaration exists (§9.5), not for an id it marks.
- **An older compiler** reads `output` as E2, an unknown key, as with any later key.

**Validator (`dsp/src/blob/`).**

- **`DecodePreset` gains no STAT rule.** Any canonical value decodes, since out of range is not
  invalid (compiler §5.3). `LoadPreset` canonicalizes it, and the rebuild reads it by the
  threshold.
- **`ValidateMode`'s `CheckSemantics`** rejects a MACR target on row 87 with a new
  `PresetError::TargetLimiter`, beside `TargetMix` (`Validate.cpp:320` at `claude/tempo-core`).
  It rejects a CTRL expression assignment on row 87 with the existing
  `PresetError::ExpressionTarget` (`:388-394`).
- **Two frozen fixtures pin those verdicts,** and the blob fuzzer's verdict digest is re-minted
  for the new code.

**The engine.** §4.7 gives the switch's behaviour and §6.1 the domain and the rebuild. A
`SetParam` on row 87 stores it like any leaf, and `LoadPreset` step 3 marks every domain, so a
load applies the incoming switch at its frame.

**Who may reach it.** Mode-compiler's rules decide this: macros and expression act on leaves
through `EvalMacro` (compiler §3.3–§3.4), and hosts follow the host model of Q12, provisionally
(b) (compiler §3.6; `ParamDisplay.h`'s `kParamAutomatable`).

- **Macros: no** (E8 in the compiler, `TargetLimiter` in the validator). A macro maps a knob's
  travel continuously. On a two-state row it would flip how overs are held at one point of a
  sweep, which is a change of sound design inside a knob gesture. `derive` and pickup (compiler
  §3.5) would also have only 0 or 1 to work with.
- **Expression: no** (E8, and `ExpressionTarget` in the validator). CTRL is outside `sound_hash`
  (compiler §6.3). An expression assignment could therefore make a package whose STAT, and so
  whose hash, says On play Off, out of reach of the factory lint and the audition, which read
  STAT. A pedal rocking across the threshold would also chatter between limiting and clipping,
  and a switch to On is an instant attack.
- **Hosts: registered, not automatable.** Under model (b), only the macros, Mix, the effect
  volume and the performance rows are automatable (`HostAutomatable`, `ParamDisplay.cpp:130-134`
  at `claude/tempo-core`). Every other leaf, this one included, is registered without
  automation, so a host records the knobs a player turns. An automation lane on this row would
  change a bounce's sound with nothing in the preset to show it. A host's generic editor can still
  set it; that arrives as a `SetParam` at the chunk's frame, like any leaf edit. If the owner ever
  chooses model (a), this row stays non-automatable for the same reason: (a) is about the leaves
  a macro fans out to, which this row never is.
- **MIDI CC and the panel: none.** It is not among clock §6.5's CCs, not a Shift secondary and
  not on the pedal's panel. It is a property of the preset, edited in the app, as the owner's
  per-preset answer to D5 has it.

**Determinism and the packages.**

- **No render changes for the switch but one.** Every preset plays it at its default, On, which
  is the limiter, except `limit_off_hot`, which L1 mints before the row exists and L2 sets Off
  (§9.3). Parameter-list goldens lack the row and load the default; packages gain it at 1.
- **Every committed package's `sound_hash` changes,** because the compiler writes the new leaf
  into STAT, which `sound_hash` covers (compiler §6.3). That means the corpus's packages, the
  compiler's examples and the factory set, with `golden.json`'s `soundHash` entries and both
  `MANIFEST`s. No `control_hash` changes.
- **The revision that carries it is the limiter's own, 10,** in the same commit as its re-mint
  and the re-stamp of every package. It cannot be folded into T2's re-stamp: at revision 9 row 87
  does not exist, so T2's compiler cannot write it. If T2 and the limiter share one pull request,
  each keeps its own commit and re-stamp (profile §5.12's per-commit rule). The request then
  carries the package-change label once, with one `Package-change:` line per revision, as wave
  1's did for revisions 4, 6 and 7 (compiler §7.6).
- **The limiter's line:** `Package-change: sound revision 10 adds leaf 87, output.limiter, which
  the compiler writes into every package at its default (On, Limit); limit_off_hot's package sets
  it Off, so its render becomes revision 9's clamped at ±1.0 (output-limiter.md §9.3's check);
  every other package preset's changed render is the limiter's own (§9.1's predicted set)`.
- **The gate.** The pull request bumps, so a changed render of a parameter-list preset is the
  engine's. A changed render of a preset whose package changed is the package's
  (`golden_changes`, `tools/ci/sound_rev_gate.py:394-414`), and at revision 10 every package
  changes. So the gate's report attributes to their packages the changed renders of the package
  presets among §9.1's set (`strum_marks`, `spillover_chain`, `lone_changes` and `midi_gate` of
  the eight; `golden.json` at `claude/tempo-core`), which are the limiter's, and
  `limit_off_hot`'s, which is the switch's. The line above names both, and the label covers the
  package hashes. Neither the bump nor "sound-neutral" waives the label (compiler §8.3).
- **Frozen fixtures are never re-stamped.** `r2-onset-marks.bsp` (`sound_rev` 2) must still load
  exact with the switch On, which L2 checks.
- **Sessions.** A `BSWS` v1 session saved before revision 10 has no `output.limiter` and plays On.
  It loads **marked inexact**, as for every leaf added since v1: `DecodeState` counts the missing
  leaf (`plugin/src/StateCodec.cpp:179` at `claude/tempo-core`), BSWS v1 carries no sound
  revision for a `sinceRev` exemption, and `setStateInformation` reports any missing id as
  inexact (`PluginProcessor.cpp:518`, `:532`). T2's row 63 does the same. The sound is unaffected,
  since the absent value is the default.

### 7.2 The engine API (not sound)

```cpp
// Engine.h, beside GrainStats
struct OutputStats {
  uint64_t limitedFrames         = 0;  // frames output at a gain below 1 (a drain's included)
  uint64_t dryFrames             = 0;  // of those, frames with G < F: the whole mix scaled
  uint64_t limiterEngages        = 0;  // frames at which the gain left 1
  uint64_t restartsWhileLimiting = 0;  // Reset (Restart, Exact load) that found G < 1
  uint64_t loadsWhileLimiting    = 0;  // Spillover loads that found G < 1
  uint64_t offFrames             = 0;  // frames rendered with the switch Off (§4.7)
  uint64_t clippedFrames         = 0;  // of those, frames at which the clamp clipped a channel
  uint64_t offWhileLimiting      = 0;  // switches to Off that found G < 1: drains started
  uint64_t onWhileDraining       = 0;  // switches to On that found a drain running
  uint64_t drainFrames           = 0;  // of limitedFrames, those with the switch Off: a drain's
  uint64_t drainDryFrames        = 0;  // of those, frames with G < F: the drain's whole-mix form
  uint8_t  on                    = 1;  // the switch as the engine plays it now
};
// Audio thread only (plain 64-bit counts, as Stats). Counts since Init, which Reset,
// Restart and loads keep, so a caller reads the difference over a render.
OutputStats OutStats() const noexcept;
// Audio thread only: the lowest wet gain since the last call (the gain at that call if no
// attack followed), then restarts from the current gain.
float ConsumeLimiterMinGain() noexcept;
// Audio thread only: the largest |s| over full scale since the last call (0 if none), then 0.
float ConsumeLimiterPeak() noexcept;
```

`ConsumeLimiterPeak()` is the "would-be peak": what the mix would have reached without the
limiter. With the wet first, the lowest gain no longer equals 1/peak, so the peak is tracked on
its own, on frames with an over only. With the switch Off it is the peak the clamp cut.

### 7.3 The plugin

- **The lamp, the dry indication and the readout.** Beside the output meter
  (`plugin/src/PluginEditor.cpp:267-268`):
  - a **LIM** lamp, lit while `limitedFrames` has advanced in the last 250 ms;
  - a **DRY** mark on the lamp while `dryFrames` has advanced in that time: the dry is dipping
    too;
  - the wet's gain reduction in dB, the lowest gain over the last UI interval, held for 1 s.
  - **The switch.** While `OutStats().on` is 0 the lamp's caption reads **CLIP** in the warning
    colour from the moment the preset loads, unlit until something clips. It is lit while
    `clippedFrames` has advanced in the last 250 ms, and also while a drain still lowers the gain
    (`limitedFrames` advancing). So a player hearing a preset clip sees that the preset does it on
    purpose.
- **The audio side.** `RenderChunk` reads `OutStats()`, `ConsumeLimiterMinGain()` and
  `ConsumeLimiterPeak()` after `Process` and raises atomics, as `outPeak_` does
  (`PluginProcessor.cpp:1107`).
- **Where the switch is edited.** It is a leaf, so the Curation slice's **Leaves** view lists it
  with the other raw leaves, under "Grain delay" as "Output ceiling", **Limit** or **Clip**, a
  two-state control. The schema-generated editor form shows it as `output.limiter`. The **Pedal**
  view does not: it is no knob and no Shift secondary. Before a save the slice's lint shows L15
  for a Clip preset, as an error for a `factory.` id that no `--clips` declaration marks (D14).
- **Clip presets are marked where presets are chosen.** The Library list and the Modes menu show
  a CLIP tag on every preset whose stored switch is Off, read from its STAT or document, so a
  preset that clips by design is known before it is loaded, including a user preset synced to the
  pedal's slots from phase E (compiler §9.1's Library banks).
- **The Curation view.** Its one-click render report shows each render's limited frames, dry
  frames, clipped frames and would-be peak, and the preset's switch. For a Clip preset with a
  non-factory id, the switch itself is the declaration: the report gives the clip share per
  render where Peak (stored) and Ceiling (moved) would fail it (§9.5).
- **The words.** Every user-facing place says **Limit** and **Clip**, never On and Off: the
  Leaves view, the host's generic editor (the `LimitClip` kind, §7.1), L15's message, the lamp's
  caption and the console. "Off" would read as "overs pass as floats", which a DAW user could
  trim later; here Clip is a hard clip inside the engine, before the wrapper's output level and
  the monitor trim (§6.3), so lowering those cannot undo it.
- The meter is measured at the engine's output, which is the last digital stage of the pedal.
  This avoids the H90's fault, a clip LED placed before the last gain stage (§3.1).

### 7.4 The pedal

- **The console, read safely.** `OutStats()` and the two `Consume` calls are audio-thread only:
  the 64-bit counts are written with separate 32-bit stores, and a `Consume` is a read then a
  reset. So the console never calls them. As `ConsumeOnsetCount()` is published through
  `g_onsets` (`firmware/live/main.cpp:116`):
  - `AudioCallback` calls `OutStats()`, `ConsumeLimiterMinGain()` and `ConsumeLimiterPeak()`
    after `Process` each block;
  - it folds the frame, dry-frame, engagement, off-frame and clipped-frame deltas into
    `std::atomic<uint32_t>` counters, and stores `OutStats().on` in an atomic byte;
  - it keeps the lowest gain and the highest peak as float bits in `std::atomic<uint32_t>`.
    Positive floats order as unsigned integers, so a plain min or max works;
  - it clears all of them on `g_statReset`;
  - `stats` (`main.cpp:332-357`) reads them inside its existing interrupt-disabled snapshot and
    prints `limiterMode limit|clip`, `limitedFrames`, `limiterDryFrames`, `limiterEngages`,
    `limiterOffFrames`, `limiterClippedFrames`, `limiterMinGainBits` and `limiterPeakBits`,
    which the host tools decode.
  - This code runs from QSPI in the interrupt, outside the governor's 85 % and inside the 15 %
    of budget §1. Draft 2's prototype builds it, all but the switch's three fields.
- **Setting the switch from the console.** `set` reaches every Leaf row by a name the firmware
  lists, and its build fails while a leaf has none (`EveryLeafSettable`,
  `firmware/live/main.cpp:187-232` at `claude/tempo-core`). L2 adds `{ParamId::OutputLimiter,
  "OutputLimiter"}`, so `set OutputLimiter 0` sets Clip for a bench check, as a `SetParam`.
- **The Rev7's LED.** The breadboard's one user LED pulses for onsets today
  (`main.cpp:116-120`). It shows limiting instead by default: lit while `limitedFrames` advanced
  in the last 250 ms, and blinking while `dryFrames` did. With the switch Off it flickers fast
  while `clippedFrames` advances. A console verb, `led onsets|limiter`, switches it back for
  onset work. This is firmware only, not a sound revision.
- **A stored Clip shows at load, not only while clipping.** When the published `on` byte goes to
  0, at a load, a `set` or boot, the LED gives a double blink, in both `led` modes, so a Clip
  preset is seen before it clips. Without it, a Clip preset that is not clipping shows nothing,
  and its clipping later reads as an input-level fault.
- **The product's panel.** Its indication belongs to the control-surface design, which owns the
  LEDs. Draft 4 hands it two requirements with D6's consequence, for D5's switch: the panel shows
  that the loaded preset's switch is Clip from the moment it loads, not only while clipping, and
  it shows clipping as distinct from limiting.

---

## 8. CPU and code placement

### 8.1 Cost (from the live image)

Draft 1 modelled the cost from a mock of Pass 3c. The embedded review disassembled the live image
instead and found the model low (record §6, H1). The figures below are counted from draft 2's own
live image: `Engine::Impl::RenderFrames` disassembled from `brainscape_live.elf`, built with the
firmware's flags (`-O3 -mcpu=cortex-m7 -mfpu=fpv5-d16 -mfloat-abi=hard -ffp-contract=off`,
profile §3.2). Each path is counted from the saturation's first compare to the loop's branch
back, so it includes today's 18-instruction tail (4 `VMRS`).

| Path, pedal (c = 1) | Instructions | `VMRS` | `VDIV` | Model cycles per frame |
|---|---|---|---|---|
| Today's tail (saturation and stores) | 18 | 4 | 0 | ≈ 22 |
| Idle (G = 1, no over) | 33 | 7 | 0 | ≈ 40 |
| Release, no over | 94 | 17 | 0 | ≈ 111 |
| Hold restart under demand, one channel over, wet only | 112 | 23 | 1 | ≈ 151 |
| Attack while limiting, one channel over, wet only | 120 | 23 | 1 | ≈ 159 |
| Release under demand, one channel over, wet only | 128 | 25 | 1 | ≈ 169 |
| **Worst: a new engagement, both channels past F, the whole mix scaled** | **163** | **26** | **4** | **≈ 253** |

**The model.** About one instruction per cycle (budget §2), plus 16 cycles for each `VDIV`, whose
18-cycle result is consumed by the next compare, plus one cycle for each `VMRS`. Branch
mispredictions are not counted: the idle path takes 6 branches per frame against today's 1,
because GCC moved the saturation's common case out of line (the review's finding, which draft 2's
image shows too).

- **Idle** costs about 18 cycles per frame more than today: 0.86k per block (0.18 %).
- **Common limiting paths** cost 111–169 cycles per frame, 5.3–8.1k per block if every frame of
  the block takes one.
- **The worst path** needs a new engagement on a frame where both channels' wets would need more
  than 12 dB: a hot dry on both channels, as the Rev7's mono input gives. About 253 cycles per
  frame, 12.1k per block.
- **The statistics are off the hot path.** `engagements` and `lowestGain` change only in the
  attack branch, where G falls; `peak` only on frames with an over; the frame counts in 32-bit
  locals folded once per call (§6.1).
- **The whole-mix alternative (D11)** is cheaper: one division per frame, no second stage. The
  embedded review counted draft 1's longest pedal path at 115 instructions, 22 `VMRS` and one
  `VDIV`, and proposed a placeholder of 160 cycles per frame.
- **Bit-identical savings L2 may take:** when both ceilings are 1, the second stages of both
  channels can share one division, F/max(|a_L + F·b_L|, |a_R + F·b_R|), because a correctly
  rounded quotient is non-increasing in its denominator (draft 1's one-division branch rested on
  the same fact). That removes one `VDIV` from the worst path. A portable likely-hint on the idle
  return changes no IEEE operation. Either needs an equivalence test (§9.4).

**The switch (draft 3, not prototyped; *estimated* from the paths above).**

- **Idle is unchanged** on both settings: the `on` test sits after the idle return.
- **Every On limiting path gains the `on` test:** a load, a compare and a not-taken branch, about
  2 cycles. The worst path is about 255 cycles per frame against the placeholder's 260.
- **A settled Off frame with an over** runs the ceilings, the hit test and two clamps, with no
  division: about 50–60 instructions and 60–75 cycles.
- **A drain frame** runs the release's product, §4.3's output forms, the hit test and the
  clamps, with no division. That is about the "release, no over" path's 111 cycles, and below
  the 169 of "release under demand", which divides.
- **The placeholder of §8.2 therefore stands.** Session 2 times the Off paths beside the others
  (§8.5).

These are models until bench session 2 times each path on the Seed (§8.5).

### 8.2 How the governor charges it

The limiter's cost depends on the signal, not on parameters. The governor's per-frame refund
P(f) is a function of parameter targets, which keeps it split-invariant (budget §5.1). The
limiter's activity in a frame is known only in Pass 3c, after the grains of that block have
rendered in Pass 2, so the governor cannot condition admission on it.

The limiter's worst case therefore joins the **static reserve S** (budget §5.2–§5.3), as a line of
its own:

- **Placeholder:** 260 cycles per frame, the worst path's model rounded up: S grows by 12.5k
  cycles per 48 frames, from about 148,000 to about 160,500 (2.6 % of the block).
- **Effect on R:** by budget §5.3's formula, R = (408,000 − S − Cmax)/48 − 1,270, R falls by
  about 260 cycles per frame.
- **Voices:** that is about 1.8–2.2 voices at the Hermite weights 119–145, or about 4.8 at the
  unity weight 54. The loss applies only at corners where the governor binds (calculated). The
  whole-mix alternative's 160 cycles would cost about 1.1–1.3 Hermite voices: the wet-first rule
  costs about 0.7–0.9 of a voice more.
- **The final figure:** session 2 times every path (§8.5), and S takes 48 × the slowest
  measured path plus budget §8.3's 10 % margin, before D8 of the budget freezes the constants.
- **The shared model:** `CostModel.h` and the plugin's "Seed load" meter (budget §7.4) include
  the same fixed term.

### 8.3 Code placement: ITCM

**Where the code goes.** It is header-only and inlined into `Engine.cpp`'s `RenderFrames`.
`Engine.cpp` is placed whole in ITCM: the ITCM input pattern is the whole `libbrainscape_dsp.a`
apart from the decoder, encoder, SHA-256 and test-signal objects (`firmware/CMakeLists.txt:198`,
`:217`). So the code lands in ITCM with no linker change.

- It must stay there. It runs every frame, and from QSPI the engine runs 1.05–1.21× slower warm
  and 1.26–1.70× cold (budget §1).
- It uses scalar constants only. An `inline constexpr` table would be a COMDAT section, which
  `cmake/ItcmCheck.cmake` rejects unless it is listed in `_bs_engine_comdat`
  (`firmware/CMakeLists.txt:211-216`). `Need` inlines: ItcmCheck reports 6 of the engine's 6 COMDAT
  sections, all in ITCM (measured).

**What it costs (measured).** Each prototype's live image was built against `main`, with the same
compiler and libDaisy:

| Image | `.itcm_text` | Change | Spare, counting the 64-byte offset |
|---|---|---|---|
| `main` (revision 7, with steps 1–2) | 62,264 bytes | — | 3,208 |
| Draft 1 (whole mix, no statistics accessors) | 63,128 | +864 | 2,344 |
| The whole-mix alternative with the dual release and its accessors | 63,264 | +1,000 | 2,208 |
| **Draft 2 (wet first, dual release, statistics and accessors)** | **63,544** | **+1,280** | **1,928** |

Draft 2's 1,280 bytes, by function: `RenderFrames` +1,026 (1,740 → 2,766), `Init` +152 (two
`Exp2D` calls), `Reset` +24, the three accessors 50 (`OutStats` 26, `ConsumeLimiterMinGain` 12,
`ConsumeLimiterPeak` 12), and 28 of alignment. The accessors' QSPI veneers (8 bytes each) are
outside ITCM. The counts of loads and restarts while limiting (§7.2) are not in the prototype;
L2 re-measures once they link.

- **Hot and cold.** `RenderFrames` and the accessors run in the audio callback: 1,076 bytes.
  `Init` and `Reset` are main-thread code, 176 bytes, which leave ITCM with the rest of the
  main-thread API once the engine is placed by function (budget §7.3). Draft 1 gave `Init`'s
  share as about 40 bytes; it was 112.

**If bytes are needed later:** 32-bit counters save about 30 bytes, and the whole-mix
alternative of D11 280 (measured). Fixing c at 1 would save the two `Ceiling` computations, but
the plugin's hot-dry contract needs them; draft 1's mock measured about 120 bytes for its separate
plugin branch, which draft 2 no longer has.

**The switch (draft 3, *estimated*).** It adds 100–200 bytes:

- the Off path and the `on` test in `RenderFrames`;
- `Switch` and the Output case in `RebuildDirty`, both in `Engine.cpp`'s ITCM object;
- the fields `OutStats` copies.

It is not prototyped, and L2 measures it.

**After CLOCK's tempo core (measured on `claude/tempo-core`, clock §11.11 item 22).**

- T1 grew the live image's `.itcm_text` from 62,256 to 64,680 bytes, which leaves **792 bytes**.
  §9.6 of clock estimated 1.0–1.6 KiB for T1 and T2 together; T1 alone took 2.4 KiB.
- The limiter's 1,280 bytes plus the switch's no longer fit. Neither do T2's crossfade and slew,
  by clock's own note.
- So **L2 starts with the cold-code move** that budget §7.3 and clock §9.6 describe. Either the
  main-thread API leaves `Engine.cpp`'s ITCM object, or the engine is placed by function. The
  audio callback never reaches about 16 KB (budget §4.1, an estimate). Whichever of T2 and L2
  lands first builds it, and the other rebases on it.
- The limiter's own 176 cold bytes (`Init`, `Reset`, and `Switch` once measured) leave ITCM with
  that move.

**Effect on the ITCM gates (calculated, on revision 7's image; draft 2's tally, kept for the
governor's lane).**

- On revision 7's image the limiter fits without moving cold code. After T1 it does not (above).
- The tally against budget G6's 8 KiB spare, with everything planned that shares the room:
  1,928 bytes after the limiter, less step 4 (3,752) and the governor (about 2,300), is −4,124;
  less CLOCK's tempo core (1.0–1.6 KiB, clock §9.6), −5,148 to −5,762.
- Keeping G6 then means moving 13,340–13,954 bytes of cold code out, plus step 5's share, which
  is not measured yet. Budget §7.3's figure was about 11.0 KB before the limiter and CLOCK.
- Against the roughly 16,016 bytes the audio callback never reaches (budget §4.1, an estimate,
  plus the limiter's 176 cold bytes), that leaves about 2.2–2.9 KB for step 5. Under draft 1
  the review computed about 2.5–3.1 KB.
- With D7 confirmed, the tally must be redone from T1's measured image rather than clock §9.6's
  estimate. T1 took 2.4 KiB where the estimate gave T1 and T2 together 1.0–1.6 KiB. So the room
  left for step 5 shrinks by about 0.8–1.4 KB plus whatever T2 measures. L2 records the figure
  from its own image once the cold-code move is built.

### 8.4 DTCM

`Engine::Impl` grows by 72 bytes, 56 of them measured in the prototype (§4.2), plus the switch's
flag and six counts, about 52 bytes (*estimated*). There is no new arena and no buffer, because
there is no look-ahead. The prototype's live image's DTCM use is unchanged at 35,840 bytes
(measured). On top of revision 9 the total may pass `kEngineImplBytes`' 8,192 on the M7: 7,944
at revision 7, plus the tempo core's state and these 124 bytes. If it does, L2 raises the
constant, which the firmware's 9 KiB DTCM slot allows (§4.2).

### 8.5 Bench session 2

The additions to budget §8:

- **Images:** every image of budget §8.1 at revision 10 or later carries the limiter.
- **`SuiteMicro`:** each path of §8.1 timed separately with DWT on the bench image: idle, attack,
  hold restart, hold decrement under demand, release under demand, release without demand, both
  channels past F with a new engagement, and the whole-mix output; and the switch's paths, a
  settled Off frame with an over and a drain frame on each side of F. Plus a sequence that changes
  path every frame, for the branch predictor's misses.
- **S's line:** 48 × the slowest measured path, plus the +10 % margin of budget §8.3, as its own
  line of budget §5.3's S table.
- **Not from `hot_out`.** A `hot_out` block plays the default voice count, so its grains' cost
  would leak into S, which the governor already charges, and its frames mix paths. `hot_out`
  stays a G3 conformance case only: the shadow ledger must be at or above DWT there.
- **G1:** the image's renders equal the host's and qemu's for the changed presets and for
  `limit_switch`, `limit_off_hot` and `limit_clip_trails`.
- **The analog measurements are not here.** They move to lane L1b, before revision 10 (§6.4).

---

## 9. Golden, test and audition impact

### 9.1 Goldens that change

With draft 2's limiter built in, the golden harness reports exactly the eight renders of §2.3 as
changed, the same eight as draft 1. Each first differs in the same second as under draft 1
(measured):

| Preset | First differing second | Seconds changed |
|---|---|---|
| `strum_marks` | 3 | 3 |
| `post_max` | 8 | 8 |
| `hot_out` | 0 | 0–5 |
| `automation_offgrid` | 3 | 3–10 (draft 1: 3, 5–8, 10) |
| `spillover_chain` | 5 | 5 |
| `exact_load_mid` | 5 | 5–7 |
| `lone_changes` | 11 | 11 |
| `midi_gate` | 3 | 3, 6 |

The other 37 keep their hashes. Two counters change, both on `automation_offgrid`:
`OutActiveFrames` 568,612 → 568,600 and `TailActiveFrames` 50,261 → 50,249, because the slow
release keeps its tail lower for longer. Every other counter of all 45 presets is unchanged
(measured). Draft 1 changed no counter.

**The review check for the re-mint is order-aware.** The table above is the check only if the
limiter lands directly on revision 7 (D7). If the governor lands first, it changes
`dense_1ms` from 0 s, `freeze_retoggle_spill` from 9 s, `automation_offgrid` from 6 s,
`spillover_chain` from 5 s and `exact_load_mid` from 7 s (budget §6, option B, on its 33-preset
corpus of the time), and any of those can move peaks across full scale. So **whichever of the
limiter and the governor lands second regenerates its predicted change set and first-differing
seconds from the first's minted renders**, with a harness that reads the engine's statistics (or
an offline model given the dry and wet terms and reset at every `Reset` frame). That set, not
this table, is its re-mint's check: any other hash that moves is a bug. The governor's own
figures on today's 45-preset corpus belong to its lane, which owns its prototype.

**With D7 confirmed, the limiter lands after CLOCK's T1 and T2 and before the governor.**

- **T1 changed none of the 45.** It reproduced all 45 of revision 7's presets, with their hashes,
  per-second hashes and counters, and added seven clock presets in corpus 13 (clock §11.11, on
  `claude/tempo-core`). T2 is to add its synced-time presets in corpus 14.
- **So the eight of the table above are expected to hold,** and L2 regenerates the predicted set
  from revision 9's minted corpus with the statistics-reading harness: any of T1's or T2's new
  presets that passes full scale joins the set. That regenerated set is revision 10's check.
- **The switch adds one render to it: `limit_off_hot`'s.** L1 mints it at revision 9 without
  row 87, so it plays unlimited; L2 sets its package's switch Off, so its render becomes revision
  9's clamped at ±1.0, where a limiter-model prediction would wrongly predict a limited render.
  §9.3's float-dump comparison is its check. No other existing preset turns the limiter Off, and
  the package re-stamp changes every other package preset's `soundHash` and no render (§7.1).
  `limit_switch` and `limit_clip_trails` are new at L2, not changed.
- **The gate attributes some of the set to packages.** Every package changes at revision 10, so
  the changed renders of package presets (four of the eight above, `limit_off_hot`, and any of
  L1's or T1's package presets that limit) are reported as their packages' changes. §7.1's
  `Package-change:` line names both causes.
- **The governor, landing second, regenerates its own set** from revision 10's renders, as
  above.

### 9.2 New counters (`dsp/tests/golden/Corpus.h`)

| Counter | Meaning | Lands in |
|---|---|---|
| `OutOverFrames` | output frames with \|out\| > 1 on either channel | L1, at the revision before the limiter's: the next free corpus version (draft 2's 13; 15 if T2's 14 lands first), sound-neutral. Shows today's clipping |
| `LimitedFrames` | `OutStats().limitedFrames` over the render | L2, revision 10: the corpus version after L1's (draft 2's 14; expected 16) |
| `LimiterDryFrames` | `OutStats().dryFrames` over the render | L2 |
| `LimiterEngages` | `OutStats().limiterEngages` over the render | L2 |
| `LimiterMinGainBits` | the bits of the lowest `ConsumeLimiterMinGain()` over the render, `0x3F800000` when none: an exact integer on every leg | L2 |
| `LimiterPeakBits` | the bits of the highest `ConsumeLimiterPeak()` over the render, 0 when none | L2 |
| `RestartsWhileLimiting` | `OutStats().restartsWhileLimiting` over the render | L2 |
| `LoadsWhileLimiting` | `OutStats().loadsWhileLimiting` over the render | L2 |
| `LimiterOffFrames` | `OutStats().offFrames` over the render: frames played with the switch Off | L2 |
| `LimiterClippedFrames` | `OutStats().clippedFrames` over the render | L2 |
| `OffWhileLimiting` | `OutStats().offWhileLimiting` over the render: drains started | L2 |
| `OnWhileDraining` | `OutStats().onWhileDraining` over the render | L2 |
| `LimiterDrainDryFrames` | `OutStats().drainDryFrames` over the render: drain frames below F, the whole-mix form. Since a drain only rises, one such frame also proves an On attack below F before it | L2 |
| `LimiterDrainWetFrames` | `drainFrames − drainDryFrames` over the render: drain frames at or above F, the wet-only form. The corpus's requirements are bounds on single counters, so the harness forms the difference | L2 |

From revision 10 the harness requires `OutOverFrames` = 0 on every preset of every vector, with
the switch On or Off. Every
golden input's dry is at or under 1.0 in magnitude: the test signals are q·2⁻²³ with q in
[−2²³, 2²³ − 1] (`TestSignal.h:10-14`, `:25-26`), so −1.0 itself occurs, and the subnormal vector
adds off-grid noise far below full scale (`Render.cpp:73-80`).

A new ablation, `limiterSwitch`, forces every package's switch On and drops every `SetParam` on
row 87. It must change `limit_switch`, `limit_off_hot` and `limit_clip_trails`, so the switch is
shown to be heard.

`AmongEdits` (`Corpus.cpp:980` at `claude/tempo-core`) gains `kDomainOutput` in its list of
domains. After each `SetParam` of another row it then sets row 87 to Off and back at the same
frame, which must change nothing, since only a frame's final value reaches `Switch` (§4.7). It
adds no pair after a `SetParam` of row 87 itself, because row 87 is the Output domain's only
leaf (`other(kDomainOutput, 87)` finds none), and at G = 1 a pair is a no-op whichever way the
engine applies it. So the pair tests the engine's coalescing only where another row's edit
falls while the gain is below 1: `limit_switch` places one there (§9.3), and an engine test
pins it at blocks {1, 7, 127} (§9.4).

`hot_out` gains `require LimitedFrames ≥ 200,000` (`Corpus.cpp:944-945`).

New counters and corpus versions change no committed hash (`tools/ci/sound_rev_gate.py`'s
docstring).

### 9.3 New golden presets (L1's corpus version, and L2's for the switch)

Each of the first seven presets is minted at the revision before the limiter's (9 if T2 lands
first, else 8 or 7), where it clips and `OutOverFrames` shows it, and re-minted at revision 10.
The parameters are starting points: L1 tunes them to the stated overs, and places each load and
restart where the earlier render is over full scale. The "while limiting" requirements apply
from L2's corpus version. `limit_switch` and `limit_clip_trails` need row 87, so L2 adds them,
and L2 places each of their events from the limiter's own gain trace, which its harness reads.

| Preset | Vector | Settings | What it pins |
|---|---|---|---|
| `limit_attack` | Plucks, 12 s | Mix 0.5, `wet_trim_db` +6, feedback 0.4 | repeated sudden overs 3–6 dB over; attack, hold, full release between plucks; at least 10 engagements; Plucks' alternating pan exercises the linking; `LimiterDryFrames` = 0 |
| `limit_sustain` | SoftNotes, 12 s | Mix 0.5, post delay mix 1, feedback 0.9, time 400 ms; a Spillover load with Trails and one with FastCut inside the held over | the owner's case: coherent build-up to about +2.5 dBFS held for 2 s or more; the hold restart and the slow release; the gain's carry-over across loads in the hash (`LoadsWhileLimiting` ≥ 2); release inside the silent tail (`LimitedFrames` ≥ 96,000; the last limited frame before the tail's last second) |
| `limit_restart` | Plucks, 12 s | `limit_attack`'s settings, with an Exact load and a `Restart` at frames where the gain is below 1 | priming: `RestartsWhileLimiting` ≥ 2, `Invariance::RestartTail` |
| `limit_hot_kill` | Saturation, 6 s | Mix 0.5, the cutoff kill | the dry at unity, −1.0 included, with the wet at exactly 0: `LimitedFrames` = 0. The pedal's dry alone never engages it |
| `limit_hot_mix25` | Saturation, 6 s | Mix 0.25, `wet_trim_db` +12 | the wet past F with the dry at full scale: `LimiterDryFrames` > 0, the dry dips |
| `limit_ev` | SoftNotes, 10 s | Mix 0.4, a loud wet, `global.effect_volume_db` +12 | the player's wet level raised: `LimitedFrames` > 0 and `LimiterDryFrames` = 0, the dry untouched |
| `limit_off_hot` | Saturation, 6 s | Mix 0.5, `wet_trim_db` +6; a package that L2 gives `"output": { "limiter": 0 }` | the switch Off on a hot input from the first frame: `LimitedFrames` = 0, `LimiterOffFrames` = every frame, `LimiterClippedFrames` > 0, `OutOverFrames` = 0. **Review check at the re-mint:** its revision-10 render equals its earlier render (minted by L1 without the row, so unlimited) with every sample clamped to ±1.0, compared from the two renders' float dumps as §9.1's seconds are |
| `limit_switch` | SoftNotes, 12 s | `limit_sustain`'s settings with `wet_trim_db` +12, held into limiting from about 2 s. The +12 dB is needed: SoftNotes' dry is at most 0.5 (two overlapping notes at `kFs / 4`), so G < F needs a wet term over 4·(1 − \|a\|) ≥ 2.0, which `limit_sustain`'s +2.5 dBFS sum (wet at most 1.83) never reaches. Events at odd frames: (1) `SetParam` Off inside the held over with G < F and the hold running, so the drain starts below F and crosses it; (2) On during that drain; (3) Off again during a release with F ≤ G < 1, a drain in the wet-only form; (4) a Spillover load (Trails) of an On package during that drain; (5) an Exact load of an Off package at a frame with G < 1, a settled Off from frame 0 of its timeline; (6) On at a frame with an over while settled Off; (7) an Off and an On at one frame with G < 1 and the hold running; (8) one `SetParam` of `feedback.amount` at another frame with G < 1 and the hold running, beside which `AmongEdits` places its row-87 pair | `OffWhileLimiting` = 2 (events 1 and 3: the Exact load counts none, since `Reset` primes before its rebuild, §6.1, and event 7 counts none, since the engine applies only the frame's final value), `OnWhileDraining` = 2 (events 2 and 4), `RestartsWhileLimiting` = 1 (event 5), `LimiterDrainDryFrames` ≥ 1 (the drain's whole-mix form, and so an On attack below F before it), `LimiterDrainWetFrames` ≥ 1, `LimiterClippedFrames` > 0, `OutOverFrames` = 0; `Invariance::HostileFpEnv`, `RestartTail` and `AmongEdits`; block-split coverage at {1, 7, 127} and split delivery, with each event off the 48-frame grid |
| `limit_clip_trails` | Saturation, 6 s | `limit_off_hot`'s Off package from frame 0, settled and clipping; at an odd frame near 3 s, a Spillover load (Trails) of an On package with `limit_hot_mix25`'s settings (Mix 0.25, `wet_trim_db` +12) | leaving a clipping preset with Trails (§4.7, "Turning On"): `LimiterOffFrames` = the load's frame exactly, `LimiterClippedFrames` > 0, `LoadsWhileLimiting` = 0 (G = 1 at the load), `LimiterEngages` ≥ 1 and `LimiterDryFrames` > 0, all of which follow the load, since a settled Off renders no frame below 1; `OutOverFrames` = 0; `Invariance::HostileFpEnv` and split delivery. It pins the instant duck at the load, the owner's answer to §11.5 Q9 (2026-10-10) |

`limit_hot_kill` replaces draft 1's `limit_hot_mix0`, which multiplied the wet by exactly 0 at
Mix 0 and so repeated what `subnormal_dry` already pins. Each preset carries
`Invariance::HostileFpEnv`, and `limit_sustain` and `limit_switch` also block-split coverage at
odd sizes. Every leg's block patterns, random sizes, fresh engines and split delivery cover all
nine, and so does the M7 under qemu, which the prototypes did not run (§12). `lone_changes`
keeps its explicit list of rows (`Corpus.cpp:316-334` at `claude/tempo-core`), so its hash does
not move for row 87. `limit_switch` carries row 87's lone changes instead.

### 9.4 Unit tests

**`dsp/tests/test_limiter.cpp` (new).** It tests `detail::OutputLimiter` directly:

1. **Identity.** At G = 1: 2²⁰ magnitudes at or under 1.0, plus ±0, subnormals, 1.0 and
   `nextbelow(1.0)`, both signs, keep their bits and the state. The exhaustive sweep of all
   1,065,353,217 runs under the nightly label.
2. **The ceiling and the clamp.** Random frames with dry terms up to ±`FLT_MAX` and wet terms up
   to ±`FLT_MAX` never exceed max(1, |a|) and are always finite. Above the floor the unclamped
   result exceeds the ceiling by at most 1 ulp. Wet terms of 2²⁵, 2³⁰, 2¹⁰⁰ and 2¹²⁶ and above
   give G = 2⁻²⁴ and an output of exactly ±c: the clamp as a hard clip.
3. **Constants.** At 48 kHz, `release` is `0x3F800325`, `releaseSlow` `0x3F8000C9` and
   `holdFrames` 480; F and 1/F are exact powers of two.
4. **Timing.** One over of 2.5 dB (starting bits `0x3F3FF911`) gives exactly 1.0f on the
   3,481st frame, counted from the over's frame; the other rows of §4.4 item 3 are checked the
   same way, from the bits of §4.7's table, the floor's 173,843 counted from a burst's last over
   frame. An over that returns during a release switches the rest of
   that release to k_s, pinned frame by frame, and the flag clears at G = 1.
5. **No stuck hold.** From every gain on a 0.05 dB grid down to −60 dB, with no input, G = 1
   within H + ⌈GR/(10 dB/s)⌉ + 2 frames, at either rate.
6. **Liveness.** From the floor, the gain rises strictly every frame at both rates.
7. **Linking.** One channel over, the other at −20 dBFS: both wets are scaled by the same G.
8. **The wet first.** With |a| < 1 and a requirement at or above F, the output is fl(a + fl(G·b))
   and the dry is untouched. Mix 0 (b = 0) during a running gain at or above F returns a bit for
   bit. At G = F the two forms give the same bits.
9. **The whole mix past F.** With a full-scale dry and a wet that needs more than 12 dB, the
   output is fl(fl(G·4)·fl(a + fl(F·b))) and the call returns 3.
10. **The plugin's hot dry, hash-pinned.** A fixed-seed stream of hot-dry plus wet frames,
    including the linked case of §4.4 item 7 and |a| up to `FLT_MAX`, runs through `Frame`; the
    SHA-256 of the output bits is pinned, so every host leg compares bits, not bounds. With no
    wet term, 10⁶ hostile frames are unchanged.
11. **Equivalences.** If L2 takes §8.1's shared second-stage division, a test asserts it equals
    the per-channel form on 10⁸ frames.
12. **Off, settled.** With `on` false and G = 1, every frame's output is `Clamp(s, c)` bit for
    bit on random frames with hot dry and wet terms up to ±`FLT_MAX`, and idle frames keep their
    bits. The return's bit 2 is set exactly when a channel's |s| passed its ceiling, and NaN
    passes unchanged without setting it.
13. **The drain.** From each starting bit pattern of §4.7's table, a switch to Off with demand
    continuing gives G_n = min(1, fl(G_{n−1}·k)) frame by frame. There is no attack, the hold is
    0 and both flags are clear. The output is §4.3's form at G_n, clamped, and G is exactly 1.0f
    after the table's frame count. From G = 1 a switch to Off changes no state.
14. **On.** From a settled Off, a switch to On followed by an over attacks from G = 1 and counts
    an engagement. During a drain, a switch to On continues from G, with `releasing` set: without
    demand the trajectory is the drain's, frame for frame, and an over that returns attacks and
    sets `slow`.
15. **An unchanged setting.** `Switch` called with the current setting changes no state, bit for
    bit and counts included, at G = 1 and at G < 1 with the hold running, with `slow` set and
    during a drain. (A same-frame Off and On is the engine's rule, not `Switch`'s: called twice
    at G < 1, `Switch` would change the state, as §4.7 says. The engine test below pins the
    pair.)
16. **Continuity.** Across a switch to Off at G = F/2 under a constant over, the gain changes by
    at most the factor k per frame, the switch frame included, so the envelope has no step. The
    click test's local-outlier ratio over the switch frame stays within the bound that the A/B
    sets for the hard knee (the click test below).

**Engine tests.**

- **Contract #2** (`test_engine.cpp:320`), **the Mix-law tests** (`test_modes.cpp:469`, `:506`)
  and **the trim test** (`:356`) pass unchanged (measured). The Mix-law test gains `CHECK(OutStats
  ().limitedFrames == 0)`, which makes its precondition explicit.
- **The limiter's sound, bounded.** `post-delay time automation is click-free`
  (`test_engine.cpp:1070`) is the one test that fails with the limiter (measured, draft 1 and
  draft 2): its feedback-0.9 renders reach 1.11–1.45 (+0.9 to +3.2 dBFS), so the limiter acts and
  the click detector sees its bend (§3.4). L1 splits it:
  - a copy at the original level becomes a limiter test, its local-outlier bound set from the
    owner's A/B (about 6.0 times with the hard knee, against the 5.84 measured);
  - the existing test's input is halved, 0.25 for the noise, so every render stays at or under
    0.75 and it measures the delay glide again. With it, the whole suite passed with draft 1's
    limiter: 201 of 201 test cases (measured).
- **Sustained material.** A `limit_sustain`-like render counts the 50 ms windows, at a 10 ms
  hop, in which the gain rises and falls by 0.5 dB or more, against a bound from L2's chosen
  release (0 with the dual release on the S0 Saturation renders measured, §4.6).
- **Lifecycle.** Render into limiting, `Restart()`, and require the rest to equal a fresh
  engine's bit for bit. Render into limiting, apply a Spillover load, and require
  `OutStats().limitedFrames` to keep advancing across the load's frame (§5.4).
- **The block-split test** gains limiting renders: `hot_out`'s and `limit_ev`'s settings at
  blocks of {1, 7, 32, 48, 64, 127, 512}, and `limit_switch`'s toggles at the same blocks.
- **The switch in the engine** (`test_params.cpp`, `test_modes.cpp`):
  - row 87 is a Leaf with `sinceRev` 10, domain `kDomainOutput` and default 1;
  - its lone change equals the same change among edits of every other domain, as every Leaf
    row's does, and that test iterates the rows, so it covers row 87 without edits;
  - an Exact load of an Off package plays `Clamp(s, c)` from frame 0;
  - an Exact load of an Off package at a frame with G < 1 counts one `restartsWhileLimiting`
    and no `offWhileLimiting`, which pins §6.1's order (`Prime` before `Reset`'s rebuild);
  - a Spillover load of an Off package mid-limiting drains from the load's frame, and one of an
    On package during a drain continues from G;
  - **the same-frame pair:** render into limiting so that G < 1, once inside the hold and once
    with `slow` set; apply `SetParam(87, 0)` and `SetParam(87, 1)` at one odd frame; the output
    and `OutStats()` must equal the same render without the two events, at blocks of
    {1, 7, 127};
  - `Restart` during a drain gives a settled Off equal to a fresh engine with that preset;
  - a frozen revision-2 package loads exact with the switch On.
- **The compiler** (`compiler/tests/`): `output.limiter` written in canonical form in every
  document; `0`, `1`, `1.0`, `1e0` and `0.3` read (`1.0` and `1e0` as the bits of 1); `true`
  refused as E3 and `2` as E4; a decompiled package holding 0.3 for row 87 compiles back to the
  same bits; a macro target and an expression assignment on it are E8; L15 is a note for a user
  document and an error under `--factory`, and, once the declaration is built, no error under
  `--factory` for an id the declarations mark `--clips` (§9.5). The random-document and
  reader-fuzz digests are re-minted for the new key; the property test needs no exception for
  it.
- **The plugin** (`plugin/tests/`): the `LimitClip` kind formats "Limit" and "Clip" at the
  threshold, reads "limit", "clip", "on", "off" and numbers, and is boolean to hosts.
- **The validator** (`dsp/tests/test_blob.cpp`, frozen fixtures): a MACR target on row 87 is
  `TargetLimiter`; a CTRL expression assignment on row 87 is `ExpressionTarget`.

### 9.5 The audition pre-screen

Once the limiter exists, no render on a pedal-faithful input can exceed 0 dBFS, with the switch
On or Off. So the peak alone judges nothing, and the checks move to the engine's counts. D8 is
confirmed as recommended. D5's switch adds the clipped count beside the limited one, and, for
D14, a factory rule (below).

| Check (`tools/audition/README.md`'s table) | Today | From revision 10 |
|---|---|---|
| Peak (stored), class inputs | ≤ −1 dBFS | **Unchanged**, for either setting. A factory preset must not lean on the limiter at its stored positions, and the 1 dB margin also keeps the soft-knee alternative off them. A preset with the switch Off that clips at its stored positions fails it, as it would without a limiter |
| Peak (moved), sweeps and S11 on the class input | ≤ 0 dBFS, unrounded (`Suite.cpp:29`, `:519`, `:677-680`) | **Ceiling (moved): zero limited frames and zero clipped frames** during sweeps and S11 on the class input. This is the same criterion, read from `OutStats()`: the mix never reached full scale, by either mechanism. A failure names the render, its limited, dry and clipped frames and its would-be peak |
| Peak (other) | the other inputs' stored peaks, and the highest of S0's other vectors, S7–S10 and the Clicks renders: reported | Reported per render, not only the highest: each S0 vector and each wet render, with its limited, dry and clipped frames and its would-be peak. These include the attack modes' SoftNotes corners at up to +2.5 dBFS, all S0 Saturation renders, Echolalia's S0 OnsetBursts render (+0.14 dBFS), Déjà Vu's S0 wet SoftNotes render (+0.52 dBFS), Lull's S10 and Runaway's S8 |
| Over full scale (new) | — | **No sample over full scale in any render** (`Metrics.overFull` = 0, `Metrics.cpp:251`), the switch Off included, since Off clips at the ceiling. This is a guard on the limiter itself |
| Limiter switch (new, D14) | — | **On (Limit), for every factory preset** (`id` under `factory.`) unless the declarations, once built, mark its id `--clips`. A factory package with the switch Off and no declaration fails, even if it bypassed the lint. Other presets report the setting in the header of their report |
| Load | births per second | unchanged |

**Reported readings (new, never failed).** They show the owner what the limiter does in normal
play:

- S0 at effect volume +6 and +12 dB and at Mix 0.5, on the class inputs and SoftNotes: per preset,
  the share of time limited, the dry frames and the lowest wet gain (§3.3's table, rendered by the
  engine);
- S0 at +3 dB input on SoftNotes and Plucks: what a boosted guitar does;
- for every limited render, the sustained-material count of §9.4;
- for a preset with the switch Off, the share of time clipped and the would-be peak per render,
  where an On preset reports limiting.

`Render.cpp` reads `OutStats()` deltas and the two `Consume` calls around each render, as it reads
`Stats()` (`tools/audition/src/Render.cpp:87`, `:137`, `:142`).

**Factory presets and the switch (D14, the owner's).** The switch, as the owner chose it, exists
so that a mode can turn the limiter off on purpose, and a factory mode is a factory preset
(compiler §1.3). So whether factory presets may set it Off is a product decision for the owner,
D14, not a design decision taken here. The design recommended that they may not for now, and the
owner confirmed it on 2026-10-10 ("Keep factory On", §11.2, record §7.6): **factory presets keep
the switch On, and lint L15 enforces it under `--factory`**, with the pre-screen's switch row
(above) as a second guard. The alternative, not chosen, was to allow Off for a factory mode that
declares it, through the declaration below, built now rather than later. The reasons for the
recommendation:

1. **The ceiling is the factory set's protection.** The owner asked for the limiter because the
   first set clipped the codec at extreme corners (`firmware/factory/AUDITION.md:43`). A factory
   preset with the switch Off would bring that back on exactly the material it was built to
   catch.
2. **Clipping at full scale is not a designed sound.** The clip's onset depends on the player's
   input level and pad (D12) and on where the wet trim sits, not on a curve the mode chose. A mode
   that wants distortion as character should get it from a designed stage inside the wet: a drive
   or waveshaper with its own level and tone, which the pre-screen can judge. That is a non-goal
   here (§1.3) and would be a design of its own.
3. **The pre-screen would need a second set of criteria.** Peak (stored) and Ceiling (moved) mean
   "never reaches full scale on the class input". A preset that clips deliberately fails them by
   construction, so admitting one needs other criteria: a clip share and a listening judgement.
4. **No first-set mode asks for it.** The 14 modes and 4 reserves pass the pre-screen at
   stored positions without the ceiling (§1.1).

**The declaration that lets a factory mode clip.** D14's alternative, which the owner did not
choose, and the path later under the owner's answer, is a declaration, built as follows.

- **It is a flag of its own, not an input class.** The input class, `ratings.py declare --class
  attack|pad` (`tools/audition/ratings.py:31`), decides which inputs judge a mode, and a mode that
  clips still needs one. So the declaration is `ratings.py declare ID --clips | --no-clips`, beside
  `--self-oscillating`, recorded in `AUDITION.md`'s declarations as the others are.
- **`bspc lint` reads it.** L15 runs inside `bspc lint --factory DOC.json`, which reads only the
  document today, while the declarations reach only `bspc render --declarations AUDITION.md`
  (`firmware/factory/README.md`). So `bspc lint` gains the same `--declarations AUDITION.md`
  input, parsed by the code `bspc render` uses (`kRatingsMarker`), and
  `tools/ci/bspc_roundtrip.py:150` passes it for `firmware/factory/`. Under `--factory`, L15 is
  then an error only for an id the declarations do not mark `--clips`.
- **What it replaces, exactly.** For a declared mode: Peak (stored) on the class inputs, Ceiling
  (moved) on the class input's sweeps, and the same Ceiling on its S11 Combinations renders, are
  each replaced by the clip share per render (clipped frames over frames), reported, and by the
  listening pass. "No sample over full scale" still applies, which Off satisfies, and so do the
  Load check and every reported reading.
- **User presets.** For an id not under `factory.`, the switch Off is itself the declaration.
  The Curation slice's one-click render reports the clip share where Peak (stored) and Ceiling
  (moved) would fail, and L15 stays a note, so a deliberate Clip preset is never reported as a
  failure.

**When it is built.** Under the owner's answer to D14 the declaration waits until a factory mode
asks for it, and until then L15 under `--factory` is an error for every Off. D14's alternative
would have had L3 build the `ratings.py` flag and the report, and L2 the `bspc lint` input, now:
about 0.5 day, not in §11.1's totals. The user-preset rule above needs no declaration and is
built either way, in L3 and L4.

A user preset may set the switch Off freely. L15 then appears as a note in the Curation slice and
in `bspc lint`.

At revision 10, L3 re-runs the pre-screen. These renders' hashes change:

- every S0 Saturation render: 18 modes;
- Echolalia's S0 OnsetBursts render and Déjà Vu's S0 wet SoftNotes render;
- the attack modes' SoftNotes sweeps and corners that passed 0 dBFS;
- Lull's S10 and Runaway's S8;
- any other render with an over.

Every render on a class input judged by Peak (stored) or Peak (moved) keeps its hash, because it
was already at or under 0 dBFS. L3 records the count and the per-mode list with `ratings.py note`.
Every factory package's `sound_hash` changes with the re-stamp (§7.1). `ratings.py` records that
move but carries ratings by render hash, so the switch alone marks no row.

### 9.6 Ratings and carry-forward

A rating row carries forward only when all of its S0–S11 render hashes held. Any other row is
marked "re-listen", with each changed render and its first differing second
(`tools/audition/ratings.py:21-27`, `:227-281`).

Every mode's S0 Saturation render changes. So **every rated row would be marked "re-listen"** if
ratings were written before revision 10. No row exists yet: the owner's knob ratings are pending
(`docs/STATUS.md:176-180`). This is the main reason for D7's order, and the reason the DAC test
runs before revision 10 (§6.4): a ceiling lowered later would mark rated rows again. CLOCK's
revisions 8 and 9 change no factory render (clock D1, as amended), so they mark no row either.

If rows exist by then, L3 annotates each re-listen entry with the render's limited frames, dry
frames and would-be peak, so the owner hears only the seconds that changed.

---

## 10. Sound revision and order

The limiter is one commit that raises `kSoundRevision` by one and mints `golden.json` at it
(profile §5.12, the per-commit rule). With D5's per-preset switch, the owner's choice, the same
commit adds Leaf row 87 and re-stamps every committed package, whose `sound_hash` changes with
the new leaf (§7.1). So its pull request needs the package-change label and §7.1's
`Package-change:` line, which names the default written into every package and
`limit_off_hot`'s package set Off. If it shares a pull request with T2, which also adds a leaf
(clock §11.3), there is one label and one line per revision.

It is independent of the other planned revisions:

| | Cost governor (budget §5) | CLOCK tempo core and synced times (clock §11.3) | Output limiter |
|---|---|---|---|
| Where | `GranularCore`: admission, fades | events 6–10, `TempoCore`, CLOCK births; row 63 and the post delay's sync | Pass 3c, after the mix |
| Touches the loop or the draws | yes | yes | no |
| ITCM | about 2.3 KB; waits for cold code to leave | T1 took 2.4 KiB, leaving 792 bytes (measured); T2 waits for cold code to leave | 1.28 KB plus the switch's 0.1–0.2 KB; waits for cold code to leave (§8.3) |
| Changes goldens | 5 presets of the 33-preset corpus of its time (budget §6) | new presets only (tempo off elsewhere); T1 reproduced all 45 | 8 of the 45, regenerated from revision 9 (§9.1) |
| Package hashes | none | T2: every `sound_hash` (row 63 becomes a leaf) | every `sound_hash` (row 87, D5's switch) |

**The order (D7, confirmed by the owner on 2026-10-10).** The owner amended CLOCK's D1 on
2026-10-09, so the tempo core (T1, revision 8) and synced times (T2, revision 9) are being built
now, ahead of the knob ratings. The limiter is therefore **the next free revision, expected
10**. It lands before the knob-rating rows are written and before the governor, which becomes
11. The number is whichever is next free when it lands, 10 if T2 has landed as D7 expects;
every "10" in this document means the limiter's revision, and every "9" the one before it. If
L2 lands before T2, the limiter is 9, and L2 adds row 86 as a `Reserved` row so row 87 can
exist (§7.1, "Row 86 first"); T2 then turns 86 into its `Global` row.

The reasons, from draft 2, which recommended revision 8 first, before that amendment:

1. It is the smallest of the three. Draft 2's other reason, that it was the only one that fit
   ITCM, no longer holds: after T1 it does not fit either, and it lands after the cold-code move
   (§8.3).
2. Its cost enters S before the budget's D8 freezes the governor's constants (budget §9).
3. The knob ratings, the listening pass and the governor's D11 factory gate then all run on the
   limited sound. No rating has to be re-heard for it (§9.6), and CLOCK's two revisions change no
   factory render.
4. It changes nothing CLOCK or the governor depend on. CLOCK's 48-frame cap belongs to whichever
   of the governor and the tempo core lands second (clock §11.3), which is now the governor.

**What it needs first.**

- Lane L1b's DAC test, so the ceiling and detector are fixed once (§6.4, D10). If L1b cannot run
  before the owner wants to rate, the owner chooses: rate first and accept that a lowered ceiling
  would mark rated rows re-listen, or wait for L1b.
- The cold-code move out of ITCM (§8.3), unless T2 has built it first.

**Re-mint checks.** The limiter regenerates its predicted change set from revision 9's minted
renders, and the governor, landing after it, regenerates its own from revision 10's (§9.1).

D7's confirmation makes some text in the other designs stale, listed in §11.6.

---

## 11. Plan and decisions

### 11.1 Lanes, order and estimates

| Lane | Work | Files | Starts | Days (*estimated*) |
|---|---|---|---|---|
| **L0** design | this document, its record and its notes | `docs/` | done | — |
| **L1** counters and presets first | `OutOverFrames`; the next free corpus version with §9.3's first seven presets, `limit_off_hot` included, minted at the revision before the limiter's, loads and restarts placed where the renders clip; the click test split (§9.4) | `dsp/tests/` | now: tests only, no sound change | 1.25–1.75 |
| **L1b** the analog check | the live image's `tone` verb (firmware only, not a sound revision); on the Rev7, the 0 dBFS output level and the fs/4 sine at 45° with sample peaks of 1.0; a record under `firmware/records/`; D10 decided from it | `firmware/live/`, `firmware/records/` | now, beside L1; before L2 | 1 |
| **L2** the limiter and its switch, revision 10 | **First**, unless T2 has built it: the cold-code move out of ITCM (§8.3). **The limiter:** `detail/OutputLimiter.h` with the Off path and `Switch` (§4.7); `Engine` Impl, `Init`, `Reset`, Spillover count, Pass 3c; `OutStats` and the two `Consume` calls; L2's corpus version with its counters (the two drain counts included), requirements, `limit_switch`, `limit_clip_trails`, the `limiterSwitch` ablation and the Output domain in `AmongEdits`; `test_limiter.cpp` and §9.4's engine tests; the `Corpus.h` and `MixLaw.h` comment notes; `SoundRevision.h`'s history line; the re-mint with §9.1's check regenerated from revision 9; parity on every host leg and the M7. **The switch (§7.1), the owner's D5, with Off clipping, the owner's answer to §11.5 Q1:** row 87 and `kDomainOutput` in `Params.h`, and row 86 as a `Reserved` row if L2 lands before T2; its `ParamDisplay` row with the `LimitClip` kind; `RebuildDirty`; the compiler's schema key, its E8 rules and L15; `TargetLimiter` in the validator, two frozen fixtures and the blob fuzzer's digest; the firmware's `set` name; every document re-formatted, every package re-stamped, `golden.json` and both `MANIFEST`s; the package-change label and its `Package-change:` line. **Before minting,** the owner's confirming A/B of the answers D11 and D3 rest on: whole mix against wet first, each with 40 dB/s, the dual release and 10 dB/s, on Echolalia's S11 corner a1r0s1t0 on SoftNotes, Lull's S0 on SoftNotes at effect volume +6 dB, one mode's S0 Saturation render, `limit_sustain` and `hot_out`; and, for D5, `limit_off_hot` and `limit_switch` with the switch Off, to hear the clip and the drain, and `limit_clip_trails` with the instant attack, the owner's answer to Q9 | `dsp/`, `compiler/`, `dsp/src/blob/`, `dsp/tests/`, `firmware/live/main.cpp` (the `set` name), `firmware/factory/`, `compiler/tests/data/` | after L1 and L1b, and after T2 (D7); before the knob-rating rows | 4.25–5.75, plus 1–2 if it builds the cold-code move |
| **L3** audition | §9.5's checks and readings; the would-be peak, dry and clipped frames; per-vector S0 reporting; the switch row of the factory gate; the clip share for Clip presets; not the `--clips` declaration, which under the owner's D14 waits until a factory mode asks for it (§9.5); README; the revision-10 pre-screen and its `ratings.py note`; re-listen annotation (§9.6) | `tools/audition/`, `firmware/factory/AUDITION.md` (through `ratings.py`) | after L2 | 1.75–2.5 |
| **L4** plugin | the LIM lamp with its CLIP caption, the DRY mark and the gain-reduction readout; the `LimitClip` text and the Limit and Clip words; CLIP tags in the Library list and the Modes menu; the Curation report's counts, switch and clip share; L15 in the pre-save lint (§7.3) | `plugin/` | after L2 | 1.5–1.75 |
| **L5** firmware | the console's `stats` fields through `AudioCallback`'s atomics, the switch's included (§7.4); the user LED, its double blink for a Clip preset and the `led` verb; `firmware/README.md`'s ITCM table | `firmware/` | after L2 | 0.75–1.25 |
| **L6** bench | §8.5 within session 2: every path in `SuiteMicro`, the switch's included; S's line; `hot_out` as G3 conformance | `firmware/`, records | with session 2 | 0.5–1 |

**Order:** L1 and L1b now. Then L2 after T2, before the owner writes knob ratings if possible.
Then L3, L4 and L5 in parallel. L6 runs with bench session 2.

**Total:** about 11–15 engineer-days beyond this design (*estimated*), plus 1–2 if L2 builds
the cold-code move. The declaration's 0.5 day is not in it, since the owner kept factory presets
On (D14). Draft 2's 8.5–11.5 had no switch: draft 3's switch added about 1–1.5 days to L2 and
about 0.25–0.5 each to L1, L3, L4 and L5, and draft 4 adds about 0.25 each to L2 (the drain
counts, `limit_clip_trails`, the `LimitClip` kind) and L4 (the Library and Modes tags).

### 11.2 Owner decisions

The owner answered all thirteen on 2026-10-10. Twelve are confirmed as recommended. **D5 is
changed by the owner to a per-preset switch**: marked "Change" on the decisions page with no
note, then chosen in the session from four options (record §7.5). What its Off setting does,
which the owner did not answer with it, is §11.5 Q1, which the owner answered later that day:
Off clips (record §7.6). **D14 is new** in draft 4: whether factory presets may set the switch
Off, which draft 3 had decided itself. The owner confirmed it as recommended the same day:
factory presets keep the switch On (record §7.6). As with every owner answer in these
designs, each stays reversible before the first public release; reversing one is an owner
decision of its own and, where it changes the sound, a sound revision. Record §7 holds the
answers as given.

The Answer column holds what the owner decided; D5's also gives this design's specification of
the switch, marked "As specified here", and the owner's answer to §11.5 Q1, what Off does. What
D5's switch adds to another decision is in that decision's Consequence column, marked "for D5",
with Off as the owner answered, a clip at the ceiling (§11.5 Q1); what follows from D14 is
marked "for D14".

| # | Decision | Answer (the owner, 2026-10-10) | Consequence | § |
|---|---|---|---|---|
| D1 | Where it lives | Confirmed: **in the engine, for the pedal and the plugin alike**, last in Pass 3c | Both targets play the same limited samples; the plugin needs no wrapper code | 6 |
| D2 | Threshold, ceiling and knee | Confirmed: **a hard knee at full scale (T = C = 1.0)**, unless L1b shows the DAC clips (D10). The soft knee from −1 dBFS stays an alternative, auditioned only if the owner asks | Only renders that clip today change (8 of 45 goldens); contract #2 stays as written. The soft knee bends sudden low overs less, but changes 10 goldens and every render between −1 and 0 dBFS, and restates contract #2 | 3.4, 4.6 |
| D3 | Time behaviour | Confirmed: **zero latency, instant attack, a 10 ms hold restarted under demand within 0.25 dB, and a release of 40 dB/s after an isolated over that slows to 10 dB/s once an over returns during the release**, ending at exactly 1 | No dry delay. Isolated transients recover as fast as draft 1's (72.5 ms after a 2.5 dB over); sustained material holds still (flutter windows, measured on the whole sum: 26 → 3 on the owner's corner, 97 and 191 → 0 on S0 Saturation). The cost: one more state flag, and limiting lasts longer once overs keep returning (1.54 → 1.85 s on the owner's corner; about 14.5 s from the floor on hostile input). L2's A/B is the listening check this answer rests on | 4.1, 4.4, 4.6 |
| D4 | Linking and the plugin's hot dry | Confirmed: **linked across channels, with each channel's ceiling at max(1, \|dry term\|)**. Not unlinked when a ceiling exceeds 1 | The stereo image never moves. No output exceeds max(1, \|a\|); the dry is untouched while the wet gives way; past F a hot dry on one channel can be pulled below its own level by the other channel's wet. Unlinking would keep that hot dry but move the image, for a plugin-only case | 4.4, 3.3 |
| D5 | Control | Changed by the owner: **a per-preset switch (row 87, `output.limiter`, default On)**. The owner marked D5 "Change" on the decisions page with no note; then, asked in the session "For limiter decision D5 (user control), what change do you want?", chose **"Per-preset switch"** from four options (a device setting, on or off; off in the plugin only; a per-preset switch; an adjustable ceiling). The chosen option read: "A stored preset parameter, so a mode can turn the limiter off on purpose (e.g. for deliberate clipping). Adds a new leaf, so every package hash changes (package-change label)". (Draft 2 recommended none: always on, no row, no device setting, no per-preset switch.) As specified here: 0 (Clip) or 1 (Limit), written `"output": { "limiter": 1 }`; a switch to Off mid-limiting drains the gain at 40 dB/s, then clips; a switch to On starts from unity, or from a running drain's gain; macros, expression and host automation cannot reach it. **What Off does** (§11.5 Q1), answered by the owner on 2026-10-10, "Hard-clip", as recommended: Off clips each channel at the same ceiling | No render changes for the switch but `limit_off_hot`'s, which L2 sets Off. Every committed package's `sound_hash` changes at revision 10, which needs the package-change label and a `Package-change:` line. With Off clipping, as the owner answered, no sample leaves the engine over full scale either way, and the plugin clips exactly as the pedal does when it is Off. The cost: an estimated 100–200 bytes of ITCM, about 2 cycles on the limiting paths, about 52 bytes of DTCM, and about 2.5–4 engineer-days across the lanes. A user preset can set it Off; the CLIP lamp, the Rev7's double blink, the plugin's CLIP tags and L15's note show it. Row 87 is the switch's | 7.1, 4.7, 9.5, 10, 11.5 |
| D6 | Indication | Confirmed: **Plugin: a LIM lamp with a DRY mark and the wet's gain reduction at the output meter. Rev7: the console counts, and the user LED shows limiting (lit; blinking when the dry dips), switchable back to onsets. Product: in the control-surface design.** | Engagement is visible where it happens, at the last digital stage, on both targets; on the Rev7 the player can see why a sound dipped. *Drafts 3 and 4 add, for D5:* the lamp's caption reads CLIP from the load of a Clip preset; the plugin tags Clip presets in the Library list and the Modes menu; the Rev7's LED double-blinks at the load of a Clip preset, in both `led` modes; and the **control-surface design is handed two requirements**, as D12 and D13 hand theirs: the panel shows that the loaded preset's switch is Clip from the moment it loads, not only while clipping, and shows clipping as distinct from limiting (§7.3, §7.4) | 7.3, 7.4 |
| D7 | Revision and order | Confirmed: **the next free revision, expected 10**: after CLOCK's tempo core (8) and synced times (9), which the owner moved ahead on 2026-10-09 (clock D1, amended); before the knob-rating rows are written and before the governor (then 11); after L1b's DAC test. (Draft 2 recommended 8, first, before that amendment.) | Ratings and the factory gate run on the limited sound; no re-listen; the governor's S includes it before its constants freeze. If L1b cannot come first, the owner chooses between rating first (a lowered ceiling later re-marks rated rows) and waiting. After T1 the limiter no longer fits ITCM as it is, so it lands after the cold-code move (§8.3) | 10, 9.6 |
| D8 | The audition | Confirmed: **keep Peak at stored positions (≤ −1 dBFS). Replace Peak (moved) with zero limiter engagement on the class input during sweeps and S11. Report engagement per render elsewhere, with dry frames and the would-be peak. Add "no sample over full scale" and the effect-volume, Mix 0.5 and +3 dB input readings.** | Factory presets never lean on the limiter where the class input is judged; the +2.5 dBFS corners become reported limiting, not clipping; the owner sees what raising the wet does. *Draft 3 adds, for D5:* a clipped frame counts beside a limited one in Ceiling (moved), and a Clip preset reports its clip share. *For D14:* the factory gate's switch row (§9.5) | 9.5 |
| D9 | CPU charge | Confirmed: **the worst limited path in the governor's static reserve S: a placeholder of 260 cycles per frame (12.5k per block, 2.6 %)**, replaced by session 2's slowest measured path plus 10 % | About 260 cycles per frame less for grains at binding corners: about 2 Hermite voices (the whole-mix alternative: about 1.2). *Draft 3 adds, for D5:* the switch's paths are cheaper than the worst one, so the placeholder stands | 8.1, 8.2 |
| D10 | The analog side | Confirmed: **measure the Rev7's 0 dBFS level and the PCM3060's inter-sample behaviour in L1b, before revision 10. Keep C = 1.0 unless the DAC clips the fs/4 test; if it does, choose a lower C or an oversampled detector then. Require the output stage to swing DAC full scale + 3 dB** | The ceiling is fixed once, before any rating; no contract changes without evidence. Inter-sample overs exist today at stored positions (up to +1.5 dBTP on OnsetBursts), so the test matters with or without the limiter. *Draft 3 adds, for D5:* a lowered C would lower the Off setting's clip with it | 6.4 |
| D11 | What gives way | Confirmed: **the wet first, down to F = −12 dB (2⁻²), then the whole mix** | The dry never dips when the player raises the effect volume (to +12 dB) or the Mix knob, on any factory preset at its stored positions; on hot input it dips about 1 dB where the whole mix would dip 3.5. The cost: about 100 cycles per frame more in S than the whole-mix rule (about 0.8 of a voice), 280 more bytes of ITCM, and a wet that pumps deeper. L2's A/B is the listening check this answer rests on | 3.3, 4.6, 8 |
| D12 | Input staging | Confirmed: **hand the hardware design a target: the Instrument/Line pad puts the hottest supported source at or below about −7 dBFS peak at the codec** | Stored positions stay under the limiter on sustained material; hot sources dip the wet, not the dry. Without it, a boosted guitar engages the limiter on most presets at stored positions | 6.4 |
| D13 | Bypass and trails | Confirmed: **hand the bypass design one requirement: the bypassed dry never passes through the limiter's gain; the limiter's state continues across bypass** | A bypass pressed mid-limiting never leaves the dry low; trails stay limited as wet. *Draft 3 adds, for D5:* with the switch Off, the bypassed dry is never clipped for the preset's sake either | 6.5 |
| D14 | Factory presets and the switch (new in draft 4) | Confirmed by the owner: **factory presets keep the switch On (Limit) for now**, enforced by L15 under `--factory` and by the pre-screen's switch row, for §9.5's four reasons: the ceiling is the factory set's protection; a clip at full scale depends on the player's level, not on a curve the mode chose; the pre-screen would need other criteria; no first-set mode asks for it. Asked in the session on 2026-10-10 "May factory presets ship with the limiter Off (deliberate clipping)?", the owner answered **"Keep factory On"**, the recommendation (record §7.6). **Alternative, not chosen:** allow Off for a factory mode that declares it, through `ratings.py declare --clips`, read by `bspc lint --declarations` and the pre-screen, which replace Peak (stored), Ceiling (moved) and S11's Ceiling for it with a reported clip share and the listening pass (§9.5) | The factory set never clips by design, and a mode that wants distortion waits for a designed drive stage; the declaration waits until a factory mode asks for it, so its 0.5 engineer-day is not spent now. The alternative would have let a factory mode clip on purpose now, which is the switch's stated use, judged by ear and by its clip share. User presets may set it Off freely, and a user Clip preset is never reported as a failure | 9.5, 7.1 |

### 11.3 Design decisions taken

| Decision | Choice | § |
|---|---|---|
| 1. Gain computer | Linear domain: per channel over its ceiling, the wet's room divided by the wet, one division, and a second only past F; no log or exp per sample | 4.3 |
| 2. What gives way | One gain state G; the wet alone while G ≥ F, then the whole mix scaled by G/F; F = 2⁻² so the scaling is exact and the two forms join to the bit at G = F | 3.3, 4.3 |
| 3. Release law | Multiplicative k per frame, a constant number of dB per second, capped at r ≤ 1; two rates chosen by one flag set at a re-attack during a release and cleared at G = 1; both from `Exp2D` at `Init` | 4.1, 5.2 |
| 4. Hold restart | Only under demand (r < 1); the variant without that condition never released | 4.4 |
| 5. Floor and clamp | 2⁻²⁴, so the gain stays normal and rises. The final clamp, with one-sided compares, runs on every limited frame and is the ceiling's bound | 4.4 |
| 6. State and flush | A gain in [2⁻²⁴, 1], an integer hold, two flags and the switch; no `FlushTiny` site | 5.3 |
| 7. Placement | After the ±`FLT_MAX` saturation and before the store, with the dry and wet terms passed in; outside the loop | 6.1 |
| 8. Lifecycle | Primed by `Reset` (so `Restart` and Exact loads); kept by Spillover; both pinned by goldens with counters | 5.4 |
| 9. Code | Header-only `detail/OutputLimiter.h`, inlined into `RenderFrames`, in ITCM; scalar constants only | 8.3 |
| 10. Statistics | `OutputStats` and two `Consume` calls, outside the sound and off the hot path; exact integer counters in the corpus; the console reads them through atomics | 7.2, 7.4, 9.2 |
| 11. What Off means | A clamp at the same ceiling, max(1, \|a\|), not a pass-through: on the pedal it is code for code the unlimited engine, in the plugin it matches the pedal, and no sample passes full scale either way. Recommended to the owner, who chose it on 2026-10-10 (§11.5 Q1) | 4.7, 1.3, 11.5 |
| 12. Off mid-limiting | A drain at the fast rate k to exactly 1, with no attack and no hold, then the clamp; not a fixed crossfade (a swell at 4.5 dB/ms) and not the slow rate (14.5 s from the floor) | 4.7 |
| 13. On | From a settled Off, the reset state, so the next over attacks from unity; during a drain, G continues with `releasing` set | 4.7 |
| 14. How the engine hears the row | A seventh parameter domain whose rebuild calls `Switch`, once per span, so only a frame's final value acts and a load applies the incoming switch at its frame; the row is read with `value >= 0.5f` | 4.7, 6.1 |
| 15. The row | 87, `output.limiter`, a Leaf in a new `output` object, `sinceRev` 10, read as any integer-valued leaf (0 or 1 written; a fraction accepted and played by the threshold; E4 outside 0–1), a `LimitClip` display reading Limit or Clip, discrete and not automatable; row 86 added as `Reserved` if L2 precedes T2 | 7.1 |
| 16. Reach | No macro or expression target (E8; `TargetLimiter`, `ExpressionTarget`); registered for hosts without automation under Q12's model (b); no panel control | 7.1 |
| 17. Factory policy | Not a design decision: it is the owner's D14 (§11.2), confirmed On for now on 2026-10-10, with the `--clips` declaration specified for later and not built | 9.5 |
| 18. Visibility of a stored Clip | Shown from the load, not only while clipping: the lamp's caption, the plugin's list tags, the Rev7's double blink, and a requirement handed to the control-surface design | 7.3, 7.4 |
| 19. Drain coverage | Two per-call counts, `drainFrames` and `drainDryFrames`, folded from the existing locals when the switch is Off, so a golden can require the drain's whole-mix form without a windowed check | 6.1, 9.2 |

### 11.4 Risks

1. **The bend at new maxima.** A sudden, large, low-frequency over bends its first rising edge,
   in the wet while G ≥ F. It is inherent to zero latency. The soft knee reduces it by 17 dB on a
   sudden +2.5 dB burst at 110 Hz, but only from 5.84 to 4.19 times its neighbourhood on the click
   test's build-up. Only look-ahead removes it.
   Mitigation: the A/B before minting; the click render kept as a bounded limiter test.
2. **The wet pumps.** It carries the whole reduction up to 12 dB, so it dips deeper than a
   whole-mix limiter would dip everything.
   Mitigation: the dual release; the A/B; the sustained-material count in the tests and the
   audition.
3. **Hot inputs.** Past F the whole mix dips, the dry included.
   Mitigation: input staging (D12); the Saturation renders and the +3 dB reading show the amount.
4. **Inter-sample overs at the DAC** could clip in the PCM3060's filter, with or without the
   limiter.
   Mitigation: L1b's measurement before revision 10, and a lower ceiling or an oversampled
   detector then, if needed.
5. **CPU.** About 2.6 % of the block at worst, in S: about 2 voices at binding corners.
   Mitigation: session 2 times every path before the constants freeze; §8.1's bit-identical
   savings; D11's whole-mix alternative saves about 0.8 of a voice.
6. **ITCM.** After CLOCK's T1 the live image has 792 bytes spare, so the limiter's 1.28 KB and
   the switch's 0.1–0.2 KB need the cold-code move first, as T2 does.
   Mitigation: budget §7.3's placement by function, or the main-thread API out of `Engine.cpp`'s
   ITCM object, built by whichever of T2 and L2 comes first (§8.3).
7. **Leaning on the limiter.** A recipe tuned until its renders "just stop clipping" would rely
   on the limiter.
   Mitigation: D8 keeps the class-input criteria, and `bspc` could lint a stored position whose
   render limits (a candidate L-lint, beside L10).
8. **Longer recoveries after returning overs.** The slow rate keeps a texture's limiting going
   longer, and a hostile burst takes about 14.5 s to clear from the floor.
   Mitigation: the A/B includes the 40 dB/s and 10 dB/s rates; the floor is reachable only by
   hostile plugin input.
9. **A preset with the switch Off clips, by design.** A user may leave it Off by accident, or
   load such a preset mid-limiting, and hear the drain hand over to a hard clip within 0.3 s from
   F (3.6 s from the floor).
   Mitigation: the CLIP caption from the load, the plugin's CLIP tags in its lists and the Rev7's
   double blink, all visible before anything clips; the control-surface requirement for the
   product (D6's consequence); L15's note in the Curation slice and `bspc lint`; the factory set
   keeps it On (D14, confirmed by the owner).
10. **Package churn.** Revision 10 changes every committed package's `sound_hash` without a
    render moving. If it shares a pull request with T2, two such re-stamps meet.
    Mitigation: the package rule's label with one `Package-change:` line per revision, as wave 1
    did; each revision in its own commit (§7.1, §10).
11. **Row 87 is claimed only on paper** until L2 lands, and other lanes append rows. It also
    cannot exist before row 86.
    Mitigation: this design, clock §10.4's table (86 the last claimed there) and STATUS hold 87
    for the limiter's switch; a lane that appends earlier starts at 88; an L2 that lands before
    T2 adds 86 as a `Reserved` row (§7.1).
12. **Leaving a clipping preset steps the level.** A switch to On over a clipping texture, by a
    `SetParam` or a Trails load of an On preset from a clipping Off one, ducks the old trails and
    the new grains at once by the whole depth of the clipped over, and the dry too when r < F;
    the release then runs at 10 dB/s, so the recovery takes seconds (§4.5, §4.7). It is the one
    Spillover load that jumps the level.
    Mitigation: `limit_clip_trails` pins it, and L2's A/B plays it; the owner chose this instant
    step over the entry ramp (§11.5 Q9, 2026-10-10); factory presets do not clip (D14, confirmed
    by the owner).

### 11.5 Open questions

The owner answered Q1, Q8 (D14) and Q9 on 2026-10-10, each as recommended (record §7.6). No
question waits on the owner now: Q2 is settled by listening in L2's A/B, Q3 by L1b's
measurement, Q4 and Q6 by other designs, and Q5 is for later. D9's placeholder is replaced by
bench session 2's timings (L6). §10's choice between rating first and waiting for L1b reaches
the owner only if L1b cannot run before the owner wants to rate.

1. **(owner, answered 2026-10-10) What Off does.** The owner chose D5's form, the per-preset
   switch (§11.2), but not at first what its Off setting does. Two answers:
   - **Clip, as designed (recommended):** Off clamps each channel at the limiter's own ceiling,
     max(1, |a|) (§4.7, decision 11). On the pedal that is, code for code, what the engine
     without a limiter plays, since the codec clamps every over anyway; in the plugin it clips
     exactly as the pedal does; and no sample leaves the engine over full scale on either
     setting, so Goal 1 and the audition's "no sample over full scale" hold for every preset.
   - **Pass:** Off lets overs through unclipped. On the pedal it is the same sound, because the
     codec clips them. In the plugin it hands a DAW floats above 1.0, which a floating-point bus
     plays unclipped, so a preset's deliberate clipping would exist on the pedal and not in the
     plugin: the split above full scale that Goal 4 removes. Goal 1 and the audition's guard
     would no longer hold for Off presets.

   Recommended: Clip, for those reasons. **The owner's answer (2026-10-10): Clip.** Asked in the
   session "When a preset's limiter switch is Off, what should happen to a sample that goes over
   full scale?", the owner answered "Hard-clip" (record §7.6). L2 builds Off as §4.7 specifies,
   and its A/B plays the clip on `limit_off_hot` and `limit_switch` (Q2). (Draft 4's Q1 asked the
   owner to choose D5's form among four readings; the owner had already chosen it, record §7.5.)
2. By ear, confirming the answers: what gives way and the value of F (D11), the release (D3) and
   the knee if asked (D2), in L2's A/B, with the switch Off on `limit_off_hot` and `limit_switch`
   for D5 and Q1, and `limit_clip_trails` for Q9.
3. L1b's results: the Rev7's 0 dBFS level and the PCM3060's inter-sample behaviour (D10).
4. How the product's panel shows engagement and a stored Clip, to D6's two requirements: the
   control-surface design.
5. Whether a later true-peak option is wanted for the plugin's bounces. It would be a sound
   change for both targets, never for the plugin alone.
6. The bypass topology that meets D13: the bypass design.
7. *Merged into Q1* with the correction of draft 4: whether Off clips at the ceiling or passes
   overs is now Q1, what Off does.
8. **(owner, answered 2026-10-10)** D14: whether a factory mode may set the switch Off under the
   `--clips` declaration now, or factory presets keep it On for now (§9.5, §11.2). Recommended:
   On. **The owner's answer: factory presets keep it On.** Asked in the session "May factory
   presets ship with the limiter Off (deliberate clipping)?", the owner answered "Keep factory
   On" (record §7.6). L15 is an error under `--factory`, and the declaration waits until a factory
   mode asks for it.
9. **(owner, answered 2026-10-10)** Turning On over a clipping texture: the instant attack as
   designed, or the entry ramp, which fades clipping into limiting over about 0.3 s at the cost of
   the incoming preset clipping that long (§4.7). Recommended as designed. Draft 4 left it to the
   owner by ear in L2's A/B; the owner answered before it. **The owner's answer: instant.** Asked
   in the session "Loading a limited preset (with trails) right after a clipping one: if the
   trails are hot, how should the level drop?", the owner answered "Instant" (record §7.6). The
   entry ramp is not built; L2's A/B plays `limit_clip_trails` with the instant attack (Q2).

### 11.6 Amendments made with this design

These notes are dated 2026-10-09 and written in each document's style:

- **mode-compiler.md** §11.3, in the as-built note on the limiter question;
- **docs/STATUS.md**: step 4's owner's answers, the Mix law's open ends and the plan's item 4;
- **determinism-profile.md** §3.7, after its list: the final mix's saturation and the live
  dry's headroom;
- **companion-app.md** §4.8, on the live path's headroom;
- **docs/README.md**: the index row.

With the owner's answers (2026-10-10), draft 3 adds a dated line to the notes in mode-compiler.md
§11.3, companion-app.md §4.8 and determinism-profile.md §3.7. Those notes said there is no
control, or proposed revision 8; the new line names the switch and the revision. It also updates
docs/STATUS.md's limiter lines and CPU-budget lines and docs/README.md's index row.

Draft 4 (2026-10-10) adds a second dated sentence to each of those three notes, and corrects
STATUS's limiter lines and the README's row, saying that the factory policy is the owner's D14.
The mode-compiler note also records that the row reads as any integer-valued leaf, as its §2.2
says, so §2.2 and §3.7 need no amendment. Draft 4 also said in each of them that D5's form was
proposed and awaited the owner; its correction (2026-10-10) restates those sentences, STATUS's
lines and the README's row: the per-preset switch is the owner's choice, and what Off does, D14
and Q9 stay open (record §7.5).

With the owner's answers to §11.5 Q1, Q8 (D14) and Q9 (2026-10-10, record §7.6), a third dated
sentence is added to each of those three notes, and STATUS's limiter lines and the README's row
say that the three are answered and no owner question is left open.

D7 is now confirmed, so these are due. They wait for the lanes that own the files:

- **cpu-budget.md's "revision 8, or 9 if CLOCK's tempo core lands first"**: §5.1's note, §7.1,
  §8.1's `bench-r8-gov`, §9's D2 and D8; and §5.3's S table, which gains the limiter's line.
  The governor becomes revision 11; its lane amends them when it starts.
- **clock.md §9.6 and §11.3's "8 if it precedes cpu-budget.md's governor"**, and §9.6's ITCM
  tally. `claude/tempo-core` is editing clock.md's as-built notes, so T2's notes carry them,
  with this design's §8.3 figure and row 87 beside clock §10.4's table.

L2 amends the code comments that state today's rules. These are `Corpus.h:71-73` (no subnormal
output from arithmetic), `MixLaw.h`'s header (the dry at unity up to the middle, now "except
while the output limiter scales the whole mix"), `Engine.h:97-112`'s `Reset` and `Restart` lists,
and `Engine.cpp:1252-1254`'s finite-output comment. `Params.h`'s `ParamDomain` comment gains
`kDomainOutput`.

---

## 12. Evidence

Record §1–§2 gives each source in full, with its scratch location.

**The owner's answers to draft 4's open questions (2026-10-10; record §7.6).** Given in the chat
session, each the recommended answer. Nothing was built or measured for them, and no technical
content changed.

**Draft 4's checks (two reviews of draft 3; record §8).** Each finding's evidence was re-read in
the tree at `claude/tempo-core` `b91b34d` and `main` `011b294`:

- compiler §2.2 and §3.7, `Schema.cpp`'s `Leaf()` (`:551-574`, range check only),
  `Validate.cpp`'s `CheckStat` (`:402-414`) and `test_property.cpp:127`;
- `Engine.cpp`'s `Reset` and `Init`, each calling `RebuildDirty` before the smoothers' priming
  (`:737-743`, `:710-713`; `main` `:681-690`), its `TableIsContiguous` assert (`:36`) and the
  span loop (`:1085-1092`);
- `Corpus.cpp`'s `AmongEdits` (`:980-1023`) and its whole-render `require` bounds;
- `TestSignal.cpp`'s `BuildSoftNotes` (two overlapping notes at `kFs / 4`);
- `StateCodec.cpp:179` and `PluginProcessor.cpp:518`, `:532`;
- `sound_rev_gate.py`'s `golden_changes` and `golden.json`'s package presets;
- `ParamDisplay.cpp`'s two-state kinds and `BrainscapeParam.cpp`'s `NamedValue` and
  `isBoolean`;
- `ratings.py`'s `declare` options, `bspc_roundtrip.py:150` and `firmware/factory/README.md`.

The floor row was re-derived by simulating §4.3's On path in binary32, and the dry dip of §4.5's
new row calculated. Nothing new was built.

**Draft 3's checks (the owner's answers; record §7).** The switch is not prototyped, so its cost
figures are estimates.

- **The answers.** Read from the decisions page's database on 2026-10-10: twelve "agree", and
  D5 `{"choice": "change", "note": ""}`. The per-preset form is not on the page: the owner chose
  it in the session the same day, answering "Per-preset switch" when asked what change D5
  should be, from four options (record §7.1, §7.5).
- **Row 87 is free.** Checked by reading `Params.h` on `main` `011b294` (rows 1–82) and on
  `claude/tempo-core` `b91b34d` (83–85), and clock §10.4's table (86). Every local and remote
  branch's `Params.h` and design row tables were also searched for a row above 85. Only clock's
  86 was found.
- **Code read at `claude/tempo-core` `b91b34d`:**
  - `ParamDisplay`'s `OffOn` kind and its host-model flags;
  - `Schema.cpp`'s key order and its `global.mix` rule;
  - `Validate.cpp`'s target rules;
  - `BrainscapeParam.cpp`'s `OffOn` text;
  - `Engine.cpp`'s span loop, `RebuildDirty` and domain assert;
  - the golden corpus's `lone_changes` and `AmongEdits`;
  - the firmware's `set` names;
  - clock §11.11's as-built ITCM figure.
- **The drain's frame counts** (§4.7) were computed by iterating the binary32 product G·k from
  each starting gain, in a scratch script in which each binary64 product of two binary32 values
  is exact and is rounded once to binary32. The six rows §4.4 item 3 measured from a single over
  are each that figure less 481 frames, the attack frame and the hold, which cross-checks them.
  §4.4's floor row is 173,843 by the same count, 481 more than the drain's 173,362; the
  prototype's 173,842 counted from the first frame after the burst (record §8.3).
- **The codec equivalence of Off** (§4.7) is calculated from LD's `f2s24` (§2.2).

**Draft 2's prototype (measured).** It is not committed.

- **The code.** §4.3's limiter as a header in a scratch checkout of `main` `011b294`, with
  §6.1's Pass 3c change, the statistics accessors, and the console publishing of §7.4 in the live
  image. The golden harness was given a scratch read of the limiter's statistics per render.
  F and the slow rate were build-time constants, so the same code also built draft 1 (F = 1, one
  rate) and the whole-mix alternative (F = 1, dual rate).
- **Builds.** MSVC 17 (`/fp:precise`) and arm-none-eabi-gcc 10.3.1 (the firmware's flags).
- **Results.**
  - The golden harness: 37 of 45 hashes held; the 8 of §9.1 differed from the seconds given
    there; two counters of `automation_offgrid` changed.
  - The 45 hashes were identical under `--block 1`, `--block 512`, `--pattern 48,1,127,32`,
    `--random-blocks 7`, `--fp-env hostile`, `--fresh-engine` and `--delivery split`; every
    invariance check passed; the forced-flush control passed on all 45.
  - With F = 1 and one 40 dB/s rate, all 45 hashes equal draft 1's prototype; with F = 1 and the
    dual rate, all 45 equal the whole-mix alternative's own build.
  - The unit suite: 200 of 201 test cases passed; the failure is the click test at 5.84 times,
    as under draft 1.
  - The live image's map and disassembly: §8.1 and §8.3.
  - A standalone check program: the constants at three rates, the clamp's overshoot counts, the
    Mix-0 and hot-dry cases of §4.4 item 7, and the floor's recovery.
- **The variant program.** It rebuilt the dry and wet terms of every factory preset's S0 render
  from `bspc`'s engaged and Mix-1 wet renders, and ran the whole mix, the wet only and the wet
  first at F = −6, −12 and −18 dB over them, at the effect volumes and Mix of §3.3, and on the
  synthetic hot input.
- **Not run:** the M7 under qemu, which is not installed on this machine (L2's `parity-m7` leg
  runs it); cycle counts on the Seed (L6); the analog measurements (L1b).

**Draft 1's prototype (measured).** The same checkout with draft 1's limiter: 37 of 45 hashes
held, no counter changed; the same eight variants; 200 of 201 unit tests, and 201 of 201 with the
click test's input halved; the exhaustive identity and the one-step bound; the live image's
ITCM; a Pass 3c mock built for the M7. The soft-knee comparison of §3.4 used it with the
quadratic knee of §3.2 C.

**The reviews' measurements.** Record §6 lists each review's own checks: the determinism review
rebuilt draft 1 and traced its priming; the sound review rendered the factory set with `bspc`,
ran release and effect-volume variants and measured inter-sample peaks; the embedded review
disassembled draft 1's live image and read the firmware's console and bench code. The revision
reproduced the numbers this document uses.

**The DSP lane (host-run).** It prototyped A–F of §3.2 in a scratch harness against the in-tree
`DetMath`. It measured:

- the sine-burst residuals and aliasing (§3.2, §4.6);
- the hold and refresh ripple at 41–220 Hz;
- the HF splatter index on the goldens (§3.4);
- the instantaneous wet-only case (§3.3);
- M7 code sizes for the standalone variants.

It also found that the in-tree `SoftSat` returns 1 + 2⁻²³ for 10,220 inputs in [2.983, 3], which
is harmless in the tamer but rules out using it for a ceiling without a clamp. Its scheme to
restart the hold within 0.25 dB is the one §4.4 item 5 corrects.

**The constraints lane.** It mapped the tree's contracts, tests, integration points and ITCM
rules (cited throughout), and computed the golden peaks at revision 7 with thresholds from
0 to −3 dBFS:

| Threshold | Renders changed |
|---|---|
| 0 dBFS | 8 |
| −1 dBFS | 10 |
| −2 dBFS | 14 |
| −3 dBFS | 18 |

**The practice lane (reported).** Sources:

- Strymon's BigSky MX firmware notes, https://strymon.net/faq/bigsky-mx-firmware-revision-release-notes,
  and its plug-in FAQ;
- Mutable Instruments' code: `clouds/dsp/granular_processor.cc`, `rings/dsp/limiter.h` and
  `stmlib/dsp/dsp.h` at https://github.com/pichenettes/eurorack and
  https://github.com/pichenettes/stmlib;
- DaisySP's `Source/Dynamics/limiter.cpp`;
- the Fractal Audio forum's thread on the output clipping light;
- Eventide's forum posts on H90 clipping;
- the EHX 45000 manual;
- Airwindows' ClipOnly2, https://airwindows.com/cliponly2;
- Ableton's Live audio effect reference;
- Benchmark Media's note on inter-sample overs;
- TI's PCM3060 datasheet (SLAS533B), https://www.ti.com/lit/ds/symlink/pcm3060.pdf;
- Electrosmith's Daisy Seed datasheet v1.2.0;
- the libDaisy v9.0.0 sources cited in §2.2;
- Giannoulis, Massberg and Reiss, *Digital Dynamic Range Compressor Design*, JAES 60(6), 2012,
  for §3.2 D;
- ITU-R BS.1770-4, Annex 2, for true peak.

Unverified, by the lane's own account:

- whether the BigSky MX's limiter can be turned off;
- the Microcosm's internal limiting;
- the Fractal behaviour, which rests on a forum only;
- the PCM3060's interpolation headroom;
- the op-amp swing figures.
