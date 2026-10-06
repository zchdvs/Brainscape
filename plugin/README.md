# Brainscape plugin and companion app

One JUCE `AudioProcessor` (`BrainscapeProcessor`) hosting the same `dsp/` engine as the pedal,
built as a **Standalone** app and a **VST3** plugin (plus **AU** on macOS). This is the skeleton
of phase B in [companion-app.md](../docs/design/companion-app.md) §8.1: the wrapper (§4), the
plain-value parameter layer (§5.1–§5.6) and a test-bench editor for playing through the engine
while the engine work lands. Presets, the library and the device link come later.

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
   44.1 kHz** if the device refused. The pedal's rate is not yet "pedal-exact": the engine is
   bit-identical across builds and host block sizes, but the parity contract also needs the
   exact-restart state, exact preset loads and frame-stamped events, which are not built yet
   (see [Current limitations](#current-limitations)).
2. Input is muted until you open **Options → Audio/MIDI Settings…** and untick **Mute audio
   input** (JUCE's guard against feedback through speakers). Pick the input channels there; on
   Windows, ASIO devices are listed too.
3. **Test input** (bottom right) replaces the live input when you have no guitar to hand:
   **Pluck** plays a plucked-string phrase, **File loop** loops a WAV, AIFF, FLAC or Ogg file.
   **Input: Mono** copies the left input to the right, for a guitar on input 1 (the Standalone's
   default); **Stereo** keeps both (the plugins' default, so a stereo track passes unchanged at
   Mix 0). **In level** and **Out level** are global settings, never part of a preset.
4. Every engine parameter is a knob, except the two-position ones (**Onset**, **Position**),
   which are switches. Drag a knob to turn it, double-click it for the default, double-click the
   value to type one in its units (`250`, `1.2 s`, `2.5k`, `40%`, `-3 dB`, `Off`, `Mark`,
   `LP`…). Typed values are stored exactly as typed. **Window → Skew** is centred: −100 % is
   percussive, 0 % symmetric, +100 % a reverse swell. **Reverb → Time** is a 0–100 scale.
5. **FREEZE** pins the grain position (host-automatable); **TRIGGER** fires one grain, as does
   any MIDI note-on (enable a MIDI input in the settings). The **ONSET** light flashes for every
   onset the detector hears; **Trigger → Sense** sets its threshold, **Trigger → Onset** makes
   onsets fire grains. An orange **control events lost** in the status line counts triggers and
   frame-stamped events that a full event queue dropped; ordinary knob edits are never lost,
   because the wrapper re-sends every parameter after an overflow.

## Load the VST3

Copy the `Brainscape.vst3` folder to your VST3 folder (`C:\Program Files\Common Files\VST3`,
`~/Library/Audio/Plug-Ins/VST3`, or `~/.vst3`), or add the build's `VST3` folder to your DAW's
plugin search path, then rescan. Use it as an insert effect on a mono or stereo track; it
accepts MIDI notes as triggers. Run the project at 48 kHz, the pedal's rate; output on the host's
buffers is not yet promised identical to the pedal (see [Current limitations](#current-limitations)).

## Windows notes

- Install Visual Studio 2022 with the **Desktop development with C++** workload, and run the
  build commands from the repository root.
- Keep the build directory's path short. On paths longer than about 150 characters JUCE's
  nested `juceaide` configure fails with `C1083: Cannot open compiler generated file` (MSBuild
  `FTK1011`), an error that does not mention the path.
- No input in the Standalone? With **Windows Audio** (WASAPI), turn on Settings → Privacy &
  security → Microphone → **Let desktop apps access your microphone**; ASIO is not affected. The
  Standalone does not yet detect a silent open input and say so (companion §3.2).
- For the pedal's 48 kHz, use an ASIO driver or **Windows Audio (Exclusive Mode)**. In shared
  mode WASAPI runs at the device's default format: set it to 48000 Hz in the device's Sound
  properties → Advanced.

## Tests

`ctest` runs, besides the `dsp/` tests (`dsp_unit`, `dsp_fpenv_forced_flush`, `dsp_symbol_audit` and
its control, `dsp_fp_profile_check` and the golden harness's `golden_report`):

- `plugin_wrapper` (`tests/plugin_tests.cpp`): the processor driven as hosts drive it, compared
  bit for bit with the engine driven directly in 48-frame blocks: host blocks of 0, 1, 7, 512,
  513 and 4096 frames, in-place mono and stereo layouts, a disabled input, non-finite input,
  44.1/48/96 kHz, exact plain values through the parameter class, the slider attachment and
  session state, state restores while running and their order against earlier and later edits
  (running, suspended and before `prepareToPlay`), MIDI triggers at their sample offsets,
  events stamped at absolute frames, freeze, lost-event accounting, the test input's file loop
  (exact playback, wrap, mono and 44.1 kHz files, bad files, loads during playback), the default
  input mode per format, and zero heap allocations inside `processBlock`.
- `plugin_editor_snapshot`: renders the editor offscreen to PNG files in
  `build/plugin/plugin/screenshots/` (default, minimum, large and 2× sizes, and a 44.1 kHz
  frozen frame).
- `plugin_vst3_hosted`: loads the built VST3 through JUCE's headless VST3 host, restores a
  session state through `IComponent::setState`, reads it back bit for bit, and checks that
  in-place audio in odd host blocks equals the engine reference.

## Current limitations

- **Not pedal-exact yet.** The engine side of the determinism profile has landed: the FP build
  profile, in-tree math, the full control-word guard, the denormal flush, the NaN-free boundary
  and the block-split fix, so Release and Debug, MSVC, GCC, Clang and the emulated Cortex-M7
  render the golden corpus bit-identically on any block size. The parity contract still needs
  the exact-restart state (`Restart`), exact preset loads (`LoadPreset`) and frame-stamped
  events from the host and the editor, which apply at the start of the next host block today.
  Live playing is outside the parity contract.
- **Other host rates run the engine natively,** so delay periods and pitch differ slightly; the
  resampled 48 kHz mode (companion §4.2) is still to come.
- **Nothing here is a release.** Parameter IDs, names and tapers are provisional until the
  companion §5.7 gate; do not rely on them in DAW projects. The tapers are power curves
  (`dsp/src/ParamDisplay.cpp`) pending that decision.
- **Session state is a provisional binary v1** (exact plain values plus the input mode and
  levels); `.bsp` packages, the preset library, offline audition and the device link are not
  built.
- **Not yet built from §4:** the "Restart on transport start" and pedal-grid options, the
  spare engine, the wrapper-owned bypass with crossfade (JUCE's default bypass stops the engine,
  so trails do not continue), MIDI CC mapping, LV2 and CLAP.
- **Event timing:** host automation, editor edits, freeze and triggers apply at the start of the
  next host block, MIDI at its sample offset. `BrainscapeProcessor::PostAt` applies an event at
  an absolute frame (frames since the engine's last Init) and splits the block there (companion
  §4.10), but nothing in the app posts stamped events yet; they wait for scripted renders and
  Restart, which will reset the frame count.
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
