# Brainscape documentation

| Section | Contents |
| --- | --- |
| [STATUS.md](STATUS.md) | Current project status, verified behavior, known gaps, and next steps |
| [research/](research/) | Background research: Daisy Seed platform, existing pedal I/O designs, granular DSP theory, the Hologram Microcosm, plugin strategy, market landscape |
| [design/](design/) | Design documents (below) |
| [design/reviews/](design/reviews/) | Review records and evidence trails behind each design |

## Design documents

| Document | Scope |
| --- | --- |
| [grain-engine.md](design/grain-engine.md) | The core engine: signal path, scheduler, modes-as-data model, memory/CPU budgets, behavioral contracts |
| [determinism-profile.md](design/determinism-profile.md) | Sample-identical output across pedal and desktop: the parity contract, numerics and floating-point rules, required engine changes, CI verification |
| [companion-app.md](design/companion-app.md) | The JUCE plugin and companion app: product shape, build and licensing, plugin hosting, parameter layer, preset format, upload to the pedal, delivery plan |
| [mode-compiler.md](design/mode-compiler.md) | Modes as data in practice: the preset schema, macro layer, permanent parameter IDs, compiled preset and `.bsp` package, engine runtime changes, the `bspc` compiler, and the step-4 factory-mode plan |
| [clock.md](design/clock.md) | The CLOCK design pass, W2's tempo core: tap, MIDI clock and host tempo as stamped events, the integer tempo phasor, Subdiv and note divisions, tempo-synced delays, clock-quantised grain births, smoothing, the MIDI breadboard, and the provisional owner decisions; its evidence and review dispositions are in [reviews/clock-record.md](design/reviews/clock-record.md) |

Hardware documentation and build guides will land here as the project matures.
