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

On Windows add `-G "Visual Studio 17 2022" -A x64` if CMake picks another generator. Linux needs
JUCE's packages (see `.github/workflows/plugin.yml` for the list), and the GUI tests need a
display (`xvfb-run ctest …`).

Outputs, under `build/plugin/plugin/Brainscape_artefacts/<Config>/`:

| Format | Path |
|---|---|
| Standalone | `Standalone/Brainscape.exe` (Windows), `Standalone/Brainscape.app` (macOS), `Standalone/Brainscape` (Linux) |
| VST3 | `VST3/Brainscape.vst3` (a bundle folder) |
| AU (macOS) | `AU/Brainscape.component` |

The build never copies plugins into system folders.

## Run the standalone

1. Start `Brainscape`. It asks the audio device for 48 kHz, the pedal's only rate; the status
   line at the bottom says **Pedal-exact @ 48 kHz** or **Not pedal-exact: host at 44.1 kHz** if
   the device refused.
2. Input is muted until you open **Options → Audio/MIDI Settings…** and untick **Mute audio
   input** (JUCE's guard against feedback through speakers). Pick the input channels there; on
   Windows, ASIO devices are listed too.
3. **Test input** (bottom right) replaces the live input when you have no guitar to hand:
   **Pluck** plays a plucked-string phrase, **File loop** loops a WAV, AIFF, FLAC or Ogg file.
   **Input: Mono** copies the left input to the right, for a guitar on input 1; **Stereo** keeps
   both. **In level** and **Out level** are global settings, never part of a preset.
4. Every engine parameter is a knob. Drag to turn it, double-click the knob for the default,
   double-click the value to type one in its units (`250`, `1.2 s`, `2.5k`, `40%`, `-3 dB`,
   `Off`, `Mark`, `LP`…). Typed values are stored exactly as typed.
5. **FREEZE** pins the grain position (host-automatable); **TRIGGER** fires one grain, as does
   any MIDI note-on (enable a MIDI input in the settings). The **ONSET** light flashes for every
   onset the detector hears; **Trigger → Sense** sets its threshold, **Trigger → Onset** makes
   onsets fire grains.

## Load the VST3

Copy the `Brainscape.vst3` folder to your VST3 folder (`C:\Program Files\Common Files\VST3`,
`~/Library/Audio/Plug-Ins/VST3`, or `~/.vst3`), or add the build's `VST3` folder to your DAW's
plugin search path, then rescan. Use it as an insert effect on a mono or stereo track; it
accepts MIDI notes as triggers. Run the project at 48 kHz for pedal-exact output.

## Tests

`ctest` runs, besides the `dsp/` unit tests:

- `plugin_wrapper` (`tests/plugin_tests.cpp`): the processor driven as hosts drive it, compared
  bit for bit with the engine driven directly in 48-frame blocks: host blocks of 0, 1, 7, 512,
  513 and 4096 frames, in-place mono and stereo layouts, a disabled input, non-finite input,
  44.1/48/96 kHz, exact plain values through the parameter class, the slider attachment and
  session state, state restores while running, MIDI triggers at their sample offsets, freeze,
  and zero heap allocations inside `processBlock`.
- `plugin_editor_snapshot`: renders the editor offscreen to PNG files in
  `build/plugin/plugin/screenshots/` (default, minimum, large and 2× sizes, and a 44.1 kHz
  frozen frame).
- `plugin_vst3_hosted`: loads the built VST3 through JUCE's headless VST3 host, restores a
  session state through `IComponent::setState`, reads it back bit for bit, and checks that
  in-place audio in odd host blocks equals the engine reference.

## Current limitations

- **Pedal-exact only at 48 kHz.** At other host rates the engine runs natively at the host
  rate, so delay periods and pitch differ slightly; the resampled 48 kHz mode (companion §4.2)
  is still to come.
- **Live playing and raw host buffers are outside the parity contract** until the block-split
  fix lands (determinism profile §5.7; companion §2.3). Bit-exact identity with the pedal also
  needs the rest of profile steps 1–9 (in-tree math, the full FP guard, the FP profile flags).
- **Nothing here is a release.** Parameter IDs, names and tapers are provisional until the
  companion §5.7 gate; do not rely on them in DAW projects. The tapers are power curves
  (`dsp/src/ParamDisplay.cpp`) pending that decision.
- **Session state is a provisional binary v1** (exact plain values plus the input mode and
  levels); `.bsp` packages, the preset library, offline audition and the device link are not
  built.
- **Not yet built from §4:** the "Restart on transport start" and pedal-grid options, the
  spare engine, the wrapper-owned bypass with crossfade (JUCE's default bypass stops the engine,
  so trails do not continue), MIDI CC mapping, LV2 and CLAP.
- **Host automation is lossy** by nature (companion §5.6): hosts store the normalised value.
  The editor and typed values write exact plain bits.
- The live input sanitizer is a local stand-in for the profile's `SanitizeInput` until
  `dsp/include/brainscape/InputCondition.h` exists. File decoding for the test loop is platform
  code and not part of any parity claim.
