# Brainscape plugin and companion app

One JUCE `AudioProcessor` (`BrainscapeProcessor`) hosting the same `dsp/` engine as the pedal,
built as a **Standalone** app and a **VST3** plugin (plus **AU** on macOS). This is the skeleton
of phase B in [companion-app.md](../docs/design/companion-app.md) §8.1: the wrapper (§4), the
plain-value parameter layer (§5.1–§5.6), and a curation bench: the mode compiler's curation
slice ([mode-compiler.md](../docs/design/mode-compiler.md) §9.1), which opens a preset document,
plays it with its mode, edits it with the pedal's knobs and the raw leaves and saves it back as
canonical JSON. The library, session state with packages and the device link come later.

JUCE 9.0.3 is used under the AGPLv3 (companion §3.5): desktop binaries are GPLv3 combined with
AGPLv3, and only targets under `plugin/` link JUCE.

## Build

Requirements: CMake 3.22 or newer, a C and C++17 compiler (Visual Studio 2022, Xcode, or
GCC/Clang), and network access on the first configure, which downloads the pinned JUCE archive
(`9.0.3.tar.gz`, SHA-256 `a81e5508…40c1aba`, commit `be29c814`) into the build directory.
Offline, point CMake at an unpacked copy with `-DFETCHCONTENT_SOURCE_DIR_JUCE=/path/to/JUCE`.

```sh
cmake -B build/plugin -DBRAINSCAPE_BUILD_PLUGIN=ON -DBRAINSCAPE_BUILD_TESTS=ON
cmake --build build/plugin --config Release --parallel
ctest --test-dir build/plugin -C Release --output-on-failure
```

Run these from the repository root. On Windows add `-G "Visual Studio 17 2022" -A x64` if CMake
picks another generator (see [Windows notes](#windows-notes)). Linux needs JUCE's packages (see
`.github/workflows/plugin.yml` for the list), and the GUI tests need a display
(`xvfb-run ctest …`). `-DBRAINSCAPE_WERROR=ON`, as CI sets it, makes warnings errors in `dsp/` and
in the plugin's own sources.

Every target that compiles or links the engine, the format wrappers included, builds under the
engine's floating-point profile (`cmake/BrainscapeFpProfile.cmake`: contraction off, no
fast-math, no LTO), which `brainscape_dsp` passes on as a PUBLIC usage requirement. Configure
fails if such a target picks up a forbidden setting from any source, JUCE's
`juce_recommended_lto_flags` for example, so the plugin never links that target. `dsp/` itself
builds without exceptions or RTTI; those options stay private to it.

Outputs, under `build/plugin/plugin/Brainscape_artefacts/<Config>/`:

| Format | Path |
|---|---|
| Standalone | `Standalone/Brainscape.exe` (Windows), `Standalone/Brainscape.app` (macOS), `Standalone/Brainscape` (Linux) |
| VST3 | `VST3/Brainscape.vst3` (a bundle folder) |
| AU (macOS) | `AU/Brainscape.component` |

The build never copies plugins into system folders.

## Run the standalone

1. Start `Brainscape`. It asks the audio device for 48 kHz, the pedal's only rate; the status
   line at the bottom says **48 kHz pedal rate · host blocks**, or **Not pedal rate: host at
   44.1 kHz** if the device refused. Live playing is outside the parity contract; the audition
   render (step 6) is what reproduces the pedal.
