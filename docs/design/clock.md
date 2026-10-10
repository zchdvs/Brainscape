# CLOCK: tempo, tap, MIDI clock and host sync

> The CLOCK design pass that [mode-compiler.md](mode-compiler.md) §1.2 defers ("CLOCK, the
> largest feature, gets its own design pass"): the tempo core of wave W2. It specifies the
> engine's tempo state, tap tempo, MIDI clock in, the plugin host's tempo and transport, the
> Subdiv control and note divisions, tempo-synced delay times, CLOCK-quantised grain births,
> the stored performance state, smoothing, source arbitration and preset-load rules, so that
> the pedal and the plugin stay sample-identical for identical event streams. It extends
> mode-compiler.md ("compiler §N"), [grain-engine.md](grain-engine.md) ("engine §N"),
> [companion-app.md](companion-app.md) ("companion §N") and
> [determinism-profile.md](determinism-profile.md) ("profile §N"); §11.9 lists the passages it
> makes stale, which this change amends with dated notes. The evidence is a status survey of
> the tree and three investigations (the Microcosm's and its peers' manuals; MIDI, host and
> estimator arithmetic; the code), summarised with their sources in §12. Numbers are
> *measured*, *calculated* or *estimated*. Code is cited as `path:line` at
> `claude/mode-compiler-impl` `343f33c` (sound revision 7); "LD" is the pinned libDaisy v9.0.0
> and "JUCE" the plugin build's fetched 9.0 tree. Status: **owner-approved design**
> (2026-10-08): draft v2, revised the same day after three reviews of draft v1 (determinism and
> parity, the musician and the product, the firmware and hardware), with the answers to the 23
> decisions of §11.5, which the owner confirmed as proposed;
> [the record](reviews/clock-record.md) ("record §N") keeps the evidence, the probes and every
> finding's disposition. *(2026-10-09: the tempo core is built as a library, §11.10, and wired
> into the engine as sound revision 8, §11.11, which have the as-built notes; 2026-10-10: amended
> after its review, §11.12.)*

---

## 1. Scope

### 1.1 What this pass delivers: the tempo core

Nothing follows a tempo today (`docs/STATUS.md:59`). The vocabulary exists and is switched off:
the stored performance state (`dsp/include/brainscape/PresetState.h:42-56`), its encoding and
validation (`dsp/src/blob/Encode.cpp:208-212`, `Decode.cpp:532-537`, `Validate.cpp:412-418`), the
reserved event numbers 6–11 (compiler §7.4), the `clock` source bit and `Subdivision` enum
(`dsp/include/brainscape/Mode.h:78`, `:86-87`), `baseSync` (`Mode.h:118`, `:123`) and row 63
`post.delay.sync` (`dsp/include/brainscape/Params.h:248`), all refused or ignored until W2
(`Mode.h:72-73`, `dsp/src/Engine.cpp:295-302`, `compiler/src/Schema.cpp:825-826`). This pass
designs, precisely enough to build and test:

1. **The tempo state in `dsp/`** (§2): an integer phasor with zero drift, tempo-derived
   durations in exact integer frames, the stored performance state applied, and what `Init`,
   `Reset`, `Restart` and both load modes do to it.
2. **Sources and arbitration** (§3): the stored tempo, tap, MIDI clock (24 ppqn with Start,
   Stop, Continue and Song Position Pointer) and the plugin host, every estimator running in
   `dsp/` on the frames where events applied, in integer arithmetic.
3. **Events 6–10** (§4): their binary payloads, how the plugin and the pedal produce them, and
   what sessions log.
4. **Divisions** (§5): the Subdiv control's six positions and the note values of synced fields.
5. **What follows tempo** (§6): the post delay (row 63), the grain base delay (`base_sync`),
   CLOCK grain births, and the Time knob.
6. **Smoothing** (§7), **determinism and tests** (§8), **firmware and hardware** (§9), **the
   plugin** (§10), and the schema, revisions, plan and decisions (§11).

### 1.2 What it leaves out

| Left out | Where it goes |
|---|---|
| The rest of W2: step tables (row 60, STEP), mark index and walk, global reverse (event 11, row 79), trigger velocity | Later W2 pull requests, each its own revision (compiler §7.6); §6.3 says where steps attach to the grid |
| The looper and its quantize | Its own design (compiler §1.2); §6.3's grid and §5.2's table are what it should reuse, and its loops should be whole tick counts realigned on Start, the Microcosm's most-cited clock fault being its looper drifting (§12) |
| Modulator `sync` | W3 (`Mode.h:190`); it reads §5.2's table and §2.3's durations |
| MIDI thru and clock out in the engine | Nothing in `dsp/`: soft thru and clock out are firmware only, lane T4b (§9.5, D10) |
| USB-MIDI in | After the move to TinyUSB (`firmware/README.md:635-640`, companion §7.1); it feeds the same translator (§4.3) |
| Time signatures and bars | Not designed; grids align to position 0 of the transport in 24-ppqn ticks, so every grid up to a whole note is bar-aligned in 4/4 only |
| Which panel gesture toggles the time mode, the LEDs | The control-surface design; §6.4 says what the gesture does |
| The pedal's MIDI CC map | The firmware's MIDI design (compiler §1.2, deferred); §6.5 recommends the Microcosm's CCs |
| The CPU budget's fix (the cost governor) | [cpu-budget.md](cpu-budget.md) (the CPU proposal as adopted, on `claude/cpu-speed-1` until it merges), its decisions D1–D13 confirmed by the owner on 2026-10-08 and its D4 applied (P3's FFT rewrite is built); §9.6 says how CLOCK births enter it |

### 1.3 Terms

| Term | Meaning |
|---|---|
| R | The engine rate as an integer, `RoundHalfAwayI32(cfg.sampleRate)`: 48,000 for every pedal-exact path (profile §2.2) |
| Tick | 1/24 of a quarter note, a MIDI clock pulse |
| P, Pc | Frames per quarter note as unsigned Q32.32 (`uint64_t`, frames × 2³²): P drives the grid, Pc (committed) the synced durations (§3.3, §7.1) |
| K | 24 × 2³² = 103,079,215,104: one frame in the phasor's units |
| Phasor | The integer grid state `(tick, acc)` (§2.2) |
| Position | The phasor's tick index: 24-ppqn ticks since the transport's position 0 |
| Grid, G | The CLOCK birth grid: every position divisible by G ticks, G being the effective Subdiv (§5.1) |
| Source | Internal, ClockFree or ClockRunning (§3.1); host tempo is Internal fed by host events |
| Committed tempo | Pc, what synced durations read; it moves only through the rules of §7.1 |
| Time mode | What the Time knob sends: Free (`macro.time`), Subdiv or Tempo (§6.4) |
| Fold | Halving or doubling a synced duration until it fits its target's range (§5.3) |
| Due | A grid position whose first frame at or after its boundary, F(k), is at or before the current frame (§2.2); §6.3 fires every due position once |
| Gap | A second or more since the last ClockTick, applied before the next event of any type (§3.5) |
| Jump, drift commit | A change of Pc above Pc/32 (crossfaded) and a deadband commit below it (slewed over about a second) (§7.1) |
| Re-assert | Logged events a producer pushes at frame 0 after an Exact load to carry the running tempo, the master's position or the live Subdiv across it (§2.5) |
| Epoch | The pedal's timeline number, raised by every Exact load; ring entries from an older one are never stamped (§4.5) |
| `UsesTempo` | A preset that reads tempo: a `clock` source, a `base_sync`, or row 63 reachable (§6.6) |

### 1.4 Principles

1. **Tempo changes only at event frames.** The tempo state changes only when an event or a load
   applies; between them every tempo-derived quantity is a closed-form function of that state.
   So block-split invariance (contract #1) holds by construction, with no internal deadline
   that would have to split a block (§3.5).
2. **Integers only.** The phasor, the estimators and the durations use integer arithmetic, with
   no floating-point arithmetic, so they need no FP guard (`dsp/src/EventQueue.cpp:5-6`) and can
   run from QSPI flash, out of the ITCM (§9.6). Two floating-point steps touch tempo, both inside
   the engine's guard and both with a stated expression order: reading a Transport event's
   position (§4.1) and a CLOCK hit's jitter delay (§6.3).
3. **The engine estimates; producers stamp.** Tap averaging, clock following and arbitration
   run in `dsp/` on the frames where events applied (compiler §7.4), so a logged stream replays
   bit-identically on any build. Producers stamp, filter by device setting, and log.
4. **Microcosm parity where the Microcosm documents its behaviour**, its peers' behaviour where
   it is silent, every such choice an owner decision in §11.5.
5. **Exact loads start from the stored state; producers carry the performance across them;
   Spillover loads never move the beat phase.** The engine's start state after an Exact load
   depends on the package alone, and the running tempo and the master's beat cross the load as
   logged events (§2.5). The Microcosm losing sync on preset changes is a documented field
   defect (§12).

## 2. Tempo state inside `dsp/`

### 2.1 Units and range

- **Tempo range:** 20–300 BPM, PresetState's 3,000,000–200,000 µs per quarter
  (`PresetState.h:48-49`). In frames at 48 kHz a quarter is 144,000–9,600 frames, a tick
  6,000–400.
- **P** is frames per quarter in unsigned Q32.32, like the grains' 32.32 positions
  (`dsp/src/detail/Granular.h:33-34`). At 20 BPM and 384 kHz, the highest rate `Init` accepts, a
  quarter is 1,152,000 frames, so P < 2⁵³ and every product below fits the stated width.
- **From nanoseconds:** a Tempo event carries ns per quarter (§4.1).
  `P = RoundHalfUp(ns·R·2³² / 10⁹)`, computed exactly as `q = (ns·R) / 10⁹`,
  `r = (ns·R) % 10⁹`, `P = (q << 32) + ((r << 32) + 5·10⁸) / 10⁹`; ns·R ≤ 1.2·10¹⁵ and
  r·2³² < 4.3·10¹⁸ both fit `uint64_t` (*calculated*). Presets keep µs (compiler §2.6), and
  ns = 1000·µs is exact.
- **Back to ns** (for `Engine::Tempo()` and displays only): `RoundHalfUp(P·10⁹ / (R·2³²))`
  through `MulDivRoundU64`.
- **Why ns in events:** a host tempo of 140 BPM rounded to whole µs per quarter drifts up to
  4.2 ms per hour against the host; rounded to ns, 4.2 µs (*calculated*: 0.5 unit per quarter
  over 8,400 quarters). The conversion to P adds at most 0.5·2⁻³² frames per quarter, 10⁻⁶
  frames per hour (*calculated*).

**Integer helpers** go in a new integer-only `dsp/src/detail/IntMath.h` whose bodies are out of
line in `dsp/src/IntMath.cpp`, an object kept out of ITCM (§9.6): `DetMath.cpp` is placed in
ITCM whole, and an inline helper compiles into its includer, `Engine.cpp` among them.
`MulDivRoundU64(a, b, c)` = round-half-up(a·b / c) with a 128-bit intermediate built from
32-bit limbs, because 32-bit `arm-none-eabi` GCC has no `__int128`; a signed wrapper
`MulDivRoundI64` that rounds the magnitude, ties away from zero; and `FloorDivI64`,
`CeilDivI64`, `FloorModI64`, since C++ `/` truncates toward zero and `>>` of a negative value
is implementation-defined in C++17. Every rounding this document calls "round" is one of these
helpers, never a C++ `/` on a signed value. They run at control rate only. The symbol audit
already allows the 64-bit division helpers they compile to (`tools/ci/audit_symbols.py:24-27`).

### 2.2 The phasor

**State** at a reference frame s: `tick`, the greatest position whose boundary time
T_tick < s, and `acc = (s − T_tick)·K`, so 0 < acc ≤ P. The next boundary, tick + 1, falls at
s + (P − acc)/K.

- **Advance** to s + n: `acc += n·K; while (acc > P) { acc -= P; ++tick; }`. The engine advances
  after every render span (n ≤ 512, so n·K < 2⁴⁶; the loop runs at most twice at 48 kHz, where a
  tick is at least 400 frames, and at most eight times at 8 kHz), and every event handler
  advances to its own frame first.
  Euclidean division is unique, so the state after any split of a span equals the state after
  the whole span (*calculated*; tested in §8.2).
- **The frame of boundary k ≥ tick:** F(k) = s + CeilDiv((k − tick)·P − acc, K), the first frame
  at or after the boundary; (k − tick)·P ≤ 192·P < 2⁶³.
- **Due positions.** Position k is due at s when F(k) ≤ s. Every position up to `tick` has
  F(k) ≤ s, and `tick` itself has F(tick) = s − ⌊acc/K⌋, which is **exactly s when acc < K**:
  its boundary lies strictly inside the frame before s, so its first frame at or after is s. Such
  a position belongs to the span that *starts* at s, not to the one that ends there. §6.3's
  firing rule catches it up at every span start; draft v1's rule fired only k > tick and lost it
  whenever a block boundary or any event landed on that frame, so 48-frame blocks fired 121
  quarters a minute at 140 BPM where 441-frame blocks fired 140 (record §2.6, finding E1).
- **Placing boundary k exactly at frame f:** `tick = k − 1, acc = P`, so F(k) = f.
- **Placing boundary k at frame f + o** for an offset 0 ≤ o under two ticks (§4.1's host
  anchor): `tick = k − 1, acc = P − o·K`, then `while (acc ≤ 0) { acc += P; −−tick; }`.
- **Init:** boundary 0 at frame 0 (`tick = −1, acc = P`); Restart likewise (§2.5).
- **A tempo change** P → P′ keeps the fraction of the current tick:
  `acc′ = max(1, MulDivRoundU64(acc, P′, P))`. It can make a position due that was not (acc
  crossing K), which §6.3's catch-up then fires at the change's frame.
- **Exactness:** at constant P, F(k) = s₀ + ⌈(k·P − c₀)/K⌉ for a constant c₀: the ideal grid of
  a tempo of exactly P, with nothing accumulated, so the phasor itself never drifts. The only
  error against the source is P's representation (§2.1). A ten-hour unit test checks F(k)
  against the closed form at every beat (§8.2).

**The phasor only moves at event frames** (§1.4): taps, clock ticks, transport events and the
clock-follower's refit place it (§3), tempo changes rescale it, and render spans advance it.
Since every event splits the render, each of these happens at a span start, which is where
§6.3's catch-up looks.

### 2.3 Durations

Every tempo-derived duration is a note value of `ticks` 24-ppqn ticks (§5.2) scaled by the
effective Subdiv's `s` ticks (§5.1; 24 at TAP, the neutral setting):

```
frames = MulDivRoundU64(Pc, ticks · s, 576 · 2³² · 2^k)        // k ≥ 0 halvings of a fold
       = MulDivRoundU64(Pc · 2^j, ticks · s, 576 · 2³²)         // or j ≥ 0 doublings (§5.3)
```

ticks·s ≤ 192·96 = 18,432 and Pc·18,432 exceeds 2⁶⁴, hence the 128-bit intermediate. The
result is an exact integer frame count below 2²⁴, so as a `float` it is exact, and today's
millisecond-to-frame conversions in floating point (`Engine.cpp:803`, `:823`) are bypassed
whenever a field is synced. Durations read Pc, never P, so clock jitter never reaches them (§7.1).

### 2.4 The performance state

`PerformanceState` (`PresetState.h:50-56`) keeps its 8 bytes and STAT's layout (compiler §6.2),
with two fields redefined:

| Byte | Field | Values | Default | Change |
|---|---|---|---|---|
| 0 | `reverse` | 0, 1 | 0 | none; still unsupported until global reverse lands |
| 1 | `time_mode` | 0 Free, 1 Subdiv, 2 Tempo | Free | none |
| 2 | `subdiv` | §5.1's codes, 0 = TAP | TAP | **re-coded** so 0 is the neutral ×1 (§5.1) |
| 3 | reserved | 0 | 0 | was `tempo_source`; the source is a device setting (D3) |
| 4–7 | `us_per_quarter` | 200,000–3,000,000 | 500,000 | none |

**One stored subdivision (D14).** The mode's SCHD chunk also carries a `subdiv`
(`Mode.h:91-97`, compiler §6.2), and no document said which of the two wins (code finding C6,
§12). A preset is one document with its mode embedded (compiler §1.4), so the two
are the same preset's: STAT's is kept, as the Microcosm keeps its Subdiv per preset, and SCHD's
byte becomes reserved (0). A live Subdiv change is then engine state, never mode content, so it
never counts as a mode switch (`Engine.cpp:739-748`); the `Clock` feature follows from the
`clock` source alone (`dsp/src/blob/Mode.cpp:100`, with `:102` removed), and
`kDefaultModeHash` (`Mode.h:269`) is unchanged because the byte was already 0.

**Effective subdivision:** the stored or live `subdiv`, except that Tempo time mode forces TAP
(the Microcosm's "subdivision is always set to quarter notes", §12); leaving Tempo mode
restores the kept value.

### 2.5 Lifecycle and loads

| Call | Tempo P, Pc | Time mode, subdiv | Phasor | Source, follower, tap chain, transport |
|---|---|---|---|---|
| `Init` | 500,000 µs (120 BPM) | Free, TAP | boundary 0 at frame 0 | Internal; all cleared; stopped at position 0 |
| `Reset` (real-time) | kept | kept | kept | kept: frames continue, so every stamp stays valid |
| `Restart` | **the active preset's stored tempo** (P = Pc) | **the active preset's stored ones** | boundary 0 at frame 0; `lastFired` = −1 | **cleared** to Init's: frames restart at 0, so every frame-based history is void |
| Exact load | the stored tempo; then the producer's re-asserts (below) | stored; re-asserted only after a restart of the same preset | Restart's | Restart's; the master's position re-asserted (below) |
| Spillover load | kept, unless `tempo_recall` is Preset and the source is Internal: then the stored tempo, phase-continuous and committed at once | stored | **kept** | kept |
| A mode change (`InstallMode`) | kept | as the load says | kept | kept; only sequencing state resets (`Engine.cpp:739-748`) |
| Spillover epoch (`ApplySpillover`) | — | — | not tied to the epoch: the phasor counts absolute frames | — |

**`Restart` sets the stored performance state.** The engine keeps the active preset's stored
performance state (8 bytes, the last loaded preset's STAT, Init's defaults before any load), and
`Restart` sets P = Pc, the time mode and the subdivision from it. An Exact load applies that
same state at step 4 and restarts at step 5 (`Engine.h:126-130`, `Engine.cpp:722-730`, code
finding C9), so the load is unaffected; a plain `Restart` then starts from a state that depends
on the active preset alone, which is what the golden harness's restart rule rebuilds (§8.3).
Draft v1 kept the running values through `Restart`, which no script could rebuild: a tapped or
fitted P is not a whole number of ns, P and Pc differ after a Stop, and a live Subdivision is
not in the package (record §6, E3). A unit test checks that `Restart` and `Init` followed by an
Exact load of the active preset leave identical tempo state.

**Exact loads play the stored tempo inside the engine (D4).** They define the exact-restart
start state of the parity contract (profile §2.1), so the engine's state after one cannot
depend on what played before. On the pedal an Exact load also mutes for about 49 ms (47.2 ms
measured, `docs/design/reviews/rev7-silicon-record.md` §3.6, plus about 2.1 ms for the longer
post-delay line, §5.3) and drops the queued events (`Engine.h:106-107`): about 2.4 MIDI ticks
at 120 BPM and 5.9 at 300 (*calculated*). The translator still parses those bytes, so it keeps
the master's position (§4.5).

**Producers re-assert the performance (D4, D17).** After every Exact load, the producer pushes
these as logged events at frame 0 of the new timeline, before audio resumes (§9.3, §10.1):

1. **Tempo**, the committed tempo just before the load in ns (`Tempo().nsPerQuarter`), when
   `tempo_recall` is Keep or a clock or the host was being followed. Nothing under Preset with
   the Internal source: the stored tempo plays.
2. **The master's position** (pedal), when the MIDI translator knows it (§4.3): Locate with
   AtNextTick to the position of the next tick to arrive, which sets the continue position, and
   Continue with AtNextTick when the master is running, so the next tick re-enters ClockRunning
   on the master's beat. Under ClockFree (a master that never sent Start) there is no master
   position: the tempo is restored, the tick phase re-locks after 24 ticks, and the beat label
   restarts until a downbeat tap (§3.2).
3. **The live time mode and subdivision**, as Subdivision events, after a restart of the same
   preset (the plugin's restart on transport start, `prepareToPlay` and session restore,
   §10.1). A preset change plays the new preset's stored ones, as the Microcosm keeps Subdiv per
   preset.

So Keep holds across every load, as the Microcosm's global tempo does, and a preset change under
a running master keeps the band's beat, which is principle 5's point; draft v1 restored only a
clock's tempo, so an Exact load threw away a tapped tempo even under Keep and left CLOCK hits
off the master's beat until its next Start (record §6, P2 and E8). The engine's start state is
still the package's, and parity holds because every re-assert is a logged event. *(2026-10-10:
and the restarted grid fires nothing at frame 0 while the re-asserted Continue waits for the
master's tick, by §3.4's hold, §11.12 note 26.)*

**`global.tempo_recall`** (row 85, a `Global` device setting, §10.4): **Keep** (0, default, the
Microcosm's global tempo) or **Preset** (1, a song preset recalls its tempo, as Strymon's Tap
Mode PRESET, Meris' TEMPO SELECT and Boss's TEMPO HOLD offer, §12). A Spillover load under
Preset applies the stored tempo as a Tempo event at the load frame would; under external clock
the clock wins and the stored tempo is ignored, the source being read after §3.5's gap rule. No
load moves the phasor (principle 5).

### 2.6 Where it lives, and the API

**Placement.** `TempoCore` (`dsp/src/detail/Tempo.h`, `dsp/src/Tempo.cpp`, integer-only) lives in
the Warm arena beside the active mode: the follower's window (96 × two `int32_t`, §3.3), a
6-entry ring of the last ticks, the tap chain and about 120 bytes of scalars, about 1.1 KiB
(*calculated*), inside the Warm arena's 7,968 spare bytes (`firmware/platform/Placement.h:20`,
`:25`). `Engine::Impl` gains a pointer; DTCM grows by about 100 bytes with §6.3's pending clock
birth, §7.3's second head and §7.2's second coefficient pair (*estimated*), inside
`kEngineImplBytes` (7,944 of 8,192 bytes at revision 7, compiler §7.1). §9.6 has the code
placement.

