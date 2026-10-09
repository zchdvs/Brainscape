# CLOCK design pass — evidence record

> The evidence behind [clock.md](../clock.md) (draft v2, owner-approved on 2026-10-09): the
> inputs, what each investigation found and where, the probes the revision ran, where the
> sources disagreed and how the design resolves it, the alternatives it rejected, and how the
> three reviews of draft v1 were disposed of (§6). The design document is normative; this
> record is not. "Design §N" is clock.md, "record §N" this file, "compiler §N"
> [mode-compiler.md](../mode-compiler.md), "engine §N" [grain-engine.md](../grain-engine.md),
> "companion §N" [companion-app.md](../companion-app.md) and "profile §N"
> [determinism-profile.md](../determinism-profile.md). Code is cited as `path:line` at
> `claude/mode-compiler-impl` `343f33c` (sound revision 7); "LD" is the pinned libDaisy v9.0.0,
> read in the Rev7 worktree's build tree, and "JUCE" the plugin build's 9.0 tree. Numbers are
> *measured*, *calculated* or *estimated*.

---

## 1. Inputs

### 1.1 How the evidence was produced

All of it on 2026-10-08. A **status survey** of the tree came first ("How close are we to tempo
match and clock sync?"): what is built, what is reserved, the open questions Q7 and Q11, two
contradictions between documents, the CPU and ITCM constraints, and eleven owner decisions with
recommended answers. Three investigations then ran in parallel, each reading the survey, the
designs and the code at `343f33c` and writing only to a scratch directory:

| Label | Investigation | What it produced |
|---|---|---|
| [microcosm] | The Microcosm and its peers: tap, Subdiv, Tempo mode, MIDI clock and transport, what follows tempo, known clock faults; four to six peers' tap averaging, clock smoothing, divisions and delay behaviour on a tempo change | the manual (v1.13) and seven peer manuals as text, the field reports, the reading of Subdiv as rate multipliers |
| [protocols] | MIDI 1.0 real-time messages and running status; clock jitter from DAWs and hardware; estimators (averages, least squares, PLL/DLL), tap algorithms; JUCE's `AudioPlayHead`, VST3's `ProcessContext`, host ramps, loops and offline bounces | the jitter figures, the least-squares factors, the ns-against-µs drift, the host conversion |
| [internal] | The integration points in the tree: events and the queue, the plugin's and the pedal's stamping, the performance state, the mode vocabulary, rows 60/63/79, the post delay's range and glide, the feedback FIFO, the birth paths, the compiler's refusals, the golden harness, the CPU record and proposal, ITCM | code findings C1–C12 (design §12) |

The author wrote draft v1 from these. Three reviews of draft v1 followed, each adversarial and
each with its own checks: **[E]** determinism and parity, **[P]** the musician and the product,
**[H]** embedded, CPU and hardware. The revision (draft v2) checked every finding's evidence
against the code, libDaisy's source, the firmware's link map and the reviewers' probes, re-ran
those probes, and ran two of its own (§2.6, §2.7). Code findings C13–C18 (design §12) come from
that checking.

### 1.2 Environment

One Windows 11 workstation. The probes are Python 3.12.2 scripts in exact integer arithmetic (no
floating point in any rule they test). The ITCM figures come from the `modes-impl` worktree's
firmware build (`build/pr-fw/firmware/brainscape_live.size.txt` and `.map`, built 2026-10-07 with
the pinned `arm-none-eabi-gcc` 10.3). Nothing ran on the Rev7 for this design; every hardware
statement is from source, the research notes or the Rev7 record.

### 1.3 Where the probes live

In the session's scratch directory, none committed: `clock/clock-status.md` (the survey),
`clock/mcref/` (the Microcosm manual as PDF and text) and `clock/peers/` (TimeLine RevE, Big Time
and its MIDI manual, Thermae, Particle 2, EHX 95000, LVX, DD-500 v2.00, as PDF and text);
`clockreview/phasor.py`, `b48.py` and `fix.py` (review [E]'s grid-firing simulation and fix); and
`clockrevise/grid.py` and `sums.py` (the revision's own). The implementing pull request of T1
should copy `grid.py` and `sums.py` into `tools/parity/clock/` as the references §8.2's unit
tests are checked against, as the compiler work did with `tools/parity/modes/`.

### 1.4 External sources

- The Hologram Microcosm manual v1.13, fetched from Hologram's CDN
  (`cdn.shopify.com/.../MC_manual_WEB.pdf`), and `docs/research/microcosm.md` §2.5, §8.3 and §13;
  the Elektronauts Microcosm thread (121669); Guitar Pedal X's pre-release listing.
- Peers: Strymon TimeLine manual RevE; Chase Bliss Big Time manual and MIDI manual, Thermae
  manual; Red Panda Particle 2 manual (firmware 2.2.0+); Electro-Harmonix 95000 manual; Meris LVX
  manual; Boss DD-500 v2.00 addendum; Eventide H9 documentation.
- MIDI 1.0's real-time and System Common messages (Clemson's specification page; jgglatt's
  `seq.htm` and `run.htm`).
- E-RM Erfindungsbüro, MIDI clock jitter report (2014), Table 1 p. 7.
- F. Adriaensen, *Using a DLL to filter time* (2005).
- JUCE 9.0 sources as cited in design §4.4 and §10.2; JUCE forum threads on Reaper's loop blocks
  and Live's VST3 offline render.
- The Seed3 datasheet's MIDI section as transcribed in `docs/research/pedal-control-surface-and-io-hardware.md`
  §5.1, and Funbox's repository as summarised in `docs/research/daisy-pedal-platforms.md` §2.3.

## 2. Evidence by design section

### 2.1 What the Microcosm documents (design §3, §5.1, §6.4, §9.5)

From the manual [microcosm], page numbers printed and PDF alike:

| Topic | What the manual says | Design |
|---|---|---|
| Tap | the left footswitch "smoothly matches the tempo" to quarter-note taps (p. 6); tap still works in Tempo mode (p. 5); tap is not available under external clock (p. 21) | §3.2, §3.6 |
| Subdiv and Tempo | the Time knob controls the subdivision or the "Global Tempo" (p. 5); in Tempo mode the subdivision is always quarter notes (p. 5) | §2.4, §6.4 |
| Subdiv's six positions | CC#5's values 0–5 are printed 1/4, 1/2, TAP, 2x, 4x and 8x (p. 20); never defined in words | §5.1 |
| The looper's speed | the same labels used as multipliers, "the 'TAP' interval (1X)" its normal speed, a range "1/4 to 4x" (p. 15) | §5.1 |
| MIDI clock | Start switches to external clock, Stop reverts to internal; the pedal can also transmit clock (p. 21) | §3.1, §3.4, §9.5 |
| Thru | soft thru on by default, and a four-way global choice of clock and thru (`microcosm.md:459-464`) | §9.5 |
| Presets | global configuration is not saved in presets (p. 18) | §2.5 (D3, D17) |

**Subdiv is a rate multiplier.** Four pieces agree: the looper uses the same labels as speed
multipliers; Tempo mode forcing quarters makes TAP the quarter, so "1/4" cannot also be one; the
pre-release listing calls the control "Tap-Division"; and users describe the 1/4 and 1/2 positions
as sparse (thread post #496). So the printed "1/4" is a whole-note grid (design §5.1, D7), which
resolves `docs/research/microcosm.md:112`.

**Not documented anywhere searched:** tap averaging and timeout, Continue, Song Position, what
happens when clock stops without a Stop, and what the Time knob does under clock. These are the
design's own choices, each an owner decision in design §11.5, taken from peers where they
document them (§2.2).

**Field reports** (Elektronauts 121669): posts #734 and #743, a stop and start realigns the beat;
#741, preset changes break the clock's phase (principle 5); #348–#350, effects stay locked while
the looper drifts; #508, the wish for direct tempo control. The "drifting, clicks and pops" line of
`docs/research/microcosm.md:552` has no source on the page it cites.

### 2.2 Peers (design §3.2, §5.2, §7)

| Peer | Tap | Clock | Divisions, delay on a tempo change |
|---|---|---|---|
| Strymon TimeLine (RevE) | Tap Mode PRESET or GLOBAL per preset | MIDI Clock Reset; Sweep | — |
| Chase Bliss Big Time | tap snaps the slider to its middle (manual p. 12) | clock "will always snap to the incoming" quarter note (MIDI manual p. 1) | CC#54's thirteen divisions; bend then snap back |
| Chase Bliss Thermae | — | clock to avoid tempo drift | GLIDE between steps, as a feature |
| Red Panda Particle 2 | — | follows clock without Start; receive clock is global; expects about ±1 ms of jitter (pp. 33–35) | per-parameter divisions (pp. 23–24) |
| EHX 95000 | the mean of the last four taps (p. 13) | Start and SPP in XT mode; 1–3 bars to settle (pp. 32–33) | 60–240 BPM |
| Meris LVX | — | — | TAP GLIDE; TEMPO SELECT (preset or global) |
| Boss DD-500 v2.00 | — | — | TEMPO HOLD (global tempo across patches) |
| Eventide H9 | averages more than two taps | — | — |

So: averaging the last four taps has a precedent (EHX); following clock without Start has one
(Particle 2); a per-preset or global tempo choice is common (TimeLine, LVX, DD-500), which is
`global.tempo_recall`; and glide on a tempo change is offered as a named feature (LVX, Thermae),
which is why draft v2 makes it the `tempo_glide` choice rather than the only behaviour (P1).

### 2.3 MIDI, jitter and estimation (design §2.1, §3.3, §3.4)

- **MIDI 1.0:** 24 clocks per quarter; the slave starts on the clock after Start; Continue
  resumes from the stop point or the last Song Position; Song Position counts 16ths (×6 ticks);
  real-time bytes F8–FF may appear anywhere, even inside SysEx, and leave running status alone,
  while System Common and SysEx bytes cancel it [protocols].
- **Jitter:** E-RM measured Ableton Live 9 as master at σ = 8.43 ms cycle to cycle with peaks of
  −38.36 and +22.24 ms, and as slave a tempo σ of 0.114 BPM. That is σₓ ≈ 3.4 ms per tick
  (*calculated*). A hardware clock stamped on the pedal's 48-frame grid has σₓ = 0.29 ms (uniform
  over 1 ms, *calculated*); the Standalone's block stamping at 512 frames σₓ ≈ 3.1 ms.
- **Estimators** (*calculated* [protocols]): the σ of a least-squares period over N ticks, as a
  multiple of σₓ, is 0.708 at N = 24 and 0.0884 at N = 96; at N = 12 the quarter's period σ is
  2σₓ (review [P]'s figure, re-derived: 24·√(12/(N(N² − 1))) = 2.0); the one-tick-ahead phase error
  is 0.21·σₓ. A one-beat average of a computer clock wobbles about ±1 %, the 96-tick fit about
  0.06 %. A DLL (Adriaensen) has infinite memory and lock-in transients after glitches; the
  window resets cleanly.
- **Units:** a 140 BPM host tempo rounded to whole µs per quarter drifts up to 4.2 ms an hour
  against the host, to whole ns 4.2 µs (*calculated*), hence ns in the Tempo event.

### 2.4 Hosts (design §4.4, §10)

JUCE reports tempo and ppq once per host block, at the block's start, each optional
(`juce_AudioPlayHead.h:284-296`); VST3 and AU fill them per block. No plugin format delivers
MIDI clock (VST3's event types, `juce_VST3Common.h:1509-1540`); only the Standalone sees device
MIDI, stamped by arrival into the next block (`juce_MidiMessageCollector.cpp:94-139`). Hosts
differ on loop wraps inside a block (VST3 `ivstprocesscontext.h:87-93`), Reaper changes block
lengths around loops, and Live's VST3 offline render does not set non-real-time mode
[protocols]. Design §4.4 lists what stays unreproducible.

### 2.5 The code (design §2, §4, §6, §10)

Findings C1–C12 are listed with their citations in design §12. Those that corrected the survey:
C1 ("Restart on transport start" is off by default, so the survey's "keep it on" assumed the
opposite), C5 (the stored default `Quarter`, labelled 1/4, is a whole-note grid under the
Microcosm's reading), C6 (two stored subdivisions, no rule for which wins), C8 (`sound_hash`
covers STAT's `tempo_source` byte) and C12 (the ITCM arithmetic). The revision's C13–C18:

- **C13**, the pedal's Exact load: a muted callback returns before `g_blocks` advances
  (`firmware/live/main.cpp:94-98`, `:110`), and the load then clears the queue and zeroes the
  timeline (`:390-403`); `Clear()` resets the order check (`dsp/src/EventQueue.cpp:56-63`).
  Behind findings E4 and H1.
- **C14**, libDaisy's UART: any receive error in DMA mode aborts reception and nothing but
  `MidiHandler::Listen()` restarts it (§2.8). Behind H2.
- **C15**, ITCM (§2.9). Behind H3.
- **C16**, the post delay's line: `kPostDelayMaxSeconds` = 2.0 (`dsp/src/PostChain.cpp:52`), the
  line round(2.0·R) frames (`:177`), its target clamped below its length (`:240-248`), and reads
  by distance behind the write head (`dsp/src/detail/PostChain.h:98-102`), so a longer line
  changes no unsynced read. Behind P5's fix.
- **C17**, the golden harness: `StateAt` folds only `SetParam`, `SpilloverLoad`, `MacroMove` and
  `Expression`, and the restart tail Exact-loads `StateAt`'s package
  (`dsp/tests/golden/Corpus.cpp:664-742`). Behind E3.
- **C18**, the plugin's restart: `ToPreset` copies the mode's stored performance state
  (`plugin/src/PluginProcessor.cpp:71`), and the real-time path swaps in the spare only when its
  leaves and mode match (`:575-587`). Behind E7.

**The factory set.** No package under `firmware/factory/`, its reserves or the golden presets
carries a clock, subdivision, `base_sync`, tempo or performance key [internal]. Of the first
set, Engram and Callback set `post.delay.time_ms`; Shards and Undertow touch only the post
delay's feedback or mix (a search of the JSON, 2026-10-08). Behind P7 and D21.

### 2.6 The grid-firing probe (design §2.2, §6.3)

**Review [E]'s simulation** (`clockreview/phasor.py`, a literal transcription of draft v1's §2.2
and §6.3; `b48.py`): quarter hits fired in 60 s at 48 kHz, by block size. Re-run unchanged:

| Tempo | 48 | 64 | 441 | 512 | Expected |
|---|---|---|---|---|---|
| 140 BPM (857.14 frames per tick) | 121 | 121 | 140 | 136 | 140 |
| 137.5 BPM | 126 | 126 | 138 | 126 | 138 |
| 128 BPM | 128 | 128 | 128 | 128 | 128 |

At 120 BPM, 1,000 frames per tick, every block size agrees, which is why a golden case at the
stored default would never have shown the bug. At block size 1 draft v1's rule fires only the hit
at frame 0. **The cause:** a boundary inside the frame before a span's start has its first frame
at the span's start, but is `tick` there, which the rule excluded, and in the previous span its
frame was not below the span's end. **Review [E]'s fix** (`fix.py`: start at k = tick when
acc < K) makes block sizes 1, 512 and random agree at 140, 137.5 and 97 BPM.

**The revision's rule** (design §6.3: a catch-up of the largest due grid position at every span
start, then the positions after `tick`) generalises that fix to the jumps of taps, placements and
tempo rescales. `clockrevise/grid.py` renders 30 s at 20, 97, 137.5, 140 and 300 BPM with grids of
3, 24 and 96 ticks and 25 random events each (tempo changes to random tempos and taps), at block
sizes 1, 48, 441, 512 and two random sequences of 1–512, every render also split at its events:
**all hits identical to block size 1** in every case, and at constant tempo every hit on
⌈k·P/K⌉. Quarter hits in 60 s at 48-frame blocks: 140, 138, 97, 300 and 20, as expected.

### 2.7 Follower arithmetic (design §3.3)

`clockrevise/sums.py` keeps the four sums incrementally (add, remove, re-base by the old sums,
design §3.3) over 20,000 ticks with random gaps of 300–900 frames and 2 % dropouts of 1–5 labels,
for a window of the fitted ticks among the last 96 labels, and asserts after every tick that they
equal sums computed directly: they do. Bounds with x ≤ 95, N ≤ 96 and y < 95·R at R = 384,000
(every gap under R, the window cleared otherwise): y ≤ 3.65·10⁷ (int32 holds 2.1·10⁹); Sxx ≤
8.7·10⁵, Sy ≤ 3.5·10⁹, Sxy ≤ 3.3·10¹¹, D ≤ 8.3·10⁷, A ≤ 6.4·10¹³, B ≤ 6.1·10¹⁵ and |ρD| ≤
1.5·10¹⁶, all below 2⁶³ ≈ 9.2·10¹⁸ (*calculated*). Draft v1's window could run for hours
outside the clock states: y overflows `int32_t` after 12.4 h at 48 kHz or 1.55 h at 384 kHz
(*calculated*, review [E]), and Sy·Sxx passes 2⁶³ after about an hour at 120 BPM (review [P]'s
calculation).

**Division by zero** (E5): when every fitted tick applied at one frame, every y is equal, so
A = N·Sxy − Sx·Sy = N·y₀·Sx − Sx·N·y₀ = 0 while D > 0; draft v1's dropout rule divided by A.
Integer division by zero traps on x86 and returns 0 on the Cortex-M7 unless `CCR.DIV_0_TRP` is
set, so the two platforms would disagree. Ticks land on one frame whenever pushes are late
(`dsp/src/EventQueue.cpp:41` puts them at offset 0), after an Exact load's mute, and in the
Standalone's block stamping.

### 2.8 Hardware and libDaisy (design §9)

- **Pins:** USART1 receives on PB7 and transmits on PB6 (LD `src/hid/midi.cpp:10-16`), D14 and
  D13 (LD `src/daisy_seed.h:223-224`); D9 is PB4 (`:219`).
- **Priorities:** USART1 and every DMA stream at NVIC priority 0, the audio's included (LD
  `src/per/uart.cpp:897`, `src/sys/dma.c:17-34`); the firmware moved USB below audio
  (`firmware/platform/UsbSerial.cpp:70-74`).
- **Receive errors** (H2): in DMA mode the HAL treats any receive error as blocking and aborts
  the DMA (LD `stm32h7xx_hal_uart.c:2296-2321`); `HAL_UART_ErrorCallback` clears
  `listener_mode_` (LD `src/per/uart.cpp:1105-1109`) and `DmaTransferFinished` re-initialises
  the UART without restarting it (`:373-382`); only `MidiHandler::Listen()` restarts reception
  (LD `src/hid/midi.h:194-205`).
- **DMA buffers:** `DMA_BUFFER_MEM_SECTION` is `.sram1_bss` (LD `src/daisy_core.h:25`), D2 SRAM
  that the linker script marks MPU non-cacheable (`firmware/linker/seed_h750.ld.in:11`, `:29`),
  16 of its 32 KiB used by the live image; libDaisy's own MIDI buffer lives there
  (`src/hid/midi.cpp:7-8`); every receive callback invalidates the buffer's cache lines rounded
  out to 32 bytes (LD `src/sys/dma.c:94-101`, `src/per/uart.cpp:987-1009`).
- **The receive RC** (design §9.1): 270 Ω × 100 nF is τ = 27 µs, and a rising edge needs about
  1.2τ = 32 µs to cross 0.7·VDD, one bit at 31.25 kbaud (*calculated*); review [H] confirmed it.
  The transcription's capacitor is almost certainly the optocoupler's supply decoupling.
- **The H11L1M:** pin 1 anode, 2 cathode, 4 output, 5 ground, 6 VCC. The research has Funbox's
  parts list only: R30 220R and R31 470R, flagged as possibly out of sync with its schematic
  (`daisy-pedal-platforms.md:349-352`); the 220 Ω and 270 Ω the design uses are the Electrosmith
  transcription's (`pedal-control-surface-and-io-hardware.md:343-360`).
- **MIDI out:** TX through two 10 Ω resistors, one to the pin and one to 3.3 V (`:363-364`).

### 2.9 ITCM and CPU (design §9.6)

**ITCM** (*measured*, the `modes-impl` live image's map): `.itcm_text` is 61,840 of 65,536 bytes
(94.4 %), 3,696 spare. By object: `Engine.cpp` 15,246, `Granular.cpp` 11,404, `PostChain.cpp`
11,244, `Validate.cpp` 8,612, `DetMath.cpp` 5,496, `OnsetDetector.cpp` 1,792, `Mode.cpp` 1,468,
`ModeEval.cpp` 1,292, `EventQueue.cpp` 452, `mem*` 1,728 and the shared tables 2,536. Only
`Decode`, `Encode`, `Sha256` and `TestSignal` are kept out (`firmware/CMakeLists.txt:198`).
`ValidateStructure` runs inside `ApplyEvent` on every Spillover load (`dsp/src/Engine.cpp:315`,
`:1068-1079`). The CPU proposal (scratch `cpu/cpu-budget-proposal.md`) adds 6.3 KB or 1 KB (its
FFT rewrite), 3.75 KB (its step 4) and about 2.3 KB (the governor), and requires 8 KB spare (its
G6). So with `Validate` out the remainder is −0.35 or +4.95 KiB before the tempo core
(C12), −1.35 to −1.95 or +3.35 to +3.95 KiB after it (*calculated*, review [H]).

**CPU** (`rev7-silicon-record.md` §3.2–§3.6, *measured* on the Rev7 at revision 1): the default
preset's warm mean 1,902 cycles per sample; XIP against ITCM 1.076× warm and 1.697× cold for it,
so its code path refetched cold costs about 1,902 × 0.621 × 48 ≈ 57k cycles per block
(*calculated*); `pess_events`' worst block 146.3 %; one birth at most 6,203 cycles at 48 voices;
the post delay 319 cycles per sample; a dirty `Restart` 47.2 ms, of which the 16 MiB clear is
44.97 ms. Review [H]'s cold-fetch estimate for a tick, 3–10k cycles for 1–2 KiB at 100–200
cycles per 32-byte line, is scaled from these and stays *estimated* until T1's bench.

**The governor** (CPU proposal): trigger births that fail admission stay pending while the
oldest voices take FastCut's 128-frame fade; scheduler births need no trigger pending; beyond 8
pending per source the oldest is dropped; P(f) charges each post stage a constant K_s; the
constants freeze once, at its revision 8 (its D8); its event reserve is a 24k-cycle placeholder.

## 3. Where the sources disagreed, and how the design resolves it

1. **What Subdiv's labels mean.** The research note left it open (`microcosm.md:112`); draft
   compiler §2.3 assumed note values ("default `1/4`"). *Resolution:* rate multipliers (§2.1),
   re-coded so 0 is TAP, and written as rates so no label reads as a note value (design §5.1).
2. **One-beat averaging against a fit.** The survey recommended averaging MIDI clock over one
   beat (about 0.2 %). That holds for hardware clocks only; a computer's clock wobbles a one-beat
   average about ±1 % (§2.3). *Resolution:* the 96-label least-squares fit (design §3.3).
3. **"Keep restart on transport start on."** The survey assumed it was on; it is off by default
   (C1). *Resolution:* it stays off, and bounces are reproducible only with it on (design §10.1).
4. **The 100 nF on the receive line** (§2.8). *Resolution:* treated as supply decoupling;
   checked against the schematic image before parts are bought (design §9.1).
5. **Tempo source per preset or global.** `companion-app.md:780` and `mode-compiler.md:257-258`
   store it per preset; `companion-app.md:788-789` calls it global. *Resolution:* global, the
   Microcosm's way (D3); STAT's byte reserved.
6. **The grain-engine API's `SetTempo` and `SetExternalClock`** against compiler §7.4's events.
   *Resolution:* events only (design §2.6); grain-engine §9 amended.
7. **The UART's priority.** The survey proposed raising it above audio, as the firmware
   re-prioritised USB. The firmware moved USB *below* audio (`UsbSerial.cpp:70-74`), and with
   block-grid stamps a higher priority buys nothing. *Resolution:* priorities unchanged (D20);
   T0b's timer stamps decide whether sub-block stamps are worth it.
8. **What an Exact load does to tempo.** The survey: the stored tempo only under the internal
   source. Draft v1: the stored tempo always, with the pedal re-asserting a clock tempo. Review
   [P]: that discards a tapped tempo under Keep and the master's beat under clock.
   *Resolution:* the engine always starts from the stored state (the parity contract's exact
   restart), and producers re-assert the running tempo, the master's position and, after a
   restart of the same preset, the live Subdiv (design §2.5).
9. **Where the tap switch goes.** The research polls footswitches through a CD4021; the survey
   and draft v1 put tap on an EXTI line. Review [H] showed the time-based EXTI debounce passes
   the release bounce. *Resolution:* sampled once per audio callback with a state debounce, which
   also suits the CD4021 later (design §9.2).
10. **The post delay's 2 s ceiling** (engine §2.6's Space-knob delay) against whole notes at the
    default tempo. *Resolution:* synced targets get 4 s; `time_ms` keeps 2 s (design §5.3).

## 4. Alternatives considered and rejected

- **A DLL or PLL clock follower.** Infinite memory, lock-in transients after a glitch; the
  finite window resets cleanly and serves re-acquisition (§2.3).
- **µs in the Tempo event.** 4.2 ms an hour of drift against a host (§2.3).
- **Clamping a synced duration** instead of folding: a clamp plays off the grid (a `2/1` at
  119 BPM would play its downbeat 34 ms early); a fold stays on it.
- **Base delay = division − 512 frames** for Q11: every echo early, the first included; **a
  variable feedback path**: sound-changing on every feedback preset (design §6.2).
- **Skip detection in tap:** it would make halving the tempo by tapping impossible.
- **Restamping old-epoch ring entries at the new timeline's first boundary** (review [E]'s first
  option for E4): it would bunch the ticks of a 49 ms mute onto one frame, exactly the input of
  E5, for a position the re-assert carries anyway. Dropping and counting them, while still
  parsing their bytes, is review [H]'s option and the design's.
- **`ToPreset` carrying rows 83/84 and the committed tempo** into the restart's Exact load
  (review [E]'s first option for E7): the engine would then restart from a state no package
  holds, rounded to whole µs, and the plugin would differ from the pedal's re-assert rule. The
  wrapper re-asserts as events instead, review [E]'s second option.
- **The Tempo-mode knob driving the Subdiv zones under external clock** (review [P], P9): the
  effective subdivision would then depend on the source, so every clock acquisition and loss in
  Tempo mode would jump the subdivision and crossfade the delay. With the two-state gesture the
  player reaches Subdiv mode with one press, and the LED shows the clock owns the tempo; the
  knob's behaviour under clock stays open (design §11.8 item 5).
- **A tempo-dependent host value text for row 63** (review [P], P5): hosts cache value text as
  a function of the value; the effective value is shown in the editor and the BPM panel instead.
- **Playing the longest straight value that fits** instead of folding (review [P]'s alternative
  for P5): a dotted value would become a triplet one at slow tempos (a `1/2d` at 40 BPM would
  play `1/1t`); folding keeps the rhythmic family, and L14 reports where a sweep stops being
  monotone.
- **JSON labels `"/4"` and `"/2"`** (review [P]'s spelling for P6): one character from the note
  names `"1/4"` and `"1/2"`; `x1/4` and `x1/2` share the `x` of every other multiplier.
- **Confirming a tap 2 ms after the press** (review [H]'s H6): a sample at exactly 2 ms can land
  on the contact's press bounce and lose the tap; two lows in the next three callbacks tolerates
  it, at a fixed 5 ms latency instead of 4.
- **Exempting pending CLOCK hits from the governor's scheduler block** (review [H]'s first option
  for H5): it would let the scheduler spend the budget the hit is waiting for; capping a hit's
  wait at 48 frames bounds both the lateness and the cloud's loss.
- **Keeping the clock-out lane out of v1** (review [P]'s fallback for P11): adopted instead as a
  firmware-only lane, since it costs no engine change and closes a documented parity gap.

## 5. Provenance

Written 2026-10-08 from the survey and the three investigations of record §1, on branch
`claude/clock-design` from `claude/mode-compiler-impl` at `343f33c`, as draft v1 (commit
`ca09f3a`), and revised the same day into draft v2 after the three reviews (§6). The design's
owner decisions (design §11.5) carried provisional answers until 2026-10-09, when the owner
confirmed every one as proposed and the design became owner-approved; its open questions (§11.8)
are settled at T0b or by listening.

## 6. Review of draft v1 and its disposition

Three reviews read draft v1: **[E]** determinism and parity (1 critical, 4 major, 6 minor),
**[P]** the musician and the product (7 major, 5 minor), **[H]** embedded, CPU and hardware (1
critical, 5 major, 2 minor). The revision checked each finding against the code at `343f33c`,
libDaisy's source, the live image's map, the CPU proposal and the reviewers' probes (§2.5–§2.9),
and re-ran review [E]'s simulation. **Every finding survived the check and was applied; none
was rejected outright.** Eight were applied with a part or a detail changed, each explained in
§4 or in its row: E4 (drop, not restamp), E7 (events, not `ToPreset`), P4 (a threshold on the
early commit), P5 (folding kept; its tempo-dependent host text rejected), P6 (the `x1/4`
spelling), P9 (its Subdiv zones under clock rejected: the Tempo knob stays inert there), H5 (a
lateness cap, not an exemption), H6 (two of three samples). Those two parts are the only ones
rejected, each with its evidence in §4. E4 and H1 are the same defect, E8 and P2's second half
the same, E6 and P4's window clearing the same.

| # | Finding | Disposition (design §) |
|---|---|---|
| E1 | Grid hits whose boundary falls inside the frame before a span start are never fired | applied, generalised: a catch-up of the largest due position at every span start, which also covers rescales and placements (2.2, 6.3); simulated identical at every block size (record §2.6); unit test at block size 1 against 512; golden clock presets at 140 and 137.5 BPM (8.2, 8.3) |
| E2 | Lazy clock loss misses a Spillover load and the `Tempo()` snapshot | applied: the gap rule before every event of any type and before a direct Spillover load; snapshots evaluate it without mutating; the reference test includes Spillover after a loss (2.6, 3.5, 8.2); profile §5.11's note corrected |
| E3 | The RestartTail rule cannot be built for tap or clock tempos, P ≠ Pc, live Subdiv | applied: `Restart` sets the active preset's stored performance state; the tail needs nothing re-sent; a `Restart` against `Init` + Exact test (2.5, 8.2, 8.3) |
| E4 | Pedal ring entries cross the Exact-load reset and stall every later event | applied with H1: epoch-tagged entries, old ones parsed but never stamped, the last stamp reset with the queue, re-asserts before `Unmute()` (4.5, 9.3); restamping rejected (§4) |
| E5 | Dropout inference can divide by zero | applied: inference only from a valid fit with N ≥ 8, through the clamped P_fit, so m < 120 and no division by A; same-frame ticks then a slow gap in the tests (3.3, 8.2) |
| E6 | The follower window is never cleared outside clock states | applied: a gap clears the window in every source; the window bounded by labels; bounds restated at 384 kHz (3.3, 3.5; record §2.7) |
| E7 | The plugin's transport-start restart replays the stored Subdiv, time mode and tempo | applied, second option: the wrapper re-asserts rows 83/84 and the tempo at frame 0 after every restart of the same preset; bounce test with row 83 changed (10.1, 8.4); `ToPreset` option rejected (§4) |
| E8 | An Exact load under a running clock loses beat phase | applied with P2: the translator follows the master's position; Locate and Continue re-asserted at frame 0 (2.5, 4.3, 9.3) |
| E9 | A restarted bounce gets a spurious hit at frame 0 | applied: host-style Start, Continue and Locate carry the anchor's offset in `id` bits 16–31 and apply at the block's first frame (2.2, 4.1, 4.4) |
| E10 | Stop does not cancel an armed Start, Continue or Locate | applied: Stop cancels and counts it; FA, FC, F8 in the tests and `clock_midi_hw` (3.4, 8.2, 8.3) |
| E11 | Several integer rules not pinned for the rational reference | applied, each: (a) the tap's beat in the old units; (b) residuals as exact numerators; (c, d) rounding through the helpers, ties up; (e) rule 3.2 counts fitted ticks, reset by an outlier; (f) `gridFrames` an integer and the jitter's binary32 order, principle 2 amended; (g) chained fades start inside the per-sample loop (1.4, 2.1, 3.2, 3.3, 6.3, 7.1, 7.3) |
| P1 | Every tempo change glides, swooping up to an octave | applied: Pc changes classed as jumps (> Pc/32, crossfaded), drifts (slewed) and steps (glided); `global.tempo_glide` (row 86, Off by default) restores the glide; `tempo_jump` golden case; S12 both ways (7.1–7.4, 8.3, 8.5, D22) |
| P2 | Exact loads discard the tapped tempo under Keep and the master's beat | applied: producers re-assert the committed tempo under Keep or a followed clock, and the master's position under a running clock; D4 and D17 restated (2.5, 9.3, 10.1) |
| P3 | A one-second gap after any earlier Stop leaves the pedal internal for the rest of the song | applied: `masterStopped` cleared by an applied Start or Continue; a loss under ClockRunning resumes ClockRunning after 24 ticks; golden `clock_song` case (3.1, 3.5, 8.3) |
| P4 | Song starts lock late: a stale window and Pc waiting 24 ticks | applied: the window cleared at every gap; an early commit at 12 ticks after a Start on a fresh window when the tempo moved more than 6.25 % (the review proposed N ≥ 12 without a threshold; the threshold keeps a 1.4 % fit from overriding a correct re-asserted tempo) (3.5, 7.1, 7.4) |
| P5 | The 2 s fold duplicates note values and Subdiv positions at the default tempo | applied: synced targets get a 4 s line (unsynced unchanged), the monotone claim corrected, lint L14, the effective value shown in the editor and BPM panel (5.2, 5.3, 11.1, D23); tempo-dependent host text and "longest straight value" rejected (§4) |
| P6 | Subdiv's "1/4" and "1/2" mean the opposite of the note names beside them | applied, with the spelling `x1/4`, `x1/2`, `tap`, `x2`, `x4`, `x8` (screens ×1/4 … ×8); old labels are E5; the product shown beside row 63 (5.1, 11.1) |
| P7 | At launch tap and the gesture do nothing in every factory preset | applied: `UsesTempo` derived, never stored; the gesture ignored and the tap LED steady in presets without it, tap still setting the global tempo; T5 gives Engram and Callback a synced post delay by listening; D21 (6.6, 11.4) |
| P8 | Clock-drift commits bend sustained trails | applied: deadband commits slew with a second coefficient pair, τ = 1 s, about 1.2 cents for a 0.4 % commit (7.1, 7.2, 7.4) |
| P9 | The three-way cycle passes through Tempo; no soft takeover on a mode change; the knob dead under clock | applied in part: the gesture is the Subdiv ↔ Tempo toggle with Free authored; soft takeover on every time-mode change; under clock the knob stays inert, with the LED showing why (driving Subdiv zones rejected, §4) (6.4, D15, D18) |
| P10 | No way to set the downbeat without a transport | applied: the first tap after a pause marks the downbeat; under ClockFree a tap re-labels the nearest tick as a downbeat; `clock_tap` case (3.2, 3.6, D16) |
| P11 | No MIDI clock out and no soft thru | applied: MIDI out on the breadboard and v1, soft thru on by default, clock out off by default, the Microcosm's four-way setting, firmware lane T4b (9.1, 9.5, D10) |
| P12 | The plugin never persists the internal tempo; saving never captures the performance | applied: the last committed tempo saved in the session and re-asserted; saving a preset captures the live tempo, Subdiv and time mode; the stored tempo beside the live one (10.1, 10.3, 10.4) |
| H1 | Exact load: old-timeline stamps lock out every later event | applied with E4, review [H]'s form: the epoch, old entries dropped and counted after parsing, 64-bit block counts, a T4 hardware-in-the-loop test (4.5, 9.3, 11.4) |
| H2 | A UART receive error stops MIDI reception for good | applied: the control loop polls `RxActive()` and restarts reception, resetting the translator; the buffer in `DMA_BUFFER_MEM_SECTION`; a ring of 256 entries or more; boot and hot-plug tests in T0b and T4 (4.5, 9.1, 9.3) |
| H3 | ITCM does not add up once the CPU plan is counted | applied: an 8 KiB ITCM gate before T1 and T2; the cold engine API out of ITCM or function-level placement; `IntMath.cpp` out of line and out of ITCM; `Validate`'s move measured; T1's estimate raised (2.1, 9.6, 11.4, D1) |
| H4 | ClockTick cost and the per-block bound underestimated | applied: incremental exact sums; the producer caps ticks at two per block; the tick path benched cold at T1; the governor's reserve sized from it; §9.6's table corrected (3.3, 4.5, 9.6) |
| H5 | CLOCK hits against the cost governor unspecified | applied with a cap: grid and birth frames reported apart; deferral counters; a hit waits at most 48 frames, then is dropped; a zero-deferral factory budget; the delay stage's constant re-measured with a fade; benches before the constants freeze (2.6, 6.3, 7.3, 7.4, 8.5, 9.6, D13) |
| H6 | The tap switch's debounce passes its release bounce as taps | applied: sampled each audio callback, a press only after 24 ms high, confirmed by two lows in the next three callbacks, a fixed 5-block latency; no EXTI (4.5, 9.2, D20) |
| H7 | T0b cannot measure the master's jitter | applied: `midilog` muted and with a timer input-capture stamp on D9 (PB4), beside the production stamp; only F8, FA, FB, FC, F2 and taps printed (9.4, 11.4) |
| H8 | Breadboard spec: DIN against TRS, the H11L1M's supply pins, the values' credit | applied: a full DIN netlist for T0b, TRS Type A named with its failure mode, the values credited to the Electrosmith transcription (9.1) |
