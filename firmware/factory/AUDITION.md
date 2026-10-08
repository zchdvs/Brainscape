# Factory audition log

The factory modes' ratings (docs/design/mode-compiler.md §11.3), written by
`tools/audition/ratings.py` (`declare`, `rate`, `carry`, `refresh`), never by hand: the
tables are generated from the data block at the end, which is the log. Ratings are keyed
by preset id and the S0-S11 render hashes they were made on; *Renders* names that set.
Knobs are rated 1-5.

| Preset | Family | Class | Status | sound_rev | sound_hash | Renders | Chord | Activity | Repeats | Shape | Time | Space | Filter | Level | Verdict | Notes |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| factory.afterimage | reverie | attack | unrated |  |  |  |  |  |  |  |  |  |  |  |  |  |
| factory.callback | echoic | attack | unrated |  |  |  |  |  |  |  |  |  |  |  |  |  |
| factory.deja-vu | reverie | attack | unrated |  |  |  |  |  |  |  |  |  |  |  |  |  |
| factory.downdraft | echoic | attack | unrated |  |  |  |  |  |  |  |  |  |  |  |  |  |
| factory.echolalia | reverie | attack | unrated |  |  |  |  |  |  |  |  |  |  |  |  |  |
| factory.engram | echoic | attack | unrated |  |  |  |  |  |  |  |  |  |  |  |  |  |
| factory.halation | reverie | attack | unrated |  |  |  |  |  |  |  |  |  |  |  |  |  |
| factory.kaleido | recall | attack | unrated |  |  |  |  |  |  |  |  |  |  |  |  |  |
| factory.lull | reverie | attack | unrated |  |  |  |  |  |  |  |  |  |  |  |  |  |
| factory.murmuration | reverie | attack | unrated |  |  |  |  |  |  |  |  |  |  |  |  |  |
| factory.pinhole | echoic | attack | unrated |  |  |  |  |  |  |  |  |  |  |  |  |  |
| factory.recurrence | recall | attack | unrated |  |  |  |  |  |  |  |  |  |  |  |  |  |
| factory.refrain | recall | attack | unrated |  |  |  |  |  |  |  |  |  |  |  |  |  |
| factory.retrograde | echoic | attack | unrated |  |  |  |  |  |  |  |  |  |  |  |  |  |
| factory.runaway | reverie | pad, self-osc. | unrated |  |  |  |  |  |  |  |  |  |  |  |  |  |
| factory.shards | misfire | attack, needs attacks | unrated |  |  |  |  |  |  |  |  |  |  |  |  |  |
| factory.undertow | reverie | attack | unrated |  |  |  |  |  |  |  |  |  |  |  |  |  |
| factory.updraft | echoic | attack | unrated |  |  |  |  |  |  |  |  |  |  |  |  |  |

## Authoring history

How each recipe came to be what the listening pass hears: where it started and every
pre-screen iteration since, with its reason (`ratings.py note`).

- **method**: The 14 modes of design §11.1 and four reserves (Afterimage, Runaway, Downdraft and Recurrence, loops at {-12, 0}; the design's two other reserves stay unwritten) start from record §2.2's recipes (draft v2), written in schema 1 at sound revision 7 with wave 1 where §11.1 names it: pitch sets, repeat, decay_ms, voice_count, bursts and onset-only sources. Every targeted leaf is derived from its macro's stored position (bspc derive --solve, anchored on each macro's first target), so a few leaves differ from the record's typed values; the history names them. Each iteration ran the whole pre-screen (bspc render --script all --metrics) over the 14 as one set, so S10 and S11 cross the set, and over the reserves as a set of their own.
- **input classes**: Every mode is attack class (judged on Plucks and Strums) except Runaway (pad, declared self-oscillating); Shards is declared as needing attacks. The cloud modes (Murmuration, Halation, Undertow, Afterimage) and Lull began as pad modes. On SoftNotes (the dry at -12.1 LUFS, peaks at -7.2 dBFS) their wet is noise-like, with an 11-13 dB crest, so a wet within 2 LU of the dry (Level) puts the engaged output over -1 dBFS (Peak): both could not pass, and Lull's stored peak moved by up to 2.9 dB with one voice more or less. As attack modes they pass; their SoftNotes peaks are reported under Peak (other). Play them with the DI pad in the listening pass: a hot sustained input can clip the codec.
- **tap modes**: Engram, Callback and Pinhole replace the record's single rectangular 100 ms grain (transparent only while nothing moves) with four Hann grains a quarter-grain apart (overlap 0.39685, sustain 0, smoothness 1): a constant window sum, still a clean tap, but Activity's spray and jitter and Time's base_ms now crossfade instead of splicing. The compiler's example Engram scores 41.7x and 11.8x its static step on SoftNotes (Clicks); these score 0.8-1.2x.
- **shape**: The Response check wants Shape to move the brightness by 5 % or the envelope's variation by 1 dB from 0 to 1; window-only Shapes (sustain, skew) measured 0-4 % on Plucks once the dry plays at unity under the Mix law. Most modes now take Shape towards fewer, longer, swelled grains: one voice at 1 in the tap modes, Retrograde and Updraft, longer grains with a late peak in the cloud modes. Skew stops at 0.9: at 1.0 a grain ends on a step (37-174x on SoftNotes in S11's corners).
- **combinations**: S11, the 16 corners of Activity x Repeats x Shape x Time and the other modes' stored positions, drove most iterations: one coherent voice at gain 1 (Shape's or Activity's low end) with Repeats at its maximum peaked over 0 dBFS. The fixes, per mode below: lower Repeats maxima, a floor of two or three voices, wet trims lower by 0.5-1.5 dB, a lower stored Mix.

### `factory.afterimage` (Afterimage)

- **v1**: Reserve. Record recipe, pad class: Peak -0.56 dBFS and Level -2.3 LU on SoftNotes, Response (Shape) -4.2 %.
- **v2**: Attack class, wet_trim_db +2.
- **v3**: Mix 0.45 to 0.4, Shape size 30 to 400 ms with a late peak: Combinations' steps (skew 1.0).
- **v4**: Skew maximum 0.9: S11 corner +0.21 dBFS.
- **v5**: wet_trim_db +1.5: passes.
- **pre-screen**: Every check passes (sound revision 7, the committed package). Level plucks -1.6 LU; strums -1.7 LU; Engaged plucks +1.5 LU; strums +1.5 LU; Peak plucks -2.49 dBFS; strums -1.55 dBFS; Tail plucks 1.99 s; strums 3.30 s; sweeps and S11: highest -0.10 dBFS (S11.corner-a0r1s1t0.plucks).

### `factory.callback` (Callback)