**API** (`Engine.h`): event types 6–10 (§4.1); and two audio-thread snapshots, read like
`Stats()` (`Engine.h:278-287`), computed at the last rendered frame with §3.5's gap predicate
applied **without mutating** the state, so a display never shows a clock that has gone:

```cpp
struct TempoInfo {           // Engine::Tempo(): what displays, logs and producers read
  int64_t  position;         // the phasor's tick at the last rendered frame
  uint32_t nsPerQuarter;     // the committed tempo, Pc, in ns per quarter
  uint8_t  source;           // 0 Internal, 1 ClockFree, 2 ClockRunning (§3.1)
  uint8_t  timeMode, subdiv; // as stored or last set (§2.4)
  uint8_t  flags;            // bit 0 transport running, bit 1 follower locked (N ≥ 24)
  int64_t  lastGridFrame;    // the frame the last grid position fired at: F(k), or the
                             // event's frame for a catch-up; -1 before any (§6.3, §8.5)
  int64_t  lastClockBirth;   // frame the last CLOCK hit was born at, after jitter and any
                             // deferral, or -1
};
struct TempoStats { uint64_t taps, tapsIgnored, tapPhases, tempoEvents, tempoIgnored, ticks,
                    tickOutliers, reacquires, dropoutTicks, gaps, losses, resumes, transports,
                    transportsCancelled, subdivEvents, clockBirths, clockDeferred,
                    clockDeferredFrames, clockDropped, commits, earlyCommits, jumps, slews,
                    crossfades, folds, invalidEvents, unknownEvents; };
```

`ProcessContext::tempoBpm`, `timelinePos` and `transportPlaying` (`Engine.h:206-210`), which the
engine never reads, are deleted with the tempo core: they invite exactly the per-block reading
compiler §7.4 forbids. **No unstamped calls** replace grain-engine §9's `SetTempo`, `Tap`,
`SetSubdiv` and `SetExternalClock`: tempo exists only as events (§8.3 says how the split-delivery
perturbation carries them).

## 3. Sources and arbitration

### 3.1 Sources and states

| Source | Grid follows | Pc follows | Entered by | Left by |
|---|---|---|---|---|
| **Internal** | P, set by the stored tempo, taps, Tempo events (the Tempo knob, the app, the plugin host) | the same, at once | Init, Restart, Stop, a lost clock | clock acquisition |
| **ClockFree** | the follower's fitted line; the position labels continue the phasor's | the deadband of §7.1 | 24 ticks in the window while the master is not stopped (`masterStopped` clear) | Start or Continue at a tick; loss; Stop |
| **ClockRunning** | the fitted line; the position is the master's | the deadband | an armed Start, Continue or Locate applying at a tick (§3.4); or 24 ticks after a loss under ClockRunning (`resumeRunning`, §3.5) | Stop; loss |

The **plugin host** is not a source of its own: the wrapper turns the host's tempo and transport
into Tempo and Transport events (§4.4), which the engine plays as Internal, and while it follows
the host the wrapper drops taps and the Tempo knob's events before they are stamped (§10.1).
MIDI clock reaches a plugin only in the Standalone (§10.2): no plugin format delivers it, VST3's
events being notes, pressure, data, CC output, note expression, chords and scales (JUCE
`juce_audio_processors_headless/format_types/juce_VST3Common.h:1509-1540`).

**Stop reverts to Internal** at its frame, keeping P, Pc and the phase (the Microcosm, §12), and
sets `masterStopped`, which an armed Start or Continue clears when it applies, and Restart
clears. Ticks after a Stop keep the follower's window warm but drive nothing until Start or
Continue, so a master that sends clock while stopped does not drag the pedal into ClockFree.
The flag means "the master is stopped now": draft v1's `transportSeen` stayed set from the first
Stop until a Restart, so after song 1 any one-second clock gap left the pedal free-running for
the rest of the set (record §6, P3). **Clock without any transport message** (some clock boxes
never send Start; peers follow it, §12) enters ClockFree after 24 ticks: tempo and tick phase
lock to the master, and the beat label is the pedal's own until a Start or Continue applies at
a tick (from a Song Position's position, if one came first), or a tap marks the downbeat (§3.2).
*(Amended 2026-10-10 to §3.4's table, §11.12 note 28: a Song Position alone sets only the continue
position.)*

### 3.2 Tap

Taps are Tap events (6); their frames are the frames where they applied. With I the interval in
frames since the chain's last tap (integers throughout, mean = Σ/n over the chain's intervals):

| Case | Test | Action |
|---|---|---|
| Under ClockRunning | — | ignored, counted (`tapsIgnored`); tap state unchanged (Microcosm: tap is unavailable under external clock; the master owns the position) |
| Under ClockFree | — | **phase only**: the tick nearest the tap becomes a downbeat (below); tempo and tap chain unchanged; counted (`tapPhases`) |
| First tap | no last tap | arm: last tap = f; nothing changes |
| Bounce or above 300 BPM | I < R/5 (9,600 at 48 kHz) | ignored; the last tap stays |
| Pause | I > 3R (below 20 BPM), or the chain has intervals and 4·n·I ≥ 7·Σ (I ≥ 1.75 × mean) | new chain armed at f; nothing changes |
| A new tempo | the chain has intervals and 5·\|n·I − Σ\| > 2·Σ (more than 40 % from the mean) | chain := {I}; apply |
| Same tempo | otherwise | append I, keeping the last 4; apply |

**Apply**, in this order, at frame f after the phasor has advanced to f:

1. **The beat, in the old units.** With the phasor's P before the tap, b = FloorDiv(tick, 24)
   and x = (tick − 24b)·P + acc, the nearest beat is B = 24(b + 1) if 2x ≥ 24·P (ties up), else
   24b. Computing x before the tempo changes leaves no question of rescaling `acc` first.
2. **The downbeat** (D16): when this apply is the first of its chain after an arm (the second
   tap after a pause, or the first ever), the first tap becomes a downbeat: B := the smallest
   position ≥ B with B ≡ 24 (mod 96). The tap now being placed is then the bar's second beat, and
   the first tap, I frames earlier, its first, so whole-note grids and (later) step patterns start
   where the player counted "one". Later taps of the chain re-phase to the nearest beat only.
3. **The tempo:** `P = Pc = RoundHalfUp((Σ << 32) / n)` (Σ ≤ 4·3R < 2²³, so the shift fits),
   committed at once as an explicit intent, a jump if it moves Pc by more than 1/32 (§7.1).
4. **The placement:** boundary B at f (`tick = B − 1, acc = P`), and
   `lastFired` = max(`lastFired`, B − 1), so a forward jump skips the grid points it passed and
   B itself fires at f unless it already has (§6.3).

**A phase tap under ClockFree** marks the downbeat without touching the tempo, which the master
owns: with k_n the tick nearest f (`tick + 1` if 2·acc ≥ P, else `tick`), the window's labels,
the ring's, the phasor's `tick` and `lastFired` all shift by δ = 96·FloorDiv(k_n + 48, 96) − k_n
(−48 ≤ δ < 48), so k_n becomes the nearest multiple of 96. Times do not move, only labels, and
`lastFired` moves with them, so no hit repeats; the grid's phase follows the new labels from the
next due position (a passed one fires at f by §6.3's catch-up). A pedalboard clock box that never
sends Start otherwise leaves whole-note grids on an arbitrary bar phase, and field reports show
Microcosm users stopping and restarting to realign (§12; record §6, P10).

So tempo is set by the second tap and refined by each tap to a mean of four intervals (EHX
averages the last four; Eventide "more than 2", §12); a pause re-arms instead of producing a
wrong slow tempo; halving the tempo takes three taps (the first long interval reads as a pause),
doubling it two. The Microcosm's "smoothly matches the tempo" is the post delay's glide for small
corrections (§7.2); a new tempo crossfades (§7.3). No skip detection: it would make halving by
tapping impossible.

### 3.3 The MIDI clock follower

**Why a fit.** A computer's MIDI clock jitters by σ = 8.43 ms cycle-to-cycle with peaks of
−38.4 ms (E-RM's Ableton Live measurement, §12), about σₓ = 3.4 ms per tick. A one-beat average
then wobbles about ±1 % (1.2 BPM); a least-squares line through the last 96 ticks (four beats)
gives σ ≈ 0.06 % (0.07 BPM), comparable to Live's own slave smoothing (σ = 0.114 BPM), and
0.005 % for a hardware clock stamped on the pedal's block grid (σₓ = 0.29 ms) (*calculated*,
§12). The status survey's first recommendation, MIDI clock "averaged over one beat (about
0.2 %)", holds for hardware clocks only (D8).

**Window.** The fitted ticks whose labels are among the last 96 (label > newest − 96), so at
most N = 96, as `int32_t` pairs (x = label − label₀, y = frame − frame₀) relative to the oldest;
plus a 6-entry ring of the last received ticks (label, frame), fitted or not. Bounding the
window by labels rather than by count keeps x in [0, 95] however many ticks were inferred or
excluded, and every tick-to-tick gap is under R (§3.5 clears the window at R), so y < 95·R:
3.65·10⁷ at 384 kHz, the highest rate `Init` accepts (*calculated*). Draft v1 bounded y for
contiguous ticks at 192 kHz, and nothing outside the clock states ever cleared its window
(record §6, E6).

**The sums** Sx = Σx, Sy = Σy, Sxx = Σx², Sxy = Σxy, in `int64_t`, are kept incrementally and
exactly: a tick entering or leaving adds or subtracts its own terms, and re-basing to a new
oldest tick δ labels and η frames later uses the old sums on the right:

```
Sx' = Sx − N·δ        Sxx' = Sxx − 2δ·Sx + N·δ²
Sy' = Sy − N·η        Sxy' = Sxy − η·Sx − δ·Sy + N·δ·η
```

A tick then costs O(1) plus its multiply-divides; draft v1 re-based and re-summed all 96 pairs
on every tick once the window was full (record §6, H4; the update is checked against direct sums
in record §2.7).

**The fit,** whenever the window changes and N ≥ 2: D = N·Sxx − Sx²; slope numerator
A = N·Sxy − Sx·Sy (frames per tick × D); intercept numerator B = Sy·Sxx − Sx·Sxy. The fit is
valid when D > 0 and A > 0. Then `P_fit = MulDivRoundU64(24·A, 2³², D)`, clamped to the tempo
range. A residual is kept as an exact numerator and never divided: for label k at frame f,

```
ρD(k, f) = (f − frame₀)·D − B − A·(k − label₀)        = (f − T̂(k))·D
```

With x ≤ 95, N ≤ 96 and y < 3.65·10⁷, |D| < 8.4·10⁷, |A| < 6.4·10¹³, |B| < 6.1·10¹⁵ and
|ρD| < 1.6·10¹⁶, all inside `int64_t` (*calculated*, record §2.7). **Cost:** at most three
128-bit multiply-divides per tick, about 1–3k cycles with its code in the I-cache and more when
it is cold (§9.6); at most 120 ticks a second, and at most two in one pedal block (§4.5).

**On each ClockTick at frame f,** after §3.5's gap rule:

