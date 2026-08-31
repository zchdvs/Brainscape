# Brainscape

An open-source granular delay pedal built on the [Electrosmith Daisy Seed](https://electro-smith.com/products/daisy-seed) — a spiritual successor to the Hologram Microcosm.

## Vision

The Microcosm showed how inspiring a granular/glitch multi-effect pedal can be, but it's closed hardware with a fixed feature set. Brainscape aims to capture what makes it beloved — playable granular textures, instant gratification, deep-but-approachable controls — in an open platform anyone can build, hack, and extend.

Two deliverables from one shared DSP core:

1. **Hardware pedal** — Daisy Seed firmware plus an open hardware design (stereo I/O, pots, footswitches).
2. **Desktop plugin** — VST3 (and friends) so the same engine runs in a DAW.

## Repository layout (polyglot monorepo)

| Path | Purpose |
| --- | --- |
| `dsp/` | Platform-agnostic C++ DSP core shared by firmware and plugin |
| `firmware/` | Daisy Seed firmware (C++, libDaisy/DaisySP) |
| `plugin/` | Desktop plugin (VST3, CLAP, AU) |
| `hardware/` | Schematics, PCB, enclosure design |
| `docs/` | Documentation and research |
| `tools/` | Build scripts and utilities |

## Status

**Early development.** Research lives in [docs/research/](docs/research/), the engine design
in [docs/design/grain-engine.md](docs/design/grain-engine.md). The `dsp/` walking skeleton —
history ring + delay tap inside the full lifecycle/memory/parameter contracts — builds and
passes its contract tests on Linux/macOS/Windows:

```bash
cmake -B build -DBRAINSCAPE_BUILD_TESTS=ON
cmake --build build --config Release
ctest --test-dir build -C Release
```

## License

Code and firmware are licensed under the [GNU GPLv3](LICENSE). Hardware design files will likely ship under CERN-OHL-S once they land. For contributors: GPL/LGPL reference code is compatible and may be vendored with attribution; AGPL sources (e.g. Essentia) remain study-only — see [docs/research/](docs/research/) for per-source licensing notes.