- **v1**: Record recipe with the tap grain stage, so Time's base_ms moves by crossfades. Failed Response (Shape): -1.4 %.
- **v2**: Shape as Engram's v2: passes Shape.
- **v3**: Shape as Engram's v3 (skew 0.95, size 300 ms): Combinations' s1t1 corners up to +2.41 dBFS.
- **v4**: wet_trim_db -1.5 dB, Repeats maximum 0.85 to 0.75: +1.25 dBFS.
- **v5**: Mix 0.4 to 0.35, Repeats 0.15-0.65, Time's base_ms 60-800 ms and delay 90-1200 ms (from 1000 and 1500), Activity's delay mix maximum 0.75 to 0.65 (ten trials: a lower trim or a two-voice Shape floor lost the Shape response, 4.1-4.7 %): passes. Activity's grain feedback (in_range 0.6-1) derives 0.11 at the stored position, not the record's 0.
- **pre-screen**: Every check passes (sound revision 7, the committed package). Level plucks -1.3 LU; strums -1.3 LU; Engaged plucks +1.3 LU; strums +1.1 LU; Peak plucks -2.94 dBFS; strums -3.67 dBFS; Tail plucks 3.24 s; strums 3.12 s; sweeps and S11: highest -0.20 dBFS (S11.corner-a1r0s1t1.plucks).

### `factory.deja-vu` (Déjà Vu)

- **v1**: Wave 1's decay_ms 3300 ms (Repeats 400-12000 ms) and bursts of 3 grains 9 ms apart; wet_trim_db -3: Level -7.1/-6.5 LU.
- **v2**: wet_trim_db +4: Combinations' r1 corners +0.11 dBFS.
- **v4**: wet_trim_db +3.5: passes.
- **pre-screen**: Every check passes (sound revision 7, the committed package). Level plucks -0.6 LU; strums +0.0 LU; Engaged plucks +2.7 LU; strums +2.8 LU; Peak plucks -3.17 dBFS; strums -3.65 dBFS; Tail plucks 3.28 s; strums 2.73 s; sweeps and S11: highest -0.26 dBFS (S11.corner-a0r1s1t0.plucks).

### `factory.downdraft` (Downdraft)

- **v1**: Reserve: Updraft at -12. Level -3.0/-2.8 LU, Response (Shape) -2.1 %.
- **v2**: wet_trim_db +1.5.
- **v3**: Shape size 60 to 400 ms with skew 0.3 to 1.0: steps in S11's corners (skew 1.0).
- **v4**: Skew maximum 0.9, Repeats maximum 0.8: passes.
- **pre-screen**: Every check passes (sound revision 7, the committed package). Level plucks -1.7 LU; strums -1.5 LU; Engaged plucks +1.8 LU; strums +1.8 LU; Peak plucks -2.65 dBFS; strums -3.40 dBFS; Tail plucks 3.57 s; strums 4.25 s; sweeps and S11: highest -1.03 dBFS (S11.corner-a1r1s1t1.plucks).

### `factory.echolalia` (Echolalia)

- **v1**: Wave 1's decay_ms 3300 ms, which Repeats moves (400-12000 ms) in place of feedback; wet_trim_db -3: Level -4.0/-3.6 LU, Response (Shape) +1.4 %.
- **v2**: wet_trim_db +1; Shape adds smoothness and takes sustain to 0: Shape passes (+15.4 %); Peak +0.12 dBFS.
- **v3**: Mix 0.5 to 0.4, wet_trim_db 0: passes.
- **pre-screen**: Every check passes (sound revision 7, the committed package). Level plucks -0.9 LU; strums -0.5 LU; Engaged plucks +1.6 LU; strums +1.8 LU; Peak plucks -1.23 dBFS; strums -1.29 dBFS; Tail plucks 2.79 s; strums 1.79 s; sweeps and S11: highest -0.57 dBFS (S11.at-factory.retrograde.plucks).

### `factory.engram` (Engram)

- **v1**: Record v2 (its repeats on the post delay's exact taps) with the tap grain stage of the set note; Activity also moved overlap. Failed only Response (Shape): -1.0 %, -0.01 dB: four overlapping windows change nothing the mix shows at Mix 0.35.
- **v2**: Activity loses overlap (spray, jitter, spread); Shape takes overlap 0.39685 to 0.25 (four voices to one), skew 0.5 to 0.85, size 100 to 220 ms: -3.4 %, +0.51 dB.
- **v3**: Shape's skew to 0.95 and size to 300 ms, single-voice swells (-8.0 %); base_ms 1 to 2 ms, since Activity's spread at 300 ms needs 1.39 ms (lint L2). Combinations: corner a0r1s1t1 +1.24 dBFS (one voice at gain 1, Repeats 0.9).
- **v4**: Repeats maximum 0.9 to 0.8: +1.00 dBFS.
- **v5**: wet_trim_db -1.5 dB, Repeats maximum 0.75: passes.
- **pre-screen**: Every check passes (sound revision 7, the committed package). Level plucks -1.5 LU; strums -1.1 LU; Engaged plucks +1.2 LU; strums +1.2 LU; Peak plucks -3.60 dBFS; strums -3.37 dBFS; Tail plucks 2.90 s; strums 2.65 s; sweeps and S11: highest -0.20 dBFS (S11.corner-a0r1s1t1.plucks).

### `factory.halation` (Halation)

- **v1**: Wave 1's pitch set {0, +12}, random; pad class, wet_trim_db +2: Peak +1.12 dBFS on SoftNotes; Combinations up to +6.6 dBFS (at Pinhole's positions Activity 0 left spray at 0: coherent feedback).
- **v2**: Attack class.
- **v4**: wet_trim_db +1.5, Activity's spray floor 20 ms, Shape on size, skew and sustain: Response (Shape) -3.7 %.
- **v5**: base_ms 450 to 520 ms and Time 520-1500 ms, so Shape's size reaches 500 ms inside the octave's near guard (L2); Shape adds reverse 0 to 0.6 from position 0.3: passes. Sustain derives 0.145 at the stored Shape (record 0.3).
- **v6**: At Retrograde's new positions S11 peaked +1.10 dBFS.
- **v7**: Mix 0.5 to 0.45, wet_trim_db +0.5: passes.
- **pre-screen**: Every check passes (sound revision 7, the committed package). Level plucks -1.6 LU; strums -1.7 LU; Engaged plucks +1.8 LU; strums +1.6 LU; Peak plucks -3.22 dBFS; strums -3.47 dBFS; Tail plucks 3.49 s; strums 5.08 s; sweeps and S11: highest -0.04 dBFS (S11.at-factory.retrograde.plucks).

### `factory.kaleido` (Kaleido)

- **v1**: Mark positioning with wave 1's set {+12 x2, 0} (cycle), repeat 3 and decay_ms 4000 ms; base_ms 300 to 450 ms so the octave's grains at Time's 400 ms clear the near guard (L2); wet_trim_db 0: Level -6.8/-6.4 LU.
- **v2**: wet_trim_db +6.5: one-voice corners (Activity 0) up to +4.2 dBFS.
- **v4**: Activity's floor overlap 0.32 (two voices): +0.7 dBFS.
- **v5**: wet_trim_db +5.5: passes.
- **pre-screen**: Every check passes (sound revision 7, the committed package). Level plucks -1.3 LU; strums -0.9 LU; Engaged plucks +2.3 LU; strums +2.4 LU; Peak plucks -3.31 dBFS; strums -2.56 dBFS; Tail plucks 4.00 s; strums 3.10 s; sweeps and S11: highest -0.08 dBFS (S11.corner-a0r0s1t0.plucks).

### `factory.lull` (Lull)

