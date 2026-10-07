# Mode compiler and presets

> Design for merged step 3 (`docs/STATUS.md`, "Next steps" item 3; companion §8.1, phase C)
> and the plan for step 4. It covers the preset document and its mode schema, the macro
> layer, the final parameter identities, the compiled mode and the `.bsp` package, the
> engine changes that play them, the desktop compiler `bspc`, app integration, tests, and
> the first factory modes. It extends [grain-engine.md](grain-engine.md) ("engine §N"),
> [companion-app.md](companion-app.md) ("companion §N") and
> [determinism-profile.md](determinism-profile.md) ("profile §N"); §12.5 lists the passages
> it makes stale. The evidence (three investigations and three reviews, with compiled
> probes), the disagreements it resolves and its provenance are in
> [reviews/mode-compiler-record.md](reviews/mode-compiler-record.md) ("record §N").
> Numbers are *measured*, *calculated* or *estimated*. Code is cited as `path:line` at
> `main` `42773a2`. Status: **draft v2**, revised after review (record §6); lanes 0, B, A, G
> and C (sound revision 2) are built (docs/STATUS.md), and notes marked "as built" record what an
> implementing lane decided where this document left a choice.

---

## 1. Scope

### 1.1 What step 3 delivers

Today a preset is 28 plain values (`dsp/include/brainscape/Params.h:63-92`), and
`PresetState` holds nothing else (`dsp/include/brainscape/PresetState.h:24-30`). Step 3
makes a preset carry a mode. It delivers:

1. **Schema 1 of the preset document** (§2): engine §4–§6's whole vocabulary, each field
   playable now or in a named wave; a field this engine cannot play is an error naming it.
2. **The macro layer** (§3): six per-mode macros and two auxiliary ones, one exact
   evaluator, macro and expression moves as frame-stamped events, and the device settings
   around them.
3. **Final parameter identities** (§4), their per-kind rules, migration and the release gate.
4. **The compiled preset and the `.bsp` package** (§5, §6), with exact canonical JSON.
5. **Engine runtime changes** (§7): a fix for a measured routing bug, the wet gain stage, and
   wave 1 of the unbuilt engine features.
6. **The compiler and `bspc`** (§8), **app integration** (§9), **tests and CI** (§10).

Step 4 (§11) starts when macros and packages exist; static listening can start now.

### 1.2 What it defers: the waves

Every sound-changing pull request bumps the internal sound revision; §7.6 gives the order.

| Wave | Contents | Lands |
|---|---|---|
| **3a** pipeline | ID table, schema 1, compiler, package, decoder and validator, macros and expression, `LoadPreset` with MODE and CTRL, wet-only trim, effect volume and Filter wet kill, the routing fix; the Mix law if approved (Q13) | step 3 |
| **W1** | trigger sources (the free-running scheduler can be off), burst, intermittency; pitch sets with transpose; micro-loop `repeat` and `decay_ms`; per-layer `voice_count` | step 3 |
| **W2** | CLOCK, with tap, subdivision, MIDI clock and host tempo and transport as events; stored performance state; step tables; mark index and walk; tempo-synced times; global reverse; trigger velocity | during step 4 |
| **W3** | modulators, routes, links; glide; per-voice SVF and bit-crush with tier-aware allocation; a second layer with `layer_mix`; `dry_duck`; reverb modes; `POS_PIN` re-arm; `POS_GRID`; scale quantization | before step 6, each when a factory mode needs it |
| deferred | runtime post-chain reordering, sidechain input, the looper, footswitch and MIDI assignments in CTRL, feedback-tamer leaves | after v1 or in their own documents |

STATUS.md's step 3 lists only the pipeline; companion §8.1 adds the features the factory
modes need, "above all" CLOCK. W1 precedes CLOCK here: its features are small, complete the
7 of the first 14 factory modes that 3a alone does not (§11.1), and fix two measured
defects, mark-positioned modes holding their note forever and onset-only modes being
impossible (record §2.2). CLOCK, the largest feature, gets its own design pass. The owner
may reorder (Q2).

### 1.3 Terms

| Term | Meaning |
|---|---|
| Preset document | The JSON file a person writes and keeps in git: one per preset, mode included (§2). |
| Leaf | A plain value with a permanent ID, stored in STAT; one of six row kinds (§4.1). |
| Structure | Compiled, non-automatable mode data (trigger sources, position source, pitch sets, macro definitions), stored in MODE. |
| Mode | A preset's structure. A *factory mode* is a factory preset. |
| Element | A part of the structure that can be absent: the second layer, a modifier op. |
| STAT, MODE, CTRL, META, JSON | The package's sections (§6.2): leaves and performance state; structure; macro positions and expression assignments; names; the source document. |
| `ModeBlob`, chunk | The decoded MODE section (§5.1); a tagged record inside MODE (§6.2). |
| Device setting | A pedal or app setting outside presets (§3.8), recorded in render recipes and PARITY requests. |
| Trails, FastCut | On a Spillover load, the old mode's grains finish, or fade out within 128 frames (§7.3). |
| rN | Sound revision N (`kSoundRevision`, profile §5.12). |
| Contract #N | Engine §10's tests: #1 block-split identity, #2 unity-rate null, #3 level, #4 feedback bound, #6 preset round trip. |
| The guard | The FP-environment guard (`dsp/src/detail/FpEnvGuard.h`, profile §3) around every floating-point `dsp/` entry point. |
| Tiers, Warm arena | Engine §7's memory tiers: Hot (DTCM), Warm (AXI), Bulk (SDRAM). |
| PARITY | The app asking the pedal to render a recipe and return hashes (companion §7.4). |
| En, Ln, Qn | Validation errors, lint findings (§2.7); open questions (§12.3). |

### 1.4 Principles

1. **One preset, one document, mode embedded.** No document references another, so
   compiling is a pure function of one file and every package is self-contained.
2. **Leaves carry values, structure carries shape, leaves are authoritative.** Macro
   positions are pickup references, never re-applied at load (profile §5.10), and a leaf a
   macro targets equals that macro's value at its stored position (§3.5).
3. **The compiler does no floating-point arithmetic.** It parses, canonicalizes and checks
   with integer operations on bit patterns; the engine derives ratios, frame counts and
   curves inside its guard. Package bytes cannot then depend on the host (record §2.5).
   Lint and `derive` (§8.2) call guarded `dsp/` functions and are not part of `Compile`.
4. **The schema names the whole vocabulary; the build decides what compiles.**
5. **Everything is internal until step 6.** IDs, names, tapers, schema 1 and the formats may
   change in place until the first public sound revision freezes them (companion §5.7). A
   reserved field may change shape in the pull request that implements it.

## 2. The preset document (schema 1)

### 2.1 Example

Engram (§11.1), shortened: the canonical form also writes every remaining leaf, `in_range`,
`curve`, `meta`, the trigger sensitivity and the `filter` macro.

```json
{
  "schema_version": 1,
  "id": "factory.engram",
  "name": "Engram",
  "family": "echoic",
  "sound_rev": 2,
  "sound_hash": "…written by bspc stamp…",
  "global": { "mix": 0.35 },
  "scheduler": { "sources": ["periodic", "footswitch", "midi_note"],
                 "overlap": 0, "jitter": 0, "intermittency": 0,
                 "burst": { "count": 1, "spacing_ms": 0 } },
  "layers": [ {
    "voice_count": 64,
    "position": { "source": "live", "base_ms": 1, "spray_ms": 0, "repeat": 1 },
    "size_ms": 100, "decay_ms": 0,
    "window": { "sustain": 1, "skew": 0.5, "smoothness": 0 },
    "pitch": { "set": [ { "st": 0, "weight": 1 } ], "select": "cycle",
               "transpose_st": 0, "spread_cents": 0, "reverse_prob": 0 },
    "pan_spread": 0 } ],
  "feedback": { "amount": 0 },
  "post": { "mod": { "rate_hz": 0.6, "depth": 0.05 },
            "delay": { "time_ms": 405, "fb": 0.45, "mix": 1 },
            "reverb": { "time": 0.4, "mix": 0.12 } },
  "wet_trim_db": 0,
  "macros": [
    { "id": "activity", "display_name": "Smear", "targets": [
        { "param": "scheduler.overlap", "range": [0, 0.45], "curve": 2 },
        { "param": "layer0.position.spray_ms", "range": [0, 40], "curve": 2 },
        { "param": "scheduler.jitter", "range": [0, 0.5] } ] },
    { "id": "repeats", "targets": [ { "param": "post.delay.fb", "range": [0, 0.9] } ] },
    { "id": "shape", "display_name": "Contour", "targets": [
        { "param": "layer0.window.sustain", "range": [1, 0.25] },
        { "param": "layer0.window.smoothness", "range": [0, 1] } ] },
    { "id": "time", "targets": [
        { "param": "post.delay.time_ms", "range": [40, 1500], "curve": 2 } ] },
    { "id": "space", "targets": [ { "param": "post.reverb.mix", "range": [0, 0.5] } ] } ],
  "controls": { "macro_positions": { "activity": 0, "repeats": 0.5, "shape": 0,
                                     "time": 0.5, "space": 0.24, "filter": 1 } }
}
```

Every targeted leaf equals its macro at the stored position, bit for bit: Time 0.5 gives
405 ms, Repeats 0.5 gives 0.45 and Space 0.24 gives 0.12 (*measured* with DetMath, record
§2.6); Activity and Shape at 0 and Filter at 1 give their range ends. The repeats run on the
post delay, whose taps are exact (§11.1).

### 2.2 Rules for every field

- **Leaf names are JSON paths**: leaf `layerN.a.b` lives at `layers[N].a.b`, any other at its
  dotted path, which is how pedal-side edits are patched in by name (companion §6.9).
- **Units are plain**; times are milliseconds or, from W2, divisions (`"1/4"`, `"1/8d"`),
  never sample counts.
- **Leaf values** are read as correctly rounded binary32 (§6.4) and canonicalized (−0 and
  subnormals become +0, the latter with finding L1). **Out of range is an error, not a
  clamp**, because clamping would silently change what was written. Integer-valued leaves
  accept fractions, which macros produce; §3.7 says how the engine reads them.
- **Structure values** are typed: enumerations as strings; counts, indices and weights as
  JSON integers (a fraction or exponent is an error); booleans.
- **Defaults and unknown keys.** A missing key takes its schema default, frozen per key
  when the key is introduced. An unknown key is an error, since a typo must not be ignored,
  and so is a `schema_version` newer than the compiler's.
- **Absent elements.** Leaves of an element that does not exist (layer 1 in a one-layer
  mode, `svf` leaves without an SVF) are not written, compile to their defaults, and must
  hold them in a package (§5.3).
- **Strings.** `id`: `[a-z0-9._-]`, 1–48 bytes, stable once released; `name`: 1–32 bytes
  of UTF-8; `display_name`: 1–16 bytes.
- **Derived fields.** `sound_rev` and `sound_hash` are written by `bspc stamp` and ignored
  as input (§8.2). As built: `fmt` keeps them as written; `compile` stamps the JSON section
  and `stamp` the document with this build's revision and the computed hash. **Editor-only
  data** lives under `editor` (schema 1 defines `editor.ratio_gen`, engine §5's pitch-set
  generator, and `editor.detached`, the targeted leaves not derived from positions, §3.5):
  validated, kept, never compiled. As built: `editor.ratio_gen`'s keys stay open until the
  editor defines them, but it is in canonical form like the rest: its numbers are read as
  binary32 (overflow is E4) and written as canonical text, its object members sorted by key.

### 2.3 Top level, scheduler, output and post

"Wave" says when the engine can play a field: "now" means r1 has it. Numbers in
parentheses are leaf IDs (§4.2).

| Key | Kind | Range | Default | Wave |
|---|---|---|---|---|
| `schema_version`, `id`, `name`, `meta` | — | §2.2 | — | 3a |
| `family` | enum | `recall`, `reverie`, `misfire`, `echoic`, `none` | `none` | 3a |
| `global.mix` (2) | leaf | 0–1 | 0.5 | now |
| `scheduler.sources` | set | `periodic`, `clock`, `onset`, `footswitch`, `midi_note` | all but `clock`, `onset` | `onset` 3a (r2); others off W1; `clock` W2 |
| `scheduler.overlap` (6), `.jitter` (11) | leaves | 0–1 | 0.55; 0.2 | now |
| `scheduler.intermittency` (57); `.burst.count` (58), `.burst.spacing_ms` (59) | leaves | 0–1; 1–16; 0–500 ms | 0; 1; 0 | W1 |
| `scheduler.subdiv`; `scheduler.steps` | enum; §2.5 | `1/4`, `1/2`, `tap`, `2x`, `4x`, `8x` | `1/4`; none | W2 |
| `layers`; `layer_mix` (61) | §2.4; leaf | 1–2 layers; 0–1 | one layer; 0.5 | 2 layers W3 |
| `feedback.amount` (3) | leaf | 0–1.1 | 0 | now |
| `post.{mod, delay, reverb, filter}.*` (16–25) | leaves | as today | as today | now; cutoff minimum = wet kill (3a) |
| `post.delay.sync` (63); `post.reverb.mode` (64) | discrete leaves | 0 off, then divisions; 0–3 (bright room, dark medium, large hall, ambient) | 0; 0 | W2; W3 |
| `post.order` | stage names | only the default order | default | deferred |
| `dry_duck.depth` (62); `.attack_ms`, `.release_ms` | leaf; structure | 0–1; 0.1–500, 1–5000 ms | 0; 5, 80 | W3 |
| `wet_trim_db` (4) | leaf | −24 to +24 dB, wet only: the mode's level match | 0 | 3a |
| `trigger.sensitivity` (26) | leaf | 0–1 | 0.5 | now |
| `modulators`, `routes`, `links`; `macros`, `controls`; `performance` | §2.5; §3; §2.6 | | | W3; 3a; W2 |

