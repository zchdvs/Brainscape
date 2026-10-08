# Factory audition log

The factory modes' ratings (docs/design/mode-compiler.md §11.3), written by
`tools/audition/ratings.py` (`declare`, `rate`, `carry`, `refresh`), never by hand: the
tables are generated from the data block at the end, which is the log. Ratings are keyed
by preset id and the S0-S11 render hashes they were made on; *Renders* names that set.
Knobs are rated 1-5.

| Preset | Family | Class | Status | sound_rev | sound_hash | Renders | Chord | Activity | Repeats | Shape | Time | Space | Filter | Level | Verdict | Notes |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| factory.afterimage | reverie | pad | unrated |  |  |  |  |  |  |  |  |  |  |  |  |  |
| factory.callback | echoic | attack | unrated |  |  |  |  |  |  |  |  |  |  |  |  |  |
| factory.deja-vu | reverie | attack | unrated |  |  |  |  |  |  |  |  |  |  |  |  |  |
| factory.downdraft | echoic | attack | unrated |  |  |  |  |  |  |  |  |  |  |  |  |  |
| factory.echolalia | reverie | attack | unrated |  |  |  |  |  |  |  |  |  |  |  |  |  |
| factory.engram | echoic | attack | unrated |  |  |  |  |  |  |  |  |  |  |  |  |  |
| factory.halation | reverie | pad | unrated |  |  |  |  |  |  |  |  |  |  |  |  |  |
| factory.kaleido | recall | attack | unrated |  |  |  |  |  |  |  |  |  |  |  |  |  |
| factory.lull | reverie | pad | unrated |  |  |  |  |  |  |  |  |  |  |  |  |  |
| factory.murmuration | reverie | pad | unrated |  |  |  |  |  |  |  |  |  |  |  |  |  |
| factory.pinhole | echoic | attack | unrated |  |  |  |  |  |  |  |  |  |  |  |  |  |
| factory.recurrence | recall | attack | unrated |  |  |  |  |  |  |  |  |  |  |  |  |  |
| factory.refrain | recall | attack | unrated |  |  |  |  |  |  |  |  |  |  |  |  |  |
| factory.retrograde | echoic | attack | unrated |  |  |  |  |  |  |  |  |  |  |  |  |  |
| factory.runaway | reverie | pad | unrated |  |  |  |  |  |  |  |  |  |  |  |  |  |
| factory.shards | misfire | attack, needs attacks | unrated |  |  |  |  |  |  |  |  |  |  |  |  |  |
| factory.undertow | reverie | pad | unrated |  |  |  |  |  |  |  |  |  |  |  |  |  |
| factory.updraft | echoic | attack | unrated |  |  |  |  |  |  |  |  |  |  |  |  |  |

## Authoring history

How each recipe came to be what the listening pass hears: where it started and every
pre-screen iteration since, with its reason (`ratings.py note`).