2. Input is muted until you open **Options → Audio/MIDI Settings…** and untick **Mute audio
   input** (JUCE's guard against feedback through speakers). Pick the input channels there; on
   Windows, ASIO devices are listed too.
3. **Test input** (bottom right) replaces the live input when you have no guitar to hand:
   **Pluck** plays a plucked-string phrase, **File loop** loops a WAV, AIFF, FLAC or Ogg file.
   **Input: Mono** copies the left input to the right, for a guitar on input 1 (the Standalone's
   default); **Stereo** keeps both (the plugins' default, so a stereo track passes unchanged at
   Mix 0). **In level** and **Out level** are global settings, never part of a preset.
4. **Leaves** (the tab under the header) shows every engine parameter as a knob, the design's
   Advanced view; **Pedal** holds the pedal's knobs and the open preset document (next section).
   Drag a knob to turn it, double-click it for the default,
   double-click the value to type one in its units (`250`, `1.2 s`, `2.5k`, `40%`, `-3 dB`,
   `Off`, `Kill`, `LP`…). Typed values are stored exactly as typed, read by the preset
   compiler's exact number reader, so a typed value has the bits a preset document with the same
   text compiles to. **Window → Skew** is centred: −100 % is percussive, 0 % symmetric, +100 %
   a reverse swell. **Reverb → Time** is a 0–100 scale. **Grain delay → Trim** scales the wet
   signal only (the mode's level match), and **Filter → Cutoff** at its minimum, shown as
   `Kill`, mutes the wet signal, as at its maximum (`Off`) it bypasses the filter.
   Changing **Post delay → Time** glides the delay to the new time, bending the repeats' pitch
   like tape (at most 0.5–1.5× speed) instead of clicking; **Grain delay → Time** still jumps.
5. **FREEZE** pins the grain position (host-automatable); **TRIGGER** fires one grain, as does
   any MIDI note-on (enable a MIDI input in the settings). The **ONSET** light flashes for every
   onset the detector hears; **Trigger → Sense** sets its threshold. Onsets firing grains and
   grains on onset marks are a mode's structure since sound revision 2: open a preset document
   whose mode has them (next section); without one the plugin plays the default mode. An orange
   **control events lost** in the status line counts triggers, macro moves and frame-stamped
   events that a full event queue dropped; ordinary knob edits are never lost, because the
   wrapper re-sends every parameter after an overflow.
6. **Render audition…** (the Test input panel's last row) renders the test input through the
   current preset into a WAV file (companion §4.9): the loaded file when **File loop** is
   selected, otherwise 10 s of `dsp/`'s plucks test signal; then 10 s of silence for the trails.
   The input goes through `ConditionInput24` (the codec's 24-bit grid) and the input mode, and
   a separate engine renders it from the exact-restart state (`Init`, then
   `LoadPreset(…, Exact)`) at 48 kHz in the pedal's 48-frame blocks; the In and Out levels are
   not applied. Beside the 32-bit float WAV it writes `<name>.recipe.json`: the preset's exact
   bits, the SHA-256 of the input and of the output (interleaved little-endian float32, as the
   golden harness hashes) with one more per 1 s segment of the output (PARITY's segmented hash,
   so a comparison finds the first differing second), the sound revision and the toolchain. A
   render of the test signal or of a 48 kHz file is identical on every conforming build; a file
   at another rate is converted by linear interpolation, and the recipe says the render is
   reproducible on this machine only. The test signal is `bspc render`'s S0 input, so a stereo
   render of it has the hash of the same preset's `S0.engaged.plucks`.

## Curate a preset (the Pedal view)

The curation slice (mode-compiler.md §9.1, §11.3) is how factory modes are tuned and rated.

0. **The Modes menu** (the header, beside the wordmark) holds the factory set, built into the
   plugin from `firmware/factory/` (every package `MANIFEST` lists, byte for byte): the set by
   family (echoic, reverie, recall, misfire), **Reserves** in a submenu, and **Open a document or
   package...**. Choosing a mode plays it exactly as opening its `.bsp` does (step 1): a
   Spillover load with Trails while audio runs, Exact before anything has played, the knobs
   waiting for pickup, and its document open below. The header names what plays and its
   family. A factory mode has no file: **Save** is **Save as...**, which writes a copy (that copy
   is then what plays and saves), and **Revert** re-opens the built-in package.
1. **Open...** (Ctrl+O) a schema-1 preset document (`.json`) or a package (`.bsp`). It compiles
   through the compiler library and plays at once: a Spillover load with Trails while audio
   runs, Exact before anything has played. A document that does not read or compile is refused
   with the compiler's findings, and the open one stays with its own (the refused file's
   findings show in **Compile · lint** while no document is open).
2. **The eight knobs** are the pedal's: Activity, Repeats, Shape, Time, Space and Filter move
   the mode's macros (titled with its display names, the knob's own name and target count
   below), Mix the wet/dry leaf, and Loop Level waits for the looper. Under each knob: the
   position and the first target's value. After every load a knob is **locked** until you turn
   it to the stored position (the pedal's pickup, owner question Q8): its marker dot shows that
   position, its pointer your hand's, and the line under it says where to pick up; double-click
   catches it there. **SHIFT** turns Repeats, Shape, Space, Filter and Mix into the mod depth,
   mod rate, reverb time, resonance and effect volume, each picked up anew.
3. **Leaves** shows which macros move each leaf (coloured dots by its caption, hollow when the
   leaf is detached). Edit any leaf there; one a macro targets turns amber when it is off the
   macro's value, since **Save** derives it from the knob's position. Right-click a targeted leaf
   to **detach** it (Save keeps your value, written to `editor.detached`) or to **solve** that
   macro's position from it: the position whose value lands nearest the leaf, the leaf kept and
   the macro's other targets moved to that position. **Solve positions** solves every macro from
   its first target that is not detached.
4. **Compile · lint** lists, live, what Save would hit: the compiler's errors, the lint findings
   (factory rules for a `factory.` id) and the leaves derive will change.
5. **Save** (Ctrl+S) writes canonical JSON stamped with this build's sound revision and hash,
   and the `.bsp` beside it when one is there (or the document came from one), then plays what it
   wrote; the pair is written together or not at all. **Save as...** writes elsewhere; **Revert**
   re-opens from disk. A document that does not compile is written as JSON only. What Save
   writes is what `bspc derive` and `bspc stamp` would: the state line says **Saved** only when
   that is the file as it is (byte for byte), and otherwise how Save would change it (unsaved
   changes, leaves it would derive, a stale stamp to renew).
6. **A stored / B working** (key B) compares the version on disk with your edits, both loaded
   with Trails; the knobs lock while A plays. **Match level** renders both offline on the input
   class's test signal (**Attack**: Plucks, **Pad**: SoftNotes), measures K-weighted loudness
   and trims the louder one on the monitor output only (never in a preset or a render).
7. **Render** renders the document as Save would write it through `tools/audition`'s scripts,
   S0 or (**S0-S11**) all twelve, with the objective pre-screen, into
   `Music/Brainscape audition/<id>/` (WAVs, recipes, `audition.json`); **Show** opens the folder,
   and the status line's tooltip holds the pre-screen summary. `bspc render` makes the same bits.
   Opening another document, closing it or closing the plugin stops a render at its next block.

The document stays open while the editor is closed. A DAW's session (BSWS v1) holds the leaves
and, while a factory mode plays, that mode by its id and package hash with the macro positions
(the `FMOD` block, `src/StateCodec.h`): recalling it plays the build's package of that id with
the session's leaves and positions (the status line says **Last state load was inexact** when
the build's package is not the one saved), and the views open its document again. Any other
session plays the default mode, as do sessions read by builds before the block, and recalling
one closes the document.

## Load the VST3

Copy the `Brainscape.vst3` folder to your VST3 folder (`C:\Program Files\Common Files\VST3`,
`~/Library/Audio/Plug-Ins/VST3`, or `~/.vst3`), or add the build's `VST3` folder to your DAW's
plugin search path, then rescan. Use it as an insert effect on a mono or stereo track; it
accepts MIDI notes as triggers. Run the project at 48 kHz, the pedal's rate.

**Restart on play** (the Test input panel's last row, saved with the session, off by default)
restarts the engine at every transport start: the first playing block after a stopped block,
`prepareToPlay` or the host switching to offline rendering (only the switch counts: VST3, VST2
and LV2 pass the mode again with every block). The engine then starts from the exact-restart
state with an Exact load of the values in effect at the start's first frame: the current preset
with that block's automation and edits, so an automated parameter starts on its lane's value.
Two bounces of one passage are identical, and at 48 kHz without automation identical to the
pedal (companion §4.9). Offline bounces, and a start before anything has played, restart the
engine in place. In real time the plugin never blocks: while the option is on, a worker thread
keeps a spare engine (another 17 MiB) restarted with the current preset, and a transport start
swaps it in when it holds exactly those values; if it does not (the preset changed a moment
ago, or automation moves a parameter at the start without the host having sent that value while
stopped), the engine runs on and the status line says **Last start not restarted**. With the
option off, trails continue across stops. The host's `reset()` is `Engine::Reset`: it keeps the
ring.

## Windows notes

- Install Visual Studio 2022 with the **Desktop development with C++** workload, and run the
  build commands from the repository root.
- Keep the build directory's path short: builds fail once its real path is longer than
  about 150 characters, usually in JUCE's nested `juceaide` configure. The symptoms vary and
  rarely blame the path: `C1083: Cannot open compiler generated file`, `C1041: cannot open
  program database`, or MSBuild `FTK1011`. An 8.3 short alias does not help, because CMake
  and `cl` expand it back to the long path; use a short real directory such as
  `C:\src\brainscape`.
- No input in the Standalone? With **Windows Audio** (WASAPI), turn on Settings → Privacy &
  security → Microphone → **Let desktop apps access your microphone**; ASIO is not affected. The
  Standalone does not yet detect a silent open input and say so (companion §3.2).
- For the pedal's 48 kHz, use an ASIO driver or **Windows Audio (Exclusive Mode)**. In shared
  mode WASAPI runs at the device's default format: set it to 48000 Hz in the device's Sound
  properties → Advanced.

## Tests

`ctest` runs, besides the `dsp/` tests (`dsp_unit`, `dsp_fpenv_forced_flush`, `dsp_symbol_audit` and
its control, `dsp_fp_profile_check` and the golden harness's `golden_check`, which requires every
golden hash of sound revision 1, `golden_check_edits` and `golden_forced_flush`):

- `plugin_wrapper` (`tests/plugin_tests.cpp`): the processor driven as hosts drive it, compared
  bit for bit with the engine driven directly from `LoadPreset(…, Exact)` in 48-frame blocks,
  its events stamped at their frames: host blocks of 0, 1, 7, 512, 513 and 4096 frames with one
  `Process` call per chunk of at most 512 frames, in-place mono and stereo layouts, a disabled
  input, non-finite input, 44.1/48/96 kHz, exact plain values through the parameter class, the
  slider attachment and session state, state restores (a Spillover load while running, an Exact
  load before anything has played) and their order against earlier and later edits (running,
  suspended and before `prepareToPlay`), MIDI triggers and host automation at every host block
  pattern, events stamped at absolute frames, freeze, lost-event accounting, Restart on play
  (offline, in real time with the spare engine, with a spare that holds another preset, off,
  with the mode passed before every block, with an automation lane, with MIDI, and stamps made
  before the restart), `reset()`, the audition render (the exact-restart state, both input
  modes, a file off the 24-bit grid, and the job's WAV bits, hashes per render and per second,
  and recipe), the test input's file loop (exact playback, wrap, mono and 44.1 kHz files, bad
  files, loads during playback), the default input mode per format, and zero heap allocations
  inside `processBlock`.
  The mode system's part: the host model's registration (the leaves, freeze, eight macros,
  expression, effect volume), document loads (Exact and Spillover, an invalid mode refused),
  host macro moves at three block patterns with the leaf mirrors following the fan-out, an
  undefined macro, expression fan-out, the effect volume across loads, Init and sessions, and the
  transport-start restart with a mode; the curation session (an unchanged document saving back
  byte for byte, every example and corpus document saved unedited as derive and stamp write it,
  a macro move, a derived and a detached leaf, solve position for every macro and from a chosen
  leaf, A/B, a session recall, refused documents that leave the open one's findings, a package
  saved as a pair or not at all, level matching, the one-click render, a render stopped by
  closing the plugin or opening another document), the knobs' names from the open document,
  the pedal knob's pickup, and the test-signal audition's hash against `bspc render`'s S0.
  The factory set: every embedded package against `MANIFEST` (byte for byte the committed
  `.bsp`, its hashes computed and decoded, META's id, name and family, validation), the Modes
  menu's structure, every mode loaded through the menu against its `.bsp` opened from a file on
  a playing processor (the same output bit for bit, a Spillover load with Trails, the same
  mirrors and document), Save as and Revert of a built-in mode, and sessions with the `FMOD`
  block (an Exact restore saved back byte for byte, a recall over an open document, readers
  without the block, unknown and truncated blocks, a mode the build lacks or packages
  differently).
- `plugin_editor_snapshot`: renders the editor offscreen to PNG files in
  `build/plugin/plugin/screenshots/`: both views with no document and with a scratch copy of
  `compiler/tests/data/engram.json` open (a knob waiting for pickup, a hand-edited leaf, Shift, A
  playing, a finished render) at the default, minimum, large and 2× sizes, a 44.1 kHz frozen
  frame with Restart on play on, a factory mode and a reserve chosen from the Modes menu (with
  the menu itself drawn as the look and feel draws it, `editor-modes-menu.png`), and the
  Standalone's editor after an audition render, which it also writes there.
- `plugin_vst3_hosted`: loads the built VST3 through JUCE's headless VST3 host, restores a
  session state through `IComponent::setState`, reads it back bit for bit, and checks that
  in-place audio in odd host blocks equals the engine reference, and that an offline export
  with Restart on play, started while the transport plays, restarts once and equals it too.

## Current limitations

- **Pedal-exact only for renders.** The engine is bit-identical across builds and block sizes
  (Release and Debug, MSVC, GCC, Clang and the emulated Cortex-M7 reproduce the golden hashes of
  internal sound revision 1, which CI enforces), and the wrapper hands it frame-stamped events
  and loads presets through `LoadPreset`.
  A render matches the pedal when it starts from the exact-restart state at 48 kHz: the
  audition render, or a bounce with Restart on play and no automation. Live playing is outside
  the parity contract, and so is a preset recalled while playing, which loads Spillover so the
  trails go on.
- **Other host rates run the engine natively,** so delay periods and pitch differ slightly; the
  resampled 48 kHz mode (companion §4.2) is still to come.
- **Nothing here is a release.** Parameter IDs, names and tapers are provisional until the
  companion §5.7 gate; do not rely on them in DAW projects. The tapers are power curves
  (`dsp/src/ParamDisplay.cpp`). The parameters are the Leaf rows of the permanent ID table
  (`dsp/include/brainscape/Params.h`, docs/design/mode-compiler.md §4), keyed on their names:
  `wet_trim_db` and `layer0.pitch.transpose_st` replaced `out_trim_db` and `layer0.pitch.st`,
  and `scheduler.onset_trigger` and `layer0.position.source` were retired into mode structure at
  sound revision 2, so automation lanes saved on the old names are lost, and a session saved
  before then loads without its onset and mark switches (§4.4). The recommended host model
  (§3.6, owner question Q12, provisionally) applies: the eight macros (`macro.activity` …
  `macro.aux2`; a host move is a `MacroMove`), Mix, the effect volume
  (`global.effect_volume_db`), **Freeze** and the expression pedal (`perf.expression`) are
  automatable, every other leaf is registered but not. A macro's fan-out updates the leaves the
  editor shows, but hosts are not told of it yet.
- **Session state is a provisional binary v1** (exact plain values plus the input mode, levels,
  the restart option and the effect volume, and a factory mode by reference): it holds no
  package, so a session saved with a document file (a copy of a factory mode included) plays the
  default mode. The preset library and the device link are not built, and the Leaves view's
  audition renders no event script (the Pedal view's **Render** runs the scripts).
- **Not yet built from §4:** the wrapper-owned bypass with crossfade (JUCE's default bypass
  stops the engine, so trails do not continue), the resampled 48 kHz mode, MIDI CC mapping, LV2
  and CLAP.
- **Event timing:** host automation, editor edits, freeze and triggers apply at the start of the
  next host block (VST3 hands automation over per block), MIDI at its sample offset.
  `BrainscapeProcessor::PostAt` applies an event at an absolute frame of the engine's timeline,
  which starts at 0 at every Init or restart; a stamp made before a restart is dropped, even
  one whose frame has passed. Every event reaches the engine through `ProcessContext::events`,
  which splits the block at it.
- **Host automation is lossy** by nature (companion §5.6): hosts store the normalised value.
  The editor and typed values write exact plain bits.
- **The taper and display functions own the floating-point environment.** `PlainFromNormalized`,
  `NormalizedFromPlain` and `FormatPlain` are `dsp/` entry points with the engine's control-word
  guard (companion §4.6), and canonicalization is `brainscape::Canonicalize`, so a host thread's
  FTZ, DAZ or rounding mode changes no plain bit. The In and Out levels are wrapper code: they
  convert dB with libm `pow` and multiply in the host thread's environment, which is why they are
  global settings and never part of a preset; at 0 dB they are bypassed exactly.
- Live input goes through `dsp/`'s `SanitizeInput` (`dsp/include/brainscape/InputCondition.h`,
  companion §4.8): NaN and ±inf become +0, every finite sample passes unchanged. The
  pedal-faithful 24-bit option (`ConditionInput24`) is not wired up. File decoding for the test
  loop is platform code and not part of any parity claim.