**Removed from engine §5's example** (record §3, item 7): `post.bypass` (each stage is
exactly transparent at its neutral leaf, `Params.h:29-37`); the feedback filter fields (the
taming chain is engine-owned for stability, engine §2 commitment 3); the source
`stochastic` (it is `periodic` with jitter 1, `dsp/src/Granular.cpp:412-420`); `size_law`
(never defined); `dry_duck.enabled` (depth 0 is off).

### 2.4 Layers

| Key in `layers[N]` | Kind | Range | Default | Wave |
|---|---|---|---|---|
| `slot_share` | structure | (0, 1], two layers sum ≤ 1 | 1 | W3 |
| `voice_count` (31) | leaf, integer | 1–64 | 64 | W1 |
| `level_db` (32) | leaf | −24 to +6 dB | 0 | W3 |
| `position.source` | enum | `live`, `mark`, `pin`, `grid` | `live` | `mark` 3a (r2); others W3 |
| `position.base_ms` (1), `.spray_ms` (7) | leaves | 1–5000; 0–2000 ms | 250; 20 | now |
| `position.base_sync`, `.spray_law` | enums | `off` or division; `uniform`, `exp` | `off`; `uniform` | W2; `exp` W3 |
| `position.repeat` (29) | leaf, integer | 1–16 passes | 1 | W1 |
| `position.mark.{index, walk, jitter}` | structure | 0–15; `none`, `cascade`, `random`; 0–1 | 0, `none`, 0 | W2 |
| `position.pin.{rearm, rearm_ms}` | structure | `off`, `time`, `onset`, `manual`; 10–20000 ms | `off`, 1000 | W3 |
| `size_ms` (5); `decay_ms` (30) | leaves | 1–500; 0 (off) to 20000 ms | 90; 0 | now; W1 |
| `window.{sustain, skew, smoothness}` (12–14) | leaves | 0–1 | 0.3, 0.5, 0.7 | now |
| `pitch.set`, `pitch.select` | structure | 1–8 `{st −24..24, weight 1–16}`; `cycle`, `random` | `[{0, 1}]`; `cycle` | W1 |
| `pitch.transpose_st` (8) | leaf | −24 to +24 st | 0 | now (renamed) |
| `pitch.spread_cents` (9), `.reverse_prob` (10) | leaves | 0–100 c; 0–1 | 0 | now |
| `pitch.quantize` | structure | mode (off or scale), root 0–11, scale | off | W3 |
| `pitch.glide.{st_start, st_end}`; `.curve` (33) | structure; leaf | −24..24 st; −1..1 | 0 | W3 |
| `pan_spread` (15) | leaf | 0–1 | 0.5 | now |
| `modifiers` | ≤ 2 distinct ops | `svf {band, cutoff_src}`, `crush` | none | W3 |
| `svf.{cutoff_hz, res}` (34, 35); `crush.{bits, downsample}` (36, 37) | leaves | 20–20000 Hz, 0–1; 1–16, 1–32 | 20000, 0.1; 16, 1 | W3 |

Layer 1 has the same leaves with IDs 38–56. Modifier leaves are named per op, not as
engine §6's op-generic `p0..p3`, because hosts need one frozen range, unit and taper per
parameter (companion §5.4). Engine §5's sub-octave op is a pitch-set entry of −12, its
glissando is glide, and its per-grain gain is the step's `gain` (record §3, item 2).

### 2.5 Steps and modulation (W2, W3)

- **`scheduler.steps`** (W2): `{count: leaf 60 (1–16, default 16), order: fixed|shuffle|
  random, entries: ≤ 16 × {slot 0–15, pos_sel, ratio_idx 0–7, gain 0–1, prob 0–1, flags}}`.
  `pos_sel` is read per position source (engine §4): milliseconds for `live`, a mark index
  for `mark`, a slice for `grid`. `flags` bit 0 reverses the step's grain; other bits are 0.
- **`modulators`** (W3): ≤ 2 × `{type lfo|env, shape, sync, attack_ms, release_ms}`, with
  `rate_hz` and `depth` as leaves 65–68. **`routes`**: ≤ 8 × `{from, to: size|cutoff|ratio|
  position|layer_mix|pan, layer, amount}`. **`links`**: ≤ 4 × `{from, to, amount}` between
  per-grain draws.
