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
> and "JUCE" the plugin build's fetched 9.0 tree. Status: **draft v1**, nothing built. The owner
> has not answered the decisions of §11.5: each carries a provisional answer (2026-10-08),
> marked as such and reversible before the first public release.

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
| MIDI clock out and thru | Not designed (D10); the out/thru footprint is reserved on the schematic |
| USB-MIDI in | After the move to TinyUSB (`firmware/README.md:635-640`, companion §7.1); it feeds the same translator (§4.3) |
| Time signatures and bars | Not designed; grids align to position 0 of the transport in 24-ppqn ticks, so every grid up to a whole note is bar-aligned in 4/4 only |
| Which panel gesture toggles the time mode, the LEDs | The control-surface design; §6.4 says what the gesture does |
| The pedal's MIDI CC map | The firmware's MIDI design (compiler §1.2, deferred); §6.5 recommends the Microcosm's CCs |
| The CPU budget's fix (the cost governor) | The CPU proposal under design; §9.6 says how CLOCK births enter it |

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

### 1.4 Principles

1. **Tempo changes only at event frames.** The tempo state changes only when an event or a load
   applies; between them every tempo-derived quantity is a closed-form function of that state.
   So block-split invariance (contract #1) holds by construction, with no internal deadline
   that would have to split a block (§3.5).
2. **Integers only.** The phasor, the estimators and the durations use integer arithmetic, with
   no floating-point arithmetic, so they need no FP guard (`dsp/src/EventQueue.cpp:5-6`) and can
   run from QSPI flash, out of the ITCM (§9.6). The only floating-point step is reading a
   Transport event's position (§4.1), inside the engine's guard.
3. **The engine estimates; producers stamp.** Tap averaging, clock following and arbitration
   run in `dsp/` on the frames where events applied (compiler §7.4), so a logged stream replays
   bit-identically on any build. Producers stamp, filter by device setting, and log.
4. **Microcosm parity where the Microcosm documents its behaviour**, its peers' behaviour where
   it is silent, every such choice a provisional answer in §11.5.
5. **Exact loads start from the stored state; Spillover loads never move the beat phase.** The
   Microcosm losing sync on preset changes is a documented field defect (§12).

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

**Integer helpers** join `dsp/src/detail/DetMath.h` (or a new integer-only `IntMath.h`):
`MulDivRoundU64(a, b, c)` = round-half-up(a·b / c) with a 128-bit intermediate built from
32-bit limbs, because 32-bit `arm-none-eabi` GCC has no `__int128`; a signed wrapper
`MulDivRoundI64` that rounds the magnitude; and `FloorDivI64`, `CeilDivI64`, `FloorModI64`,
since C++ `/` truncates toward zero and `>>` of a negative value is implementation-defined in
C++17. They run at control rate only. The symbol audit already allows the 64-bit division
helpers they compile to (`tools/ci/audit_symbols.py:24-27`).

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
- **The frame of boundary k > tick:** F(k) = s + CeilDiv((k − tick)·P − acc, K), the first frame
  at or after the boundary; (k − tick)·P ≤ 192·P < 2⁶³.
- **Placing boundary k exactly at frame f:** `tick = k − 1, acc = P`, so F(k) = f.
- **Init and Restart:** boundary 0 at frame 0 (`tick = −1, acc = P`).
- **A tempo change** P → P′ keeps the fraction of the current tick:
  `acc′ = max(1, MulDivRoundU64(acc, P′, P))`.
- **Exactness:** at constant P, F(k) = s₀ + ⌈(k·P − c₀)/K⌉ for a constant c₀: the ideal grid of
  a tempo of exactly P, with nothing accumulated, so the phasor itself never drifts. The only
  error against the source is P's representation (§2.1). A ten-hour unit test checks F(k)
  against the closed form at every beat (§8.2).

**The phasor only moves at event frames** (§1.4): taps, clock ticks, transport events and the
clock-follower's refit place it (§3), tempo changes rescale it, and render spans advance it.

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
| `Restart` | **kept** | **kept** | boundary 0 at frame 0; `lastFired` = −1 | **cleared** to Init's: frames restart at 0, so every frame-based history is void |
| Exact load | the stored tempo, always | stored | Restart's | Restart's |
| Spillover load | kept, unless `tempo_recall` is Preset and the source is Internal: then the stored tempo, phase-continuous and committed at once | stored | **kept** | kept |
| A mode change (`InstallMode`) | kept | as the load says | kept | kept; only sequencing state resets (`Engine.cpp:739-748`) |
| Spillover epoch (`ApplySpillover`) | — | — | not tied to the epoch: the phasor counts absolute frames | — |

`Restart` keeps the tempo value, time mode and subdivision because an Exact load applies the
stored performance state at step 4 and then restarts at step 5 (`Engine.h:126-130`,
`Engine.cpp:722-730`); clearing them there would discard what step 4 set (code finding C9).

**Exact loads play the stored tempo (D4).** They define the exact-restart start state of the
parity contract (profile §2.1), so they cannot depend on the tempo before the restart. On the
pedal an Exact load also mutes for 47.2 ms (`docs/design/reviews/rev7-silicon-record.md` §3.6)
and drops the queued events (`Engine.h:106-107`): 2.3 MIDI ticks at 120 BPM and 5.7 at 300
(*calculated*). Where the pedal was following clock before the load, its control loop
re-asserts the running tempo with a Tempo event at frame 0 of the new timeline, a logged event
(§9.3), and the follower re-acquires from the next ticks (§3.3): "external clock always wins"
holds in practice without making the start state depend on history.

**`global.tempo_recall`** (row 85, a `Global` device setting, §10.4): **Keep** (0, default, the
Microcosm's global tempo) or **Preset** (1, a song preset recalls its tempo, as Strymon's Tap
Mode PRESET, Meris' TEMPO SELECT and Boss's TEMPO HOLD offer, §12). A Spillover load under
Preset applies the stored tempo as a Tempo event at the load frame would; under external clock
the clock wins and the stored tempo is ignored. No load moves the phasor (principle 5).

### 2.6 Where it lives, and the API

**Placement.** `TempoCore` (`dsp/src/detail/Tempo.h`, `dsp/src/Tempo.cpp`, integer-only) lives in
the Warm arena beside the active mode: the follower's window (96 × two `int32_t`, §3.3), a
6-entry ring of the last ticks, the tap chain and about 120 bytes of scalars, about 1.1 KiB
(*calculated*), inside the Warm arena's 7,968 spare bytes (`firmware/platform/Placement.h:20`,
`:25`). `Engine::Impl` gains a pointer; DTCM grows by about 80 bytes with §6.3's pending clock
birth and §7.3's second head (*estimated*), inside `kEngineImplBytes` (7,944 of 8,192 bytes at
revision 7, compiler §7.1). §9.6 has the code placement.

**API** (`Engine.h`): event types 6–10 (§4.1); and two audio-thread snapshots, read like
`Stats()` (`Engine.h:278-287`):

