# Brainscape — Project Status

> Snapshot as of **2026-10-07**.
> Brainscape is an open-source granular delay — a spiritual successor to the Hologram
> Microcosm — targeting a Daisy Seed hardware pedal (prototyped on a Seed Rev7) **and** a JUCE desktop plugin and
> companion app from one shared C++ DSP core. Licensed [GPLv3](../LICENSE).

---

## Where the project is

The project has completed its **research**, the **core DSP engine** (v1 scope), the
**designs for pedal/desktop parity and the companion app**, and the **engine side of the
determinism profile**: one `dsp/` build profile, in-tree math, a full floating-point
control-word guard, a deterministic denormal flush, a NaN-free boundary, the block-split
fix, the engine state API (an exact `Restart`, a random-number epoch, one `LoadPreset`
entry point and frame-stamped events) and the post-delay time glide. **Internal sound
revision 7 is minted** (2026-10-07): the engine plays compiled modes (revision 2, mode-compiler
lane C), mixes by the Mix law, the dry at unity up to the knob's middle and the wet at unity
from it (revision 3, the owner's provisional answer to Q13), and plays wave 1 of the mode
vocabulary (revisions 4–7, lane F: trigger sources, bursts and intermittency; pitch sets;
micro-loop repeat and decay; per-layer voice count), and `dsp/tests/golden/golden.json` holds
the hashes of a 45-preset corpus, 23 of them loaded from packages that `bspc` compiled. MSVC
reproduces it at any block size and from a hostile caller floating-point environment, and so do
GCC 11, Clang 14 and the Cortex-M7 code run under emulation, at revisions 3 to 7 each; at
revision 3 the Cortex-M7 code reproduced its file in CI and on the Seed Rev7 itself too; CI fails a
pull request that changes them (see [Internal sound revisions 4–7](#internal-sound-revisions-47-wave-1),
[3](#internal-sound-revision-3), [2](#internal-sound-revision-2) and
[1](#internal-sound-revision-1)). A JUCE
plugin and standalone skeleton hosts the engine through its stamped events and `LoadPreset`,
with reproducible bounces and an offline audition render; lane D's curation slice opens preset
documents and packages in it, plays them with their modes, edits them with the pedal's macro
knobs (with pickup) and the raw leaves, and saves canonical JSON through the compiler. The Daisy
Seed Rev7 bring-up images (parity, bench, live) build at sound revision 7 and carry the corpus's
packages. On the owner's Rev7 (2026-10-07) the parity image rendered the golden corpus bit for bit
at sound revisions 1 and 3, and the DWT pass (revision 1) showed that **the worst-case CPU budget
is not met**: a fix is under design, with owner decisions pending
([the silicon record](design/reviews/rev7-silicon-record.md)); revisions 4 to 7 have not run on
the board. **Step 4's first factory set is authored and pre-screened** (see
[Step 4: the first factory set](#step-4-the-first-factory-set)): 14 modes and 4 reserves in
`firmware/factory/`, every one passing the objective pre-screen at revision 7; the owner has
heard it (2026-10-08) and keeps all 14, the knob ratings in the app's curation view next. The
preset jobs that mode-compiler lane G added to CI have run on GitHub and passed on pull requests #5
(run 37658865968), #6 (run 37693138971) and #7 (run 37701943741); wave 1, lanes E and D and the
factory set have not yet run on GitHub.

| Phase | State |
| --- | --- |
| Research corpus | ✅ Complete — 11 sourced documents + synthesis ([docs/research/](research/)) |
| Engine design | ✅ Complete — reviewed v2 ([grain-engine.md](design/grain-engine.md)) |
| `dsp/` core: contracts + skeleton | ✅ Shipped & hardened |
| `dsp/` core: grain scheduler + 64-voice pool | ✅ Shipped & hardened (block-split defect found and fixed 2026-10-05) |
| `dsp/` core: post chain + feedback taming | ✅ Shipped & hardened |
| `dsp/` core: onset detector + trigger layer | ✅ Shipped & hardened |
| Determinism profile (sample-identical pedal ↔ desktop) | 🚧 Internal sound revision 1 minted and gating ([determinism-profile.md](design/determinism-profile.md) §8.4 steps 1–9 and most of step 10; what step 10 still lacks is under [Known gaps](#known-gaps-and-deferred-work)). Next: the rest of step 10 and the nightly full-system emulation leg. The hardware measurements are taken (Rev7, revision 1, [record](design/reviews/rev7-silicon-record.md)): gradual underflow stays; explicit FMA (now a candidate under §7.3's rule), kernels or tables and the default load mode stay open beside the CPU budget's fix |
| Companion app + plugin (JUCE: VST3, AU, standalone) | 🚧 Skeleton built ([plugin/README.md](../plugin/README.md); design in [companion-app.md](design/companion-app.md)): wrapper on stamped events and `LoadPreset`, plain-value parameters under host model (b) with the macro, expression and effect-volume parameters, Restart on transport start, offline audition; lane D's curation slice (mode-compiler.md §9.1): a preset document or package opened, played with its mode, edited with the pedal's eight knobs (pickup, Shift) and the raw leaves, saved as canonical JSON through the compiler (derive, solve position, the compiler's errors and lint), A/B against the stored version with level matching, and a one-click render through `tools/audition`; no library, session v2 or device link yet |
| Mode system (JSON → compiled mode, desktop-only compiler) | 🚧 Designed ([mode-compiler.md](design/mode-compiler.md), draft v2); lanes 0, B, A, G and C are built: the permanent parameter-ID table with the macro IDs, the decoded preset (`ModeBlob`, CTRL, performance state) with its decoder, validator and encoder, the compiler with `bspc` (schema 1, canonical JSON, lint, derive), their CI (the sound-revision gate's package rule, `bspc-roundtrip` on seven host legs, the decoder's fuzzers on every leg and the emulated M7, the compiler audit, nightly legs), and the engine runtime at sound revision 2 (modes loaded and validated, the onset source and mark positioning as structure, macro and expression moves, Trails and FastCut mode switches, the wet-only trim, the effect volume and the cutoff's wet kill), the Mix law at sound revision 3 (dry at unity to the knob's middle, wet at unity from it), with the first factory set's recipes re-measured under it, and wave 1 at sound revisions 4–7 (lane F: trigger sources, bursts, intermittency, pitch sets, micro-loop repeat and decay, voice count); lane E's audition tooling ([tools/audition](../tools/audition/README.md): the offline render the app and `bspc render` share, the scripts S0–S11, the objective pre-screen, the ratings log with its carry-forward by render hash and the authoring history); and lane D's curation slice in the app (above). Step 4's first factory set ([firmware/factory](../firmware/factory/README.md): 14 modes in all four families and 4 reserves, every one passing the pre-screen at revision 7) is heard: the owner keeps all 14 (2026-10-08), the knob ratings pending. Next: those ratings, then W2's CLOCK |
| Preset package + upload to the pedal | 🚧 The `.bsp` format is built in `dsp/src/blob/` (decode, validate, encode, SHA-256; no floating-point instruction on the M7) with frozen fixtures and fuzzers, and `bspc` compiles documents to packages byte-identically on MSVC, GCC and Clang; the golden corpus commits 29 packages, which the harness decodes on every leg, the M7 included; upload needs hardware |
| Tempo/clock trigger source | ⬜ Not started (`ProcessContext` fields reserved) |
| Looper subsystem | ⬜ Not started (memory/CPU envelope budgeted in the design) |
| Firmware bring-up (Daisy Seed Rev7 prototype; custom H750 board later) | 🚧 Bring-up images built at sound revision 7 and verified off-hardware at revisions 2 to 7 (the parity stream's code under emulation, in the image's placement) ([firmware/README.md](../firmware/README.md)): silicon parity check (the revision-7 corpus, version 12, from its packages, compiled into the image), DWT measurement pass (contraction-off costs, the §4.2 silent-tail rule, the §7.3 budget rule; it cannot compare explicit FMA or kernels against tables), live audio with mode switches, macros and the expression pedal, on pinned libDaisy v9.0.0; faults are recorded and reported after a reset. **On the owner's Rev7, 2026-10-07** ([record](design/reviews/rev7-silicon-record.md), captures in [`firmware/records/`](../firmware/records/rev7-2026-10-07/README.md)): parity passed at revision 1 (28 presets) and revision 3 (33 presets, 18 packages), each at `maxBlockSize` 512 and 48 and from a hostile caller; the DWT pass (revision 1) keeps gradual underflow and ITCM placement, measures the flush at 0.9–1.8 % and an Exact load at 47.2 ms, and finds **the worst-case CPU budget not met**: the nominal row peaks at 99.1 % (100.3 % cold), the pessimistic rows at 135.5–168.5 % (136.2–169.0 % cold), the corpus's `dense_1ms` at 118.6 % (119.7 % cold); a fix is under design, owner decisions pending. Not yet run on the board: the live image, the bench at revision 3, any image at revisions 4 to 7 |
| Hardware (schematic/PCB) | ⬜ Not started (reference design chosen in research) |

**The one-engine bet is validated in code.** The design's central claim — that the
Microcosm's 11 effects are one voice engine + one scheduler with modes as data — now has
its strongest possible evidence: Brainscape's clean delay *is* a grain configuration
(rectangular window, abutting unity-rate grains), and it nulls **bit-exactly** against
the raw int16 history ring.

## Decisions recorded on 2026-10-05

The owner set three product requirements for the desktop side:

1. **The companion app is for preset creation with 1:1 recreation.** Presets authored on the
   desktop must render exactly as the Brainscape engine on the pedal renders them, and the
   app uploads them to the pedal.
2. **JUCE, in one C++ monorepo.** This replaces the research's iPlug2 recommendation. JUCE
   must be used under AGPLv3 (its commercial licence forbids combining with copyleft code),
   so released desktop binaries are a GPLv3 + AGPLv3 combined work; see the README.
3. **"Same sound" means sample-identical output**, not perceptual similarity.

The parity investigation showed requirement 3 is achievable. A prototype that replaced every
standard-library transcendental with in-tree math and disabled compiler multiply-add fusion
produced **one identical SHA-256 across 32 builds** — MSVC, GCC 11, 12 and 14 and Clang on
x86, and the real firmware code generation for the Cortex-M7 run under emulation — over 10
presets × 30 s. `dsp/` now has this property (measured 2026-10-06), and it is now enforced:
the golden corpus (13 vectors, 28 presets, up to 123 s each) gives the SHA-256 per preset
committed for sound revision 1 with MSVC 19.40 (SSE2 and AVX2), GCC 11 and 14 (also at
`-march=x86-64-v3`, and in Debug), Clang 14 and the Cortex-M7 build from the pinned
arm-none-eabi 10.3 run under `qemu-arm -cpu cortex-m7`. A build with contraction turned back
on (the negative control) misses it on 25 of the 28 presets, with GCC 11 and with MSVC.

What the guarantee covers is precise. Two conforming builds of the same sound revision,
restarted into the exact-restart state, loading the same compiled preset with an Exact load,
and fed the same 48 kHz float32 input and the same frame-stamped events, write identical
float32 output, whatever block sizes each side uses. Not
covered: the pedal's analog path, live playing, preset loads that keep trails (Spillover),
DAW sessions at other sample rates, and DAW automation. The full contract is in
[determinism-profile.md](design/determinism-profile.md). The engine now has every piece of
that recipe: the corpus's presets with structure are packages compiled by `bspc` and decoded
by `DecodePreset` (mode-compiler lane C), the rest parameter lists of the default mode. The golden harness
Inits one engine, and for every render restarts it with `LoadPreset(…, Exact)` from a preset
state and hands its scripted events to `Process` through the engine's `EventQueue`; three
presets also load Spillover, restart, or load Exact mid-render. Rendering with a fresh `Init`
per preset, with the events applied between blocks split at their frames, or from a caller
whose control word is FTZ|DAZ (on the M7, FZ|DN) with round toward zero, gives the same bits.

## Step 4: the first factory set

Authored 2026-10-07 ([mode-compiler.md](design/mode-compiler.md) §11; [firmware/factory](../firmware/factory/README.md)).
No engine change: the set plays at sound revision 7.

- **The modes.** The 14 of design §11.1, schema-1 documents with their packages, `MANIFEST` and
  the ratings log `AUDITION.md`: echoic Engram, Callback, Retrograde, Updraft and Pinhole; reverie
  Murmuration, Halation, Undertow, Lull, Echolalia and Déjà Vu; recall Kaleido and Refrain; misfire
  Shards. Wave 1 carries what §11.1 says it needs: Halation's and Undertow's pitch sets ({0, +12},
  {0, −12}), Echolalia's and Déjà Vu's `decay_ms` (Repeats moves it, in place of feedback), Déjà
  Vu's bursts, Kaleido's set, `repeat` and decay, Refrain's set, `repeat` and `voice_count`, and
  Shards' onset-only source with bursts. Four reserves (`reserve/`: Afterimage, Runaway, Downdraft,
  Recurrence) are written and pre-screened too; the design's other two (a filtered drone and a
  sub-octave drone) are not. Names are working names (Q3); nothing matches the L9 denylist.
- **Checks.** Every document is canonical, stamped at revision 7, derived (each targeted leaf is
  its macro's value at the stored position) and passes `bspc lint --factory` with no finding;
  `bspc-roundtrip` now runs the factory lint over `firmware/factory/` on every leg, as design §2.7
  says CI does (and skips a `renders/` directory there). The objective pre-screen (the commands in
  [firmware/factory/README.md](../firmware/factory/README.md): the 14 as one set in the design's
  order, so S10 and S11 cross it, the reserves as another, written to `build/`) passes every check
  on all 18: the attack modes' wet sits −2.0 to +0.1 LU from the dry on Plucks and Strums and their
  engaged output +0.9 to +1.9 LU over bypass, the pad modes' −1.1 to 0.0 and +0.9 to +1.7 LU on
  SoftNotes; stored peaks on the class inputs −1.07 to −3.87 dBFS, and on the other inputs
  (reported, not judged) −1.11 to −7.18 dBFS; every sweep and S11 render on the class input at or
  under 0 dBFS (the tightest −0.01 dBFS, Halation at Pinhole's positions); every tail finite; Clicks at most 1.19 times the static's step; Activity's
  births per second rising by 2.7 to 10 times from 0 to 1. The packages and the documents compiled
  in memory give the same 1,488 render hashes. Each iteration is recorded with its reason in
  `AUDITION.md` (`ratings.py note`): Shape made audible (most modes take it towards fewer, longer,
  swelled grains; Engram, Retrograde, Updraft and two reserves with births dropping out at its
  top), S11's corners and the other modes' positions brought under full scale (lower Repeats
  maxima, voice floors, trims and stored Mix), the tap modes' single rectangular grain replaced by
  Hann grains a quarter-grain apart so Activity and Time crossfade instead of splicing.
- **Input classes and the review.** Lull and the clouds (Murmuration, Halation, Undertow,
  Afterimage) and Runaway are pad class, the rest attack class; Shards needs attacks; nothing is
  declared self-oscillating (Runaway's tail ends, 8.4 s). The first pass had moved the clouds and
  Lull to attack class on the reading that Level and Peak could not both pass on SoftNotes, which
  hid 7 of the 14 clipping on SoftNotes at their stored positions (up to +1.4 dBFS). The review
  found the reading wrong (SoftNotes peaks 2.7 dB under Plucks; Level is measured at Mix 1, so a
  lower stored Mix lowers the peak alone): the stored Mix now holds every mode under −1 dBFS on
  all three inputs (Murmuration, Halation, Undertow, Déjà Vu and Kaleido at 0.3, Lull and Refrain
  0.35, Updraft 0.4), and the review had the tooling judge the stored Peak on all three whatever
  the class (the owner's answers, below, returned it to the class inputs). It
  also found Activity's response unjudged and dead in the echoic modes (Callback's births flat at
  40 per second; the audition now reads `Engine::Stats()` and judges births per second at five
  Activity positions), Undertow's Repeats moving nothing (its grain feedback decorrelated by
  spray; it now moves the post delay's feedback, S7's tail 2.3 to 9.3 s) and Pinhole's Repeats
  stored at its maximum. The tooling now reports a Repeats knob whose tail at 1 is under 1.5 times
  its tail at 0 as "listen": Updraft (its climb leaves the audio band), Refrain and the reserves
  Afterimage, Runaway, Downdraft and Recurrence. An attack mode's sweeps and S11 are judged on
  Plucks only, and on SoftNotes several corners still pass full scale (up to +2.5 dBFS).
- **The owner's answers (2026-10-08)**, from the listening page, recorded in `AUDITION.md`
  (`ratings.py note --set`): all 14 modes keep, pending the knob ratings, which the owner gives in
  the app's curation view; the pad modes' level at Mix 0.3–0.35 is about right; an output safety
  limiter is wanted, as a sound revision of its own (not yet built); and of the review's stricter
  checks, Activity judged on births and Repeats' "listen" stay, while Peak on every input was too
  strict: Peak is judged on the class inputs again and the other inputs' stored peaks are reported
  under Peak (other), never failed. No recipe or stored Mix changed; the re-run pre-screen gives
  the same 1,488 render hashes and all 18 pass, every other input's stored peak under −1 dBFS.
- **The owner's audition set**, rendered outside the repository with lane E's render: per mode the
  stored positions on Plucks and SoftNotes (3 s of input, 4 s of tail) and one macro sweep on its
  class input (Repeats over 4 s of input, then 3 s of silence at its maximum), plus a bypass
  reference per input, 48 kHz 16-bit stereo, each file fading out over its last 50 ms, with a
  manifest (what to listen for, render hashes, every pre-screen check, which files cut a tail). The
  owner rates in `firmware/factory/AUDITION.md` only. The files are 7 s, not 8: 44 files of 8 s are
  67.6 MB, over the set's 60 MB cap (59.1 MB at 7 s).
- **What remains for step 4**: the owner's knob ratings, in the app's curation view, and the
  ratings rows (`ratings.py rate` on the full S0–S11 renders), the exit criteria (10 keepers, one
  per family, every knob 3 or more), the keepers joining the golden corpus, display names and Q3's
  name check, and DI clips (Q9).

## Internal sound revisions 4–7: wave 1

Minted 2026-10-07 by mode-compiler lane F ([mode-compiler.md](design/mode-compiler.md) §7.5,
§7.6 item 6), one revision and one commit per feature, each re-minting
[`golden.json`](../dsp/tests/golden/golden.json) with `--mode mint`. Internal, like revisions 1–3,
and they keep all their gates. The owner's provisional Q2 put wave 1 before CLOCK.

**What changed in the engine.**

- **4, trigger sources, bursts and intermittency** (R9). The free-running scheduler, and its
  one-voice floor, runs only when the mode lists `periodic`; footswitch and MIDI-note triggers
  fire only when it lists their source, decided at the frame each falls due (Sidechain and
  unknown ids count as the footswitch). Every trigger, onsets included, births
  `scheduler.burst.count` grains, the rest of a burst every max(1, round(spacing_ms·48)) frames,
  each with its own frame's draws (at most one burst grain a frame, none at a frame a trigger
  fired at; 8 bursts in flight). A draw below `scheduler.intermittency` skips a whole trigger or a
  periodic birth, which still consumes its interval (R8's extended keys, purpose 9, an ordinal per
  kind). Without a free-running source the normalization's N is the burst. Leaves 57–59.
- **5, pitch sets** (R10). Layer 0 plays its set: `cycle` steps through the entries in order,
  each `weight` times, `random` picks by weight in integers from the birth's draw (purpose 8). The
  pitch is (entry + `transpose_st`) + detune, clamped to ±24 st; the normalization's pitch term
  takes the set's largest |ratio − 1|. The default set {0: 1} is revision 4's pitch bit for bit.
- **6, micro-loop repeat and decay** (R11). A voice with `layer0.position.repeat` N reads one
  region N times, each pass windowed, for a life of N passes; the far rail covers the whole life
  ((N − 1)·L more frames off it). `layer0.decay_ms` is the time to fall 60 dB as the position
  reference ages, by DetMath's `Exp2F` at birth and each pass: a mark's age, or the pass's start
  for a live position. The scheduler spaces births by the life. Leaves 29 and 30. The grain grew
  from 80 to 96 bytes, so `Engine::Impl` is 7,928 bytes on the M7 and 8,080 on x86-64
  (*measured*): `kEngineImplBytes` rose to 8,192 and 8,448, and the firmware's DTCM engine slot
  from 8 to 9 KiB.
- **7, voice count** (R12). At most `layer0.voice_count` voices sound: the free-running target is
  min(64·overlap³, voice_count, a voice's life), and a trigger with that many voices sounding
  steals the oldest, as one with all 64 busy did. Leaf 31.

Each revision's defaults (all sources but onset and clock, burst 1, intermittency 0, the set {0},
repeat 1, decay 0, 64 voices) keep the previous revision's arithmetic, and `Engine::Stats()`
counts births, burst births, skips, repeat passes and steals for the tests and the audition
metrics. The sequencing state the design names (the bursts in flight, the pitch cycle's position)
resets on a load of another mode and on `Restart`, and survives a load of the same mode.

**What the re-mints changed, preset by preset: nothing.** At each of the four revisions every
earlier preset reproduced its hash, per-second hashes and counters bit for bit (33 of 33 at 4,
38 at 5, 40 at 6, 43 at 7). The corpus grew by twelve presets in versions 9–12, each with
ablations that change its output: `onset_only`, `onset_burst`, `burst_spaced`,
`intermittent_cloud` and `midi_gate` (sources, bursts, intermittency, MIDI triggers);
`pitch_cycle` and `pitch_random` (both selections, transpose moves over a set, switches between
them); `repeat_loops`, `decay_marks` and `repeat_mark_aging` (passes, decay on live positions and
on marks, 16 reverse passes reaching the life's far rail 8.5 s before the 2^22 ring's staleness
guard, where the ring ablation first differs); `voice_limit` and `mono_stutter` (a cloud held to
6, 2 and 64 voices, one voice cut by every grain). The counters `births`, `burstBirths`, `skips`,
`repeatPasses` and `steals` and the ablations `sources`, `burst`, `intermittency`, `pitchSet`,
`pitchSelect`, `repeat`, `decay` and `voiceCount` came with them; 45 presets in all, 23 starting
from packages (29 packages, some only loaded mid-render).

**What changed in the packages.** A new leaf joins every package's STAT at its default (the
compiler writes every Leaf row, design §6.4), so at revisions 4, 6 and 7 every corpus package's
`sound_hash` changed while its render did not, and the pull request that carries each of those
revisions needs the package rule's `package-change` label with its cause, one `Package-change:`
line per revision (`wave 1's leaves 57-59 join every package's STAT`, then 29–30, then 31);
revision 5 only re-stamped them. The frozen fixtures keep their bytes and change their verdicts
as the design said they would:
`future-pitch-set.bsp` decodes and validates since revision 5, `w1-leaf-macro-target.bsp` and
`w1-leaf-expression.bsp` since 6. The compiler's random-document and reader-fuzz digests and the
package fuzzer's verdict digest were re-minted at each revision. Lane F's review restored what
those verdicts had covered: `w3-leaf-macro-target.bsp`, `w3-leaf-expression.bsp` (a macro target
and an expression assignment on `layer0.level_db`, ID 32, a Reserved row until W3:
`UnsupportedTarget`) and `w2-step-table.bsp` (`UnsupportedFeature`, step tables) are frozen beside
them, 18 fixtures in all; the package fuzzer's Reserved target moved from row 30 to row 32, so
`DecodePreset` reaches all 51 of its codes again (50 at revision 7 before), its digest re-minted;
and a compiler test reads wave 1 with revision 3's features to reach the E6 messages a build
without it gives. Lint L5 now tells `scheduler.sources: []`, which never plays a grain (the engine
drops a trigger whose source the mode leaves out), from a mode that waits for its triggers, and
`--factory` makes that case an error.

**Which builds agree.** MSVC 19.40 reproduces the file at every one of the four revisions in
check mode: the whole suite in Release (SSE2 and AVX2) and Debug, and in Release the harness at
blocks of 1, 48 and 512, the patterns {48, 1, 127, 32} and {300, 512, 5, 64}, random patterns 1
and 2, a hostile caller, split delivery (48, 512 and random 3), fresh engines and the forced-flush
control; the parity stream's host ctests, the package fuzzer's digest and the frozen fixtures
match. GCC 11.4 and Clang 14 (in Docker) reproduce it at each revision too: GCC's Release suite and
its harness in 15 configurations (blocks of 48 with the ablations, 1 and 512, the patterns 7, 37,
127, {48, 1, 127, 32} and {300, 512, 5, 64}, random patterns 1 and 2, a hostile caller, split
delivery at 48, {300, 512, 5, 64} and random 3, fresh engines), Clang's Release suite and blocks
of 48, GCC's Debug suite at 6 and 7, and `bspc roundtrip`'s manifests identical to MSVC's. So does
the Cortex-M7 oracle under `qemu-arm -cpu cortex-m7` (arm-none-eabi 10.3.1): the golden check at
blocks of 48, 1 and 512, both mixed patterns, random pattern 1 and a hostile caller, the
forced-flush control, the parity stream in the parity image's placement at `maxBlockSize` 48 and
512 (the whole corpus and every package) and on the hostile quick set, and the package fuzzer's
digest and the frozen fixtures. Its engine archives built on Linux equal those built on Windows and
the firmware's (revision 7: `987179706f090560…`, hooks `2c086121ddc937d6…`, as this tree builds
them; at revision 7's own commit, `54ce912`, `ea7c5dfb08cf44e3…`, hooks `959493f9b4cf03f2…`, which
lane D's flags-only `ParamDisplay` change, `c8b739d`, changed with no render moving; 6:
`9e07ead26fac7402…`, hooks `bb616b8b55d80185…`; 5: `fc2d3076d962a781…`; 4: `7c20000633f303ee…`).
The plugin's ctest passes in Release and Debug at revisions 4 (whose leaves its session test had
to learn) and 7. The static audits report 0 at every revision, the arm symbol audit included,
which lane F's review found failing at 6 and 7 as first built: `DecayGain` converted a 64-bit age
to `float`, which the M7 does in libgcc's soft-float `__aeabi_ul2f` (540 more bytes of ITCM); it
takes a 32-bit age now (an age stays below 2^27), and no output changed. The five firmware images
build at each revision. CI's `parity-host`, `bspc-roundtrip` and `parity-m7` jobs repeat these
checks on each pull request.

## Internal sound revision 3

Minted 2026-10-07 ([mode-compiler.md](design/mode-compiler.md) §7.1 R3b, §7.6 item 5):
`kSoundRevision` is 3 and [`golden.json`](../dsp/tests/golden/golden.json) is re-minted. Internal,
like revisions 1 and 2, and it keeps all their gates. It builds the owner's provisional answer to
Q13, which stays reversible until the first public revision.

**What changed in the engine.** Only the Mix law. Mix was a linear crossfade,
dry·(1 − m) + wet·m, so at the first factory set's stored Mix of 0.35–0.55 every mode played
quieter engaged than bypassed (review finding P1). Now the dry stays at unity up to the knob's
middle and the wet is at unity from it: dry·min(1, 2(1 − m)) + wet·min(1, 2m), on the smoothed
Mix ([`dsp/src/detail/MixLaw.h`](../dsp/src/detail/MixLaw.h)). Both gains are exact in binary32,
so Mix 0 is the dry input bit for bit (up to the sign of a zero: a −0 input sample comes out +0
where the wet sample is positive or +0), Mix 1 the wet alone and the middle both at unity, and at
the two ends the arithmetic is revision 2's. `Engine::Impl` keeps its size: the law is a pure
function of the smoothed Mix and adds no state (6,600 bytes on the M7, measured).

**What changed in the corpus** (version 8). `subnormal_wet` moves from Mix 0.5 to 0.75, where the
law scales its dry path by 0.5 as the crossfade did at 0.5, so it still covers subnormal input
through a scaled dry path. The 18 corpus packages and the compiler's seven examples are
re-stamped (`sound_rev` 3 and the package hash only: every sound and control hash is unchanged,
so the package rule sees no change), and the three committed digests that include the stamp are
re-minted: the compiler's random-document and reader-fuzz digests and the package fuzzer's
verdict digest. Built with `kSoundRevision` 2, the same tree reproduces all three revision-2
digests, so the stamp is their only change.

**What the re-mint changed, preset by preset.** 6 of the 33 presets reproduce revision 2's hashes
bit for bit: `freeze_marks`, `freeze_retoggle_spill`, `hot_out` and `reverse_mark_aging`, which
play at Mix 1 throughout, `subnormal_dry` at Mix 0, and `default_silence`, whose input is silent.
The other 27 start at an interior Mix (the default 0.5, 0.6–0.9 in four packages, and
`subnormal_wet` now 0.75), so each changes from second 0: `default`, `clean_delay`, `strum_marks`,
`pitch_reverse_spray`, `max_delay_spray`, `max_delay_spray_rev_up24`, `near_rail_spray`,
`pitch_extremes`, `dense_1ms`, `post_max`, `post_sweep`, `clean_fixed`, `selfosc`, `fb_decay`,
`tail_post_fb`, `freeze_long`, `freeze_live_long`, `exact_load_mid`, `subnormal_wet`,
`macro_sweep`, `expression` and `mode_switch` at their stored Mix, and `automation_offgrid`,
`spillover_chain`, `restart_kept_params`, `wet_kill` and `lone_changes`, whose scripts also move
it. Only silent seconds and seconds at Mix 1 keep their hashes (the last of `default` and
`pitch_reverse_spray`, 8–19 of `fb_decay`, 6 and 10 of `wet_kill`), and the only counters that
moved count louder output reaching further (`outActiveFrames`, `tailActiveFrames`,
`lastActiveFrame`; `fb_decay`'s tail stays above 2⁻¹⁶ 0.19 s longer).

**The first set re-measured** (review finding P1, design §11.1, the evidence record's §2.2). The
probe [`tools/parity/modes/mixlaw/`](../tools/parity/modes/mixlaw/) renders the 14 first-set
recipes and 2 reserves over the curation probe's three scores and the Plucks, Strums and SoftNotes
vectors, and measures the output against bypass in K-weighted loudness (ITU-R BS.1770-4). Under
revision 2's crossfade all 84 first-set renders were 1.2–6.9 LU quieter engaged than bypassed;
under the law 82 pass the pre-screen's −1 to +4 LU (−0.9 to +4.9 LU), the two above it Echolalia
and Kaleido on Strums, whose wet runs about 3 LU over the dry there. The law raises peaks too: 10
of the 84 renders exceed the pre-screen's −1 dBFS at stored positions and three exceed 0 dBFS (all
on SoftNotes), against −4.6 dBFS at most before. The wet trims (36 renders have the wet more than
2 LU from the dry), the declared input classes and the owner's listening pass are step 4's.

**Which builds agree, so far.** MSVC 19.40 reproduces the file in check mode: the whole suite in
Release (SSE2 and AVX2) and Debug, and in Release the harness at blocks of 1, 48 and 512, the
patterns {48, 1, 127, 32} and {300, 512, 5, 64}, random patterns 1 and 2, a hostile caller, split
delivery ({300, 512, 5, 64} and random 3), fresh engines and the forced-flush control; the parity
stream's host ctests match the file and `MANIFEST` at `maxBlockSize` 48 and 512; the package
fuzzer's re-minted digest and the 15 frozen fixtures match. The five firmware images build at
revision 3, and their engine archives are byte-identical to the M7 oracle built on Windows with the
pinned arm-none-eabi 10.3: `1d6fe1dc41f02fd9…` (hooks `023a9fa933fa9c0d…`), and so is the M7
oracle built on Linux in Docker. In Docker, on the pre-commit tree (`dsp/src`, `dsp/include` and
`golden.json` identical to the commit's; `test_modes.cpp` different only in a comment, a test name
and formatting), GCC 11 and Clang 14 reproduce it too: GCC 11 Release ctest 19/19 with 0 warnings
and the harness at blocks of 1, 48 and 512, both mixed patterns, random patterns 1 and 2, a
hostile caller, split delivery and fresh engines (33 of 33 presets each), and the package
fuzzer's re-minted digest; GCC 11 Debug ctest 19/19 and four harness patterns; Clang 14 Release
ctest 19/19 and blocks of 48 and 512 and a hostile caller. **The M7 at revision 3.** The local
run of the M7 oracle under `qemu-arm -cpu cortex-m7` was cut off when Docker stopped, every hash
that arrived matching the file (17 of 33 presets at blocks of 48 and 512, {48, 1, 127, 32}, random
1 and a hostile caller, 11 at blocks of 1, 10 under the forced-flush control, 17 in the parity
streams at `maxBlockSize` 48 and 512 and 25 in the hostile quick set). CI's `parity-m7` job then
rendered the whole corpus, on pull requests #6 (run 37693138971) and #7 (run 37701943741) and on
`main` at `4090270` (run 37700307648): the archive
`1d6fe1dc…` under QEMU 10.2.3 matches the file on all 33 presets at blocks of 48 and 512,
{48, 1, 127, 32}, random pattern 1, from a hostile caller and under the forced-flush control, the
parity stream at `maxBlockSize` 48 in the parity image's placement matches the file and
`MANIFEST`, and the package fuzzer's digest and the 15 fixtures match. Lane F's review ran the
local run in full on revision 3's last tree (the Mix law review's, whose engine archive and golden
file are the commit's): the golden check at blocks of 48, 1 and 512, both mixed patterns, random
pattern 1 and a hostile caller, and the forced-flush control, 33 of 33 presets each; the parity
stream in the parity image's placement at `maxBlockSize` 48 and 512 (33 presets, 18 packages) and
on the hostile quick set (29); and the package fuzzer's digest and the 15 frozen fixtures. Its
archive built on Linux is the one above, and the arm symbol, flag and armv7 fused audits report 0.
On 2026-10-07 the Rev7 itself rendered the whole corpus bit for bit at `maxBlockSize` 512 and 48
and from a hostile caller, the 18 packages matching `MANIFEST`
([the silicon record](design/reviews/rev7-silicon-record.md) §2). 1-frame blocks have run in full
on the M7 at revision 3 only under emulation, in lane F's run.

## Internal sound revision 2

Minted 2026-10-07 by mode-compiler lane C ([mode-compiler.md](design/mode-compiler.md) §7,
§7.6 item 4): `kSoundRevision` is 2 and [`golden.json`](../dsp/tests/golden/golden.json) is
re-minted. Internal, like revision 1, and it keeps all of revision 1's gates.

**What changed in the engine.** A parameter change rebuilds what its row's domain names (R1;
no routing by ID, so a lone change always takes effect at its frame). Rows 27 and 28 are
retired into mode structure (R2b): the onset source and mark positioning come from the loaded
mode. `wet_trim_db` and the effect volume (`global.effect_volume_db`, ID 82, a device setting)
scale the wet signal after the post chain, as one gain, and the cutoff's minimum kills the wet
signal (R3, `WetGainTarget`). `LoadPreset` validates the mode and CTRL and applies nothing when
they fail, applies the `sinceRev` rule, copies the mode into the Warm arena and reports
unsupported performance state (R5). `MacroMove` and `Expression` events fan out through the
shared evaluator (R6). A Spillover load is Trails or FastCut, which fades the sounding grains
over 128 frames (R7). `Engine::Impl` grew to 6,600 bytes on the M7 (budget raised to
7,168) and 6,752 on x86-64 (7,424).

**What changed in the corpus** (version 7). The revision-1 presets that set rows 27 or 28 take
that structure from committed packages compiled by `bspc` from documents holding exactly their
values (`dsp/tests/golden/presets/`, with `MANIFEST`): eight start from one and `exact_load_mid`
loads one mid-render; the automation preset's toggles of 27 and 28 became Spillover loads between
two modes in both styles, none while a freeze is held; `post_max` moved from 40 Hz to 41 Hz,
since 40 Hz now kills the wet; and a new 14 s vector holds a macro sweep, the expression pedal on
macros and leaves, a chain of mode switches with macro and pedal moves after the loads (each
switch package with its own macros and CTRL, the FastCuts fading unity and pitched grains), the
wet kill (at mix 1 too, where the output shows it) and one lone change per leaf, with counters
for macro moves, expression events, mode switches, killed frames and muted output frames,
ablations for the mode, the moves, the switches, FastCut and the kill, and an invariance that
renders every parameter event among edits rebuilding every other domain. `golden.json` records
each package preset's `soundHash` and `controlHash`, which check mode compares.

**What the re-mint changed, preset by preset.** Of revision 1's 28 presets, 25 reproduce
revision 1's hashes bit for bit (every converted package preset included, and `hot_out`, whose
+24 dB trim runs at mix 1, where wet-only and whole-output trims give the same bits), and
exactly the three the design expects changed: `automation_offgrid` (mode switches instead of 27
and 28, none while a freeze is held, so its freezes hold as long as at revision 1; and its trim,
below full mix, now wet only; from second 0), `subnormal_wet` (−6 dB trim at mix 0.5; from second
0) and `post_max` (41 Hz; from second 2). The five new presets have no earlier hash. Lane C's
review re-minted `automation_offgrid`, `mode_switch` and `wet_kill` for its corpus changes; no
other hash moved.

**Which builds agree.** In check mode against the file: MSVC 19.40 (Release SSE2 and AVX2,
Debug), GCC 11.4 (Release and Debug), Clang 14 (Release) and the Cortex-M7 archive from
arm-none-eabi 10.3 under `qemu-arm -cpu cortex-m7`. GCC Release ran blocks of 1, 48 and 512, the
patterns {48, 1, 127, 32} and {300, 512, 5, 64}, random patterns 1 and 2, a hostile caller,
split delivery ({300, 512, 5, 64} and random 3, macro and expression moves sent as `SetParam`s
of the evaluator's leaves) and fresh engines; the M7 ran blocks of 1, 48 and 512, {48, 1, 127,
32}, random 1, a hostile caller and the forced-flush control; MSVC SSE2 blocks of 1, 48 and 512,
{48, 1, 127, 32}, random 1, a hostile caller and split delivery, and AVX2 blocks of 1, 48 and 512
and {48, 1, 127, 32}; GCC Debug 48-frame blocks, {300, 512, 5, 64}, random 2 and 1-frame blocks
from a hostile caller, MSVC Debug ctest's checks. The package fuzzer's re-minted digest and the
15 frozen fixtures match on the host legs and the M7.

## Internal sound revision 1

Minted 2026-10-06 (determinism-profile.md §5.12, §6.1, §8.4 step 10): `kSoundRevision` is 1
and [`dsp/tests/golden/golden.json`](../dsp/tests/golden/golden.json) is its golden file.
It is internal: nothing carrying it has left the project, and the hardware-gated decisions
below will bump it before the first published revision.

**What it certifies.** The `dsp/` sources as of the mint, the shared constants, the canonical
`EngineConfig` (48 kHz, a 2²² ring, stereo input, dither on the ring write) and the profile's
rules and flags, for every input. The golden file holds 13 integer-generated input vectors
and 28 presets (639 s of rendered audio): per preset, the SHA-256 of the whole float32 output,
one per second, and 18 coverage counters, rendered from one restarted engine with each
preset loaded Exact and its scripted events stamped through the engine's `EventQueue`. It
covers the post-delay glide (two presets glide from second 0, one through a Spillover load),
Spillover and Exact loads and a `Restart` mid-render, freeze, onset marks, feedback above 1,
every post stage, subnormal input, and a 120 s silent tail (in a 123 s vector).

**Which builds agree.** Every one tried, in `--mode check` against the file: MSVC 19.40
(Release SSE2, Debug, Release AVX2), GCC 11.4 (Release, Debug, x86-64-v3), GCC 14.2 (Release,
Debug), Clang 14 (Release, Debug) and the Cortex-M7 archive from arm-none-eabi 10.3 under
`qemu-arm -cpu cortex-m7`. Every Release build and the M7 ran blocks of 1, 48 and 512 frames,
the patterns {48, 1, 127, 32} and {300, 512, 5, 64} and the random pattern of seed 1 (257
sizes of 1–512 frames, `--random-blocks`); the x86 Release builds also 7 and seed 2, and
MSVC SSE2 also 32, 37, 64 and 127. Every Debug build ran 48-frame blocks,
{300, 512, 5, 64}, 1-frame blocks from a hostile caller and seed 2. Events went through the
engine or through a wrapper that splits its blocks at them (seed 3 included); renders came
from a restarted engine or one `Init`'d per render, and from a caller whose control word is
FTZ|DAZ (FZ|DN on the M7) with round toward zero. The forced-flush control reproduces it
too, on every host build and with FZ on the M7. The M7 archive is the same bytes across two
builds, across the bump and across the review fixes (`4f4ddaa3583e46f2`).

**What gates.** Check mode fails on any difference from the file: a hash, a per-second hash or
a counter of any preset, a vector's input hash or ring sizes, or the header's revision,
versions and engine configuration. `parity.yml` with `PARITY_HASHES_GATING` on: seven host legs
and the emulated M7 render in check mode at 48-frame blocks, 512-frame blocks and from a
hostile caller (the M7 also at {48, 1, 127, 32}, at a random pattern and with flushing forced
on inside the guard, FZ; the Linux GCC leg at every block size above, two random patterns, with
wrapper-side splitting, a random pattern included, and with fresh engines); each leg uploads
a WAV file of every preset that misses the file on the 48-frame grid, for triage; the
contraction-on negative control must miss the file; the static audits (no libm import, no
fused instruction, no forbidden flag, no JUCE include outside `plugin/`); and parity-summary
fails on any canonical leg that differs from another. ctest's `golden_check`,
`golden_check_edits` (edited copies of the file must fail check) and `golden_forced_flush`
run in every build, so `host.yml` and `plugin.yml` gate on the file as well. `sound-rev.yml`
(companion-app.md §3.4's `sound-rev-gate`) fails a pull request that changes a golden hash
without bumping `kSoundRevision`, whatever its labels, and one that touches `dsp/src`,
`dsp/include`, `dsp/CMakeLists.txt`, the root `CMakeLists.txt`, the profile CMake file, the
forbidden-flag list or the arm toolchain file without a bump or the "sound-neutral" label.
It checks bumps per commit: each revision is one commit that raises `kSoundRevision` by exactly
one, and its golden file is minted (`brainscape_golden --mode mint`) at that revision, by that
commit or a later one before the next bump; a pull request may carry several consecutive
revisions. It walks every commit the pull request adds and fails a commit that lowers the
revision or skips a number, a revision two commits introduce or one the base already has, and a
revision no commit at that revision minted; the walk needs the whole history, so the job checks
out with `fetch-depth: 0` and the gate fails on a shallow clone. It checks a golden file below
the head's revision by its key and the commit that wrote it, not by rendering it (parity renders
the head's); `sound-rev-render.yml` renders every lower revision on every parity leg: the gate's
`--list-revisions` names, for each revision below the head's, every commit at that revision that
a commit at another revision has as a parent (the next revision's bump, or a merge into a later
one, so a side line merged after the next bump is rendered too). One job per leg of the head's
`parity-host` (Linux x64 GCC and Clang, Linux arm64 GCC, MSVC SSE2 and AVX2, AppleClang pinned
and latest) renders every listed commit and one job per listed commit renders it on
`parity-m7`'s runner: each builds the commit's own harness in a worktree as that commit's own
`parity.yml` builds it on the leg and makes every render that `parity.yml` makes there against
the commit's golden file, what parity would have checked with the commit as the head
(`tools/ci/parity_plan.py` reads `parity.yml` and fails on any shape it does not know; a commit
whose `parity.yml` does not run a leg gets the head's renders there; today on every host leg 48
and 512-frame blocks and the hostile FP environment, on Linux GCC also the other block sizes,
random sizes, split delivery and fresh engines, on the M7 its block sizes, the hostile FPSCR, the
parity stream and the forced-flush control). The job `sound-rev-render`, which has no `needs` so
that its check is pending from the start of every run, lists the revisions itself and waits for
the listing and every render job: with at most one revision it passes after the listing, and a
listed commit that cannot be built or checked on any leg, or whose golden file is not keyed to
its revision, fails it. While branch protection requires `sound-rev-render`, a pull request may
push several revisions at once; until the owner requires it, each revision commit is pushed and
passes parity and host as the pull request's head before the next is pushed (Known gaps). The
head alone still gets `parity-negative-control`, `host.yml`'s tests and the checks that read no
golden file. The rule holds on `main` only for pull requests merged with a merge commit (Known
gaps). Its package rule (mode-compiler.md §8.3, lane G) fails a pull request
that changes a committed package's `soundHash` or `controlHash` (a golden preset's, a corpus or
factory package's in `dsp/tests/golden/presets/MANIFEST` or `firmware/factory/MANIFEST`) without
the "package-change" label and a `Package-change: <cause>` line in its description, and counts a
package preset's changed render as an engine change unless its package changed too and the pull
request touches no path-trigger path without a bump; no package is committed yet, so it binds
from sound revision 2.
Since 2026-10-06 this binds on GitHub: branch protection on `main` requires all 23 CI checks
(every `parity-*` leg, `parity-summary`, `sound-rev-gate`, and the `host` and `plugin` jobs),
an up-to-date branch and code-owner review (Known gaps has the caveats); lane G's eight new
checks are not required yet.

**The gates were shown to fail.** A one-ULP change to a binary32 filter constant fails check
on MSVC and on the emulated M7 (the feedback tamer's diffuser gain: 16 of 28 presets; a reverb
allpass gain: 7), and so does DetMath's ln 2 written at binary32 precision (6 of 28); the same
presets fail on both, and each mutant still renders identically on both. A one-ULP change to a
binary64 DetMath coefficient leaves every corpus output bit unchanged (ln 2 in `Exp2D` on MSVC
and the M7, 1/3! in the sine kernel and 2/π in `SinCosD` on MSVC): each DetMath result is
rounded to binary32 before it reaches the signal, and one binary64 ULP moves that rounding
only when the result lies that close to a binary32 rounding boundary, roughly one evaluation
in 2²⁹. Only the path trigger catches such a change, which is why §5.12's rule is "any change
that can change output", not golden coverage. A contracting build is refused by the configure
check (`-ffp-contract=fast`, `/fp:contract`); through the test-only escape it misses 25 of 28
presets and the harness refuses check and mint. Check mode fails on each edit
`golden_check_edits` makes to a copy of the file (one per-second hash, the per-second list cut
short, the ring sizes, the generator version, a counter, a counter the harness does not count);
the harness of the mint (`06557f3`) passed all of them but the changed counter. The gate's
self-test runs 26 synthetic pull requests on every run.

**What remains.** The rest of profile step 10 ([Known gaps](#known-gaps-and-deferred-work)) and
the nightly legs (profile step 11). On hardware, the DWT measurements are taken (2026-10-07, on
revision 1; [the silicon record](design/reviews/rev7-silicon-record.md)): they keep gradual
underflow and the engine's flush, and measure `Restart`; the decisions they leave (explicit FMA,
polynomial kernels or tables, the pedal's default load mode) and the CPU budget's fix remain,
each adopted change bumping the revision. The resampled 48 kHz plugin mode, the mode compiler,
the `.bsp` preset package and the device link with its PARITY check ([Next steps](#next-steps-recommended-order) 3–5).

## What the engine does today

One `brainscape::Engine` (`dsp/`: ≈4,400 lines of platform-agnostic C++17, the test-signal
generator and the parameter display table included; no allocation and no locks in the audio
path; no libm at all, with in-tree math (`DetMath`) for every transcendental and the IEEE
square root; an opaque engine object whose floating-point code stays in private headers)
implementing:

- **Granular core** — 64 POD voices, split-32.32 phase, resolve-once-at-birth
  scheduling, periodic↔Poisson jitter morph, overlap-referenced cubic density with a
  dithered fractional ceiling, spray (guard-reflected), ±24 st pitch with cents spread,
  per-grain reverse, equal-power pan, tiered interpolation (8× cubic Hermite / linear)
  with a bit-exact integer path at unity rate, coherence-aware `N^−p` normalization,
  and freeze as a pinned anchor (holding onset-mark positions too) with per-sample
  re-anchor-on-wrap. Wave 1 (sound revisions 4–7): the mode's trigger sources (the free-running
  scheduler only with `periodic`), bursts and intermittency, a pitch set of up to 8 weighted
  entries played by `cycle` or `random` under the transpose, micro-loops that re-read their
  region up to 16 times with a decay as the position ages, and a voice count.
- **Feedback path** — a fixed taming chain (DC → HP 100 Hz → feedback-dependent LP →
  soft saturator → allpass diffusion) that makes **feedback up to 1.1 a bounded
  self-oscillation feature**, with counter-keyed TPDF dither so the loop decays to
  *exact* silence.
- **Post chain** — ordered, bypassable stages: stereo chorus-class mod, the Space-knob
  stereo delay (damped, DC-blocked regeneration; a time change glides the tap and bends
  pitch like tape instead of clicking), a Clouds-style Dattorro/Griesinger
  reverb with multi-tap early output, and a double-sampled SVF with continuous
  LP→BP→HP→Notch morph (equal-power laws throughout).
- **Trigger layer** — spectral-flux onset detection (512/256, in-tree FFT) with
  adaptive whitening, a relative whitening floor, Dixon's peak-picker and growth
  hysteresis; a mark ring feeding mark positioning (the Strum-family mechanism), and
  onsets as an OR'd trigger source with oldest-steal allocation, both chosen by the loaded
  mode (sound revision 2); footswitch and MIDI-note triggers through `Trigger()` or stamped
  events, each firing only when the mode lists its source (sound revision 4), a burst per
  trigger, and never dropped otherwise; and a counted onset indicator for
  the trigger LED. End-to-end onset→grain latency: **5.3 ms** (measured).
- **The permanent parameter-ID table** ([mode-compiler.md](design/mode-compiler.md) §4): 82
  rows, each with a kind, a domain bitmask (what a change rebuilds) and the sound revision that
  made it a leaf. IDs 1–26 keep their numbers (4 is now `wet_trim_db`, the wet signal's trim,
  and 8 `layer0.pitch.transpose_st`, an offset over the pitch set) and with wave 1's 29–31 and
  57–59 (sound revisions 4–7) are the 32 Leaf rows the engine plays; 27 and 28 are Retired since
  sound revision 2, their structure in the mode; 32–56 and 60–68 are Reserved under their final
  names until wave 2 or 3 builds them; 69–76 are the eight
  macros, 77 and 78 the freeze and expression performance controls, 79–81 Reserved, and 82 the
  effect volume, a Global device setting that scales the wet signal with the trim and that every
  load and `Restart` keeps. `SetParam` stores only Leaf and Global rows, and `LoadPreset` reads
  only Leaf rows (any other id is unknown and makes the load inexact). A change rebuilds what
  its row's domain bitmask names (design §7.2, R1). Tapers, step counts, display text, groups and host flags for every
  row, Reserved ones included, live beside the descriptors in `dsp/` (`ParamDisplay.h`), so a
  pedal pot and a plugin knob at the same position give the same plain bits. The host flags
  follow the recommended host model (owner question Q12, provisionally: only Mix, the macros,
  the effect volume and the performance rows automatable). The plugin registers, since lane D's
  curation slice, the Leaf rows (only Mix automatable), Freeze, the eight macros (a host move is
  a `MacroMove`), the expression pedal and the effect volume (design §9.2); the leaf mirrors
  follow a macro's fan-out through the engine's own evaluator, but the fan-out is not yet
  reported to hosts. Every consumer, the golden
  harness and the plugin included, iterates the Leaf rows, and the plugin's 32-bit touched mask
  is a per-leaf set.
- **The preset package** ([mode-compiler.md](design/mode-compiler.md) §5–§6, lane B), the
  part of the mode system the firmware links, in `dsp/src/blob/` behind `brainscape/Preset.h`.
  `PresetState` is the whole decoded package: `soundRev`, up to 128 leaves, the `ModeBlob`
  (`Mode.h`: MODE's chunks at their caps, 1,484 bytes), CTRL and the stored performance state,
  2,656 bytes with one layout on every target (`static_assert`ed). Its defaults are the
  schema-default mode, which plays as revision 1 does, so every leaf-only producer plays what
  it played. `DecodePreset` checks the 128-byte header, the sections and their order, the three
  SHA-256 hashes, STAT, MODE's nine chunks in their canonical form (zero padding, nothing past
  a count, no optional chunk equal to its default, `features` equal to what the content needs
  and supported by this build), CTRL and META, with one of 62 error codes per rule; an unknown
  MODE chunk or feature bit is `UnsupportedFeature`, named, a macro or expression target on a
  later wave's leaf (a Reserved row) is `UnsupportedTarget`, named, and an unknown section
  after MODE is skipped and kept. `ValidateMode` holds a state built in memory to the same
  rules (nothing past any count, STAT's leaves included), then to E8–E11 and to absent
  elements' leaves at their defaults. The encoder gives one encoding per state: decoding then
  re-encoding gives back every package's bytes, META, JSON and unknown sections carried. It is all integer-only: built for the Cortex-M7 (`-mgeneral-regs-only`)
  its objects hold no floating-point instruction, not even a move, and import only `memcpy`
  and `memset` (*measured*). This build supports the onset source and mark positioning (sound
  revision 2), source selection (4) and pitch sets (5), so a package decodes with them, the
  default structure otherwise and any macros.
  Also from lane B: the SHA-256 core moved into
  the engine library (`brainscape::Sha256Hasher`; the tests keep `golden::Sha256` for hex), and
  the random-number keys gained the design's extension (layer, same-frame ordinal, purposes 8
  and up, mixed into the frame's hash), which keeps every revision-1 key and aliases no other
  stream at a fixed frame offset; nothing draws an extended key yet. The compiler's exact binary32 reader and shortest writer
  (`compiler/src/Number.*`, design §6.5, owner question Q1: no `fast_float`) landed with it,
  with the two eight-digit exceptions and the plugin's lenient typed-text entry point.
- **The preset compiler** ([mode-compiler.md](design/mode-compiler.md) §2, §3, §6.4, §8, lane
  A): the library `brainscape_compiler` in `compiler/` (namespace `bsc`, desktop hosts only)
  and its tool `bspc` in [`tools/bspc/`](../tools/bspc/README.md). A strict RFC 8259 reader
  (UTF-8, every escape and surrogate pair, no duplicate keys, depth 16, line, column and JSON
  pointer on every error) feeds schema 1, written once as a table in code that a reader and the
  canonical writer both walk: every key, its order, type, range, default and the wave that
  plays it. `Compile` runs the design's eight steps, errors E1–E12 with JSON pointers, the
  package with its META, CTRL and the formatted, stamped JSON section, then decodes and
  validates what it wrote and requires the same state; an empty document compiles to the
  default mode's committed `modeHash`. It is integer-only: numbers go through the exact
  reader, floats are compared as bit patterns, and no library number formatting is used.
  `bspc` compiles, decompiles (rebuilding from STAT, MODE, CTRL and META when the JSON section
  is absent or stale), formats (`fmt --check`), verifies, stamps, lints (L1–L9, `--factory`
  making L4 and L7–L9 errors, the denylist of other makers' marks as a floor), diffs by field,
  derives targeted leaves from macro positions and solves positions from leaves, runs the
  round-trip checks with a hash manifest, and migrates BSWS v1 sessions. What lint and derive
  compute in floating point is two guarded `dsp/` functions, `EvalMacro` (the design's
  evaluator, §3.3) and `NearGuardMs` (`brainscape/ModeEval.h`), whose bodies the engine's macro
  and expression events run too. A document compiles with the default structure, the onset
  source and mark positioning, any subset of the sources and pitch sets (any macros, positions
  and expression assignments included); anything else is error E6 naming its wave, and later
  waves' leaves are accepted at their defaults only. The plugin's typed text is now read by the
  same exact reader.
- **The mode runtime** ([mode-compiler.md](design/mode-compiler.md) §7, lane C, sound revision
  2). `LoadPreset` follows the design's steps 0–5: it validates the mode and CTRL with
  `ValidateMode`'s rules (STAT's leaves are canonicalized and counted instead) and applies
  nothing when they fail; counts a missing leaf only if its row existed at the package's
  revision (`sinceRev`); copies the mode and CTRL into one active mode in the Warm arena,
  compared with the previous one by content (`ModeSwitches`), never by `modeHash`; and counts
  stored performance state it cannot play yet. The `SpilloverLoad` event's `id` picks Trails or
  FastCut (every grain sounding at the load fades to zero over 128 frames, each load fading its
  own). `MacroMove` (4) and `Expression` (5) events apply the mode's targets and CTRL's
  assignments as `SetParam` events would, through the exported `EvalMacro` and
  `EvalExpression`. The wet signal's gain after the post chain is `WetGainTarget`: the trim and
  the effect volume in decibels as one gain, exactly 0 at the cutoff's minimum (shown `Kill`).
- **The Mix law** ([mode-compiler.md](design/mode-compiler.md) §7.1, sound revision 3): the dry
  signal stays at unity up to the Mix knob's middle and the wet is at unity from it, so engaging
  a mode below the middle never takes dry level away; Mix 0 is the dry input and Mix 1 the wet
  alone, bit for bit up to the sign of a zero (a −0 sample can come out +0; `MixLaw.h`).
- **The determinism profile's engine side** ([determinism-profile.md](design/determinism-profile.md)):
  - a build profile (`cmake/BrainscapeFpProfile.cmake`: contraction off, no fast-math, no
    `errno` square roots) that `brainscape_dsp` passes on PUBLIC, tripwire headers, and a
    configure-time check, with a self-test, that rejects forbidden FP and LTO settings on
    every target that compiles or links the engine, the JUCE targets included;
  - a full control-word guard (`dsp/src/detail/FpEnvGuard.h`): every entry point (`Init`,
    `Reset`, `ClearHistory`, `Process`, `SetParam`, `PlanMemory`, `Canonicalize` and the
    taper and display functions) writes the complete word and restores the caller's;
  - gradual underflow with a deterministic in-code flush of every recursive state, so FTZ
    hosts and the M7's FZ give the same bits and silent tails raise no subnormal flags;
  - a NaN-free boundary: canonical parameter values decided on the bit pattern,
    `SanitizeInput` and `ConditionInput24` for input, and finite input giving finite output;
  - the engine defects the profile listed, fixed: mono in-place aliasing, the dither key
    (it now folds the whole 64-bit sample counter), and the three block-split mechanisms,
    with pin-eligible marks (a frozen grain positions only at marks recorded before the pin);
  - the state API: `Engine::Restart` (the exact post-`Init` state, parameters kept; on an
    engine that has rendered nothing since its buffers were cleared it skips the clears, so
    a load before the first block is real-time safe), a
    random-number epoch that Spillover loads restart, `LoadPreset(PresetState, Exact or
    Spillover)` in the profile's fixed order with a report of inexact loads, frame-stamped
    events in `ProcessContext` (parameter, freeze, trigger, Spillover load, macro and
    expression moves) applied at
    their frames by splitting the block inside `Process` (freeze settling once per frame),
    the `EventQueue` transport (it refuses and counts overflows and out-of-order stamps, is
    cleared with a restart, and retires events so a staged preset can be reused), and a
    toolchain ID (compiler, version, target, FP-flag hash).

### Verified behavioral contracts (the test suite enforces these)

- **Bit-exact block-split invariance within one build**: identical output whether the host
  chops the stream into 1-, 7-, 48-, 127- or 512-frame blocks, for the configurations the
  suite exercises — dither, jitter, spray, reverse, pitch, onset-mark positioning with onsets
  that actually fire, freeze engaged mid-render and held past the re-anchor point, grain
  positions on the ring's far rail, and queued manual triggers; the golden corpus agrees at
  1-, 48- and 512-frame blocks, mixed patterns and random sizes of 1–512 frames on every
  toolchain. A Debug assertion
  checks that no grain reads a ring frame the current block has already written ahead of the
  live write head (for every grain the ring can hold: a ring shorter than one block plus a
  grain's span cannot be invariant, determinism-profile.md §5.7). The freeze/onset-mark/
  far-rail defect found on 2026-10-05 is fixed (see determinism-profile.md, "The block-split
  bug").
- **Bit-exact degenerate-delay null** through the int16 ring (the one-engine proof).
- **The Mix law, exact**: Mix 0 plays the dry input bit for bit, hostile input included, but for
  the sign of a zero (a −0 sample comes out with the wet sample's sign), Mix 1 the wet alone, and every
  Mix between plays dry·min(1, 2(1 − m)) + wet·min(1, 2m) bit for bit against the wet rendered at
  Mix 1; a Mix move lands on the endpoints' bits.
- **Level consistency** within ±1 dB across the whole overlap sweep, including
  coherent, decorrelated, frozen, and mark-anchored populations.
- **Feedback decays to exact zero** (not just quiet) and self-oscillates bounded
  above unity.
- **Deterministic, reproducible renders** — every random draw is keyed on a free-running
  counter (relative to the random-number epoch), never on block structure or call history.
- **Exact restart and stamped events**: a used engine after `Restart` renders exactly what a
  fresh `Init` renders (feedback, freeze, marks, every post stage, a Spillover epoch and
  queued triggers left behind), and events at odd frames render what a wrapper splitting its
  blocks there renders, at block sizes 1 to 512, a freeze released and re-engaged at one
  frame and Spillover loads while frozen included. A Spillover load never reconverges with the
  Exact render (4 s measured, as the profile states); followed by `Reset` it reconverges after
  0.250–0.264 s, the record's figures. The golden corpus checks the same on every leg: after a
  `Restart` or an Exact load mid-render, the rest of the output equals a render of the rest
  from the exact-restart state.
- **Bit-identical across conforming builds** on the golden corpus (above), whatever the
  host's floating-point environment: renders under FTZ|DAZ with round-toward-zero, or with
  every exception unmasked, match the clean render, and flushing forced on inside the guard
  reproduces every golden vector on the 24-bit grid (`golden_forced_flush` on the host, FZ on
  the emulated M7). One vector feeds
  subnormal input where the plucks are silent and must output subnormals, which no flushing
  mode can produce: forced flushing must change it, so a guard that lets a caller's flush bit
  through fails the corpus. The corpus renders eight presets again on a fresh engine from a
  hostile caller (Init included) on every leg, the M7 too, and a whole-corpus hostile run
  matches the clean one.
- **The plugin reproduces the engine bit for bit**: host blocks of any size against the
  engine driven by stamped events from `LoadPreset(…, Exact)`, one `Process` call per chunk of
  at most 512 frames; restores, MIDI, host automation and scripted events at their frames, at
  every host block pattern; offline and real-time bounces with Restart on transport start equal
  a fresh render, with MIDI, with an automation lane (the start folds the first block's values
  into the load) and with the wrapper passing the offline mode before every block; a stamp made
  before the restart is void; the audition render equals the engine's from the exact-restart
  state, and its WAV and hashes (whole and per second) match it.
- **NaN-free boundary**: a fuzz test feeds NaN, ±inf, subnormal and ±`FLT_MAX` input and
  parameters under a hostile environment; the output stays finite and equals the run fed
  sanitized input and canonical values.
- Onset acceptance: plucks count once each (including 200 ms decays and −30 dB
  levels), hiss and steady tones fire nothing, held-distorted sustain chatter is
  bounded, mid-stream `Reset()` fires nothing.

**Suite** (`ctest`): `dsp_unit` (200 test cases / ~4.1M assertions in Release, 199 in Debug; 29 are the package's, `test_blob.cpp`, 7 the
evaluators', `test_mode_eval.cpp`, 17 the mode runtime's and the Mix law's, `test_modes.cpp`, and 29 wave 1's,
`test_wave1.cpp`), the
forced-flush tests, the undefined-symbol audit and its negative control, the configure-check self-test, `golden_check` (every golden hash of sound
revision 7), `golden_check_edits` (check mode fails on edited copies of the golden file), the
corpus's forced-flush control `golden_forced_flush`, `blob_fuzz` (200,000 mutated packages, a
quarter of them structurally, against a committed digest of every decoder and validator
verdict that the emulated M7 reproduces) and `blob_fixtures` (the 18 frozen packages), and the compiler's `compiler_number_unit` and `compiler_number_hashes` (the
number code's per-pull-request sets against committed hashes), `compiler_unit` (32 test cases:
the JSON grammar suite, every rule E1–E12, the canonical form, packages, decompile, verify and
diff, lint and derive, and two committed digests that every host must reproduce, the packages
of 400 random documents and the verdicts of a 20,000-mutant reader fuzz) and
`compiler_roundtrip` (`bspc roundtrip` over seven example documents against their committed
hash manifest); a plugin build adds the wrapper tests (32 test cases), the editor snapshot and
a hosted-VST3 check. The `dsp/` and compiler
tests are green in Release and Debug with MSVC 19.40 and GCC 11 and in Release with Clang 14,
the compiler's digests and manifest identical on all three (GCC 14, and Clang 14 in Debug,
were last run before the ID table; at sound revisions 2 and 3 MSVC AVX2 ran the whole suite too;
at revisions 3 to 7 GCC 11, Clang 14 and the emulated M7 ran in lane F's review, GCC's Debug suite
at 6 and 7, and at revision 3 the emulated M7 ran in CI's `parity-m7` job too), and the emulated M7 runs
the package fuzzer and fixtures beside the golden check; the plugin tests with MSVC, Release
and Debug (Linux and macOS plugin builds are left to CI).
**CI**: `host.yml` (Linux/macOS/Windows with `-Werror`, Debug+ASan/UBSan, Release+ASan, a
compile-only Cortex-M7 build), `parity.yml` (seven host legs and the emulated M7 checking the
golden file, block-size, random-block, hostile-FP-environment and, on the M7, forced-flush
perturbations, a contraction-on negative control and the static audits, all gating; from lane
G, every one of those legs and the M7 also runs the package fuzzer against its committed digest
and the frozen fixtures, `bspc-roundtrip` runs the compiler's tests and compiles every
committed document on the seven host legs, each set against its required `MANIFEST` and with
`.gitattributes` keeping its documents LF, with `parity-summary` requiring their package
manifests identical, `blob-libfuzzer` fuzzes the decoder for 90 s, and the compiler audit runs
its source ban in `parity-audits` and its import check on the GCC and Clang legs),
`sound-rev.yml` (the sound-revision gate with the package rule), `sound-rev-render.yml` (each
revision a pull request carries below its head's, rendered on every parity leg, parity-host's
seven and the emulated M7, at every commit at it a later revision is built on, with the renders
that commit's own parity.yml makes; not yet run on GitHub in this form), `nightly.yml` (the number code's exhaustive round trip on x86-64 and arm64, and 30
minutes of libFuzzer from a kept corpus) and `plugin.yml` (every format on three OSes, Release
and Debug). Everything but lane G's additions first ran on GitHub on 2026-10-06 (Known gaps).

## How it was built (methodology)

Every increment followed the same loop: **implement → contract tests green →
adversarial multi-agent review with empirical probes → fix → commit**. Six review
rounds so far produced **232 findings** (engine design: 60; skeleton: 34; grain engine: 26;
post chain: 26; trigger layer: 23; parity + companion designs: 63), the large majority
verified by compiled probes, bit-exact replicas, mutation testing, emulated Cortex-M7 runs, or
disassembly rather than inspection. Highlights of what that caught before it could ship: a
voice population that varied −3.5 dB with the DAW buffer size, a reverb that was silent for
its first 107 ms, an unbounded filter at its own knob stop (+76 dB), a detector that free-ran
on rig hiss, a normalization law wrong by +18 dB on the flagship delay modes, and — in the
parity work — a block-split defect under freeze and onset marks (since fixed) and a
licensing conflict in libDaisy. Review
records live in [docs/design/reviews/](design/reviews/).

## Known gaps and deferred work

- **Parity holds on silicon; the CPU budget does not.** The owner prototypes on a Daisy Seed Rev7
  (STM32H750, PCM3060), with a custom H750 board later; the Rev7 parity, DWT bench and live-audio
  images are built at sound revision 7 and fit their memory; at revisions 2 to 7 they passed
  every off-hardware check (the parity stream's code matched each revision's golden file and the
  packages' `MANIFEST` under `qemu-arm -cpu cortex-m7` in the parity image's own memory
  placement) ([firmware/README.md](../firmware/README.md)). On the owner's Rev7 on 2026-10-07
  ([the silicon record](design/reviews/rev7-silicon-record.md); the captures and the commands that
  re-check them in [`firmware/records/rev7-2026-10-07/`](../firmware/records/rev7-2026-10-07/README.md)),
  the parity image rendered the golden corpus bit for bit at sound revision 1 (28 presets) and
  revision 3 (33 presets, its 18 packages matching `MANIFEST`), each at `maxBlockSize` 512 and 48
  and from a hostile caller. The DWT pass ran on revision 1's three bench builds and found **the
  worst-case CPU budget not met**: with the engine's code in ITCM and warm caches, the nominal
  64-voice row peaks at 99.1 % of the 480,000-cycle block (100.3 % cold), the pessimistic rows at
  135.5 % (20 ms grains), 168.5 % (1 ms grains) and 146.3 % (under events), and the corpus's
  `dense_1ms`, a live-image preset, at 118.6 %.
  A fix is under design, with owner decisions pending; none of it is in this tree. Still to run
  on the board: the live image's checks, the bench at revision 3 and every image at revisions 4
  to 7. The images use libDaisy's
  ST USB code and must not be distributed. The first GitHub runs (2026-10-06) rendered the
  golden corpus bit-identically on every leg: Windows x64 (MSVC, MSVC AVX2), Linux x64 (GCC, Clang), Linux arm64 (GCC 13), macOS arm64 (AppleClang 15 on
  `macos-14` and AppleClang 21 on `macos-latest`) and the Cortex-M7 under QEMU 10.2.3, whose
  engine archive was byte-identical to a local build (`4f4ddaa3…`). The ARM toolchain's MD5
  passed against Arm's download and QEMU is now pinned to the commit that run recorded. The
  run's failures were all in tooling (fixed in #1; see the record there): LLVM 21 folding
  the harness's subnormal bit tests into floating-point compares under the hostile control
  word, two symbol-audit allowlist gaps, and a Windows smoke-test path. A hardware-in-the-loop
  runner in CI and the nightly legs (full-system QEMU with the interrupt `FPDSCR`, exhaustive
  DetMath accuracy, toolchain drift) are not built.
- **How far the gates bind.** Branch protection on `main` requires all 23 CI checks, an
  up-to-date branch and code-owner review; [`.github/CODEOWNERS`](../.github/CODEOWNERS)
  names @zchdvs for `dsp/` (`dsp/tests/golden/golden.json` included), `cmake/`, the root
  `CMakeLists.txt`, the arm toolchain file, `.github/workflows/` and `tools/ci/`, as profile
  §5.12 and §6.1 require, and, from mode-compiler lane G, `compiler/`, `tools/bspc/`,
  `firmware/factory/` and the root `.gitattributes` (mode-compiler.md §8.1, §8.3; the golden
  corpus's packages sit under `dsp/`).
  Lane G adds eight checks, which have run and passed (Mode compiler lane G's open ends), for
  the owner to require: `bspc-roundtrip` on
  `linux-x64-gcc`, `linux-x64-clang`, `linux-arm64-gcc`, `windows-x64-msvc`,
  `windows-x64-msvc-avx2`, `macos-arm64-appleclang` and `macos-arm64-appleclang-latest` (each
  named `bspc-roundtrip (<leg>)`) and `blob-libfuzzer (linux-x64-clang)`; its other checks are
  steps of jobs already required, and `nightly.yml`'s jobs are not pull-request checks.
  `sound-rev-render` (`sound-rev-render.yml`) is not required either. Require only the job of
  that name, never `sound-rev-render (list)` or a `sound-rev-render (<leg>)` job: a pull request
  with at most one revision skips the render jobs as one job whose name is not expanded, so a
  required one would never report, and the job `sound-rev-render` already requires the listing
  and every render job. That job has no `needs`, so its check is pending from the start of every
  run (a base change or a reopen keeps the head commit, and behind `needs` the older run's result
  would stand until the renders finish); it lists the revisions itself and reads the run's jobs
  from the GitHub API until they finish. It renders each sound revision a pull request carries
  below its head's on every parity leg, one job per `parity-host` leg and one emulated-M7 job per
  listed commit, at every commit at it a later revision is built on, each with the renders that
  commit's own `parity.yml` makes on the leg (`tools/ci/parity_plan.py` reads it, so a render or
  harness option the head adds later does not reach an older revision; a commit whose
  `parity.yml` does not run a leg gets the head's renders there); the runners and the pinned M7
  toolchain are the head's. In this form it has run only locally: step 4's r4, r5 and r6 passed
  every run on MSVC SSE2 and AVX2 (Windows, 71 and 70 s), GCC 11 (54 runs, 191 s), Clang 14
  (54 s) and the emulated M7 (7 runs per commit, one process per commit as its jobs run: 329,
  337 and 375 s for r4, r5 and r6, each archive the one recorded above), the last three in
  Docker on 4 CPUs; a run with an r4 whose golden file had one hash altered and an r5 that did
  not compile failed the r4's three MSVC runs and the r5's build and passed r6; and the gate's
  self-test keeps the listing of the two states the review of its first version found
  unrendered (an unminted engine change at r4 under the r5 bump, beside a side line at r4 merged
  after that bump; and an r4 whose last commit took its golden file back to the base's), which
  failed that version's GCC and M7 runs. `test_render_revisions.py` (130
  cases, in the listing job) checks the YAML reader against PyYAML and the reading of
  `parity.yml` against a recorded plan, that the reader refuses 21 kinds of `parity.yml` change
  it cannot follow, that the workflow repeats parity's setup steps, the required job's verdicts
  over the run's jobs (re-runs included), a commit subject a Windows console cannot print, and
  every fail-closed path. The required job's reads of the GitHub API were tried on an earlier
  parity run of this repository, but the job itself first runs on GitHub, as do Linux arm64 and
  the two macOS legs, which cannot run here. While the owner requires it, a pull request may
  push several revisions at once; until then, a pull request that carries several revisions
  pushes each revision commit and lets parity and host pass it as the head before pushing the
  next. The head alone still gets `parity-negative-control`, `host.yml`'s tests and the checks
  that read no golden file. It also runs on description edits, since its listing depends on the
  base and a base change fires only that event. Only
  collaborators can apply the "sound-neutral" label, so today only the owner can waive the path
  trigger; the same holds for the "package-change" label, which the owner creates in the
  repository before the first package lands. Caveats: every gate runs the pull request's own
  code (a pull request that edits the harness or a workflow can pass its own checks), so
  code-owner review of those paths is the real control; and the owner is the only code owner and
  cannot approve their own pull requests, so owner merges go through the administrator bypass,
  which the protection allows on purpose. Running `sound-rev-gate` from the base branch
  (`pull_request_target`) would not close the first caveat alone, since a pull request can add a
  workflow whose job has the same name.
- **Squash and rebase merging are still allowed** (2026-10-07). `sound-rev-gate` checks
  GitHub's test merge of a pull request, and its per-commit rule holds on `main` only when the
  pull request lands as a merge commit. A squash merge of one that carries r2 and r3 lands a
  single commit that raises `kSoundRevision` from 1 to 3, and no check reads `main`'s history
  afterwards; a rebase merge lands copies of the pull request's commits, so a branch built on
  the originals claims revisions `main` already has until it is rebased. Owner merges go
  through the administrator bypass, so the method is picked by hand each time: merge with a
  merge commit until the owner turns squash and rebase merging off (Settings, General, Pull
  Requests).
- **Profile step 10 is not finished** (determinism-profile.md §8.4 lists it). Not yet built:
  the engine-side coverage counters (births, steals, reverse and mark-positioned births,
  re-anchors, far-rail clamps, blocks with an underflow flag, the write-ahead counter) and the
  gcov thresholds of §6.1 (revision 1 substitutes the harness's 18 counters with per-preset
  minimums, ablations, and the Debug write-ahead assertion over the whole corpus); host blocks
  up to 8,192 frames through the wrapper's chunker against the corpus (§6.4; the wrapper tests
  reach 4,096 against the engine); the ODR controls, renders and the audit with `dsp/` and
  plugin objects in swapped link order (§6.3, §6.4); the literal-bit audit (§6.3); the plugin
  format targets' flags compared with the engine's through `compile_commands.json` (§6.3);
  the negative control with one DetMath function swapped back to libm, as a hash control
  (§6.4); the Rosetta 2 and Prism host legs (§6.2); and a mint job: revision 1 was minted
  locally, and its pull request must pass every x86 leg and the emulated M7 against the
  committed file in one CI run, the deviation §6.1 records.
- **The Rev7 bring-up is folded in at sound revision 2** (2026-10-07: `claude/rev7-bringup`, PR
  #4, merged into the mode-compiler branch, then adapted). The two branches merged with one
  textual conflict (README's firmware row) but did not build: the live image assumed one name per
  table row (82 rows now, 26 of them leaves), set the retired rows 27 and 28, and the parity image
  could not read the corpus's packages, the engine archive's linked code overflowed the 64 KiB
  ITCM (76,871 bytes in the parity image), and `sizeof(Engine)` (7,424 bytes on x86-64)
  outgrew the 7 KiB slot that `brainscape_parity_stream --placement` mirrors (the merge commit
  raises the slot to 8 KiB, so the host build and its tests pass at every commit). As adapted:
  - **Packages compiled in.** The Seed has no file system, so every image (and
    `brainscape_parity_stream`, which renders as the parity image does) links the corpus's 18
    committed packages as a table generated at build time from `presets/MANIFEST` and the `.bsp`
    files (`dsp/tests/golden/EmbeddedPackages.h`, `GoldenPackages.cmake`); the harness's
    `LoadPackage` reads it when linked. The parity image renders the revision-2 corpus (14
    vectors, 33 presets, 709 s) from them.
  - **The parity stream, format 3.** Before the vectors it lists every package the corpus loads,
    as the program decoded it, and a preset that starts from a package names it with its sound
    and control hashes; `tools/hil/parity_check.py` requires the packages to equal `MANIFEST`'s
    hash for hash and the preset fields to equal `golden.json`'s (the package rule), checks the
    package count against `parity-end`, and compares `mutedFrames` and the other new counters
    with the rest. A revision-1 image's stream (format 2) is refused.
  - **The live image** selects modes instead of parameters: `onset on|off` and `marks on|off`
    (and `set onset|marks V`, ≥ 0.5 on, as sessions migrate) Spillover-load the current leaves
    with the mode of a corpus package that differs from the default mode in exactly that
    structure (`lone_busy`, `reverse_mark_aging`, `strum_marks`, checked at boot); its presets
    take their package's mode; `preset N cut` is a FastCut switch; `macro NAME POS` and
    `expression POS` send events 4 and 5; `set` reaches the 26 leaves and the effect volume
    (ID 82) through a table checked at compile time against `kLeafParams`. Its presets and
    structures are a host ctest, `firmware_live_presets`.
  - **The bench** takes onset grains at marks from `strum_marks`'s mode, and its event stream's
    loads are mode switches, alternately FastCut and Trails.
  - **ITCM** holds the engine's code and constants, not the package decoder, encoder, SHA-256 or
    test-signal generator, which never run in the audio path: parity 54.0 KiB, bench 53.8, hooks
    54.3, live 56.0 of 64 (about 8 KiB left in the live image, which a wave that grows the
    engine must budget). The constants include the shared tables (C++17 inline variables:
    `kParamTable`, `kLeafParams`, `kLeafOrdinal`, `kDefaultModeHash`), which the linker would
    otherwise take from a golden-harness object and leave in QSPI;
    `firmware/cmake/ItcmCheck.cmake` fails the build when any COMDAT section of the engine's ITCM
    members lands outside ITCM. The engine slot is 8 KiB (`sizeof(Engine)` 7,168 bytes on the
    M7); the 136 KiB Warm arena holds `PlanMemory`'s 131,296 bytes at both block sizes.
  - **Verified** (firmware/README.md §9): all five images build with `-Werror` and fit; their
    engine archives equal the M7 oracle's, Windows and Linux builds alike (`89b73b51…`, hooks
    `71a38352…`; at sound revision 3 `1d6fe1dc…` and `023a9fa9…`, with ITCM use within 0.1 KiB
    of the figures above); the parity stream in the
    image's placement under `qemu-arm -cpu cortex-m7` matches the revision-2 `golden.json` and
    `MANIFEST` on the whole corpus at `maxBlockSize` 48 and 512 and on the quick set from a hostile
    caller, and so does each revision's from 3 to 7 (lane F's review; at revision 3 CI's
    `parity-m7` job runs it at `maxBlockSize` 48 too, and the Rev7 itself matched at 48 and 512 and
    from a hostile caller); the static audits report 0 on the firmware build (256
    translation units); `parity_check.py` passes the intact stream and gives the intended verdict
    on 14 damaged revision-2 streams. On the board since (2026-10-07): the parity and bench items
    of firmware/README.md §6 ([the silicon record](design/reviews/rev7-silicon-record.md)); the
    live items are still to do.
  - The firmware now spells IDs 4 and 8 `WetTrimDb` and `TransposeSt`; the old `OutTrimDb` and
    `PitchSt` in `Params.h`, kept for this merge, remain only for the `tools/parity/bugcheck`
    probes. The live image's 16 `PresetState`s (2,656 bytes each) sit in `.data`, since their
    default is not zero, so their 41.5 KiB load image is in QSPI and copied at boot.
- **Mode compiler lane B's open ends.** MODE's vocabulary of later waves (STEP's `pos_sel`,
  modulator shapes, route and link endpoints, several ranges) is decoded and validated but
  provisional until the wave that plays it (design §1.4 principle 5); E11's fit in the memory
  tiers is W3's; `TargetAbsent` cannot fire with today's table (every leaf of an optional
  element is still a Reserved row, so `UnsupportedTarget` comes first) and is tested through
  `ElementPresent`; the frozen `w1-leaf-*.bsp` fixtures changed verdict at sound revision 6,
  when `layer0.decay_ms` became a Leaf row, and `w3-leaf-*.bsp` (`layer0.level_db`, ID 32) hold
  their role until W3. META's byte layout and its free-text limits (author
  64, description 512, eight tags of 32) are this lane's, for lane A's schema to adopt. Lane C
  (sound revision 2) widened the supported features to onset and mark, which re-minted the
  fuzzer's verdict digest, took 27 and 28 out of the samples' macro targets, made the revision-1
  fixtures load inexact (their 27 and 28 are unknown ids) and added `r2-onset-marks.bsp`, which
  must load exact on every later build; the fixtures' recipes now spell out each revision's
  leaves, so they keep rebuilding the committed bytes. **The
  number code's committed hashes are provisional:** measured on x86-64 (MSVC 19.40, GCC 11.4,
  Clang 14; the exhaustive one with MSVC and GCC) and, the per-pull-request sets, on the 32-bit
  Cortex-M7 under qemu (arm-none-eabi GCC 10.3, run serially by the lane B review), but the
  design commits them only after one linux-arm64 and one macOS run (§10.2), which no host here
  offered. Lane G's `bspc-roundtrip` legs run them on linux-arm64 and both macOS legs, and
  their first GitHub run reproducing them is still the gate before the number code is relied
  on; a leg that differs is a finding, not a reason to re-mint. The fuzzers' M7 run, the
  libFuzzer leg and the number-check legs are CI jobs since lane G, and the plugin's typed-text
  parser now uses `Number` (lane A).
- **Mode compiler lane A's open ends.** The compiler admits the default structure, the onset
  source and mark positioning (sound revision 2 widened `kSupportedModeFeatures`; lane C dropped
  the shims that wrote rows 27 and 28 from the structure, re-stamped the examples and re-minted
  the property and fuzz digests); its tests compile, decode and round-trip the whole vocabulary
  of every wave with the support table widened, but
  later waves' vocabulary that the design leaves open stays provisional: tempo divisions are
  accepted as `"off"` only (W2 defines them, so anything else is E6 whatever the support),
  `quantize.scale` is a list of pitch classes, route sources are `modulator0`/`modulator1` and
  link endpoints `grain.pitch` and the like (engine §5), and a step entry's `gain` and `prob`
  default to 1. Choices the design left to the implementation: the header's `FACTORY` flag is
  set for ids under `factory.` (a pure function of the document); a macro written without
  `targets` keeps its default targets (per-key defaulting); `fmt` keeps a document's stamp and
  `compile` and `stamp` compute it, so a stale stamp passes `fmt --check` but fails `bspc
  roundtrip` and `stamp --check`; L4's "display resolution" is the display text (`FormatPlain`)
  of the leaf against the derived value; L2 compares the smallest `base_ms` the leaf and macros
  reach with the near guard at the largest size, transpose and spread they reach; `EvalMacro`
  (R6) landed in `dsp/` with lane A because lint and derive need it, as an uncalled function,
  sound-neutral by construction; `editor.ratio_gen`'s keys stay open until the editor defines
  them, but its numbers and key order are canonical; documents over 1 MiB are refused unread
  (E12); `derive --solve` keeps a stored position that lands as near as any; `bspc` refuses
  options a command does not take (exit 2). Lane G turned the compiler's tests into the
  `bspc-roundtrip` legs (with the manifest upload and `parity-summary`'s comparison) and
  `compiler_bspc_cli` into a leg check, and added the compiler audit, its source ban scoped to
  `compiler/src` since the tests cross-check against `std::from_chars`, `to_chars` and `printf`,
  and CODEOWNERS for `compiler/`, `tools/bspc/`, `firmware/factory/` and `.gitattributes`. The
  compiler's digests, like the number code's hashes, are measured on x86-64 only until those
  legs first run on GitHub. `render` came with lane E's `tools/audition/`.
- **Mode compiler lane G's open ends.** Lane G's jobs have run on GitHub since pull request #5:
  its first run (37657240905) aborted `compiler_number_hashes` on the two Linux GCC legs at a
  fortified buffer in the test's random generator (fixed in `7f20481`), and its second
  (37658865968) and those of #6 and #7 (37693138971, 37701943741) passed all eight jobs,
  `compiler_number_hashes` and `bspc roundtrip` on linux-arm64 and both macOS legs included.
  Those runs were the gate for the number code's and the compiler's digests on arm64 and macOS
  (above), and for `bspc`'s non-ASCII file names and the import check on macOS, which no host
  here offered; the "provisional" and "x86-64 only" wording above predates them. Locally the new
  steps passed with MSVC 19.40, GCC 11.4, Clang 14 (libFuzzer included, where
  CI's runner has Clang 18) and the emulated M7, and actionlint (with shellcheck) passes. For
  the owner: create the "package-change" label and require the eight new checks (above). What
  later lanes must write for the package rule: lane C's harness puts `soundHash` and
  `controlHash` (64 hex digits, as `bspc` prints them) in each package preset's entry of
  `golden.json`, and its check mode fails when a preset's recorded pair differs from the package
  it loads; lane C commits each corpus document beside its `.bsp` under
  `dsp/tests/golden/presets/` (`frozen/` stays exempt) with that directory's `MANIFEST`, and
  lane E does the same in `firmware/factory/`. Each `MANIFEST` is written with `bspc roundtrip
  --write-manifest MANIFEST -- <documents>` in its directory, paths relative to it;
  `bspc_roundtrip.py` fails a set with documents and no `MANIFEST`, and fails any document or
  `MANIFEST` that the root `.gitattributes` does not keep at LF (`text eol=lf`) or `.bsp` it
  does not mark binary, which the committed rules already do for both directories. Choices the
  design left: the cause is named on a `Package-change: <cause>` line of the description, which
  the gate requires and prints (the workflow re-runs on edits); a dropped package or a package
  preset turned back into a parameter list is a change, a re-stamp is not; an unbumped change to
  a path-trigger path beside a package change leaves the package preset's changed render to the
  hard trigger (land the two apart, or bump); the corpus commits a `MANIFEST` (since lane C), which the
  gate reads beside `golden.json`'s pairs, so a package change the render does not hear still
  needs the label; the source ban also bans the stream and locale headers and the other float
  formatters, and the import check rejects the same families as imports (a superset of libm,
  `strtof` and `strtod`), each after a self-test that it can fail; the nightly exhaustive round
  trip skips the standard library cross-checks (the committed hash is the in-house code's). Not
  built: the parameter manifest and version-hint check (design §4.5: they need step 6's frozen
  table, Q12 and companion Q17) and the optional `bspc` leg on the emulated M7. The design's
  probes are kept, sources only, in [`tools/parity/modes/`](../tools/parity/modes/)
  (`tools/parity/README.md`).
- **Mode compiler lane C's open ends** (sound revision 2). Choices the design left: step 0
  validates the mode and CTRL with `ValidateMode`'s rules but not STAT's, so a state built in
  memory with non-canonical, unsorted or duplicate leaves still loads (inexact), as before;
  `fadeStart` is the grain's own frame index and a FastCut shortens the grain to end with its
  fade; `LoadPreset` takes the switch style as an optional argument for wrappers that split
  their own blocks; `Engine::ModeSwitches()` exposes the content comparison. Not built:
  sequencing state (none exists before W1, so the reset on a mode change is a hook, and §10.4's
  "a stale `modeHash` resets sequencing" is tested as the comparison only), `ActiveModeInfo`,
  the `trigger_offset` device setting (row 81 stays Reserved until phase D, so the detector reads
  the preset's sensitivity alone), and the DTCM map audit the raised `kEngineImplBytes` should
  pass. The golden file ties a preset's render to the package it starts from, not to packages it
  loads mid-render: a change to one of those shows in `MANIFEST` (the label), and its render's
  change counts as the engine's, conservatively. Unit tests whose parameter lists named 27 or 28
  still do, read as structure (`dsp/tests/RetiredRows.h`). The plugin loads modes since lane D's
  curation slice, but its session state (`BSWS` v1) still holds leaves only, so a session plays
  the default mode and an older session loads without its onset and mark switches.
  Nothing lane C added has run on GitHub; the M7 and the x86 legs here agree.
- **The Mix law's open ends** (sound revision 3). It builds the owner's provisional answer to
  Q13, reversible until the first public revision (a reversal is its own revision). The law raises
  output peaks with the level, by up to 6 dB at Mix 0.5 where dry and wet peak together: on the
  hottest test vector three first-set recipes peak above 0 dBFS at their stored positions, and
  the engine saturates only near `FLT_MAX`, so the pedal's codec or a host would clip them. Lane
  E's trims and input classes and the owner's listening decide whether such a mode wants a lower
  trim or Mix. The knob's taper stays linear, and the plugin's wrapper bypass with its crossfade
  is still to build (plugin gaps, below).
- **Mode compiler lane F's open ends** (wave 1, sound revisions 4–7). Choices the design left:
  a trigger's source is decided when it falls due, so a load at its frame decides it (a due
  trigger whose source the playing mode leaves out is dropped there); Sidechain and unknown
  trigger ids count as the footswitch (schema 1 has no sidechain source); a burst fires at most
  one grain a frame and none at a frame a trigger fired at, a deferred grain keeping its spacing
  from where it fired, and 8 bursts fly at once, the oldest dropped past them; intermittency
  draws carry an ordinal per kind (periodic, onset, manual), so the kinds deciding at one frame
  draw apart; births at one frame share the frame's draws, the pitch selection's included, as
  they share spray, pan and the rest; the pitch cycle advances on every birth from any source;
  a live position's decay ages from the grain's birth (each pass begins older), a mark's from the
  mark (so a frozen mark mode keeps fading), and the design's "pin age" waits for W3's `POS_PIN`
  (freeze does not age a live position); below 2^-126 a decay gain is 0; the scheduler spaces
  births by a voice's whole life and caps its target at it; a FastCut fade can run into a voice's
  next pass. `Engine::Stats()` (births, burst births, skips, repeat passes, steals) is new API
  for tests and lane E's metrics. Not built: layer 1's leaves and E11's voice sum across two
  layers (W3), a burst gain or velocity (W2's trigger velocity). Wave 1 grew the images' ITCM by
  about 4.4 KiB: the live image's is at 94.4% (60.4 KiB, 87.5% at revision 3) and the parity
  image's at 91.2%, so the next wave moves cold engine code (`Validate` first) out of ITCM. The
  four revisions are one commit each, each minting its own golden file: since pull request #7 the
  sound-revision gate checks them commit by commit, so one pull request may carry all four (with
  the package-change label for 4, 6 and 7, above). Parity renders only the head's golden file,
  and `sound-rev-render` renders 4, 5 and 6 on every parity leg at their own commits, the ones the
  next revision is built on; until the owner requires that check, each revision commit passes
  parity and host as the pull request's head before the next is pushed: the pull request opens at
  r4's commit and pushes r5's, r6's and r7's in turn, then the rest. Here all three match their
  golden files, 38, 40 and 43 presets, in every render parity makes on MSVC SSE2 and AVX2
  (Windows), GCC 11, Clang 14 and the emulated M7 (Docker), the M7 from the archives
  [Internal sound revisions 4–7](#internal-sound-revisions-47-wave-1) records; Linux arm64 and
  macOS first render them on GitHub. The review's fixes and these docs follow the last of them,
  with no bump. Nothing lane
  F added has run on GitHub yet; here GCC 11, Clang 14 and the emulated M7 reproduce each revision
  (lane F's review). A local check that review added: `audit_symbols.py --toolchain arm` on the
  Windows M7 oracle's archive, which needs no Docker (it caught `__aeabi_ul2f` at 6).
- **Engine API still to come:** tap/tempo events (W2) and `SaveState`/`LoadState` (which will
  carry the epoch). Smaller items: automating `DelayMs`
  still splices clean delays (the grain engine's glide, below), input above 0 dBFS
  hard-clips in the int16 ring, and a trigger's velocity is carried but unread (its source
  gates it since sound revision 4).
- **Plugin skeleton gaps:** the resampled 48 kHz mode (other host rates run the engine
  natively), the wrapper bypass with crossfade, the pedal-faithful live input option
  (`ConditionInput24`; the audition render applies it), event scripts in the app's audition panel
  (the shared render plays them, as `bspc render`'s scripts do), MIDI CC
  mapping, pluginval in CI, CLAP and LV2, session state v2 (a `.bsp` in the session, so a
  recalled session plays its mode; v1 holds leaves only), the library and its banks, reporting a
  macro's fan-out to hosts (mode-compiler.md §3.6, §9.2), and the
  freeze of parameter IDs and tapers (the table exists; step 6 freezes it). The In/Out level
  controls are wrapper code outside the guard and never part of a preset.
- **The curation slice's open ends** (lane D, mode-compiler.md §9.1). Built: open, play,
  edit, save, derive, solve, findings, A/B with level matching, one-click render, pickup and
  Shift. Not yet: watching the document on disk and reloading on an outside save, the "capture
  endpoint" button (a target's `lo` or `hi` from the current leaves), the rating form that
  appends a row to `firmware/factory/AUDITION.md` (`tools/audition/ratings.py` does it from the
  command line), the declarations from the ratings log (the slice's input-class toggle stands in),
  "knobs follow" (Q8's alternative), a recent-documents list, and editing structure (the editor of
  §9.1, with its schema-generated form). The level match measures both versions on the class's
  test-signal vector, not the live input, and the one-click render writes through `fopen`, so a
  render folder whose path is not ASCII fails on Windows. A DAW session recall closes the open
  document, since session v1 cannot carry it.
- **The audition tooling's readings** (lane E, `tools/audition`, its README's "Readings"). The
  pre-screen reads the design's checks where they need a measurement: a tail the render does not
  see end is measured on a 60 s probe and only a steady fall over its last 30 s extrapolates;
  Clicks score each step against the steps around it, on SoftNotes, where a step can show (on
  Plucks the dry's attacks under the Mix law make 4 times the static's largest step
  unreachable), so an attack mode renders S1–S6 and S11 twice; Activity is judged on the
  engine's births per second (`Engine::Stats()`) at five stored positions; the stored Peak is
  judged on the class inputs, the other inputs' stored peaks reported by name (the owner's
  decision, 2026-10-08); a Repeats knob that barely lengthens S7's tail is named for listening. Shape's response is read from 2 s windows of the engaged output
  and moves by up to about 5 % and 0.5 dB with the grains' random draws alone, so a Shape near its
  threshold passes or fails by draw. The
  compiler's example `engram.json` fails Clicks (its grains are hard-edged at Contour 0, window
  sustain 1 and smoothness 0, and Smear's spray and Contour's first move splice them: step
  scores 42 and 12 times the static's) and Combinations (`S11.corner-a0r1s1t1` peaks at
  +0.77 dBFS): examples, not factory modes; the factory set meets the same checks and passes
  them (step 4). The engine reports no count of sounding voices, so Load logs births per second
  (and steals), not voices.
- **Licensing, firmware side:** libDaisy's USB device/host code and its stock SD-card glue
  carry ST's SLA0044 licence, which forbids open-source redistribution, and libDaisy's
  `System` object links the USB interrupt handlers into every firmware. GPLv3 firmware needs
  a pinned libDaisy patch, TinyUSB (MIT) for USB, and its own SD disk-I/O layer — see
  companion-app.md. Not yet reviewed by a lawyer.
- **M7 budget pass (design §8/§10 gates): measured, and the worst case does not fit.** The DWT
  pass on the Rev7 (2026-10-07, sound revision 1;
  [the silicon record](design/reviews/rev7-silicon-record.md) §3) replaces §8's derived
  totals: the nominal row costs 7,774 cycles/sample on average (77.7 %) and 9,915 in its worst
  warm block (99.1 %; 100.3 % cold), the pessimistic rows 8,922–11,958 on average and up to
  16,847 (168.5 %), against §8's ≈2,950–3,700 and ≈6,800 (3,200–4,050 and 7,700–7,800 with the
  determinism profile's costs). By difference, 64 voices against 1 cost 6,549 cycles/sample in
  the pessimistic configuration (§8: ≈4,000 for the grain render), and a birth at most 6,203
  cycles (§8: ≈530 cycles/sample at 1 ms grains). Of the profile's costs, subnormal operations
  cost nothing extra and the flush 0.9–1.8 %; contraction off is inside every figure, not
  isolated. A fix is under design, with owner decisions pending. From the design, still: the
  scheduler's 64-slot sweep wants a free bitmask; segment batching at extreme birth rates; the
  detector's per-hop FFT is a single-sample cost spike (~2–2.4× the budgeted pessimistic row in
  its worst block) that likely wants stage-splitting.
- **Trigger layer**: no sidechain input; detector constants are calibrated for 44.1/48 kHz
  (the plugin design runs the engine at 48 kHz and resamples at other host rates; until that
  mode lands, the skeleton runs it at the host rate).
- **Engine features from the design not yet built**: W2's CLOCK, steps, mark walk and synced
  times; W3's glide, per-grain SVF/crush modifiers, dual layers, `POS_PIN`, `POS_GRID` and scale
  quantization of the pitch set (wave 1 is built, sound revisions 4–7).
- **Post chain**: reverb damping/bandwidth as parameters, tempo-synced delay time,
  runtime stage reordering.
- **The central product risk is unchanged**: mode *feel*. No amount of architecture
  replaces the curation effort on the 44 factory modes. The first 14 (and 4 reserves) exist and
  pass the objective pre-screen; none has been heard by the owner yet.

## Next steps (recommended order)

These are the six steps of the merged sequence in
[companion-app.md §8 (Delivery plan)](design/companion-app.md), which also places the
determinism profile's own steps; the numbers match the designs' "merged step" references.
Steps 1–4 need no hardware.

1. **Determinism profile and the `dsp/` API.** *Done:* the build profile, header hygiene,
   in-tree math with the symbol audit, the full control-word guard and denormal flush, the
   NaN-free boundary, the block-split fix, mono aliasing and the dither key, the post-delay
   time glide, the parity harness and its M7 leg, the JUCE skeleton, and `Restart`, the
   random-number epoch, `LoadPreset` and frame-stamped events (profile steps 1–9).
2. **Mint internal sound revision 1.** *Minted* (profile step 10): golden hashes, the
   sound-revision gate and the emulated Cortex-M7 parity job check every pull request, and
   branch protection makes them required on `main`; every leg, AArch64 included, matched the
   golden file on its first GitHub run. Remaining: the rest of step 10 (Known gaps) and the
   nightly legs (profile step 11).
3. **Mode compiler**, designed in [mode-compiler.md](design/mode-compiler.md) (draft v2), whose
   §12.4 plan runs lane 0, lane B, lane A, lane G's package rule, then lane C at sound revision
   2. *Done:* lane 0, the permanent parameter-ID table with the macro IDs; lane B, the
   decoded preset with its decoder, validator, encoder, frozen fixtures and fuzzers (plus the
   compiler's number code); lane A, the compiler and `bspc` (schema 1, canonical JSON,
   lint, derive), which compiles only the default structure until sound revision 2; and lane G,
   the CI: the sound-revision gate's package rule, `bspc-roundtrip` on seven host legs, the
   decoder's fuzzers on every leg and the emulated M7, libFuzzer, nightly legs and the compiler
   audit (on their first GitHub run, the arm64 and macOS legs must reproduce the number code's
   and the compiler's committed hashes); and lane C, the engine runtime at sound revision 2
   (modes loaded and validated, macro and expression moves, Trails and FastCut, the wet-only
   trim, the effect volume and the wet kill, the corpus on compiled packages). The Mix law (Q13,
   the owner's provisional answer) is sound revision 3, with the first set's recipes re-measured
   under it, and wave 1 (lane F, the owner's Q2 putting it before CLOCK) sound revisions 4–7, one
   per feature. Lane E's audition render and scripts (`tools/audition`, `bspc render`) and lane
   D's curation slice (the app's Pedal view, documents, derive and solve, A/B, one-click render)
   are built. Next: W2.
4. **First factory modes through the app's offline audition** — burning down the feel risk.
   *Done:* the first set authored and pre-screened ([Step 4](#step-4-the-first-factory-set)): 14
   modes and 4 reserves in `firmware/factory/`, every one passing the pre-screen at revision 7,
   with the owner's audition set rendered; the owner has heard it (2026-10-08) and keeps all 14.
   *Next:* the owner's knob ratings in the curation view (the factory set's **feel**, which only
   the owner's ears can judge) and the exit criteria; then the keepers join the golden corpus. The
   output safety limiter the owner asked for comes as a sound revision of its own. App integration continues in parallel: the resampled 48 kHz plugin mode for other host
   rates, session state v2 and the library, the rest of lane D, and the plugin gaps above.
5. **Hardware bring-up and the hardware-gated decisions, then the device link.** On the
   Daisy Seed Rev7 prototype: the silicon parity check and the DWT measurement pass are done
   (2026-10-07, [the silicon record](design/reviews/rev7-silicon-record.md)), and they settled the
   subnormal policy (gradual underflow stays), the flush's form and cost and `Restart`'s time.
   Next, the CPU budget's fix (under design, owner decisions pending; the worst case is not
   met) and the decisions the pass leaves (explicit FMA, polynomial kernels or tables, the
   pedal's default load mode); the live image's checks; the pedal side of the device link
   (TinyUSB, GPL-clean SD disk I/O, the preset slot store, the upload protocol with its PARITY
   check, the firmware update path, and the engine's SPSC event queue for the firmware's
   producers); and the app side
   (upload, download, verified upload, sound-revision skew handling). A desktop pedal
   simulator lets the protocol work start before the hardware is finished. Each adopted
   hardware-gated decision bumps the internal sound revision.
6. **First public sound revision** — parameter IDs, names, tapers and sound fixed together;
   only then do the public plugin, app and firmware ship.

In parallel when ready: **clock/tempo sync and looper** feature work (rhythmic quantization
remains the Microcosm's most-praised musical trait), and the **hardware schematic** (a 40-pin Seed carrier that takes a Seed3 or Rev7, starting from Daisy's open-hardware Seed3 Pedal Dev Kit and the
Electrosmith reference stereo I/O front end, per
[pedal-control-surface-and-io-hardware.md](research/pedal-control-surface-and-io-hardware.md)).