- **When routes act.** Engine §2 resolves everything at birth, but sweeps on long grains
  (Tunnel B, Seq C, Interrupt C) need modulation during a grain's life. So `cutoff` routes act
  per sample on the current mode's voices that carry an SVF, other destinations act at birth,
  and nothing acts per block (contract #1). Voices of an earlier mode keep their last value.

### 2.6 Performance state (W2)

`performance: {reverse, time_mode: free|subdiv|tempo, subdiv, tempo_source:
internal|midi|host, tempo_us_per_quarter: 200000–3000000}`, default `{false, free, 1/4,
internal, 500000}`. Tempo is integer microseconds per quarter (companion §6.2), keeping
floating point out of the compiler. Freeze is never stored. Until W2, other values are E6.

### 2.7 Validation of a document

Findings carry a JSON pointer, line and column. **Errors** stop compilation:

| # | Rule |
|---|---|
| E1 | Strict RFC 8259: UTF-8, no duplicate keys, no comments, depth ≤ 16. |
| E2 | Unknown key; key or `schema_version` newer than the compiler. |
| E3 | Type mismatch, or a fraction or exponent in an integer field. |
| E4 | Leaf outside its descriptor range. |
| E5 | Enumeration value outside the vocabulary, or a duplicate within a set. |
| E6 | Defined but unsupported: names the feature and wave ("`scheduler.sources`: `clock` needs W2; this build supports periodic, onset, footswitch, midi_note"). |
| E7 | Caps: 2 layers, 8 pitch entries, 2 distinct modifier ops, 16 steps, 2 modulators, 8 routes, 4 links, 8 macros, 8 targets per macro and 32 in all, 4 expression assignments. |
| E8 | References: macros unique by ID; a macro target is a leaf of a present element other than `global.mix`, at most once per macro; route and link endpoints exist; `ratio_idx` < set size; mark index < 16 (`dsp/src/detail/Granular.h:111`); an expression target is a leaf or a macro. |
| E9 | Target `curve` in [1/16, 16]; `in_range` [a, b] with 0 ≤ a < b ≤ 1; both ends of `range` within the target's range (reversed ranges are allowed). |
| E10 | A written `shape` macro has at least one target ("Shape does nothing" is a mapping defect, engine §5). |
| E11 | Two layers: slot shares sum ≤ 1, `voice_count` maxima sum ≤ 64, filter and glide layers fit the tiers (W3). |
| E12 | MODE ≤ 4 KiB, package ≤ 16 KiB. |

**Lint** (`bspc lint`, §8.2) never stops compilation; `--factory`, which CI runs over
`firmware/factory/`, makes L4 and L7–L9 errors. L1 a subnormal leaf was written (it compiles
to +0); L2 the smallest `base_ms` its leaf and macros reach is below `size_ms`·(r − 1) for a
pitch entry, so the guard moves those grains back (engine §3); L3 `activity`, `repeats` or
`time` has no targets; L4 a targeted leaf is further than its display resolution from its
macro's value at the stored position, or a position is omitted (§3.5); L5 no free-running
source and no onset (silent until triggered); L6 `mark` positioning with `decay_ms` 0 holds
the last note (record §2.2); L7 a macro targets a Shift-secondary leaf (§3.1); L8 an
overridden Filter or Space macro breaks the universal endpoints (§3.1); L9 a product string
(`name`, `display_name`, META, tags) matches the denylist of other makers' marks (§11.2).

As built (lane A): a document's text over 1 MiB is refused before parsing (E12), and the
reader's duplicate-key check and column count are linear. L2 compares the smallest `base_ms`
the leaf and macro targets reach with the near guard at the largest size, transpose and spread
they reach, per pitch entry, through `dsp/`'s `NearGuardMs`; L4's display resolution is the
display text, `FormatPlain`, of the leaf against the derived value, and L4 names leaves as
schema 1 does (`layer0.position.spray_ms`, the form `editor.detached` takes); L5's
free-running sources are `periodic` and `clock`; L8 wants a Filter target on
`post.filter.cutoff_hz` from its minimum to its maximum, and every Space target on
`post.delay.mix` or `post.reverb.mix` starting at 0; L9 matches whole words of the id, name,
author, description, tags and display names, case-insensitively.

Engine §3's `d_min_fb` guard is dropped: it prevents a comb at the guard-margin period inside
the feedback loop, but feedback re-enters the ring through a fixed 512-frame FIFO
(`dsp/include/brainscape/Engine.h:16-22`), so every loop is at least 10.67 ms long
(*calculated*). Contract #4's `base_ms → 0` sweep stays.

## 3. The macro layer

### 3.1 The eight knobs

| Knob | Sends | Defined by | Shift secondary (firmware mapping) |
|---|---|---|---|
| Activity | `MacroMove(macro.activity)` | the mode | — |
| Repeats | `MacroMove(macro.repeats)` | the mode | `post.mod.depth` |
| Shape | `MacroMove(macro.shape)` | the mode | `post.mod.rate_hz` |
| Time | `MacroMove(macro.time)`; Subdiv or Tempo in CLOCK modes (W2) | the mode | looper speed (reserved) |
| Space | `MacroMove(macro.space)` | the mode, with a default | `post.reverb.time`, `.mode` (W3) |
| Filter | `MacroMove(macro.filter)` | the mode, with a default | `post.filter.res` |
| Mix | `SetParam(global.mix)` through its taper | the preset | `global.effect_volume_db` (§3.8), the Microcosm's wet-only Effect Volume |
| Loop Level | `perf.loop_level` (reserved) | the looper | loop fade (reserved) |

Activity, Repeats, Shape and Time are per-mode, as on the Microcosm (microcosm.md §2.5).
Space and Filter are per-mode with defaults, not engine §6's fixed mappings, because Engram
and Callback (§11.1) use the post delay rhythmically and their Space must reach only the
reverb. Mix is a preset leaf no macro may target (`Params.h:60-61`). `aux1` and `aux2` have
no knob, making engine §6's eight. In a `clock` mode the Time knob drives tempo and
subdivision (engine §6) and `macro.time` is reachable only by expression, MIDI and hosts (Q7).

Two rules keep the panel predictable. Shift secondaries write their leaves directly, so a
factory macro may not target them, or the knob moved last would silently win (L7). Filter and
Space keep the endpoints players perform with: Filter at 0 reaches the cutoff minimum, which
kills the wet, and at 1 the maximum, which bypasses the filter, monotonically; Space at 0 adds
no wet. An overridden macro must keep them (L8).

### 3.2 Definitions and defaults

A macro is `{id, display_name, targets: [{param, range [lo, hi], in_range [a, b], curve}]}`,
with `in_range` [0, 1] and `curve` 1 by default. An omitted macro takes its schema default,
which `Compile` fills in and the formatter writes, so every preset plays from the pedal:

| Macro | Default targets (`range`, `curve`) |
|---|---|
| activity | `scheduler.overlap` [0.25, 0.85] 1; `layer0.position.spray_ms` [0, 200] 2 |
| repeats | `feedback.amount` [0, 0.9] 1 |
| shape | `layer0.window.sustain` [0.9, 0.1] 1; `layer0.window.skew` [0.3, 0.7] 1 |
| time | `layer0.position.base_ms` [20, 2000] 2 |
| space | `post.delay.mix` [0, 0.5] 1; `post.reverb.mix` [0, 1] 1 |
| filter | `post.filter.cutoff_hz` [40, 20000] 4 |
| aux1, aux2 | none |

Step 4 tunes the defaults. Display names go to META, not MODE (§6.2), so renaming a knob
keeps `sound_hash`. As built: a macro written without `targets` keeps these (per-key
defaulting, §2.2), so `{"id": "space", "display_name": "Room"}` only renames Space; `[]`
empties it.

### 3.3 The evaluator

One exported, non-inline, guarded `dsp/` function, `EvalMacro` (§7.4), computes every macro
and expression fan-out, in the engine and in the app when it derives leaves (§3.5). For a
target and a canonical position `m` in [0, 1]:

```cpp
// One FP operation per statement (profile §3.9); binary64 intermediates; no contraction.
float u;
if (!(m > in_lo))      u = 0.f;
else if (!(m < in_hi)) u = 1.f;
else { double num = (double)m - (double)in_lo, den = (double)in_hi - (double)in_lo;
       u = (float)(num / den); }
float c = (u == 0.f) ? 0.f : (u == 1.f) ? 1.f
        : (curve == 1.f) ? u : detmath::PowF(u, curve);
float v;
if (c == 0.f)      v = lo;                          // exact endpoints
else if (c == 1.f) v = hi;
else { double span = (double)hi - (double)lo, step = span * (double)c;
       v = (float)((double)lo + step); }
return Canonicalize(param, v);
```

The branches keep `PowF` off its domain edges (profile §3.9; the code adds a clamped pole at
x = 0, y < 0, `dsp/src/detail/DetMath.h:127-129`, which `curve > 0` makes unreachable), and a
linear target skips it. The result is monotonic with exact endpoints, *measured* over every
12-bit pot code for nine exponents from 1/16 to 16 and over all 2³⁰ binary32 positions for
exponents 1.5 and 3.0 (record §2.6). The output is never rounded to a grid. `curve` is one
power exponent, as engine §6 folds in the research's `lin|exp|log` and `curve_amount`
(preset-parameter-and-patch-format.md §4.2): below 1 is the "log" shape, above 1 the "exp".

As built: `EvalMacro` landed in `dsp/` with lane A (`brainscape/ModeEval.h`), beside
`NearGuardMs` for lint L2, because lint and `derive` need it before R6; nothing in the engine
calls it until R6, so it is sound-neutral by construction. 848 bytes on the M7 (*measured*).
Since lane C (R6) the engine's `MacroMove` and `Expression` events run the same bodies
(`detail/ModeEvalBody.h`) inside the engine's own guard, so one evaluator serves the engine,
lint, `derive` and the app.

**Cost on the M7:** about 538 cycles (1.1 µs) per curved target (*estimated* with LLVM 14's
`llvm-mca` Cortex-M7 model): an 8-target move is about 0.9 % of a 48-frame block, paid only
while a pot moves. Tables would be 17 times cheaper but need 32 KiB per mode and DetMath in
the compiler (record §2.6).

### 3.4 Macro moves and expression are events

- **`MacroMove`** (event 4, §7.4) carries the macro ID and a position canonicalized to
  [0, 1]. At its frame the engine applies the targets in list order as leaf changes, exactly as
  `SetParam` events would (profile §5.11). An undefined macro does nothing.
- **A move overwrites all its targets.** A leaf edited by hand keeps its value until the next
  move of a macro that targets it; of two macros on one leaf, the later move wins.
- **Expression** (event 5) carries the pedal position. CTRL's up to 4 assignments
  `{target, lo, hi, curve}` (companion §6.2) apply in order: a leaf is set by `EvalMacro`, a
  macro receives a `MacroMove`. Without assignments the device's default assignment applies
  (§3.8), and without that the pedal does nothing, avoiding the Microcosm's unassigned pedal
  defaulting to Filter and muting the effect (microcosm.md §2.6). Factory modes may carry
  curated assignments.
- **Parity.** CTRL is outside `sound_hash` yet reaches the sound through expression, so a
  recipe or PARITY request with expression events also records `control_hash` (§6.3).

As built (lane C): an assignment maps the pedal position as a macro target with `in_range`
[0, 1] would, through its `lo`, `hi` and `curve`; on a macro the result is the macro's
position, and its targets follow as a `MacroMove`'s. The exported `EvalExpression(mode, control,
position, out, cap)` writes the resulting leaves in application order (at most 32). A
`MacroMove` or `Expression` with a non-macro id, an undefined macro or no assignment does
nothing, and `SetParam` on a macro row stays a no-op.

### 3.5 Positions, leaves and soft takeover

Macro positions in CTRL are pickup references (companion §6.2): after a load each macro pot
is locked until it reaches its stored position; Mix picks up against
`NormalizedFromPlain(global.mix)`. A firmware setting, **"knobs follow"** (Q8), instead sends
a `MacroMove` per pot at the load frame, as the Microcosm behaves when its selector turns.

Pickup only avoids a jump if the stored leaves already equal what the macro produces at the
stored position, since the unlocking move overwrites every target. Hand-typed pairs almost
never match bit for bit (the first draft's own example missed on three macros, record §2.6),
so the leaf is derived, not typed:

- **The editor derives by default**: the author sets positions, and each targeted leaf is
  written as `EvalMacro` at its position unless listed in `editor.detached`. A **"solve
  position"** command does the reverse, bisecting over binary32 positions for the one whose
  value lands nearest the leaf (`EvalMacro` is monotonic). `bspc derive` does both for
  documents written by hand (§8.2).
- **The compiler only checks**: L4 flags a leaf further than its display resolution from the
  derived value, or an omitted position (compiled as 0.5); both are errors for factory
  packages.

As built (lane A): "solve position" compares distances exactly; of the positions that land
equally near the leaf it keeps the stored one, so a derived document solves back to itself
byte for byte (`EvalMacro` is flat over runs of positions, and 0.49999997 often gives what 0.5
gives), and otherwise takes the smallest, also past either end of the range. `derive` logs
each leaf's net change once, and says when the stamp has gone stale.

### 3.6 What hosts see

A host macro move becomes a `MacroMove`; conflicts resolve per event in `(frame, sequence)`
order (profile §5.11), not per audio block (preset-parameter-and-patch-format.md §4.3). What
a host may *record* is the owner's choice (Q12): JUCE 9.0.3 has no display-only report, since
every reported value reaches the host as an edit or an output parameter change, which hosts
record in write modes, and no format exposes which leaves have lanes (record §2.8).

- **(b), recommended for Microcosm fidelity:** macros, `global.mix`, the effect volume and the
  performance rows are automatable; leaves are registered non-automatable (the existing
  `kParamAutomatable` flag) and the fan-out is reported as their new values, which hosts do not
  record.
- **(a):** leaves stay automatable and the fan-out is never reported; the wrapper treats a host
  set equal to the last value it reported for that leaf as an echo, so a stale display cannot
  overwrite a macro's result. Generic host views show stale leaves.

A recording test in Reaper and Cubase, and Live and Logic where available, joins the release
checklist (§10.4).

### 3.7 Integer-valued leaves

Counting leaves are stored as their exact canonical binary32, never gridded (companion §5.5),
and read by the engine as `RoundHalfAwayI32(value)` (profile §3.9), in range because
canonicalization clamps first; the UI shows that integer.

### 3.8 Device settings

Settings of the player's rig persist on the device and in the app, survive every load and
`Restart`, and are recorded in render recipes and PARITY requests:

- **`global.trigger_offset`** (81): the calibration gesture's offset (§4.2).
- **`global.effect_volume_db`** (82): Shift+Mix, the player's wet level, −24 to +12 dB,
  default 0, applied with `wet_trim_db` (§7.2). The trim stays the curator's level match, off
  the panel, so a player's turn never erases it and a mode change never resets it.
- **Mix lock** (off by default): the producer writes the current `global.mix` into the staged
  preset, which the library labels modified, and the Mix pot stays live.
- **Default expression assignment** (none by default), set by the plug-in-and-move gesture
  (microcosm.md §2.6) or in the app: the producer substitutes it into a staged CTRL that has
  none.

The first two are `Global` rows the engine reads; the producer applies the other two before
staging, so the engine's load contract does not change.

## 4. Parameter identities

### 4.1 New descriptor columns and per-kind rules

The table stays contiguous and ordered by ID (`dsp/src/Engine.cpp:24-30`). Each row gains:

- **`kind`**, with these rules:

| Kind | In STAT | `SetParam` | `LoadPreset` | Host (§3.6) |
|---|---|---|---|---|
| `Leaf` | yes | stores | default, then stored value | registered |
| `Macro` | never; position in CTRL | no-op; moved only by `MacroMove` | nothing | automatable; the wrapper sends `MacroMove` |
| `Performance` | never as a leaf; stored state in STAT's tail | no-op; own events | freeze off; step 4 | automatable; own events |
| `Global` | never | stores | kept, also across `Restart` | per row |
| `Reserved` (an unbuilt feature's ID), `Retired` (null name, never reused) | never | no-op | nothing | not registered |

  `pending_` and `active_` hold `Leaf` and `Global` rows only; `Init` gives `Global` rows
  their defaults and the producer restores them.
- **`domain`**: a bitmask of what a change rebuilds (`Granular`, `Post`, `Mix`, `Feedback`,
  `Wet`, `Detector`); it replaces dispatch by ID range (§7.2). The cutoff is `Post | Wet`.
- **`sinceRev`**: the revision that made the row a `Leaf` (§7.3).

### 4.2 The ID table

IDs 1–28 keep their numbers; the renames, retirements and additions are made now, while
nothing is released. "Wave" is when a row takes its kind; until then it is `Reserved`.

| ID | Name | Change | Kind, domain | Wave |
|---|---|---|---|---|
| 1 | `layer0.position.base_ms` | keep | Leaf, Granular | now |
| 2 | `global.mix` | keep | Leaf, Mix | now |
| 3 | `feedback.amount` | keep | Leaf, Feedback | now |
| 4 | `wet_trim_db` | **rename** from `out_trim_db`; **wet only** (sound-changing) | Leaf, Wet | 3a |
| 5–7, 9–15 | `layer0.size_ms`, `scheduler.overlap`, `layer0.position.spray_ms`; `layer0.pitch.{spread_cents, reverse_prob}`, `scheduler.jitter`, `layer0.window.{sustain, skew, smoothness}`, `layer0.pan_spread` | keep | Leaf, Granular | now |
| 8 | `layer0.pitch.transpose_st` | **rename** from `layer0.pitch.st`: an offset over the pitch set | Leaf, Granular | now |
| 16–25 | `post.mod.*`, `post.delay.{time_ms, fb, mix}`, `post.reverb.{time, mix}`, `post.filter.{cutoff_hz, res, morph}` | keep; cutoff minimum becomes the wet kill | Leaf, Post (cutoff Post \| Wet) | now; kill 3a |
| 26 | `trigger.sensitivity` | keep, as the mode's intended threshold | Leaf, Detector | now |
| 27 | — | **retire** `scheduler.onset_trigger` → `onset` in `scheduler.sources` | Retired | r2 |
| 28 | — | **retire** `layer0.position.source` → `layers[0].position.source` | Retired | r2 |
| 29–31 | `layer0.position.repeat`, `layer0.decay_ms`, `layer0.voice_count` | add | Leaf, Granular | W1 |
| 32–37 | `layer0.level_db`, `layer0.pitch.glide.curve`, `layer0.svf.{cutoff_hz, res}`, `layer0.crush.{bits, downsample}` | add | Leaf, Granular | W3 |
| 38–56 | layer 1: `base_ms`, `spray_ms`, `repeat`, `size_ms`, `decay_ms`, `voice_count`, `level_db`, `pan_spread`, `window.*` (3), `pitch.{transpose_st, spread_cents, reverse_prob, glide.curve}`, `svf.*` (2), `crush.*` (2) | add | Leaf, Granular | W3 |
| 57–59 | `scheduler.intermittency`, `scheduler.burst.{count, spacing_ms}` | add | Leaf, Granular | W1 |
| 60, 61 | `steps.count`; `layer_mix` | add | Leaf, Granular | W2; W3 |
| 62 | `dry_duck.depth` | add | Leaf, Mix | W3 |
| 63, 64 | `post.delay.sync`; `post.reverb.mode` | add, discrete | Leaf, Post | W2; W3 |
| 65–68 | `modulator{0,1}.{rate_hz, depth}` | add | Leaf, Granular | W3 |
| 69–76 | `macro.{activity, repeats, shape, time, space, filter, aux1, aux2}` | add | Macro | 3a |
| 77, 78 | `perf.freeze` (the plugin's provisional ID, `plugin/src/BrainscapeParam.cpp:310-312`); `perf.expression` (companion §5.6) | add | Performance | 3a |
| 79 | `perf.reverse`: the Microcosm's FWD/REV toggle, also stored as performance state | add | Performance | W2 |
| 80 | `perf.loop_level` | reserve (companion Q29) | Reserved | looper |
| 81 | `global.trigger_offset` (§3.8, companion §6.2) | add | Global, Detector | phase D |
| 82 | `global.effect_volume_db` (§3.8) | add | Global, Wet | 3a |

Ranges and defaults are those of §2.3–§2.4. The detector uses
`clamp(trigger.sensitivity + global.trigger_offset, 0, 1)`, so a mode keeps its threshold and
a player's calibration applies everywhere. Trigger, Tap and Bypass stay outside the table
(companion §5.7).

As built (lane C): rows 27 and 28 are `Retired` tombstones (null name, no domain) since sound
revision 2; the C++ enumerators `OnsetTrigger` and `PositionSource` keep their ids, so code
naming them still builds, and a `SetParam` on them is a no-op. Their display rows stay, never
registered. Row 81 stays `Reserved` until phase D, so the detector reads
`trigger.sensitivity` alone. The cutoff shows `Kill` at its minimum.

A transpose leaf survives beside the pitch set because with the default set `{0}` the engine
computes `(0 + transpose) + detune`, bit for bit today's `PitchSt + detune`
(`dsp/src/Granular.cpp:101-104`). `steps.count`, `layer_mix`, `glide.curve`, the burst
fields and the modulator rates and depths are leaves because engine §5's acid test makes
them macro targets, and macros target leaves only.

### 4.3 Tapers and display

The shared table keeps its power tapers (`Linear`, `Square`, `Quartic`,
`dsp/include/brainscape/ParamDisplay.h:114-118`), which need no DetMath, so companion §5.4's
log tapers are withdrawn. Each new row gets a taper, step count, display kind, flags, title
and group: integer leaves are discrete; the cutoff shows "Kill" at its minimum as it shows
"Off" at its maximum; the new groups are Scheduler, Layer 2, Modifiers, Modulation, Macros,
Performance and Device. All of this freezes with the IDs at step 6.

### 4.4 Migration

Nothing is released, so migration serves internal state:

- **Skeleton sessions** (`BSWS` v1, `plugin/src/StateCodec.h:22-33`) become documents: IDs
  1–26 by name; 27 ≥ 0.5 adds `onset` to the sources; 28 ≥ 0.5 sets position source `mark`;
  macros take their defaults. A session with a nonzero trim then sounds different (the trim no
  longer scales dry), and the plugin says so once.
- **The golden corpus**: parameter-list presets gain structure only by encoding a document and
  decoding it, never by assigning `ModeBlob` fields; the ablations of 27 and 28
  (`Corpus.cpp:258-259`) switch structure; the automation preset's toggles of 27 and 28
  (`Corpus.cpp:78-79`) become Spillover loads between two modes.
- **Host automation lanes** on renamed or retired names in internal builds are lost.

### 4.5 The release gate

Companion §5.7's gate is met when this table is implemented, the macro rows exist, every row
has its taper, the host model is chosen (Q12) and companion Q17's VST3 numeric-ID policy is
settled. Step 6 then freezes the table: a committed manifest of every published row (ID,
name, kind, range, default, taper, steps, `versionHint`, and the derived VST3, AU and CLAP
numeric IDs) is checked on every pull request. Logic identifies AU parameters by index, and
JUCE orders them by version hint, then by a hash of the string ID, so a parameter added later
must carry a hint above every existing one or saved automation shifts (record §2.8). Rows
published at step 6 get hint 1; a row activated later gets its release's ordinal, and CI
checks that it exceeds every earlier hint. Published rows are never renumbered, renamed or
reused.

As built (lane G): the manifest check and the version-hint check wait for step 6's committed
manifest, which needs Q12 and companion Q17 settled first; until then the table may change in
place (principle 5), so there is nothing to check against.

## 5. The compiled preset

### 5.1 Decoded types

`dsp/include/brainscape/Mode.h` and `Preset.h` define the decoded package as fixed-capacity
data with fixed-width members and `enum class : uint8_t` only (no pointers, `bool`, `size_t`
or `long`), and `static_assert`s on `sizeof` and `offsetof` checked on x64, arm64 and the M7:
one struct of `bool`, plain enum, `size_t`, `long`, `float` and pointer *measured* 20, 32 and
40 bytes on the M7, MSVC x64 and Linux x64 (record §2.5). `ModeBlob` mirrors §6.2's chunks at
their caps, `targets` being `{u32 param; f32 lo, hi, inLo, inHi, curve}`, plus `features` and
`modeHash`. `PresetState` holds `soundRev`, the leaves (`kMaxLeaves` 128), the `ModeBlob`, a
`ControlState` (macro positions, expression assignments) and a `PerformanceState`.

**Defaults.** `ModeBlob`'s member initializers are the schema-default mode, which plays as
r1 does: sources {periodic, footswitch, midi_note}, one live layer, the set {0: 1}, the
default macros. Every leaf-only producer today (plugin, audition, golden harness) therefore
still plays what it played. A unit test checks the default's `modeHash` against the compiler's
encoding of an empty document.

`ModeBlob` is about 1.5 KiB and `PresetState` about 2.6 KiB (*calculated* from the caps),
within engine §7's 4 KiB slot and, three copies at a time, the firmware's 12 KiB staging
budget. **A `PresetState` is never a stack local in `dsp/`**: MSVC x64 inserts `__chkstk` into
frames above 4 KiB, which the symbol audit rejects (*measured*, record §2.7).

### 5.2 Versioning and portability

- **Fields are written one by one**, little-endian, floats as raw bits; no struct is copied to
  bytes or hashed (companion §6.3).
- **One state, one encoding**: unused slots and padding are zero, floats canonical and every
  chunk in the canonical form of §5.3, so decoding then encoding reproduces the STAT, MODE and
  CTRL bytes, hence `sound_hash` and `control_hash`; the header's flags, META, JSON and unknown
  sections are carried byte for byte. The prototype's fuzz evidence (81,971 accepted mutants,
  same `sound_hash`) is for the fixed-capacity sketch (record §2.5); lane B re-runs it on the
  chunked layout before this is cited as measured.
- **Additive evolution**: an older decoder rejects an unknown MODE chunk as
  `UnsupportedFeature`, naming it, so a new chunk does not bump `blob_format`; a changed layout
  does, and after step 6 the firmware keeps older decoders (companion §6.5). A macro or
  expression target on a later wave's leaf (a `Reserved` row of the older build) is
  `UnsupportedTarget`, named, not a corrupt target (added when lane B was built).
- **No floating point, allocation or C library beyond `memcpy` and `memset`**: the probe's M7
  decoder and validator had no floating-point instruction in 4,775 bytes (*measured*). Byte
  comparisons are loops, since `memcmp` is not on the symbol allowlist.

### 5.3 The validator

`dsp/src/blob/` holds `DecodePreset` and `ValidateMode`; the firmware links both, the compiler
runs both on every package it writes, and `LoadPreset` runs `ValidateMode` on every load
(§7.3). Floats are compared as integers (a binary32 bit pattern maps to an integer that orders
like its value), so no guard is needed.

- **Header and STAT** (`DecodePreset`, one error code each): length, magic, formats,
  `total_bytes`, `sound_rev` nonzero, section bounds and order, duplicate or missing sections,
  unknown header flags, the three hashes; STAT count ≤ 128, strictly ascending IDs, values
  finite and canonical; performance enumerations.
- **MODE**: chunk order, duplicates, unknown tags, lengths, enumerations, source bits only for
  the vocabulary, counts and indices, finite in-range floats, zero padding and zero entries
  past every count; MACR's macro IDs strictly ascending within 69–76, each `first` the running
  sum of the counts before it and `target_count` their total; a present optional chunk equal to
  its absent default; `features` equal to what the content requires and **supported by this
  build**.
- **CTRL**, when present: one position per macro MACR defines, by ascending ID, each canonical
  in [0, 1];
  `expr_count` ≤ 4; targets that are `Leaf` or `Macro` rows; `lo`, `hi` within the target's
  range; `curve` in [1/16, 16]; zero padding.
- **META**: per-field byte limits (§2.2), valid UTF-8 without control characters, `family` in
  its enumeration, display names only for defined macros, no duplicates. `DecodePreset` parses
  META, so the firmware has one parser.
- **Semantic** (`ValidateMode`, integer-only): E8–E11 and the rule that absent elements'
  leaves hold their defaults. It takes no sample rate, since the engine runs only at 48 kHz
  (engine §11).
- **Out of range is not invalid.** A known leaf outside this build's range decodes, and
  `LoadPreset` canonicalizes it and reports the load inexact (profile §5.10): a package from a
  build with another range is not corrupt. Lint findings are the compiler's alone.

## 6. The `.bsp` package and canonical JSON

### 6.1 Container

A 128-byte header, then sections; at most 16,384 bytes in all.

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 0 | `magic` | 4 bytes | `BSPK` |
| 4, 6 | `package_format`, `flags` | u16, u16 | 1; bit 0 `FACTORY` (as built: the compiler sets it for ids under `factory.`), bit 1 `JSON_STALE` (companion §6.9), others 0 |
| 8 | `sound_rev` | u32 | revision of the build that compiled or stamped it, never 0 |
| 12, 16 | `blob_format`, `schema_version` | u32, u32 | 1, 1 |
| 20, 24, 28 | `total_bytes`, `section_count`, reserved | u32 × 3 | reserved is 0 |
| 32, 64, 96 | `sound_hash`, `control_hash`, `package_hash` | 32 bytes each | §6.3 |

Each section is `{u32 tag, u32 length, payload, zero padding to 4}`. Known sections come in
the order STAT, MODE, CTRL, META, JSON; STAT and MODE are required. A section a decoder does
not know may follow MODE anywhere, in the writer's order; the decoder skips it, and the slot
store, which keeps whole packages, keeps it verbatim. A new section that changes what the
engine plays bumps `blob_format`.

### 6.2 Sections

**STAT**: `u32 n` (≤ 128), `n × {u32 id, u32 bits}` with ascending IDs, then
`{u8 reverse, u8 time_mode, u8 subdiv, u8 tempo_source, u32 us_per_quarter}` (§2.6). The
compiler writes every `Leaf` row of a present element, defaults included (companion §6.1).

**MODE**: `u32 features, u32 chunk_count`, then chunks `{u32 tag, u32 length, payload}` in
this order, each at most once; "always" chunks are required, the others present exactly when
used.

| Tag | Presence | Payload | Max B | Wave |
|---|---|---|---|---|
| `SCHD` | always | u8 sources (bits: periodic, clock, onset, footswitch, midi_note), layer_count, subdiv, step_order, pad[4] | 8 | 3a |
| `LAYR` | always | per layer 36 B: 13 u8 enumerations of §2.4 and a pad, u16 scale mask, f32 slot_share, mark_jitter, pin_rearm_ms, glide_st_start, glide_st_end | 72 | 3a |
| `PSET` | a set other than `{0: 1}` | per layer 68 B: u8 count, pad[3], 8 × {f32 st, u16 weight, u16 0} | 136 | W1 |
| `STEP` | steps | u8 count_max, pad[3]; 16 × {u8 slot, ratio_idx, flags, 0; f32 pos_sel, gain, prob} | 260 | W2 |
| `MODS` | modulators | 2 × {u8 type, shape, sync, 0; f32 attack_ms, release_ms} | 24 | W3 |
| `ROUT`, `LINK` | routes, links | u8 n, pad[3]; n × {u8 from, to, layer or 0, 0; f32 amount} | 68, 36 | W3 |
| `DUCK` | dry duck | f32 attack_ms, release_ms | 8 | W3 |
| `MACR` | always | u8 macro_count, target_count, u16 0; macros × {u32 id, u8 first, count, u16 0}; targets × {u32 param; f32 lo, hi, in_lo, in_hi, curve} | 836 | 3a |

The largest MODE is 1,528 bytes (*calculated*); a one-layer mode with six macros and 18
targets is about 560.

**CTRL**: `u8 macro_count, u8 expr_count, u16 0`, then `{u32 macro_id, u32 position_bits}`
by ascending ID, then `{u32 target_id; f32 lo, hi, curve}` per assignment; at most 132
bytes. Footswitch and per-preset MIDI assignments are reserved for the firmware design.

**META**: length-prefixed UTF-8, validated by `DecodePreset` (§5.3): `id`, `name`, `family`,
`author`, `description`, tags, and macro display names as `{u32 macro_id, string}`. The pedal
shows names from META and never parses JSON. **JSON**: the canonical source (§6.4), stamped.

### 6.3 Hashes

- `sound_hash` = SHA-256(`u32 len(STAT)` ‖ STAT ‖ `u32 len(MODE)` ‖ MODE): "the same sound"
  (companion §6.3). Knob positions, names and JSON formatting do not change it.
- `control_hash` = SHA-256(`u32 len(CTRL)` ‖ CTRL), length 0 if CTRL is absent.
- `package_hash` = SHA-256 of the whole package with this field zeroed.
- `modeHash` = SHA-256 of the MODE payload, computed by `DecodePreset`: the library groups
  presets by it. The engine never trusts it (§7.3).

The SHA-256 digest core moves from `dsp/tests/golden/Sha256.h`, already integer-only, to
`dsp/src/blob/` behind a public header; its hex formatting (`snprintf`, `std::string`) stays
in the test helpers, and the golden harness, `test_state.cpp` and the plugin tests change
their includes (lane B).

### 6.4 Canonical JSON

What `bspc fmt` writes and `bspc fmt --check` gates (companion §6.4):

- **Content.** Every `Leaf` row of a present element, defaults included (a landing feature
  adds keys, and its pull request re-formats the factory documents); structure fields that
  differ from their defaults, plus a core always written (identity, sources, each layer's
  position source and pitch set, the six performance macros, macro positions); derived fields
  stamped.
- **Layout.** Schema key order, two-space indent, LF, UTF-8 without BOM, final newline;
  arrays of scalars on one line, other arrays one element per line; strings escape only `"`,
  `\` and control characters. Sets (`scheduler.sources`) are sorted in vocabulary order and
  `macros` by macro ID; targets, pitch entries and steps keep their authored order, which
  carries meaning.
- **Numbers.** The shortest decimal that reads back to the same binary32 under correct
  rounding (closest, even digit on a tie: `std::to_chars` without precision), laid out by
  ECMAScript's `Number::toString` rules (`0.55`, `375`, `1e-7`, `3.4028235e+38`), with one
  exception: bits `0x15AE43FD` and `0x95AE43FD` are written `7.0385307e-26` and
  `-7.0385307e-26`, eight digits, because their shortest text read through binary64 rounds to
  the neighbouring float; they are the only two (*measured* exhaustively, record §2.3). So
  `JSON.parse` plus `Math.fround` reads every canonical number exactly, but JavaScript writes
  a float's binary64 value (`0.550000011920929`): a JavaScript client needs its own binary32
  shortest writer, checked against the committed exhaustive hash.
- **Reading.** Strict RFC 8259; numbers correctly rounded to binary32, ties to even;
  overflow is an error; integers range-checked as 64-bit. RFC 8785 and hex floats stay out.

### 6.5 In-house number and JSON code

`compiler/src/Number.*` is an exact binary32 reader and shortest-digit writer of about 330
lines using only integers. Its prototype (*measured* on one host, record §2.3), a pure
shortest writer, round-tripped all 4,278,190,080 finite floats with one text hash under MSVC
19.40, GCC 11.4, GCC 14.2 and Clang 14, wrote `std::to_chars`'s digits for every float, read
1,889,845,423 strings at and beside halfway points correctly, and has no floating-point
arithmetic instruction on the M7. The specified writer differs from it, and from
`std::to_chars`, only in §6.4's two values; lane A commits the exhaustive hash of that writer.

The standard `float` `from_chars` and `to_chars` were exact where measured, but libc++ gates
them on macOS (deployment targets 13.3 and 26.0; read from its sources), which is why the
plugin falls back to locale-dependent `strtof` (`plugin/src/BrainscapeParam.cpp:29-44`). The
plugin's text parser switches to this code, so typed and compiled values have the same bits.
Typed text is looser than JSON (`.5 s`, `5. ms`, `05`, `+25`, `plugin/src/BrainscapeParam.cpp:47-74`),
so `Number` gets a lenient entry point for it with the same exact rounding.

`compiler/src/Json.*` is a strict reader that keeps each number's text until its schema type
is known, rejects duplicate keys and invalid UTF-8, handles all escapes including surrogate
pairs and reports line, column and pointer; plus the canonical writer. The golden harness's
reader stays as it is: it rejects `0.55` and accepts `007`, raw control characters, invalid
UTF-8 and duplicate keys (*measured*, record §2.4).

**Dependency decision (owner, Q1).** Companion §3.5 and §6.4 name a vendored `fast_float`.
None is needed: it supplies only a reader, and a writer would still need another library. The
recommendation is not to request it.

### 6.6 Version fields

Companion §6.5 stands, except that a new MODE chunk does not bump `blob_format`. The header's
`sound_rev` is the compiling build's constant `kSoundRevision`, so compiling stays a pure
function of the JSON (companion §6.6).

## 7. Engine runtime changes

### 7.1 Change list

"Sound" says whether the change can alter output for some input (profile §5.12); "Lands"
names the pull request of §7.6.

| # | Change | Sound | Lands |
|---|---|---|---|
| R2a | Descriptor `kind`, `domain`, `sinceRev`; rows 29–82 at their final names, `Reserved` until their wave; renames of 4 and 8; §4.1's per-kind rules | no, by construction | ID table |
| R4 | `PresetState` with MODE, CTRL, performance state, `soundRev`; `DecodePreset`, `ValidateMode`, SHA-256 | no | blob |
| R8 | Random-number key extension keeping r1's keys (§7.5) | no until used | blob |
| R1 | `ApplyParam` dispatches on the `domain` bitmask; one wet gain function (§7.2) | yes | r2 |
| R2b | Retire 27 and 28 into structure | yes | r2 |
| R3 | `wet_trim_db` and the effect volume scale the wet signal after the post chain; the cutoff minimum mutes it | yes | r2 |
| R5 | `LoadPreset` with validation and steps 1–5 (§7.3); the active mode in the Warm arena; structure read from it | no, for presets equivalent to r1's | r2 |
| R6 | `MacroMove` and `Expression` events; exported `EvalMacro` | yes: new inputs | r2 |
| R7 | Spillover switch style, Trails or FastCut | yes (FastCut) | r2 |
| R3b | The Mix law, if approved (Q13) | yes | r3 |
| R9–R12 | Trigger sources, burst, intermittency; pitch sets; `repeat` and `decay_ms`; `voice_count` | yes (the set `{0}` keeps r1's bits) | W1, one each |
| R13 | CLOCK and tempo events, performance state, steps, mark walk, synced times, global reverse, velocity | yes | W2 |
| R14 | Modulation, glide, SVF and crush with tiers, layer 2, `dry_duck`, reverb modes, `POS_PIN`, `POS_GRID`, quantization | yes | W3 |

**The Mix law (R3b, Q13).** Mix is a linear crossfade (`dsp/src/Engine.cpp:895-906`), so at
the first set's stored Mix of 0.35–0.55 every mode plays 1.2–8.8 dB quieter than bypass
(*calculated*, record §2.2), the Microcosm's most-cited level complaint (microcosm.md §12.1).
The recommended law keeps dry at unity up to the middle and wet at unity from it:
`dry = min(1, 2(1 − m))`, `wet = min(1, 2m)`, exact at `m` = 0 and 1 for contract #2. It
changes every preset with 0 < mix < 1, so it takes its own bump after r2.

**Budgets.** `Engine::Impl` (6,032 of 6,656 bytes on the M7, `Engine.h:24-30`) holds the
64-voice pool (`dsp/src/detail/Granular.h:134`), so each `Grain` byte costs 64: it grows about
0.6–1.1 KiB in 3a (FastCut included), 1.3–1.5 KiB in W1 and 2.5 KiB in W3 before packing
(*estimated*), all within engine §7's DTCM row. Each wave's pull request raises
`kEngineImplBytes` under the DTCM map audit (engine §10, gate 10). `Grain` is 72 of its 128
bytes (`Granular.h:25-38`); 3a adds 4, W1 about 20 and W3 about 40, so W3 must pack it
(risk 4). The active mode goes in the Warm arena through `PlanMemory`.

As built (lane C, *measured*): `Grain` is 80 bytes (the 4-byte `fadeStart` and 4 of
alignment), so `Engine::Impl` grew by 568 bytes on the M7 (512 of them the grain pool's), to
6,600 of the raised 7,168 (`kEngineImplBytes`; x86-64: 6,752 of 7,424, from 6,168). The active
mode and its CTRL, 1,616 bytes rounded to 16, end the Warm arena. No DTCM map audit exists yet
to check the raise against.

### 7.2 Parameter domains: the routing fix

`Engine::Impl::ApplyParam` rebuilds granular parameters only for IDs below 16
(`dsp/src/Engine.cpp:569-578`), but `RebuildGranularParams` reads IDs 27 and 28 (`:638-639`),
so a change to only `OnsetTrigger` or `PositionSource` never takes effect: an event turning
either on at frame 0 rendered the 4 s plucks vector (8 onsets) bit-identically to leaving it
off, while loading it on differed from frame 13,748 (27) and 1,849 (28) (*measured*, record
§2.1). The golden script hides this by changing them alongside granular parameters
(`Corpus.cpp:59-79`), and every new granular ID would be misrouted the same way. R1
dispatches on `domain` through an exhaustive switch, marking each domain in the bitmask.

A value can feed more than one rebuild. One function, `WetGainTarget(trim, effectVolume,
cutoff)`, gives the smoothed wet gain's target: 0 at the cutoff minimum, else
`Exp2F((trim + effectVolume) · log2(10)/20)`. The `Wet` rebuild runs it and the cutoff marks
`Post | Wet`, so a lone cutoff change engages the kill at its frame and a lone trim change
while killed stays muted; today's trim handler (`:561-565`) and the raw-cutoff bypass in
`RebuildPostParams` (`:593-597`) would each re-open the bug class alone. Tests: one lone change
per `Leaf` row against the same change among other edits, and §10.3's kill cases.

As built (lane C): a change marks its row's domain bits; before the next frame renders,
`RebuildDirty` rebuilds each marked domain once, through a switch that names all six (Mix,
Feedback, Wet and Detector included, which revision 1 applied at once by ID; the order cannot
matter, their state is disjoint). `WetGainTarget` is the target of a 10 ms smoother, as the
trim's was, that scales the wet signal after the post chain and before the mix: `dry·(1 − mix)
+ (wet·g)·mix`. At `g` = 1 every product is exact, so a preset without trim plays revision 1's
bits; the kill is exact silence once the smoother snaps to 0 (about 0.46 s). The trim and the
effect volume add in decibels before the one conversion, so `(−6, 0)` and `(0, −6)` give the
same bits. The bypass at the cutoff's maximum stays the post rebuild's.

### 7.3 Loading a mode, without a blob ring

`LoadPreset` follows profile §5.10, and so does the `SpilloverLoad` event:

0. `ValidateMode` checks the staged `ModeBlob` and CTRL (integer-only, about one pass over
   1.5 KiB); on failure nothing is applied (`applied` false, `invalidMode` set).
1. Every `Leaf` row takes its default; `Global` rows keep their values (§4.1).
2. Every stored leaf, canonicalized, by ascending ID. A STAT ID is unknown, and ignored,
   unless its row is a `Leaf` with `sinceRev` ≤ `kSoundRevision`. A `Leaf` row the package
   lacks counts as missing only if its `sinceRev` ≤ the package's `soundRev`, so a new leaf does
   not make older packages inexact; a `soundRev` of 0 or above `kSoundRevision` counts as
   `kSoundRevision`, so a leafless state is never exact (`DecodePreset` rejects `sound_rev` 0).
3. The `ModeBlob` and CTRL expression table are copied into active storage; every domain is
   marked for rebuild.
4. Performance state (defaults until W2); freeze off. A non-default field this build cannot
   play counts in a new `LoadReport.unsupported`, making the load inexact.
5. Exact: `Restart`, which rebuilds from the new mode. Spillover: the epoch restarts at the
   load frame (profile §5.9).

**One active mode replaces engine §5's 4-slot ring.** A grain resolves at birth everything it
reads later (position, passes, decay gain, ratio, envelope, gains; in W3 filter
coefficients), and during-life routes act only on the current mode's grains (§2.5), so no
grain needs its birth mode's data after a switch. The staged copy lives in the producer's
`PresetState`, which already stays valid until its Spillover event retires
(`dsp/include/brainscape/Engine.h:135-147`); the engine copies it at the event's frame, about
1.5 KiB in a few microseconds on the M7 (*estimated*). This removes `PublishMode`, slot
reclamation and its race (engine §5, §9), 12 KiB of AXI memory and the producer's wait for a
slot; `ActiveModeInfo` becomes a copy of the active `modeHash` and display data.

**Trails and FastCut.** A Spillover event's `id` picks the style: 0 Trails (default), where
grains born before the load finish as resolved, or 1 FastCut, where they fade linearly to zero
over `kFastCutFrames` = 128 frames (2.67 ms), a shared constant, so the cut does not click.
Each grain carries a `fadeStart` frame (4 bytes): a FastCut load stamps it on every sounding
grain not already fading, so several loads within 128 frames each fade their own grains.

**Sequencing state.** `Restart` resets the pitch-cycle index, bursts, step position and
modulator phases. A Spillover load keeps them only if the incoming `ModeBlob` equals the
active one, which the engine checks itself, word by word, rather than trusting `modeHash`;
every index is reduced modulo its table's size on any load. Scheduler phase and grains always
carry over.

As built (lane C): step 0 is `ValidateMode`'s rules on the mode and CTRL, structural and
semantic, but not STAT's: the leaves are step 2's, which canonicalizes them and counts what is
unknown, duplicated or changed instead of refusing the load (a non-canonical value or an
unsorted leaf list in a state built in memory stays loadable, as before). The comparison of
modes is `ModeBlob`'s bytes up to `modeHash` (the struct has no implicit padding), and each load
whose mode differs counts in `Engine::ModeSwitches()`; sound revision 2 has no sequencing state
to reset, so the reset is a hook for W1. FastCut stamps `fadeStart` as the grain's own frame
index (a `uint32_t`, no absolute frame), shortens the grain to end with its fade, which frees
its voice, and renders frames from `fadeStart` on with the envelope times `(128 − k)/128`,
exact multiples of 2⁻⁷; frames before it render as any grain's, so a Trails load and a grain no
load cut keep revision 1's arithmetic. A direct `LoadPreset(…, Spillover)` takes the style as
an optional last argument (Trails by default), so a wrapper splitting blocks itself, as the
golden harness's split delivery does, can cut too. No `ActiveModeInfo` exists yet.

### 7.4 Events and API

| Type | No. | `id` | `value` | Wave |
|---|---|---|---|---|
| SetParam, Freeze, Trigger | 0–2 | as today (`Engine.h:131-137`); SetParam stores only `Leaf` and `Global` rows | | now |
| SpilloverLoad | 3 | style: 0 Trails, 1 FastCut | — | 3a |
| MacroMove | 4 | macro ID (69–76) | position, canonical in [0, 1] | 3a |
| Expression | 5 | 0 | position in [0, 1] | 3a |
| Tap; Tempo; ClockTick | 6; 7; 8 | 0; µs per quarter; 0 (24 per quarter) | — | W2 |
| Transport; Subdivision; GlobalReverse | 9; 10; 11 | stop, start, continue; subdivision or time mode; 0 | —; —; nonzero = on | W2 |

Numbers are permanent once landed; W2's are reserved and the CLOCK pull request may change
their payloads first. Tempo fits `id` as an integer, so the event structs do not widen. **The
engine never reads `ProcessContext::tempoBpm`, `timelinePos` or `transportPlaying`**
(`Engine.h:172-175`): the wrapper turns host tempo and transport into events, because
per-block fields would make output depend on the block grid. Tap and MIDI-clock smoothing run
in `dsp/` in integer frame arithmetic, so logged taps replay identically.

New exported functions: `DecodePreset` and `ValidateMode` (integer-only, unguarded);
`EvalMacro(const ModeBlob&, ParamId, float position, PresetLeaf* out, size_t cap)` and
`EvalExpression` (guarded); `CheckPreset` also validates the mode. `LoadReport` gains
`invalidMode` and `unsupported`.

As built (lane C): events 4 and 5 as above; a `SpilloverLoad` event's `id` is its
`SwitchStyle` (0 Trails, 1 FastCut, anything else Trails), and `LoadPreset` takes the style as
an optional fourth argument. `kFastCutFrames` = 128 is in `Engine.h` beside
`kFeedbackDelayFrames`. `Engine::ModeSwitches()` (audio thread) counts the loads whose mode
differed by content. `LoadReport.unsupported` counts stored performance fields away from their
defaults; `applied` is false for an invalid mode.

### 7.5 Wave 1 and shared rules

**Trigger sources (R9).** Without `periodic` there are no free-running births, and the
one-voice floor (`dsp/src/Granular.cpp:400-402`) applies only with it. `onset` fires a burst
per onset, stealing the layer's oldest voice if needed; marks are always recorded.
`footswitch` and `midi_note` triggers fire only if listed, and the default lists both, as r1
behaves. A trigger births `burst.count` grains, one per frame when `spacing_ms` is 0 (as
same-frame triggers already do, `Granular.cpp:357-365`), else every
`max(1, round(spacing_ms · 48))` frames, each with its own frame's random key. A draw below
`intermittency` skips a periodic birth (which still consumes its interval) or a trigger.

**Pitch sets (R10).** `cycle` steps through entries in order, each `weight` times; `random`
picks entry *i* with probability `weight_i / Σ weight`, in integers from the draw's top 24
bits. The pitch is `clamp((entry.st + transpose_st) + detune, −24, 24)`, converted at birth
as today (`dsp/src/detail/GrainMath.h:55-60`). The blob stores semitones, not engine §5's
compiled ratios, which would put DetMath in the compiler and make detune multiply instead of
add, changing every pitched preset (record §3, item 4).

**Repeat and decay (R11).** A voice with `repeat` *N* reads the same ring region *N* times,
each pass windowed, for a life of *N*·*L*. Re-reading the same absolute frames is a snapshot,
since only the write head overwrites the ring, which answers engine §12 item 6 in principle;
recordings still judge the feel. The far rail covers the whole life (*calculated* as in
engine §3, with its symbols: *d* the read delay at birth, *L* one pass's length and *r* the
absolute rate, `bufLen` the ring, `W_a` the block write-ahead, `margin` the guard margin):
forward `d ≤ bufLen − W_a − (N − 1)·L − L·max(0, 1 − r) − margin`, reverse
`d ≤ bufLen − W_a − (N − 1)·L − L·(1 + r) − margin`. `decay_ms` is the time to fall 60 dB as
the position reference ages (mark age, pass start or pin age): gain
`2^(−age_ms · log2(1000) / decay_ms)` by DetMath `Exp2F`, at birth and each pass; 0 is off and
age 0 gives exactly 1, so r1's presets are unchanged.

**Voice count (R12).** At most `voice_count` voices of a layer sound: free-running births
beyond it are refused and triggers steal the oldest. The free-running target becomes
`min(64·overlap³, voice_count, grain length)`; 64 keeps r1's law (`Engine.cpp:621-628`).

**Random-number keys (R8).** Today's key is `abs·8 + purpose`, all 8 purposes used, folded
to 32 bits as `lo ^ hi` (`GrainMath.h:26-48`). A 6-bit `ext` packs the layer (1 bit), a
same-frame ordinal (3 bits) and `purpose >> 3` (2 bits). With `ext` = 0 the key is r1's,
`Fold(abs·8 + purpose)`, so every r1 key is unchanged; otherwise the frame is hashed before the
extension is mixed in: `k32 = Hash32(Hash32(Fold(abs·8)) ^ ((ext << 3) | (purpose & 7)))`. The
504 extended keys of a frame are pairwise distinct (one value XOR-ed with distinct 9-bit
values, then the bijection `Hash32`), and keys of two frames meet only by chance (2⁻²³ per
pair of frames), never at a fixed offset. Both simpler forms alias deterministically: XOR-ing
`ext << 56` before the fold aliases every layer-1 draw with the layer-0 draw 2²¹ frames away
(*measured*, record §2.8), and this draft's first formula, `Hash32(Fold(abs·8 + (purpose & 7))
^ Hash32(ext))`, leaves the frame linear under the XOR, so every draw of one `ext` equalled a
draw of another at frame `abs ^ (D >> 3)`, `D = Hash32(ext) ^ Hash32(ext')`, some within 2¹⁸
frames (*measured*, lane B review; amended when lane B was built). A purpose from 8 on goes
through the extension whichever overload keys it, since r1's fold would give it the next
frame's key. New purposes start at 8 (pitch select, intermittency, step shuffle, step
probability, mark walk, random cutoff). Re-keying everything would change every jittered
golden hash and blunt §7.6's check of the r2 re-mint.

