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

**Early research phase.** See [docs/research/](docs/research/) for platform, DSP, and market research.

## License

TBD. GPLv3 is the leading candidate (compatible with the VST3 SDK's dual license and common in open-source audio); hardware design files may use CERN-OHL. To be decided before first code lands.
