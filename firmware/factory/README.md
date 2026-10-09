# firmware/factory — the factory modes

The first factory set ([mode-compiler.md](../../docs/design/mode-compiler.md) §11, step 4): one
schema-1 preset document per mode, its package compiled by `bspc` beside it, `MANIFEST` with
the packages' hashes (the sound-revision gate's package rule reads it), and `AUDITION.md`, the
ratings log with each mode's input class, its authoring history and the owner's ratings
(written by `tools/audition/ratings.py`, never by hand). Names are working names (owner question
Q3): nothing here has had a trademark search.

| Mode | Family | What it does | Wave 1 |
| --- | --- | --- | --- |
| Engram | echoic | A clean delay whose repeats land on the post delay's exact taps; Activity smears what enters it | |
| Callback | echoic | A grain tap answered by the post delay's exact taps | |
| Retrograde | echoic | A reverse delay in crossfaded chunks | |
| Updraft | echoic | Echoes that climb an octave with every repeat | |
| Pinhole | echoic | Repeats through a narrow resonant band; Filter moves the band | |
| Murmuration | reverie | A dense, randomized grain cloud around the playing | |
| Halation | reverie | A grain cloud with an octave above glowing through it | pitch set {0, +12} |
| Undertow | reverie | A grain cloud with an octave below pulling under it | pitch set {0, −12} |
| Lull | reverie | A long, slow wash that holds what you play; Repeats sets how long it lingers | |
| Echolalia | reverie | Repeats the newest note over and over, fading as it ages | `decay_ms` |
| Déjà Vu | reverie | Phasing copies of the newest onset, fading as it ages | `decay_ms`, bursts |
| Kaleido | recall | Octave-up loops anchored to the newest onset | pitch set, `repeat`, `decay_ms` |
| Refrain | recall | Free-running loops at pitch and an octave up; Activity sets how many play | pitch set, `repeat`, `voice_count` |
| Shards | misfire | Each attack scatters a stream of octave-up shards of itself, half of them reversed | onset-only source, bursts, `decay_ms`, `voice_count` |

`reserve/` holds four more, written and pre-screened like the set, for a mode the listening pass
drops: Afterimage (a short, close cloud), Runaway (an octave shimmer that feeds on itself into a
long swell, then fades), Downdraft (Updraft falling) and Recurrence (Refrain's loops an octave
down).

Lull and the clouds (Murmuration, Halation, Undertow, Afterimage) and Runaway are pad class, judged
on SoftNotes; the rest are attack class, judged on Plucks and Strums, and Shards is declared as
needing attacks (`AUDITION.md` holds the declarations).

Every document is canonical, stamped at the current sound revision and passes `bspc lint
--factory`; CI's `bspc-roundtrip` checks all of that and that each `.bsp` is the document's
package byte for byte. After a change:

```bash
bspc fmt DOC.json && bspc derive DOC.json && bspc stamp DOC.json   # or derive --solve after editing leaves
bspc lint --factory DOC.json && bspc compile DOC.json
bspc roundtrip --write-manifest MANIFEST -- *.json reserve/*.json  # in this directory
```

The objective pre-screen (§11.3), before every listening pass, over the set (S10 and S11 cross
it) and over the reserves, in this directory. The order is part of the result: S10 loads each mode
from the one before it in the list (the last for the first). The renders go outside the tree
(`build/` is ignored; a render's recipe JSON inside `firmware/factory/` would be a preset document
to `bspc_roundtrip.py`):

```bash
bspc render --script all --metrics --declarations AUDITION.md -o ../../build/prescreen-set \
    engram.bsp callback.bsp retrograde.bsp updraft.bsp pinhole.bsp murmuration.bsp halation.bsp \
    undertow.bsp lull.bsp echolalia.bsp deja-vu.bsp kaleido.bsp refrain.bsp shards.bsp
bspc render --script all --metrics --declarations AUDITION.md -o ../../build/prescreen-reserve \
    reserve/afterimage.bsp reserve/runaway.bsp reserve/downdraft.bsp reserve/recurrence.bsp
```

The owner rates each mode in `AUDITION.md` against those renders (`ratings.py rate --renders
../../build/prescreen-set ...`, see `tools/audition/README.md`). All 18 pass every check at sound
revision 7; `AUDITION.md` records each mode's numbers and every iteration that got it there, and
names the knobs the pre-screen reports for listening (Response (Repeats) "listen").