- **v1**: Record recipe, pad class: Peak +0.57 dBFS on SoftNotes; the Repeats sweep +2.56 dBFS (feedback to 1.06).
- **v2-v4**: Mix 0.6, wet_trim_db -1 then -2 dB, Repeats maximum 0.99 then 0.97: Peak -0.43 dBFS at -1 dB, Level -2.9 LU at -2 dB.
- **trials**: Thirteen pad variants (Mix 0.6-0.8, fewer voices, less spray, feedback 0.9): the stored peak ranged from -1.35 to +1.53 dBFS, so none held Peak and Level with margin.
- **v5**: Attack class; feedback 0.95 to 0.9, overlap 0.6 to 0.45, spray 150 to 60 ms, spread 10 to 5 cents, jitter 0.5 to 0.2, Mix 0.6, wet_trim_db +0.5, Repeats 0.7-0.97, Shape size 40 to 500 ms with skew 0.1 to 0.6 (+6.6 % on Plucks): passes. Repeats stays under 1, so the drone's tail is finite (not declared self-oscillating).
- **pre-screen**: Every check passes (sound revision 7, the committed package). Level plucks -1.2 LU; strums -1.3 LU; Engaged plucks +1.4 LU; strums +1.2 LU; Peak plucks -3.67 dBFS; strums -5.45 dBFS; Tail plucks 10.51 s (60 s probe); strums 11.19 s (60 s probe); sweeps and S11: highest -2.71 dBFS (S11.at-factory.updraft.plucks).

### `factory.murmuration` (Murmuration)

- **v1**: Record recipe, pad class, wet_trim_db +2: Peak +1.44 dBFS on SoftNotes, Combinations, Response (Shape) +4.4 %.
- **v2**: Attack class (set note).
- **v3**: Shape size 60 to 500 ms, skew 0.4 to 1.0, sustain 0.2 to 0 (-9.2 %); Combinations: steps at s1 corners, 37-50x (skew 1.0).
- **v4**: Skew maximum 0.9: passes.
- **pre-screen**: Every check passes (sound revision 7, the committed package). Level plucks -1.9 LU; strums -1.9 LU; Engaged plucks +1.6 LU; strums +1.4 LU; Peak plucks -3.10 dBFS; strums -4.48 dBFS; Tail plucks 4.65 s; strums 7.02 s; sweeps and S11: highest -1.41 dBFS (S11.corner-a0r1s1t1.plucks).

### `factory.pinhole` (Pinhole)

- **v1**: Record recipe with the tap grain stage. Combinations: corner a1r1s0t0 +10.0 dBFS with an unending tail (Repeats 0.95 into the resonant band); S6's Filter sweep over 0 dBFS at the bypass end; Response (Shape) -0.3 %.
- **v2**: wet_trim_db -1.5 dB, Repeats maximum 0.85, Shape as Engram's: Shape passes.
- **v4**: Repeats maximum 0.7, resonance 0.55 to 0.5: corner a0r1s1t1 +4.4 dBFS.
- **v5**: Mix 0.5 to 0.4, Repeats maximum 0.6: passes.
- **pre-screen**: Every check passes (sound revision 7, the committed package). Level plucks -1.9 LU; strums -1.8 LU; Engaged plucks +1.3 LU; strums +1.2 LU; Peak plucks -2.52 dBFS; strums -3.32 dBFS; Tail plucks 3.92 s; strums 3.78 s; sweeps and S11: highest -1.36 dBFS (S11.corner-a0r1s1t1.plucks).

### `factory.recurrence` (Recurrence)

- **v1**: Reserve: Refrain's loops at {-12, 0}. Combinations up to +3.7 dBFS, Response (Shape) -2.8 %.
- **v4**: Activity's floor two voices, wet_trim_db +3: S11 corners up to +0.79 dBFS.
- **v6**: Three-voice floor, wet_trim_db +2, Shape sustain 0.5 to 0 with skew 0.5 to 0.15: passes.
- **pre-screen**: Every check passes (sound revision 7, the committed package). Level plucks -1.9 LU; strums -1.8 LU; Engaged plucks +2.1 LU; strums +2.0 LU; Peak plucks -2.18 dBFS; strums -3.35 dBFS; Tail plucks 2.49 s; strums 3.37 s; sweeps and S11: highest -0.62 dBFS (S11.at-factory.runaway.plucks).

### `factory.refrain` (Refrain)

- **v1**: Wave 1's set {0, +12} (cycle), repeat 4 and voice_count 4; base_ms 420 ms (L2); wet_trim_db +5: Level +2.0/+3.1 LU, Engaged +4.1/+4.6 LU, Combinations.
- **v2**: wet_trim_db +3: Response (Shape) +4.8 %.
- **v4**: wet_trim_db +2.5, Activity's floor two voices, Shape sustain 0.8 to 0 and skew 0.1 to 0.9: Shape +0.6 %.
- **v5**: wet_trim_db +2, Shape sustain 0.5 to 0 with skew 0.5 to 0.9 (the loops swell): passes.
- **pre-screen**: Every check passes (sound revision 7, the committed package). Level plucks -1.0 LU; strums +0.1 LU; Engaged plucks +2.5 LU; strums +2.8 LU; Peak plucks -2.69 dBFS; strums -3.37 dBFS; Tail plucks 2.46 s; strums 3.42 s; sweeps and S11: highest -0.09 dBFS (S11.corner-a0r0s1t1.plucks).

### `factory.retrograde` (Retrograde)

- **v1**: Record recipe. Failed Level -2.7/-2.5 LU and Response (Shape) +2.8 %.
- **v2**: wet_trim_db +1.5 dB; Shape variants on skew, sustain and smoothness measured 0.9-3.6 %.
- **v3**: Shape takes overlap 0.32 to 0.25 with sustain 0.6 to 0 and skew 0.5 to 0.95 (single swelled reversed chunks), Activity becomes spray and jitter. Combinations: s1 corners up to +5.7 dBFS (one voice, Repeats 0.85, no spray).
- **v4**: Repeats maximum 0.7, a 10 ms spray floor: +2.2 dBFS.
- **v5**: wet_trim_db +0.8, Mix 0.5 to 0.4, Repeats maximum 0.6, Activity's spray 10-80 ms, Time's size 120-400 ms (from 500): passes, but the stored 400 ms chunk sat at Time's maximum.
- **v6**: Stored size 400 to 320 ms, Time at 0.71 (base_ms derives 45.7 ms): passes.
- **pre-screen**: Every check passes (sound revision 7, the committed package). Level plucks -1.7 LU; strums -2.0 LU; Engaged plucks +1.5 LU; strums +1.3 LU; Peak plucks -2.47 dBFS; strums -3.34 dBFS; Tail plucks 2.35 s; strums 2.79 s; sweeps and S11: highest -0.21 dBFS (S11.corner-a0r1s1t0.plucks).

### `factory.runaway` (Runaway)

