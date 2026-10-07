# Brainscape

An open-source granular delay pedal built on the [Electrosmith Daisy Seed](https://electro-smith.com/products/daisy-seed) — a spiritual successor to the Hologram Microcosm.

## Vision

The Microcosm showed how inspiring a granular/glitch multi-effect pedal can be, but it's closed hardware with a fixed feature set. Brainscape aims to capture what makes it beloved — playable granular textures, instant gratification, deep-but-approachable controls — in an open platform anyone can build, hack, and extend.

Two deliverables from one shared DSP core:

1. **Hardware pedal** — Daisy Seed firmware plus an open hardware design (stereo I/O, pots, footswitches).
2. **Desktop plugin and companion app** — built with JUCE (VST3, AU, standalone) so the same engine runs in a DAW. The standalone app is the pedal's companion: create presets on the desktop, upload them to the pedal, and get engine output designed to be **sample-identical** to the pedal's for the same input (see [determinism-profile.md](docs/design/determinism-profile.md) for exactly what that covers).

## Repository layout (polyglot monorepo)

| Path | Purpose |
| --- | --- |
| `dsp/` | Platform-agnostic C++ DSP core shared by firmware and plugin |
| `compiler/` | Desktop-only preset compiler: preset documents (JSON) to `.bsp` packages; its tool is [`tools/bspc`](tools/bspc/README.md) |
| `firmware/` | Daisy Seed firmware (C++, libDaisy); today the Rev7 bring-up images ([firmware/README.md](firmware/README.md)) |
| `plugin/` | Desktop plugin and companion app (JUCE: VST3, AU, standalone; CLAP optional) |
| `hardware/` | Schematics, PCB, enclosure design |
| `docs/` | Documentation and research |
| `tools/` | Build scripts and utilities |

## Status

**Core DSP engine complete (v1 scope)** — granular scheduler and 64-voice pool, post chain
with above-unity feedback taming, and the spectral-flux trigger layer, deterministic and
block-size-invariant. **The engine side of the determinism profile has landed**, with the
engine's restart, preset-load and stamped-event API, and **internal sound revision 1 is
minted:** MSVC, GCC, Clang and the Cortex-M7 build run under emulation reproduce its golden
hashes bit for bit, and CI fails any change that alters them without a revision bump. **In
progress:** the JUCE plugin and companion app, whose skeleton builds
([plugin/README.md](plugin/README.md)). See
**[docs/STATUS.md](docs/STATUS.md)** for the full picture and next steps. Research lives in
[docs/research/](docs/research/), the designs in [docs/design/](docs/design/). Build and test
the engine on Linux/macOS/Windows (no JUCE download):

```bash
cmake -B build -DBRAINSCAPE_BUILD_TESTS=ON
cmake --build build --config Release
ctest --test-dir build -C Release
```

Add `-DBRAINSCAPE_BUILD_PLUGIN=ON` to build the plugin and standalone app as well; it fetches
the pinned JUCE 9.0.3 archive on the first configure.

## License

Code and firmware are licensed under the [GNU GPLv3](LICENSE). Hardware design files will likely ship under CERN-OHL-S once they land.

The desktop plugin and app use [JUCE](https://juce.com) under **AGPLv3**: JUCE's commercial licence forbids combining it with copyleft code, and GPLv3 §13 permits linking with AGPLv3. Released desktop binaries are therefore a GPLv3 + AGPLv3 combined work, and every release ships, or links to, its complete Corresponding Source, including the pinned JUCE version. This is a reading of the licence texts, not legal advice; see [docs/design/companion-app.md](docs/design/companion-app.md).

For contributors: GPL/LGPL reference code is compatible and may be vendored with attribution. AGPLv3 code may be linked only into desktop targets under `plugin/` (today: JUCE). `dsp/`, `protocol/`, `link/` and `firmware/` must never include AGPL code, which keeps the engine framework-free and the firmware GPLv3-only. Desktop release artifacts state that they are GPLv3 combined with AGPLv3. Other AGPL sources (e.g. Essentia) remain study-only — see [docs/research/](docs/research/) for per-source licensing notes.