- **method**: The 14 modes of design §11.1 and four reserves (Afterimage, Runaway, Downdraft and Recurrence, loops at {-12, 0}; the design's two other reserves stay unwritten) start from record §2.2's recipes (draft v2), written in schema 1 at sound revision 7 with wave 1 where §11.1 names it: pitch sets, repeat, decay_ms, voice_count, bursts and onset-only sources. Every targeted leaf is derived from its macro's stored position (bspc derive --solve, anchored on each macro's first target), so a few leaves differ from the record's typed values; the history names them. Each iteration ran the whole pre-screen (bspc render --script all --metrics) over the 14 as one set, so S10 and S11 cross the set, and over the reserves as a set of their own.
- **input classes**: Lull and the cloud modes (Murmuration, Halation, Undertow, Afterimage) are pad class, judged on SoftNotes, as they began; Runaway is pad class too, and no longer declared self-oscillating (its tail ends: 8.4 s at the stored positions; S7 8.3 s at Repeats 0, 8.5 s at 1). The rest are attack class, judged on Plucks and Strums; Shards is declared as needing attacks. Peak at the stored positions is judged on the class inputs, and the other inputs' stored peaks are reported under Peak (other), never failed (the owner's decision, 2026-10-08: the review had judged Plucks, Strums and SoftNotes for every mode, so that a held chord into an attack mode would not clip either; the stored Mix it set stays). History: v2-v5 below moved the pad modes to attack class on the reading that Level and Peak could not both pass on SoftNotes, which hid their SoftNotes clipping (up to +1.39 dBFS in 7 of the 14). That reading was wrong: SoftNotes is not the hot input (its dry peaks at -7.2 dBFS, 2.7 dB under Plucks at -4.5 dBFS; it is louder only in loudness, because it sustains), and Level is measured at Mix 1, so a lower stored Mix lowers the stored peak without moving the Level. The review fixes (each mode's "review" step) set the stored Mix instead; this note replaces the one that read otherwise.
- **tap modes**: Engram, Callback and Pinhole replace the record's single rectangular 100 ms grain (transparent only while nothing moves) with four Hann grains a quarter-grain apart (overlap 0.39685, sustain 0, smoothness 1): a constant window sum, still a clean tap, but Activity's spray and jitter and Time's base_ms now crossfade instead of splicing. The compiler's example Engram scores 41.7x and 11.8x its static step on SoftNotes (Clicks); these score 0.8-1.2x.
- **shape**: The Response check wants Shape to move the brightness by 5 % or the envelope's variation by 1 dB from 0 to 1; window-only Shapes (sustain, skew) measured 0-4 % on Plucks once the dry plays at unity under the Mix law. Most modes now take Shape towards fewer, longer, swelled grains: one voice at 1 in the tap modes, Retrograde and Updraft, longer grains with a late peak in the cloud modes. Skew stops at 0.9: at 1.0 a grain ends on a step (37-174x on SoftNotes in S11's corners).
- **combinations**: S11, the 16 corners of Activity x Repeats x Shape x Time and the other modes' stored positions, drove most iterations: one coherent voice at gain 1 (Shape's or Activity's low end) with Repeats at its maximum peaked over 0 dBFS. The fixes, per mode below: lower Repeats maxima, a floor of two or three voices, wet trims lower by 0.5-1.5 dB, a lower stored Mix.
- **activity**: Response (Activity) is judged from the grains born per second (Engine::Stats()) with Activity stored at 0, 0.25, 0.5, 0.75 and 1 over the class input: monotonic within 5 % and changed by a quarter (tools/audition/README.md). Before the review fixes the echoic modes' Activity had no density term: Callback 40.0 per second at every rung, Retrograde 6.6-6.9 and not monotonic, Engram and Pinhole 40.0 falling to 35.4 with jitter, Updraft 48.6 falling to 42.3. The tap modes (Engram, Pinhole, Callback) and Updraft now add voices with Activity (layer0.voice_count: an integer, so the coherent tap's Hann sum stays constant at every step, where a fractional overlap ripples at the birth rate), with Shape's overlap starting high enough (0.63, sixteen voices, in the tap modes; 0.6 in Updraft; curve 0.5) that the voice count is what binds; Retrograde, whose reversed chunks are never coherent, takes the record's overlap term back and Shape thins its voice count.
- **shape, revisited**: Response (Shape) is measured on the engaged output over 2 s windows, against the reference at the same time: with the dry at unity it moves by up to about 5 % and 0.5 dB with the grains' random draws alone (Retrograde's unchanged Shape measured +9.2 % with an 11 ms spray floor and -5.9 % with 9 ms), and a lower stored Mix dilutes it. After the Activity and Mix changes Engram (-4.7 %, +0.42 dB), Updraft (+0.8 %, +0.75 dB), Retrograde, Downdraft, Recurrence and Afterimage fell under the threshold; Engram, Updraft, Retrograde and Recurrence take intermittency at Shape's top (to 0.4: births dropping out, so the swelled grains come with gaps), Downdraft from Shape 0.25 (to 0.5) and Afterimage reverse grains from Shape 0.3 (to 0.5, as Halation). Each passes by 1.7 dB or more of envelope or 10 % of brightness (the least: Downdraft +1.71 dB, Recurrence +10.3 %).
- **repeats**: Response (Repeats) is reported, not judged: S7's tail at Repeats 1 against its tail at 0, "listen" under 1.5 times. Undertow's Repeats moved grain feedback that its spray decorrelates on every pass (S7 tails 2.3 s at 0, 2.7 s at 1); it now moves the post delay's feedback (2.3 to 9.3 s). Named for the listening pass: Updraft 1.04x (its octave climb leaves the audio band), Refrain 1.26x, and the reserves Afterimage 1.00x, Runaway 1.02x, Downdraft 1.28x and Recurrence 1.24x.
- **owner, 2026-10-08**: The owner's answers from the listening page. (1) All 14 modes keep, pending the knob ratings, which the owner gives in the app's Curation view. (2) The pad modes' level at Mix 0.3-0.35 is about right. (3) An output safety limiter: yes, as a sound revision of its own, later; nothing here has one, and an attack mode's sweeps and S11 on SoftNotes still pass full scale (up to +2.5 dBFS, Echolalia's S11.corner-a1r0s1t0.soft_notes). (4) Of the review's stricter checks, Peak at the stored positions on every input is too strict: Peak is judged on the class inputs again (Plucks and Strums for an attack mode, SoftNotes for a pad mode), and the other inputs' stored peaks are reported under Peak (other), never as a failure; Activity judged on births and a weak Repeats flagged "listen" stay. No recipe or stored Mix changed (the owner listened at these values): the pre-screen re-run under the new Peak rule gives the same 1,488 render hashes, and all 18 pass. The other inputs' stored peaks sit at -1.11 to -7.18 dBFS, none over -1 dBFS (the highest Downdraft's SoftNotes -1.11, Updraft's -1.16 and Refrain's -1.19 dBFS). Each mode's pre-screen step below lists its stored peaks on all three inputs, as the review judged them; the numbers stand.

### `factory.afterimage` (Afterimage)

- **v1**: Reserve. Record recipe, pad class: Peak -0.56 dBFS and Level -2.3 LU on SoftNotes, Response (Shape) -4.2 %.
- **v2**: Attack class, wet_trim_db +2.
- **v3**: Mix 0.45 to 0.4, Shape size 30 to 400 ms with a late peak: Combinations' steps (skew 1.0).
- **v4**: Skew maximum 0.9: S11 corner +0.21 dBFS.
- **v5**: wet_trim_db +1.5: passes.
- **review**: Review fixes: pad class again; Mix 0.4 to 0.3 (SoftNotes -0.35 dBFS at the stored positions); Shape adds reverse grains 0 to 0.5 from Shape 0.3 (Response (Shape) -4.7 %, -0.68 dB on SoftNotes without it); Repeats maximum 0.85 to 0.65 (S11's a0r1s1t0 corner +0.50 dBFS on SoftNotes).
- **pre-screen**: Every check passes (sound revision 7, the committed package). Level soft_notes -0.8 LU; Engaged soft_notes +1.1 LU; Peak plucks -3.38 dBFS; strums -2.24 dBFS; soft_notes -1.66 dBFS; Tail soft_notes 1.94 s; sweeps and S11: highest -0.23 dBFS (S11.corner-a0r1s1t0.soft_notes); births per second at Activity 0 to 1: 60.3 116.0 191.4 298.8 446.2. For listening: Repeats barely lengthens the tail (S7 1.94 s against 1.94 s (1.00x)).

### `factory.callback` (Callback)

- **v1**: Record recipe with the tap grain stage, so Time's base_ms moves by crossfades. Failed Response (Shape): -1.4 %.
- **v2**: Shape as Engram's v2: passes Shape.
- **v3**: Shape as Engram's v3 (skew 0.95, size 300 ms): Combinations' s1t1 corners up to +2.41 dBFS.
- **v4**: wet_trim_db -1.5 dB, Repeats maximum 0.85 to 0.75: +1.25 dBFS.
- **v5**: Mix 0.4 to 0.35, Repeats 0.15-0.65, Time's base_ms 60-800 ms and delay 90-1200 ms (from 1000 and 1500), Activity's delay mix maximum 0.75 to 0.65 (ten trials: a lower trim or a two-voice Shape floor lost the Shape response, 4.1-4.7 %): passes. Activity's grain feedback (in_range 0.6-1) derives 0.11 at the stored position, not the record's 0.
- **review**: Review fixes: Activity adds voices (1 to 5, the stored 0.75 still four: births 10 to 50 per second) and, above the stored position, spray 0 to 20 ms, so it also thickens the tap, not only the post delay's answers (the reviewed Activity moved the wet by 0.4 LU and 1.1 dB of spectrum). Shape's overlap starts at 0.63 as in the other tap modes.
- **pre-screen**: Every check passes (sound revision 7, the committed package). Level plucks -1.3 LU; strums -1.3 LU; Engaged plucks +1.3 LU; strums +1.1 LU; Peak plucks -2.94 dBFS; strums -3.67 dBFS; soft_notes -1.74 dBFS; Tail plucks 3.24 s; strums 3.12 s; sweeps and S11: highest -0.61 dBFS (S11.corner-a1r0s1t1.plucks); births per second at Activity 0 to 1: 10.0 20.0 30.0 40.0 50.0.

### `factory.deja-vu` (Déjà Vu)

- **v1**: Wave 1's decay_ms 3300 ms (Repeats 400-12000 ms) and bursts of 3 grains 9 ms apart; wet_trim_db -3: Level -7.1/-6.5 LU.
- **v2**: wet_trim_db +4: Combinations' r1 corners +0.11 dBFS.
- **v4**: wet_trim_db +3.5: passes.
- **review**: Review fixes: Mix 0.5 to 0.3 (SoftNotes +1.39 dBFS at the stored positions, 423 samples over full scale).
- **pre-screen**: Every check passes (sound revision 7, the committed package). Level plucks -0.6 LU; strums +0.0 LU; Engaged plucks +1.1 LU; strums +1.2 LU; Peak plucks -3.56 dBFS; strums -3.63 dBFS; soft_notes -1.62 dBFS; Tail plucks 2.90 s; strums 2.33 s; sweeps and S11: highest -1.89 dBFS (S11.corner-a0r1s1t0.plucks); births per second at Activity 0 to 1: 14.4 27.2 48.8 82.0 125.4.

### `factory.downdraft` (Downdraft)

- **v1**: Reserve: Updraft at -12. Level -3.0/-2.8 LU, Response (Shape) -2.1 %.
- **v2**: wet_trim_db +1.5.
- **v3**: Shape size 60 to 400 ms with skew 0.3 to 1.0: steps in S11's corners (skew 1.0).
- **v4**: Skew maximum 0.9, Repeats maximum 0.8: passes.
- **review**: Review fixes: Mix 0.45 to 0.3 (SoftNotes +0.87 dBFS at the stored positions); Shape adds intermittency 0 to 0.5 from Shape 0.25 (Response (Shape) -4.4 %, +0.93 dB without it).
- **pre-screen**: Every check passes (sound revision 7, the committed package). Level plucks -1.7 LU; strums -1.5 LU; Engaged plucks +0.8 LU; strums +0.7 LU; Peak plucks -3.32 dBFS; strums -3.47 dBFS; soft_notes -1.11 dBFS; Tail plucks 3.17 s; strums 3.87 s; sweeps and S11: highest -2.54 dBFS (S6.filter.plucks); births per second at Activity 0 to 1: 22.7 37.4 56.1 80.9 112.6. For listening: Repeats barely lengthens the tail (S7 3.67 s against 2.87 s (1.28x)).

### `factory.echolalia` (Echolalia)

- **v1**: Wave 1's decay_ms 3300 ms, which Repeats moves (400-12000 ms) in place of feedback; wet_trim_db -3: Level -4.0/-3.6 LU, Response (Shape) +1.4 %.
- **v2**: wet_trim_db +1; Shape adds smoothness and takes sustain to 0: Shape passes (+15.4 %); Peak +0.12 dBFS.
- **v3**: Mix 0.5 to 0.4, wet_trim_db 0: passes.
- **pre-screen**: Every check passes (sound revision 7, the committed package). Level plucks -0.9 LU; strums -0.5 LU; Engaged plucks +1.6 LU; strums +1.8 LU; Peak plucks -1.23 dBFS; strums -1.29 dBFS; soft_notes -1.79 dBFS; Tail plucks 2.79 s; strums 1.79 s; sweeps and S11: highest -0.57 dBFS (S11.at-factory.retrograde.plucks); births per second at Activity 0 to 1: 6.1 10.9 20.2 30.7 41.9.

### `factory.engram` (Engram)

- **v1**: Record v2 (its repeats on the post delay's exact taps) with the tap grain stage of the set note; Activity also moved overlap. Failed only Response (Shape): -1.0 %, -0.01 dB: four overlapping windows change nothing the mix shows at Mix 0.35.
- **v2**: Activity loses overlap (spray, jitter, spread); Shape takes overlap 0.39685 to 0.25 (four voices to one), skew 0.5 to 0.85, size 100 to 220 ms: -3.4 %, +0.51 dB.
- **v3**: Shape's skew to 0.95 and size to 300 ms, single-voice swells (-8.0 %); base_ms 1 to 2 ms, since Activity's spread at 300 ms needs 1.39 ms (lint L2). Combinations: corner a0r1s1t1 +1.24 dBFS (one voice at gain 1, Repeats 0.9).
- **v4**: Repeats maximum 0.9 to 0.8: +1.00 dBFS.
- **v5**: wet_trim_db -1.5 dB, Repeats maximum 0.75: passes.
- **review**: Review fixes: Activity adds voices (voice_count 4 to 16: births 40.0 to 147.2 per second) beside spray, jitter and spread; Shape's overlap starts at 0.63 (curve 0.5) so the voice count binds, and Shape adds intermittency 0 to 0.4 (Response (Shape) -4.7 %, +0.42 dB without it; +2.46 dB with). The stored sound is the same four Hann voices.
- **pre-screen**: Every check passes (sound revision 7, the committed package). Level plucks -1.5 LU; strums -1.1 LU; Engaged plucks +1.2 LU; strums +1.2 LU; Peak plucks -3.60 dBFS; strums -3.37 dBFS; soft_notes -1.91 dBFS; Tail plucks 2.90 s; strums 2.65 s; sweeps and S11: highest -0.68 dBFS (S11.corner-a1r1s1t0.plucks); births per second at Activity 0 to 1: 40.0 68.1 95.4 122.0 147.2.

### `factory.halation` (Halation)

- **v1**: Wave 1's pitch set {0, +12}, random; pad class, wet_trim_db +2: Peak +1.12 dBFS on SoftNotes; Combinations up to +6.6 dBFS (at Pinhole's positions Activity 0 left spray at 0: coherent feedback).
- **v2**: Attack class.
- **v4**: wet_trim_db +1.5, Activity's spray floor 20 ms, Shape on size, skew and sustain: Response (Shape) -3.7 %.
- **v5**: base_ms 450 to 520 ms and Time 520-1500 ms, so Shape's size reaches 500 ms inside the octave's near guard (L2); Shape adds reverse 0 to 0.6 from position 0.3: passes. Sustain derives 0.145 at the stored Shape (record 0.3).
- **v6**: At Retrograde's new positions S11 peaked +1.10 dBFS.
- **v7**: Mix 0.5 to 0.45, wet_trim_db +0.5: passes.
- **review**: Review fixes: pad class again; Mix 0.45 to 0.3 (SoftNotes +0.48 dBFS). S11 at Pinhole's positions on SoftNotes is the set's tightest margin: -0.01 dBFS.
- **pre-screen**: Every check passes (sound revision 7, the committed package). Level soft_notes -1.1 LU; Engaged soft_notes +0.9 LU; Peak plucks -3.67 dBFS; strums -3.57 dBFS; soft_notes -1.61 dBFS; Tail soft_notes 3.74 s; sweeps and S11: highest -0.01 dBFS (S11.at-factory.pinhole.soft_notes); births per second at Activity 0 to 1: 21.1 37.7 62.6 97.1 138.2.

### `factory.kaleido` (Kaleido)

- **v1**: Mark positioning with wave 1's set {+12 x2, 0} (cycle), repeat 3 and decay_ms 4000 ms; base_ms 300 to 450 ms so the octave's grains at Time's 400 ms clear the near guard (L2); wet_trim_db 0: Level -6.8/-6.4 LU.
- **v2**: wet_trim_db +6.5: one-voice corners (Activity 0) up to +4.2 dBFS.
- **v4**: Activity's floor overlap 0.32 (two voices): +0.7 dBFS.
- **v5**: wet_trim_db +5.5: passes.
- **review**: Review fixes: Mix 0.5 to 0.3 (SoftNotes +1.07 dBFS at the stored positions).
- **pre-screen**: Every check passes (sound revision 7, the committed package). Level plucks -1.3 LU; strums -0.9 LU; Engaged plucks +0.9 LU; strums +1.0 LU; Peak plucks -3.87 dBFS; strums -2.99 dBFS; soft_notes -1.49 dBFS; Tail plucks 3.58 s; strums 2.67 s; sweeps and S11: highest -1.96 dBFS (S11.corner-a0r1s0t0.plucks); births per second at Activity 0 to 1: 2.7 5.2 7.4 10.5 14.1.

### `factory.lull` (Lull)

- **v1**: Record recipe, pad class: Peak +0.57 dBFS on SoftNotes; the Repeats sweep +2.56 dBFS (feedback to 1.06).
- **v2-v4**: Mix 0.6, wet_trim_db -1 then -2 dB, Repeats maximum 0.99 then 0.97: Peak -0.43 dBFS at -1 dB, Level -2.9 LU at -2 dB.
- **trials**: Thirteen pad variants (Mix 0.6-0.8, fewer voices, less spray, feedback 0.9): the stored peak ranged from -1.35 to +1.53 dBFS, so none held Peak and Level with margin.
- **v5**: Attack class; feedback 0.95 to 0.9, overlap 0.6 to 0.45, spray 150 to 60 ms, spread 10 to 5 cents, jitter 0.5 to 0.2, Mix 0.6, wet_trim_db +0.5, Repeats 0.7-0.97, Shape size 40 to 500 ms with skew 0.1 to 0.6 (+6.6 % on Plucks): passes. Repeats stays under 1, so the drone's tail is finite (not declared self-oscillating).
- **review**: Review fixes: pad class again; Mix 0.6 to 0.35 (SoftNotes +0.21 dBFS at the stored positions). Repeats 0.4 to 1.0 (from 0.7 to 0.97; curve 0.8, the stored 0.9 at 0.80): a single pluck's wet falls 60 dB in 8.5 s at 0, 11 s stored and 14.5 s at 1 (9.5 to 13.5 s before), S7's tails on SoftNotes 7.1 to 13.1 s. The reverb holds the short end long, so Repeats lengthens a wash rather than turning a bloom into a drone; the description now says a wash. How distinct Lull is from Murmuration is for the listening pass.
- **pre-screen**: Every check passes (sound revision 7, the committed package). Level soft_notes +0.0 LU; Engaged soft_notes +1.7 LU; Peak plucks -2.74 dBFS; strums -3.59 dBFS; soft_notes -1.07 dBFS; Tail soft_notes 10.13 s (60 s probe); sweeps and S11: highest -0.09 dBFS (S11.at-factory.callback.soft_notes); births per second at Activity 0 to 1: 5.9 12.9 23.3 38.2 58.7.

### `factory.murmuration` (Murmuration)

- **v1**: Record recipe, pad class, wet_trim_db +2: Peak +1.44 dBFS on SoftNotes, Combinations, Response (Shape) +4.4 %.
- **v2**: Attack class (set note).
- **v3**: Shape size 60 to 500 ms, skew 0.4 to 1.0, sustain 0.2 to 0 (-9.2 %); Combinations: steps at s1 corners, 37-50x (skew 1.0).
- **v4**: Skew maximum 0.9: passes.
- **review**: Review fixes: pad class again; Mix 0.55 to 0.3 (SoftNotes peaked +1.38 dBFS at the stored positions, 61 samples over full scale).
- **pre-screen**: Every check passes (sound revision 7, the committed package). Level soft_notes -0.9 LU; Engaged soft_notes +1.1 LU; Peak plucks -3.22 dBFS; strums -3.73 dBFS; soft_notes -1.17 dBFS; Tail soft_notes 4.46 s; sweeps and S11: highest -0.31 dBFS (S11.at-factory.pinhole.soft_notes); births per second at Activity 0 to 1: 66.0 112.7 168.2 259.6 360.7.

### `factory.pinhole` (Pinhole)

- **v1**: Record recipe with the tap grain stage. Combinations: corner a1r1s0t0 +10.0 dBFS with an unending tail (Repeats 0.95 into the resonant band); S6's Filter sweep over 0 dBFS at the bypass end; Response (Shape) -0.3 %.
- **v2**: wet_trim_db -1.5 dB, Repeats maximum 0.85, Shape as Engram's: Shape passes.
- **v4**: Repeats maximum 0.7, resonance 0.55 to 0.5: corner a0r1s1t1 +4.4 dBFS.
- **v5**: Mix 0.5 to 0.4, Repeats maximum 0.6: passes.
- **review**: Review fixes: Activity and Shape as Engram's tap stage (voices 4 to 16; overlap from 0.63), without the intermittency. Repeats' maximum 0.6 stays above the stored feedback, now 0.5 at position 0.83: at 0.6 the stored position was the maximum, so turning Repeats up changed nothing (stored and Repeats 1 rendered the same bits), and a 0.7 maximum put S11's a0r1s1t1 corner at +2.49 dBFS. wet_trim_db -1.5 to -0.7 dB for the Level the lower feedback took (-2.5/-2.3 LU at -1.5). A single pluck's wet now falls 60 dB in 4.5 s stored and 5.5 s at Repeats 1.
- **pre-screen**: Every check passes (sound revision 7, the committed package). Level plucks -1.7 LU; strums -1.5 LU; Engaged plucks +1.4 LU; strums +1.3 LU; Peak plucks -2.44 dBFS; strums -3.29 dBFS; soft_notes -3.92 dBFS; Tail plucks 3.09 s; strums 2.93 s; sweeps and S11: highest -0.56 dBFS (S11.corner-a0r1s1t1.plucks); births per second at Activity 0 to 1: 40.0 68.1 95.4 122.0 147.2.

### `factory.recurrence` (Recurrence)

- **v1**: Reserve: Refrain's loops at {-12, 0}. Combinations up to +3.7 dBFS, Response (Shape) -2.8 %.
- **v4**: Activity's floor two voices, wet_trim_db +3: S11 corners up to +0.79 dBFS.
- **v6**: Three-voice floor, wet_trim_db +2, Shape sustain 0.5 to 0 with skew 0.5 to 0.15: passes.
- **review**: Review fixes: Mix 0.5 to 0.35 (SoftNotes +0.56 dBFS at the stored positions); Shape swells the loops as Refrain's does (skew 0.5 to 0.9) with intermittency 0 to 0.4 (the 0.15 skew measured -3.6 %, -0.20 dB at the lower Mix).
- **pre-screen**: Every check passes (sound revision 7, the committed package). Level plucks -1.9 LU; strums -1.8 LU; Engaged plucks +1.1 LU; strums +1.1 LU; Peak plucks -2.93 dBFS; strums -3.44 dBFS; soft_notes -1.26 dBFS; Tail plucks 2.38 s; strums 3.04 s; sweeps and S11: highest -2.36 dBFS (S4.time.plucks); births per second at Activity 0 to 1: 5.9 8.1 12.0 14.0 16.0. For listening: Repeats barely lengthens the tail (S7 2.74 s against 2.21 s (1.24x)).

### `factory.refrain` (Refrain)

- **v1**: Wave 1's set {0, +12} (cycle), repeat 4 and voice_count 4; base_ms 420 ms (L2); wet_trim_db +5: Level +2.0/+3.1 LU, Engaged +4.1/+4.6 LU, Combinations.
- **v2**: wet_trim_db +3: Response (Shape) +4.8 %.
- **v4**: wet_trim_db +2.5, Activity's floor two voices, Shape sustain 0.8 to 0 and skew 0.1 to 0.9: Shape +0.6 %.
- **v5**: wet_trim_db +2, Shape sustain 0.5 to 0 with skew 0.5 to 0.9 (the loops swell): passes.
- **review**: Review fixes: Mix 0.5 to 0.35 (SoftNotes +0.73 dBFS at the stored positions).
- **pre-screen**: Every check passes (sound revision 7, the committed package). Level plucks -1.0 LU; strums +0.1 LU; Engaged plucks +1.3 LU; strums +1.5 LU; Peak plucks -3.22 dBFS; strums -3.44 dBFS; soft_notes -1.19 dBFS; Tail plucks 2.33 s; strums 3.11 s; sweeps and S11: highest -1.42 dBFS (S11.at-factory.echolalia.plucks); births per second at Activity 0 to 1: 4.0 8.1 9.7 14.0 16.0. For listening: Repeats barely lengthens the tail (S7 2.70 s against 2.15 s (1.26x)).

### `factory.retrograde` (Retrograde)

- **v1**: Record recipe. Failed Level -2.7/-2.5 LU and Response (Shape) +2.8 %.
- **v2**: wet_trim_db +1.5 dB; Shape variants on skew, sustain and smoothness measured 0.9-3.6 %.
- **v3**: Shape takes overlap 0.32 to 0.25 with sustain 0.6 to 0 and skew 0.5 to 0.95 (single swelled reversed chunks), Activity becomes spray and jitter. Combinations: s1 corners up to +5.7 dBFS (one voice, Repeats 0.85, no spray).
- **v4**: Repeats maximum 0.7, a 10 ms spray floor: +2.2 dBFS.
- **v5**: wet_trim_db +0.8, Mix 0.5 to 0.4, Repeats maximum 0.6, Activity's spray 10-80 ms, Time's size 120-400 ms (from 500): passes, but the stored 400 ms chunk sat at Time's maximum.
- **v6**: Stored size 400 to 320 ms, Time at 0.71 (base_ms derives 45.7 ms): passes.
- **review**: Review fixes: Activity takes the record's overlap term back (0.32 to 0.5: births 6.6 to 22.2 per second); Shape thins voice_count 8 to 1 (curve 0.3) in place of overlap and adds intermittency 0 to 0.4. With voice_count alone Shape measured -4.7 to -2.1 % and +0.58 to +1.02 dB across four spray floors (three under the threshold); with intermittency +1.6 to +2.1 dB on all four. The stored sound is unchanged (overlap 0.32, 2.1 voices).
- **pre-screen**: Every check passes (sound revision 7, the committed package). Level plucks -1.7 LU; strums -2.0 LU; Engaged plucks +1.5 LU; strums +1.3 LU; Peak plucks -2.47 dBFS; strums -3.34 dBFS; soft_notes -1.58 dBFS; Tail plucks 2.35 s; strums 2.79 s; sweeps and S11: highest -0.78 dBFS (S11.corner-a0r1s1t0.plucks); births per second at Activity 0 to 1: 6.6 9.9 14.1 18.7 22.2.

### `factory.runaway` (Runaway)

- **v1**: Reserve. Record recipe (feedback 1.05, Repeats to 1.1), pad class, declared self-oscillating: Peak +2.04 dBFS, Level +2.7 LU, Engaged +4.4 LU on SoftNotes.
- **v2**: wet_trim_db -4.
- **v4**: wet_trim_db -5.5, Repeats maximum 1.08: Level -2.8 LU.
- **v5**: Mix 0.5 to 0.6, wet_trim_db -4.5: passes, the one pad-class mode.
- **review**: Review fixes: no longer declared self-oscillating: at the stored positions and at Repeats 1 it falls about 7 dB per second once the input stops (Tail 8.4 s), so the description no longer says endless. Activity adds overlap 0.47 to 0.67 (births 31.2 to 93.4 per second; it moved none), the stored 0.5 kept; Mix 0.6 to 0.45 (S11's a0r1s1t0 corner +0.10 dBFS). Repeats still changes little at its top (feedback 1.05 to 1.08).
- **pre-screen**: Every check passes (sound revision 7, the committed package). Level soft_notes -1.8 LU; Engaged soft_notes +1.7 LU; Peak plucks -3.79 dBFS; strums -3.81 dBFS; soft_notes -1.15 dBFS; Tail soft_notes 8.44 s; sweeps and S11: highest -0.14 dBFS (S11.corner-a1r1s1t0.soft_notes); births per second at Activity 0 to 1: 31.2 41.8 56.3 73.1 93.4. For listening: Repeats barely lengthens the tail (S7 8.49 s against 8.32 s (1.02x)).

### `factory.shards` (Shards)

- **v1**: Wave 1's sources {onset} only, mark positioning, bursts of 6 grains 35 ms apart, decay_ms 2000 ms: Level -15.7/-14.1 LU.
- **v2**: voice_count 3 (normalized as three voices, not six), wet_trim_db +10, post-delay echoes: Level -2.8/-2.0 LU.
- **v4**: Bursts of 8, wet_trim_db +9: Strums -6.0 LU (an onset every 2 s, against Plucks' two a second).
- **v5**: Each attack streams 16 shards 120 ms apart at two voices with a 20 s decay, so dense playing steals and sparse playing streams: Plucks +1.6, Strums +0.7 LU; wet_trim_db +6, Mix 0.4. Activity's stored position was its maximum (16 shards).
- **v6**: Activity anchored on spray (400 ms at 0.42), its burst count reaching 16 by 0.42: S11 at Deja Vu's positions +1.36 dBFS.
- **v7**: wet_trim_db +4.5, Mix 0.38: passes.
- **review**: Review fixes: the description says what plays: half the shards reversed (reverse_prob 0.5, the record's), not all.
- **pre-screen**: Every check passes (sound revision 7, the committed package). Level plucks +0.1 LU; strums -0.8 LU; Engaged plucks +1.9 LU; strums +1.5 LU; Peak plucks -3.27 dBFS; strums -3.56 dBFS; soft_notes -7.18 dBFS; Tail plucks 3.70 s; strums 3.00 s; sweeps and S11: highest -0.41 dBFS (S11.at-factory.deja-vu.plucks); births per second at Activity 0 to 1: 8.0 21.1 29.8 29.8 29.8.

### `factory.undertow` (Undertow)

- **v1**: Wave 1's pitch set {0, -12}, random; pad class, wet_trim_db +1: Peak -0.78 dBFS on SoftNotes; Combinations up to +6.75 dBFS.
- **v2**: Attack class, wet_trim_db +2 (Level on Plucks -2.3 LU at +1).
- **v4**: Activity's spray floor 20 ms (at Engram's positions spray 0 peaked +2.2 dBFS): passes.
- **review**: Review fixes: pad class again; Mix 0.5 to 0.3 (SoftNotes +0.14 dBFS at the stored positions; at 0.35 S11's corners on SoftNotes reached +0.36 dBFS). Repeats moves the post delay's feedback (0.15 to 0.75; its mix 0.35, 420 ms) in place of the grain feedback its spray decorrelated, which stays at 0.3: S7's tails 2.3 to 9.3 s (from 2.3 to 2.7 s); a single pluck's wet falls 60 dB in 4 s at 0 and stored, 10 s at Repeats 1.
- **pre-screen**: Every check passes (sound revision 7, the committed package). Level soft_notes -0.8 LU; Engaged soft_notes +1.1 LU; Peak plucks -3.52 dBFS; strums -3.54 dBFS; soft_notes -1.87 dBFS; Tail soft_notes 2.56 s; sweeps and S11: highest -0.37 dBFS (S11.corner-a0r1s0t1.soft_notes); births per second at Activity 0 to 1: 18.2 31.5 52.6 79.3 115.2.

### `factory.updraft` (Updraft)

- **v1**: Record recipe. Failed Response (Shape): +1.3 %.
- **v2-v4**: Shapes on sustain, skew, size and smoothness reached 2.8 %; a single-voice swell passed alone (+8.4 %) but peaked at s1 corners (+2.5 dBFS); Repeats maximum 0.95 to 0.8. The octave's near guard keeps Shape's size under 190 ms.
- **v5**: wet_trim_db 0, Repeats maximum 0.75, Time's base_ms 250-1200 ms (from 150), Activity on spray, spread and jitter (overlap moves to Shape), Shape overlap 0.45 to 0.25 with sustain 0.35 to 0, skew 0.5 to 0.9 and size 120 to 190 ms: passes.
- **review**: Review fixes: Activity adds voices (3 to 12, six at the stored 0.32 where 5.8 played: births 25.0 to 89.1 per second); Shape's overlap starts at 0.6 (curve 0.5) and adds intermittency 0 to 0.4 (+0.8 %, +0.75 dB without it); Mix 0.45 to 0.4 (SoftNotes peaked -0.61 dBFS at the stored positions).
- **pre-screen**: Every check passes (sound revision 7, the committed package). Level plucks -1.7 LU; strums -1.5 LU; Engaged plucks +1.5 LU; strums +1.4 LU; Peak plucks -3.39 dBFS; strums -3.63 dBFS; soft_notes -1.16 dBFS; Tail plucks 2.44 s; strums 3.80 s; sweeps and S11: highest -0.69 dBFS (S11.corner-a1r1s1t0.plucks); births per second at Activity 0 to 1: 25.0 40.5 63.2 76.7 89.1. For listening: Repeats barely lengthens the tail (S7 2.48 s against 2.39 s (1.04x)).

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
        "class": "pad",
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
          "step": "review",
          "text": "Review fixes: pad class again; Mix 0.4 to 0.3 (SoftNotes -0.35 dBFS at the stored positions); Shape adds reverse grains 0 to 0.5 from Shape 0.3 (Response (Shape) -4.7 %, -0.68 dB on SoftNotes without it); Repeats maximum 0.85 to 0.65 (S11's a0r1s1t0 corner +0.50 dBFS on SoftNotes)."
        },
        {
          "step": "pre-screen",
          "text": "Every check passes (sound revision 7, the committed package). Level soft_notes -0.8 LU; Engaged soft_notes +1.1 LU; Peak plucks -3.38 dBFS; strums -2.24 dBFS; soft_notes -1.66 dBFS; Tail soft_notes 1.94 s; sweeps and S11: highest -0.23 dBFS (S11.corner-a0r1s1t0.soft_notes); births per second at Activity 0 to 1: 60.3 116.0 191.4 298.8 446.2. For listening: Repeats barely lengthens the tail (S7 1.94 s against 1.94 s (1.00x))."
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
          "step": "review",
          "text": "Review fixes: Activity adds voices (1 to 5, the stored 0.75 still four: births 10 to 50 per second) and, above the stored position, spray 0 to 20 ms, so it also thickens the tap, not only the post delay's answers (the reviewed Activity moved the wet by 0.4 LU and 1.1 dB of spectrum). Shape's overlap starts at 0.63 as in the other tap modes."
        },
        {
          "step": "pre-screen",
          "text": "Every check passes (sound revision 7, the committed package). Level plucks -1.3 LU; strums -1.3 LU; Engaged plucks +1.3 LU; strums +1.1 LU; Peak plucks -2.94 dBFS; strums -3.67 dBFS; soft_notes -1.74 dBFS; Tail plucks 3.24 s; strums 3.12 s; sweeps and S11: highest -0.61 dBFS (S11.corner-a1r0s1t1.plucks); births per second at Activity 0 to 1: 10.0 20.0 30.0 40.0 50.0."
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
          "step": "review",
          "text": "Review fixes: Mix 0.5 to 0.3 (SoftNotes +1.39 dBFS at the stored positions, 423 samples over full scale)."
        },
        {
          "step": "pre-screen",
          "text": "Every check passes (sound revision 7, the committed package). Level plucks -0.6 LU; strums +0.0 LU; Engaged plucks +1.1 LU; strums +1.2 LU; Peak plucks -3.56 dBFS; strums -3.63 dBFS; soft_notes -1.62 dBFS; Tail plucks 2.90 s; strums 2.33 s; sweeps and S11: highest -1.89 dBFS (S11.corner-a0r1s1t0.plucks); births per second at Activity 0 to 1: 14.4 27.2 48.8 82.0 125.4."
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
          "step": "review",
          "text": "Review fixes: Mix 0.45 to 0.3 (SoftNotes +0.87 dBFS at the stored positions); Shape adds intermittency 0 to 0.5 from Shape 0.25 (Response (Shape) -4.4 %, +0.93 dB without it)."
        },
        {
          "step": "pre-screen",
          "text": "Every check passes (sound revision 7, the committed package). Level plucks -1.7 LU; strums -1.5 LU; Engaged plucks +0.8 LU; strums +0.7 LU; Peak plucks -3.32 dBFS; strums -3.47 dBFS; soft_notes -1.11 dBFS; Tail plucks 3.17 s; strums 3.87 s; sweeps and S11: highest -2.54 dBFS (S6.filter.plucks); births per second at Activity 0 to 1: 22.7 37.4 56.1 80.9 112.6. For listening: Repeats barely lengthens the tail (S7 3.67 s against 2.87 s (1.28x))."
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
          "text": "Every check passes (sound revision 7, the committed package). Level plucks -0.9 LU; strums -0.5 LU; Engaged plucks +1.6 LU; strums +1.8 LU; Peak plucks -1.23 dBFS; strums -1.29 dBFS; soft_notes -1.79 dBFS; Tail plucks 2.79 s; strums 1.79 s; sweeps and S11: highest -0.57 dBFS (S11.at-factory.retrograde.plucks); births per second at Activity 0 to 1: 6.1 10.9 20.2 30.7 41.9."
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
          "step": "review",
          "text": "Review fixes: Activity adds voices (voice_count 4 to 16: births 40.0 to 147.2 per second) beside spray, jitter and spread; Shape's overlap starts at 0.63 (curve 0.5) so the voice count binds, and Shape adds intermittency 0 to 0.4 (Response (Shape) -4.7 %, +0.42 dB without it; +2.46 dB with). The stored sound is the same four Hann voices."
        },
        {
          "step": "pre-screen",
          "text": "Every check passes (sound revision 7, the committed package). Level plucks -1.5 LU; strums -1.1 LU; Engaged plucks +1.2 LU; strums +1.2 LU; Peak plucks -3.60 dBFS; strums -3.37 dBFS; soft_notes -1.91 dBFS; Tail plucks 2.90 s; strums 2.65 s; sweeps and S11: highest -0.68 dBFS (S11.corner-a1r1s1t0.plucks); births per second at Activity 0 to 1: 40.0 68.1 95.4 122.0 147.2."
        }
      ]
    },
    "factory.halation": {
      "name": "Halation",
      "family": "reverie",
      "declare": {
        "class": "pad",
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
          "step": "review",
          "text": "Review fixes: pad class again; Mix 0.45 to 0.3 (SoftNotes +0.48 dBFS). S11 at Pinhole's positions on SoftNotes is the set's tightest margin: -0.01 dBFS."
        },
        {
          "step": "pre-screen",
          "text": "Every check passes (sound revision 7, the committed package). Level soft_notes -1.1 LU; Engaged soft_notes +0.9 LU; Peak plucks -3.67 dBFS; strums -3.57 dBFS; soft_notes -1.61 dBFS; Tail soft_notes 3.74 s; sweeps and S11: highest -0.01 dBFS (S11.at-factory.pinhole.soft_notes); births per second at Activity 0 to 1: 21.1 37.7 62.6 97.1 138.2."
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
          "step": "review",
          "text": "Review fixes: Mix 0.5 to 0.3 (SoftNotes +1.07 dBFS at the stored positions)."
        },
        {
          "step": "pre-screen",
          "text": "Every check passes (sound revision 7, the committed package). Level plucks -1.3 LU; strums -0.9 LU; Engaged plucks +0.9 LU; strums +1.0 LU; Peak plucks -3.87 dBFS; strums -2.99 dBFS; soft_notes -1.49 dBFS; Tail plucks 3.58 s; strums 2.67 s; sweeps and S11: highest -1.96 dBFS (S11.corner-a0r1s0t0.plucks); births per second at Activity 0 to 1: 2.7 5.2 7.4 10.5 14.1."
        }
      ]
    },
    "factory.lull": {
      "name": "Lull",
      "family": "reverie",
      "declare": {
        "class": "pad",
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
          "step": "review",
          "text": "Review fixes: pad class again; Mix 0.6 to 0.35 (SoftNotes +0.21 dBFS at the stored positions). Repeats 0.4 to 1.0 (from 0.7 to 0.97; curve 0.8, the stored 0.9 at 0.80): a single pluck's wet falls 60 dB in 8.5 s at 0, 11 s stored and 14.5 s at 1 (9.5 to 13.5 s before), S7's tails on SoftNotes 7.1 to 13.1 s. The reverb holds the short end long, so Repeats lengthens a wash rather than turning a bloom into a drone; the description now says a wash. How distinct Lull is from Murmuration is for the listening pass."
        },
        {
          "step": "pre-screen",
          "text": "Every check passes (sound revision 7, the committed package). Level soft_notes +0.0 LU; Engaged soft_notes +1.7 LU; Peak plucks -2.74 dBFS; strums -3.59 dBFS; soft_notes -1.07 dBFS; Tail soft_notes 10.13 s (60 s probe); sweeps and S11: highest -0.09 dBFS (S11.at-factory.callback.soft_notes); births per second at Activity 0 to 1: 5.9 12.9 23.3 38.2 58.7."
        }
      ]
    },
    "factory.murmuration": {
      "name": "Murmuration",
      "family": "reverie",
      "declare": {
        "class": "pad",
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
          "step": "review",
          "text": "Review fixes: pad class again; Mix 0.55 to 0.3 (SoftNotes peaked +1.38 dBFS at the stored positions, 61 samples over full scale)."
        },
        {
          "step": "pre-screen",
          "text": "Every check passes (sound revision 7, the committed package). Level soft_notes -0.9 LU; Engaged soft_notes +1.1 LU; Peak plucks -3.22 dBFS; strums -3.73 dBFS; soft_notes -1.17 dBFS; Tail soft_notes 4.46 s; sweeps and S11: highest -0.31 dBFS (S11.at-factory.pinhole.soft_notes); births per second at Activity 0 to 1: 66.0 112.7 168.2 259.6 360.7."
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
          "step": "review",
          "text": "Review fixes: Activity and Shape as Engram's tap stage (voices 4 to 16; overlap from 0.63), without the intermittency. Repeats' maximum 0.6 stays above the stored feedback, now 0.5 at position 0.83: at 0.6 the stored position was the maximum, so turning Repeats up changed nothing (stored and Repeats 1 rendered the same bits), and a 0.7 maximum put S11's a0r1s1t1 corner at +2.49 dBFS. wet_trim_db -1.5 to -0.7 dB for the Level the lower feedback took (-2.5/-2.3 LU at -1.5). A single pluck's wet now falls 60 dB in 4.5 s stored and 5.5 s at Repeats 1."
        },
        {
          "step": "pre-screen",
          "text": "Every check passes (sound revision 7, the committed package). Level plucks -1.7 LU; strums -1.5 LU; Engaged plucks +1.4 LU; strums +1.3 LU; Peak plucks -2.44 dBFS; strums -3.29 dBFS; soft_notes -3.92 dBFS; Tail plucks 3.09 s; strums 2.93 s; sweeps and S11: highest -0.56 dBFS (S11.corner-a0r1s1t1.plucks); births per second at Activity 0 to 1: 40.0 68.1 95.4 122.0 147.2."
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
          "step": "review",
          "text": "Review fixes: Mix 0.5 to 0.35 (SoftNotes +0.56 dBFS at the stored positions); Shape swells the loops as Refrain's does (skew 0.5 to 0.9) with intermittency 0 to 0.4 (the 0.15 skew measured -3.6 %, -0.20 dB at the lower Mix)."
        },
        {
          "step": "pre-screen",
          "text": "Every check passes (sound revision 7, the committed package). Level plucks -1.9 LU; strums -1.8 LU; Engaged plucks +1.1 LU; strums +1.1 LU; Peak plucks -2.93 dBFS; strums -3.44 dBFS; soft_notes -1.26 dBFS; Tail plucks 2.38 s; strums 3.04 s; sweeps and S11: highest -2.36 dBFS (S4.time.plucks); births per second at Activity 0 to 1: 5.9 8.1 12.0 14.0 16.0. For listening: Repeats barely lengthens the tail (S7 2.74 s against 2.21 s (1.24x))."
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
          "step": "review",
          "text": "Review fixes: Mix 0.5 to 0.35 (SoftNotes +0.73 dBFS at the stored positions)."
        },
        {
          "step": "pre-screen",
          "text": "Every check passes (sound revision 7, the committed package). Level plucks -1.0 LU; strums +0.1 LU; Engaged plucks +1.3 LU; strums +1.5 LU; Peak plucks -3.22 dBFS; strums -3.44 dBFS; soft_notes -1.19 dBFS; Tail plucks 2.33 s; strums 3.11 s; sweeps and S11: highest -1.42 dBFS (S11.at-factory.echolalia.plucks); births per second at Activity 0 to 1: 4.0 8.1 9.7 14.0 16.0. For listening: Repeats barely lengthens the tail (S7 2.70 s against 2.15 s (1.26x))."
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
          "step": "review",
          "text": "Review fixes: Activity takes the record's overlap term back (0.32 to 0.5: births 6.6 to 22.2 per second); Shape thins voice_count 8 to 1 (curve 0.3) in place of overlap and adds intermittency 0 to 0.4. With voice_count alone Shape measured -4.7 to -2.1 % and +0.58 to +1.02 dB across four spray floors (three under the threshold); with intermittency +1.6 to +2.1 dB on all four. The stored sound is unchanged (overlap 0.32, 2.1 voices)."
        },
        {
          "step": "pre-screen",
          "text": "Every check passes (sound revision 7, the committed package). Level plucks -1.7 LU; strums -2.0 LU; Engaged plucks +1.5 LU; strums +1.3 LU; Peak plucks -2.47 dBFS; strums -3.34 dBFS; soft_notes -1.58 dBFS; Tail plucks 2.35 s; strums 2.79 s; sweeps and S11: highest -0.78 dBFS (S11.corner-a0r1s1t0.plucks); births per second at Activity 0 to 1: 6.6 9.9 14.1 18.7 22.2."
        }
      ]
    },
    "factory.runaway": {
      "name": "Runaway",
      "family": "reverie",
      "declare": {
        "class": "pad",
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
          "step": "review",
          "text": "Review fixes: no longer declared self-oscillating: at the stored positions and at Repeats 1 it falls about 7 dB per second once the input stops (Tail 8.4 s), so the description no longer says endless. Activity adds overlap 0.47 to 0.67 (births 31.2 to 93.4 per second; it moved none), the stored 0.5 kept; Mix 0.6 to 0.45 (S11's a0r1s1t0 corner +0.10 dBFS). Repeats still changes little at its top (feedback 1.05 to 1.08)."
        },
        {
          "step": "pre-screen",
          "text": "Every check passes (sound revision 7, the committed package). Level soft_notes -1.8 LU; Engaged soft_notes +1.7 LU; Peak plucks -3.79 dBFS; strums -3.81 dBFS; soft_notes -1.15 dBFS; Tail soft_notes 8.44 s; sweeps and S11: highest -0.14 dBFS (S11.corner-a1r1s1t0.soft_notes); births per second at Activity 0 to 1: 31.2 41.8 56.3 73.1 93.4. For listening: Repeats barely lengthens the tail (S7 8.49 s against 8.32 s (1.02x))."
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
          "step": "review",
          "text": "Review fixes: the description says what plays: half the shards reversed (reverse_prob 0.5, the record's), not all."
        },
        {
          "step": "pre-screen",
          "text": "Every check passes (sound revision 7, the committed package). Level plucks +0.1 LU; strums -0.8 LU; Engaged plucks +1.9 LU; strums +1.5 LU; Peak plucks -3.27 dBFS; strums -3.56 dBFS; soft_notes -7.18 dBFS; Tail plucks 3.70 s; strums 3.00 s; sweeps and S11: highest -0.41 dBFS (S11.at-factory.deja-vu.plucks); births per second at Activity 0 to 1: 8.0 21.1 29.8 29.8 29.8."
        }
      ]
    },
    "factory.undertow": {
      "name": "Undertow",
      "family": "reverie",
      "declare": {
        "class": "pad",
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
          "step": "review",
          "text": "Review fixes: pad class again; Mix 0.5 to 0.3 (SoftNotes +0.14 dBFS at the stored positions; at 0.35 S11's corners on SoftNotes reached +0.36 dBFS). Repeats moves the post delay's feedback (0.15 to 0.75; its mix 0.35, 420 ms) in place of the grain feedback its spray decorrelated, which stays at 0.3: S7's tails 2.3 to 9.3 s (from 2.3 to 2.7 s); a single pluck's wet falls 60 dB in 4 s at 0 and stored, 10 s at Repeats 1."
        },
        {
          "step": "pre-screen",
          "text": "Every check passes (sound revision 7, the committed package). Level soft_notes -0.8 LU; Engaged soft_notes +1.1 LU; Peak plucks -3.52 dBFS; strums -3.54 dBFS; soft_notes -1.87 dBFS; Tail soft_notes 2.56 s; sweeps and S11: highest -0.37 dBFS (S11.corner-a0r1s0t1.soft_notes); births per second at Activity 0 to 1: 18.2 31.5 52.6 79.3 115.2."
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
          "step": "review",
          "text": "Review fixes: Activity adds voices (3 to 12, six at the stored 0.32 where 5.8 played: births 25.0 to 89.1 per second); Shape's overlap starts at 0.6 (curve 0.5) and adds intermittency 0 to 0.4 (+0.8 %, +0.75 dB without it); Mix 0.45 to 0.4 (SoftNotes peaked -0.61 dBFS at the stored positions)."
        },
        {
          "step": "pre-screen",
          "text": "Every check passes (sound revision 7, the committed package). Level plucks -1.7 LU; strums -1.5 LU; Engaged plucks +1.5 LU; strums +1.4 LU; Peak plucks -3.39 dBFS; strums -3.63 dBFS; soft_notes -1.16 dBFS; Tail plucks 2.44 s; strums 3.80 s; sweeps and S11: highest -0.69 dBFS (S11.corner-a1r1s1t0.plucks); births per second at Activity 0 to 1: 25.0 40.5 63.2 76.7 89.1. For listening: Repeats barely lengthens the tail (S7 2.48 s against 2.39 s (1.04x))."
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
      "text": "Lull and the cloud modes (Murmuration, Halation, Undertow, Afterimage) are pad class, judged on SoftNotes, as they began; Runaway is pad class too, and no longer declared self-oscillating (its tail ends: 8.4 s at the stored positions; S7 8.3 s at Repeats 0, 8.5 s at 1). The rest are attack class, judged on Plucks and Strums; Shards is declared as needing attacks. Peak at the stored positions is judged on the class inputs, and the other inputs' stored peaks are reported under Peak (other), never failed (the owner's decision, 2026-10-08: the review had judged Plucks, Strums and SoftNotes for every mode, so that a held chord into an attack mode would not clip either; the stored Mix it set stays). History: v2-v5 below moved the pad modes to attack class on the reading that Level and Peak could not both pass on SoftNotes, which hid their SoftNotes clipping (up to +1.39 dBFS in 7 of the 14). That reading was wrong: SoftNotes is not the hot input (its dry peaks at -7.2 dBFS, 2.7 dB under Plucks at -4.5 dBFS; it is louder only in loudness, because it sustains), and Level is measured at Mix 1, so a lower stored Mix lowers the stored peak without moving the Level. The review fixes (each mode's \"review\" step) set the stored Mix instead; this note replaces the one that read otherwise."
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
    },
    {
      "step": "activity",
      "text": "Response (Activity) is judged from the grains born per second (Engine::Stats()) with Activity stored at 0, 0.25, 0.5, 0.75 and 1 over the class input: monotonic within 5 % and changed by a quarter (tools/audition/README.md). Before the review fixes the echoic modes' Activity had no density term: Callback 40.0 per second at every rung, Retrograde 6.6-6.9 and not monotonic, Engram and Pinhole 40.0 falling to 35.4 with jitter, Updraft 48.6 falling to 42.3. The tap modes (Engram, Pinhole, Callback) and Updraft now add voices with Activity (layer0.voice_count: an integer, so the coherent tap's Hann sum stays constant at every step, where a fractional overlap ripples at the birth rate), with Shape's overlap starting high enough (0.63, sixteen voices, in the tap modes; 0.6 in Updraft; curve 0.5) that the voice count is what binds; Retrograde, whose reversed chunks are never coherent, takes the record's overlap term back and Shape thins its voice count."
    },
    {
      "step": "shape, revisited",
      "text": "Response (Shape) is measured on the engaged output over 2 s windows, against the reference at the same time: with the dry at unity it moves by up to about 5 % and 0.5 dB with the grains' random draws alone (Retrograde's unchanged Shape measured +9.2 % with an 11 ms spray floor and -5.9 % with 9 ms), and a lower stored Mix dilutes it. After the Activity and Mix changes Engram (-4.7 %, +0.42 dB), Updraft (+0.8 %, +0.75 dB), Retrograde, Downdraft, Recurrence and Afterimage fell under the threshold; Engram, Updraft, Retrograde and Recurrence take intermittency at Shape's top (to 0.4: births dropping out, so the swelled grains come with gaps), Downdraft from Shape 0.25 (to 0.5) and Afterimage reverse grains from Shape 0.3 (to 0.5, as Halation). Each passes by 1.7 dB or more of envelope or 10 % of brightness (the least: Downdraft +1.71 dB, Recurrence +10.3 %)."
    },
    {
      "step": "repeats",
      "text": "Response (Repeats) is reported, not judged: S7's tail at Repeats 1 against its tail at 0, \"listen\" under 1.5 times. Undertow's Repeats moved grain feedback that its spray decorrelates on every pass (S7 tails 2.3 s at 0, 2.7 s at 1); it now moves the post delay's feedback (2.3 to 9.3 s). Named for the listening pass: Updraft 1.04x (its octave climb leaves the audio band), Refrain 1.26x, and the reserves Afterimage 1.00x, Runaway 1.02x, Downdraft 1.28x and Recurrence 1.24x."
    },
    {
      "step": "owner, 2026-10-08",
      "text": "The owner's answers from the listening page. (1) All 14 modes keep, pending the knob ratings, which the owner gives in the app's Curation view. (2) The pad modes' level at Mix 0.3-0.35 is about right. (3) An output safety limiter: yes, as a sound revision of its own, later; nothing here has one, and an attack mode's sweeps and S11 on SoftNotes still pass full scale (up to +2.5 dBFS, Echolalia's S11.corner-a1r0s1t0.soft_notes). (4) Of the review's stricter checks, Peak at the stored positions on every input is too strict: Peak is judged on the class inputs again (Plucks and Strums for an attack mode, SoftNotes for a pad mode), and the other inputs' stored peaks are reported under Peak (other), never as a failure; Activity judged on births and a weak Repeats flagged \"listen\" stay. No recipe or stored Mix changed (the owner listened at these values): the pre-screen re-run under the new Peak rule gives the same 1,488 render hashes, and all 18 pass. The other inputs' stored peaks sit at -1.11 to -7.18 dBFS, none over -1 dBFS (the highest Downdraft's SoftNotes -1.11, Updraft's -1.16 and Refrain's -1.19 dBFS). Each mode's pre-screen step below lists its stored peaks on all three inputs, as the review judged them; the numbers stand."
    }
  ]
}
```
