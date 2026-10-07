# tools/audition — the factory audition's renders, pre-screen and ratings log

The tooling of [mode-compiler.md](../../docs/design/mode-compiler.md) §11.3 (design §12.4 lane
E): the offline render the app and `bspc render` share, the scripted renders S0–S11, the
objective pre-screen that runs before every listening pass, and the ratings log whose rows
carry forward across sound-revision bumps by render hash.

| Piece | Where |
| --- | --- |
| The shared render, inputs, hashes, WAVs, recipes, scripts and pre-screen | `src/` (`brainscape_audition`, namespace `bsa`, no JUCE) |
| The command line | `bspc render` ([`tools/bspc`](../bspc)) |
| The ratings log and its carry-forward | `ratings.py` (Python 3, standard library only) |
| Tests | `tests/` (`audition_unit`, `audition_ratings`, `audition_bspc_render` in `ctest`) |

The app's offline audition (`plugin/src/Audition.*`, companion §4.9) renders through the same
`bsa::Renderer` and writes the same recipe format, so an app render and a `bspc render` of one
preset on one input give one hash.

## The render

`bsa::Renderer` holds one engine in the canonical configuration (determinism profile §2.3: 48
kHz, a 2^22-frame ring, stereo input, dither on the ring write). Each render sets the device
settings to their defaults, loads the preset Exact (which restarts the engine, so a render
starts from the exact-restart state whatever ran before), conditions a copy of the input as the
pedal's codec hands it over (`ConditionInput24` on each channel, then R := L for a mono input)
and processes it in a block pattern (the pedal's 48 frames unless a script asks otherwise).
Script events (`SetParam`, `Freeze`, `Trigger`, `SpilloverLoad`, `MacroMove`, `Expression`)
travel through the engine's own `EventQueue` at their frames, so any block pattern renders the
same bits; the tests check that against an engine driven by hand.

Render identity is the SHA-256 of the interleaved little-endian float32 frames, whole and per
1 s segment (the golden harness's byte stream), never the WAV's bytes. The 16-bit WAVs are for
listening: each sample is `round(x * 32768)` half away from zero, saturated, computed exactly
in binary64, so they are the same bytes on every machine too.

## `bspc render`

```bash
bspc render [--script S0,S2,...|all] [--class attack|pad] [--declarations AUDITION.md]
            [--metrics] [--no-wav] [-o DIR] FILE...
```

Each `FILE` is a preset document (compiled in memory as it reads now; a missing or stale stamp
is reported, not refused) or a `.bsp` package. The files given form the set: S10 loads each
preset from the one before it (the last for the first, the default preset for a set of one),
and S11 plays each at every other member's stored positions. `--class` sets the input class of
presets the declarations do not name; `--declarations` reads each preset's class,
self-oscillation and attack needs from a ratings log (below). `--metrics` runs the pre-screen
and exits 1 when a check fails. The default script is S0 and the default directory `audition`.

Output, per preset, in `DIR/<id>/`:

| File | Holds |
| --- | --- |
| `<render>.wav` | 48 kHz 16-bit stereo, for listening (S11 writes only its worst cases, below) |
| `<render>.json` | The recipe (`brainscape-audition/2`): sound revision, engine toolchain, block pattern, device settings, the package's identity and hashes, every leaf as loaded with its bits, the macro positions, the load report, the input (vector, generator version, conditioning, SHA-256), the events (inline up to 64, always by hash), the output's whole and per-second SHA-256, the WAV's path and SHA-256, and with `--metrics` the measurements |
| `audition.json` | The index (`brainscape-audition-index/1`): the preset's identity and declarations, each script's hash, every render's hashes and headline measurements, what was skipped, and the pre-screen's checks |
| `prescreen.txt` | With `--metrics`: the summary `bspc render` prints |

A full run (`--script all`) of one preset in a set of 14 is about 60 renders of 16–30 s; on the
workstation of record §1.2 a set of one takes about 10 s with WAVs.

## The scripts

The inputs are dsp's test-signal vectors (`dsp/include/brainscape/TestSignal.h`), integer
generated, so every machine renders the same input bits: Plucks, Strums, SoftNotes,
OnsetBursts, Saturation and Silence, each 10 s and then 10 s of silence. A preset declares its
input class: an attack mode's class input is Plucks (its levels are judged on Plucks and
Strums), a pad mode's SoftNotes. DI clips join later, outside git and keyed by their hashes
(design Q9).

| Script | Renders |
| --- | --- |
| S0 | The stored preset on every vector; the wet alone (Mix 1) on Plucks, Strums and SoftNotes; the class render again on a fresh request and in a mixed block pattern (determinism) |
| S1–S6 | Activity, Repeats, Shape, Time, Space, Filter: each macro from its stored position to 0, to 1 and back over 16 s at constant speed, one `MacroMove` per 48-frame block, the class input looped for the whole sweep; beside them the stored preset on the same looped input (the sweeps' reference, named under S1), and the first sweep again in 512-frame blocks |
| S7 | Repeats stored at 0, 0.25, 0.5, 0.75 and 1, each on the class input and then 20 s of silence |
| S8 | Freeze engaged at 4 s and released at 14 s |
| S9 | A footswitch trigger every 0.5 s from 1.25 s to 8.75 s |
| S10 | A Spillover load at 5 s from the set's previous preset (loaded Exact at frame 0), with Trails and with FastCut |
| S11 | The preset at every other set member's stored macro positions, and at the 16 corners of Activity × Repeats × Shape × Time (the others stored) |

A position a script stores is applied as the derive rule applies it (§3.5): each targeted leaf
set to the macro's value there and CTRL's position moved, so a combination plays as if saved
there. A macro the mode leaves undefined is skipped and listed. S11 writes WAVs only for its
three worst renders by peak, by tail and by largest step, re-rendered and checked against their
hashes.

## The pre-screen

Levels are integrated K-weighted loudness (ITU-R BS.1770-4: the K filter at 48 kHz, 400 ms
blocks with 75 % overlap, the −70 LKFS absolute and −10 LU relative gates) over the 10 s the
input sounds in, against the dry input's own loudness, which is what bypass plays. Values are
rounded to 0.1 before they are compared. The measurements use binary64 arithmetic and libm;
they change no render and are not part of a render's identity.

| Check | Passes when |
| --- | --- |
| Renders | every render rendered and every load was exact |
| Determinism | the repeated and re-blocked renders give the same hash as their originals |
| Finite, Denormals | no NaN, infinite or subnormal sample in any render |
| Peak | ≤ −1 dBFS on the class inputs at stored positions |
| Level | the wet alone (Mix 1) within ±2 LU of the dry on the class inputs |
| Engaged | the stored preset from 1 LU below to 4 LU above the dry on the class inputs |
| Tail | the time to −70 dBFS after the input stops is finite, unless the mode is declared self-oscillating |
| Fallback | an onset mode (onset source or mark positioning): its wet-to-dry level on SoftNotes within 12 dB of that on Plucks; a mode declared as needing attacks is reported as "declared" instead |
| Sweeps | Activity, Shape and Time: each 3 s window within ±3 LU of the reference at the same time; Repeats, Space and Filter are reported, not judged |
| Clicks | no sample step in a sweep above 4 times the reference's largest step |
| Repeats | S2's rising leg does not fall; S7's levels and tails do not fall rung to rung; at maximum at most +10 LU over the stored render |
| Combinations | every S11 render holds Peak (≤ 0 dBFS), Tail and Clicks (against S0's class render); the worst cases are named |
| Peak (moved) | every sweep and S11 render ≤ 0 dBFS |
| Response (Shape) | Shape from 0 to 1 moves the brightness by 5 % or the envelope's variation by 1 dB, each against the reference at the same time |
| Response (Activity) | reported: the brightness, the envelope's variation and the onset density the engine's own detector hears in the output, from 0 to 1 and across the rising leg |
| Peak (other), Load | reported: peaks of S0's other vectors and of S7–S10, onsets per second on the class input |

### Readings

Where the design says "non-decreasing" or "measurably" of measurements that move with the
input, the tooling uses its own tolerances (`src/Suite.cpp`): S2's rising leg may dip by 1 LU
from one 1 s window step to the next; S7's levels by 0.5 LU and its tails by 0.25 s from rung
to rung; Shape responds when the brightness moves by 5 % or the envelope's variation by 1 dB.
Brightness is the frequency of the sinusoid whose first differences carry the same share of
its energy; the envelope's variation is the standard deviation in dB of the 10 ms RMS envelope
over its frames above −60 dBFS.

A tail that has not reached −70 dBFS 0.5 s before the render ends is extrapolated: the 100 ms
peak envelope over the render's last 5 s (at most the silent part) is fitted with a line, and
a fall of at least 0.2 dB per second is extended to −70 dBFS (reported as "about N s,
extrapolated"); a slower fall, a flat level or a rising one is "unending". So a long Repeats
tail at high feedback is finite, as the design means it, and only a sustained or growing tail
fails the Tail check.

The engine does not report voices or births, so the Load check logs the input's onset count
and Activity's response is judged through the output; both are information, not verdicts.

## The ratings log

`ratings.py` keeps the factory audition's ratings in the file the design names,
`firmware/factory/AUDITION.md` by default: tables generated for reading, and at its end the log
itself, a JSON block under a marker line that the script writes and `bspc render
--declarations` reads. (It is not a `.json` file because every `.json` in `firmware/factory/` is
a preset document to `bspc_roundtrip.py`.) A row per preset `id` holds what the mode declares (input class, self-oscillation, needs attacks) and the owner's
rating: one chord sounds finished (yes or no), each knob 1–5, the level against bypass, keep,
revise or drop, notes; recorded with the `sound_rev`, `sound_hash` and the hash of every
S0–S11 render the rating was made on (with per-second hashes), and whether the pre-screen had
failed.

```bash
python tools/audition/ratings.py init
python tools/audition/ratings.py declare factory.lull --class pad --self-oscillating
bspc render --script all --metrics --declarations firmware/factory/AUDITION.md -o renders firmware/factory/*.json
python tools/audition/ratings.py rate --renders renders factory.lull --chord yes \
    --knob activity=4 --knob repeats=3 --knob shape=4 --knob time=3 --knob space=4 --knob filter=5 \
    --level "+0.8 LU" --verdict keep --notes "..."
# after a bump or a re-stamp:
python tools/audition/ratings.py refresh --renders renders --bspc build/tools/bspc/Release/bspc firmware/factory/*.json
python tools/audition/ratings.py exit --renders renders
```

`rate` refuses renders that lack a script or were made with other declarations than the log's,
and asks for every knob the mode defines. `carry` (which `refresh` runs after re-rendering)
compares each rating's renders with the new ones: a rating whose renders all kept their hashes
carries forward, its `sound_rev` and `sound_hash` moved to the new ones and the move recorded;
any other row is marked "re-listen", naming each changed or missing render and its first
differing second. Renders a rating never heard (S11 against a mode added to the set since) do
not break the carry; they are listed. `exit` checks §11.3's exit: at least 10 modes kept, one
in every family, every kept mode's knobs rated 3 or more, and no objective failure in their
latest pre-screen. `md` regenerates the tables from the data block, and `md --check` fails when
they differ from it (a hand edit).

## Tests

- `audition_unit` (`tests/test_audition.cpp`): the shared render against an engine driven by
  hand in the 48, 512, 1-frame and mixed block patterns, on a reused and a fresh engine; events
  at their stamps through the transport; refused requests; a Spillover load; the inputs, looped
  and conditioned; render hashes against an independent byte stream; 16-bit and float WAVs; the
  loudness against known values (a 997 Hz sine, both gates); peaks, tails (measured and
  extrapolated), steps and the number checks; positions applied as stored ones; the scripts'
  plan; every pre-screen threshold on both sides of its edge; a suite written to disk.
- `audition_ratings` (`tests/test_ratings.py`): the log, declarations, a rating, the
  carry-forward on a re-stamp, re-listen with the first differing second, gone and new renders,
  the exit criteria, the tables against the data block and a missing or broken block, over
  synthetic indexes.
- `audition_bspc_render` (`tests/test_render_cli.py`): `bspc render` end to end on
  `compiler/tests/data/engram.json`: the files and their hashes, the same hashes without WAVs,
  refused usage and declarations, declarations read from a log, and a rating carried and then
  marked re-listen when the document changes what it plays.