- **v1**: Reserve. Record recipe (feedback 1.05, Repeats to 1.1), pad class, declared self-oscillating: Peak +2.04 dBFS, Level +2.7 LU, Engaged +4.4 LU on SoftNotes.
- **v2**: wet_trim_db -4.
- **v4**: wet_trim_db -5.5, Repeats maximum 1.08: Level -2.8 LU.
- **v5**: Mix 0.5 to 0.6, wet_trim_db -4.5: passes, the one pad-class mode.
- **pre-screen**: Every check passes (sound revision 7, the committed package). Level soft_notes -1.8 LU; Engaged soft_notes +1.0 LU; Peak soft_notes -1.51 dBFS; Tail soft_notes 8.61 s; sweeps and S11: highest -0.05 dBFS (S11.corner-a1r1s1t0.soft_notes).

### `factory.shards` (Shards)

- **v1**: Wave 1's sources {onset} only, mark positioning, bursts of 6 grains 35 ms apart, decay_ms 2000 ms: Level -15.7/-14.1 LU.
- **v2**: voice_count 3 (normalized as three voices, not six), wet_trim_db +10, post-delay echoes: Level -2.8/-2.0 LU.
- **v4**: Bursts of 8, wet_trim_db +9: Strums -6.0 LU (an onset every 2 s, against Plucks' two a second).
- **v5**: Each attack streams 16 shards 120 ms apart at two voices with a 20 s decay, so dense playing steals and sparse playing streams: Plucks +1.6, Strums +0.7 LU; wet_trim_db +6, Mix 0.4. Activity's stored position was its maximum (16 shards).
- **v6**: Activity anchored on spray (400 ms at 0.42), its burst count reaching 16 by 0.42: S11 at Deja Vu's positions +1.36 dBFS.
- **v7**: wet_trim_db +4.5, Mix 0.38: passes.
- **pre-screen**: Every check passes (sound revision 7, the committed package). Level plucks +0.1 LU; strums -0.8 LU; Engaged plucks +1.9 LU; strums +1.5 LU; Peak plucks -3.27 dBFS; strums -3.56 dBFS; Tail plucks 3.70 s; strums 3.00 s; sweeps and S11: highest -0.41 dBFS (S11.at-factory.deja-vu.plucks).

### `factory.undertow` (Undertow)

- **v1**: Wave 1's pitch set {0, -12}, random; pad class, wet_trim_db +1: Peak -0.78 dBFS on SoftNotes; Combinations up to +6.75 dBFS.
- **v2**: Attack class, wet_trim_db +2 (Level on Plucks -2.3 LU at +1).
- **v4**: Activity's spray floor 20 ms (at Engram's positions spray 0 peaked +2.2 dBFS): passes.
- **pre-screen**: Every check passes (sound revision 7, the committed package). Level plucks -1.0 LU; strums -1.0 LU; Engaged plucks +2.5 LU; strums +2.4 LU; Peak plucks -3.33 dBFS; strums -3.66 dBFS; Tail plucks 2.26 s; strums 3.54 s; sweeps and S11: highest -0.72 dBFS (S11.corner-a0r1s1t0.plucks).

### `factory.updraft` (Updraft)

- **v1**: Record recipe. Failed Response (Shape): +1.3 %.
- **v2-v4**: Shapes on sustain, skew, size and smoothness reached 2.8 %; a single-voice swell passed alone (+8.4 %) but peaked at s1 corners (+2.5 dBFS); Repeats maximum 0.95 to 0.8. The octave's near guard keeps Shape's size under 190 ms.
- **v5**: wet_trim_db 0, Repeats maximum 0.75, Time's base_ms 250-1200 ms (from 150), Activity on spray, spread and jitter (overlap moves to Shape), Shape overlap 0.45 to 0.25 with sustain 0.35 to 0, skew 0.5 to 0.9 and size 120 to 190 ms: passes.
- **pre-screen**: Every check passes (sound revision 7, the committed package). Level plucks -1.7 LU; strums -1.5 LU; Engaged plucks +1.8 LU; strums +1.8 LU; Peak plucks -3.35 dBFS; strums -3.59 dBFS; Tail plucks 2.41 s; strums 3.93 s; sweeps and S11: highest -0.14 dBFS (S11.corner-a1r1s1t0.plucks).

## Exit criteria

Not met.

    MISS 0 of at least 10 modes kept
    MISS a keeper in every family: recall 0, reverie 0, misfire 0, echoic 0
    ok   every kept mode's knobs rated 3 or more
    ok   no objective failure among the kept

## Data

The log: each preset's declarations (which `bspc render --declarations` reads), its
rating and the hashes of the renders it was made on.

