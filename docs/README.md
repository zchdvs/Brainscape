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
| [clock.md](design/clock.md) | The CLOCK design pass, W2's tempo core: tap, MIDI clock and host tempo as stamped events, the integer tempo phasor, Subdiv and note divisions, tempo-synced delays, clock-quantised grain births, smoothing, the MIDI breadboard, and its owner decisions, every answer confirmed by the owner on 2026-10-08; its evidence and review dispositions are in [reviews/clock-record.md](design/reviews/clock-record.md) |
| [cpu-budget.md](design/cpu-budget.md) | The Seed's worst-case CPU budget, owner-approved: why the engine §8 estimate missed, the measured cost model, a bit-exact speed pack (steps 1–2 built), a deterministic cost governor at 85 % for any input, the factory gate, and bench session 2 |
| [output-limiter.md](design/output-limiter.md) | The output safety limiter, draft 4 (2026-10-10): the owner's answers, twelve confirmed and D5 marked "Change" with no note, then two reviews (draft 2, 2026-10-09): a zero-latency, linked peak limiter at the end of the engine for the pedal and the plugin, engaging only on samples over full scale (37 of 45 goldens unchanged), the wet giving way before the dry, a program-dependent release, a proposed per-preset switch awaiting the owner's confirmation (row 87, `output.limiter`, Limit by default; Clip clips at the same ceiling; whether factory presets may clip is the owner's D14), its arithmetic, placement, cost and ITCM, the Rev7 DAC test before minting, golden and audition impact, and sound revision 10, after CLOCK's 8 and 9 and before the governor; its evidence, review dispositions and the owner's answers are in [reviews/output-limiter-record.md](design/reviews/output-limiter-record.md) |

Hardware documentation and build guides will land here as the project matures.