1. **Label.** Normally the last label + 1: one label per received tick, so a jitter burst never
   corrupts the position. A **dropout** is inferred only from a valid fit with N ≥ 8, the
   outlier floor: when f − (last tick's frame) ≥ max(4·P_fit/K, R/10), m =
   `MulDivRoundU64(f − last, K, P_fit)` − 1 lost ticks; the tick is labelled last + 1 + m and m
   is counted (`dropoutTicks`). P_fit is clamped to the tempo range, so m < 120 for any gap
   under R, and nothing divides by a slope that can be zero: draft v1 divided by A, which ticks
   applied at one frame make 0, a trap on x86 and a silent 0 on the Cortex-M7 (record §6, E5).
   100 ms exceeds the worst computer-clock burst measured, so jitter never looks like loss.
   Shorter gaps are not inferred (§11.6 item 6): a lost tick then offsets the labels by one tick
   until the next Start, Continue or Song Position.
2. **Outlier.** From N ≥ 8, with ρD = ρD(label, f), the tick is an outlier when 4·|ρD| > A and
   100·|ρD| > R·D: more than a quarter tick and 10 ms off the line. An outlier keeps its label
   but is not fitted. Six outliers in a row of one sign mean the master changed tempo:
   **re-acquire**, replacing the window with the 6-entry ring (`reacquires`).
3. **Fit,** if the tick was fitted; ticks whose labels fall out of the last 96 leave the window.
4. **Place the phasor** (ClockFree and ClockRunning only) on the fitted line at f, with k_L the
   tick's label: d = `MulDivRoundI64(K, ρD(k_L, f), D)`, the signed offset of f past T̂(k_L) in
   phasor units; `tick = k_L + CeilDiv(d, P_fit) − 1`, `acc = d − (tick − k_L)·P_fit`, so
   0 < acc ≤ P_fit; P := P_fit. With N < 2 or an invalid fit the tick's own frame is its boundary
   (`tick = k_L − 1, acc = P`, P unchanged). A grid position this makes due fires at f by §6.3's
   catch-up.
5. **Commit** Pc by §7.1 (ClockFree and ClockRunning).
6. **Acquire:** under Internal with N reaching 24 and `masterStopped` clear, enter ClockFree, or
   ClockRunning when `resumeRunning` is set (§3.5), relabelling the window so the newest tick's
   label is the phasor's nearest tick (`tick + 1` if 2·acc ≥ P, else `tick`: ties go up), so the
   grid does not jump by a tick; then steps 4 and 5, with §7.1's acquisition commit.

Between ticks the phasor runs on at P_fit, so births follow the fitted line, not the raw ticks,
and a missing tick never delays one. The phase error of the line's one-tick-ahead prediction is
about 0.21·σₓ: 0.06 ms for a hardware clock, 0.7 ms for a computer's (*calculated*, §12). A
digital PLL (Adriaensen's DLL, §12) was considered: its infinite memory gives lock-in transients
after glitches, and the finite window resets cleanly and serves re-acquisition.

### 3.4 Transport

MIDI's transport applies at the next clock: "the slave starts upon receipt of the very next MIDI
Clock" after Start, and Continue resumes from the last stop point or Song Position (MIDI 1.0,
§12). Transport events therefore carry an **AtNextTick** flag (§4.1): the MIDI translator sets
it, the plugin host's events do not.

| Event | With AtNextTick (MIDI) | Without (host) |
|---|---|---|
| Start, position p | armed; at the next tick, that tick's label becomes p (the window relabelled, keeping its tempo data), `lastFired` = p − 1, the phasor placed (§3.3 step 4) so the downbeat fires at that tick or by catch-up, `masterStopped` cleared; source ClockRunning; if the window was empty before that tick, §7.1's early commit is armed | boundary p placed at the event's frame plus its offset (§2.2, §4.1), `lastFired` = p − 1; the transport runs; source unchanged |
| Continue | armed; as Start with p = the stored continue position | as Start with the stored position |
| Locate p (Song Position × 6) | the continue position := p at once; when ClockRunning also armed, applied as Start *(the continue position under ClockRunning too since 2026-10-10, §11.12 note 27)* | as Start, the transport's running state unchanged |
| Stop | at its frame: **cancels any armed Transport** (`transportsCancelled`); ClockRunning → Internal (phase and tempo kept), the continue position := last label + 1, `masterStopped` set; ticks keep the window warm | at its frame: the transport stops; nothing else changes, the grid runs on |

A host-style Transport (no flag) under ClockFree or ClockRunning is ignored and counted: two
transports cannot both own the position. An armed event waits until a tick arrives; `Restart`
and Stop clear it, and a later armed event replaces it. *(2026-10-10, §11.12 note 26:)* **While an
event is armed the CLOCK grid is held:** nothing fires and the grid positions passed count as
fired, so the tick that applies it places the downbeat with no hit of the old grid just before it,
and an Exact load under a running master fires nothing at frame 0 before the re-asserted Continue
applies (§2.5). Draft v1 let Stop leave an armed Start
in place, so FA, then FC, then the ticks many masters send while stopped started the pedal on a
stopped master (record §6, E10). Start's position is 0; the MIDI translator sends Song Position
as Locate with 6 × its 16th-note count, the plugin its tick-aligned host position and frame
offset (§4.4).

### 3.5 Gaps and losses without deadlines

A **gap** at frame f: a tick has been seen and f − (last tick's frame) ≥ R, a second without a
tick. It is evaluated lazily, **before every event of any type** at frame f and before a direct
Spillover load applies, and read-only by the snapshots (§2.6). When it holds, in every source:

- the window, the ring and the dropout reference are cleared (`gaps`), so the next tick starts a
  fresh fit;
- under ClockFree or ClockRunning it is also a **loss**: source → Internal with P, Pc and the
  phase kept (`losses`), and a loss under ClockRunning sets `resumeRunning`.

**Resume.** With `resumeRunning` set, ticks that come back re-enter **ClockRunning** after 24
ticks (§3.3 step 6), their labels continuing from the phasor's nearest tick: an implicit
Continue, counted in `resumes`. Stop, `Restart` and an applied Start or Continue clear the flag.
A loss under ClockRunning with no Stop is a cable knock, an interface re-enumerating or a host
stall mid-song; the phasor ran on at P_fit through it, about 0.3 ms per beat off for a computer
master (*calculated* from §3.3's 0.06 %), so after a gap of a few seconds the nearest tick is
still the master's.

**Why lazily is exact.** Nothing between events reads the source or the window: the grid runs
at P either way, Pc does not move, and every effect of a gap (a tap accepted, a Tempo applied, a
Spillover recall honoured, a fresh window) shows only at the next event's frame. So applying it
there gives the same bits as applying it at its deadline, and no block is split at the deadline.
Draft v1 checked only before tempo events, so a Spillover load under recall Preset after a lost
clock still saw the clock, and the BPM display showed MIDI indefinitely (record §6, E2). A unit
test compares the engine with a reference that applies every gap at its deadline frame and
splits there, on streams that include ticks, a second of silence and then a Spillover load under
Preset (§8.2). Clearing the window outside the clock states too means a master that sends clock
only while playing starts the next song from an empty window rather than the last song's slope
(record §6, P4), and it keeps §3.3's bounds over hours.

### 3.6 Priority

After §3.5's gap rule:

| Event | Internal | ClockFree | ClockRunning |
|---|---|---|---|
| Tap | applies (§3.2) | marks the downbeat, tempo untouched (§3.2) | ignored, counted |
| Tempo (knob, app, host, recall, re-assert) | applies | ignored, counted | ignored, counted |
| ClockTick | feeds the window; may acquire ClockFree, or ClockRunning on a resume | follows | follows |
| Transport with AtNextTick | armed, or the continue position | armed | armed |
| Transport without | applies | ignored, counted | ignored, counted |
| Subdivision | applies | applies | applies |
| Spillover with recall Preset | stored tempo applies | ignored (clock wins) | ignored |

"Receive MIDI clock" is a device setting the pedal's control loop honours by not feeding the
translator (§9.3), and the plugin's Tempo source setting decides whether the wrapper emits host
events (§10.1): both shape the stream before it is logged, so the engine needs neither.

## 4. Events

### 4.1 Payloads

Events keep `(int64 frame, uint32 seq, uint8 type, uint32 id, float value)` (`Engine.h:173-190`);
nothing widens. Numbers are permanent once landed (compiler §7.4).

| Type | No. | `id` | `value` | Applies |
|---|---|---|---|---|
| Tap | 6 | 0 | +0 | at its frame |
| Tempo | 7 | ns per quarter, 200,000,000–3,000,000,000 (unsigned) | +0 | at its frame; phase-continuous; committed at once |
| ClockTick | 8 | 0 | +0 | at its frame |
| Transport | 9 | bits 0–1 kind (0 Stop, 1 Start, 2 Continue, 3 Locate); bit 8 AtNextTick; bits 16–31 the host anchor's frame offset (Start, Continue and Locate without AtNextTick; 0 otherwise); other bits 0 | Start and Locate: the position in 24-ppqn ticks, an integer in [0, 6,291,456); otherwise +0 | Stop at its frame; the others at their frame or, with AtNextTick, at the next ClockTick (§3.4) |
| Subdivision | 10 | bits 0–7 the code, bits 8–15 the field: 0 subdivision (code 0–5, §5.1), 1 time mode (0–2); other bits 0 | +0 | at its frame |
| GlobalReverse | 11 | reserved for the rest of W2 | | |

- **Position range.** 6,291,456 = 96 × 2¹⁶ ticks is a multiple of every grid (3 to 96 ticks) and
  of a 16-step pattern of whole notes (1,536 ticks), and below 2²⁴, so every position is an exact
  binary32. Producers reduce a host position modulo it (floor-mod, so pre-roll wraps to the same
  grid phase); Song Position's largest value, 16,383 × 6 = 98,298, is inside it. The engine
  converts the value inside its guard after checking it is an integer in range (profile §3.10).
- **The anchor offset** (D11). A host-style Start, Continue or Locate applies at the host block's
  first frame f and places boundary p at f + o (§2.2), o being `HostAnchor`'s offset: at most
  one tick rounded to a frame, 48,000 frames at 20 BPM and 384 kHz, inside 16 bits. Nothing
  renders between the restart and the anchor with a grid of its own: draft v1 sent Start at
  f + o, so the boundary Init or `Restart` placed at frame 0 fired a hit between the host's beats
  before the anchor took the grid over (record §6, E9).
- **Song position (D11)** needs no event of its own: it is Locate with AtNextTick.
- **The Tempo event's value is reserved** (+0): taps and knob tempo under host sync are dropped
  by the wrapper, not flagged for the engine (§3.1).

### 4.2 Validity and counting

An event whose payload breaks the table (a tempo out of range, a nonzero reserved bit, a code or
field out of range, a non-integer or out-of-range position, an offset with AtNextTick or on a
Stop, a nonzero value where +0 is required) is ignored and counted (`invalidEvents`), never
clamped, because a clamped event would replay differently on a build with another range. An
event type above 10 is ignored and counted (`unknownEvents`) before `ApplyEvent`'s switch (`Engine.cpp:1050-1098`), which keeps no `default`
so `-Wall` with CI's `-Werror` (`dsp/CMakeLists.txt:75-79`) still fails the build until each new
type is handled (code finding C4); the golden harness's and the plugin's switches
(`dsp/tests/golden/EventScript.cpp:168-184`, `plugin/src/PluginProcessor.cpp:740-772`, `:784-800`)
gain the new types the same way.

### 4.3 MIDI bytes to events: one translator

`brainscape::MidiClockParser` (`dsp/include/brainscape/MidiClock.h`, integer-only, used by
producers, never by `Process`) turns a byte stream into events, so the pedal and the Standalone
give identical events for identical bytes:

| Bytes | Event |
|---|---|
| F8 | ClockTick |
| FA | Transport Start, position 0, AtNextTick |
| FB | Transport Continue, AtNextTick |
| FC | Transport Stop |
| F2 lsb msb | Transport Locate, position 6·(lsb + 128·msb), AtNextTick |
| F9, FD, FE (Active Sensing), FF and all other bytes | none (passed on for other parsing) |

Real-time bytes F8–FF may arrive anywhere, inside another message or SysEx, and leave running
status alone; System Common and SysEx bytes (F0–F7) cancel it (MIDI 1.0, §12). The parser keeps
only F2's two-byte state, which any status byte other than a real-time one aborts. It runs before
libDaisy's parser because that parser mis-handles F2: a System Common message neither clears
`running_status_` nor is `sc_type` cleared by a channel message, so a Song Position after a
Program Change or Channel Pressure is cut after one data byte, and after a Song Select or MTC
quarter frame every Note On and CC loses its second data byte (LD `src/hid/midi_parser.cpp:83-86`,
`:106-109`). The fix belongs upstream too; Brainscape does not depend on it.

**The master's position.** The parser also follows the master's song position, for the
re-assert after an Exact load (§2.5) and nothing else: `positionKnown`, `running` and
`nextTickPosition`, the position of the next F8. FA sets the position to 0 and `running`; FB
sets `running`, keeping the position; F2 sets the position to 6·(lsb + 128·msb); FC clears
`running`; an F8 while running adds one. Until the first FA or F2 the position is unknown, and a
`Reset()` (after a UART restart, §9.3, which may have lost bytes) forgets it with the F2 state.
The parser sees every byte, the ones received while an Exact load mutes the pedal included
(§4.5), so the position it reports at frame 0 of a new timeline is the one the next tick will
carry.

### 4.4 The plugin: host tempo and transport

JUCE reports tempo and position once per host block, "the time at the start of the current audio
callback", each field optional (JUCE `juce_audio_basics/audio_play_head/juce_AudioPlayHead.h:284-296`);
VST3 and AU fill them per block (JUCE `juce_audio_plugin_client/juce_audio_plugin_client_VST3.cpp:2999-3042`,
`juce_audio_plugin_client_AU_1.mm:1249-1325`). The wrapper turns them into events with two
guarded `dsp/` functions, so every build computes the same frames:

- `uint32_t NsPerQuarterFromBpm(double bpm)`: round(6·10¹⁰ / bpm), clamped to the tempo range; 0
  for a non-finite or non-positive bpm (no event).
- `bool HostAnchor(double ppq, uint32_t ns, uint32_t rate, uint32_t* position, uint32_t* offset)`:
  with x = 24·ppq, the first tick at or after the block's start, k = ⌈x⌉ (x within 10⁻⁹ of an
  integer counts as that integer), and its frame offset round((k − x)·ns·rate / (24·10⁹)), at
  most one tick rounded to a frame; the position is k reduced modulo 6,291,456.

**Per host block, while the Tempo source is Host and the host reports a tempo** (§10.1):

1. **Tempo.** At the block's first frame, a Tempo event when ns differs from the last sent by
   1,000 or more (1 µs per quarter, 0.0002 % at 120 BPM): hysteresis, so a host's rounding noise
   never moves P and the output's bits.
2. **Transport start** (the edge `CheckTransportStart` already detects,
   `PluginProcessor.cpp:529-542`), after the optional restart and its re-asserts (§10.1): a Tempo
   event, then Transport Start at the block's first frame carrying `HostAnchor`'s position and
   offset (§4.1), so the anchor's boundary lies at the first frame plus the offset however short
   the host block is. Without a ppq, Start at position 0 and offset 0.
3. **While playing,** predict this block's x from the last block's x and frames at the last
   tempo; when the reported x differs by more than half a tick (a loop wrap, a jump), Transport
   Locate at the block's first frame, anchored as in 2.
4. **Transport stop:** Transport Stop at the first non-playing block's first frame.

**Reproducible:** at a constant tempo, with "Restart on transport start" on (§10.1), every event
depends only on the first block's ppq and tempo, so a bounce gives the same event stream, hence
the same bits, at every host block size; §8.4 tests it.

**Not reproducible across host block sizes:** tempo ramps (one Tempo event per block), loop wraps
inside a block (hosts may or may not split there: VST3 `ivstprocesscontext.h:87-93`), hosts that
omit ppq, Reaper's block-length changes around loops, a real-time restart that misses the spare
(`SpareNotReady`, `PluginProcessor.cpp:595-596`), and Live's VST3 offline render, which does not
set non-real-time mode, so its restart takes the spare path (§12). The labels of profile §2.5
already withhold "1:1 render" there. Sample rates other than 48 kHz are exact at their own rate
but outside parity (profile §2.2).

### 4.5 The pedal: stamping and logs

**Today** every pedal event is stamped `NextBoundary()` = (`g_blocks` + 1) × 48
(`firmware/live/main.cpp:130-132`), and a stamp the audio callback has already passed applies at
offset 0 of the next block (`dsp/src/EventQueue.cpp:41`). `BlockEvent` carries no frame
(`Engine.h:183-190`), so the engine knows only where an event applied.

**The rule (D20):** every producer stamps an event `(c + L) × 48`, where c is the block count
when the event was captured and L its fixed latency in blocks: **2** for MIDI bytes (captured in
the UART's DMA callback), the console and the knobs (captured in the control loop), and **5** for
the tap switch, which is sampled in the audio callback and confirmed within three callbacks of
the press (§9.2). The control loop, the queue's single producer (`EventQueue.h:10-14`), pushes
`max(stamp, last pushed stamp)` so stamps never fall (`EventQueue.cpp:14-19`). Two blocks (2 ms)
cover a control-loop iteration of up to about a millisecond between capture and push; a later
push applies a block late, which the loop counts (a push whose stamp is below
`(g_blocks + 1) × 48`). Block-grid stamps never split a pedal block, which matters while the
budget is over (§9.6), and their σ of 0.29 ms (48 frames uniform, *calculated*) costs the
96-tick fit 0.005 % (§3.3). Sub-block stamps from the cycle counter (10,000 cycles per frame) are
an option only if T0b shows the need (§11.8).

- **At most two ClockTicks per block.** The loop pushes a third tick stamped for a block at the
  next block's frame, and so on (`ticksCarried`), which bounds a block's tick handling (§9.6) when
  a computer master's held tick releases several at once. The engine sees only applied frames,
  so parity is unaffected; the fit absorbs the shift like any stamp jitter.
- **Block counts are 64-bit.** `g_blocks` is a 32-bit atomic that wraps after 49.7 days; the loop
  extends every captured c to 64 bits by its modular difference from the last count it saw, so
  the max rule never holds stamps at a pre-wrap value.

**The timeline epoch** (D20). An Exact load mutes, loads for about 49 ms in the control loop,
clears the queue and sets `g_frame`, `g_blocks` and `g_seq` to 0 (`firmware/live/main.cpp:390-403`).
A muted callback returns before `g_blocks` advances (`:94-98`, `:110`), so every byte and tap
captured during the load, or still waiting in a ring, carries the old timeline's count: pushed
after the reset, it would land minutes or hours into the new timeline, `Clear()` having reset the
queue's order check (`EventQueue.cpp:61`), and the max rule would then hold every later tick, tap
and console command behind it, the pedal ignoring all control for as long as it had run before
the load. Draft v1 had exactly that (record §6, E4 and H1). So:

1. Interrupts record `(epoch, c, byte or tap)`; `g_epoch` is an atomic the control loop raises,
   with the audio muted, in the same step that sets `g_blocks` to 0.
2. The drain feeds an older epoch's bytes to the translator, so its F2 state and the master's
   position stay right (§4.3), and drops the events they make, and older taps, counting both
   (`staleDropped`): these are the ticks an Exact load drops anyway.
3. The last pushed stamp resets together with `g_queue.Clear()`.
4. The re-asserts of §2.5 are pushed at frame 0 before `Unmute()`, after the drain of step 2,
   so the position they carry is the next tick's.

**Interrupts hand over, never push.** The UART's DMA callback writes `(epoch, c, byte)` into its
own single-producer ring of at least 256 entries, which holds the roughly 150 bytes a dense
stream delivers during a 49 ms load (3,125 bytes a second); the audio callback writes taps into a
small ring of its own. Overflows are counted. The control loop drains the rings, runs the
translator (§4.3) and pushes in stamp order. An interrupt cannot push: the queue has one producer
and refuses a stamp below the last (code finding C10).

**Logs record the applied frame.** A session log (profile §6.7's capture, and T0b's console log)
writes `(applied frame, seq, type, id, value bits)` in the audio callback as `PopBlock` hands the
events out (`firmware/live/main.cpp:105`), the applied frame being the block's first frame plus
the offset. Replaying the stamps instead would land a late tap a block early.

## 5. Divisions

### 5.1 Subdiv: six rate multipliers

The Microcosm's Subdiv positions, CC#5's values 0–5, are printed 1/4, 1/2, TAP, 2x, 4x and 8x
(Microcosm manual p. 20). Its manual never defines them in words, but four pieces of evidence
agree that they are **multiples of the tapped quarter-note rate, not note values** (D7,
resolving `docs/research/microcosm.md:112`): the looper's speed uses the same labels as
multipliers, with "the 'TAP' interval (1X)" its normal speed and a range "1/4 to 4x" (p. 15);
Tempo mode forces the subdivision "to quarter notes" (p. 5), so TAP is the quarter note and
"1/4" cannot also be one; the pre-release specification calls the control "Tap-Division"; and
users report the 1/4 and 1/2 positions as sparse (§12). The design's names and default assumed
note values (`Mode.h:86`, `PresetState.h:53`, compiler §2.3 "default `1/4`"), which under this
reading is a whole-note grid (code finding C5).

| Code | Label (JSON) | Display | Knob position, CC#5 | Rate | Grid G (ticks) | Grid at 120 BPM |
|---|---|---|---|---|---|---|
| 0 | `tap` (default) | TAP | 2 | ×1 | 24, quarters | 500 ms |
| 1 | `x1/4` | ×1/4 | 0 | ×1/4 | 96, whole notes | 2 s |
| 2 | `x1/2` | ×1/2 | 1 | ×1/2 | 48, halves | 1 s |
| 3 | `x2` | ×2 | 3 | ×2 | 12, eighths | 250 ms |
| 4 | `x4` | ×4 | 4 | ×4 | 6, sixteenths | 125 ms |
| 5 | `x8` | ×8 | 5 | ×8 | 3, thirty-seconds | 62.5 ms |

**Labels say "rate", never a note value.** The Microcosm's printed "1/4" and "1/2" mean a
quarter and a half of the rate, the opposite of the note names "1/4" and "1/2" that §5.2 gives
row 63 beside them in the same JSON, host parameter list and editor, and Subdiv multiplies row 63
(D12); the compiler design's own default `"1/4"` meant a quarter note (C5). So wherever
Brainscape controls the text, every position other than TAP is written as a multiplier (`x1/4`
in JSON, ×1/4 on screens; an ASCII "x1/4" in the plugin's text parser), and an old `"1/4"` in a
document is an E-error, never silently reinterpreted (record §6, P6). Parity with the Microcosm
lives where it matters: CC#5's value order, which does not change. The editor shows the product
beside a synced field, e.g. row 63 "1/4 · Subdiv ×1/2 → 1/2 · 1,000 ms".

**Codes keep 0 neutral.** Code 0 is TAP, the default and the ×1 setting, so every stored byte
written so far (all 0) keeps a neutral meaning, no package re-hashes, and the frozen fixtures keep
their verdicts; the enum becomes `{Tap, QuarterRate, HalfRate, Double, Quadruple, Octuple}`.
The knob and MIDI CC#5 use the Microcosm's order; the table maps them. Every grid is a whole
number of ticks, so the phasor of §2.2 places every grid point exactly.

**Scope (D12).** Subdiv is "the musical subdivision of the effect" (Microcosm p. 5): it scales
every tempo-derived duration of the mode, the CLOCK grid (G = s) and the synced fields (§2.3's s),
so in an echoic mode with a synced post delay the Subdiv knob halves or doubles the repeats, as
the Microcosm's Pattern presumably does (its delay "uses the delay times to create rhythmic
patterns", §12).

### 5.2 Note values for synced fields

`post.delay.sync` (row 63), `layerN.position.base_sync` and, in W3, a modulator's `sync` take 0
(off) or a code 1–16 (`Mode.h:118`, `Params.h:248`): sixteen note values ordered by duration.
Each is a whole number of ticks, a superset of Chase Bliss Big Time's thirteen (§12):

| Code | Name | Ticks | | Code | Name | Ticks |
|---|---|---|---|---|---|---|
| 1 | `1/32` | 3 | | 9 | `1/4` | 24 |
| 2 | `1/16t` | 4 | | 10 | `1/2t` | 32 |
| 3 | `1/16` | 6 | | 11 | `1/4d` | 36 |
| 4 | `1/8t` | 8 | | 12 | `1/2` | 48 |
| 5 | `1/16d` | 9 | | 13 | `1/1t` | 64 |
| 6 | `1/8` | 12 | | 14 | `1/2d` | 72 |
| 7 | `1/4t` | 16 | | 15 | `1/1` | 96 |
| 8 | `1/8d` | 18 | | 16 | `2/1` | 192 |

`t` is a triplet (two thirds), `d` dotted (three halves), as compiler §2.2's `"1/8d"` writes it.

**Sweeps.** A macro sweeping the code lengthens monotonically wherever nothing folds (§5.3): on
the post delay at TAP that is every code from 120 BPM up, the default tempo included. Below, a
folded code can play shorter than the code before it: at 50 BPM `1/2d` plays 172,800 frames and
`1/1`, folded, 115,200. Draft v1 claimed monotone sweeps outright while its 2 s line folded `1/1`
at the default tempo itself (record §6, P5); lint L14 now reports a synced field whose reachable
codes fold at the preset's stored tempo and Subdiv.

### 5.3 Folding

A synced duration that leaves its target's range folds by octaves: halved (k += 1 in §2.3) while
above the maximum, doubled while below the minimum, each fold counted (`folds`):

| Target | Minimum | Maximum |
|---|---|---|
| Post delay, synced | 10 ms (480 frames at 48 kHz, `post.delay.time_ms`'s minimum, `Params.h:203`) | **4.0·R frames inclusive** (192,000 at 48 kHz): a whole note at 60 BPM, a `2/1` at 120 |
| Post delay, `time_ms` | 10 ms, as today | unchanged: round(2.0·R) − 1 = 95,999 (`PostChain.cpp:148-150`, `:240-248`) |
| Grain base delay | 1 ms (48 frames, `base_ms`'s minimum) | 5,000 ms (240,000 frames, `base_ms`'s maximum, `Params.h:186`) |

**The post-delay line grows to round(4.0·R) + 2 frames** in the synced-times revision
(`kPostDelayMaxSeconds`, `PostChain.cpp:52`, `:177`), the two extra frames for the cubic read's
neighbours (`dsp/src/detail/PostChain.h:177-180`). With draft v1's 2 s line, `1/1` and `2/1`
folded to a half note at exactly 120 BPM, the default, and Subdiv's ×1/4 and ×1/2 sounded the same
on a quarter-synced echo (record §6, P5). Unsynced output does not change: `time_ms` keeps its
10–2,000 ms range and its clamp at round(2.0·R) − 1, and a read `back` frames behind the write
head returns the same frame in a longer line (`ReadBack`, `PostChain.h:98-102`). Cost:
+768,000 bytes of SDRAM at 48 kHz (2 s × 2 channels × 4 bytes), so the Bulk arena grows from 17
to 18 MiB (`firmware/platform/Placement.h:26`; 280,576 bytes spare today), and an Exact load's
clear about 2.1 ms longer, at the measured 44.97 ms per 16 MiB (`rev7-silicon-record.md` §3.6,
*calculated*).

A clamp would be wrong: a `2/1` at 119 BPM is 193,613 frames, 1,613 more than the longest, and
would play its downbeat 34 ms early; folded it plays an exact whole note, 96,807 (*calculated*).
At the other end, a 1/32 under ×8 at 300 BPM is 150 frames and plays as 600 (12.5 ms). A fold
change caused by a tempo change crosses an octave of delay, so it crossfades rather than glides
(§7.3).

**Displays show what plays.** The fold and Subdiv never change a field's stored code, but the
editor and the plugin's BPM panel show the effective value wherever either changes it ("1/1 →
1/2 · 1,000 ms"), computed by a `dsp/` function of the code, the committed tempo, the effective
Subdiv and R. The host's value text for row 63 stays the code's name: a host caches value text
as a function of the value alone, and a tempo-dependent string would change under it.

### 5.4 Names, JSON and display

- **Structure fields** (`base_sync`, modulator `sync`) are JSON strings, `"off"` or a name of
  §5.2 (`Schema.cpp:660-675`, `:1904-1907` today accept and write only `"off"` and a placeholder).
- **Row 63** stays a numeric leaf, 0–16 (`Schema.cpp:412`), because macros and expression target
  leaves by number (compiler §3.2) and read counting leaves as `RoundHalfAwayI32`
  (compiler §3.7); its display is the name.
- **`performance.subdiv`** is a string of §5.1's labels; `scheduler.subdiv` is withdrawn (D14).
- **Display** (`dsp/src/ParamDisplay.cpp:455-464` prints "Div N" today): row 63 shows "Off" or the
  name with an upper-case suffix ("1/8D", "1/16T"), which the plugin's text parser reads back in
  either case; rows 83–86 show their labels (§10.4).

## 6. What follows tempo

### 6.1 The post delay: row 63

Row 63 becomes a `Leaf` in the synced-times revision (§11.3). When it is nonzero, the post
delay's target is §2.3's duration of its note value, folded (§5.3), in exact integer frames,
instead of `post.delay.time_ms` (`Engine.cpp:803`). `time_ms` keeps its stored value and plays
again when the code returns to 0. A committed-tempo change marks `kDomainPost`, and the head
follows the new target by the change's class (§7.1): a small change glides, a deadband commit
slews over about a second (§7.2), and a jump crossfades (§7.3), as does a discrete change (code,
Subdiv, time mode, fold). The post delay is already the engine's tempo-exact part: its taps
measured exact, 251 and 626 ms (`docs/design/reviews/mode-compiler-record.md:156-162`).

### 6.2 The grain base delay: `base_sync` and Q11

When a layer's `base_sync` is not `off`, its base delay is §2.3's duration, folded (§5.3), as an
exact integer `double` in `gp_.baseDelayFrames` (`Engine.cpp:823`), marking `kDomainGranular` on
every committed-tempo or Subdiv change. Grains resolve their position at birth, so a change
reaches only grains born after it and needs neither glide nor crossfade (compiler Q10's base-delay
glide stays the listening pass's question).

**Q11, tempo-exact grain feedback (D6).** Feedback re-enters the ring through a 512-frame FIFO
(`Engine.h:16-22`), so grain-feedback repeats run 10.67 ms late per pass: 760.7, 1146.3, 1532.0
and 1917.7 ms against a 750 ms grid (`mode-compiler-record.md:156-162`). The FIFO cannot shrink:
Pass 1 writes a whole block before grains render, and blocks reach 512 frames
(`Engine.cpp:571`, `:1164-1167`, `:1213-1224`; code finding C11). The options:

| Option | Result | Verdict |
|---|---|---|
| Base = division − 512 frames | exact spacing, every echo 10.67 ms early, the first tap included | rejected: the first tap is the one listeners hear most |
| A variable-length feedback path | exact | rejected for the tempo core: sound-changing on every feedback preset, with a cost not yet estimated |
| **Rhythmic repeats on the post delay** (Engram's way) | exact taps, measured | **chosen**: `base_sync` places the first grain tap exactly on the division; grain feedback keeps its one-FIFO pass, the smear and lateness echoic modes already have; tempo-exact repeats use `post.delay.sync` with `post.delay.fb` |

Lint **L12** (after cpu-budget.md's L10 and L11, its §7.4) notes a layer with `base_sync` whose
`feedback.amount` is above 0 at the stored position or any macro corner: "grain-feedback repeats
fall 10.67 ms later per pass than the grid; tempo-exact repeats belong on the post delay". A
warning, never an error.

### 6.3 CLOCK births

**The source.** A mode listing `clock` (`Mode.h:78`) births on the grid: every position divisible
by G, the effective Subdiv's ticks (§5.1), counted from the transport's position 0 (`FloorMod`).
`periodic` and `clock` may both be listed: a cloud with clocked hits.

**Firing.** Each grid position fires once, at its frame F(k) (§2.2), or at the frame of the
event that made it due. For each render span [s, e), `TempoCore::GridFrames`:

1. **catches up:** with g the largest grid position ≤ `tick` (`FloorDiv`), if g > `lastFired`,
   g fires at s and `lastFired` := g. In steady running this is exactly the position whose
   boundary lies inside the frame before s (acc < K, F(g) = s); after a placement, a tap or a
   tempo rescale it is the one position the jump made due, the largest only (a forward jump
   skips the rest). *(2026-10-10, §11.12 note 25:)* A change of the grid G under a still phasor
   (a Subdivision event, a load's stored Subdiv or time mode) is no jump: the new grid starts at
   its next position, its positions whose first frames lie before the change counting as fired,
   and only one due at the change's frame itself (F(g) = s) fires there;
2. then fires each grid position k > max(`tick`, `lastFired`) with F(k) < e at F(k), and sets
   `lastFired` to the last.

At most `kMaxClockPerSpan` = 4 per span (the shortest grid, 3 ticks at 300 BPM, is 200 frames
at 8 kHz, so a 512-frame span holds three, plus the catch-up). A frame is in exactly one span
whichever way a block is split, and a position due at it fires there either by the catch-up
(when the span starts at it) or by step 2 (when the span runs through it), so the hits are
block-split invariant. A simulation of this rule fires identical hits at block sizes 1, 48,
441, 512 and random, with tempo changes and taps at random frames, at 20, 97, 137.5, 140 and 300
BPM and grids of 3, 24 and 96 ticks, and at constant tempo every hit lands on ⌈k·P/K⌉ (record
§2.6). Draft v1 lacked step 1 (record §6, E1).

**Delivery.** The hits reach `GranularCore` like onsets, as offsets in `TriggerEvents`
(`dsp/src/detail/Granular.h:125-132`), bounded by a `static_assert` as the onsets are
(`Engine.cpp:178-180`). At its frame each fires through `FireTrigger` with a new ordinal,
`kOrdinalClock` = 3 (`Granular.h:119-121`; ordinals 0–7 exist, `GrainMath.h:60-86`), so:

- **intermittency** skips the whole hit, its burst included (purpose 9, ordinal 3,
  `Granular.cpp:409-421`);
- **burst** adds `burst.count − 1` grains after it at `burst.spacing_ms`, as any trigger does;
- **jitter** delays the hit, never advances it and never accumulates: at the grid frame a draw
  u (purpose `Interval`, ordinal 3) gives a delay of
  `(uint32_t)((jitter * u) * (0.5f * (float)gridFrames))` frames, evaluated in binary32 in that
  order inside the engine's guard, where `gridFrames = MulDivRoundU64(P, G, K)` is an integer
  below 2²⁴ (96 ticks at 20 BPM and 384 kHz is 4,608,000 frames), so its conversion is exact. At
  jitter 1 a hit lands up to half a grid period late; the pending hit carries across spans in
  `GranularCore`;
- **allocation (D13)** is the trigger class's: the hit steals the oldest voice at `voice_count`
  (`FireExternal`, `Granular.cpp:375-407`), and `overlap` does not cap it. Engine §4's "under a
  CLOCK source … `overlap` then acts as a don't-fire ceiling" is withdrawn: a refused grid hit is
  a missing beat, and the rhythmic families the source exists for (engine §5's Seq, Arp, Pattern,
  Warp, Mosaic) are hits;
- **under the cost governor** (cpu-budget.md §5, its own sound revision), a hit that fails admission
  waits as triggers do, blocking scheduler births while it waits, but **at most
  `kClockLateFrames` = 48 frames** past its (jittered) frame: a hit not admitted by then is
  dropped (`clockDropped`), where other triggers wait for up to eight pending. Deferred hits are
  counted with their lateness (`clockDeferred`, `clockDeferredFrames`). So a CLOCK hit is never
  more than 1 ms late, a `periodic` + `clock` cloud loses at most 48 frames of births per hit,
  and the factory budget is no deferral at all (§7.4); draft v1 left all of this unstated
  (record §6, H5);
- **draw keys:** the grid hit's grain draws with ordinal 3 in the key extension (compiler §7.5,
  R8), so a hit sharing a frame with a periodic, onset or manual birth never stacks an identical
  grain (today births at one frame share every draw, `Granular.cpp:102`; code finding C3); its
  burst's later grains draw as every burst grain does;
- **one hit per frame:** a hit due where a clock hit already fired (a catch-up and the next grid
  point at one frame) waits for the next frame.

**The grid is never moved by births.** It is the phasor's, which births never touch, so no refusal
or deferral, today's or the cost governor's, can make it drift; contrast the periodic countdown,
a `float` that a refused birth shifts (`Granular.cpp:544-597`). `TempoInfo` reports the grid's
own frame (`lastGridFrame`) apart from the birth's (`lastClockBirth`), so a timing metric never
mistakes jitter or a deferral for a grid error (§8.5).

**Freeze** pins positions as for every source; hits continue. **Steps** (later W2) attach here:
the step index is ⌊position / G⌋ modulo the step count, so patterns align to the transport.

### 6.4 The Time knob, and Q7

| Time mode | The Time knob sends | Subdiv | `macro.time` |
|---|---|---|---|
| Free (default) | `MacroMove(macro.time)`, as today | the stored one; a Subdivision event can still change it | the knob |
| Subdiv | a Subdivision event (field 0) when its zone changes, with hysteresis at zone edges | the knob's | expression, MIDI CC, hosts |
| Tempo | Tempo events, exponential from 20 BPM (fully counter-clockwise) to 300 BPM: ns = round(3·10⁹ · 2^(−m·log₂15)) through DetMath's `Exp2D`, exported as `TempoNsFromKnob(m)` | forced to TAP | expression, MIDI CC, hosts |

**Q7 (answered here, D5, which the owner confirmed on 2026-10-08):** in Subdiv and Tempo modes
the Time knob never reaches `macro.time`; expression, MIDI and hosts still do, in every mode
(compiler §3.1's lean).

**The Tempo knob (D15)** is exponential, because equal turns then make equal tempo ratios, the
"smooth acceleration and deceleration" of the Microcosm's Tempo mode (§12). Under external clock
its events are ignored (§3.6) and the time-mode LED shows that the clock owns the tempo; Big
Time's bend-then-snap-back is the precedent for something better, left open (§11.8).

**Soft takeover on every change of meaning** (D15). The Time knob locks, like a macro after a
load (compiler §3.5), until it passes the current value of what it now sends: after a load, a
tap, a recall or a clock tempo (the committed tempo), and **on every time-mode change** (the
committed tempo in Tempo mode, the current Subdiv zone in Subdiv mode, `macro.time` in Free).
Draft v1 took over only after a tempo change, so entering Tempo mode with the knob at 80 % jumped
the tempo to about 175 BPM on the first touch (record §6, P9).

**The time-mode gesture (D18)** is the Microcosm's two-state toggle, **Subdiv ↔ Tempo**, in
presets that use tempo (§6.6); from Free it enters Subdiv. Free, macro time in milliseconds and
without a Microcosm counterpart, is the preset's authored state, set from the editor, by a load,
or by a shift gesture the control-surface design chooses, so a stage gesture never passes through
a third state: draft v1's cycle Free → Subdiv → Tempo → Free crossed Tempo, crossfading the delay
twice, on the way back to Free (record §6, P9). The gesture sends a Subdivision event (field 1),
so the time mode is logged and replays. In a preset that does not use tempo the gesture does
nothing (§6.6).

### 6.5 Expression, MIDI, macros and hosts

- **Macros and expression** reach row 63 (a leaf: a Time macro on `post.delay.sync` steps
  through note values) and `macro.time`, but neither the tempo nor the Subdiv, which are not
  leaves (D19). An expression assignment to tempo would need a `Performance` target kind, left
  for later.
- **A target overridden by sync** does nothing: `post.delay.time_ms` while row 63 is nonzero, a
  layer's `base_ms` while its `base_sync` is not `off`. Lint **L13** reports such a target when
  the override holds at every position of every macro that could lift it.
- **MIDI CC** belongs to the firmware's MIDI design; it should adopt the Microcosm's CC#5 (Subdiv
  position 0–5), CC#10 (Time, following the knob's mode) and CC#93 (tap, any value) (§12).
- **Hosts** see Subdiv and time mode as automatable `Performance` rows (83, 84, §10.4); tap is a
  momentary event, not a parameter (companion §5.7).

### 6.6 Presets that do not use tempo (D21)

No first-set mode lists `clock`, sets a `base_sync` or a nonzero row 63 (§11.2), so once the
tempo core ships the tap footswitch changes a tempo that nothing in any factory preset reads; on
the Microcosm, every tap is audible (record §6, P7). Two measures:

- **`UsesTempo(const PresetState&)`**, a pure `dsp/` function beside `EvalMacro`
  (`dsp/include/brainscape/ModeEval.h`): true when the mode lists `clock`, a layer's `base_sync`
  is not `off`, or row 63 is nonzero at its stored value or at an end of any macro target or
  expression assignment on it. It is computed from the package, never stored, so no byte and no
  hash changes; `bspc` reports it. Producers read it at each load: in a preset without it the
  pedal ignores the time-mode gesture and shows the tap LED steady instead of the beat, and the
  editor dims rows 83–84 and marks the BPM panel unused. **Tap still sets the tempo**, which is
  global under Keep and plays in the next preset that uses it.
- **Factory presets that follow tempo at launch.** T5 gives the first set's post-delay echo
  modes, Engram and Callback (`firmware/factory/engram.json`, `callback.json`), a synced post
  delay, by default or on a macro corner, chosen by listening, so tap is audible on day one. If
  the listening rejects it, D21 records that tap is inert in the first set until the rhythmic
  second set.

## 7. Smoothing

### 7.1 The committed tempo: commits and classes

Under Internal, Pc = P at every change: taps, the knob, host tempo, recalls and re-asserts are
explicit intents. Under ClockFree and ClockRunning, Pc moves to P_fit only by these commits:

1. **Acquisition:** Pc := P_fit at once (`commits`) whenever a clock state is entered with
   N ≥ 24 (ClockFree's acquisition, a resume, a Start or Continue applying on a warm window), and
   whenever the window under a clock state reaches N = 24 (after a Start on a fresh window, or a
   re-acquisition).
2. **The early commit after a Start:** when an armed Start or Continue applied with an empty
   window (§3.4), at N = 12, Pc := P_fit if |P_fit − Pc| > Pc >> 4 (6.25 %) (`earlyCommits`).
   A 12-tick fit's period σ is 2σₓ: 1.4 % for a computer clock and 0.12 % for a hardware one
   (*calculated*), so the threshold is 4.6σ of the worse. A song that starts at a clearly new
   tempo has its committed tempo half a beat after the downbeat, before that beat's first
   quarter-note echo is read; a tempo within 6.25 % waits for rule 3. After an Exact load the
   re-asserted tempo (§2.5) is already right and the early commit stays out of the way. Draft
   v1 waited for N = 24, after a stale window had first followed the previous song's slope
   (record §6, P4).
3. **The deadband,** from N ≥ 24:
   1. |P_fit − Pc| > Pc >> 9 (0.195 %, about 3.3 σ of the computer-clock fit, never reached by
      a hardware clock), at once; or
   2. |P_fit − Pc| > Pc >> 12 (0.024 %) on every fitted tick for 192 consecutive fitted ticks
      (8 beats). The run counts fitted ticks only; an outlier, or a fitted tick inside the band,
      resets it.

Rule 3.2 makes Pc converge on a steady clock, so the post delay's repeats do not drift against
the grid by the fit's residual. Synced durations change only with Pc, so a jittery clock never
moves the delay, and TapGlide's `Retarget` ignores an unchanged integer target
(`dsp/src/detail/PostChain.h:139-145`).

**Every change of Pc has a class,** computed from engine state alone, so it replays:

| Class | Test | Examples | The post delay |
|---|---|---|---|
| **Jump** | \|Pc′ − Pc\| > Pc >> 5 (3.1 %) | a tap starting a new chain, a recall, an acquisition or early commit, a host tempo jump at a section change, a re-assert | crossfades (§7.3), or glides when `global.tempo_glide` is On |
| **Drift** | a rule-3 commit within Pc >> 5 | clock drift, a slow ramp on the master | slews over about a second (§7.2) |
| **Step** | any other change within Pc >> 5 | a Tempo-knob step, a host ramp's per-block step, a tap refining its chain | glides, τ = 50 ms (§7.2) |

Pc >> 5 is above one block of any host ramp and one step of a Tempo-knob sweep (a full turn in
0.2 s is about 1.4 % per 1 ms step, *calculated*), so sweeps stay smooth accelerandos and only
real jumps crossfade. Draft v1 glided every change at the glide's 0.5 frame-per-frame cap, so a
tap from 120 to 90 BPM held a quarter-synced echo an octave flat for a third of a second and
feedback recirculated it (record §6, P1).

**`global.tempo_glide`** (row 86, a `Global` device setting, §10.4): **Off** (0, the default by
D22, which also has it checked by ear on S12, §11.8) or **On** (1): jumps glide as in draft v1,
the tape swoop Meris' TAP GLIDE and Thermae's GLIDE offer as a choice (§12). Drifts slew either
way.

### 7.2 Glide and slew

**A Step glides** as `time_ms` does today: two cascaded 50 ms one-poles with the speed capped at
0.5 frames per frame, a tape-like bend (`PostChain.h:113-166`, `PostChain.cpp:53-55`; profile
§5.6). A Tempo-mode sweep, sending a Tempo event per pot step, is thus a smooth accelerando with
the repeats bending pitch, as the Microcosm "smoothly matches" a tapped tempo (§12). The pair is
critically damped with τ = 50 ms (2,400 frames), so a change of Δ frames moves the head at most
Δ/(e·τ) frames per frame, at t = τ: a tap refining 120 to 121 BPM moves a 500 ms delay by 198
frames and bends the repeats by up to 3 % (half a semitone) for about a tenth of a second
(*calculated*).

**A Drift slews.** `TapGlide` gains a second coefficient pair with τ = 1 s (48,000 frames); a
retarget selects its pair by its class, and a later retarget replaces both. A 0.4 % commit on a
500 ms delay (96 frames) then bends the repeats by about 0.07 % (1.2 cents), where the 50 ms
glide bent them 1.5 % (25 cents), and the echoes sit at most 2 ms off the grid for about a
second (*calculated*). Draft v1 glided drift commits, an audible wobble on long trails
recurring through a set (record §6, P8).

A silent stage jumps instead (`PostChain.cpp:323-334`). Whether these feel right is a listening
question (§11.8).

### 7.3 Crossfade for jumps

A glide over an octave of delay swoops: a 2 s to 10 ms throw at feedback 0.9 bends the repeats
nearly three octaves over 4 s (profile §5.6, *measured*). So a change of the synced post-delay
target crossfades instead when it is a **Jump** of Pc (§7.1) or **discrete**: a change of row
63's code, of the effective Subdiv (a Subdivision event, entering or leaving Tempo mode, a load),
of the fold count, or sync switching on or off.

- `RebuildPostParams` raises a jump serial in `PostParams` when the target changes for one of
  these reasons; a Step or Drift does not.
- `PostChain` (`dsp/src/PostChain.cpp`) keeps a **second head**: on a jump it copies the current
  `TapGlide` as the outgoing head, which keeps reading (and gliding toward its old target), primes
  the incoming head on the new target, and mixes them over `kXfadeFrames` = 1,024 frames (21.3 ms at
  48 kHz) with gains (1024 − n)/1024 and n/1024, exact multiples of 2⁻¹⁰ like FastCut's 2⁻⁷
  (compiler §7.3). The delay line's feedback write takes the mixed read.
- **One fade at a time:** a jump during a fade waits; the latest pending target starts the next
  fade **at the frame the previous one ends, inside the per-sample loop**. The Delay stage today
  decides its target at a span's start (`PostChain.cpp:323-334`), and a chained fade started
  there would begin at the next span, wherever the block happened to split (record §6, E11). So
  a Subdiv knob turned through all six zones gives a chain of fades, never a dropped head or a
  step, at the same frames for every split; a unit test runs it at block size 1.
- A silent stage, which jumps today, jumps here too; a Step or Drift during a fade retargets the
  incoming head.

**Cost:** one more Catmull-Rom read per sample during a fade, about 60–80 cycles (*estimated* from
the post delay's 319 cycles per sample with one head, `rev7-silicon-record.md` §3.5), 3–4k cycles
per 48-frame block, 0.6–0.8 % of the budget, for 21 ms per jump. The cost governor charges the
delay stage a constant measured with one head (cpu-budget.md §5.1, P(f) = Σ K_s), so T2 measures
the stage with a fade in progress, both heads moving in stereo, and raises its constant in the same
revision (§9.6).

### 7.4 The artefact budget

| Artefact | Budget | Checked by |
|---|---|---|
| A hardware clock (σₓ ≤ 0.3 ms) at constant tempo | 0 commits after lock over 10 minutes | the golden `clock_midi_hw` counters (§8.3) |
| A computer clock (E-RM's distribution) at constant tempo | at most one commit per minute after lock, each ≤ 0.4 % | `clock_midi_computer` counters |
| CLOCK grid timing, internal or host tempo | exactly F(k): 0 frames from the ideal grid (`lastGridFrame`) | §8.5's beat-lock metric |
| CLOCK grid timing after lock, hardware / computer clock | rms ≤ 0.3 ms, max ≤ 1 ms / rms ≤ 1 ms, max ≤ 3 ms against the generator's true ticks (*estimated*, set at T0b) | §8.5 |
| CLOCK hits deferred or dropped by the governor | 0 for every factory clock mode at stored positions and over S12; never later than 48 frames (§6.3) | the governor's shadow ledger, `clockDeferred` and `clockDropped` |
| A Start more than 6.25 % from the last tempo after a second or more without clock | Pc committed by the 12th tick, so the downbeat's first synced echo plays at the new tempo | the golden `clock_song` case (§8.3) |
| Synced post-delay echoes | at exactly the synced frames | §8.5's impulse render |
| A jump (discrete or Jump class) | no sample step above 4× the static render's largest (the audition's Clicks check) | S12 |
| A Step | the glide's own pitch, s² at read speed s (≤ 2.25×), as for `time_ms` | profile §5.6's measurement; S12 by ear |
| A Drift | the repeats bent at most 0.1 % (§7.2) | S12 by ear, the slew's unit test |

## 8. Determinism and testing

### 8.1 What is guaranteed

The parity contract (profile §2.1) extends unchanged: two conforming builds given the same package,
input, device settings (`global.tempo_recall` and `global.tempo_glide` among them) and stamped
stream, events 6–10 included, write identical bits. The stream is the source of truth, and the
producers' re-asserts after an Exact load (§2.5) are part of it. A pedal following a MIDI clock
and a plugin following a DAW produce different streams, which match only when one replays the
other's log, as companion §1 already says in excluding host tempo sync "unless turned into the
same event stream". Not guaranteed: live clock timing on either side, the host cases of §4.4,
Spillover loads (profile §2.4).

### 8.2 Unit tests (`dsp/tests/test_tempo.cpp`)

Each rule is tested against a frame-by-frame reference in exact rational arithmetic, and each
case named for a draft-v1 finding reproduces that finding's failure on draft v1's rule.

- **Integer helpers:** `MulDivRoundU64` and the signed form against a reference on edge operands
  (0, 1, 2⁶⁴ − 1, products straddling 2⁶⁴ and 2¹²⁸) and 10⁷ random ones, ties included;
  `FloorDiv`, `CeilDiv`, `FloorMod` on every sign combination.
- **Phasor:** span-split invariance of `Advance` over random splits of 10⁶ frames at random P;
  F(k) against the closed form at every beat of a **ten-hour** run at 140 BPM and 48 kHz (no drift,
  bit for bit); tempo changes keep the tick fraction; the offset placement of §2.2 up to two
  ticks.
- **Grid firing (E1):** `GridFrames`' hits at block size 1 equal those at 48, 441, 512 and random
  sizes, with tempo changes and taps at random frames, at 137.5, 140 and 97 BPM (fractional frames
  per tick) and every Subdiv; at constant tempo every hit on ⌈k·P/K⌉; a rescale that makes a
  position due fires it at the event's frame.
- **Tap:** the table of §3.2 case by case, at both thresholds' edges; the beat computed in the
  old units at a tap that changes the tempo by 40 % near a half beat (E11); the downbeat of a new
  chain (P10); a phase tap under ClockFree moves labels only.
- **Follower:** seeded tick generators (§8.3) at 20, 120 and 300 BPM, hardware and computer
  jitter: P within the §3.3 accuracy after lock; a tempo step re-acquires within 6 outliers; a
  dropout of 200 ms infers its ticks; a 38 ms burst neither re-labels nor re-acquires; ticks
  applied at one frame and then a slow-tempo gap infer nothing and divide by nothing (E5); the
  incremental sums equal direct sums over 10⁶ ticks with dropouts and outliers (H4); the nearest
  tick's tie goes up; rule 3.2's run reset by an outlier.
- **Gaps (E2, E6, P3, P4):** the engine equals a reference that applies each gap at its deadline
  frame and splits there, on streams with ticks, a second of silence and then each event type, a
  Spillover load under recall Preset among them; the snapshot shows Internal after a gap with no
  event; a window cleared under Internal after a clock-silent stop; a resume into ClockRunning
  after a 1.5 s gap mid-song; ticks resumed after twelve hours at 384 kHz overflow nothing.
- **Restart (E3):** `Restart`, and `Init` followed by an Exact load of the active preset, leave
  identical tempo state after taps, a fitted clock, a Stop and live Subdivision events.
- **Transport:** Start and Continue at the next tick, Song Position while stopped and while
  running, Stop's continue position, host-style events ignored under clock, FA then FC then F8
  starting nothing (E10), a host Start with an offset firing its first hit at the anchor and none
  before (E9); the early commit at N = 12 after a Start with an empty window (P4).
- **Classes:** each example of §7.1's table lands in its class; `tempo_glide` On glides jumps.
- **Payloads:** every invalid form of §4.2 ignored and counted; an unknown type counted.
- **`MidiClockParser`:** real-time bytes inside a Note On and inside SysEx, F2 after a Program
  Change and after Channel Pressure (libDaisy's failure cases), running status kept across F8;
  the master's position through FA, F8, FC, F2, FB and `Reset()`.
- **Durations and folds:** every code × every Subdiv at 20, 30, 60, 119, 120 and 300 BPM against a
  128-bit reference; the fold edges of §5.3; `UsesTempo` on every factory package and on presets
  that reach row 63 only through a macro or an expression assignment.
- **Post chain:** a chain of fades at block size 1 equal to block size 512 (E11); unsynced
  renders unchanged by the longer line.

### 8.3 The golden corpus

- **Script verbs** (`dsp/tests/golden/EventScript.h:58-115`): `Tap(frame)`, `Tempo(frame, ns)`,
  `Tick(frame)`, `Transport(frame, kind, position, atNextTick, offset)`,
  `Subdivision(frame, field, code)`.
- **Generators,** integer-only and seeded through `TestSignal.h`'s hash: `ClockTicks(script,
  start, ns, count, model, seed)` with models **none**, **hardware** (uniform over a 48-frame
  block: the pedal's grid) and **computer** (a sum of four uniform draws scaled to σ = 163 frames,
  3.4 ms, plus every 500th tick held 1,843 frames, 38.4 ms, the ones after it bunching behind it
  onto one frame, so frames never fall), with dropouts, gaps and tempo steps; and
  `TapSeries(start, intervals, spread)`.
- **Tempos with fractional frames per tick.** Every clock preset runs at 140 BPM (857.14 frames
  per tick) or 137.5 BPM, never at the stored default of 120 BPM, whose 1,000 frames per tick hid
  draft v1's firing bug at every block size (record §6, E1).
- **Presets**, corpus version 13 with the tempo core and 14 with synced times, on a new 30 s
  vector `plucks_clock_30s`: `clock_internal` (Subdiv stepped through all six, intermittency,
  burst, jitter), `clock_tap` (tap chains: set, refine, pause, halve, bounce, a downbeat),
  `clock_midi_hw` (Start, Stop, Continue, Song Position, ticks while stopped, FA then FC then F8),
  `clock_midi_computer` (bursts, a 200 ms dropout, a tempo step, a loss and a tap accepted after
  it), `clock_song` (Stop, five seconds without clock, Start at another tempo; Stop, Start, a
  1.5 s gap mid-song, ticks resuming), `clock_loads` (Spillover under Keep and Preset, a Spillover
  under Preset after a loss, an Exact load mid-clock followed by the re-asserts of §2.5 at frame
  0), `tempo_jump` (taps from 120 to 90 BPM, a recall, a host-style tempo jump), then `sync_post`
  (row 63 swept by a macro, Subdiv jumps crossfading, a fade chain), `sync_base` (`base_sync` with
  feedback), and `sync_fold` (a tempo sweep across the 4 s fold). Events both on and off the
  48-frame grid.
- **Counters** (`dsp/tests/golden/Corpus.h:55-98`, 28 today): `Taps`, `TapsIgnored`,
  `TapPhases`, `ClockTicks`, `TickOutliers`, `Reacquires`, `DropoutTicks`, `ClockGaps`,
  `ClockLosses`, `ClockResumes`, `TransportEvents`, `SubdivEvents`, `ClockBirths`, `Commits`,
  `EarlyCommits`, `Jumps`, `Slews`, `Crossfades`, `Folds`, `InvalidEvents`, from
  `Engine::TempoCounts()`; each preset's minimums prove its features ran.
- **Ablations,** each of which must change its presets' output and not before the feature acts:
  `clock` (the source removed), `tempoEvents` (events 6–10 dropped), `subdiv` (TAP throughout),
  `sync` (row 63 at 0, `base_sync` off), `crossfade` (jumps glide), `slew` (drifts glide).
- **Perturbations,** unchanged, must reproduce every hash: block sizes, the pattern
  {48, 1, 127, 32}, random sizes 1–512, fresh engines and the hostile FP environment
  (`golden_main.cpp`, profile §6.4). **Split delivery** (`EventScript.h:140-145`) has no
  unstamped call for events 6–10 (§2.6): the harness splits at the event's frame and hands the
  event to the part that starts there as its only event, at offset 0, which still moves every
  block boundary. **A restart mid-render** (`Corpus.h:140-146`) needs nothing re-sent: `Restart`
  sets the tempo state to the active preset's stored performance state (§2.5), which the tail's
  Exact load of `StateAt`'s package (`Corpus.cpp:723-742`) applies too. A script that wants the
  running tempo across its restart sends a Tempo event itself, as a producer would. Draft v1
  re-sent "the tempo `StateAt` records", which `StateAt` cannot know (record §6, E3).
- **Long render:** nightly on the host legs, a 10-minute render at 140 BPM with per-second hashes,
  whose clock-birth count and last birth frame must equal the closed form.

### 8.4 Plugin tests

The test playhead (`plugin/tests/plugin_tests.cpp:224-233`) gains a tempo and a ppq. With "Restart
on transport start" on, a bounce at 137.5 BPM starting at ppq 3.37 gives one output hash at host
blocks of 37, 64, 441, 512, 1,024 and 4,096 frames, with no hit before the anchor; the same with
row 83 changed before play (the restart re-asserts it, §10.1; record §6, E7); a stop and restart at
another ppq still aligns its first hit to the anchor; under the Internal source a tapped tempo
survives a session save and restore and a `prepareToPlay`; the Standalone's MIDI path turns a
byte stream with interleaved real-time bytes into the events of §4.3 at their sample positions.

### 8.5 Audition

A new script **S12** next to S0–S11 (`tools/audition/README.md:78-86`): taps setting 120 then
90 BPM (a jump); a recall to another tempo; a hardware-model MIDI clock at 128 BPM with Start at
2 s, the Subdiv stepped every 2 s through all six positions, Stop at 14 s, five seconds of
silence and a Start at 100 BPM; a Tempo-mode sweep from 80 to 160 BPM over 8 s. Each clock mode
is rendered on its class input, with `tempo_glide` Off and On, so the owner can choose its
default by ear (§11.8).

**Beat-lock metric:** the audition tool reads `TempoInfo::lastGridFrame` after every 48-frame
block (grid points are at least 200 frames apart, so none is missed) and reports each grid
point's offset from the ideal grid, which it computes from the script's own ticks; the tempo it
rendered is known exactly, so an internal or host tempo must give 0 frames. Jitter and the
governor's deferrals are reported apart, from `lastClockBirth` and the counters. **Echo check:**
an impulse render of each synced post-delay mode must put its echoes on the synced frames
exactly. The pre-screen gains a **Lock** row (§7.4's budgets) and runs its Clicks check over
S12.

## 9. Firmware and hardware

### 9.1 MIDI in and out on the Rev7 breadboard

**Circuit.** The topology is Electrosmith's reference, transcribed from the Seed3 datasheet,
whose optocoupler is not identified (`docs/research/pedal-control-surface-and-io-hardware.md:342-361`),
with the H11L1M that Funbox v3.2, "VERIFIED WORKING" on a Seed Rev7, uses for MIDI in
(`docs/research/daisy-pedal-platforms.md:301-306`). The resistor values are the Electrosmith
transcription's: the research has only Funbox's parts list, whose 220R and 470R entries it flags
as unverified against Funbox's schematic (`daisy-pedal-platforms.md:349-352`), and draft v1
credited them to Funbox (record §6, H8). T0b's netlist, on a 5-pin DIN socket:

| From | To |
|---|---|
| DIN 4 (the sender's current source) | 220 Ω, then H11L1M pin 1 (LED anode) |
| DIN 5 (the sink) | H11L1M pin 2 (LED cathode) |
| 1N4148 | cathode at pin 1, anode at pin 2: reverse protection across the LED |
| H11L1M pin 6 (VCC) | Seed 3V3_D (header pin 38), with 100 nF from pin 6 to pin 5 |
| H11L1M pin 5 (ground) | Seed DGND (header pin 40) |
| H11L1M pin 4 (open-collector Schmitt output) | 270 Ω to 3V3_D, and Seed D14 (header pin 15), USART1 RX |
| DIN 2 (shield) | unconnected at a receiver |

Confirm the header pins on Electrosmith's pinout card before soldering. **TRS** follows Type A
(ring = DIN 4, tip = DIN 5, sleeve = DIN 2). A Type B cable swaps tip and ring, so the 1N4148
conducts and nothing reaches the LED: silence, not damage, which would look like a firmware
fault. T0b uses DIN; DIN or TRS Type A for the product is the schematic's choice (D10).

**The RC concern.** The transcription puts the 100 nF on the receive line itself, behind the
270 Ω pull-up (`pedal-control-surface-and-io-hardware.md:351-352`). That is τ = 27 µs, and a rising
edge needs about 1.2 τ, 32 µs, to cross the input's high threshold (0.7·VDD): one MIDI bit at
31.25 kbaud (*calculated*), which would corrupt bytes. It is almost certainly the opto's supply
decoupling, misread onto the receive line, and the netlist above puts it there; nothing larger
than stray capacitance goes on the receive net. Check it against the schematic image before
buying parts.

**Pins.** libDaisy's MIDI UART is USART1, receiving on PB7 and transmitting on PB6 (LD
`src/hid/midi.cpp:10-16`), the Seed's D14 and D13 (LD `src/daisy_seed.h:223-224`); the live
wiring uses only the audio pins (`firmware/README.md:337-344`). **MIDI out** (§9.5): USART1 TX
through the reference's two 10 Ω resistors, one to the signal pin and one to 3.3 V
(`pedal-control-surface-and-io-hardware.md:363-364`), on a DIN socket or TRS Type A.

**The receive buffer.** libDaisy listens by DMA and calls back on line idle or a half or full
buffer of 256 bytes (LD `src/hid/midi.cpp:5-17`; code finding C2). On a quiet line a byte is
reported one idle frame after it ends, a constant offset; in a continuous stream (a SysEx dump,
dense notes) bytes wait for the half buffer, up to 41 ms. The firmware passes a 16-byte buffer, so
a byte waits at most 8 bytes, 2.6 ms (*calculated*). It is declared `DMA_BUFFER_MEM_SECTION`
(LD `src/daisy_core.h:25`), the D2 SRAM the MPU leaves uncached (16 of its 32 KiB used today):
libDaisy invalidates the buffer's cache lines rounded out to 32 bytes on every callback (LD
`src/sys/dma.c:94-101`, `src/per/uart.cpp:987-1009`), so a buffer in cached AXI SRAM would
throw away its neighbours' dirty lines (record §6, H2).

### 9.2 The tap footswitch

A momentary SPST switch to ground on a free GPIO with its internal pull-up (research
`pedal-control-surface-and-io-hardware.md:160-161`), **sampled once per audio callback** (every
millisecond) and debounced by the switch's state, not by the time since a press (D20):

- a **press** is a low sample after the pin has read high for at least 24 consecutive callbacks,
  so the contacts' bounce on release, a string of falling edges, can never count;
- it is **confirmed** when the pin reads low in at least two of the next three callbacks, which
  rejects a glitch and tolerates a contact still settling;
- its stamp is `(c + 5) × 48`, c being the callback of the first low sample (§4.5): the
  confirmation's three milliseconds inside a fixed five-millisecond latency, constant from tap to
  tap, so intervals are exact to the block.

Draft v1 stamped the first falling edge on an EXTI line and then ignored edges for 30 ms, so a
press held longer bounced on release into a second tap, and a 250 ms press at 120 BPM set
240 BPM (record §6, H6). Sampling needs no EXTI line and no HAL code, gives the same block-grid
stamps, and keeps working when the footswitches move to the polled CD4021 shift register the
research uses (`:153-159`). The switch is the Microcosm's left footswitch, tap. T0b confirms the
constants on the Rev7 (§11.8).

### 9.3 Interrupts and the control loop

- **Priorities stay as they are (D20).** USART1 and every DMA stream run at priority 0, the audio
  DMA's (LD `src/per/uart.cpp:897`, `src/sys/dma.c:17-34`), so neither pre-empts the other, and a
  byte arriving during the audio callback is stamped after it: at most one block late, which the
  fit absorbs. The firmware moved USB below audio, not above (`firmware/platform/UsbSerial.cpp:70-74`);
  raising the UART above audio would only matter for sub-block stamps (§4.5). The tap switch
  needs no interrupt (§9.2).
- **The control loop** (`firmware/live/main.cpp:615-620`) drains the UART and tap rings each
  iteration, forwards bytes to the soft thru (§9.5), runs the translator (§4.3), pushes with
  §4.5's rule, and honours "receive MIDI clock" (default on) by not feeding the translator when
  it is off.
- **UART restarts.** In DMA listening mode the H7 HAL treats any receive error (framing, noise,
  parity, overrun) as blocking and aborts the DMA (LD
  `Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_uart.c:2296-2321`); libDaisy's error callback
  clears `listener_mode_` and re-initialises the UART without restarting reception (LD
  `src/per/uart.cpp:1105-1109`, `:373-382`). Only `MidiHandler::Listen()` restarts it (LD
  `src/hid/midi.h:194-205`), and the firmware bypasses `MidiHandler` to read raw bytes. A framing
  error is all but certain when the pedal boots, or a cable is plugged in, while a master is
  sending: the receiver starts inside a byte. So every iteration polls the transport's
  `RxActive()`, and when it is false resets the translator (§4.3), flushes, calls `StartRx`
  again, and counts and logs the restart, in `midilog` too. Draft v1 said libDaisy restarts
  reception itself (record §6, H2).
- **An Exact load** runs in this order: read `Tempo()` (which the audio callback copies into a
  variable) and the translator's position; `Mute()`; `LoadPreset(Exact)`; raise `g_epoch`, clear
  the queue and the last pushed stamp, set `g_frame`, `g_blocks` and `g_seq` to 0; drain the
  rings' older-epoch entries through the translator, dropping their events (§4.5); push §2.5's
  re-asserts at frame 0 (the Tempo by the recall rule, then Locate and Continue from the
  translator's position); `Unmute()`.
- **Counters** on the console: UART errors and restarts, late pushes, ring overflows, stale
  entries dropped, ticks carried to a later block.

### 9.4 Console commands

Beside `trigger` (`firmware/live/main.cpp:575-579`), with the same latency rule: `tap`;
`tempo <bpm>` (a Tempo event); `subdiv <x1/4|x1/2|tap|x2|x4|x8>`; `timemode <free|subdiv|tempo>`;
`clock <bpm> <count>` (an internal tick generator for hardware-in-the-loop tests, ticks stamped as
the UART's would be); `transport <start|stop|continue|spp N>`; `tempostat` (`Tempo()` and
`TempoCounts()` as JSON); `thru on|off` and `clockout on|off` (§9.5); and for T0b,
`midilog on|off|capture`. `midilog` prints each received F8, FA, FB, FC and F2 and each tap with
its capture stamp (block index and cycle count) and its production stamp, and counts other bytes
without printing them, so the drop-on-full USB serial keeps up under dense traffic; `capture`
adds the timer input-capture stamp of §11.4's T0b.

### 9.5 USB-MIDI, MIDI thru and clock out (D10)

USB-MIDI clock arrives on cable 0 (companion §7.1) once the firmware moves to TinyUSB; it feeds the
same translator. Full-speed USB quantises to 1 ms frames, the pedal's block anyway.

**MIDI out**, lane T4b: firmware only, no engine change and no sound revision. The Microcosm
transmits MIDI clock and by default echoes MIDI IN to MIDI OUT, with a four-way global setting
(clock and thru, thru only, clock only, neither) (`docs/research/microcosm.md:459-464`). With MIDI
in alone Brainscape would have to be the last device in a pedalboard's MIDI chain and could not
hand its tapped tempo on; draft v1 deferred both without listing the gap (record §6, P11).

- **Soft thru:** the control loop forwards every received byte from the UART ring to transmit,
  in order, before parsing.
- **Clock out:** under the Internal source, an F8 each time `Tempo().position` advances by a
  tick, read after every block: at most one block (1 ms) of jitter, as full-speed USB-MIDI has
  anyway. Under external clock the thru already forwards the master's clock and nothing is
  generated. No FA or FC: the pedal has no transport control, and receivers follow clock without
  Start, as §3.1's ClockFree does.
- **The setting** is the Microcosm's four-way global choice; default (D10): thru on,
  clock out off, so adding the pedal to a chain changes nothing downstream until asked.

### 9.6 Placement and CPU

**ITCM.** The live image at revision 7 has 61,840 bytes of `.itcm_text`, which starts 64 bytes
into the 65,536-byte region, leaving 3,632 (*measured* from the `modes-impl` build's
`brainscape_live.size.txt` and map; `firmware/README.md:476-491` gives about 3.6 KiB). Objects are
placed whole: `Engine.cpp` 15,246 bytes, `Granular.cpp` 11,404, `PostChain.cpp` 11,244,
`Validate.cpp` 8,612, `DetMath.cpp` 5,496; only `Decode`, `Encode`, `Sha256` and `TestSignal`
stay out (`firmware/CMakeLists.txt:198`). The CPU plan's steps 1–2, built bit-exact, take 424
bytes more: 3,208 spare (*measured*, cpu-budget.md §4.1). Its D4 is applied: P1's FFT rewrite
costs 8,016 bytes, not 6.3 KB, and does not link on wave 1's tree even before any tempo-core code,
so P3's rewrite (560 bytes) is the one built, and no smaller FFT is left to swap in. The plan
still adds step 4 (3,752 bytes) and the governor (about 2.3 KB), and step 5 an amount not yet
measured, and asks for 8 KB spare (its G6). With `Validate` moved out, the remainder is 3,208 −
3,752 − 2,300 + 8,612 ≈ +5.6 KiB before step 5 and the tempo core, and about +4.0 to +4.6 KiB
after the tempo core's 1.0–1.6 KiB (*calculated*): below that gate. Draft v1's figures (code
finding C12) gave −0.35 or +4.95 KiB, and its "adds next to no ITCM" is withdrawn (record §6,
H3). So:

- **An entry gate for T1 and T2** (D1): today's use, minus what moves out, plus the CPU plan's
  additions, plus the tempo core's, leaves at least 8 KiB, tallied from the merged tree's map.
  The CPU plan's FFT choice, which the gate waited on, is settled and applied: the owner's D4
  (2026-10-08) takes P3's rewrite when the spare after wave 1, the plan's steps 4–5 and the
  governor falls below 8 KB, which it does, so P3's is built (cpu-budget.md §4.1).
- **Cold code moves out, not only `Validate`.** `Engine.cpp`'s main-thread-only API (`Init`, the
  Exact-load and `Restart` clears, `GetParam`, `ModeSwitches`, the accessors the console reads)
  goes to a translation unit kept out of ITCM, or the engine is placed by function with a
  `.text.cold` section, the alternative `firmware/README.md:489-491` already offers. Moving
  `Validate` is not free: `ValidateStructure` runs inside `ApplyEvent` on every Spillover load
  (`dsp/src/Engine.cpp:315`, `:1068-1079`), so from QSPI its 8.4 KiB become a cold fetch in the
  load's block, where `pess_events`' worst block is already 146.3 % (`rev7-silicon-record.md`
  §3.2). T1 measures that block and keeps the structural check's hot path in ITCM if it does not
  fit.
- **The tempo core stays thin in ITCM.** `Tempo.cpp`, `IntMath.cpp`, `MidiClock.cpp` and the
  host-conversion object join `_bs_not_itcm_members`, checked by `NOT_ITCM`
  (`firmware/CMakeLists.txt:251-258`); `Engine.cpp`'s new code (load step 4's performance state,
  `Tempo()`, `TempoCounts()`, §4.2's payload checks) is thin calls into `Tempo.cpp`. *(As built,
  §11.12 notes 29 and 31: the engine reaches the core on every render span and every event through
  inline fast paths in ITCM, `ItcmCheck` refuses any other call out of ITCM, and the main-thread
  API left ITCM by function, leaving 4,000 bytes.)* ITCM grows by
  the clock offsets in `GranularCore::Process`, the new cases in `ApplyEvent`, the span call in
  `RenderFrames`, and the crossfade and slew in `PostChain`'s per-sample loop: about 1.0–1.6 KiB
  (*estimated*).
- The placement work is in T1's estimate (§11.4).

**CPU.** The budget is not met today: the nominal row peaks at 99.1 %, `dense_1ms` at 118.6 %, the
pessimistic rows at 135.5–168.5 % (`rev7-silicon-record.md` §3.3). The tempo core adds:

| Cost | Size | Where it lands |
|---|---|---|
| A CLOCK hit | one birth, at most 6,203 cycles at 48 voices (`rev7-silicon-record.md` §3.5) | at most one per 200 frames; the densest grid (×8 at 300 BPM) is 40 hits per second, 0.05 % on average (*calculated*) |
| A hit with a 16-grain burst at spacing 0 | 16 births on consecutive frames, ≤ 99k cycles, 20.7 % of a block when all 16 fall in one (*calculated*) | exactly a footswitch trigger's cost today; the worst blocks are the burst's, not the clock's |
| A ClockTick | O(1) sums and up to three 128-bit multiply-divides: about 1–3k cycles with its code in the I-cache (*estimated*). `Tempo.cpp` runs from QSPI behind the 16 KiB I-cache, ticks come 8–21 ms apart while the main loop's QSPI code runs, and refetching 1–2 KiB at roughly 100–200 cycles per 32-byte line adds about 3–10k (*estimated* from the default preset's XIP cold-against-warm gap, 1,902 × (1.697 − 1.076) × 48 ≈ 57k cycles per block, `rev7-silicon-record.md` §3.2, §3.4) | at most two per block by the producer's cap (§4.5): a worst block about 26k cycles, 5.4 % of the budget (*estimated*). Draft v1's 1–2k cycles left out the cold fetch, and its "at most two" had no mechanism (record §6, H4) |
| A crossfade | 3–4k cycles per block for 21 ms (§7.3) | per jump |

Block-grid stamps split no pedal block (§4.5); a sub-block stamp would cost about 5–6k cycles at
64 voices (cpu-budget.md §3: a `RenderRun` call per voice, 75–100 cycles). **The cost governor**
(cpu-budget.md §5, sound revision 8 there, or 9 if the tempo core lands first, §11.3):

- CLOCK hits are trigger-class, placed in its source order as manual/MIDI, onset, **clock**,
  burst, scheduler (D13); a deferred hit takes the draws of the frame it is born at, as the
  governor rules; it waits at most 48 frames and is then dropped (§6.3); and the grid never moves.
- ClockTick and Tap events are exempt from the firmware's coalescing and counted in its per-block
  event limit (its D12); its event reserve, a 24k-cycle placeholder, is sized at T1's bench as
  the measured worst tick, with the I-cache invalidated, times the cap of two. If that does not
  fit, the tick handler moves into ITCM (1–2 KiB, inside the gate above).
- The delay stage's constant is re-measured with a fade in progress, and T2's revision raises it
  (§7.3). Hits with bursts, bunched ticks and the crossfade are benched on the Rev7 before the
  governor's constants freeze (its D8); if they have frozen by then, T2 re-freezes them.
- The shared cost model and the audition's Load row count CLOCK's hit rate and bursts.

## 10. The plugin

### 10.1 Following the host

- **Tempo source**, a wrapper setting saved in the session (`BSWS`): **Host** (default, D9) or
  Internal. Under Host, when the host reports a tempo, the wrapper emits §4.4's events at the Host
  rank (`plugin/src/EventQueue.h:18`) and drops taps and Tempo-knob events before stamping them;
  without a reported tempo (the Standalone, some hosts) it behaves as Internal.
- **"Restart on transport start" stays off by default** (`plugin/src/StateCodec.h:18`,
  `PluginProcessor.h:297`): delay trails across stops are the reason it is off (companion §4.9),
  and the transport anchor aligns the grid to the host's beats either way. The status survey's
  "keep it on" assumed it was on (code finding C1). Bounces are reproducible only with it on
  (§4.4); the labels already say so (profile §2.4).
- **Re-asserts after every Exact load** (§2.5), at frame 0 of the new timeline, before the
  block's own events (`LoadAfterRestart`, `PluginProcessor.cpp:249-258`):
  1. **Tempo:** under Host with a reported tempo, the host's; otherwise the **last committed
     tempo** (`Tempo().nsPerQuarter` after the last block), except on a preset change under
     recall Preset, where the stored tempo plays.
  2. **Subdivision events from rows 83 and 84** after a restart of the same preset: the
     transport-start restart, `prepareToPlay` and a session restore. The restart loads
     `ToPreset(start, activeMode_)`, whose performance state is the mode's stored one
     (`PluginProcessor.cpp:71`), and the spare's identity check compares only leaves and mode
     (`:587`); without the re-assert a bounce would play the stored Subdiv while the host showed
     row 83's (record §6, E7). On a preset change the wrapper instead sets rows 83 and 84 to the
     new preset's stored values and notifies the host.
  3. **Transport,** as §4.4.
  Each is a function of the host's state and the session's, so a bounce at constant tempo stays
  reproducible.
- **The internal tempo persists:** the session saves the last committed tempo in ns, so the
  Standalone, which has no host tempo, and a DAW set to Internal keep a tapped or typed tempo
  across relaunch, project reload and a change of audio device or sample rate, every one of
  which runs an Exact load (`PluginProcessor.cpp:231-237`, `:522`); draft v1 reverted them to
  the preset's 120 BPM (record §6, P12).
- **The engine runs at the host's rate** (`PluginProcessor.cpp:191-194`): P is exact at that rate
  (§2.1); parity holds at 48 kHz only.

### 10.2 MIDI clock in the Standalone

JUCE's Standalone has a playhead without tempo or playing state and stamps device MIDI by arrival,
scaled into the next block (JUCE `juce_audio_utils/players/juce_AudioProcessorPlayer.cpp:243`,
`:391-393`; `juce_audio_devices/midi_io/juce_MidiMessageCollector.cpp:94-139`), so a tick's frame
is off by up to a block, σₓ ≈ 3.1 ms at 512 frames, which the 96-tick fit absorbs to 0.055 %
(*calculated*, §12). The MIDI filters that keep only note-ons (`PluginProcessor.cpp:857-867`,
`:973-982`) pass F8, FA, FB, FC and F2 to the translator at their sample positions, at the MIDI
rank. A "Receive MIDI clock" setting (default on) gates it. Ticks bunched onto one frame are
harmless to the follower (§3.3 step 1). A later refinement maps the device's timestamps to
frames with a DLL (§12).

### 10.3 Tap button, BPM display and saving

The pedal view gains a **TAP** button (a Tap event at the next chunk's first frame, UI rank, greyed
under host sync) and a **BPM display** from `Engine::Tempo()`, copied to an atomic after each
block: the committed tempo to 0.1 BPM, the source (HOST, MIDI, INT), a lock dot, a beat LED from
the position, the preset's **stored tempo beside the live one** with a store action, and, in a
preset that does not use tempo, a note saying so (§6.6). Typing a tempo sends a Tempo event.
Today the editor has only TRIGGER and FREEZE (`plugin/src/PluginEditor.cpp:88-95`).

**Saving a preset captures the performance.** The editor's Save, Curation, and any later pedal
save write the live committed tempo (rounded to whole µs), the live subdivision and the live time
mode into STAT's performance state, so the tempo a song preset recalls under Preset, and every
Exact load plays, is the one the player set; draft v1 never said how a tempo reached a preset
(record §6, P12).

### 10.4 Parameters and session

| ID | Name | Kind | Range, default | Host |
|---|---|---|---|---|
| 63 | `post.delay.sync` | Leaf (synced-times revision) | 0–16, 0; display §5.4 | registered as a leaf; 17 steps |
| 83 | `perf.subdiv` | Performance | knob position 0–5 (×1/4 … ×8), 2 (TAP) | automatable; the wrapper sends Subdivision with §5.1's code |
| 84 | `perf.time_mode` | Performance | 0–2 (Free, Subdiv, Tempo), 0 | automatable; Subdivision field 1 |
| 85 | `global.tempo_recall` | Global | 0–1 (Keep, Preset), 0 | a device setting, not registered |
| 86 | `global.tempo_glide` | Global | 0–1 (Off, On), 0 | a device setting, not registered |

Rows are appended after 82, as host indices require (`PluginProcessor.cpp:113-121`; compiler
§4.5). Rows 85 and 86 have no domain: the engine reads 85 at load step 4 and 86 when it
classifies a change of Pc (§7.1). The wrapper's `WrapperEvent::Type`
(`plugin/src/EventQueue.h:16`) gains Tap, Tempo, ClockTick, Transport and Subdivision; the
session saves the Tempo source, "Receive MIDI clock" and the last committed internal tempo.

## 11. Schema, revisions, plan and decisions

### 11.1 Schema and compiler

- **`scheduler.subdiv` is withdrawn** (D14; compiler principle 5 lets a reserved field change in
  the pull request that builds it): an E2 unknown key. The `Clock` feature is the `clock` source
  only (`Mode.cpp:100`, `:102` removed).
- **`performance.tempo_source` is withdrawn** (D3): the source is a device setting, and a byte in
  STAT, which `sound_hash` covers (compiler §6.3), would give presets that sound the same different
  hashes (code finding C8).
- **`performance.subdiv`** reads §5.1's labels in code order (`Schema.cpp:82`, `kSubdivNames`,
  becomes `tap, x1/4, x1/2, x2, x4, x8`), default `tap`; the printed Microcosm labels `"1/4"`,
  `"1/2"`, `"2x"`, `"4x"` and `"8x"` are E5, never read as rates or note values (§5.1).
- **Divisions:** `Division()` reads and writes §5.2's names (`Schema.cpp:660-675`, `:1904-1907`);
  row 63 stays numeric (§5.4).
- **Support** (`Schema.cpp:205-233`, `:1752-1758`): the single `kNeedPerformance` bit
  (`Schema.cpp:44-45`), which unlocks all five performance fields with CLOCK, splits into
  `kNeedPerformanceTempo` (time mode, subdiv, tempo; the tempo-core build) and
  `kNeedPerformanceReverse` (global reverse; later), so `reverse: true` stays E6 (code finding
  C7). `kModeFeatureClock` is supported from the tempo core, `kModeFeatureTempoSync` and
  row 63 from synced times.
- **Lint:** L12 (§6.2), L13 (§6.5), **L14** (§5.2: a synced field whose reachable codes fold at
  the preset's stored tempo and Subdiv); L2 evaluates a synced base delay at its shortest duration
  over the tempo range and the Subdiv the mode can reach; L5 already counts `clock` as
  free-running. `bspc` reports `UsesTempo` (§6.6) beside the lint.
- **Tests:** `kFullStructure` (`compiler/tests/test_compile.cpp:110-134`) moves `"subdiv"` to
  `performance` and drops `tempo_source`; E6 cases (`:275-316`) follow the support table; E5
  cases cover the printed labels.

### 11.2 Package and validator

STAT and MODE keep their layouts. `ValidateMode` and `DecodePreset` require SCHD's byte 2 and STAT's
performance byte 3 to be 0, reserved (`Validate.cpp:412-418`, `Decode.cpp:532-537`), and read
`subdiv` with §5.1's codes; `UnsupportedPerformance` (`Engine.cpp:295-302`) narrows to `reverse`,
with `dsp/tests/test_modes.cpp:291-303` updated. No package in the tree carries a nonzero byte
there (no clock, subdivision, `base_sync`, tempo or performance key appears in
`firmware/factory/`, its reserves or the golden presets), so no package hash changes in the
tempo core. A frozen fixture with STAT byte 3 at 1 must fail as `Performance`;
`w2-step-table.bsp` keeps its `UnsupportedFeature` verdict (`dsp/tests/blob/Fixtures.cpp:136-140`).

### 11.3 Sound revisions

Profile §5.12: each step that can change output is one commit raising `kSoundRevision` by one.

| Step | Revision | Sound | Package hashes |
|---|---|---|---|
| Tempo core: events 6–10; `TempoCore` (the phasor with §6.3's catch-up, tap with the downbeat, the follower with gaps and resumes); the performance state applied, `Restart` setting it; CLOCK hits; Subdiv re-coded; SCHD and STAT bytes reserved | N | yes: new inputs and a new source | unchanged (every byte involved is 0) |
| Synced times: row 63 a `Leaf`, `base_sync`, §5.2's table, folds, the 4 s line, the jump classes with the crossfade and the slew, row 86 | N + 1 | yes | every package's `sound_hash` changes, since the compiler writes the new leaf at its default; the package-change label, as revisions 4, 6 and 7 needed (compiler §7.6) |
| `MidiClockParser` (with the master's position), host-conversion functions, `UsesTempo`, the effective-value display | none | producer code in `dsp/`: the "sound-neutral" label | — |
| Firmware (stamps, epoch, UART restarts, the sampled tap, thru and clock out), plugin (re-asserts, the persisted tempo, saving the performance), audition, compiler-only changes | none | outside the trigger paths | — |

N is the next revision when the tempo core lands: 8 if it precedes cpu-budget.md's governor,
which that design numbers 8, else 9. CLOCK's 48-frame lateness cap (§6.3) belongs to whichever
of the two lands second. Each revision re-mints `golden.json`, and every earlier preset must
reproduce its hashes and counters, as at every wave-1 revision.

### 11.4 Lanes, order and estimates

| Lane | Work | Files | Starts | Days (*estimated*) |
|---|---|---|---|---|
| **T0** design | this document, its record and its amendments | `docs/` | done | — |
| **T0b** breadboard | MIDI in and out and the tap switch on the Rev7 (§9.1's netlist, DIN). `midilog` stamps captures independently of the audio interrupt, since the UART callback shares priority 0 with the audio DMA and fires only on line idle or a half buffer (LD `src/per/uart.cpp:897`, `:1025-1036`, `:1095-1102`): run with the engine muted, so the callback returns at once, and better, with the opto's output wired also to a timer input-capture pin (D9 = PB4, LD `src/daisy_seed.h:219`; TIM3_CH1 there, to confirm on the H750 datasheet) logging each start bit's edge in cycles beside the production stamp. Measure tick jitter from a hardware master and a DAW with the pedal's own capture delay apart, tap variability and the switch's bounce, UART errors and restarts at boot and on hot-plug mid-stream, late pushes | `firmware/`, parts about $3 | now: no sound change | 3–5 |
| **T1** tempo core | §2, §3, §4.1–§4.3, §6.3, §6.4's engine side, §6.6's function; integer helpers; rows 83–85; §11.1–§11.2; its unit tests, verbs, generators, presets, counters and ablations; the ITCM placement work and a cold-cache bench of the tick path on the Rev7 (§9.6) | `dsp/`, `compiler/`, `dsp/tests/`, `firmware/CMakeLists.txt` | after the first set's knob ratings and the ITCM gate | 10–15 |
| **T2** synced times | §5.2–§5.4, §6.1, §6.2, §7: the 4 s line and the Bulk arena, the classes, the slew, the crossfade, row 86; L12–L14; their tests and presets; the delay stage re-measured with a fade | `dsp/`, `compiler/`, `firmware/platform/` | after T1, through the ITCM gate | 5–8 |
| **T3** plugin | §4.4 and §10: host events with offsets, re-asserts, the persisted tempo, saving the performance, the BPM panel | `plugin/`, `dsp/` (host conversion) | after T1 | 4–6 |
| **T4** pedal firmware | §4.5, §9.2–§9.4: latencies, the epoch, the tick cap, 64-bit counts, UART restarts, the sampled tap switch, the Exact-load sequence and re-asserts, console commands; a hardware-in-the-loop test: an Exact load with clock running at 300 BPM, then a tap and a console set within 10 ms, both applied | `firmware/` | after T0b and T1 | 3–5 |
| **T4b** MIDI out | §9.5: soft thru, clock out, the four-way setting | `firmware/` | after T4 | 1–2 |
| **T5** audition | S12, the beat-lock metric, the Lock row; re-rate the echoic modes' Time; Engram and Callback given a synced post delay by listening (§6.6) | `tools/audition/`, `firmware/factory/` | after T2 | 3–5 plus listening |
| **T6** bench | DWT at revision N + 1 on the Rev7, with the CPU fix: hits with bursts, bunched ticks, the crossfade | `firmware/` | after T2, with the governor | 1–2 |

**Order** (amended 2026-10-09, D1): T1, T2 and T3 now, for a plugin build the owner can test, with the tempo code outside ITCM unless it fits; T4 after T0b and the ITCM gate. As first written: T0b now; the ITCM gate passed (§9.6), the CPU plan's FFT choice being settled and
applied (2026-10-08: P3's rewrite, cpu-budget.md §4.1); the first set's knob ratings
(`docs/STATUS.md:57`); T1, then T2, each one revision, with T3 and T4 alongside; T4b; T5; T6;
then the rest of W2 (steps, mark walk, global reverse, velocity) and step 4's rhythmic second set
(compiler §11.4). **Total** about 30–48 engineer-days beyond this design (*estimated*). Draft
v1's 22–38 left out the ITCM work, the producers' re-asserts, the epoch and UART recovery, MIDI
out and the reviews' added tests; the survey's 25–45 included the design and golden coverage as
separate lines, folded here into T1 and T2.

### 11.5 Owner decisions

The owner confirmed every answer below on 2026-10-08, as proposed and without amendment, so none
is provisional any more. Each was first adopted provisionally, earlier the same day, so the
design was complete; as with every owner answer in this design, each stays reversible before the
first public release, and reversing one is an owner decision of its own and, where it changes
the sound, a sound revision. The CPU proposal's decisions were confirmed at the same time
([cpu-budget.md](cpu-budget.md) §9), its D4 settling the FFT choice that D1's gate is tallied
with; D4 has since been applied: P1's rewrite does not fit wave 1's ITCM, so P3's is built
(cpu-budget.md §4.1; §9.6). D1–D11 are the status survey's eleven, with its recommendations
amended where the evidence of §12 or the reviews required (D2, D4, D7, D8, D9, D10, D11); D12–D20
are draft v1's, several amended by the reviews (D13, D15, D16, D17, D18, D20); D21–D23 are new in
draft v2.

| # | Decision | Answer (confirmed by the owner, 2026-10-08) | Consequence | § |
|---|---|---|---|---|
| D1 | Order | Keep W2 after the first set's knob ratings; start the breadboard (T0b) now; land the tempo core before steps, mark walk, global reverse and velocity; enter T1 and T2 only through the ITCM gate (8 KiB spare after the CPU plan). **Amended by the owner 2026-10-09:** T1, T2 and T3 start now, ahead of the knob ratings, so the owner can test tempo in the plugin; T1 and T2 place their code outside ITCM unless it fits the live image's spare (the firmware's ItcmCheck decides); T4 still waits for T0b and the ITCM gate | Nothing here changes the sound before the ratings and the CPU plan's FFT choice; T0b's data arrives before the thresholds freeze. The amendment keeps that: no factory mode uses tempo, so their renders, and the ratings made on them, carry across revisions N and N + 1 unchanged except where N + 1's new leaf re-stamps package hashes (§11.3) | 11.4, 9.6 |
| D2 | Sources and priority | The Microcosm's: Start and Continue switch to external clock at the next tick and set the position; Stop reverts to internal keeping tempo and phase, and cancels an armed Start; tap and the Tempo knob are ignored under a running master. Added: clock without transport is followed after 24 ticks while the master is not stopped; a second without a tick reverts to internal and clears the window in any source; a loss mid-song resumes ClockRunning after 24 ticks; host tempo is the wrapper's, which drops taps while following it | A clock box that never sends Start still locks; a cable knock costs a beat or two of free-running, not the rest of the set; tap is unavailable while a master runs | 3 |
| D3 | Tempo source per preset or global | Global: "receive MIDI clock" (pedal) and the Tempo source (plugin) are device settings; STAT's `tempo_source` byte is reserved | A preset change can never unhook the pedal from the band's clock, and presets that sound the same hash the same; no preset can opt out of clock | 2.4, 3.6 |
| D4 | What a load does to tempo | In the engine: Exact plays the stored tempo, time mode and subdivision; Spillover applies the stored time mode and subdivision and keeps the running tempo unless `tempo_recall` is Preset with the internal source; no load moves the beat. Producers re-assert at frame 0 after every Exact load: the committed tempo under Keep or a followed clock or host, the master's position under a running clock, and the live Subdiv after a restart of the same preset | Keep is a true global tempo, and a preset change mid-song keeps the band's beat; the engine's start state is still the package's, and every re-assert is in the log | 2.5, 9.3, 10.1 |
| D5 | What syncs; Q7 | The post delay (row 63), `base_sync` and CLOCK hits. Q7: in Subdiv and Tempo modes the Time knob never reaches `macro.time`, which expression, MIDI and hosts still do | Modulators wait for W3 and the looper for its design; a mode that wants Time on `macro.time` on stage stays in Free | 6 |
| D6 | Q11, tempo-exact grain feedback | Rhythmic repeats on the post delay; `base_sync` places the first tap exactly; grain feedback keeps its 10.67 ms pass (lint L12) | No feedback-path rework; grain-feedback echoes drift late against the grid, as echoic modes already do | 6.2 |
| D7 | Divisions | Subdiv is the Microcosm's six rate multipliers (resolved from its manual), labelled as rates (`x1/4` … `x8`), code 0 = TAP the default; sixteen note values by duration; folding by octaves | CC#5 matches the Microcosm; no screen or document shows "1/4" for a rate; no package re-hashes | 5 |
| D8 | Smoothing | A least-squares fit over the last 96 tick labels; a two-level deadband; taps averaged over up to four intervals; Pc changes above 3.1 % crossfade, deadband commits slew over about a second, smaller changes glide over 50 ms; after a Start, an early commit at 12 ticks when the tempo moved more than 6.25 % | Clock jitter never moves the delay; a new tapped or song tempo does not swoop; Tempo-knob sweeps still bend like tape; a song's first beat plays at its tempo | 3.3, 7 |
| D9 | Host tempo | Follow the host by default; ns resolution, 1 µs hysteresis, a tick-aligned anchor carried as a frame offset; "Restart on transport start" stays off by default (it is off, not on) | Live DAW playback tracks the host with no stray hit before the anchor; a reproducible bounce needs the restart setting | 4.4, 10.1 |
| D10 | MIDI hardware for v1 | MIDI in with the H11L1M (DIN, or TRS Type A); MIDI out populated, with soft thru (default on) and clock out (default off) as the Microcosm's four-way global setting, firmware lane T4b; DIN against TRS at schematic time | The pedal can sit mid-chain and lead a chain from its tap tempo, closing a Microcosm parity gap; about $1 of parts and 1–2 days of firmware; the schematic carries an out jack | 9.1, 9.5 |
| D11 | Song position | A position in 24-ppqn ticks in Transport's value (Song Position × 6), no separate event; host-style events carry the anchor's frame offset in `id` bits 16–31 | SPP and host locates need no event number of their own; those `id` bits are spent | 4.1 |
| D12 | Subdiv's scope | It scales every tempo-derived duration of the mode: the CLOCK grid and the synced fields | The Subdiv knob halves or doubles synced echoes as well as hits; the editor shows the product | 5.1 |
| D13 | CLOCK allocation | Trigger class: oldest-steal at `voice_count`, never capped by `overlap`; the governor's order manual/MIDI, onset, clock, burst, scheduler; a hit not admitted within 48 frames is dropped and counted | Hits never vanish to `overlap`; under load a hit is at most 1 ms late and a cloud loses at most 48 frames per hit; factory clock modes must show no deferral | 6.3, 9.6 |
| D14 | Two stored subdivisions | One: `performance.subdiv`; `scheduler.subdiv` withdrawn | One source of truth; SCHD's byte reserved | 2.4 |
| D15 | The Tempo knob | 20–300 BPM exponential; soft takeover after a load, a tempo change and every time-mode change; inert under external clock, with the LED showing why | The knob never jumps the tempo; under a running master it does nothing in Tempo mode (Subdiv mode still works) | 6.4 |
| D16 | Tap feel | Two taps set the tempo; a pause re-arms; more than 40 % off restarts the chain; the mean of up to four intervals; no skip detection; the first tap after a pause is the downbeat; under clock without Start a tap marks the downbeat only | Whole-note grids and later patterns land on the player's "one"; re-tapping after a pause moves the bar line too | 3.2 |
| D17 | Tempo recall default | Keep (the Microcosm's global tempo), holding across Exact loads too; Preset as the device setting's other value | The tempo carries from preset to preset as on the Microcosm; song presets with their own tempo need the setting changed | 2.5 |
| D18 | The time-mode gesture | The Microcosm's Subdiv ↔ Tempo toggle in presets that use tempo (from Free it enters Subdiv); Free set from the editor, a load or a shift gesture | No stage gesture passes through a third state; reaching Free on stage needs the shift gesture | 6.4 |
| D19 | Expression and MIDI reach | Expression and macros reach row 63 and `macro.time`, not tempo or Subdiv; the pedal's MIDI map adopts the Microcosm's CC#5, CC#10 and CC#93 | No expression-to-tempo until a `Performance` target kind exists | 6.5 |
| D20 | Pedal event timing | Block-grid stamps at a fixed latency (two blocks; five for the tap); ring entries tagged with a timeline epoch; at most two ticks per block; the tap switch sampled each audio callback with a state debounce; interrupt priorities unchanged; sub-block stamps only if T0b shows the need | 2 ms of control latency (5 ms for tap); an Exact load cannot freeze control; a block's tick handling is bounded | 4.5, 9.2, 9.3 |
| D21 | Presets that do not use tempo | `UsesTempo` derived from the package, never stored; in such a preset the time-mode gesture does nothing and the tap LED is steady, while tap still sets the global tempo; T5 gives Engram and Callback a synced post delay by listening | Tap is audible in factory presets at launch, or, if the listening rejects it, inert in the first set until the rhythmic second set | 6.6 |
| D22 | Tempo jumps | `global.tempo_glide` Off by default, so jumps crossfade; On restores the tape glide for them; the default confirmed by ear on S12 | A new tapped tempo or a song recall switches cleanly; the swoop is a choice, not a side effect | 7.1 |
| D23 | The synced post-delay maximum | 4 s inclusive for synced targets (+768 KB of SDRAM, the Bulk arena 17 → 18 MiB, an Exact load about 2 ms longer); `time_ms` keeps 2 s | Every note value fits at 120 BPM and above and unsynced presets are unchanged; slower songs still fold | 5.3 |

### 11.6 Design decisions taken

| Decision | Choice | § |
|---|---|---|
| 1. Tempo representation | Q32.32 frames per quarter; an exact integer phasor; durations by 128-bit multiply-divide, from out-of-line integer helpers | 2.1–2.3 |
| 2. Event units | ns per quarter in Tempo; ticks in Transport's position, with a frame offset for host anchors; µs stays in presets | 4.1 |
| 3. Where estimation runs | In `dsp/`, on applied frames; producers only stamp, filter, re-assert and log | 1.4 |
| 4. Timeouts | A gap applied before the next event of any type, valid because nothing between events reads the source or the window | 3.5 |
| 5. Clock following | A least-squares fit over the fitted ticks among the last 96 labels, with incremental exact sums and residuals as exact numerators; outliers excluded; re-acquired after 6 of one sign | 3.3 |
| 6. Lost ticks | One label per tick, except dropouts of at least four ticks and 100 ms, inferred from a valid fit of 8 or more through the clamped P_fit; inference with confirmation is a later refinement if T0b measures losses | 3.3 |
| 7. Unstamped API | None for events 6–10; `ProcessContext`'s tempo fields deleted | 2.6 |
| 8. The MIDI translator | Shared in `dsp/`, ahead of libDaisy's parser, also following the master's position | 4.3 |
| 9. Births on the grid | `FireTrigger` with ordinal 3; a catch-up at every span start; the grid independent of admission | 6.3 |
| 10. Tempo changes | Classes: jumps crossfade through a second post-delay head, one 1,024-frame fade at a time chained inside the per-sample loop; drifts slew; steps glide | 7 |
| 11. Restart and loads | `Restart` sets the active preset's stored performance state; producers re-assert the running performance as logged events | 2.5 |
| 12. The pedal's timeline | Fixed latencies, an epoch on every ring entry, at most two ticks per block | 4.5 |
| 13. Placement | `TempoCore` in the Warm arena; its code out of ITCM behind an ITCM gate | 2.6, 9.6 |

### 11.7 Risks

1. **CPU.** CLOCK modes with bursts make the worst blocks a footswitch burst's, already over
   budget, and a tick's handler is costly when its code is cold. Mitigation: the cost governor
   with the 48-frame cap (§6.3, §9.6); the producer's tick cap; T1's cold-cache bench; compiler
   lints on hit rate × burst.
2. **ITCM.** The gate may fail even with `Validate` out. Mitigation: the cold API out of ITCM or
   placement by function (§9.6), in T1's estimate. That is the only lever: the CPU plan already
   built its smaller FFT rewrite (cpu-budget.md §4.1). Placed by function, about 16 KB of the live
   image's ITCM is never reached from the audio callback (*estimated*, cpu-budget.md §4.1).
3. **Computer clocks.** Real DAW jitter may exceed the E-RM case. Mitigation: T0b's recordings
   replayed in the corpus; the thresholds of §3.3 and §7.1 are constants to tune before the first
   public revision.
4. **Feel.** The crossfade, the slew, the 3.1 % jump threshold, the Tempo knob's curve and the
   downbeat tap may each suit poorly. Mitigation: S12 with `tempo_glide` both ways and the
   owner's listening; each is a constant.
5. **The follower's complexity.** Mitigation: every rule is a table in §3, tested against a
   frame-by-frame rational reference, with a case for every draft-v1 finding (§8.2).
6. **Host quirks.** Mitigation: the documented limits of §4.4 and the labels.
7. **A producer that forgets a re-assert** loses Keep or the master's beat silently, though not
   parity. Mitigation: one re-assert helper per producer, the plugin's bounce and persistence
   tests (§8.4) and T4's hardware-in-the-loop test.

### 11.8 Open questions, settled at T0b or by listening

1. Tick jitter on the Rev7 from a hardware master and a DAW, and later USB, with the pedal's own
   capture delay measured apart (T0b's timer stamps): the outlier and dropout thresholds, the
   window, the deadband and the early commit's constants.
2. Human tap variability and the switch's bounce on the Rev7: the 40 % and 1.75× thresholds, the
   24 ms release and the confirmation (§9.2).
3. UART errors and late pushes under load: lost-tick inference (§11.6 item 6), sub-block stamps
   and the UART's priority (D20).
4. By ear, on S12: the crossfade's length, the jump threshold, the slew's second, and
   `tempo_glide`'s default (D22).
5. The Time knob under external clock: inert (D15) or Big Time's bend-and-snap-back.
6. Whether the downbeat tap (D16) feels right, or wants a gesture of its own.
7. Bar alignment beyond 4/4: a host's time signature and bar position; MIDI has neither.
8. Reporting upstream libDaisy's parser defect (§4.3) and its UART listener staying stopped after
   an error (§9.3).

### 11.9 Amendments made with this design

Dated 2026-10-08, in each document's style: **grain-engine.md** §4 (CLOCK's allocation and
`overlap`, D13), §6 (Subdiv as rate multipliers with rate labels, one stored subdivision, the
two-state gesture), §9 (the API listing's `SetTempo`, `Tap`, `SetSubdiv` and `SetExternalClock`
replaced by events 6–10, compiler record §3 item 14); **companion-app.md** §4.10 (the pedal's
latencies, epoch and logs) and §6.2 (the tempo source is a device setting; STAT's byte reserved;
Keep across loads through re-asserts; saving captures the performance); **mode-compiler.md** §1.2
(the pass is this document), §2.3 and §2.6 (`scheduler.subdiv` and `tempo_source` withdrawn, the
re-coded default, the rate labels), §7.4 (the payloads), §12.3 (Q2 recorded as provisionally
answered, Q7 and Q11 answered here); **determinism-profile.md** §5.11 (tempo events, the gap rule
and the pedal's stamps); **docs/README.md** and **docs/STATUS.md** (this document). Draft v2
revised the notes of draft v1's commit where its rules changed. On 2026-10-08 the owner's
confirmation of §11.5 was recorded, dated, in the notes that called its answers provisional
(grain-engine.md §4 and §6, companion-app.md §6.2, mode-compiler.md §1.2, §2.6 and §12.3, whose
Q2 the confirmed D1 settles), in determinism-profile.md §5.11, in docs/README.md and
docs/STATUS.md, and in the record. The same day, once the CPU plan's steps 1–2 were built, §1.2,
§9.6, §11.3–§11.5, §11.7's risk 2, C12 and record §2.9 were brought up to its D4 as applied
(P3's FFT rewrite) and to the measured ITCM (3,632 bytes spare, counting the 64-byte offset,
and 3,208 with steps 1–2), and the CPU proposal's citations now point at cpu-budget.md. The
implementing pull requests amend the code comments that cite the old meanings (`Mode.h:86-97`,
`PresetState.h:42-56`, `Engine.h:206-210`).

### 11.10 As built: the tempo core as a library (2026-10-09)

Lane T1's first part, under D1 as amended: the tempo core as library code that nothing in the
engine calls yet, so it changes no output (sound-neutral commits before revision 8). Built:
`dsp/src/detail/IntMath.h` and `dsp/src/IntMath.cpp` (§2.1's helpers);
`dsp/include/brainscape/Tempo.h` (events 6–10's numbers and payloads, `TempoInfo`, `TempoStats`);
`dsp/include/brainscape/MidiClock.h` and `dsp/src/MidiClock.cpp` (§4.3); `dsp/src/detail/Tempo.h`
and `dsp/src/Tempo.cpp` (`TempoCore`: §2.1–§2.5, §3, §4.2, §6.3's `GridFrames`, §7.1's commits and
classes). The three objects join `_bs_not_itcm_members` (§9.6) and compile `-mgeneral-regs-only`
on the M7. §8.2's unit tests are `dsp/tests/test_intmath.cpp`, `test_midiclock.cpp`,
`test_tempo.cpp` (TempoCore against a reference model written from this document,
`dsp/tests/TempoReference.h`: the phasor in closed form in exact 128-bit arithmetic, the grid
frame by frame, the follower's sums computed directly, gaps applied at their deadlines; random and
adversarial streams at block sizes 1, 48, 441, 512, {48, 1, 127, 32}, {300, 512, 5, 64} and random,
each comparison failing on a perturbed reference) and `test_tempo_rules.cpp` (each rule against
hand-worked numbers); `brainscape_tempo_tool --check` repeats the comparison on every CI leg and on
the emulated M7 with a committed digest. Record §1.3's probes are in `tools/parity/clock/`.

Where the text left a choice, or read differently in two places, the build follows the design's
intent, as below:

1. **FC carries AtNextTick.** §4.3's table gives FC as a plain Transport Stop, but §3.4 tells
   MIDI's Stop (it cancels an armed transport, reverts a clock source to Internal, sets the
   continue position and `masterStopped`) from the host's (it stops the transport) by that flag,
   and §3.6 ignores a host-style Transport under clock. The translator sends FC as Stop with
   AtNextTick, which still applies at its own frame.
2. **`transportsIgnored`** joins `TempoStats`: §3.4 counts a host-style Transport ignored under
   clock, and §2.6's list had no field for it. A bounce counts in `tapsIgnored`, beside taps under
   ClockRunning.
3. **Positions are read in integers.** A Transport's value is checked and converted from its
   binary32 bits by integer arithmetic (`DecodePositionBits`), not in floating point inside the
   guard as §4.1 and principle 2 say: the same positions and the same refusals (−0, fractions,
   subnormals, infinities, NaNs, the range), and principle 2's two floating-point steps become
   one, the CLOCK jitter. A value that must be +0 is checked by its bits, so −0 is invalid (§4.2).
4. **Acquisition and commits need a valid fit.** §3.3 step 6 enters a clock state at N = 24 and
   commits P_fit; with every fitted tick at one frame (A = 0) there is no P_fit, so the core
   acquires, places and commits only on a valid fit (D > 0, A > 0), and until then the ticks feed
   the window as under Internal.
5. **The first label.** "The last label + 1" needs a first: a tick with no labelled tick before it
   since `Init` or `Restart` takes the phasor's nearest tick (ties up), so ClockFree's labels
   continue the phasor's (§3.1) from the start. Labels survive a gap, which clears the window, the
   ring and the dropout reference only (§3.5).
6. **MIDI's Stop in every source.** It reverts ClockFree to Internal as well as ClockRunning
   (§3.1's table: ClockFree is "left by … Stop"), and sets the continue position to the last label
   + 1 whenever a tick has been labelled.
7. **Continue reads the continue position when it applies**, at the next tick, so a Song Position
   that arrives between FB and that tick (while not ClockRunning) is honoured.
8. **The phase tap's δ** lies in (−48, 48]: §3.2's formula, 96·FloorDiv(k_n + 48, 96) − k_n, sends
   a position exactly 48 past a bar line up, where the text says −48 ≤ δ < 48. The formula is
   built.
9. **A Spillover load sets the stored performance state** that `Restart` plays (§2.5: the active
   preset's, "the last loaded preset's STAT").
10. **The grid runs whatever the mode.** `GridFrames` advances the grid and `lastFired` in every
    span, so switching to a mode that lists `clock` never fires a stale catch-up; whether a hit
    becomes a birth, and §6.3's one hit per frame, are the engine's (T1's second part). Noted for
    the owner: a Subdivision event to a finer grid makes §6.3's catch-up fire the largest grid
    position already passed, at the event's frame (×1/4 to ×8 mid-bar fires the 32nd just passed),
    as the rule says for any jump; whether a Subdiv change should rather skip passed positions is
    a listening question for T5. *(2026-10-10: it skips them now, note 25.)*
11. **The translator's `Reset()`** forgets the running state with the position: a UART restart may
    have lost bytes, a Stop among them.
12. **§7.4's commit budgets against §7.1's constants, measured** (ten minutes per case with §8.3's
    clock models, the hidden `[experiment]` case of `test_tempo_rules.cpp`). A hardware clock
    commits nothing after the acquisition at 60, 120 and 140 BPM, but once at 300 BPM (rule 3.2,
    0.026 %), against a budget of none. A computer clock commits 1–2 times at 60 BPM, 19–117 times
    at 120, 150–190 at 140 and 520–710 at 300, nearly all by rule 3.1, each 0.2–0.5 %, against "at
    most one per minute, each ≤ 0.4 %". The fit itself is as §3.3 calculates (P_fit's σ 0.029 %,
    0.059 %, 0.070 % and 0.105 % at those tempos), but rule 3.1's band, Pc >> 9 = 0.195 %, compares
    two noisy fits (Pc is a committed P_fit), and the fit's σ grows with the tempo, so at 140 BPM
    the band is about two σ of the difference; rule 3.2's run likewise outlasts 192 ticks after an
    early acquisition at a fast tempo. Every such commit is a Drift, so the post delay slews and
    never jumps, but `clock_midi_computer`'s counters will exceed §7.4's budget. The constants are
    built as designed; tuning them (§11.7 risk 3, §11.8 item 1) is an owner decision. *(2026-10-10:
    a measured candidate that meets the budgets at 60–140 BPM is in note 32.)*
13. **A loss under ClockRunning clears the running bit.** §3.5 keeps P, Pc and the phase and sets
    `resumeRunning`; it does not say whether `TempoInfo`'s "transport running" bit stays set. The
    core clears it with the source, so a display shows a stopped transport while the clock is
    gone (the snapshot reads the same through the gap predicate), and the resume's implicit
    Continue sets it again.
14. **An armed Locate applied on an empty window arms the early commit.** §3.4 applies a Locate
    armed under ClockRunning "as Start", and a Start on an empty window arms §7.1's early commit,
    which §7.1 names for Start and Continue only. The core follows §3.4. The case needs a window
    emptied under ClockRunning, which only an outlier labelled 96 or more past every fitted tick
    can do.

### 11.11 As built: the tempo core wired in, sound revision 8 (2026-10-09)

Lane T1's second part, under D1 as amended. Two sound-neutral commits first: the schema half
(§2.4, §5.1, §11.1, §11.2: `Subdivision` re-coded so code 0 is TAP; SCHD's byte 2 and STAT's
performance byte 3 reserved, a nonzero one `ModePadding` and `Performance`, pinned by two new
frozen fixtures; `scheduler.subdiv` and `performance.tempo_source` withdrawn as E2; the Subdiv
labels in code order with the Microcosm's printed ones E5; the performance support bit split into
the stored tempo, time mode and subdivision, which come with CLOCK, and global reverse; §5.2's
names for divisions), and the producer functions (`UsesTempo` beside `EvalMacro`, reported by
`bspc lint`; `tempo::TempoNsFromKnob`; row 63's note-value names), in `dsp/src/TempoProducer.cpp`
out of ITCM. Then revision 8 as one commit: events 6–10 in `Engine::EventType` and `ApplyEvent`
(the gap rule before every event, unknown types and invalid payloads counted), the stored
performance state applied at load step 4 and played by `Restart`, Spillover's time mode,
subdivision and recall, CLOCK births in `GranularCore` (§6.3, D13), `Engine::Tempo()` and
`TempoCounts()`, rows 83–85, CLOCK supported, corpus version 13 (seven clock presets on
`plucks_clock_30s` at 140 and 137.5 BPM over three packages, the five script verbs, the
`ClockTicks` and `TapSeries` generators, twenty counters, the `clock`, `tempoEvents` and `subdiv`
ablations) and the re-mint: all 45 presets of revision 7 reproduce their hashes, per-second hashes
and counters bit for bit. `dsp/tests/test_clock.cpp` checks the wiring (hits at F(k) for four
grids, block-split invariance with every event type, D13's steal, the jitter's bound, no stale
hit on a switch to a clock mode, `Restart` against `Init` and an Exact load, Spillover recall).

Where the text left a choice, or the code read differently, the build follows the design's
intent, as below (continuing §11.10's numbering):

15. **The Time knob's routing is the producers'.** §6.4 and Q7 keep the Time knob from
    `macro.time` in Subdiv and Tempo modes while expression, MIDI and hosts still reach it, but
    every one of them reaches it as the same MacroMove event, which carries no origin. So the
    engine plays every MacroMove it is given, and the routing is the producers' (T3, T4): the
    engine's side is the time mode itself (Subdivision field 1, reported in `Tempo().timeMode`),
    the effective subdivision Tempo mode forces, and `TempoNsFromKnob` for the Tempo knob's
    events. `Engine.h` states it beside the re-assert contract of §2.5.
16. **No lateness cap yet.** The cost governor does not exist, so no hit waits for admission:
    each is born at its (jittered) frame, `clockDeferred` and `clockDeferredFrames` read 0, and
    `kClockLateFrames` lands with whichever of the governor's revision and this one comes second,
    as §11.3 says, i.e. with the governor. Hits waiting for their jittered frame queue in
    `GranularCore`, at most eight (two at most at jitter 1, since a hit waits at most half a grid
    period); one past that is dropped and counted in `clockDropped`, which only placements at
    nearly every frame could reach.
17. **Row 85 has no domain.** A Global row rebuilds something everywhere else; `tempo_recall`
    acts only where a Spillover load reads it, so the table's well-formedness rule admits it as
    the one Global row without a domain. A load reads its latest stored value, so an unstamped
    `SetParam` before a direct Spillover call counts as at the load's frame, which split delivery
    needs. Rows 83 and 84 are Performance rows, automatable as §10.4 lists them, and act only
    through Subdivision events: a `SetParam` on them does nothing, and the plugin does not
    register them yet (T3).
18. **Split delivery of events 6–10.** As §8.3 says, the harness hands each to the block that
    starts at its frame, at offset 0. At a frame shared with an unstamped event the unstamped one
    applies first (a direct Spillover load at once, a `SetParam` at the block's start), where the
    engine's transport would apply them in sequence order; the corpus keeps such frames apart,
    and no preset reorders.
19. **A restart's tail sets the device settings.** RestartTail renders from Init's device
    settings, while a restart keeps them, so `clock_loads` (a recall set to Preset before its
    Exact load) could not be rebuilt. The tail now sets, at its frame 0, every Global row the
    script set before the restart; no earlier preset sets one before a restart, so no earlier check changes.
20. **`lastClockBirth`** is the grain core's last CLOCK birth, taken when the span's count of
    them grew, so a `Restart` never reports the frame of an earlier timeline.
21. **Every package re-stamped.** As at revision 5, the corpus's 29 packages, the compiler's 7
    examples and the factory's 18 carry `sound_rev` 8: each package hash changes, every
    `sound_hash` and `control_hash` stays, and the sound-revision gate's package rule passes
    without the package-change label. The compiler's two digests (the stamp alone) and the blob
    fuzzer's verdict digest (the samples' revision and CLOCK supported) are re-minted.
22. **ITCM** (*measured*, the live image's map): `.itcm_text` grows from 62,256 to 64,680 bytes
    (`Engine.cpp` 15,222 to 16,148, `Granular.cpp` 11,404 to 12,812), leaving 792 bytes of the
    65,536 with its 64-byte offset; `ItcmCheck` and `BootCheck` pass on every image. The tempo
    objects stay in QSPI: `Tempo.cpp` 14,008 bytes and `IntMath.cpp` 1,256 (`MidiClock.cpp` and
    `TempoProducer.cpp` are not linked into the live image). §9.6 estimated 1.0–1.6 KiB of ITCM
    for T1 and T2 together; T1 alone takes 2.4 KiB, mostly the CLOCK path and the draw ordinal in
    `GranularCore`. §9.6's cold-code move (the main-thread API out of `Engine.cpp`'s ITCM object,
    or placement by function) is not built: D1 as amended keeps code that fits in ITCM, and it
    fits. T2's crossfade and slew in `PostChain`'s per-sample loop will not fit 792 bytes without
    it. *(2026-10-10: built by function, note 31: 61,472 bytes, 4,000 spare.)*
23. **Not built in T1:** the nightly ten-minute render of §8.3 (a CI schedule) and §8.2's post-
    chain cases, `sync_post`, `sync_base`, `sync_fold` and the `sync`, `crossfade` and `slew`
    ablations, which belong with synced times (T2, corpus version 14). `crossfades` and `folds`
    read 0 until then.
24. **The commit budgets, in the corpus.** `clock_midi_computer` (137.5 and 150 BPM, a computer's
    jitter) commits 26 times in about 22 seconds of clock, 23 of them drifts, as item 12 measured;
    its counters are minted as they are, and §7.4's budget remains the owner's decision on the
    constants (note 32).

### 11.12 As built: the review's amendments (2026-10-10)

Three reviews of lane T1 at `b91b34d` (determinism and exactness, behaviour against this design,
the embedded side) found no correctness, determinism or portability defect and four places where
the text, read literally, gave behaviour its own intent rules out. Revision 8 is amended in place,
before any later revision and unpushed, so it stays revision 8 with its golden file re-minted:
the 45 presets of revision 7 still reproduce their hashes, per-second hashes and counters bit for
bit, and of the seven clock presets `clock_internal` (five CLOCK births fewer, note 25),
`clock_loads` (the frame-0 hit, note 26; its output, rendered from a ring the load cleared, is
unchanged) and `tempo_jump` (new events, note 33) changed. Continuing the numbering:

25. **A change of the grid starts the new grid at its next position** (§6.3's step 1 amended).
    The catch-up exists for a jump of the phasor: the boundary inside the frame before a span's
    start, and the one position a placement, a tap or a rescale made due. A change of G (a
    Subdivision event, either field; a load's stored Subdiv or time mode; D18's gesture) moves no
    boundary, but read literally the catch-up then fired the finer grid's last passed position at
    the change's frame, up to a whole new-grid period late: TAP to ×2 to ×4 to ×8 turned 18 ticks
    into a beat at 120 BPM fired an eighth 6,000 frames (125 ms) late and a sixteenth 300 frames
    late, on the main performance control (CC#5). Now the new grid's positions whose first frames
    lie before the change count as fired, and one due exactly at the change's frame (F(g) = s,
    acc < K) still fires there. A load that also recalls a tempo applies the grid after the
    rescale, as a Tempo event and then the grid would. Coarser grids never caught up (they nest).
    Note 10's listening question is answered by this rule.
26. **The grid is held while a MIDI transport is armed** (§3.4 and §2.5 amended). An armed
    Start, Continue or Locate waits for its tick, which relabels and places the phasor; until
    then the old grid ran on. So after an Exact load under a running master the restarted grid
    fired position 0 at frame 0 (with its burst), off the master's beat, before the re-asserted
    Continue applied, which D4's "a preset change under a running master keeps the band's beat"
    rules out; and FA under ClockFree at ×8 let the old grid's hit fire 6–20 frames before the
    downbeat's own (a flam, at 21 of 48 phases probed). Now nothing fires while an event is
    armed, and the grid positions passed count as fired, so a Stop that cancels the event lets
    the grid resume at its next position. The hold changes only at events, so it is block-split
    invariant. An armed event that no tick follows holds the grid until a Stop, a later armed
    event's tick or `Restart`, as a slave waits for the clock after Start.
27. **Song Position under ClockRunning sets the continue position too** (§3.4's Locate row). It is
    armed and applies at the next tick as Start, as before; a Continue that replaces it before
    that tick (a master relocating with F2 then FB, no FC) read the continue position of the last
    Stop and left the grid about 4,000 frames off every beat until the next Start. §3.4: Continue
    resumes "from the last stop point or Song Position".
28. **Song Position under ClockFree sets only the continue position** (§3.1's sentence amended to
    §3.4's table, which the code follows): the beat label stays the pedal's own until a Start or
    Continue applies at a tick, or a tap marks the downbeat.
29. **The engine's calls into the core stay in ITCM unless something can happen** (§9.6). Since
    revision 8 `RenderFrames` called `GridFrames` on every render span of every preset, and
    `ApplyEvent` called `BeforeEvent` before every event, both in QSPI: about 2–4k cycles a block
    with a cold I-cache (*estimated*, §9.6's 100–200 cycles a line), which §9.6's table did not
    count, and the firmware comment said the tempo code ran at control rate. Now `detail/Tempo.h`
    has inline fast paths compiled into `Engine.cpp`: a span that ends at or before the first
    frame at which a grid position can fire (kept by the full path after every span that may fire,
    cleared by every event and load) only advances the phasor, at most eight loop turns; an event
    with no gap due needs nothing; the grid's period and Pc in ns are kept from their last change,
    so a hit's jitter and `Tempo()` divide nothing, and `Info()` is inline (the audio callback reads
    it after every block, §9.3, §9.5). The core's state is unchanged (Euclidean advance), so no
    golden hash or digest moved for this. `ItcmCheck` now refuses any veneer in ITCM, i.e. any
    call out of ITCM, other than the five control-rate entry points it names (`GridFramesFull`,
    `BeforeEventFull`, `ApplyEvent`, `CountUnknownEvent`, `SpilloverLoad`), so a hot call into
    QSPI fails the build; there were ten such veneers at `b91b34d`, five remain.
30. **A ClockTick's cost, and the dropout tick.** The window's eviction re-based all four sums once
    per leaving tick (six 64-bit products each), so one tick cost O(evicted): a dropout tick at 140
    BPM evicts up to 55 entries, at 300 BPM the whole window. It now subtracts each leaving tick's
    own terms (two products) and re-bases once, the same exact integers (the digests and every
    reference comparison are unchanged by it), and clears in O(1) when every entry leaves: about 36
    instructions per evicted entry on the M7, from about 94 (*measured* in the disassembly). The
    tick path's code is about 6.4 KiB at -O3 (`OnClockTick` 1,656 bytes, `ApplyEvent` 616,
    `WindowEvict` 654, `BeforeEventFull` 442, `Commit` 400, `Refit` 338, `PlaceOnLine` 244,
    `WindowAdd` 234, `IntMath`'s multiply-divide 814 and its wrappers; *measured*), three to four
    times §9.6's 1–2 KiB, so a tick with a cold I-cache may refetch up to about 200 lines, 20–40k
    cycles (*estimated*), against §9.6's 3–10k. T6's worst-tick bench, with the I-cache invalidated,
    must include a dropout tick that evicts most of the window and ticks bunched two to a block;
    the governor's event reserve (24k cycles, a placeholder) is sized from it, and if it does not
    fit, the tick handler moves into ITCM (§9.6), which now has room (note 31).
31. **Main-thread engine code out of ITCM** (§9.6's cold-code move, by function). Functions
    marked `BRAINSCAPE_COLD` (`dsp/src/detail/Placement.h`: `Engine`'s constructor, `Init`,
    `LoadPreset`, `Restart`, `ClearHistory`, `SetParam`, `GetParam`, `TempoCounts` and
    `Descriptors`, `Impl`'s `Init`, `Restart`, `ClearHistory`, `LoadPreset`, `SetParam` and
    `GetParam`, and `PlanMemory`) go to `.text_cold`, which the ITCM input patterns do not match
    and the QSPI `.text` section's `*(.text*)` does; only on bare-metal Arm, so the firmware's and
    the emulated M7's archives stay byte-identical and nothing else changes. The core is created
    and its counters copied out of line (`TempoCore::Create`, `Counts`), so none of that code is in
    ITCM either. The live image's `.itcm_text` goes from 64,680 to 61,472 bytes (*measured*;
    `Engine.cpp` 16,148 to 12,980), leaving **4,000 bytes** of the 65,536 with its 64-byte offset;
    parity 59,336, bench 59,192, bench_hooks 59,448 (62,544, 62,400 and 62,880 at `b91b34d`). The
    fast paths of note 29 cost 380 bytes of it. Moving `Validate` and the rest of §9.6's list
    stays for when T2 or the governor needs it, after bench session 2 (cpu-budget.md §7.3).
32. **The commit budgets, measured against a candidate** (notes 12 and 24; §11.7 risk 3). A review
    measured ten minutes per case, three seeds, commits counted after the window reached 96: as
    built, a computer clock's worst is 115 commits at 120 BPM, 189 at 140 and 657 at 300, and a
    hardware clock's one at 300 BPM. A variant where rule 3.1's band must be exceeded on 48
    consecutive fitted ticks (counted like rule 3.2's run, reset by a fitted tick inside the band)
    and rule 3.2 moves to Pc >> 11 over 384 fitted ticks commits at most once at 60, 120 and 140 BPM
    and 8 times at 300 with a computer clock, never with a hardware clock; its worst |Pc/true − 1|
    is 0.097, 0.193, 0.225 and 0.340 %, and it still follows a 1 % ramp over 600 beats. Rule 3.1 at
    Pc >> 8 alone fails at 300 BPM (189). The constants stay as designed: changing rule 3.1 from
    "at once" to a run reshapes D8's deadband, and §11.7 risk 3 and §11.8 item 1 set these
    constants from T0b's recordings of real masters. The variant is the measured candidate for the
    owner's decision; adopting it changes `clock_midi_computer`'s counters and is revision 8's
    re-mint if it lands before the next revision, a revision of its own after.
33. **Tests added** (§8.2): every exact integer threshold of §3.3 and §7.1 with a value at it and one
    past it (the dropout's R/10 and four ticks, the outlier's quarter tick and 10 ms, the early
    commit, rules 3.1 and 3.2, the Jump class), which twelve mutants of `Tempo.cpp` had survived;
    notes 25–28 case by case; the engine's one CLOCK hit per frame (a Start whose first ticks
    arrive bunched on one frame: the catch-up and the next grid point at one frame), a jittered
    hit dropped by a load to a mode without `clock`, the gap rule before a `SetParam`, and an
    Exact load under a running master firing nothing before its tick; the reference model across
    2^31, 2^32 and 2^33 frames (the window's 32-bit storage, with dropouts, re-acquisitions and
    relabels); and the reference's perturbations for notes 25–27, each caught. The streams send
    a MIDI Start or Continue up to half a second before their first tick, so the hold runs over
    grid points, and the tempo digest is `0d1b22dd690c3742`. The corpus's `tempo_jump` ends with a
    MIDI Start whose 49 ticks arrive on one frame, pinning one hit per frame on every leg, the
    emulated M7's included. The clamp `acc′ = max(1, …)` of §2.2 stays untested: only a placement
    can leave acc at 7 phasor units or less before a rescale, and it is defensive.

## 12. Evidence

**The Microcosm** (manual v1.13, https://cdn.shopify.com/s/files/1/0920/2928/8752/files/MC_manual_WEB.pdf;
printed and PDF page numbers agree). *Documented:* tap "smoothly matches the tempo to the timing of
quarter note taps" (p. 6); tap works in Tempo mode (p. 5) and not under external clock (p. 21);
Subdiv "determines the musical subdivision of the effect" and Tempo mode forces quarters (p. 5);
CC#5 Subdiv 0–5 = 1/4, 1/2, TAP, 2x, 4x, 8x, CC#10 Time, CC#93 tap (p. 20; midi.guide); the
looper's "'TAP' interval (1X)" and "1/4 to 4x" (p. 15); the Time knob's other mode sets the
"Global Tempo" (p. 5); Start switches to external clock and Stop reverts, and the pedal can also
transmit MIDI clock (p. 21), with soft thru on by default and a four-way thru and clock setting
(`docs/research/microcosm.md:459-464`); global configuration is not saved in presets (p. 18).
*Not documented anywhere searched:* tap averaging and timeout, Continue, Song Position, clock
loss, the Time knob under clock. *Field reports* (Elektronauts thread 121669): posts #734 and #743, a stop and start realigns
the beat; #741, presets break the clock's phase; #348–#350, effects stay locked while the looper
drifts; #496, the 1/4 and 1/2 positions sound sparse; #508, the wish for direct tempo control.
Guitar Pedal X's pre-release listing calls Time "Tap-Tempo / Tap-Division". The "drifting, clicks
and pops" line of `docs/research/microcosm.md:552` has no source on the page it cites.

**Peers.** Strymon TimeLine (manual RevE: per-preset Tap Mode PRESET/GLOBAL, MIDI Clock Reset and
Sweep); Chase Bliss Big Time (MIDI manual pp. 1, 5, 7, manual pp. 12, 17, 30–31: tap snaps the
slider to its middle, clock "always snap[s] to the incoming quarter note", CC#54's thirteen
divisions); Chase Bliss Thermae (GLIDE between steps; clock to "avoid tempo drift"); Red Panda
Particle 2 (manual for 2.2.0+, pp. 23–24, 33–35: per-parameter divisions, "+/-1 milliseconds of
jitter", follows clock without Start, receive clock global); EHX 95000 (pp. 13, 15, 32–33: the mean
of the last four taps, 60–240 BPM, Start/SPP in XT mode, 1–3 bars to settle); Meris LVX (TAP GLIDE,
TEMPO SELECT); Boss DD-500 v2.00 (TEMPO HOLD); Eventide H9 (tap averages "more than 2").

**Protocols and estimation.** MIDI 1.0 real-time and Song Position (Clemson's specification page;
jgglatt's `seq.htm` and `run.htm`): 24 clocks per quarter, Song Position in 16ths, Start on the next
clock, real-time bytes anywhere. E-RM's jitter report (2014, Table 1 p. 7): Ableton Live 9 as
master, σ = 8.43 ms cycle-to-cycle, −38.36/+22.24 ms peaks; as slave, tempo σ = 0.114 BPM.
Adriaensen, *Using a DLL to filter time* (2005). Least-squares factors (σ of the period as a
multiple of σₓ: 0.708 at N = 24, 0.0884 at N = 96; the next-tick phase 0.21·σₓ) are the protocol
investigation's calculations. Hosts: JUCE's `AudioPlayHead`, VST3 wrapper and `ProcessContext`,
AU wrapper, `AudioProcessorPlayer` and `MidiMessageCollector` as cited in §4.4 and §10.2; JUCE forum
threads on Reaper's loop blocks and Live's VST3 offline flag.

**The reviews of draft v1** (determinism and parity, the musician and the product, the firmware and
hardware) and their probes are in [the record](reviews/clock-record.md) §2.6–§2.9 and §6.

**The code** at `343f33c`, as cited throughout. The status survey and the code investigation
behind this design are not committed; the findings it relies on, several of which correct the
survey, are C1–C12, and C13–C18 come from checking the reviews:

- **C1.** "Restart on transport start" is off by default (`plugin/src/StateCodec.h:18`,
  `plugin/src/PluginProcessor.h:297`).
- **C2.** libDaisy runs USART1 and every DMA stream, the audio's included, at interrupt priority
  0 (LD `src/per/uart.cpp:897`, `src/sys/dma.c:17-34`), and receives MIDI by DMA with callbacks on
  line idle or a half or full 256-byte buffer (LD `src/hid/midi.cpp:5-17`).
- **C3.** Births at one frame share every draw, keyed by frame and purpose only
  (`dsp/src/Granular.cpp:102-115`), and a periodic birth is not skipped at a frame where a trigger
  fired (`:516-597`).
- **C4.** A new event type cannot slip through silently at build time: `ApplyEvent`, the golden
  harness's and the plugin's switches have no `default` (`Engine.cpp:1050-1098`,
  `EventScript.cpp:168-184`, `PluginProcessor.cpp:740-772`).
- **C5.** The `Subdivision` enum's index 0, `Quarter`, labelled 1/4, is the default of both
  stored subdivisions (`Mode.h:86`, `:94`, `PresetState.h:53`): under the Microcosm's reading, a
  whole-note grid.
- **C6.** Two subdivisions are stored, the mode's (SCHD) and the performance state's (STAT), and no
  document said which wins.
- **C7.** One compiler bit unlocks all five performance fields with CLOCK (`Schema.cpp:44-45`,
  `:1755`).
- **C8.** `sound_hash` covers all of STAT, `tempo_source` included (compiler §6.3), and STAT's
  tail has no spare byte (`Decode.cpp:526`).
- **C9.** An Exact load applies the performance state and then restarts (`Engine.h:126-130`,
  `Engine.cpp:722-730`).
- **C10.** The event queue has one producer and refuses a stamp below the last
  (`EventQueue.h:10-14`, `EventQueue.cpp:14-19`).
- **C11.** The 512-frame feedback FIFO cannot shrink: Pass 1 writes the whole block before grains
  render, and blocks reach 512 frames (`Engine.cpp:571`, `:1164-1167`, `:1213-1224`).
- **C12.** The live image has 3.6 KiB of ITCM left (`firmware/README.md:482-491`); the CPU
  proposal adds an FFT rewrite (6.3 KB, or 1 KB), its step 4 (3.75 KB) and the governor (about
  2.3 KB) and asks for 8 KB spare, so even with `Validate` (8.4 KiB) moved out the remainder is
  −0.35 or +4.95 KiB. *(2026-10-08, measured in the CPU plan's builds, cpu-budget.md §4.1: the
  live image has 3,632 bytes spare, `.itcm_text` starting 64 bytes into the region; P1's rewrite
  costs 8,016 bytes and does not link on wave 1's tree; P3's, built, costs 560, leaving 3,208;
  with `Validate` out the remainder is about +5.6 KiB before step 5 and the tempo core.)*
- **C13.** On the pedal an Exact load mutes, and a muted callback returns before `g_blocks`
  advances; the load then clears the queue and zeroes `g_frame`, `g_blocks` and `g_seq`
  (`firmware/live/main.cpp:94-98`, `:110`, `:390-403`), and `Clear()` resets the queue's order
  check (`dsp/src/EventQueue.cpp:56-63`). `g_blocks` is a 32-bit atomic (`main.cpp:72`).
- **C14.** libDaisy's UART stops listening after any receive error in DMA mode and only
  `MidiHandler::Listen()` restarts it (LD `stm32h7xx_hal_uart.c:2296-2321`, `src/per/uart.cpp:373-382`,
  `:1105-1109`, `src/hid/midi.h:194-205`); its DMA buffers belong in `DMA_BUFFER_MEM_SECTION`
  (LD `src/daisy_core.h:25`, `src/hid/midi.cpp:7-8`).
- **C15.** The live image's ITCM holds 61,840 of 65,536 bytes, `Engine.cpp` 15,246,
  `Validate.cpp` 8,612 and `DetMath.cpp` 5,496 of them (the `modes-impl` build's map), and
  `ValidateStructure` runs on every Spillover load (`dsp/src/Engine.cpp:315`, `:1068-1079`).
- **C16.** The post delay's line is round(2.0·R) frames and its target is clamped below its
  length (`dsp/src/PostChain.cpp:52`, `:177`, `:240-248`); a read is by distance behind the write
  head (`dsp/src/detail/PostChain.h:98-102`), so a longer line reads the same frames.
- **C17.** The golden harness's `StateAt` folds only `SetParam`, `SpilloverLoad`, `MacroMove`
  and `Expression`, and the restart tail Exact-loads its package (`dsp/tests/golden/Corpus.cpp:664-742`).
- **C18.** The plugin's restart loads `ToPreset` with the mode's stored performance state and
  checks the spare by leaves and mode only (`plugin/src/PluginProcessor.cpp:71`, `:575-587`).