**Normalization.** Engine §3's `N^(−p)` gains: the pitch term uses the largest |ratio − 1|
over the set; `repeat` > 1 counts as decorrelated, like marks (`Engine.cpp:659-665`); without
a free-running source *N* = `min(voice_count, burst.count)`; layers normalize separately.
Each feature runs contract #3's level test over its cases.

### 7.6 The sound-revision plan

Profile §5.12 governs: any change that *can* alter output bumps `kSoundRevision` by one and
re-mints `golden.json`, with code-owner review. Companion §6.1's "a new leaf … can land as
sound-neutral" is withdrawn, because a new leaf gives new presets new sound; what a neutral
default buys is that older packages stay exact (§7.3). The pull requests, in order:

1. **ID table (lane 0), no bump.** R2a and every `kNumParams` consumer (§12.4). Rows 27 and
   28 stay `Leaf` and `ApplyParam` is unchanged for IDs 1–28, so no engine path reads anything
   new: the "sound-neutral" label rests on that construction, and r1's golden hashes
   reproducing is a check, not the justification (profile §5.12).
2. **Blob (lane B), no bump**, by the same construction: R4, and R8, whose `ext` = 0 path is
   r1's `DrawKey`.
3. **Compiler (lane A)**, outside the trigger paths. This build's support table admits only
   the default structure, so `onset` or `mark` is E6 and `DecodePreset` rejects any other MODE
   as `UnsupportedFeature`: no package can load as exact before R5, and none is committed.