<!-- brainscape-ratings/1 data: written by tools/audition/ratings.py, never by hand -->
```json
{
  "format": "brainscape-ratings/1",
  "presets": {
    "factory.afterimage": {
      "name": "Afterimage",
      "family": "reverie",
      "declare": {
        "class": "attack",
        "self_oscillating": false,
        "needs_attacks": false
      },
      "status": "unrated",
      "rating": null,
      "relisten": [],
      "new_renders": [],
      "retired_renders": [],
      "history": [
        {
          "step": "v1",
          "text": "Reserve. Record recipe, pad class: Peak -0.56 dBFS and Level -2.3 LU on SoftNotes, Response (Shape) -4.2 %."
        },
        {
          "step": "v2",
          "text": "Attack class, wet_trim_db +2."
        },
        {
          "step": "v3",
          "text": "Mix 0.45 to 0.4, Shape size 30 to 400 ms with a late peak: Combinations' steps (skew 1.0)."
        },
        {
          "step": "v4",
          "text": "Skew maximum 0.9: S11 corner +0.21 dBFS."
        },
        {
          "step": "v5",
          "text": "wet_trim_db +1.5: passes."
        },
        {
          "step": "pre-screen",
          "text": "Every check passes (sound revision 7, the committed package). Level plucks -1.6 LU; strums -1.7 LU; Engaged plucks +1.5 LU; strums +1.5 LU; Peak plucks -2.49 dBFS; strums -1.55 dBFS; Tail plucks 1.99 s; strums 3.30 s; sweeps and S11: highest -0.10 dBFS (S11.corner-a0r1s1t0.plucks)."
        }
      ]
    },
    "factory.callback": {
      "name": "Callback",
      "family": "echoic",
      "declare": {
        "class": "attack",
        "self_oscillating": false,
        "needs_attacks": false
      },
      "status": "unrated",
      "rating": null,
      "relisten": [],
      "new_renders": [],
      "retired_renders": [],
      "history": [
        {
          "step": "v1",
          "text": "Record recipe with the tap grain stage, so Time's base_ms moves by crossfades. Failed Response (Shape): -1.4 %."
        },
        {
          "step": "v2",
          "text": "Shape as Engram's v2: passes Shape."
        },
        {
          "step": "v3",
          "text": "Shape as Engram's v3 (skew 0.95, size 300 ms): Combinations' s1t1 corners up to +2.41 dBFS."
        },
        {
          "step": "v4",
          "text": "wet_trim_db -1.5 dB, Repeats maximum 0.85 to 0.75: +1.25 dBFS."
        },
        {
          "step": "v5",
          "text": "Mix 0.4 to 0.35, Repeats 0.15-0.65, Time's base_ms 60-800 ms and delay 90-1200 ms (from 1000 and 1500), Activity's delay mix maximum 0.75 to 0.65 (ten trials: a lower trim or a two-voice Shape floor lost the Shape response, 4.1-4.7 %): passes. Activity's grain feedback (in_range 0.6-1) derives 0.11 at the stored position, not the record's 0."
        },
        {
          "step": "pre-screen",
          "text": "Every check passes (sound revision 7, the committed package). Level plucks -1.3 LU; strums -1.3 LU; Engaged plucks +1.3 LU; strums +1.1 LU; Peak plucks -2.94 dBFS; strums -3.67 dBFS; Tail plucks 3.24 s; strums 3.12 s; sweeps and S11: highest -0.20 dBFS (S11.corner-a1r0s1t1.plucks)."
        }
      ]
    },
    "factory.deja-vu": {
      "name": "Déjà Vu",
      "family": "reverie",
      "declare": {
        "class": "attack",
        "self_oscillating": false,
        "needs_attacks": false
      },
      "status": "unrated",
      "rating": null,
      "relisten": [],
      "new_renders": [],
      "retired_renders": [],
      "history": [
        {
          "step": "v1",
          "text": "Wave 1's decay_ms 3300 ms (Repeats 400-12000 ms) and bursts of 3 grains 9 ms apart; wet_trim_db -3: Level -7.1/-6.5 LU."
        },
        {
          "step": "v2",
          "text": "wet_trim_db +4: Combinations' r1 corners +0.11 dBFS."
        },
        {
          "step": "v4",
          "text": "wet_trim_db +3.5: passes."
        },
        {
          "step": "pre-screen",
          "text": "Every check passes (sound revision 7, the committed package). Level plucks -0.6 LU; strums +0.0 LU; Engaged plucks +2.7 LU; strums +2.8 LU; Peak plucks -3.17 dBFS; strums -3.65 dBFS; Tail plucks 3.28 s; strums 2.73 s; sweeps and S11: highest -0.26 dBFS (S11.corner-a0r1s1t0.plucks)."
        }
      ]
    },
    "factory.downdraft": {
      "name": "Downdraft",
      "family": "echoic",
      "declare": {
        "class": "attack",
        "self_oscillating": false,
        "needs_attacks": false
      },
      "status": "unrated",
      "rating": null,
      "relisten": [],
      "new_renders": [],
      "retired_renders": [],
      "history": [
        {
          "step": "v1",
          "text": "Reserve: Updraft at -12. Level -3.0/-2.8 LU, Response (Shape) -2.1 %."
        },
        {
          "step": "v2",
          "text": "wet_trim_db +1.5."
        },
        {
          "step": "v3",
          "text": "Shape size 60 to 400 ms with skew 0.3 to 1.0: steps in S11's corners (skew 1.0)."
        },
        {
          "step": "v4",
          "text": "Skew maximum 0.9, Repeats maximum 0.8: passes."
        },
        {
          "step": "pre-screen",
          "text": "Every check passes (sound revision 7, the committed package). Level plucks -1.7 LU; strums -1.5 LU; Engaged plucks +1.8 LU; strums +1.8 LU; Peak plucks -2.65 dBFS; strums -3.40 dBFS; Tail plucks 3.57 s; strums 4.25 s; sweeps and S11: highest -1.03 dBFS (S11.corner-a1r1s1t1.plucks)."
        }
      ]
    },
    "factory.echolalia": {
      "name": "Echolalia",
      "family": "reverie",
      "declare": {
        "class": "attack",
        "self_oscillating": false,
        "needs_attacks": false
      },
      "status": "unrated",
      "rating": null,
      "relisten": [],
      "new_renders": [],
      "retired_renders": [],
      "history": [
        {
          "step": "v1",
          "text": "Wave 1's decay_ms 3300 ms, which Repeats moves (400-12000 ms) in place of feedback; wet_trim_db -3: Level -4.0/-3.6 LU, Response (Shape) +1.4 %."
        },
        {
          "step": "v2",
          "text": "wet_trim_db +1; Shape adds smoothness and takes sustain to 0: Shape passes (+15.4 %); Peak +0.12 dBFS."
        },
        {
          "step": "v3",
          "text": "Mix 0.5 to 0.4, wet_trim_db 0: passes."
        },
        {
          "step": "pre-screen",
          "text": "Every check passes (sound revision 7, the committed package). Level plucks -0.9 LU; strums -0.5 LU; Engaged plucks +1.6 LU; strums +1.8 LU; Peak plucks -1.23 dBFS; strums -1.29 dBFS; Tail plucks 2.79 s; strums 1.79 s; sweeps and S11: highest -0.57 dBFS (S11.at-factory.retrograde.plucks)."
        }
      ]
    },
    "factory.engram": {
      "name": "Engram",
      "family": "echoic",
      "declare": {
        "class": "attack",
        "self_oscillating": false,
        "needs_attacks": false
      },
      "status": "unrated",
      "rating": null,
      "relisten": [],
      "new_renders": [],
      "retired_renders": [],
      "history": [
        {
          "step": "v1",
          "text": "Record v2 (its repeats on the post delay's exact taps) with the tap grain stage of the set note; Activity also moved overlap. Failed only Response (Shape): -1.0 %, -0.01 dB: four overlapping windows change nothing the mix shows at Mix 0.35."
        },
        {
          "step": "v2",
          "text": "Activity loses overlap (spray, jitter, spread); Shape takes overlap 0.39685 to 0.25 (four voices to one), skew 0.5 to 0.85, size 100 to 220 ms: -3.4 %, +0.51 dB."
        },
        {
          "step": "v3",
          "text": "Shape's skew to 0.95 and size to 300 ms, single-voice swells (-8.0 %); base_ms 1 to 2 ms, since Activity's spread at 300 ms needs 1.39 ms (lint L2). Combinations: corner a0r1s1t1 +1.24 dBFS (one voice at gain 1, Repeats 0.9)."
        },
        {
          "step": "v4",
          "text": "Repeats maximum 0.9 to 0.8: +1.00 dBFS."
        },
        {
          "step": "v5",
          "text": "wet_trim_db -1.5 dB, Repeats maximum 0.75: passes."
        },
        {
          "step": "pre-screen",
          "text": "Every check passes (sound revision 7, the committed package). Level plucks -1.5 LU; strums -1.1 LU; Engaged plucks +1.2 LU; strums +1.2 LU; Peak plucks -3.60 dBFS; strums -3.37 dBFS; Tail plucks 2.90 s; strums 2.65 s; sweeps and S11: highest -0.20 dBFS (S11.corner-a0r1s1t1.plucks)."
        }
      ]
    },
    "factory.halation": {
      "name": "Halation",
      "family": "reverie",
      "declare": {
        "class": "attack",
        "self_oscillating": false,
        "needs_attacks": false
      },
      "status": "unrated",
      "rating": null,
      "relisten": [],
      "new_renders": [],
      "retired_renders": [],
      "history": [
        {
          "step": "v1",
          "text": "Wave 1's pitch set {0, +12}, random; pad class, wet_trim_db +2: Peak +1.12 dBFS on SoftNotes; Combinations up to +6.6 dBFS (at Pinhole's positions Activity 0 left spray at 0: coherent feedback)."
        },
        {
          "step": "v2",
          "text": "Attack class."
        },
        {
          "step": "v4",
          "text": "wet_trim_db +1.5, Activity's spray floor 20 ms, Shape on size, skew and sustain: Response (Shape) -3.7 %."
        },
        {
          "step": "v5",
          "text": "base_ms 450 to 520 ms and Time 520-1500 ms, so Shape's size reaches 500 ms inside the octave's near guard (L2); Shape adds reverse 0 to 0.6 from position 0.3: passes. Sustain derives 0.145 at the stored Shape (record 0.3)."
        },
        {
          "step": "v6",
          "text": "At Retrograde's new positions S11 peaked +1.10 dBFS."
        },
        {
          "step": "v7",
          "text": "Mix 0.5 to 0.45, wet_trim_db +0.5: passes."
        },
        {
          "step": "pre-screen",
          "text": "Every check passes (sound revision 7, the committed package). Level plucks -1.6 LU; strums -1.7 LU; Engaged plucks +1.8 LU; strums +1.6 LU; Peak plucks -3.22 dBFS; strums -3.47 dBFS; Tail plucks 3.49 s; strums 5.08 s; sweeps and S11: highest -0.04 dBFS (S11.at-factory.retrograde.plucks)."
        }
      ]
    },
    "factory.kaleido": {
      "name": "Kaleido",
      "family": "recall",
      "declare": {
        "class": "attack",
        "self_oscillating": false,
        "needs_attacks": false
      },
      "status": "unrated",
      "rating": null,
      "relisten": [],
      "new_renders": [],
      "retired_renders": [],
      "history": [
        {
          "step": "v1",
          "text": "Mark positioning with wave 1's set {+12 x2, 0} (cycle), repeat 3 and decay_ms 4000 ms; base_ms 300 to 450 ms so the octave's grains at Time's 400 ms clear the near guard (L2); wet_trim_db 0: Level -6.8/-6.4 LU."
        },
        {
          "step": "v2",
          "text": "wet_trim_db +6.5: one-voice corners (Activity 0) up to +4.2 dBFS."
        },
        {
          "step": "v4",
          "text": "Activity's floor overlap 0.32 (two voices): +0.7 dBFS."
        },
        {
          "step": "v5",
          "text": "wet_trim_db +5.5: passes."
        },
        {
          "step": "pre-screen",
          "text": "Every check passes (sound revision 7, the committed package). Level plucks -1.3 LU; strums -0.9 LU; Engaged plucks +2.3 LU; strums +2.4 LU; Peak plucks -3.31 dBFS; strums -2.56 dBFS; Tail plucks 4.00 s; strums 3.10 s; sweeps and S11: highest -0.08 dBFS (S11.corner-a0r0s1t0.plucks)."
        }
      ]
    },
    "factory.lull": {
      "name": "Lull",
      "family": "reverie",
      "declare": {
        "class": "attack",
        "self_oscillating": false,
        "needs_attacks": false
      },
      "status": "unrated",
      "rating": null,
      "relisten": [],
      "new_renders": [],
      "retired_renders": [],
      "history": [
        {
          "step": "v1",
          "text": "Record recipe, pad class: Peak +0.57 dBFS on SoftNotes; the Repeats sweep +2.56 dBFS (feedback to 1.06)."
        },
        {
          "step": "v2-v4",
          "text": "Mix 0.6, wet_trim_db -1 then -2 dB, Repeats maximum 0.99 then 0.97: Peak -0.43 dBFS at -1 dB, Level -2.9 LU at -2 dB."
        },
        {
          "step": "trials",
          "text": "Thirteen pad variants (Mix 0.6-0.8, fewer voices, less spray, feedback 0.9): the stored peak ranged from -1.35 to +1.53 dBFS, so none held Peak and Level with margin."
        },
        {
          "step": "v5",
          "text": "Attack class; feedback 0.95 to 0.9, overlap 0.6 to 0.45, spray 150 to 60 ms, spread 10 to 5 cents, jitter 0.5 to 0.2, Mix 0.6, wet_trim_db +0.5, Repeats 0.7-0.97, Shape size 40 to 500 ms with skew 0.1 to 0.6 (+6.6 % on Plucks): passes. Repeats stays under 1, so the drone's tail is finite (not declared self-oscillating)."
        },
        {
          "step": "pre-screen",
          "text": "Every check passes (sound revision 7, the committed package). Level plucks -1.2 LU; strums -1.3 LU; Engaged plucks +1.4 LU; strums +1.2 LU; Peak plucks -3.67 dBFS; strums -5.45 dBFS; Tail plucks 10.51 s (60 s probe); strums 11.19 s (60 s probe); sweeps and S11: highest -2.71 dBFS (S11.at-factory.updraft.plucks)."
        }
      ]
    },
    "factory.murmuration": {
      "name": "Murmuration",
      "family": "reverie",
      "declare": {
        "class": "attack",
        "self_oscillating": false,
        "needs_attacks": false
      },
      "status": "unrated",
      "rating": null,
      "relisten": [],
      "new_renders": [],
      "retired_renders": [],
      "history": [
        {
          "step": "v1",
          "text": "Record recipe, pad class, wet_trim_db +2: Peak +1.44 dBFS on SoftNotes, Combinations, Response (Shape) +4.4 %."
        },
        {
          "step": "v2",
          "text": "Attack class (set note)."
        },
        {
          "step": "v3",
          "text": "Shape size 60 to 500 ms, skew 0.4 to 1.0, sustain 0.2 to 0 (-9.2 %); Combinations: steps at s1 corners, 37-50x (skew 1.0)."
        },
        {
          "step": "v4",
          "text": "Skew maximum 0.9: passes."
        },
        {
          "step": "pre-screen",
          "text": "Every check passes (sound revision 7, the committed package). Level plucks -1.9 LU; strums -1.9 LU; Engaged plucks +1.6 LU; strums +1.4 LU; Peak plucks -3.10 dBFS; strums -4.48 dBFS; Tail plucks 4.65 s; strums 7.02 s; sweeps and S11: highest -1.41 dBFS (S11.corner-a0r1s1t1.plucks)."
        }
      ]
    },
    "factory.pinhole": {
      "name": "Pinhole",
      "family": "echoic",
      "declare": {
        "class": "attack",
        "self_oscillating": false,
        "needs_attacks": false
      },
      "status": "unrated",
      "rating": null,
      "relisten": [],
      "new_renders": [],
      "retired_renders": [],
      "history": [
        {
          "step": "v1",
          "text": "Record recipe with the tap grain stage. Combinations: corner a1r1s0t0 +10.0 dBFS with an unending tail (Repeats 0.95 into the resonant band); S6's Filter sweep over 0 dBFS at the bypass end; Response (Shape) -0.3 %."
        },
        {
          "step": "v2",
          "text": "wet_trim_db -1.5 dB, Repeats maximum 0.85, Shape as Engram's: Shape passes."
        },
        {
          "step": "v4",
          "text": "Repeats maximum 0.7, resonance 0.55 to 0.5: corner a0r1s1t1 +4.4 dBFS."
        },
        {
          "step": "v5",
          "text": "Mix 0.5 to 0.4, Repeats maximum 0.6: passes."
        },
        {
          "step": "pre-screen",
          "text": "Every check passes (sound revision 7, the committed package). Level plucks -1.9 LU; strums -1.8 LU; Engaged plucks +1.3 LU; strums +1.2 LU; Peak plucks -2.52 dBFS; strums -3.32 dBFS; Tail plucks 3.92 s; strums 3.78 s; sweeps and S11: highest -1.36 dBFS (S11.corner-a0r1s1t1.plucks)."
        }
      ]
    },
    "factory.recurrence": {
      "name": "Recurrence",
      "family": "recall",
      "declare": {
        "class": "attack",
        "self_oscillating": false,
        "needs_attacks": false
      },
      "status": "unrated",
      "rating": null,
      "relisten": [],
      "new_renders": [],
      "retired_renders": [],
      "history": [
        {
          "step": "v1",
          "text": "Reserve: Refrain's loops at {-12, 0}. Combinations up to +3.7 dBFS, Response (Shape) -2.8 %."
        },
        {
          "step": "v4",
          "text": "Activity's floor two voices, wet_trim_db +3: S11 corners up to +0.79 dBFS."
        },
        {
          "step": "v6",
          "text": "Three-voice floor, wet_trim_db +2, Shape sustain 0.5 to 0 with skew 0.5 to 0.15: passes."
        },
        {
          "step": "pre-screen",
          "text": "Every check passes (sound revision 7, the committed package). Level plucks -1.9 LU; strums -1.8 LU; Engaged plucks +2.1 LU; strums +2.0 LU; Peak plucks -2.18 dBFS; strums -3.35 dBFS; Tail plucks 2.49 s; strums 3.37 s; sweeps and S11: highest -0.62 dBFS (S11.at-factory.runaway.plucks)."
        }
      ]
    },
    "factory.refrain": {
      "name": "Refrain",
      "family": "recall",
      "declare": {
        "class": "attack",
        "self_oscillating": false,
        "needs_attacks": false
      },
      "status": "unrated",
      "rating": null,
      "relisten": [],
      "new_renders": [],
      "retired_renders": [],
      "history": [
        {
          "step": "v1",
          "text": "Wave 1's set {0, +12} (cycle), repeat 4 and voice_count 4; base_ms 420 ms (L2); wet_trim_db +5: Level +2.0/+3.1 LU, Engaged +4.1/+4.6 LU, Combinations."
        },
        {
          "step": "v2",
          "text": "wet_trim_db +3: Response (Shape) +4.8 %."
        },
        {
          "step": "v4",
          "text": "wet_trim_db +2.5, Activity's floor two voices, Shape sustain 0.8 to 0 and skew 0.1 to 0.9: Shape +0.6 %."
        },
        {
          "step": "v5",
          "text": "wet_trim_db +2, Shape sustain 0.5 to 0 with skew 0.5 to 0.9 (the loops swell): passes."
        },
        {
          "step": "pre-screen",
          "text": "Every check passes (sound revision 7, the committed package). Level plucks -1.0 LU; strums +0.1 LU; Engaged plucks +2.5 LU; strums +2.8 LU; Peak plucks -2.69 dBFS; strums -3.37 dBFS; Tail plucks 2.46 s; strums 3.42 s; sweeps and S11: highest -0.09 dBFS (S11.corner-a0r0s1t1.plucks)."
        }
      ]
    },
    "factory.retrograde": {
      "name": "Retrograde",
      "family": "echoic",
      "declare": {
        "class": "attack",
        "self_oscillating": false,
        "needs_attacks": false
      },
      "status": "unrated",
      "rating": null,
      "relisten": [],
      "new_renders": [],
      "retired_renders": [],
      "history": [
        {
          "step": "v1",
          "text": "Record recipe. Failed Level -2.7/-2.5 LU and Response (Shape) +2.8 %."
        },
        {
          "step": "v2",
          "text": "wet_trim_db +1.5 dB; Shape variants on skew, sustain and smoothness measured 0.9-3.6 %."
        },
        {
          "step": "v3",
          "text": "Shape takes overlap 0.32 to 0.25 with sustain 0.6 to 0 and skew 0.5 to 0.95 (single swelled reversed chunks), Activity becomes spray and jitter. Combinations: s1 corners up to +5.7 dBFS (one voice, Repeats 0.85, no spray)."
        },
        {
          "step": "v4",
          "text": "Repeats maximum 0.7, a 10 ms spray floor: +2.2 dBFS."
        },
        {
          "step": "v5",
          "text": "wet_trim_db +0.8, Mix 0.5 to 0.4, Repeats maximum 0.6, Activity's spray 10-80 ms, Time's size 120-400 ms (from 500): passes, but the stored 400 ms chunk sat at Time's maximum."
        },
        {
          "step": "v6",
          "text": "Stored size 400 to 320 ms, Time at 0.71 (base_ms derives 45.7 ms): passes."
        },
        {
          "step": "pre-screen",
          "text": "Every check passes (sound revision 7, the committed package). Level plucks -1.7 LU; strums -2.0 LU; Engaged plucks +1.5 LU; strums +1.3 LU; Peak plucks -2.47 dBFS; strums -3.34 dBFS; Tail plucks 2.35 s; strums 2.79 s; sweeps and S11: highest -0.21 dBFS (S11.corner-a0r1s1t0.plucks)."
        }
      ]
    },
    "factory.runaway": {
      "name": "Runaway",
      "family": "reverie",
      "declare": {
        "class": "pad",
        "self_oscillating": true,
        "needs_attacks": false
      },
      "status": "unrated",
      "rating": null,
      "relisten": [],
      "new_renders": [],
      "retired_renders": [],
      "history": [
        {
          "step": "v1",
          "text": "Reserve. Record recipe (feedback 1.05, Repeats to 1.1), pad class, declared self-oscillating: Peak +2.04 dBFS, Level +2.7 LU, Engaged +4.4 LU on SoftNotes."
        },
        {
          "step": "v2",
          "text": "wet_trim_db -4."
        },
        {
          "step": "v4",
          "text": "wet_trim_db -5.5, Repeats maximum 1.08: Level -2.8 LU."
        },
        {
          "step": "v5",
          "text": "Mix 0.5 to 0.6, wet_trim_db -4.5: passes, the one pad-class mode."
        },
        {
          "step": "pre-screen",
          "text": "Every check passes (sound revision 7, the committed package). Level soft_notes -1.8 LU; Engaged soft_notes +1.0 LU; Peak soft_notes -1.51 dBFS; Tail soft_notes 8.61 s; sweeps and S11: highest -0.05 dBFS (S11.corner-a1r1s1t0.soft_notes)."
        }
      ]
    },
    "factory.shards": {
      "name": "Shards",
      "family": "misfire",
      "declare": {
        "class": "attack",
        "self_oscillating": false,
        "needs_attacks": true
      },
      "status": "unrated",
      "rating": null,
      "relisten": [],
      "new_renders": [],
      "retired_renders": [],
      "history": [
        {
          "step": "v1",
          "text": "Wave 1's sources {onset} only, mark positioning, bursts of 6 grains 35 ms apart, decay_ms 2000 ms: Level -15.7/-14.1 LU."
        },
        {
          "step": "v2",
          "text": "voice_count 3 (normalized as three voices, not six), wet_trim_db +10, post-delay echoes: Level -2.8/-2.0 LU."
        },
        {
          "step": "v4",
          "text": "Bursts of 8, wet_trim_db +9: Strums -6.0 LU (an onset every 2 s, against Plucks' two a second)."
        },
        {
          "step": "v5",
          "text": "Each attack streams 16 shards 120 ms apart at two voices with a 20 s decay, so dense playing steals and sparse playing streams: Plucks +1.6, Strums +0.7 LU; wet_trim_db +6, Mix 0.4. Activity's stored position was its maximum (16 shards)."
        },
        {
          "step": "v6",
          "text": "Activity anchored on spray (400 ms at 0.42), its burst count reaching 16 by 0.42: S11 at Deja Vu's positions +1.36 dBFS."
        },
        {
          "step": "v7",
          "text": "wet_trim_db +4.5, Mix 0.38: passes."
        },
        {
          "step": "pre-screen",
          "text": "Every check passes (sound revision 7, the committed package). Level plucks +0.1 LU; strums -0.8 LU; Engaged plucks +1.9 LU; strums +1.5 LU; Peak plucks -3.27 dBFS; strums -3.56 dBFS; Tail plucks 3.70 s; strums 3.00 s; sweeps and S11: highest -0.41 dBFS (S11.at-factory.deja-vu.plucks)."
        }
      ]
    },
    "factory.undertow": {
      "name": "Undertow",
      "family": "reverie",
      "declare": {
        "class": "attack",
        "self_oscillating": false,
        "needs_attacks": false
      },
      "status": "unrated",
      "rating": null,
      "relisten": [],
      "new_renders": [],
      "retired_renders": [],
      "history": [
        {
          "step": "v1",
          "text": "Wave 1's pitch set {0, -12}, random; pad class, wet_trim_db +1: Peak -0.78 dBFS on SoftNotes; Combinations up to +6.75 dBFS."
        },
        {
          "step": "v2",
          "text": "Attack class, wet_trim_db +2 (Level on Plucks -2.3 LU at +1)."
        },
        {
          "step": "v4",
          "text": "Activity's spray floor 20 ms (at Engram's positions spray 0 peaked +2.2 dBFS): passes."
        },
        {
          "step": "pre-screen",
          "text": "Every check passes (sound revision 7, the committed package). Level plucks -1.0 LU; strums -1.0 LU; Engaged plucks +2.5 LU; strums +2.4 LU; Peak plucks -3.33 dBFS; strums -3.66 dBFS; Tail plucks 2.26 s; strums 3.54 s; sweeps and S11: highest -0.72 dBFS (S11.corner-a0r1s1t0.plucks)."
        }
      ]
    },
    "factory.updraft": {
      "name": "Updraft",
      "family": "echoic",
      "declare": {
        "class": "attack",
        "self_oscillating": false,
        "needs_attacks": false
      },
      "status": "unrated",
      "rating": null,
      "relisten": [],
      "new_renders": [],
      "retired_renders": [],
      "history": [
        {
          "step": "v1",
          "text": "Record recipe. Failed Response (Shape): +1.3 %."
        },
        {
          "step": "v2-v4",
          "text": "Shapes on sustain, skew, size and smoothness reached 2.8 %; a single-voice swell passed alone (+8.4 %) but peaked at s1 corners (+2.5 dBFS); Repeats maximum 0.95 to 0.8. The octave's near guard keeps Shape's size under 190 ms."
        },
        {
          "step": "v5",
          "text": "wet_trim_db 0, Repeats maximum 0.75, Time's base_ms 250-1200 ms (from 150), Activity on spray, spread and jitter (overlap moves to Shape), Shape overlap 0.45 to 0.25 with sustain 0.35 to 0, skew 0.5 to 0.9 and size 120 to 190 ms: passes."
        },
        {
          "step": "pre-screen",
          "text": "Every check passes (sound revision 7, the committed package). Level plucks -1.7 LU; strums -1.5 LU; Engaged plucks +1.8 LU; strums +1.8 LU; Peak plucks -3.35 dBFS; strums -3.59 dBFS; Tail plucks 2.41 s; strums 3.93 s; sweeps and S11: highest -0.14 dBFS (S11.corner-a1r1s1t0.plucks)."
        }
      ]
    }
  },
  "notes": [
    {
      "step": "method",
      "text": "The 14 modes of design §11.1 and four reserves (Afterimage, Runaway, Downdraft and Recurrence, loops at {-12, 0}; the design's two other reserves stay unwritten) start from record §2.2's recipes (draft v2), written in schema 1 at sound revision 7 with wave 1 where §11.1 names it: pitch sets, repeat, decay_ms, voice_count, bursts and onset-only sources. Every targeted leaf is derived from its macro's stored position (bspc derive --solve, anchored on each macro's first target), so a few leaves differ from the record's typed values; the history names them. Each iteration ran the whole pre-screen (bspc render --script all --metrics) over the 14 as one set, so S10 and S11 cross the set, and over the reserves as a set of their own."
    },
    {
      "step": "input classes",
      "text": "Every mode is attack class (judged on Plucks and Strums) except Runaway (pad, declared self-oscillating); Shards is declared as needing attacks. The cloud modes (Murmuration, Halation, Undertow, Afterimage) and Lull began as pad modes. On SoftNotes (the dry at -12.1 LUFS, peaks at -7.2 dBFS) their wet is noise-like, with an 11-13 dB crest, so a wet within 2 LU of the dry (Level) puts the engaged output over -1 dBFS (Peak): both could not pass, and Lull's stored peak moved by up to 2.9 dB with one voice more or less. As attack modes they pass; their SoftNotes peaks are reported under Peak (other). Play them with the DI pad in the listening pass: a hot sustained input can clip the codec."
    },
    {
      "step": "tap modes",
      "text": "Engram, Callback and Pinhole replace the record's single rectangular 100 ms grain (transparent only while nothing moves) with four Hann grains a quarter-grain apart (overlap 0.39685, sustain 0, smoothness 1): a constant window sum, still a clean tap, but Activity's spray and jitter and Time's base_ms now crossfade instead of splicing. The compiler's example Engram scores 41.7x and 11.8x its static step on SoftNotes (Clicks); these score 0.8-1.2x."
    },
    {
      "step": "shape",
      "text": "The Response check wants Shape to move the brightness by 5 % or the envelope's variation by 1 dB from 0 to 1; window-only Shapes (sustain, skew) measured 0-4 % on Plucks once the dry plays at unity under the Mix law. Most modes now take Shape towards fewer, longer, swelled grains: one voice at 1 in the tap modes, Retrograde and Updraft, longer grains with a late peak in the cloud modes. Skew stops at 0.9: at 1.0 a grain ends on a step (37-174x on SoftNotes in S11's corners)."
    },
    {
      "step": "combinations",
      "text": "S11, the 16 corners of Activity x Repeats x Shape x Time and the other modes' stored positions, drove most iterations: one coherent voice at gain 1 (Shape's or Activity's low end) with Repeats at its maximum peaked over 0 dBFS. The fixes, per mode below: lower Repeats maxima, a floor of two or three voices, wet trims lower by 0.5-1.5 dB, a lower stored Mix."
    }
  ]
}
```