```cpp
struct TempoInfo {           // Engine::Tempo(): what displays and logs show
  int64_t  position;         // the phasor's tick at the last rendered frame
  uint32_t nsPerQuarter;     // the committed tempo, Pc, in ns per quarter
  uint8_t  source;           // 0 Internal, 1 ClockFree, 2 ClockRunning (§3.1)
  uint8_t  timeMode, subdiv; // as stored or last set (§2.4)
  uint8_t  flags;            // bit 0 transport running, bit 1 follower locked (N ≥ 24)
  int64_t  lastClockBirth;   // frame of the last CLOCK grid birth, or -1 (§8.5)
};
struct TempoStats { uint64_t taps, tapsIgnored, tempoEvents, tempoIgnored, ticks, tickOutliers,
                    reacquires, dropoutTicks, losses, transports, subdivEvents, clockBirths,
                    commits, crossfades, folds, invalidEvents, unknownEvents; };
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
| **ClockFree** | the follower's fitted line; the position labels continue the phasor's | the deadband of §7.1 | 24 ticks in the window and no Stop seen since the last Restart | Start or Continue at a tick; loss; Stop |
| **ClockRunning** | the fitted line; the position is the master's | the deadband | an armed Start, Continue or Locate applying at a tick (§3.4) | Stop; loss |

The **plugin host** is not a source of its own: the wrapper turns the host's tempo and transport
into Tempo and Transport events (§4.4), which the engine plays as Internal, and while it follows
the host the wrapper drops taps and the Tempo knob's events before they are stamped (§10.1).
MIDI clock reaches a plugin only in the Standalone (§10.2): no plugin format delivers it, VST3's
events being notes, pressure, data, CC output, note expression, chords and scales (JUCE
`juce_audio_processors_headless/format_types/juce_VST3Common.h:1509-1540`).

**Stop reverts to Internal** at its frame, keeping P, Pc and the phase (the Microcosm, §12), and
sets `transportSeen`; ticks after a Stop keep the follower's window warm but drive nothing until
Start or Continue, so a master that sends clock while stopped does not drag the pedal into
ClockFree. **Clock without any transport message** (some clock boxes never send Start; peers
follow it, §12) enters ClockFree after 24 ticks: tempo and tick phase lock to the master, and the
beat label is the pedal's own until a Start, Continue or Song Position arrives.

### 3.2 Tap

Taps are Tap events (6); their frames are the frames where they applied. With I the interval in
frames since the chain's last tap (integers throughout, mean = Σ/n over the chain's intervals):

| Case | Test | Action |
|---|---|---|
| Under ClockFree or ClockRunning | — | ignored, counted (`tapsIgnored`); tap state unchanged (Microcosm: tap is unavailable under external clock) |
| First tap | no last tap | arm: last tap = f; nothing changes |
| Bounce or above 300 BPM | I < R/5 (9,600 at 48 kHz) | ignored; the last tap stays |
| Pause | I > 3R (below 20 BPM), or the chain has intervals and 4·n·I ≥ 7·Σ (I ≥ 1.75 × mean) | new chain armed at f; nothing changes |
| A new tempo | the chain has intervals and 5·\|n·I − Σ\| > 2·Σ (more than 40 % from the mean) | chain := {I}; apply |
| Same tempo | otherwise | append I, keeping the last 4; apply |

**Apply:** `P = Pc = RoundHalfUp((Σ << 32) / n)` (Σ ≤ 4·3R < 2²³, so the shift fits), committed
at once, as an explicit intent; then **the tap is a beat**: with b = FloorDiv(tick, 24) and
x = (tick − 24b)·P + acc, the nearest beat is B = 24(b + 1) if 2x ≥ 24·P, else 24b; the phasor
places boundary B at f, and `lastFired` = max(`lastFired`, B − 1), so a forward jump skips the
grid points it passed and B itself fires at f unless it already has (§6.3).

So tempo is set by the second tap and refined by each tap to a mean of four intervals (EHX
averages the last four; Eventide "more than 2", §12); a pause re-arms instead of producing a
wrong slow tempo; halving the tempo takes three taps (the first long interval reads as a pause),
doubling it two. The Microcosm's "smoothly matches the tempo" is the post delay's glide (§7.2),
not slower estimation. No skip detection: it would make halving by tapping impossible.

### 3.3 The MIDI clock follower

**Why a fit.** A computer's MIDI clock jitters by σ = 8.43 ms cycle-to-cycle with peaks of
−38.4 ms (E-RM's Ableton Live measurement, §12), about σₓ = 3.4 ms per tick. A one-beat average
then wobbles about ±1 % (1.2 BPM); a least-squares line through the last 96 ticks (four beats)
gives σ ≈ 0.06 % (0.07 BPM), comparable to Live's own slave smoothing (σ = 0.114 BPM), and
0.005 % for a hardware clock stamped on the pedal's block grid (σₓ = 0.29 ms) (*calculated*,
§12). The status survey's first recommendation, MIDI clock "averaged over one beat (about
0.2 %)", holds for hardware clocks only (D8).

**Window.** Up to N = 96 fitted ticks as `int32_t` pairs (x = label − label₀, y = frame −
frame₀), re-based when the oldest leaves; plus a 6-entry ring of the last received ticks
(label, frame), fitted or not.

**The fit,** whenever the window changes and N ≥ 2, in `int64_t`: Sx = Σx, Sy = Σy, Sxx = Σx²,
Sxy = Σxy; D = N·Sxx − Sx²; slope numerator A = N·Sxy − Sx·Sy (frames per tick × D); intercept
numerator B = Sy·Sxx − Sx·Sxy. The fit is valid when D > 0 and A > 0. Then
`P_fit = MulDivRoundU64(24·A, 2³², D)`, clamped to the tempo range, and the fitted time of label k
is T̂(k) = frame₀ + (B + A·(k − label₀))/D. With y ≤ 2.3·10⁶ (96 ticks at 20 BPM and 192 kHz)
every sum and numerator fits `int64_t` (*calculated*). Cost: about 96 multiply-adds and one
128-bit division per tick, about 1,000–2,000 cycles from QSPI flash (*estimated*), at most 120
ticks per second.

**On each ClockTick at frame f,** after §3.5's loss check:

1. **Label.** Normally the last label + 1: one label per received tick, so a jitter burst never
   corrupts the position. A **dropout**, when N ≥ 2 and f − (last tick's frame) ≥ max(4 ticks of
   the fit, R/10), infers m = round((f − last)·D/A) − 1 lost ticks, labels the tick last + 1 + m
   and counts m (`dropoutTicks`); 100 ms exceeds the worst computer-clock burst measured, so jitter
   never looks like loss. Shorter gaps are not inferred (§11.6 item 6): a lost tick then
   offsets the labels by one tick until the next Start, Continue or Song Position.
2. **Outlier.** From N ≥ 8, with ρ = f − T̂(label), the tick is an outlier when
   |ρ·D| > max(A/4, (R/100)·D): more than a quarter tick and 10 ms off the line. An outlier keeps
   its label but is not fitted. Six outliers in a row of one sign mean the master changed tempo:
   **re-acquire**, replacing the window with the 6-entry ring (`reacquires`).
3. **Fit,** if the tick was fitted.
4. **Place the phasor** (ClockFree and ClockRunning only) on the fitted line at f, with k_L the
   tick's label: d = MulDivRoundI64(K, (f − frame₀)·D − B − A·(k_L − label₀), D), the signed
   offset of f past T̂(k_L) in phasor units; `tick = k_L + CeilDiv(d, P_fit) − 1`,
   `acc = d − (tick − k_L)·P_fit`, so 0 < acc ≤ P_fit; P := P_fit. With N < 2 or an invalid fit
   the tick's own frame is its boundary (`tick = k_L − 1, acc = P`, P unchanged). Then
   **catch-up:** if the largest grid position g ≤ tick exceeds `lastFired`, it fires at f and
   `lastFired` := g (one at most; §6.3).
5. **Commit** Pc by §7.1 (ClockFree and ClockRunning, N ≥ 24).
6. **Acquire:** under Internal with N reaching 24 and no Stop seen, enter ClockFree, relabelling
   the window so the newest tick's label is the phasor's nearest tick (the grid does not jump by
   a tick), then steps 4 and 5 with Pc := P_fit.

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
| Start, position p | armed; at the next tick, that tick's label becomes p (the window relabelled, keeping its tempo data), `lastFired` = p − 1, the phasor placed (§3.3 step 4) so the downbeat fires at that tick or by catch-up; source ClockRunning | boundary p placed at the event's frame, `lastFired` = p − 1; the transport runs; source unchanged |
| Continue | armed; as Start with p = the stored continue position | as Start with the stored position |
| Locate p (Song Position × 6) | when not ClockRunning: the continue position := p at once; when ClockRunning: armed, applied as Start | as Start, the transport's running state unchanged |
| Stop | at its frame: ClockRunning → Internal (phase and tempo kept), the continue position := last label + 1, `transportSeen` set; ticks keep the window warm | at its frame: the transport stops; nothing else changes, the grid runs on |

A host-style Transport (no flag) under ClockFree or ClockRunning is ignored and counted: two
transports cannot both own the position. An armed event waits until a tick arrives; `Restart`
clears it, and a later armed event replaces it. Start's position is 0; the MIDI translator sends
Song Position as Locate with 6 × its 16th-note count, the plugin its tick-aligned host position
(§4.4).

### 3.5 Timeouts without deadlines

A **loss** is f − (last tick's frame) ≥ R (one second without a tick), checked lazily: before any
tempo event at frame f is handled, if the source is ClockFree or ClockRunning and the last tick
is a second or more behind f, the loss is applied first: source → Internal with P, Pc and the
phase kept, the window cleared (`losses`). The loss itself changes nothing audible: between ticks
the phasor already ran on at P_fit, and Pc does not move. So applying it at the next event
instead of at its deadline gives the same bits, and no block is split at the deadline: every
effect of the loss (a tap now accepted, a Tempo event now applied, a new window) shows only at
that next event's frame. A unit test compares this with a reference that applies the loss at the
deadline frame (§8.2). The dropout rule of §3.3 is evaluated the same way, at the next tick.

### 3.6 Priority

| Event | Internal | ClockFree | ClockRunning |
|---|---|---|---|
| Tap | applies (§3.2) | ignored, counted | ignored, counted |
| Tempo (knob, app, host, recall) | applies | ignored, counted | ignored, counted |
| ClockTick | feeds the window; may acquire ClockFree | follows | follows |
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
| Transport | 9 | bits 0–1 kind (0 Stop, 1 Start, 2 Continue, 3 Locate); bit 8 AtNextTick; other bits 0 | Start and Locate: the position in 24-ppqn ticks, an integer in [0, 6,291,456); otherwise +0 | Stop at its frame; the others at their frame or, with AtNextTick, at the next ClockTick (§3.4) |
| Subdivision | 10 | bits 0–7 the code, bits 8–15 the field: 0 subdivision (code 0–5, §5.1), 1 time mode (0–2); other bits 0 | +0 | at its frame |
| GlobalReverse | 11 | reserved for the rest of W2 | | |

- **Position range.** 6,291,456 = 96 × 2¹⁶ ticks is a multiple of every grid (3 to 96 ticks) and
  of a 16-step pattern of whole notes (1,536 ticks), and below 2²⁴, so every position is an exact
  binary32. Producers reduce a host position modulo it (floor-mod, so pre-roll wraps to the same
  grid phase); Song Position's largest value, 16,383 × 6 = 98,298, is inside it. The engine
  converts the value inside its guard after checking it is an integer in range (profile §3.10).
- **Song position (D11)** needs no event of its own: it is Locate with AtNextTick.
- **The Tempo event's value is reserved** (+0): taps and knob tempo under host sync are dropped
  by the wrapper, not flagged for the engine (§3.1).

### 4.2 Validity and counting

An event whose payload breaks the table (a tempo out of range, a nonzero reserved bit, a code or
field out of range, a non-integer or out-of-range position, a nonzero value where +0 is
required) is ignored and counted (`invalidEvents`), never clamped, because a clamped event would
replay differently on a build with another range. An event type above 10 is ignored and counted
(`unknownEvents`) before `ApplyEvent`'s switch (`Engine.cpp:1050-1098`), which keeps no `default`
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
  integer counts as that integer), and its frame offset round((k − x)·ns·rate / (24·10⁹)), less
  than one tick; the position is k reduced modulo 6,291,456.

**Per host block, while the Tempo source is Host and the host reports a tempo** (§10.1):

1. **Tempo.** At the block's first frame, a Tempo event when ns differs from the last sent by
   1,000 or more (1 µs per quarter, 0.0002 % at 120 BPM): hysteresis, so a host's rounding noise
   never moves P and the output's bits.
2. **Transport start** (the edge `CheckTransportStart` already detects,
   `PluginProcessor.cpp:529-542`), after the optional restart: a Tempo event, then Transport
   Start at `HostAnchor`'s position and offset from the block's first frame (later than the block
   when the offset says so; the wrapper's pending list holds it, as it holds late note-ons,
   `:967-982`). Without a ppq, Start at position 0 at the block's first frame.
3. **While playing,** predict this block's x from the last block's x and frames at the last
   tempo; when the reported x differs by more than half a tick (a loop wrap, a jump), Transport
   Locate anchored as in 2.
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

**The rule (D20):** every producer stamps an event `(c + 2) × 48`, where c is `g_blocks` when the
event was captured (in an interrupt for MIDI bytes and the tap switch, in the main loop for the
console and knobs), and the main loop, the queue's single producer (`EventQueue.h:10-14`), pushes
`max(stamp, last pushed stamp)` so stamps never fall (`EventQueue.cpp:14-19`). Two blocks (2 ms)
of fixed latency cover a main-loop iteration of up to about a millisecond between capture and
push; a later push applies a block late, which the control loop counts (a push whose stamp is
below `(g_blocks + 1) × 48`). Block-grid stamps never split a pedal block, which matters while
the budget is over (§9.6), and their σ of 0.29 ms (48 frames uniform, *calculated*) costs the
96-tick fit 0.005 % (§3.3). Sub-block stamps from the cycle counter (10,000 cycles per frame) are
an option only if step 0b shows the need (§11.8).

**Interrupts hand over, never push.** The UART's DMA callback and the tap switch's EXTI handler
write `(byte or tap, c)` into their own single-producer rings; the main loop drains the rings,
runs the translator (§4.3) and pushes in stamp order. An interrupt cannot push: the queue has one
producer and refuses a stamp below the last (code finding C10).

**Logs record the applied frame.** A session log (profile §6.7's capture, and step 0b's console
log) writes `(applied frame, seq, type, id, value bits)` in the audio callback as `PopBlock`
hands the events out (`firmware/live/main.cpp:105`), the applied frame being the block's first
frame plus the offset. Replaying the stamps instead would land a late tap a block early.

## 5. Divisions

### 5.1 Subdiv: six rate multipliers

The Microcosm's Subdiv positions, CC#5's values 0–5, are 1/4, 1/2, TAP, 2x, 4x and 8x (Microcosm
manual p. 20). Its manual never defines them in words, but four pieces of evidence agree that
they are **multiples of the tapped quarter-note rate, not note values** (D7, resolving
`docs/research/microcosm.md:112`): the looper's speed uses the same labels as multipliers, with
"the 'TAP' interval (1X)" its normal speed and a range "1/4 to 4x" (p. 15); Tempo mode forces the
subdivision "to quarter notes" (p. 5), so TAP is the quarter note and "1/4" cannot also be one;
the pre-release specification calls the control "Tap-Division"; and users report the 1/4 and 1/2
positions as sparse (§12). The design's names and default assumed note values
(`Mode.h:86`, `PresetState.h:53`, compiler §2.3 "default `1/4`"), which under this reading is a
whole-note grid (code finding C5).

| Code | Label (JSON) | Knob position, CC#5 | Rate | Grid G (ticks) | Grid at 120 BPM |
|---|---|---|---|---|---|
| 0 | `tap` (default) | 2 | ×1 | 24, quarters | 500 ms |
| 1 | `1/4` | 0 | ×1/4 | 96, whole notes | 2 s |
| 2 | `1/2` | 1 | ×1/2 | 48, halves | 1 s |
| 3 | `2x` | 3 | ×2 | 12, eighths | 250 ms |
| 4 | `4x` | 4 | ×4 | 6, sixteenths | 125 ms |
| 5 | `8x` | 5 | ×8 | 3, thirty-seconds | 62.5 ms |

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
(off) or a code 1–16 (`Mode.h:118`, `Params.h:248`): sixteen note values ordered by duration, so a
macro sweeping the code lengthens monotonically. Each is a whole number of ticks, a superset of
Chase Bliss Big Time's thirteen (§12):

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

### 5.3 Folding

A synced duration that leaves its target's range folds by octaves: halved (k += 1 in §2.3) while
above the maximum, doubled while below the minimum, each fold counted (`folds`):

| Target | Minimum | Maximum |
|---|---|---|
| Post delay | 10 ms (480 frames at 48 kHz, `post.delay.time_ms`'s minimum, `Params.h:203`) | the line's last frame, round(2.0·R) − 1 = 95,999 (`PostChain.cpp:148-150`, `:240-248`) |
| Grain base delay | 1 ms (48 frames, `base_ms`'s minimum) | 5,000 ms (240,000 frames, `base_ms`'s maximum, `Params.h:186`) |

A clamp would be wrong: a quarter at 30 BPM is 96,000 frames, one more than the longest tap, and
would play 95,999, one frame off the grid; folded it plays an exact eighth, 48,000 (*calculated*).
At the other end, a 1/32 under 8x at 300 BPM is 150 frames and plays as 600 (12.5 ms). A fold
change caused by a tempo change crosses an octave of delay, so it crossfades rather than glides
(§7.3). The fold never changes a note value's identity in displays or documents.

### 5.4 Names, JSON and display

- **Structure fields** (`base_sync`, modulator `sync`) are JSON strings, `"off"` or a name of
  §5.2 (`Schema.cpp:660-675`, `:1904-1907` today accept and write only `"off"` and a placeholder).
- **Row 63** stays a numeric leaf, 0–16 (`Schema.cpp:412`), because macros and expression target
  leaves by number (compiler §3.2) and read counting leaves as `RoundHalfAwayI32`
  (compiler §3.7); its display is the name.
- **`performance.subdiv`** stays a string of §5.1's labels; `scheduler.subdiv` is withdrawn (D14).
- **Display** (`dsp/src/ParamDisplay.cpp:455-464` prints "Div N" today): row 63 shows "Off" or the
  name with an upper-case suffix ("1/8D", "1/16T"), which the plugin's text parser reads back in
  either case; rows 83–85 show their labels (§10.4).

## 6. What follows tempo

### 6.1 The post delay: row 63

Row 63 becomes a `Leaf` in the synced-times revision (§11.3). When it is nonzero, the post
delay's target is §2.3's duration of its note value, folded (§5.3), in exact integer frames,
instead of `post.delay.time_ms` (`Engine.cpp:803`). `time_ms` keeps its stored value and plays
again when the code returns to 0. A committed-tempo change marks `kDomainPost` and the delay
glides to the new target (§7.2); a discrete change (code, Subdiv, time mode, fold) crossfades
(§7.3). The post delay is already the engine's tempo-exact part: its taps measured exact, 251 and
626 ms (`docs/design/reviews/mode-compiler-record.md:156-162`).

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

Lint **L12** (after the CPU proposal's L10 and L11) notes a layer with `base_sync` whose
`feedback.amount` is above 0 at the stored position or any macro corner: "grain-feedback repeats
fall 10.67 ms later per pass than the grid; tempo-exact repeats belong on the post delay". A
warning, never an error.

### 6.3 CLOCK births

**The source.** A mode listing `clock` (`Mode.h:78`) births on the grid: every position divisible
by G, the effective Subdiv's ticks (§5.1), counted from the transport's position 0 (`FloorMod`).
`periodic` and `clock` may both be listed: a cloud with clocked hits.

**Firing.** For each render span [s, e), `TempoCore::GridFrames` returns the frames of the grid
positions k > max(tick, `lastFired`) with F(k) < e (§2.2), at most `kMaxClockPerSpan` = 4 (the
shortest grid, 3 ticks at 300 BPM, is 200 frames at 8 kHz, so a 512-frame span holds three, plus
one catch-up), and sets `lastFired` to the last. They reach `GranularCore` like onsets, as offsets
in `TriggerEvents` (`dsp/src/detail/Granular.h:125-132`), bounded by a `static_assert` as the
onsets are (`Engine.cpp:178-180`). At its frame each fires through `FireTrigger` with a new
ordinal, `kOrdinalClock` = 3 (`Granular.h:119-121`; ordinals 0–7 exist, `GrainMath.h:60-86`), so:

- **intermittency** skips the whole hit, its burst included (purpose 9, ordinal 3,
  `Granular.cpp:409-421`);
- **burst** adds `burst.count − 1` grains after it at `burst.spacing_ms`, as any trigger does;
- **jitter** delays the hit, never advances it and never accumulates: at the grid frame a draw
  u (purpose `Interval`, ordinal 3) gives a delay of `(uint32_t)(jitter · u · 0.5 · gridFrames)`
  frames, gridFrames being G·P/K, so at jitter 1 a hit lands up to half a grid period late; the
  pending hit carries across spans in `GranularCore`;
- **allocation (D13)** is the trigger class's: the hit steals the oldest voice at `voice_count`
  (`FireExternal`, `Granular.cpp:375-407`), and `overlap` does not cap it. Engine §4's "under a
  CLOCK source … `overlap` then acts as a don't-fire ceiling" is withdrawn: a refused grid hit is
  a missing beat, and the rhythmic families the source exists for (engine §5's Seq, Arp, Pattern,
  Warp, Mosaic) are hits;
- **draw keys:** the grid hit's grain draws with ordinal 3 in the key extension (compiler §7.5,
  R8), so a hit sharing a frame with a periodic, onset or manual birth never stacks an identical
  grain (today births at one frame share every draw, `Granular.cpp:102`; code finding C3); its
  burst's later grains draw as every burst grain does;
- **one hit per frame:** a hit due where a clock hit already fired (a catch-up and the next grid
  point at one frame) waits for the next frame.

**The grid is never moved by births.** It is the phasor's, which births never touch, so no refusal
or deferral, today's or the cost governor's, can make it drift; contrast the periodic countdown,
a `float` that a refused birth shifts (`Granular.cpp:544-597`).

**Freeze** pins positions as for every source; hits continue. **Steps** (later W2) attach here:
the step index is ⌊position / G⌋ modulo the step count, so patterns align to the transport.

### 6.4 The Time knob, and Q7

| Time mode | The Time knob sends | Subdiv | `macro.time` |
|---|---|---|---|
| Free (default) | `MacroMove(macro.time)`, as today | the stored one; a Subdivision event can still change it | the knob |
| Subdiv | a Subdivision event (field 0) when its zone changes, with hysteresis at zone edges | the knob's | expression, MIDI CC, hosts |
| Tempo | Tempo events, exponential from 20 BPM (fully counter-clockwise) to 300 BPM: ns = round(3·10⁹ · 2^(−m·log₂15)) through DetMath's `Exp2D`, exported as `TempoNsFromKnob(m)` | forced to TAP | expression, MIDI CC, hosts |

**Q7 (answered here, provisionally, D5):** in Subdiv and Tempo modes the Time knob never reaches
`macro.time`; expression, MIDI and hosts still do, in every mode (compiler §3.1's lean).

**The Tempo knob (D15)** picks up like a macro after a load (compiler §3.5): after a tap, a recall
or a clock tempo it stays inert until it passes the committed tempo, so it never jumps the tempo.
Exponential, because equal turns then make equal tempo ratios, the "smooth acceleration and
deceleration" of the Microcosm's Tempo mode (§12). Under external clock its events are ignored
(§3.6); Big Time's bend-then-snap-back is the precedent for something better, left open (§11.8).

**The time-mode gesture (D18)** cycles Free → Subdiv → Tempo → Free with one LED state each; the
Microcosm's SELECT tap toggles its two, and Brainscape's Free mode, macro time in milliseconds,
has no Microcosm counterpart. The gesture sends a Subdivision event (field 1), so the time mode is
logged and replays.

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

## 7. Smoothing

### 7.1 The committed tempo: a deadband in integers

Under Internal, Pc = P at every change: taps, the knob and host tempo are explicit intents. Under
ClockFree and ClockRunning, Pc moves to P_fit only when the window holds N ≥ 24 ticks and either

1. |P_fit − Pc| > Pc >> 9 (0.195 %, about 3.3 σ of the computer-clock fit, never reached by a
   hardware clock), at once; or
2. |P_fit − Pc| > Pc >> 12 (0.024 %) on every tick for 192 consecutive ticks (8 beats).

Rule 2 makes Pc converge on a steady clock, so the post delay's repeats do not drift against the
grid by the fit's residual; acquisition and re-acquisition past 24 ticks commit at once (counted
in `commits`). Synced durations change only with Pc, so a jittery clock never moves the delay,
and TapGlide's `Retarget` ignores an unchanged integer target (`dsp/src/detail/PostChain.h:139-145`).

### 7.2 Glide

A changed Pc retargets the post delay's head, which glides as it does for `time_ms` today: two
cascaded 50 ms one-poles with the speed capped at 0.5 frames per frame, a tape-like bend
(`PostChain.h:113-166`, `PostChain.cpp:53-55`; profile §5.6). A Tempo-mode sweep, sending a Tempo
event per pot step, is thus a smooth accelerando with the repeats bending pitch, as the Microcosm
"smoothly matches" a tapped tempo and Meris's TAP GLIDE and Thermae's GLIDE do (§12). The pair is
critically damped with τ = 50 ms (2,400 frames), so a change of Δ frames moves the head at most
Δ/(e·τ) frames per frame, at t = τ: a tap from 120 to 121 BPM moves a 500 ms delay by 198 frames
and bends the repeats by up to 3 % (half a semitone) for about a tenth of a second, and a clock
commit at the deadband's 0.195 % (47 frames) by up to 0.7 % (12 cents) (*calculated*). Whether
that is the feel wanted is a listening question (§11.8). A silent stage jumps instead
(`PostChain.cpp:323-334`).

### 7.3 Crossfade for discrete jumps

A glide over an octave of delay swoops: a 2 s to 10 ms throw at feedback 0.9 bends the repeats
nearly three octaves over 4 s (profile §5.6, *measured*). So a **discrete** change of the synced
post-delay target crossfades instead: a change of row 63's code, of the effective Subdiv (a
Subdivision event, entering or leaving Tempo mode, a load), of the fold count, or sync switching
on or off.

- `RebuildPostParams` raises a jump serial in `PostParams` when the target changes for one of
  these reasons; a Pc change alone does not.
- `PostChain` (`dsp/src/PostChain.cpp`) keeps a **second head**: on a jump it copies the current
  `TapGlide` as the outgoing head, which keeps reading (and gliding toward its old target), primes
  the incoming head on the new target, and mixes them over `kXfadeFrames` = 1,024 frames (21.3 ms at
  48 kHz) with gains (1024 − n)/1024 and n/1024, exact multiples of 2⁻¹⁰ like FastCut's 2⁻⁷
  (compiler §7.3). The delay line's feedback write takes the mixed read.
- **One fade at a time:** a jump during a fade waits; when the fade ends, the latest pending
  target starts the next one. So a Subdiv knob turned through all six zones gives a chain of
  fades, never a dropped head or a step.
- A silent stage, which jumps today, jumps here too; a Pc change during a fade retargets the
  incoming head, which glides.

Cost: one more Catmull-Rom read per sample during a fade, about 60–80 cycles (*estimated* from the
post delay's 319 cycles per sample with one head, `rev7-silicon-record.md` §3.5), 3–4k cycles per
48-frame block, 0.6–0.8 % of the budget, for 21 ms per jump.

### 7.4 The artefact budget

| Artefact | Budget | Checked by |
|---|---|---|
| A hardware clock (σₓ ≤ 0.3 ms) at constant tempo | 0 commits after lock over 10 minutes | the golden `clock_midi_hw` counters (§8.3) |
| A computer clock (E-RM's distribution) at constant tempo | at most one commit per minute after lock, each ≤ 0.4 % | `clock_midi_computer` counters |
| CLOCK hit timing, internal or host tempo | exactly F(k): 0 frames from the ideal grid | §8.5's beat-lock metric |
| CLOCK hit timing after lock, hardware / computer clock | rms ≤ 0.3 ms, max ≤ 1 ms / rms ≤ 1 ms, max ≤ 3 ms against the generator's true ticks (*estimated*, set at step 0b) | §8.5 |
| Synced post-delay echoes | at exactly the synced frames | §8.5's impulse render |
| A discrete jump | no sample step above 4× the static render's largest (the audition's Clicks check) | S12 |
| A tempo change | the glide's own pitch, s² at read speed s (≤ 2.25×), as for `time_ms`; a deadband commit bends the repeats at most 0.7 % (§7.2) | profile §5.6's measurement; S12 by ear |

## 8. Determinism and testing

### 8.1 What is guaranteed

The parity contract (profile §2.1) extends unchanged: two conforming builds given the same package,
input, device settings (`global.tempo_recall` among them) and stamped stream, events 6–10
included, write identical bits. The stream is the source of truth. A pedal following a MIDI clock
and a plugin following a DAW produce different streams, which match only when one replays the
other's log, as companion §1 already says in excluding host tempo sync "unless turned into the
same event stream". Not guaranteed: live clock timing on either side, the host cases of §4.4, Spillover
loads (profile §2.4).

### 8.2 Unit tests (`dsp/tests/test_tempo.cpp`)

- **Integer helpers:** `MulDivRoundU64` and the signed form against a reference on edge operands
  (0, 1, 2⁶⁴ − 1, products straddling 2⁶⁴ and 2¹²⁸) and 10⁷ random ones; `FloorDiv`, `CeilDiv`,
  `FloorMod` on every sign combination.
- **Phasor:** span-split invariance of `Advance` over random splits of 10⁶ frames at random P;
  F(k) against the closed form at every beat of a **ten-hour** run at 140 BPM and 48 kHz (no drift,
  bit for bit); tempo changes keep the tick fraction; `lastFired` and catch-up on forward and
  backward moves (a reference that tests every frame rationally).
- **Tap:** the table of §3.2 case by case, at both thresholds' edges.
- **Follower:** seeded tick generators (§8.3) at 20, 120 and 300 BPM, hardware and computer
  jitter: P within the §3.3 accuracy after lock; a tempo step re-acquires within 6 outliers; a
  dropout of 200 ms infers its ticks; a 38 ms burst neither re-labels nor re-acquires.
- **Lazy loss:** the engine's results equal a reference that applies each loss at its deadline
  frame and splits there.
- **Transport:** Start and Continue at the next tick, Song Position while stopped and while
  running, Stop's continue position, host-style events ignored under clock.
- **Payloads:** every invalid form of §4.2 ignored and counted; an unknown type counted.
- **`MidiClockParser`:** real-time bytes inside a Note On and inside SysEx, F2 after a Program
  Change and after Channel Pressure (libDaisy's failure cases), running status kept across F8.
- **Durations and folds:** every code × every Subdiv at 20, 30, 120 and 300 BPM against a
  128-bit reference; the fold edges of §5.3.

### 8.3 The golden corpus

- **Script verbs** (`dsp/tests/golden/EventScript.h:58-115`): `Tap(frame)`, `Tempo(frame, ns)`,
  `Tick(frame)`, `Transport(frame, kind, position, atNextTick)`, `Subdivision(frame, field, code)`.
- **Generators,** integer-only and seeded through `TestSignal.h`'s hash: `ClockTicks(script,
  start, ns, count, model, seed)` with models **none**, **hardware** (uniform over a 48-frame
  block: the pedal's grid) and **computer** (a sum of four uniform draws scaled to σ = 163 frames,
  3.4 ms, plus every 500th tick held 1,843 frames, 38.4 ms, the ones after it bunching behind it,
  so frames never fall), with dropouts and tempo steps; and `TapSeries(start, intervals, spread)`.
- **Presets**, corpus version 13 with the tempo core and 14 with synced times, on a new 30 s
  vector `plucks_clock_30s`: `clock_internal` (stored tempo, Subdiv stepped through all six,
  intermittency, burst, jitter), `clock_tap` (tap chains: set, refine, pause, halve, bounce),
  `clock_midi_hw` (Start, Stop, Continue, Song Position, ticks while stopped), `clock_midi_computer`
  (bursts, a 200 ms dropout, a tempo step, a loss and a tap accepted after it), `clock_loads`
  (Spillover under Keep and Preset, an Exact load mid-clock), then `sync_post` (row 63 swept by a
  macro, Subdiv jumps crossfading, a fade chain), `sync_base` (`base_sync` with feedback), and
  `sync_fold` (a tempo sweep across the 2 s fold). Events both on and off the 48-frame grid.
- **Counters** (`dsp/tests/golden/Corpus.h:55-98`, 28 today): `Taps`, `TapsIgnored`,
  `ClockTicks`, `TickOutliers`, `Reacquires`, `DropoutTicks`, `ClockLosses`, `TransportEvents`,
  `SubdivEvents`, `ClockBirths`, `Commits`, `Crossfades`, `Folds`, `InvalidEvents`, from
  `Engine::TempoCounts()`; each preset's minimums prove its features ran.
- **Ablations,** each of which must change its presets' output and not before the feature acts:
  `clock` (the source removed), `tempoEvents` (events 6–10 dropped), `subdiv` (TAP throughout),
  `sync` (row 63 at 0, `base_sync` off), `crossfade` (jumps glide).
- **Perturbations,** unchanged, must reproduce every hash: block sizes, the pattern
  {48, 1, 127, 32}, random sizes 1–512, fresh engines and the hostile FP environment
  (`golden_main.cpp`, profile §6.4). **Split delivery** (`EventScript.h:140-145`) has no
  unstamped call for events 6–10 (§2.6): the harness splits at the event's frame and hands the
  event to the part that starts there as its only event, at offset 0, which still moves every
  block boundary. A restart mid-render (`Corpus.h:140-146`) begins with the tempo the script's
  `StateAt` records, re-sent as a Tempo event at the restart's frame 0, as a producer would.
- **Long render:** nightly on the host legs, a 10-minute render at 140 BPM with per-second hashes,
  whose clock-birth count and last birth frame must equal the closed form.

### 8.4 Plugin tests

The test playhead (`plugin/tests/plugin_tests.cpp:224-233`) gains a tempo and a ppq. With "Restart
on transport start" on, a bounce at 137.5 BPM starting at ppq 3.37 gives one output hash at host
blocks of 37, 64, 441, 512, 1,024 and 4,096 frames; a stop and restart at another ppq still
aligns its first hit to the anchor; the Standalone's MIDI path turns a byte stream with
interleaved real-time bytes into the events of §4.3 at their sample positions.

### 8.5 Audition

A new script **S12** next to S0–S11 (`tools/audition/README.md:78-86`): taps setting 120 then
90 BPM; a hardware-model MIDI clock at 128 BPM with Start at 2 s, the Subdiv stepped every 2 s
through all six positions, Stop at 14 s; a Tempo-mode sweep from 80 to 160 BPM over 8 s. Each
clock mode is rendered on its class input.

**Beat-lock metric:** the audition tool reads `TempoInfo::lastClockBirth` after every 48-frame
block (grid points are at least 200 frames apart, so none is missed) and reports each hit's
offset from the ideal grid, which it computes from the script's own ticks; the tempo it rendered
is known exactly, so an internal or host tempo must give 0 frames. **Echo check:** an impulse
render of each synced post-delay mode must put its echoes on the synced frames exactly. The
pre-screen gains a **Lock** row (§7.4's budgets) and runs its Clicks check over S12.

## 9. Firmware and hardware

### 9.1 MIDI in on the Rev7 breadboard

**Circuit,** Funbox v3.2's, "VERIFIED WORKING" on a Seed Rev7 (`docs/research/daisy-pedal-platforms.md:301-306`),
with the topology of Electrosmith's reference (`docs/research/pedal-control-surface-and-io-hardware.md:342-361`):
the MIDI current loop through 220 Ω into the LED of an H11L1M, a 1N4148 reversed across the LED;
the H11L1M's open-collector Schmitt output pulled up to 3.3 V through 270 Ω into the Seed's UART
receive pin; 100 nF from the H11L1M's supply pin (6) to ground.

**The RC concern.** The transcription puts the 100 nF on the receive line itself, behind the
270 Ω pull-up (`pedal-control-surface-and-io-hardware.md:351-352`). That is τ = 27 µs, and a rising
edge needs about 1.2 τ, 32 µs, to cross the input's high threshold (0.7·VDD): one MIDI bit at
31.25 kbaud (*calculated*), which would corrupt bytes. It is almost certainly the opto's supply
decoupling, misread onto the receive line; nothing larger than stray capacitance goes on the
receive net. Check it against the schematic image before buying parts.

**Pins.** libDaisy's MIDI UART is USART1, receiving on PB7 (LD `src/hid/midi.cpp:10-16`), the
Seed's D14 (LD `src/daisy_seed.h:224`); the live wiring uses only the audio pins
(`firmware/README.md:337-344`). Confirm D14's header pin on Electrosmith's pinout card. **Jack:**
TRS for the prototype, as Funbox; DIN against TRS type A or B is the schematic's choice (D10).

**The receive buffer.** libDaisy listens by DMA and calls back on line idle or a half or full
buffer of 256 bytes (LD `src/hid/midi.cpp:5-17`; code finding C2). On a quiet line a byte is
reported one idle frame after it ends, a constant offset; in a continuous stream (a SysEx dump,
dense notes) bytes wait for the half buffer, up to 41 ms. The firmware passes a 16-byte buffer, so
a byte waits at most 8 bytes, 2.6 ms (*calculated*).

### 9.2 The tap footswitch

A momentary SPST switch to ground on a free pin with its internal pull-up (research
`pedal-control-surface-and-io-hardware.md:160-161`), on its own EXTI line rather than through the
CD4021 shift register the research uses for footswitches (`:153-159`), which is polled. D7 (PG10,
EXTI line 10) is proposed; any free pin with an EXTI line will do, and libDaisy has no EXTI
wrapper (LD `src/per/`), so the firmware uses the HAL. The handler stamps the first falling edge
and ignores edges for 30 ms (debounce); the switch is the Microcosm's left footswitch, tap.

### 9.3 Interrupts and the control loop

- **Priorities stay as they are (D20).** USART1 and every DMA stream run at priority 0, the audio
  DMA's (LD `src/per/uart.cpp:897`, `src/sys/dma.c:17-34`), so neither pre-empts the other, and a
  byte arriving during the audio callback is stamped after it: at most one block late, which the
  fit absorbs. The firmware moved USB below audio, not above (`firmware/platform/UsbSerial.cpp:70-74`);
  raising the UART above audio would only matter for sub-block stamps (§4.5). EXTI for the tap
  switch goes below audio too, at USB's 2.
- **The control loop** (`firmware/live/main.cpp:615-620`) drains the UART and tap rings each
  iteration, runs the translator (§4.3), pushes with §4.5's rule, and honours "receive MIDI clock"
  (default on) by not feeding the translator when it is off.
- **After an Exact load** while the engine's `Tempo().source` was a clock (read in the audio
  callback into a variable), it pushes Tempo(ns) at frame 0 of the new timeline (§2.5).
- **Counters** on the console: UART errors and restarts (libDaisy restarts reception after an
  overrun, losing bytes; LD `src/hid/midi.h:194-205`), late pushes, ring overflows.

### 9.4 Console commands

Beside `trigger` (`firmware/live/main.cpp:575-579`), with the same latency rule: `tap`;
`tempo <bpm>` (a Tempo event); `subdiv <1/4|1/2|tap|2x|4x|8x>`; `timemode <free|subdiv|tempo>`;
`clock <bpm> <count>` (an internal tick generator for hardware-in-the-loop tests, ticks stamped as
the UART's would be); `transport <start|stop|continue|spp N>`; `tempostat` (`Tempo()` and
`TempoCounts()` as JSON); and for step 0b, `midilog on|off`, printing each received byte and tap
with its block index and cycle count.

### 9.5 USB-MIDI, MIDI out

USB-MIDI clock arrives on cable 0 (companion §7.1) once the firmware moves to TinyUSB; it feeds the
same translator. Full-speed USB quantises to 1 ms frames, the pedal's block anyway. MIDI out and
thru, and sending clock, are not in the tempo core (D10).

### 9.6 Placement and CPU

**ITCM.** The live image has about 3.6 KiB of ITCM left at revision 7, and the next wave that
grows the engine moves `Validate` (8.4 KiB) out first (`firmware/README.md:476-491`); the CPU plan
adds 7–12 KB and asks for 8 KB spare (code finding C12). So the tempo core adds next to no ITCM:
`Tempo.cpp.obj`, `MidiClock.cpp.obj` and the host-conversion object join `_bs_not_itcm_members`
(`firmware/CMakeLists.txt:198`), checked by `NOT_ITCM` (`:251-258`); headers keep their bodies
out-of-line, because inline code compiles into its includer, which is in ITCM. ITCM grows only by
the clock offsets in `GranularCore::Process`, the new cases in `ApplyEvent` and the span call in
`RenderFrames` (calls into QSPI code), and the crossfade in `PostChain`'s per-sample loop: about
1.0–1.6 KiB (*estimated*). Control-rate code running from QSPI costs 1.05–1.21× warm and
1.26–1.70× cold (`rev7-silicon-record.md` §3.4), which is negligible at a tick's rate.

**CPU.** The budget is not met today: the nominal row peaks at 99.1 %, `dense_1ms` at 118.6 %, the
pessimistic rows at 135.5–168.5 % (`rev7-silicon-record.md` §3.3). The tempo core adds:

| Cost | Size | Where it lands |
|---|---|---|
| A CLOCK hit | one birth, at most 6,203 cycles at 48 voices (`rev7-silicon-record.md` §3.5) | at most one per 200 frames; the densest grid (8x at 300 BPM) is 40 hits per second, 0.05 % on average (*calculated*) |
| A hit with a 16-grain burst at spacing 0 | 16 births on consecutive frames, ≤ 99k cycles, 20.7 % of a block when all 16 fall in one (*calculated*) | exactly a footswitch trigger's cost today; the worst blocks are the burst's, not the clock's |
| A ClockTick | the fit, about 1,000–2,000 cycles (*estimated*) | ≤ 120 per second, 0.05 % on average; at most two per block |
| A crossfade | 3–4k cycles per block for 21 ms (§7.3) | per discrete jump |

Block-grid stamps split no pedal block (§4.5); a sub-block stamp would cost about 5–6k cycles at
64 voices (CPU proposal). **The cost governor** (CPU proposal, sound revision 8): CLOCK hits are
trigger-class, deferred when needed (at most 8 pending per source) and placed in its source order
as manual/MIDI, onset, **clock**, burst, scheduler (D13); a deferred hit takes the draws of the
frame it is born at, as the governor rules, and the grid never moves (§6.3). ClockTick and Tap
events are exempt from the firmware's coalescing and counted in its per-block event limit; the
shared cost model and the audition's Load row count CLOCK's hit rate and bursts.

## 10. The plugin

### 10.1 Following the host

- **Tempo source**, a wrapper setting saved in the session (`BSWS`): **Host** (default, D9) or
  Internal. Under Host, when the host reports a tempo, the wrapper emits §4.4's events at the Host
  rank (`plugin/src/EventQueue.h:18`) and drops taps and Tempo-knob events before stamping them;
  without a reported tempo (the Standalone, some hosts) it behaves as Internal.
- **"Restart on transport start" stays off by default** (`plugin/src/StateCodec.h:18`,
  `PluginProcessor.h:297`): delay trails across stops are the reason it is off (companion §4.9),
  and the transport anchor aligns the grid to the host's beats either way. The status survey's
  "keep it on" assumed it was on (code finding C1). Bounces are reproducible only with it on (§4.4); the
  labels already say so (profile §2.4).
- **After a restart** (`LoadAfterRestart`, `PluginProcessor.cpp:249-258`) the spare holds only the
  preset's tempo, so the wrapper re-emits Tempo and Transport at frame 0 of the new timeline.
- **The engine runs at the host's rate** (`PluginProcessor.cpp:191-194`): P is exact at that rate
  (§2.1); parity holds at 48 kHz only.

### 10.2 MIDI clock in the Standalone

JUCE's Standalone has a playhead without tempo or playing state and stamps device MIDI by arrival,
scaled into the next block (JUCE `juce_audio_utils/players/juce_AudioProcessorPlayer.cpp:243`,
`:391-393`; `juce_audio_devices/midi_io/juce_MidiMessageCollector.cpp:94-139`), so a tick's frame
is off by up to a block, σₓ ≈ 3.1 ms at 512 frames, which the 96-tick fit absorbs to 0.055 %
(*calculated*, §12). The MIDI filters that keep only note-ons (`PluginProcessor.cpp:857-867`,
`:973-982`) pass F8, FA, FB, FC and F2 to the translator at their sample positions, at the MIDI
rank. A "Receive MIDI clock" setting (default on) gates it. A later refinement maps the device's
timestamps to frames with a DLL (§12).

### 10.3 Tap button and BPM display

The pedal view gains a **TAP** button (a Tap event at the next chunk's first frame, UI rank, greyed
under host sync) and a **BPM display** from `Engine::Tempo()`, copied to an atomic after each
block: the committed tempo to 0.1 BPM, the source (HOST, MIDI, INT), a lock dot and a beat LED
from the position. Typing a tempo sends a Tempo event. Today the editor has only TRIGGER and
FREEZE (`plugin/src/PluginEditor.cpp:88-95`).

### 10.4 Parameters and session

| ID | Name | Kind | Range, default | Host |
|---|---|---|---|---|
| 63 | `post.delay.sync` | Leaf (synced-times revision) | 0–16, 0; display §5.4 | registered as a leaf; 17 steps |
| 83 | `perf.subdiv` | Performance | knob position 0–5 (1/4 … 8x), 2 (TAP) | automatable; the wrapper sends Subdivision with §5.1's code |
| 84 | `perf.time_mode` | Performance | 0–2 (Free, Subdiv, Tempo), 0 | automatable; Subdivision field 1 |
| 85 | `global.tempo_recall` | Global | 0–1 (Keep, Preset), 0 | a device setting, not registered |

Rows are appended after 82, as host indices require (`PluginProcessor.cpp:113-121`; compiler
§4.5). Row 85 has no domain: the engine reads it at load step 4. The wrapper's `WrapperEvent::Type`
(`plugin/src/EventQueue.h:16`) gains Tap, Tempo, ClockTick, Transport and Subdivision; the
session saves the Tempo source and "Receive MIDI clock" as settings.

## 11. Schema, revisions, plan and decisions

### 11.1 Schema and compiler

- **`scheduler.subdiv` is withdrawn** (D14; compiler principle 5 lets a reserved field change in
  the pull request that builds it): an E2 unknown key. The `Clock` feature is the `clock` source
  only (`Mode.cpp:100`, `:102` removed).
- **`performance.tempo_source` is withdrawn** (D3): the source is a device setting, and a byte in
  STAT, which `sound_hash` covers (compiler §6.3), would give presets that sound the same different
  hashes (code finding C8).
- **`performance.subdiv`** reads §5.1's labels in code order (`Schema.cpp:82`, `kSubdivNames`,
  becomes `tap, 1/4, 1/2, 2x, 4x, 8x`), default `tap`.
- **Divisions:** `Division()` reads and writes §5.2's names (`Schema.cpp:660-675`, `:1904-1907`);
  row 63 stays numeric (§5.4).
- **Support** (`Schema.cpp:205-233`, `:1752-1758`): the single `kNeedPerformance` bit
  (`Schema.cpp:44-45`), which unlocks all five performance fields with CLOCK, splits into
  `kNeedPerformanceTempo` (time mode, subdiv, tempo; the tempo-core build) and
  `kNeedPerformanceReverse` (global reverse; later), so `reverse: true` stays E6 (code finding
  C7). `kModeFeatureClock` is supported from the tempo core, `kModeFeatureTempoSync` and
  row 63 from synced times.
- **Lint:** L12 (§6.2), L13 (§6.5); L2 evaluates a synced base delay at its shortest duration over
  the tempo range and the Subdiv the mode can reach; L5 already counts `clock` as free-running.
- **Tests:** `kFullStructure` (`compiler/tests/test_compile.cpp:110-134`) moves `"subdiv"` to
  `performance` and drops `tempo_source`; E6 cases (`:275-316`) follow the support table.

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
| Tempo core: events 6–10, `TempoCore`, performance state applied, CLOCK hits, Subdiv re-coded, SCHD and STAT bytes reserved | N | yes: new inputs and a new source | unchanged (every byte involved is 0) |
| Synced times: row 63 a `Leaf`, `base_sync`, §5.2's table, folds, the crossfade | N + 1 | yes | every package's `sound_hash` changes, since the compiler writes the new leaf at its default; the package-change label, as revisions 4, 6 and 7 needed (compiler §7.6) |
| `MidiClockParser`, host-conversion functions | none | producer code in `dsp/`: the "sound-neutral" label | — |
| Firmware, plugin, audition, compiler-only changes | none | outside the trigger paths | — |

N is the next revision when the tempo core lands: 8 if it precedes the CPU proposal's governor,
which that proposal numbers 8, else 9. Each revision re-mints `golden.json`, and every earlier
preset must reproduce its hashes and counters, as at every wave-1 revision.

### 11.4 Lanes, order and estimates

| Lane | Work | Files | Starts | Days (*estimated*) |
|---|---|---|---|---|
| **T0** design | this document and its amendments | `docs/` | done | — |
| **T0b** breadboard | MIDI in and the tap switch on the Rev7; firmware that logs raw bytes and taps with block and cycle stamps (`midilog`); measure tick jitter from a hardware master and a DAW over DIN, tap variability, UART errors and late pushes | `firmware/`, parts about $2 | now: no sound change | 2–4 |
| **T1** tempo core | §2, §3, §4.1–§4.3, §6.3, §6.4's engine side; integer helpers; rows 83–85; §11.1–§11.2; its unit tests, verbs, generators, presets, counters and ablations | `dsp/`, `compiler/`, `dsp/tests/`, `firmware/CMakeLists.txt` | after the first set's knob ratings; `Validate` out of ITCM first | 8–12 |
| **T2** synced times | §5.2–§5.4, §6.1, §6.2, §7.3; L12, L13; their tests and presets | `dsp/`, `compiler/` | after T1 | 4–7 |
| **T3** plugin | §4.4, §10 | `plugin/`, `dsp/` (host conversion) | after T1 | 3–5 |
| **T4** pedal firmware | §4.5, §9.2–§9.4 | `firmware/` | after T0b and T1 | 2–4 |
| **T5** audition | S12, the beat-lock metric, the Lock row; re-rate the echoic modes' Time | `tools/audition/` | after T2 | 2–4 plus listening |
| **T6** bench | DWT at revision N + 1 on the Rev7, with the CPU fix | `firmware/` | after T2, with the governor | 1–2 |

**Order:** T0b now; the first set's knob ratings (`docs/STATUS.md:57`); T1, then T2, each one
revision, with T3 and T4 alongside; T5; T6; then the rest of W2 (steps, mark walk, global reverse,
velocity) and step 4's rhythmic second set (compiler §11.4). **Total** about 22–38 engineer-days
beyond this design (*estimated*); the survey's 25–45 included the design and golden coverage as
separate lines, folded here into T1 and T2.

### 11.5 Owner decisions

Each answer is provisional (2026-10-08), adopted so the design is complete, and reversible before
the first public release. D1–D11 are the status survey's eleven, with its recommendations
amended where the evidence of §12 required (D2, D4, D7, D8, D9, D11); D12–D20 are new.

| # | Decision | Provisional answer | § |
|---|---|---|---|
| D1 | Order | Keep W2 after the first set's knob ratings; start this design (done) and the breadboard (T0b) now; split W2 so the tempo core lands before steps, mark walk, global reverse and velocity | 11.4 |
| D2 | Sources and priority | The Microcosm's: Start and Continue switch to external clock at the next tick and set the position; Stop reverts to internal keeping tempo and phase; tap and the Tempo knob are ignored under clock. Added: clock without transport is followed for tempo and tick phase after 24 ticks unless a Stop was seen; a second without a tick reverts to internal; host tempo is the wrapper's, which drops taps while following it | 3 |
| D3 | Tempo source per preset or global | Global: "receive MIDI clock" (pedal) and the Tempo source (plugin) are device settings; STAT's `tempo_source` byte is reserved | 2.4, 3.6 |
| D4 | What a load does to tempo | Exact plays the stored tempo, time mode and subdivision (the pedal re-asserts a running clock tempo after it); Spillover applies time mode and subdivision and keeps the running tempo unless `tempo_recall` is Preset and no clock is followed; no load moves the phase | 2.5 |
| D5 | What syncs; Q7 | The post delay (row 63), `base_sync` and CLOCK hits. Q7: in Subdiv and Tempo modes the Time knob never reaches `macro.time`, which expression, MIDI and hosts still do | 6 |
| D6 | Q11, tempo-exact grain feedback | Rhythmic repeats on the post delay; `base_sync` places the first tap exactly; grain feedback keeps its 10.67 ms pass (lint L12) | 6.2 |
| D7 | Divisions | Subdiv is the Microcosm's six rate multipliers (resolved from its manual), code 0 = TAP the default; sixteen note values by duration; folding by octaves | 5 |
| D8 | Smoothing | A 96-tick least-squares fit (not a one-beat average), a two-level deadband, taps averaged over up to four intervals, the post delay's glide for tempo changes, a 21 ms crossfade for discrete jumps | 3.3, 7 |
| D9 | Host tempo | Follow the host by default; ns resolution, 1 µs hysteresis, a tick-aligned start anchor; "Restart on transport start" stays off by default (it is off, not on), bounces reproducible only with it on | 4.4, 10.1 |
| D10 | MIDI hardware for v1 | TRS MIDI in on the Rev7 with the H11L1M; DIN or TRS type A/B at schematic time; the out/thru footprint reserved; no clock out in the tempo core | 9 |
| D11 | Song position | A position in 24-ppqn ticks in Transport's value (Song Position × 6), no separate event | 4.1 |
| D12 | Subdiv's scope | It scales every tempo-derived duration of the mode, the CLOCK grid and the synced fields | 5.1 |
| D13 | CLOCK allocation | Trigger class: oldest-steal at `voice_count`, never capped by `overlap`; the governor's order manual/MIDI, onset, clock, burst, scheduler | 6.3, 9.6 |
| D14 | Two stored subdivisions | One: `performance.subdiv`; `scheduler.subdiv` withdrawn | 2.4 |
| D15 | The Tempo knob | 20–300 BPM exponential, with pickup; inert under external clock | 6.4 |
| D16 | Tap feel | Two taps set the tempo; a pause re-arms; more than 40 % off restarts the chain; the mean of up to four intervals; no skip detection | 3.2 |
| D17 | Tempo recall default | Keep (the Microcosm's global tempo); Preset as the device setting's other value | 2.5 |
| D18 | The time-mode gesture | Cycles Free → Subdiv → Tempo, one LED state each | 6.4 |
| D19 | Expression and MIDI reach | Expression and macros reach row 63 and `macro.time`, not tempo or Subdiv; the pedal's MIDI map adopts the Microcosm's CC#5, CC#10 and CC#93 | 6.5 |
| D20 | Pedal event timing | Block-grid stamps with a fixed two-block latency for every producer; interrupt priorities unchanged; sub-block stamps only if T0b shows the need | 4.5, 9.3 |

### 11.6 Design decisions taken

| Decision | Choice | § |
|---|---|---|
| 1. Tempo representation | Q32.32 frames per quarter; an exact integer phasor; durations by 128-bit multiply-divide | 2.1–2.3 |
| 2. Event units | ns per quarter in Tempo; ticks in Transport's position; µs stays in presets | 4.1 |
| 3. Where estimation runs | In `dsp/`, on applied frames; producers only stamp, filter and log | 1.4 |
| 4. Timeouts | Lazily at the next event, valid because a loss changes nothing audible at its deadline | 3.5 |
| 5. Clock following | A windowed least-squares fit over up to 96 ticks, outliers excluded, re-acquired after 6 of one sign | 3.3 |
| 6. Lost ticks | One label per tick, except dropouts of at least four ticks and 100 ms; inference with confirmation is a later refinement if T0b measures losses | 3.3 |
| 7. Unstamped API | None for events 6–10; `ProcessContext`'s tempo fields deleted | 2.6 |
| 8. The MIDI translator | Shared in `dsp/`, ahead of libDaisy's parser | 4.3 |
| 9. Births on the grid | `FireTrigger` with ordinal 3; the grid independent of admission | 6.3 |
| 10. Discrete jumps | A second post-delay head, one 1,024-frame fade at a time | 7.3 |
| 11. Placement | `TempoCore` in the Warm arena and out of ITCM | 2.6, 9.6 |

### 11.7 Risks

1. **CPU.** CLOCK modes with bursts make the worst blocks a footswitch burst's, already over
   budget. Mitigation: the cost governor (§9.6); compiler lints on hit rate × burst.
2. **ITCM.** The crossfade must run per sample in ITCM. Mitigation: `Validate` out first; the
   tempo core's control-rate code out of ITCM; the `NOT_ITCM` check.
3. **Computer clocks.** Real DAW jitter may exceed the E-RM case. Mitigation: T0b's recordings
   replayed in the corpus; the thresholds of §3.3 and §7.1 are constants to tune before the first
   public revision.
4. **Feel.** Glides on tempo changes bend pitch; a 21 ms crossfade may sound too short or too long;
   the Tempo knob's curve may suit poorly. Mitigation: S12 and the owner's listening, each a
   constant.
5. **The follower's complexity.** Mitigation: every rule is a table in §3, tested against a
   frame-by-frame rational reference (§8.2).
6. **Host quirks.** Mitigation: the documented limits of §4.4 and the labels.

### 11.8 Open questions, settled at T0b or by listening

1. Tick jitter on the Rev7 from a hardware master and a DAW over DIN, and later USB: the outlier
   and dropout thresholds, N, and the deadband's constants.
2. Human tap variability on the Rev7's switch: the 40 % and 1.75× thresholds.
3. UART errors and late pushes under load: lost-tick inference (§11.6 item 6), sub-block stamps
   and the UART's priority (D20).
4. The crossfade's length and the glide on tempo commits, by ear.
5. The Time knob under external clock: inert (D15) or Big Time's bend-and-snap-back.
6. Bar alignment beyond 4/4: a host's time signature and bar position; MIDI has neither.
7. Reporting libDaisy's parser defect upstream (§4.3).

### 11.9 Amendments made with this design

Dated 2026-10-08, in each document's style: **grain-engine.md** §4 (CLOCK's allocation and
`overlap`, D13), §6 (Subdiv as rate multipliers, one stored subdivision), §9 (the API listing's
`SetTempo`, `Tap`, `SetSubdiv` and `SetExternalClock` replaced by events 6–10, compiler record §3
item 14); **companion-app.md** §4.10 (the pedal's latency rule and logs) and §6.2 (the tempo source
is a device setting; STAT's byte reserved); **mode-compiler.md** §1.2 (the pass is this document),
§2.3 and §2.6 (`scheduler.subdiv` and `tempo_source` withdrawn, the re-coded default), §7.4 (the
payloads), §12.3 (Q2 recorded as provisionally answered, Q7 and Q11 answered here);
**determinism-profile.md** §5.11 (tempo events and the pedal's stamps); **docs/README.md** and
**docs/STATUS.md** (this document). The implementing pull requests amend the code comments that
cite the old meanings (`Mode.h:86-97`, `PresetState.h:42-56`, `Engine.h:206-210`).

## 12. Evidence

**The Microcosm** (manual v1.13, https://cdn.shopify.com/s/files/1/0920/2928/8752/files/MC_manual_WEB.pdf;
printed and PDF page numbers agree). *Documented:* tap "smoothly matches the tempo to the timing of
quarter note taps" (p. 6); tap works in Tempo mode (p. 5) and not under external clock (p. 21);
Subdiv "determines the musical subdivision of the effect" and Tempo mode forces quarters (p. 5);
CC#5 Subdiv 0–5 = 1/4, 1/2, TAP, 2x, 4x, 8x, CC#10 Time, CC#93 tap (p. 20; midi.guide); the
looper's "'TAP' interval (1X)" and "1/4 to 4x" (p. 15); Start switches to external clock and Stop
reverts (p. 21); global configuration is not saved in presets (p. 18). *Not documented anywhere
searched:* tap averaging and timeout, Continue, Song Position, clock loss, the Time knob under
clock. *Field reports* (Elektronauts thread 121669): posts #734 and #743, a stop and start realigns
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

**The code** at `343f33c`, as cited throughout. The status survey and the code investigation
behind this design are not committed; the findings it relies on, several of which correct the
survey, are:

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
  −0.35 or +4.95 KiB.