4. **Runtime, r2 (lane C)**, after lane G's package rule (§8.3): R1, R2b, R3, R5–R7, support
   for `onset` and `mark`, the corpus converted (§4.4) with packages compiled by `bspc`. Its
   description explains each changed golden hash: expected are the automation preset (it edits
   27 and 28), presets with a nonzero trim below full mix, and `post_max`, moved from 40 Hz to
   41 Hz so its post-stage ablations still hear something under the kill (§10.3). R8 keeps
   r1's keys, so every other hash must reproduce; an unexplained change blocks the merge.
5. **The Mix law, r3**, if approved (Q13).
6. **One bump per W1 feature**, then W2 and W3, each re-minting and re-stamping the factory
   packages with `bspc stamp`.

Bumps are cheap until a revision is published (companion §8.1); the last internal revision
before step 6 becomes the first published one.

As built (lane C, item 4): of revision 1's 28 golden presets, 25 reproduce their hashes bit for
bit at r2, every converted package preset included, and exactly the three item 4 expects
change: `automation_offgrid` (its toggles of 27 and 28 became mode switches; it also moves the
trim below full mix), `subnormal_wet` (trim −6 dB at mix 0.5) and `post_max` (40 Hz to 41 Hz,
from second 2). `hot_out` (trim +24 dB at mix 1) reproduces, as the arithmetic says it must.

