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

Hardware documentation and build guides will land here as the project matures.