## 8. The compiler library and `bspc`

### 8.1 Where it lives

```
compiler/          library brainscape_compiler, namespace bsc, desktop only: Number, Json,
                   Schema (one table), Compile, Decompile, Format, Migrate, Pack, Lint, Derive
tools/bspc/        the command-line tool
tools/audition/    render, event scripts and metrics for bspc and the app; no JUCE
dsp/src/blob/      DecodePreset, ValidateMode, SHA-256: the only part the firmware links
```

Companion §3.1 put the compiler under `dsp/src/compiler`, where every compiler pull request
would need the "sound-neutral" label (`tools/ci/sound_rev_gate.py:29-34`), although a
compiler change alters package bytes, not the engine's output for a package, which is what
`kSoundRevision` certifies (profile §5.12). At the top level it is gated by its committed
outputs and the package rule (§8.3), with CODEOWNERS entries for `/compiler/`, `/tools/bspc/`
and `/firmware/factory/`. Namespace `bsc` keeps compiler functions out of `brainscape::`;
companion §3.4's `symbol-scan` rule is scoped to functions, because the `inline constexpr`
`kParamTable` is COMDAT constant data in any object that indexes it at run time, as
`plugin/src/StateCodec.cpp:85` already does (*measured*, record §2.8), and constant data
carries no floating-point flags. Linking `brainscape_dsp` (descriptors, `Canonicalize`,
`kSoundRevision`, decoder, validator, `EvalMacro`) puts it under the configure-time flag
check. It is built for hosts only and linked by `bspc`, the app and the plugin's text parser.

### 8.2 Pipeline and commands

`Compile`: (1) parse, keeping number text (E1); (2) check keys, types and version, migrate
by name (an empty table in schema 1), fill defaults (E2, E3, E5); (3) read floats through
`Number`, canonicalize, range-check (E4); (4) check support, caps and references (E6–E11);
(5) build the decoded `PresetState` and META; (6) encode field by field, the JSON section being
the formatted, stamped document; (7) hash; (8) decode and validate the result and require the
same state. All of it uses integers only; a prototype of steps 5–8 gave identical bytes on
MSVC 19.40, GCC 11.4, GCC 14.2 and Clang 14 (*measured*, record §2.5). Lint and `derive` are
separate passes over a compiled state that call guarded `dsp/` functions (`EvalMacro`), which
are deterministic across hosts (profile §3.9).

| `bspc` command | Does |
|---|---|
| `compile`, `decompile` | §8.2; prints the JSON section, or rebuilds JSON from STAT, MODE, CTRL and META if it is absent or stale (editor data is then lost) |
| `fmt [--check]` | canonical form, or a list of files that differ |
| `verify`, `stamp` | hashes, structure, and that the JSON section compiles to the same STAT and MODE; recompile and rewrite `sound_rev` and `sound_hash` after a bump |
| `lint [--factory]`, `diff` | L1–L9 (§2.7); the first differing field of two packages, by name |
| `derive [--solve]` | rewrite each targeted leaf as `EvalMacro` at its position, or each position from its leaf (§3.5) |
| `render [--metrics]` | the shared render (companion §4.9) with an input and an event script; WAV, recipe, hashes, §11.3's metrics |
| `migrate-session` | a `BSWS` v1 session to a preset document (§4.4) |

As built (lane A): `bspc roundtrip` runs §8.3's checks with a sorted hash manifest
(`--expect`, which names each document whose hashes differ), and `bspc version` prints the
build's constants; `render` arrives with lane E. Each command takes only its own options:
anything else exits 2 before a file is touched, so a misspelled `--check` or `--factory` in CI
fails instead of passing or rewriting. `diff` compares what plays and controls the sound
before the id, name, META and display names, and the header flags (FACTORY follows from the
id) after them. On Windows `bspc` runs with UTF-8 as its code page, so any file name opens.
Later waves' vocabulary that this document leaves open is provisional: tempo divisions are
`"off"` only until W2 defines them, `quantize.scale` lists pitch classes, route sources are
`modulator0`/`modulator1`, link endpoints `grain.pitch`-style names (engine §5), a step's
`gain` and `prob` default to 1, and `modulatorN.x` leaves live at `modulators[N].x`.

### 8.3 Determinism and its gate

Compiling is a pure function of the JSON and the build's constants (companion §6.6): no
clock, user, locale, hash-map order, pointer-ordered sort or floating-point arithmetic.

- **`bspc-roundtrip`** on every host leg (Linux GCC and Clang, Linux arm64, MSVC SSE2 and
  AVX2, both macOS legs): every factory and corpus document compiles to its committed `.bsp`
  byte for byte and passes `fmt --check`; each leg uploads a sorted manifest of package hashes,
  and `parity-summary` requires them identical.
- **The package rule.** A compiler change alters what a document means, not what the engine
  does with a package, so it needs visibility, not a bump. `golden.json` records each package
  preset's `soundHash` and `controlHash`, and `firmware/factory/MANIFEST` each factory
  package's. `sound_rev_gate.py`, which today reads only `hash` and `secondHashes`
  (`:71-92`), treats a changed audio hash as an engine change, needing a bump, only when the
  preset's package hashes are unchanged; a changed package hash needs a CODEOWNERS-approved
  `package-change` label naming the cause. `test_sound_rev_gate.py` covers both.
- **The compiler audit**, on the `brainscape_compiler` target: a source check over `compiler/`
  bans `<charconv>`, `<cmath>`, `<math.h>`, the `strto*`, `ato*` and `sto*` families,
  `std::to_string`, `printf` and `scanf` families and stream number formatting; the GCC and
  Clang legs also reject imports of libm, `strtof` and `strtod`. On MSVC the float conversions
  are header-only and import nothing that names them (*measured*, record §2.8).

An optional leg runs `bspc` on the emulated M7, for later on-pedal compilation.

As built (lane G): `bspc-roundtrip` is a job in `.github/workflows/parity.yml` on
`parity-host`'s seven legs. It runs the compiler's tests (`ctest -R '^compiler_'`: the number
sets of §10.2, the JSON grammar, the property and reader-fuzz digests, `bspc roundtrip` over the
examples and `bspc`'s command line), then `tools/ci/bspc_roundtrip.py` over every document set
that exists: `compiler/tests/data`, `dsp/tests/golden/presets` (lane C; `frozen/` exempt) and
`firmware/factory` (lane E), the last two with a committed `.bsp` beside each `.json` and a
`.json` beside each `.bsp`. Every set with documents must commit its `MANIFEST` (`bspc roundtrip
--write-manifest`, paths relative to the set) and is checked against it, so no package changes
without its manifest line changing; and the root `.gitattributes` must give every document and
`MANIFEST` `text eol=lf` and every `.bsp` `binary`, which the script checks with `git
check-attr` on every leg (canonical JSON is LF, §6.4, and Git for Windows checks text out as
CRLF under `core.autocrlf`, so without the rule the Windows legs would reject every document). A
self-test step first proves each of those checks can fail and that the committed rules cover all
three sets; CODEOWNERS covers `/.gitattributes` too. Each leg uploads the combined manifest,
paths prefixed with their set, and `parity-summary` requires all seven byte-identical. The
package rule reads `dsp/tests/golden/presets/MANIFEST` and `firmware/factory/MANIFEST` in `bspc
roundtrip --write-manifest`'s format, and `soundHash` and `controlHash` (64 hex digits, as
`bspc` prints them) from each preset entry of `golden.json` that has them, which lane C's
harness writes and its check mode must verify against the package the preset loads; the
manifests catch a package change that `golden.json`'s entry misses (a CTRL-only change, or a
STAT change the render does not hear), and `golden.json`'s entries tie a preset's render to its
package. A changed render counts as its package's only when the pull request touches none of the
path trigger's paths without a bump: with the engine changed beside the package, nothing tells
which moved the render, so it is the hard trigger's, whatever the labels (land the two apart, or
bump). "Naming the cause" is a `Package-change: <cause>` line in the pull request's description,
which the gate requires beside the label and prints, re-running when the description is edited.
A dropped package or a package preset turned back into a parameter list counts as a change; a
new package, a preset that gains one and a re-stamp (`sound_rev` and package hash only) do not;
neither a bump nor "sound-neutral" waives the label. The compiler audit is
`tools/ci/audit_compiler.py`: the source ban runs over `compiler/src` in `parity-audits`, after
a self-test of cases it must and must not flag (comments and string literals are skipped); it
also bans the stream and locale headers and the other float formatters (`ecvt`, `gcvt`,
`strfrom*`). The import check runs on the GCC and Clang `bspc-roundtrip` legs and rejects the
same families as imports (libm, the `strto`/`wcsto`/`ato` families, `printf` and `scanf`, the
float formatters, and `to_chars`, `from_chars`, `to_string` or string streams), after proving on
`tools/ci/compiler_audit_selftest.cpp`'s object that it catches `strtof`, `strtod` and libm. The
optional M7 leg is not built.

## 9. App integration

### 9.1 Library, editor and views

- **Library** (companion §2.5): banks **Factory** (read-only, identical to the firmware's),
  **User** (a folder of `.bsp` files, with Import and Export JSON) and, from phase E, the
  pedal's slots. Duplicating a factory preset gives a user preset with a new `id`.
- **Curation slice**, built first because step 4's rated pass needs it (§11.3): watch a
  preset document on disk and, on save, recompile it and Spillover-load it with Trails,
  re-sending the current knob positions, while a DI clip loops through the existing FileLoop
  input (`plugin/src/gui/Widgets.cpp`); eight macro sliders plus Shift; a one-key,
  level-matched A/B between the last kept package and the working one; a "capture endpoint"
  button storing the current raw leaves as a target's `lo` or `hi`; a rating form appending
  the `AUDITION.md` row.
- **Editor**: a form generated from the schema table, a macro editor drawing each curve with
  `EvalMacro`, derived leaves and "solve position" (§3.5), §2.7's findings inline, and a JSON
  tab. Saving compiles on a worker thread; a document that does not compile is saved as JSON
  only.
- **Pedal view**: the eight knobs with the mode's display names and a Shift toggle; Loop Level
  disabled until the looper exists. The test GUI's raw parameters stay as an **Advanced** tab.
- **Audition**: a package, an input and an event script (§11.3); the render moves to
  `tools/audition/` and takes a `PresetState` instead of 28 floats (`plugin/src/Audition.h:45`).

### 9.2 Load modes, parameters and session state

- **Loads**: Spillover with Trails while audio runs, Exact when nothing has played since the
  last `Init` or restart (companion §6.9); "Load Exact" is a menu command, and audition and
  PARITY always load Exact. The processor's presets become `PresetState`s
  (`plugin/src/PluginProcessor.h:116-121`); a staged Spillover preset lives until it retires.
- **Parameters**: one `BrainscapeParam` per `Leaf`, `Macro` and `Performance` row and for the
  effect volume, keyed on the stable name as today (`plugin/src/BrainscapeParam.cpp:244-246`), automatable as
  the host model says (§3.6); a host move of a macro parameter becomes a `MacroMove`.
  `Reserved` rows and rows retired before release are not registered.
- **Device settings** (§3.8) live in the wrapper's settings and are written into every recipe.
- **Session state** (`BSWS` v2): a wrapper header, the current `.bsp` and the wrapper settings
  (companion §6.9). A host set that is not an exact echo rewrites STAT, patches the JSON and
  recomputes `sound_hash` before saving; v1 states are migrated (§4.4).

## 10. Tests and CI

### 10.1 The round-trip contract

Engine §10 contract #6 becomes, for every factory and corpus document *J*:
`Decompile(Compile(J))` equals `Fmt(J)` and compiles back to the same bytes; `Fmt` is
idempotent; without the JSON section, decompiling gives `Fmt(J)` minus `editor` data. A
property test requires encode, decode, encode to give the same STAT, MODE and CTRL bytes for
random valid states, and `bspc-roundtrip` gives cross-leg identity (§8.3).

### 10.2 Numbers, JSON and fuzzing

- **Numbers**, every leg, per pull request: edge cases (§6.4's two exceptions and the
  lenient typed forms included), halfway strings at stride 1009, 20 million random strings and
  a strided round trip, each against a committed hash over fixed index chunks combined in
  order, so no hash depends on the thread count as the prototype's random-string hash did
  (record §2.8); the set runs once on linux-arm64 and macOS before its hashes are committed.
  About 8 s in Release, 92 s in Debug with sanitizers, on 4 CPUs (*measured*). Nightly, on one
  x86-64 and one arm64 leg: the exhaustive round trip, about 3.6 CPU-hours (*estimated*).
- **JSON**: a grammar suite (escapes, surrogate pairs, invalid UTF-8, duplicate keys, depth)
  and a reader fuzz whose accepted outputs compile to fixed points of `Decompile` then
  `Compile`.
- **Decoder and validator**, CTRL and META included: libFuzzer with sanitizers on the Linux
  Clang leg (60–120 s per pull request, nightly with a kept corpus), and a deterministic
  in-repository mutation fuzzer on every leg including the emulated M7, whose 32-bit `size_t`
  the host legs miss; host and M7 must accept and reject the same inputs, and every accepted
  input must re-encode to the same STAT, MODE and CTRL bytes with its carried sections intact.

As built (lane G): the per-pull-request number sets run on the seven `bspc-roundtrip` legs; the
nightly workflow (`.github/workflows/nightly.yml`) runs the exhaustive round trip on
linux-x64-gcc and linux-arm64-gcc without the standard library cross-checks (the committed hash
is the in-house code's; the per-pull-request sets keep the cross-checks), and libFuzzer for 30
minutes from a corpus kept between nights in the Actions cache and minimized after each run.
`blob-libfuzzer` runs 90 s per pull request with AddressSanitizer and UndefinedBehaviorSanitizer
at `-O1` with asserts live, seeded with the frozen fixtures. Every `parity-host` leg and
`parity-m7` run `brainscape_blob_tool --fuzz` against its committed verdict digest and
`--fixtures`.

### 10.3 The golden corpus with modes

- `PresetCase` (`dsp/tests/golden/Corpus.h:80-87`) gains an optional committed package
  (JSON and `.bsp` under `dsp/tests/golden/presets/`), loaded through `DecodePreset` by the
  host and M7 harnesses, which tests the decoder on the M7; each JSON must recompile to its
  committed bytes. Parameter-list presets remain; any structure they use comes from encoding
  and decoding a document (§4.4).
- **Frozen fixtures** under `dsp/tests/golden/presets/frozen/`, exempt from `bspc-roundtrip`
  and never re-stamped: an r2 package that must load with `exact` and no missing IDs on every
  later build (the `sinceRev` rule); a package with an unknown MODE chunk or feature bit that
  must be rejected as `UnsupportedFeature`; a package with an unknown section after MODE that
  must decode and keep it byte for byte.
- New script events (`MacroMove`, `Expression`, mode-changing Spillover loads in both styles)
  off the 48-frame grid; new presets (a full macro sweep, expression on a macro and a leaf, an
  onset-only mode, both pitch-set selections, `repeat` near the far rail, `decay_ms` on marks,
  one lone change per `Leaf` row against the same change among other edits, and the **wet
  kill**: into and out of 40 Hz by `SetParam` and `MacroMove`, a lone trim change while
  killed, an ablation at 41 Hz); new counters (macro moves, expression events, mode switches,
  bursts, skips, repeat passes, killed frames); new ablations (`mode`, `macro`, `modeSwitch`).
  `post_max` moves to 41 Hz (§7.6); `kCorpusVersion` is bumped.
- Every factory preset joins once step 4 keeps it (profile §6.1): 14 presets of 12 s add about
  21 s per pass on the emulated M7 (*estimated*), 44 about 66 s.

Each W1 feature adds unit tests and contracts #1 and #3 over its cases.

As built (lane C): `PresetCase` names its package (`package`); a script stages packages too
(`SpilloverPackage`, `ExactLoadPackage`), with a switch style per load. The documents are in
`dsp/tests/golden/presets/` with their packages and `MANIFEST`, compiled by `bspc` at r2: ten
for the r1 presets that used 27 or 28 at their start or in a load (each holding exactly the r1
values), the automation preset's second mode, and six for the new presets. The harness decodes
each package on the host and the M7 alike and records `package`, `soundHash` and `controlHash`
per package preset in `golden.json`, which check mode compares. Corpus version 6 adds the vector
`plucks_modes_14s` with `macro_sweep` (114 macro moves over all eight macros, the Filter macro
through the kill, a `SetParam` and a `MacroMove` on one leaf at one frame in both orders),
`expression` (300 pedal moves over four assignments, two on macros), `mode_switch` (six
Spillover loads among three modes, Trails and FastCut, two FastCuts 77 frames apart, one load of
the same mode), `wet_kill` (into and out of 40 Hz by `SetParam` and the Filter macro, lone trim
and effect-volume changes while killed) and `lone_changes` (every `Leaf` row and the effect
volume changed alone); the counters `macroMoves`, `expressionEvents`, `modeSwitches` (the
engine's, after the render's first load) and `killedFrames`; the ablations `mode` (the default
mode and CTRL), `macro` (moves dropped), `modeSwitch` (every load keeps the starting mode),
`fastCut` (FastCut loads made Trails) and `wetKill` (40 Hz moved to 41 Hz), with `markPosition`
and `onsetTrigger` now switching structure off; and the invariance `amongEdits`, which renders
every `SetParam` of a script among edits that rebuild every other domain and must give the same
bits (R1 on every leg, the M7 included). Every render starts from the device settings' defaults
(part of a render's recipe, §3.8). An ablation that switches structure off edits the decoded
`ModeBlob`; the presets themselves take it only from packages. A staged package's
hashes are not in `golden.json`: a change to one is visible in `MANIFEST`, and its render's
change counts as the engine's, a conservative attribution.

### 10.4 Engine and plugin tests

- **Per-kind rules**: STAT containing IDs 27, 31, 69 or 81 loads inexact and changes no
  state; `SetParam(69)` does nothing; an Exact or Spillover load keeps
  `global.trigger_offset` and `global.effect_volume_db`; a `PresetState` with `soundRev` 0 and
  no leaves is inexact; an invalid `ModeBlob` is not applied; a Spillover load of a different
  mode carrying a stale `modeHash` resets sequencing.
- **Defaults and keys**: the default `ModeBlob`'s hash (§5.1); no key aliasing at ±2²¹ and
  ±2²⁵ frames, between two extended streams at their XOR partners, or between a purpose from 8
  on and the next frame's key (§7.5).
- **Plugin**: typed text `.5 s`, `5. ms`, `05` and `+25`; a per-leaf touched set replacing the
  32-bit mask (`plugin/src/PluginProcessor.cpp:24`); the release checklist's host recording
  test (§3.6).

As built (lane C): `dsp/tests/test_modes.cpp` holds the load, event, wet-gain and FastCut cases,
`test_params.cpp` the per-kind rules (27 and 28 among the unknown ids) and one lone change per
`Leaf` row and the effect volume, `test_mode_eval.cpp` `EvalExpression`, and the frozen fixture
`r2-onset-marks.bsp` the `sinceRev` rule. "A different mode with a stale `modeHash` resets
sequencing" is tested as the content comparison (`ModeSwitches`, and the stale-hash load
playing as the true one): r2 has no sequencing state, so the reset itself is W1's test. Unit
tests whose parameter lists named 27 or 28 still do, read as structure (`RetiredRows.h`).

## 11. Step 4: the first factory modes

### 11.1 The first set

Of the Microcosm's 44 variations, 3 can be built faithfully today (Haze A, Haze B, Warp C),
Pattern A once its repeats are exact, 21 approximately and 20 not yet (record §2.2). These
fourteen exercise every engine path that exists, from the bit-exact unity-rate read and
feedback above unity to onset marks and every post stage, and cover all four families.
Wet−dry levels are *measured* on a plucked phrase, a strum and a soft pad from static recipes
at r1, wet at Mix 1; record §2.2 holds every recipe and macro map. Every recipe is re-measured
under the chosen Mix law (Q13) and the pre-screen before it is rated.

| Mode | Family | Microcosm analog | Character | Wet−dry dB | Needs |
|---|---|---|---|---|---|
| Engram | echoic | Pattern A | clean delay on the post delay's exact taps; Activity smears what enters it | not yet measured (re-authored) | now |
| Callback | echoic | Pattern B–D, approx. | grain tap plus exact post-delay taps | +0.2 / +0.9 / −0.7 | now |
| Retrograde | echoic | the FWD/REV button as a mode | reverse delay in crossfaded chunks | −2.6 / −2.0 / −2.4 | now |
| Updraft | echoic | Warp C | octave-climbing cascade | −1.8 / −2.1 / −1.4 | now |
| Pinhole | echoic | Warp B, approx. | resonant band-pass taps | +2.9 / +1.7 / −10.9 | now |
| Murmuration | reverie | Haze B | dense randomized cloud | −0.1 / −0.6 / −1.7 | now |
| Halation | reverie | Haze C | octave-up cloud | −0.4 / −0.9 / −0.2 | W1 set {0, +12} |
| Undertow | reverie | Haze D | sub-octave cloud | −0.6 / −0.1 / −1.3 | W1 set {0, −12} |
| Lull | reverie | Tunnel, static | drone; Repeats = decay | −0.9 / −0.4 / −1.1 | now |
| Echolalia | reverie | Strum A | repeats the newest note | −2.9 / +3.7 / −8.3 | W1 `decay_ms` |
| Déjà Vu | reverie | Strum B | phasing copies of the newest onset | −2.5 / +3.3 / −5.6 | W1 `decay_ms`, burst |
| Kaleido | recall | Mosaic C, approx. | onset-anchored octave-up loops | −2.5 / +3.1 / −2.5 | W1 `repeat`, set |
| Refrain | recall | Mosaic A | free-running loops at pitch and octave up; Activity sets the voices | not yet measured | W1 set {0, +12}, `repeat`, `voice_count` |
| Shards | misfire | Blocks B | onset-only reversed octave shards | −3.0 / +1.2 / −3.6 (r1, onset OR'd) | W1 sources |

Reserves: Afterimage (Haze A), Runaway (bounded self-oscillating shimmer), Vesper (Tunnel C),
Lethe (Tunnel B, static), Downdraft (Updraft at −12) and a Mosaic B analog ({−12, 0}). Hologram
names appear only as references in design and research documents, never in product strings
or in `firmware/factory/` (§11.2).

The probe also found (record §2.2): grain-feedback repeats land `base_ms` + 10.67 ms apart
(the FIFO), so a repeat runs 43 ms behind a 375 ms grid by the fourth, while post-delay taps
are exact; Engram therefore runs its repeats on the post delay, and Q11 asks how W2 makes
grain feedback tempo-exact. Onset modes fall back to `base_ms` on soft pads; Pinhole's level
depends on the source; and Time sweeps on grain delays splice (STATUS.md), so a mode may route
Time to `post.delay.time_ms`, which glides (profile §5.6).

### 11.2 Naming

One theme, memory and perception; families Recall (micro-loops), Reverie (granular), Misfire
(glitch) and Echoic (multi-delay). Product strings avoid Hologram's marks and effect names and
other makers' product names (record §2.2 lists them); `bspc lint --factory` enforces the list
as L9. No trademark search has been done, and the recall-based list already missed two names;
the owner clears mode, family and knob display names in one pass (Q3).

### 11.3 The audition protocol

**Inputs**: the test-signal vectors Plucks, Strums, SoftNotes (few onsets), OnsetBursts,
Saturation and Silence (`dsp/include/brainscape/TestSignal.h:111-118`), each with a 10 s
silent tail; and owner-recorded DI clips in 48 kHz integer PCM (notes, chords, palm mutes,
fuzz, bass, a pad), which render identically across machines (companion §4.9; Q9). Each mode
declares its input class: attack modes are judged on Plucks, Strums and DI, pad modes on
SoftNotes and the DI pad.

**Scripts**: S0 stored positions; S1–S6 each macro moved from its stored position to 0, to 1
and back over 16 s, one move per 48-frame block, with the input looped for the whole script;
S7 Repeats at maximum, then silence; S8 freeze; S9 footswitch triggers; S10 a Spillover load
from the previous mode; S11 combinations: each mode at every other factory mode's stored
positions (14 × 13) and at the 16 corners of Activity × Repeats × Shape × Time.

**Objective pre-screen** (`bspc render --metrics`, before every listening pass). Levels are
K-weighted loudness (ITU-R BS.1770's filters, computed only in the desktop metrics code) on
the mode's declared input class:

| Check | Pass |
|---|---|
| Peak | ≤ −1 dBFS at stored positions; ≤ 0 dBFS during sweeps and S11 |
| Level | wet at Mix 1 within ±2 LU of dry at stored positions, set with `wet_trim_db` |
| Engaged | output at stored positions no more than 1 LU below bypass and no more than 4 LU above it |
| Sweeps | Activity, Shape and Time within ±3 LU; Repeats: level and tail non-decreasing, at most +10 LU at maximum |
| Tail | time to −70 dBFS after the input stops: finite unless the mode is declared self-oscillating |
| Response | Activity changes event density or voice count monotonically; Shape changes envelope or spectrum measurably |
| Fallback | onset modes on SoftNotes within 12 dB of their Plucks level, or documented as needing attacks |
| Clicks | no sample step in a sweep above 4 times the largest step of the static render |
| Combinations | S11 holds Peak, Tail and Clicks; the worst cases are listed for listening |
| Load, determinism | voices and births per second logged; two renders and two block patterns give one hash |

Feedback *g* gains a sustained input 1/(1 − g²) in energy, +7.2 dB at 0.9 (*calculated*), so
Repeats may build; "Engaged" lets the wet add level but not take it away (record §2.8).

**Listening pass**, through lane D's curation slice (§9.1). The owner listens on monitors
and through a guitar amp, plays the standalone live (outside the parity contract), and fills
one row per mode in `firmware/factory/AUDITION.md`, keyed by preset `id` and recording
`sound_rev`, `sound_hash` and the S0–S11 render hashes: one chord sounds finished (yes or
no); each knob musical across its range (1–5); level against bypass; keep, revise or drop;
notes. After each bump or re-stamp a `tools/audition` script re-renders S0–S11: a row whose
render hashes held carries forward; any other is marked "re-listen" with its first differing
second. Revisions pass the pre-screen again. **Exit**: at least 10 modes "keep", at least one
per family, every knob rated 3 or more, no objective failure; they then join the golden
corpus. Echoic modes' Time ratings stay provisional until W2. Static recipes can be heard now
to tune character, but no rating counts before macros exist, because the knobs are the
product (engine §1).

### 11.4 What waits

| Microcosm family | Waits for |
|---|---|
| Mosaic B, D | W1 approximates (B in reserve); tempo-locked loops need W2 |
| Seq, Arp, Pattern B–D | W2 CLOCK and steps; W3 SVF, layer 2, crush |
| Glide, Warp A and D, Tunnel A, B and D | W3 glide, routes, layer 2, `POS_PIN` |
| Strum C, D | W2 mark walk; W1 sets |
| Blocks A, C, D, Interrupt | W1 sources and burst; W2 steps; W3 crush and `dry_duck` |

The rhythmic families form step 4's second set.

## 12. Decisions, risks, open questions, plan

### 12.1 Decisions taken

| Decision | Choice | § |
|---|---|---|
| Scope | pipeline and W1 in step 3; W2 during step 4; W3 as modes need it | 1.2 |
| Document | one per preset, mode embedded; leaf names are JSON paths; unsupported fields are errors | 1.4, 2 |
| Macros | six per-mode (Space, Filter with defaults and universal endpoints), two auxiliary; one evaluator; moves are events; targeted leaves derived | 3 |
| IDs | keep 1–28, rename 4 and 8, retire 27 and 28, add 29–82 with per-kind rules and domain bitmasks; power tapers; version hints | 4 |
| Output | wet-only trim as level match; effect volume a device setting; one wet gain function with the cutoff kill; sensitivity plus global offset | 3.8, 7.2 |
| Blob and loading | semitones; chunked MODE in canonical form; `sinceRev`; validation at load; one active mode; Trails or FastCut | 5–7 |
| Keys and tempo | r1's random keys kept, extended through `Hash32`; tempo only as events | 7.4, 7.5 |
| Compiler | `compiler/`, namespace `bsc`, integer-only, no new dependency; the package rule | 6.5, 8 |
| Sound revision | profile §5.12 over companion §6.1; neutral pull requests justified by construction | 7.6 |

Record §3 keeps the 29 disagreements behind these, and record §6 the review findings.

### 12.2 Risks

1. **Feel**, the central risk (engine §12 item 1). Mitigation: §11.3, the curation slice; W1
   before CLOCK.
2. **Schema churn** before step 6, since the vocabulary precedes W2 and W3. Mitigation:
   principle 5; factory documents re-formatted in the same pull request.
3. **Revision churn**: about 8–12 internal bumps (*estimated*). Mitigation: `bspc stamp`,
   the mint script and rating carry-forward by render hash.
4. **`Grain` and `Engine::Impl` budgets** under W3. Mitigation: the `static_assert`s; pack or
   move cold state to the Warm arena; the DTCM map audit each wave.
5. **The owner's listening time** bounds step 4. Mitigation: the curation slice and the
   objective pre-screen.
6. **Hosts** record what plugins report. Mitigation: the host model (Q12) and the recording
   test.
7. **Hostile uploads** reach the firmware decoder. Mitigation: §5.3 over every section;
   fuzzing on the M7.
8. **Trademarks.** Mitigation: Q3 before anything ships; L9.

### 12.3 Open questions

1. **(owner)** Approve the in-house number code and not vendoring `fast_float` (§6.5).
2. **(owner)** W1 before CLOCK (recommended) or CLOCK first, as companion §8.1 has it (§1.2).
3. **(owner)** Clear the factory, family and knob display names (§11.2).
4. **(owner)** `wet_trim_db` wet only with the cutoff wet kill and Shift+Mix as a separate
   effect volume device setting (recommended), or a whole-output trim (§3.8, §4.2).
5. **(owner)** Sensitivity as a preset leaf plus a global offset (recommended), or a global
   setting only, as companion §6.2 implies.
6. **(owner)** Reserve `perf.loop_level` now (recommended) or with the looper (companion Q29).
7. In Tempo mode, does the Time knob still reach `macro.time`? Decided in the CLOCK pull request.
8. **(owner)** After a load: pickup (recommended, engine §6) or "knobs follow" (§3.5).
9. **(owner)** DI clips committed as integer FLAC (about 10 MB, *estimated*) or kept outside
   git with recorded hashes (§11.3).
10. A base-delay glide, so Time sweeps on grain delays bend pitch instead of splicing? The
    listening pass decides.
11. Tempo-exact grain feedback (W2): compensate the 10.67 ms FIFO in the base delay, or route
    rhythmic repeats through the post delay, as Engram does?
12. **(owner)** The host model (§3.6): (b) only macros, Mix, effect volume and performance
    rows automatable (recommended), or (a) leaves automatable with unreported fan-out; with
    companion Q17's numeric-ID policy, before §4.5's gate.
13. **(owner)** The Mix law (§7.1): dry at unity to the middle and wet at unity from it
    (recommended, its own bump after r2), or today's linear crossfade.

### 12.4 Implementation plan

Lanes own the files listed; where two lanes touch one file, the earlier pull request lands
first and the later lane rebases. The files are taken from a search of what each change
touches.

| Lane | Work | Files | Starts | Days (*estimated*) |
|---|---|---|---|---|
| **0** ID table | R2a; every `kNumParams` consumer iterates `Leaf` rows; the plugin's 32-bit touched mask becomes a per-leaf set; dsp tests' IDs | `dsp/include/brainscape/{Params,ParamDisplay}.h`, `dsp/src/{Engine,ParamDisplay}.cpp`, `dsp/tests/*.cpp`, `dsp/tests/golden/{Corpus,EventScript}.cpp`, `dsp/tests/render_main.cpp`, `plugin/src/{PluginProcessor,StateCodec}.*`, `plugin/src/{PluginEditor,Audition}.cpp`, `plugin/tests/` | first | 3–5 |
| **B** blob | R4 with the default `ModeBlob`, CTRL and META rules; SHA-256 split; R8; decoder tests, mutation fuzzer | `dsp/include/brainscape/{Mode,Preset,PresetState,Sha256}.h`, `dsp/src/blob/`, `dsp/src/detail/GrainMath.h`, `dsp/tests/test_blob.cpp`, `dsp/tests/golden/{Sha256.h,Render.cpp}`, `dsp/tests/test_state.cpp`, `plugin/tests/plugin_tests.cpp`, `plugin/CMakeLists.txt` | after 0 | 5–7 |
| **A** compiler | `Number` (with the lenient entry), `Json`, schema table, compile, decompile, format, lint, derive; `bspc` | `compiler/`, `tools/bspc/`, `plugin/src/BrainscapeParam.cpp` (text parser) | after 0; B's headers | 9–14 |
| **G** CI | package rule and its tests; `bspc-roundtrip`, fuzz jobs, number tests and nightly legs, compiler audit, M7 decoder leg, CODEOWNERS; probes into `tools/parity/modes/`; the version-hint check before step 6 | `.github/`, `tools/ci/`, `CODEOWNERS`, `tools/parity/modes/` | after A; package rule before C | 3–5 |
| **C** runtime | R1, R2b, R3, R5–R7; R3b after approval; corpus conversion, frozen fixtures, wet-kill cases; r2 | `dsp/include/brainscape/{Params,Engine,PresetState,SoundRevision}.h`, `dsp/src/{Engine,Granular}.cpp`, `dsp/src/detail/Granular.h`, `dsp/tests/`, `dsp/tests/golden/` | after B; merges after A and G's rule | 7–10 |
| **E** audition | first, the JUCE-free render out of `plugin/src/Audition.*`; scripts S0–S11, metrics, carry-forward; factory documents, `MANIFEST`, `AUDITION.md` | `tools/audition/`, `plugin/src/Audition.*` (extraction), `firmware/factory/` | after 0; scripts after C | 4–7 plus curation |
| **D** app | curation slice first (§9.1); then parameters by kind and host model, `PresetState` plumbing, session v2, views | `plugin/` (rebased onto 0 and E's extraction) | slice after A and C; rest after B | slice 6–9; rest 12–25; GUI unsized |
| **F** wave 1 | R9–R12, a pull request and bump each | `Granular.*`, `GrainMath.h`, `Engine.cpp`, `Params.h`, `ParamDisplay.cpp`, the compiler's support table, `DecodePreset`'s feature set, `dsp/tests/golden/`, `firmware/factory/` | after C | 8–14 |

Pull requests in order: lane 0; B; A; G's package rule; C (r2); the Mix law (r3, if
approved); then F's features one by one while D and E continue, F serializing with A, B and E
on the files it shares with them; D's curation slice; step 4's first rated pass; W2; step 4's
second set. Total about 57–96 engineer-days (*estimated*), plus curation and the rest of the
GUI. Companion §8.1 keeps its six steps.

### 12.5 Amendments this design requires

Made when this document is accepted, not in this change: **grain-engine.md** §2 (the Mix law
if approved), §5 (semitones in the blob; one active mode instead of the ring and
`PublishMode`; validation without a sample rate; `d_min_fb` and the removed example fields),
§6 (§4.2's leaves; Space and Filter per-mode with universal endpoints; Filter endpoints as leaf
semantics; its "§2.6" references, which mean §2 commitment 6), §7 (no ring), §9 (§7.4's API),
§12 items 6–7; **companion-app.md** §3.1 (compiler location), §3.4 (`bspc-roundtrip` on every
leg, compiler audit, `symbol-scan` scoped to functions), §5 (host model, device settings),
§5.4 (power tapers), §5.7 (gate, version hints), §6.1 (sound-neutral leaves, missing leaves),
§6.3–§6.5 (header, chunks, in-house numbers, the writer's exception), §8.1 step 3 (waves);
**determinism-profile.md** §5.10 (validation at load, per-kind rules, `sinceRev`), §5.11 (mode
switch as a styled Spillover load; new events), §5.12 and §6.1 (the package rule);
**STATUS.md** next steps 3–4.
