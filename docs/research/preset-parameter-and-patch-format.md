# Parameter Model, Preset/Patch Format, and Pedal-to-Plugin State Interchange

> Research document for **Brainscape**: an open-source granular delay pedal (Daisy Seed firmware + open hardware) and desktop VST3/CLAP plugin, from one shared platform-agnostic C++ DSP core.
> This document exists because the corpus names the preset/parameter contract as the single most important thing to design (microcosm.md rec #8, §13.1–13.2; granular-pedal-landscape.md's gap table) and then contains no research on it. `vst-and-shared-dsp.md`'s proposed `dsp/` interface — `Init(sampleRate, maxBlockSize)` / `Process(in, out, numFrames)` — has no seam for parameters at all. This document proposes the missing contract: how parameters are identified and versioned, how presets/patches are serialized, how the macro-mapping data model works, how firmware stores state in flash without glitching audio, how VST3/CLAP expose parameters to hosts, and how MIDI CC + soft takeover should behave.

---

## Summary

- **No comparable project ships a human-readable, git-diffable preset format.** ZOIA's patches are a fixed-size 32 KB opaque binary blob that Empress Effects has never officially documented — the only spec that exists was reverse-engineered by community contributors (with informal help from an Empress engineer) and lives in a third-party GitHub repo, [meanmedianmoge/zoia_lib](https://github.com/meanmedianmoge/zoia_lib). OWL patches are compiled code artifacts flashed via SysEx, not data files. Red Panda's Particle 2 presets are raw MIDI SysEx dumps. VST3's native preset container (`.vstpreset`) is a binary chunked file with an XML metadata chunk buried inside, not a plain-text format. **Brainscape shipping a genuinely human-readable, versioned, diffable preset format (JSON is sufficient) would be a first in this category**, not merely a nice-to-have — it directly fills the gap granular-pedal-landscape.md's gap table marks as verified and unclaimed.
- **ZOIA's own reverse-engineered format doc is the strongest cautionary tale available.** The [ZOIA Binary Format spec](https://github.com/meanmedianmoge/zoia_lib/blob/master/documentation/Binary%20Format.pdf) shows a 32 KB fixed patch size, little-endian fields, per-module "Module Version" numbers requiring the *entire historical set* of block-configuration versions to be known forever to decode old patches correctly, and a bit-packed "Starred Elements" section that overloads one 32-bit field to mean either a parameter-MIDI-CC assignment or a favorited connection depending on sign bit — clever, but exactly the kind of implicit, undocumented cleverness a from-scratch open project should not repeat.
- **VST3's parameter model is the right shape to copy for `dsp/`:** a stable, never-reused 32-bit integer `ParamID` per parameter, a normalized `[0,1]` float value, `ParameterInfo` flags (`kCanAutomate`, `kIsBypass`, `kIsReadOnly`, `kIsList`, `kIsProgramChange`), and sample-accurate automation delivered as time-stamped points in an `IParamValueQueue` per parameter per block. [VST3 Parameters & Automation docs](https://steinbergmedia.github.io/vst3_dev_portal/pages/Technical+Documentation/Parameters+Automation/Index.html). Rule: **parameter IDs must never be reused or reassigned** — Steinberg's own docs state plugins may only add/remove parameters across versions, and removing one loses any automation written against it. [same source]
- **CLAP's parameter model is functionally the same shape, with better-written migration guidance.** `clap_param_info.id` is a `clap_id` that "must never change." Rescanning after adding/removing parameters requires `CLAP_PARAM_RESCAN_ALL` and can only happen while the plugin is deactivated. CLAP explicitly warns: don't shrink a parameter's range across versions, be careful expanding it, and **hosts should store plain (denormalized) automation values, not normalized positions** — a directly reusable rule for Brainscape's own preset format. [clap/ext/params.h](https://github.com/free-audio/clap/blob/main/include/clap/ext/params.h)
- **Both plugin ABIs solve the "I added/removed/renamed a parameter" problem the same way: an explicit remap table, not silent breakage.** VST3's `IRemapParamID::getCompatibleParamID()` lets a plugin declare old-ID → new-ID mappings so a host can resynchronize automation lanes when loading an old project. [VST3 IRemapParamID docs](https://steinbergmedia.github.io/vst3_dev_portal/pages/Technical+Documentation/Change+History/3.7.11/IRemapParamID.html) CLAP's `rescan(CLAP_PARAM_RESCAN_ALL)` is the equivalent structural signal, paired with the "store plain values" rule above so a changed ID doesn't silently corrupt automation math. **Brainscape's own preset schema needs the identical mechanism**: a `schema_version` field plus a migration table keyed by stable parameter names (not array position), because a community patch repo makes format churn expensive exactly as the task brief warns.
- **VCV Rack's module-versioning pattern is the cleanest precedent for tolerant forward/backward compatibility in a JSON-based format**: each module's own `dataFromJson()` checks for the presence of a key; if a key introduced in a later plugin version is absent from an older saved patch, the module falls back to that version's old default, and the patch is "upgraded" transparently the next time it's saved. [VCV Rack Manifest docs](https://vcvrack.com/manual/Manifest), [Rack patch.cpp](https://github.com/VCVRack/Rack/blob/v2/src/patch.cpp) This "absent key = old default" rule is directly adoptable for Brainscape's JSON preset format and is far simpler than a migration-function chain.
- **Surge XT's own maintainers consider their current `.fxp`-wrapped format a mistake and are replacing it.** [GitHub issue #6627](https://github.com/surge-synthesizer/surge/issues/6627) proposes moving to a plain text format (XML or JSON, wavetables base64-encoded inline) specifically because the current binary FXP header prevents "regular text editing and proper formatting" — i.e., a mature, well-funded open-source synth project independently arrived at exactly the requirement microcosm.md's rec #8 states, and is treating the change as a breaking, major-version-only migration. This validates doing it right the first time rather than retrofitting later.
- **On Daisy hardware, saving a preset is a blocking flash operation today, not an async one — this is a real, primary-source-confirmed risk for glitch-free mid-performance saves.** libDaisy's `PersistentStorage<T>::Save()` calls `qspi_.Erase()` then `qspi_.Write()` synchronously, and the class's own header comments carry an explicit `\todo — Make Save() non-blocking` and `\todo — Add wear leveling`. [libDaisy PersistentStorage.h source, fetched directly from the repo] Brainscape must design around this rather than assume it: never call `Save()` from the audio callback (it already isn't — Daisy's audio path runs on a separate DMA-completion interrupt from the main loop where `Save()` would be called), debounce/gate saves to an explicit user gesture rather than autosave-per-tweak (there's no wear leveling), and keep the QSPI-resident preset blob tiny.
- **The Daisy Seed's QSPI flash chip (ISSI IS25LP064A/IS25LP080D, confirmed from libDaisy's own `qspi.h`) has a 4 KB minimum erase granularity and a 256-byte page-program size** — meaning even a single-byte preset change costs a full 4 KB sector erase-and-rewrite. This is fine for occasional explicit "save preset" gestures but wrong for continuous autosave or per-parameter persistence.
- **Loops cannot realistically live in the same QSPI region as parameter presets.** The IS25LP064A is only 8 MB total (with the bootloader already claiming the first 256 KB per Electrosmith's own bootloader docs), while a single 60-second stereo loop at 48 kHz float is ~23 MB by itself (per microcosm.md's own §13.5 math) — larger than the entire flash chip. **Loop-bearing presets must go on SD card via `FatFSInterface`**, not QSPI; the parameter/macro-map JSON blob is the only thing that belongs in QSPI. libDaisy's `WavWriter` class already demonstrates the buffered/async pattern needed to write large audio data to SD without blocking the audio callback, and that same buffering pattern is directly reusable for writing loop audio alongside a preset save.
- **No project researched implements Brainscape's proposed two-tier macro model (one knob → N parameters, each with its own curve/range, saved as data) at the shared firmware+plugin level** — the closest partial precedents are OWL's per-parameter curve/smoothing/hysteresis primitives (`getFloatParameter(name, min, max, default, lambda, delta, skew)` with `EXP`/`LIN`/`LOG` skew built into the single-parameter binding, [OwlProgram Patch.h source, fetched directly]) and Native Instruments' NKS "Macro" concept, where one macro knob drives several underlying plugin parameters simultaneously, pre-mapped for factory content. Neither is data-driven and shared across an embedded target and a plugin the way Brainscape needs. **This means the macro-mapping data model is genuinely novel work, not something to copy wholesale** — but OWL's per-target curve primitive (`skew` as a single exponent value) is a good, cheap building block to reuse for each individual macro→target mapping rather than inventing a full piecewise-curve system on day one.
- **MIDI relative/incremental CC has no single standard** — Ardour's own docs note controllers use at least four incompatible relative-CC encodings (Relative 2's Complement, Binary/Signed Offset by 64, Sign-Magnitude, and others) with **no reliable way to auto-detect which one a given controller uses**; hosts that support relative CC (Ardour, several DAW control-surface layers) simply expose a configurable choice per controller. Brainscape must pick one encoding, document it explicitly as a published spec, and treat "which relative encoding" as a per-controller-profile setting rather than assuming universality — this is exactly the class of problem that forced Morningstar MC users to build workarounds against the Microcosm's absolute-only CC map (microcosm.md §12.2 item 10).
- **Soft takeover ("Pickup") is a MIDI-controller/pedal-knob problem, not a DAW-automation problem, and this distinction resolves the task brief's open question about how it interacts with plugin automation: it doesn't, structurally.** Ableton Live's own manual defines three takeover modes for *external MIDI controllers* mapped to a parameter (`None`, `Pickup`, `Value Scaling`) — but host automation lanes are authoritative by definition; a DAW automation lane "jumping" a parameter on playback is the correct, intended behavior, not a defect to fix. [Ableton Live 12 manual, MIDI/Key remote control] Soft takeover in Brainscape only needs to exist in two places: (1) the pedal's own physical knobs vs. a freshly-recalled preset's stored values (exactly the Microcosm's documented #1 UX defect), and (2) an external MIDI controller sending absolute CC vs. the pedal/plugin's current parameter value. The desktop plugin's own on-screen knobs never need takeover logic against host automation — only if a user maps an external hardware controller to the plugin's own GUI, a secondary, much rarer case.
- **Surge XT did not have soft takeover for MIDI-learned parameters until a 2024 feature addition** ([issue #7510](https://github.com/surge-synthesizer/surge/issues/7510), merged via [PR #7639](https://github.com/surge-synthesizer/surge/pull/7639) on 2024-05-06) — i.e., even a mature, funded open-source synth treated this as new, non-trivial work, not a solved problem to assume for free. The shipped algorithm is a simple state machine: on MIDI learn, the parameter locks; incoming MIDI values are held in a "wait" state and compared against the current value; when the incoming value comes within a proximity threshold of the current value ("close enough"), control unlocks and 1:1 tracking begins — functionally identical to Ableton's "Pickup" mode definition, and directly implementable in Brainscape's shared parameter layer.
- **The `dsp/` interface can gain a parameter seam without breaking the platform-agnostic rule** by treating parameters the same way `vst-and-shared-dsp.md` already treats buffers: define identity, ranges and curves once in `dsp/` as pure data (a `constexpr` or code-generated `ParamDescriptor` table), and add two calls to the `Engine` interface — `SetParam(ParamId, float plainValue, uint32_t sampleOffset)` and `GetParam(ParamId)` — with sample-accurate application handled internally via the same atomic/lock-free patterns the sibling document already prescribes for cross-thread parameter updates. Neither firmware nor plugin code needs to know about VST3/CLAP/pots at all inside `dsp/`; each platform's thin wrapper layer (`firmware/`, `plugin/`) is solely responsible for turning a pot ADC read or a host automation event into a `SetParam()` call using IDs pulled from the identical shared table.

---

## 1. Prior art: parameter/preset interchange across embedded + desktop, and patchable systems

### 1.1 Empress Effects ZOIA — the strongest patch-sharing culture, and an entirely opaque format

ZOIA (and its Euroburo Eurorack sibling) is explicitly named by granular-pedal-landscape.md as having the category's strongest patch-sharing culture, centered on the third-party community site [PatchStorage](https://patchstorage.com/platform/zoia/), which supports search by author/tag/date/likes and packages patches as `.zip` archives containing at least one `.bin` file. [zoia_lib README](https://github.com/meanmedianmoge/zoia_lib)

But the format itself is neither official nor human-readable. The authoritative technical reference — [`meanmedianmoge/zoia_lib`'s "ZOIA Patch File binary Format Technical Document"](https://github.com/meanmedianmoge/zoia_lib/blob/master/documentation/Binary%20Format.pdf) — states plainly that it is a *reverse-engineered* document, "created and maintained by the contributors with the help of Steeve Bragg of Empress Effects Inc.," verified only against firmware v2.50 and v2.70, with an explicit disclaimer that "there are no guarantees that the information contained in this document is exact." Key structural facts, read directly from the spec:

- **Every patch file is a fixed 32 KB blob**, unused space zero-filled. There is no variable-length container; every patch consumes the same footprint on the SD card regardless of complexity.
- **All multi-byte fields are little-endian**, and text fields (patch names, module names) are fixed-width ASCII with an explicitly restricted character set (`a-z`, `A-Z`, `0-9`, space, dash, dot) — the format doesn't even support arbitrary Unicode names.
- **Structure is a flat sequence of sections**: Patch Header → Modules → Connections → Page Names → Starred Elements → (optional, firmware ≥1.10) Module Colors. Each Module definition embeds a **per-module-instance "Module Version"** field — not a whole-patch format version — meaning a correct decoder must retain the *entire historical catalog* of block/option layouts for every module type across every firmware version ever shipped, forever, to read old patches. This is the single most important lesson for Brainscape: **version at too fine a grain and you create an unbounded maintenance liability; version the whole patch schema, not each parameter's history.**
- **Connections reference modules and blocks by index**, and block indices are stable across which options happen to hide a block on the ZOIA grid — a "hidden but still present" model directly analogous to what Brainscape's macro system will need when a mode has fewer active macro targets than its declared maximum.
- **The "Starred Elements" section is a compact but opaque encoding**: a single 32-bit integer whose top bit selects parameter-type vs. connection-type meaning, whose next-most-significant bits (23–30) double as a MIDI CC assignment (offset by 1, so 0 means "none"), and whose remaining bits address a module+block or a connection index. It is a real, working design for "assign this control to a CC and mark it a favorite" in 4 bytes — but it required a standalone PDF and community reverse-engineering to explain, which is precisely the failure mode a documented, versioned JSON schema avoids for free.

**Takeaway for Brainscape:** ZOIA proves patch-sharing culture drives long-term engagement (granular-pedal-landscape.md's own conclusion) — but it proves that *despite*, not *because of*, its format. A vendor that had shipped an official, documented, JSON or similarly text-based patch format from day one would have gotten the same community benefit without years of unofficial reverse-engineering. That gap is exactly what Brainscape should not leave open.

### 1.2 Surge XT — a mature open-source synth abandoning its own binary-preset legacy

Surge XT's presets currently ship as `.fxp` files — the legacy Steinberg VST2 "preset" container format, which wraps arbitrary chunk data in a small binary RIFF-like header. Surge's own maintainers have an open, milestone-tagged issue — [surge-synthesizer/surge#6627](https://github.com/surge-synthesizer/surge/issues/6627), targeted at the "Surge XT 2.0" milestone and explicitly labeled a **Breaking Change** — proposing to replace `.fxp` with a plain **text-based format (XML or JSON)**, with binary payloads like wavetables base64-encoded inline. The stated motivation is that the current format "requires dealing with FXP header and whatnot" and "prevents regular text editing and proper formatting." The issue does not resolve migration details in the visible discussion, but its existence is itself the finding: **a project with Surge's scale, funding, and engineering maturity independently concluded, years after `.fxp` had real-world traction, that a git-diffable text format was worth a breaking major-version change.** Brainscape gets to make that same decision on day one instead of retrofitting it after a community has already accumulated binary patches.

Separately, Surge's synthesis engine itself (`src/common`) is decoupled enough from JUCE to build headless (documented in `vst-and-shared-dsp.md` already), which is the architecture precedent for keeping parameter *identity* and *ranges* inside the shared core rather than the JUCE-facing wrapper — directly relevant to §7 below.

### 1.3 Cardinal / VCV Rack — JSON patches, module-level schema tolerance, immutable slugs

VCV Rack's `.vcv` patch format is a **POSIX tar archive compressed with Zstandard**, containing a `patch.json` describing the rack plus, optionally, a per-module "patch storage" directory where a module can stash arbitrarily large private files (comparable to Brainscape wanting to store a loop alongside a preset). [VCV Rack Manifest docs](https://vcvrack.com/manual/Manifest), [Rack/src/patch.cpp](https://github.com/VCVRack/Rack/blob/v2/src/patch.cpp)

Two design choices are directly reusable:

1. **Plugin/module "slugs" (stable string identifiers) must never change once released, because patch compatibility depends on them.** This is the JSON-world equivalent of VST3's "ParamID must never change" rule, and it should apply to Brainscape's `mode` identifiers and macro-target parameter names, not just numeric IDs — human-readable slugs are also what make a diff readable in a git patch repo.
2. **Backward/forward compatibility is handled per-key, not per-file-version.** When a saved patch is missing a JSON key a newer module version expects, the module falls back to that key's old default and the option becomes "upgraded" transparently the next time the patch is saved — no explicit migration function chain required for additive changes. Combined with an explicit top-level `schemaVersion` for the rare *breaking* change, this is close to the ideal balance of simplicity and robustness for Brainscape's own format.

**Cardinal** (`DISTRHO/Cardinal`, already covered in depth in `vst-and-shared-dsp.md`) wraps the entire VCV Rack engine — patch format included — inside a plugin, meaning a Cardinal preset *is* a VCV patch. This is the closest real precedent to "the same patch format works identically whether the host is a standalone rack or a plugin," which is structurally what Brainscape needs between firmware and plugin, just across a hardware/software boundary instead of a plugin/standalone one.

### 1.4 Red Panda Particle 2 — presets as literal, replayable MIDI SysEx

Red Panda's [web-based editor](https://www.redpandalab.com/editor/) for the Particle 2 (praised by granular-pedal-landscape.md as the "real granular synthesis for musicians, not menus" benchmark) connects to the pedal over USB and communicates entirely in MIDI SysEx. Per Red Panda's own documentation, the editor "prints the System Exclusive messages that it sends to the JavaScript console," and critically: **"When preset data is retrieved, the returned SysEx string is a valid set message that can be sent back to the pedal to restore the preset at the same location."** In other words, a Particle 2 "preset" *is* the SysEx command required to recreate it — there is no separate file format at all; the wire protocol and the storage format are the same bytes. The pedal stores up to 127 user presets. [ManualsLib mirror of Particle 2 manual](https://www.manualslib.com/manual/1573171/Red-Panda-Particle-2.html)

This is a genuinely elegant idea worth partially borrowing: **a Brainscape preset file can be defined as "the exact sequence of parameter-set operations needed to reproduce this state,"** which composes naturally with a JSON array of `{param, value}` pairs and sidesteps needing a second, divorced "apply preset" code path from "receive live parameter changes" — both go through the identical `SetParam()` call. It is not, however, a git-diffable *design* format on its own (raw SysEx bytes are exactly as opaque as ZOIA's binary blob) — Brainscape should take the *replay* idea, not the *SysEx-as-storage* implementation.

### 1.5 OWL / OwlProgram — the closest hardware+desktop precedent, but patches are code, not data

As `vst-and-shared-dsp.md` already documents, OWL is the single closest analog to Brainscape's ambition: identical patch code runs on OWL hardware or, via OwlSim, as a desktop VST/AU. Reading OwlProgram's actual SDK source directly (`LibSource/Patch.h`, `PatchParameter.h`, `OpenWareMidiControl.h`) clarifies exactly what this precedent does and does not solve for Brainscape:

**What it solves — the per-parameter binding primitive.** `Patch::getFloatParameter(name, min, max, defaultValue, lambda, delta, skew)` binds a single knob (`PARAMETER_A`…`PARAMETER_H`, extendable to 40 total across four shift-banks `AA`…`DH`) to a scaled float range, with **smoothing (`lambda`), hysteresis/stiffness (`delta`), and a curve (`skew`, with named constants `EXP`/`LIN`/`LOG`)** all built into the binding call itself, not bolted on separately. [OwlProgram `Patch.h`, fetched directly from GitHub] This is exactly the right *unit* primitive for a single macro→target mapping in Brainscape's own model (§4) — reuse the shape, not the exact API.

**What it does not solve — presets as data.** An OWL "patch" is a compiled program (C++, or a Faust/Pure Data/Max Gen/SOUL source cross-compiled to one), uploaded to the device via a SysEx-based firmware protocol (`SYSEX_FIRMWARE_UPLOAD`/`STORE`/`RUN`/`FLASH`, per `OpenWareMidiControl.h`). Switching "patches" on an OWL means swapping which compiled program is loaded — there is no separate preset/patch-*state* file format at the SDK level to interchange between hardware and OwlSim; the *code itself* is the interchange unit. This means OWL validates "one codebase, two runtimes" (already covered) but contributes nothing to the specific preset-serialization question this document exists to answer — that gap is exactly why this document had to be written from first principles rather than copied from the closest analog.

### 1.6 Cross-project comparison

| Project | Format | Human-readable / diffable? | Officially documented? | Versioning granularity |
|---|---|---|---|---|
| ZOIA | Fixed 32 KB binary blob | No | No — community reverse-engineered | Per-module-instance version number |
| Surge XT (current) | `.fxp` (binary FXP wrapper) | No | Yes (legacy VST2 format), but maintainers consider it a mistake | Whole-patch, ad hoc |
| Surge XT (planned, `#6627`) | Proposed text (XML/JSON) | Yes (planned) | Would be, as part of Surge XT 2.0 | Unresolved in visible discussion |
| VCV Rack / Cardinal | `.vcv` = tar+zstd of `patch.json` + per-module blobs | Partially — JSON is readable once decompressed, not a plain diffable file on disk | Yes, official | Per-module (slug + per-key fallback) + patch schema version |
| Red Panda Particle 2 | Raw MIDI SysEx dump | No (opaque bytes), though it is literally the "recipe" to reproduce state | Yes, documented protocol | Not really versioned as a file format |
| OWL / OwlProgram | N/A — patches are compiled code, not saved state | N/A | Yes (SDK is documented) | N/A |
| VST3 `.vstpreset` | Binary chunked container w/ XML metadata chunk | No (binary header/chunk list; XML only inside one chunk) | Yes, official Steinberg spec | Per-parameter ID stability + `IRemapParamID` |
| **Brainscape (proposed)** | **JSON** | **Yes** | **Yes — first-class, versioned spec** | **Whole-schema `schema_version` + per-key fallback + name-keyed remap table** |

No project surveyed ships what microcosm.md's rec #8 asks for. Brainscape doing so is a real, verifiable differentiator, not a marketing claim.

---

## 2. Plugin-side mechanics: how VST3 and CLAP expose parameters

### 2.1 VST3

- **Identity.** Every parameter has a `ParamID` (32-bit integer). Per Steinberg's own developer portal: *"it is not allowed to change this assignment at any time."* Plugins may add or remove parameters across versions, but "automation data can get lost when parameters are removed." [VST3 Parameters & Automation](https://steinbergmedia.github.io/vst3_dev_portal/pages/Technical+Documentation/Parameters+Automation/Index.html)
- **Value representation.** All parameter values communicated between host and plugin are **normalized floats in `[0.0, 1.0]`**; the plugin's Processor and Controller convert to/from a "plain" (DSP-meaningful) value internally.
- **Flags** (`ParameterInfo`) include `kCanAutomate` (host may record automation), `kIsBypass` (Steinberg "highly recommends" every effect expose one, and it must be saved/restored via `getState`/`setState` like any other parameter), `kIsReadOnly`, `kIsList` (render as a discrete list in generic editors), and `kIsProgramChange` (incompatible with normal automation on that parameter).
- **Sample-accurate automation.** During `process()`, automation arrives via `IParameterChanges`, a collection of per-parameter `IParamValueQueue`s, each holding automation points tagged with an in-block sample offset — the plugin decides how much of that time resolution to actually honor.
- **State (presets/projects).** The Processor implements `getState`/`setState` for its own audio-relevant state; the Controller separately implements `getState`/`setState` (and receives the Processor's state via `setComponentState`) so it can keep its own UI-facing parameter cache in sync. [VST3 EditController docs, via DeepWiki summary of `vst3_public_sdk`]
- **Migration mechanism.** `IRemapParamID` exists specifically for "a newer plugin replaces an older one with a different UID" or "a plugin updates and changes its parameter IDs." The plugin implements `getCompatibleParamID()` returning an old-ID → new-ID table; the host uses it, on load, to resynchronize automation lanes and remote-control assignments so old projects keep working. [VST3 IRemapParamID docs](https://steinbergmedia.github.io/vst3_dev_portal/pages/Technical+Documentation/Change+History/3.7.11/IRemapParamID.html) A plugin signals that a remap is needed via the `kParamIDMappingChanged` flag during state loading.
- **The `.vstpreset` file format.** A 48-byte header (`'VST3'` magic, version int32, 32-byte ASCII class ID, 8-byte offset to the chunk list) precedes a data area holding one or more named chunks, followed by a chunk-list directory. Chunk types include `kComponentState`, `kControllerState`, `kProgramData`, and `kMetaInfo` — the last of which holds **XML-encoded metadata** (name, descriptive tags such as "Acoustic"/"Clean"/"Percussive"). [`vst3_public_sdk/source/vst/vstpresetfile.h`, fetched directly from GitHub] This confirms the format is binary-container-with-an-XML-chunk-inside, not a plain text file — reinforcing why Surge's maintainers consider `.fxp` (a conceptual cousin) worth replacing for their own community-facing format.

### 2.2 CLAP

- **Identity.** `clap_param_info.id` is a `clap_id` (32-bit) that, per the header's own comment, "must never change." [`clap/ext/params.h`, fetched directly from GitHub]
- **Metadata.** Each parameter also carries a `module` string — a hierarchical, `/`-separated path (e.g. `"Granular/Voice 1/Pitch"`) — which is a built-in, standardized way to express the kind of nested organization Brainscape's macro tree will need (macro → targets), without inventing a separate taxonomy.
- **Flags** cover the same ground as VST3 plus real modulation support: `STEPPED`, `PERIODIC` (for wrapping values like phase), `HIDDEN`, `READONLY`, `BYPASS` (a stepped 0/1 parameter the host and plugin bypass states merge into), `AUTOMATABLE` (+ per-note-ID/per-key/per-channel/per-port variants), `MODULATABLE` (+ the same four per-dimension variants, for real-time non-destructive modulation separate from automation), and `REQUIRES_PROCESS` (value changes must be applied on the audio thread, not just cached).
- **Sample-accurate changes.** The plugin sends `CLAP_EVENT_PARAM_VALUE` events with per-sample offsets during `process()` or `flush()` (never both concurrently), with `CLAP_EVENT_PARAM_GESTURE_BEGIN`/`END` bracketing a user's knob-drag gesture — directly useful for Brainscape's own soft-takeover state machine (§6), since "gesture begin" is the natural moment to decide whether a takeover lock should be armed.
- **Rescanning after structural changes.** `clap_host_params_t::rescan(flags)` is how a plugin tells the host its parameter list changed. `CLAP_PARAM_RESCAN_VALUES` (values changed, e.g. after a preset load), `..._TEXT` (display-text formatting changed), `..._INFO` (name/module/flags changed), and `..._ALL` — the last of which **requires the plugin to be deactivated first**, i.e., you cannot add or remove parameters live mid-stream; that's an explicit, host-visible transition, not a silent one.
- **Explicit migration guidance in the spec itself:** "avoid shrinking ranges; expanding ranges risks automation playback anomalies," and — the most directly reusable rule for Brainscape's own JSON format — **"hosts should store plain automation values, not normalized positions."** Storing a preset's parameter values in real (plain) units rather than a `[0,1]` fraction of *whatever the current range happens to be* is what lets a later firmware/plugin update widen a parameter's range without silently rescaling every existing preset's meaning.
- **Preset Discovery** (`clap/factory/preset-discovery.h`) lets a host index a plugin's presets across arbitrary locations (filesystem paths or bundled inside the plugin binary) without loading the plugin, via metadata callbacks (name, `load_key`, creators, description, feature tags, timestamps, factory/user/demo/favorite flags). Crucially, **CLAP does not mandate any particular preset file format** — "it is the indexer's job to understand it and remap it to its internal categorization." [`clap/factory/preset-discovery.h`, fetched directly] This means Brainscape's own JSON preset format can be registered with CLAP hosts as a first-class discoverable preset type without any format compromise — the extension is a discovery/indexing layer over whatever format the plugin chooses, not a competing storage format Brainscape would have to also support.

### 2.3 What breaks, and the shared migration story to design around

Synthesizing VST3 + CLAP + VCV Rack's precedents, the pattern that recurs across all three ecosystems is: **identity must be permanent, but the list of identities is allowed to grow, shrink, or be explicitly remapped, as long as the format carries a version marker and either (a) a name/ID-keyed remap table or (b) tolerant per-key defaulting.** Concretely, for Brainscape's own preset schema:

1. Never reuse a parameter's stable name/ID for a different meaning, ever, even across a major version — retire it and mint a new one (VST3 rule).
2. Store `schema_version` at the top of every preset file, and store parameter values keyed by **stable string name**, not array position or a bare integer that could silently collide (a lesson from ZOIA's opaque bit-packed fields).
3. Store parameter values in **plain (denormalized) units**, not normalized `[0,1]`, so a later firmware update can widen a range without needing to touch every existing preset file (CLAP's explicit guidance).
4. When loading an older preset whose schema lacks a key a newer build expects, fall back to that build's declared default for the key, exactly as VCV Rack modules do — this handles the overwhelming majority of additive changes with zero migration code.
5. For the rare breaking change (a parameter removed, split, or reinterpreted), ship an explicit `migrations: [{from_version, to_version, ...}]`-style table or function keyed by name, mirroring the *intent* of VST3's `IRemapParamID` and CLAP's `rescan(ALL)` — a deliberate, visible transition, not silent data loss.
6. Treat mode/macro identifiers as permanent slugs once released into any published patch, following VCV Rack's plugin-slug rule — this matters even more for Brainscape than for VCV, because Brainscape's slugs will appear in a community git repo where old commits (and old pedals still running old firmware) must keep meaning what they meant when written.

---

## 3. Firmware-side storage on the Daisy Seed

### 3.1 QSPI flash and `PersistentStorage<T>`

The Daisy Seed's external QSPI NOR flash is an **ISSI IS25LP064A (or IS25LP080D on some boards)**, confirmed directly from libDaisy's own `per/qspi.h` header comments, which state the chip's "smallest erase possible is 4kB at a time" (erasures happen in 4 KB/32 KB/64 KB increments) and that "page size is 256 bytes" for writes. The Daisy bootloader claims the **first 256 KB of QSPI flash** for the application image (`0x90040000`, a 256 KB offset from the QSPI base address) — anything written beyond that offset is untouched by the bootloader and safe for application data, per Electrosmith's own bootloader documentation. [`electro-smith/DaisyBootloader`](https://github.com/electro-smith/DaisyBootloader), [libDaisy bootloader getting-started doc](https://electro-smith.github.io/libDaisy/md_doc_2md_2__a7___getting-_started-_daisy-_bootloader.html)

libDaisy ships exactly one non-volatile storage abstraction, `daisy::PersistentStorage<SettingStruct>` (`src/util/PersistentStorage.h`), read directly from source for this document:

```cpp
template <typename SettingStruct>
class PersistentStorage {
  // ... 
  void Init(const SettingStruct &defaults, uint32_t address_offset = 0);
  State GetState() const;                 // UNKNOWN / FACTORY / USER
  SettingStruct &GetSettings();
  void Save();                            // marks USER, then StoreSettingsIfChanged()
  void RestoreDefaults();
 private:
  void StoreSettingsIfChanged() {
    // ... compares new vs. currently-stored settings ...
    if (settings_ != storage_data->user_data) {
      qspi_.Erase(address_offset_, address_offset_ + sizeof(s));
      qspi_.Write(address_offset_, sizeof(s), (uint8_t *)&s);
    }
  }
};
```

Verified, primary-source facts that matter directly for Brainscape's design:

- **It is a single-slot template.** One `PersistentStorage<T>` instance manages exactly one `SettingStruct` at one flash address. A multi-preset-slot system (Brainscape will want more than one saved patch, the way the Microcosm has 16) needs either multiple `PersistentStorage` instances at distinct `address_offset` values (each rounded down to a 256-byte boundary internally), or — better, given the write-amplification concern below — a custom single larger struct containing an array of preset slots, saved as one atomic blob.
- **`Save()` is synchronous and blocking.** The class's own header comment says so directly: `\todo — Make Save() non-blocking`. `StoreSettingsIfChanged()` calls `qspi_.Erase()` then `qspi_.Write()` in sequence, on the calling thread, with no async completion callback exposed. Because the erase granularity is 4 KB minimum, even a one-byte change in a `SettingStruct` costs a full sector erase-and-rewrite.
- **There is no wear leveling.** Also flagged directly in the class's own doc comment: `\todo — Add wear leveling`. NOR flash sectors have a finite (commonly on the order of ~100,000) program/erase cycle rating; a design that calls `Save()` on every parameter tweak (rather than an explicit user "save preset" gesture) would measurably shorten the flash's service life over years of use.
- **It does include a "don't rewrite if unchanged" optimization** (`if (settings_ != storage_data->user_data)`), which somewhat mitigates the wear concern for the common case of repeatedly calling `Save()` with identical data (e.g., an accidental double-press), but does nothing for genuinely repeated distinct writes.
- **It correctly skips writing when nothing changed even across power cycles**, and distinguishes `FACTORY` (never explicitly saved by the user) from `USER` (has been saved at least once) state — a useful two-state model Brainscape's own preset-slot struct should probably keep (each slot could be `EMPTY`/`FACTORY`/`USER`).

**Does saving a preset glitch audio, concretely?** Not directly, if firmware is architected the way Daisy's own examples already are: the audio callback runs on a DMA-completion **interrupt**, while `PersistentStorage::Save()` would be called from the **main loop** (e.g., on a long-press of a footswitch, polled outside the ISR) — these are different execution contexts, and the audio ISR still preempts the main loop by default, so a blocking QSPI erase in the main loop does not, by itself, stall the audio interrupt. However, two real caveats apply and should be verified on real hardware before shipping: (1) if Brainscape's firmware is built with `APP_TYPE=BOOT_QSPI` (running application code directly, memory-mapped, out of the same QSPI chip — needed for programs bigger than the ~512 KB internal-flash limit, which a full granular engine plus DSP core plausibly is), then an `Erase()`/`Write()` on that same chip requires switching the QSPI peripheral out of memory-mapped mode into indirect mode for the duration of the operation — during which the CPU cannot fetch instructions from QSPI at all, a well-known STM32H7 XIP/QSPI subtlety; any code or interrupt vector that would otherwise execute from QSPI during that window must already be resident in internal RAM, or the system will hang. This should be explicitly tested, not assumed, given Brainscape's likely `BOOT_QSPI` requirement. (2) even without that hazard, a full 4 KB sector erase on this flash family typically takes tens of milliseconds (NOR flash sector-erase timings in this class of chip are commonly quoted in the tens-of-milliseconds range; **the exact IS25LP064A datasheet timing table could not be retrieved in this research pass — flagged as unverified below**), which is long enough to visibly stutter any main-loop-driven UI (LED updates, footswitch debounce) even if audio itself is unaffected — a real, if minor, UX cost worth designing the save gesture around (e.g., a brief "saving…" LED animation).

### 3.2 SD card storage for loops (`FatFSInterface`)

Because a single 60-second stereo loop at 48 kHz float is roughly 23 MB (per microcosm.md §13.5) and the entire QSPI chip is only 8 MB, **any preset that embeds a loop — as the Microcosm's presets do — must store that loop on SD card, not in QSPI.** libDaisy's `FatFSInterface` provides standard `f_open`/`f_write` FatFS semantics against an SDMMC-attached card, and the existing `WavWriter<transfer_size>` class demonstrates the pattern Brainscape needs: buffered, chunked writes driven from outside the audio ISR, so a long-running file write does not itself become a real-time hazard. `WavWriter` already proves the "record live audio to SD without glitching playback" case works on this platform — the identical buffering pattern (audio ISR pushes into a lock-free ring buffer; a main-loop task drains that buffer to SD in bounded chunks) is directly reusable for "save a preset with an embedded loop" (write the loop audio from the loop-buffer, which is already sitting in SDRAM, to SD in the same chunked fashion, rather than as one giant blocking write).

**Design implication:** Brainscape's preset format should therefore be two-tiered at the storage layer even though it's one schema at the data-model layer: a small (single-digit-KB) parameter/macro-map JSON-equivalent blob that lives in QSPI and can be saved cheaply and often, plus an optional reference to a larger loop-audio file on SD that is only written when a loop is actually present and is written via the chunked/async pattern, never as one blocking call.

### 3.3 What this means for autosave / mid-performance saves

Given the confirmed blocking-and-non-wear-leveled nature of `PersistentStorage::Save()`, Brainscape should **not** implement continuous or implicit autosave of every parameter tweak to flash. The right model — consistent with how the Microcosm itself works (explicit hold-to-copy, then hold-to-confirm at a target slot) — is an explicit, infrequent, user-initiated save gesture, ideally with visible feedback during the brief write, and a firmware-level guard against saving unnecessarily-frequently (e.g., a minimum interval between saves, beyond the class's own unchanged-data skip).

---

## 4. The macro-mapping data model (microcosm.md §13.1)

### 4.1 What needs representing

microcosm.md §13.1 proposes that every macro (e.g., "Activity," "Repeats") be a **user-editable mapping from one physical/virtual knob to N underlying parameters**, each target with its own curve and range, saved with the preset and shareable. No project surveyed in §1 implements exactly this at the shared-firmware-and-plugin level. The closest usable primitives are:

- **OWL's per-target curve/smoothing/hysteresis binding** (`getFloatParameter(name, min, max, default, lambda, delta, skew)` with `EXP`/`LIN`/`LOG` `skew` constants) — a clean, cheap unit for *one* macro→target edge, even though OWL never composes several of these under a single external control as a saved, editable mapping.
- **NI Komplete Kontrol's "Macro"** concept, where turning one physical knob "is the same as adjusting the knob or slider of its target parameter" for several gathered parameters at once — validates the *musical* idea (this is functionally what the Microcosm already does today, just hard-coded in firmware rather than user-editable), but NI's macros are pre-mapped by the plugin/NKS content author for the desktop software layer only; there is no evidence of this being a shared, hardware-portable data format.

### 4.2 Proposed representation

Given no precedent solves this end to end, the cleanest data shape — using the CLAP `module` path idea (§2.2) and the OWL per-target-curve primitive (§4.1) as building blocks — is a small, flat, JSON-serializable record per macro:

```json
{
  "macro_id": "activity",
  "display_name": "Activity",
  "targets": [
    { "param": "granular.grain_density", "range": [0.1, 40.0], "curve": "exp", "curve_amount": 2.0 },
    { "param": "granular.voice_count",   "range": [1, 8],      "curve": "lin" },
    { "param": "scheduler.jitter",       "range": [0.0, 0.3],  "curve": "log", "invert": true }
  ]
}
```

Design rules that follow directly from the research above:

1. **`param` is a stable string name**, matching the "name-keyed, never-reused identity" rule derived in §2.3 — the same identity space the plugin uses for its VST3/CLAP `ParamID`s and the firmware uses for its internal parameter table (see §7), so a macro definition means the same thing on both platforms without translation.
2. **Curve is a small, closed enum** (`lin`/`exp`/`log`, mirroring OWL's `LIN`/`EXP`/`LOG` skew constants) plus a single scalar `curve_amount`, not an arbitrary breakpoint spline — this keeps the evaluation function trivial (a single `pow()`-based shaping function implementable once in `dsp/` and shared verbatim by firmware and plugin, avoiding any risk of the two platforms computing a mapping differently), while still covering the overwhelming majority of musically useful shapes. A more expressive piecewise curve can be added later as an additive schema change (§2.3 rule 4) without breaking this baseline.
3. **A macro's evaluation is a pure function of the macro's own value**, recomputed only when the macro control itself changes (not per-audio-sample) — each individual target's own existing per-parameter smoothing (already required by the RT-safety rules in `vst-and-shared-dsp.md`) absorbs the resulting rate of change, so macro fan-out adds no new real-time-safety surface.
4. **Macros are data, saved with the preset, not code** — directly satisfying microcosm.md §13.2's "modes as data, not code" principle and making community-shared macro remixes possible via the same git-diffable JSON preset repo, with zero recompilation.
5. **The macro itself should also be a first-class addressable parameter identity** (its own stable name/ID) so it can be MIDI-CC-mapped, automated in the plugin, and driven by an expression pedal exactly like any leaf parameter — the pedal panel and the plugin's compact view expose only macros; the plugin's (and a future editor's) advanced view expose the full underlying parameter tree, exactly as microcosm.md §13.1 specifies.

### 4.3 Consequence for the parameter surface exposed to hosts

Because a macro is itself an addressable parameter, a VST3/CLAP host sees: N macro parameters (small, curated, matching the pedal's physical panel) **plus** the full underlying parameter tree (for automation power users and the future editor), exactly mirroring how NI's NKS exposes both a curated macro row and full parameter pages. Automating a macro parameter and automating one of its underlying targets directly are both valid and should both work — the underlying target's value at any instant is simply `f(own_direct_automation_if_any, macro_value)`, and the simplest correct rule (also the one least surprising to a DAW user) is **last-write-wins per audio block**: whichever of "macro fan-out" or "direct automation on this specific target" issued the most recent `SetParam()` call for that parameter ID takes effect, with no attempt at automatic blending between the two.

---

## 5. MIDI CC map design

microcosm.md §8.1 already flags the Microcosm's own CC map colliding with MIDI-standard Channel Volume (CC7) and Pan (CC10) — a DAW or controller writing standard automation on those numbers fights the pedal. The map Brainscape designs should:

1. **Avoid CC0, 1, 6/38, 7, 10, 11, 32, 64, 65, and 121/123** — Bank Select, Mod Wheel, Data Entry (used by NRPN/RPN), Channel Volume, Pan, Expression, Bank Select LSB, Sustain, Portamento switch, and All-Controllers-Off/All-Notes-Off respectively — all of which carry either a strong conventional meaning most DAWs and controllers already assume, or (CC6/38, 98/99/100/101) are structurally reserved for (N)RPN addressing, which Brainscape should consider using deliberately (see point 4) rather than colliding with by accident.
2. **Publish the map as a versioned, documented spec** (not "a table in a PDF," per microcosm.md's own framing of the Microcosm's failure mode) — ideally the same JSON-schema philosophy as the preset format itself, so a controller-configuration tool can consume it programmatically instead of a human transcribing a table.
3. **Support relative/incremental CC for macro and mode navigation**, addressing the exact gap that forced Morningstar MC users into workarounds against the Microcosm's absolute-PC-only navigation. There is, per Ardour's own control-surface documentation, **no single industry-standard relative-CC encoding** — common variants include "Relative 2's Complement" (values 1–63 = positive increments of that magnitude, values sent as `128 − N` for negative `N`, 64 = center/no-op) and "Binary/Signed Offset by 64" (64 = zero, 65 = +1, 63 = −1) — and a host or controller **cannot reliably auto-detect which variant another device is using**; DAWs that support relative CC at all (Ardour is a documented example) expose it as an explicit per-controller configuration choice rather than a universal default. Brainscape should: pick one encoding (Relative 2's Complement is the more commonly implemented of the two in hardware controllers, based on the sources reviewed here), document it explicitly, and expose a firmware/plugin setting for "which relative encoding to expect" per MIDI input, since a genuinely universal default does not exist in the wild.
4. **Consider NRPN (CC98/99 for the parameter number, CC6/38 for the 14-bit value) for the full underlying-parameter tree**, reserving plain CC only for the curated macro row — NRPN's larger address space (14-bit parameter number) comfortably covers a macro's full fan-out of underlying parameters without exhausting the 0–127 CC space the way a flat CC-per-parameter scheme would once the "full parameter tree" (§4.3) is included.
5. **MIDI CC learn, and CC output of parameter changes** (already recommended in microcosm.md §13.7) should both route through the identical stable-name parameter identity used everywhere else in this document — a CC-learn table is itself just another small JSON-serializable mapping (`{cc_number: param_name}` or `{cc_number: macro_name}`), shareable and versioned the same way as a preset.

---

## 6. Soft takeover / pickup / relative modes

### 6.1 Where the concept applies, and where it structurally does not

The task brief's open question — "how this interacts with plugin automation, where the concept does not exist" — has a clean, precedent-backed answer: **it doesn't interact, because host automation is authoritative by definition.** Ableton Live's own manual defines the industry's clearest three-way taxonomy for *external MIDI controller* takeover, quoted directly from the Live 12 Reference Manual:

- **None:** "As soon as the physical control is moved, its new value is sent immediately to its destination parameter, usually resulting in abrupt value changes."
- **Pickup:** "Moving the physical control has no effect until it reaches the value of its destination parameter. As soon as they are equal, the destination value tracks the control's value 1:1."
- **Value Scaling:** "This option ensures smooth value transitions. It compares the physical control's value to the destination parameter's value and calculates a smooth convergence of the two as the control is moved."

These three modes exist to resolve a mismatch between a **physical control's position** and a **parameter's current value** — a category of problem that a DAW automation lane cannot have, because an automation lane doesn't have a "physical position" of its own to reconcile; it simply *is* the value at that point in time, and jumping to it on playback is the entire point of automation. Brainscape's plugin therefore needs **zero** takeover logic for its own on-screen controls responding to host automation. Takeover logic is needed in exactly two places, both involving a genuine physical-position-vs-stored-value mismatch:

1. **The pedal's own physical knobs after a preset recall** — precisely the Microcosm's documented #1 fixable UX defect (microcosm.md §7, "Known preset friction"): after loading a preset, a knob's physical position almost never matches the value the preset just set, and the Microcosm's manual explicitly documents that touching the knob jumps the value with no pickup behavior at all.
2. **An external MIDI controller's physical knob sending absolute CC**, mapped to either a pedal parameter or (more rarely) a plugin GUI control, where the same physical-position mismatch can occur after any patch/preset/bank change on the controller side.

The plugin only needs case 2's logic, and only for the secondary scenario of an external hardware controller driving the plugin GUI — not for host-written automation, which the plugin should always simply obey immediately, exactly as VST3/CLAP intend.

### 6.2 A concrete, precedent-backed algorithm

Surge XT shipped essentially this same feature only recently: [issue #7510](https://github.com/surge-synthesizer/surge/issues/7510), opened February 2024 requesting "snap mode" support for MIDI-learned parameters (citing Faderfox-style hardware as the reference behavior), was closed by [PR #7639](https://github.com/surge-synthesizer/surge/pull/7639), merged May 6, 2024. The shipped approach, per the merged implementation's commit history, is a small state machine functionally identical to Ableton's "Pickup" definition:

1. On entering takeover-armed state (a preset load on the pedal, or a MIDI-learn/bank-change event for an external controller), the target parameter **locks** — incoming physical/MIDI values are received but not yet applied.
2. Each incoming value is compared against the parameter's current (locked) value; when the incoming value comes within a small proximity threshold of the current value ("close enough"), the lock releases and the physical/MIDI control begins tracking the parameter 1:1 from that point forward.
3. A boundary case worth copying explicitly (flagged in the same PR): values at or near a parameter's **minimum/maximum extrema** must still be able to unlock control even though a naive proximity check could otherwise get stuck near a range edge — the shipped fix generalizes the "close enough" check to correctly handle extrema.

This is directly implementable in Brainscape's shared parameter layer as a small per-parameter (or per-macro) state (`locked: bool`, `lock_reference_value: float`) alongside the existing value, checked once per incoming physical-pot-read or MIDI-CC event — cheap, and naturally shared between firmware (physical pots) and the rarer plugin-with-external-controller case, since it operates on the same `SetParam()` entry point either way (§7). CLAP's `CLAP_EVENT_PARAM_GESTURE_BEGIN` event (§2.2) is a good, already-standardized signal for exactly when to arm a takeover lock in the rare plugin-side case.

### 6.3 Recommendation for the default mode

Given the Microcosm's own manual documents "no soft takeover" as the shipped (bad) default, and Ableton's own default behavior in most contexts is "Pickup" rather than "Value Scaling" (Value Scaling requires knowing the direction and magnitude of travel, which is harder to reason about live), **Pickup should be Brainscape's default and only mode at v1**, exposed as a firmware-level and plugin-level toggle only if user feedback later asks for Value Scaling as an alternative — this matches microcosm.md §13.6's own recommendation ("selectable, default to pickup").

---

## 7. Where parameters enter the `dsp/` interface without breaking the platform-agnostic rule

`vst-and-shared-dsp.md`'s Recommended Repo Layout establishes strict rules this section must respect: `dsp/` depends on nothing platform-specific, `firmware/` and `plugin/` both depend on `dsp/`, and there must be no `#ifdef`-forked logic inside the shared core. Extending the existing proposed interface —

```cpp
class Engine {
 public:
  void Init(float sampleRate, size_t maxBlockSize);
  void Process(const float* const* in, float* const* out, size_t numFrames);
};
```

— to carry parameters without violating any of those rules requires adding exactly three things, all of which are pure data or narrow function calls, none of which reference a hardware or plugin-framework header:

1. **A single, shared parameter identity table**, living in `dsp/include/brainscape/Params.h`, expressed as plain data (a `constexpr` array of `ParamDescriptor{ name, id, min, max, default_value, curve, group }`, or a small build-time code-generation step reading one YAML/JSON source of truth and emitting this header — a common pattern already implicit in how `vst-and-shared-dsp.md` treats buffer sizing as compile-time-parameterized data). This is the *one* place parameter identity, range, default, and curve are defined; both `firmware/`'s pot-reading code and `plugin/`'s VST3/CLAP parameter registration read from it, so a plugin `ParamID` (§2.1) or a CLAP `clap_id` (§2.2) and a firmware pot's target are guaranteed to mean the same thing because they're generated from the same table, never hand-duplicated.
2. **Two calls on `Engine` itself:**
   ```cpp
   void SetParam(ParamId id, float plainValue, uint32_t sampleOffset = 0);
   float GetParam(ParamId id) const;
   ```
   `SetParam` is the single entry point every platform uses — `firmware/`'s pot-polling loop calls it after applying takeover logic (§6) to a raw ADC read; `plugin/`'s VST3/CLAP wrapper calls it once per automation point pulled out of an `IParamValueQueue` or a `CLAP_EVENT_PARAM_VALUE`, passing along that event's own `sampleOffset` so `dsp/` can apply sample-accurate changes internally using the same atomic/lock-free cross-thread patterns `vst-and-shared-dsp.md` already prescribes for parameter updates generally — no new RT-safety surface is introduced. `plainValue` is always a real, denormalized unit (per the CLAP-derived rule in §2.3), so `dsp/` never needs to know a host-side normalized `[0,1]` representation exists at all; normalization/denormalization is entirely `plugin/`'s (or a future editor's) responsibility, kept out of the shared core.
3. **The macro-evaluation function from §4.2** (`float ApplyCurve(float macroValue, Curve curve, float amount, float rangeMin, float rangeMax)`) as a small, pure, allocation-free function in `dsp/`, called by `Engine::SetParam()` internally when the `ParamId` passed in identifies a macro rather than a leaf parameter, fanning out to the macro's declared targets via the same `SetParam()` call recursively — meaning a macro is not a separate code path from a leaf parameter from any platform's point of view, only from `dsp/`'s internal implementation.

Nothing above requires `dsp/` to `#include` a libDaisy, VST3, or CLAP header, and nothing above requires a preprocessor fork between firmware and plugin builds — exactly the constraint `vst-and-shared-dsp.md` sets. The preset file itself (the JSON described in §1–§2) is deliberately **not** parsed inside `dsp/` at all: `firmware/` and `plugin/` each own their own (thin, platform-appropriate) JSON-loading code that reads a preset file and issues a sequence of `SetParam()` calls against the identical shared table — mirroring the Red Panda "preset as a replayable sequence of set-operations" idea from §1.4, and keeping any JSON library dependency (which may not be embedded-friendly without care) entirely out of the platform-agnostic core.

---

## Recommendations for Brainscape

Concrete and opinionated, ordered by how foundational each decision is:

1. **Adopt JSON as the canonical preset/patch format, with a top-level `schema_version` and parameter values keyed by stable string name in plain (denormalized) units.** This single decision — unmatched by ZOIA, Surge's current format, VST3's `.vstpreset`, or Red Panda's SysEx dumps — is the concrete fulfillment of microcosm.md rec #8 and the unclaimed gap granular-pedal-landscape.md's gap table names. Do this before writing the DSP engine, not after, exactly as rec #8 demands, because every parameter added later has to fit this schema from day one.
2. **Never reuse a parameter or macro's stable name once it has appeared in any released firmware, plugin build, or committed community patch.** Retire and rename instead. This is the one rule VST3, CLAP, and VCV Rack all converge on independently, and it's the rule a git-based community patch repo makes most expensive to violate after the fact.
3. **Use per-key tolerant defaulting for additive schema changes (VCV Rack's pattern) and an explicit name-keyed migration table only for genuine breaking changes (VST3's `IRemapParamID` / CLAP's `rescan(ALL)` pattern).** Do not build a general migration-function-chain system on day one; it's premature complexity for a project with no legacy presets yet.
4. **Split preset storage into two tiers matching hardware reality**: a small parameter/macro JSON-equivalent blob in QSPI flash (cheap, safe for an explicit "save preset" gesture, sized well under any single 4 KB sector where practical), and, only when a loop is actually embedded, a reference to loop audio on SD card written via the same chunked/buffered pattern `WavWriter` already validates on this platform. Never attempt to fit loop audio in QSPI — it physically cannot (8 MB chip vs. ~23 MB for one 60-second stereo loop).
5. **Never call `PersistentStorage::Save()` (or equivalent) from the audio callback, gate it to an explicit user gesture rather than autosave-per-tweak, and budget for a brief (tens-of-milliseconds-class) main-loop stall during the save** — all three follow directly from libDaisy's own source-code TODOs (`Make Save() non-blocking`, `Add wear leveling`). If Brainscape ships `APP_TYPE=BOOT_QSPI` (likely, given a full granular engine's probable code size), explicitly test that a QSPI erase/write during normal operation does not stall instruction fetch for any RAM-resident interrupt path — this is a real, testable risk this research could not fully resolve from documentation alone (see Open Questions).
6. **Model a macro as `{ macro_id, display_name, targets: [{ param, range, curve, curve_amount }] }`**, evaluated by a single shared pure function in `dsp/`, with the macro itself registered as its own addressable parameter identity alongside every leaf parameter — satisfying microcosm.md §13.1's "editor/VST expose the full tree, pedal panel exposes only macros" requirement without inventing two separate parameter systems.
7. **Extend the `dsp/` `Engine` interface with exactly `SetParam(ParamId, float plainValue, uint32_t sampleOffset)` and `GetParam(ParamId) const`, backed by a single shared, code-generated-or-`constexpr` parameter identity table** (`dsp/include/brainscape/Params.h`) that both `firmware/`'s pot-reading code and `plugin/`'s VST3/CLAP parameter registration consume — this is the minimal seam that closes the gap `vst-and-shared-dsp.md` left open, without violating that document's own platform-agnostic dependency rules.
8. **Ship both VST3 and CLAP parameter registration generated from the same table**, with CLAP's `module` path field used to express the macro→leaf-parameter hierarchy natively, and register Brainscape's own JSON preset format with CLAP's Preset Discovery extension (which does not require conforming to any CLAP-defined format) so hosts that support it can index community presets without a Brainscape-specific plugin.
9. **Implement soft takeover ("Pickup") as the default and only mode at v1**, using the lock/proximity-unlock algorithm Surge XT shipped in 2024 (PR #7639) — applied to (a) the pedal's own physical pots after any preset recall, and (b) an external MIDI controller's absolute CC into either firmware or the plugin. Do **not** build any takeover logic for host DAW automation reaching the plugin's own parameters directly — that path should always apply immediately, matching every DAW's own expectation of what automation means.
10. **Publish the MIDI CC map as a versioned spec alongside the preset schema, avoid CC 0/1/6/7/10/11/32/38/64/65/98/99/100/101/121/123, and pick and document one relative-CC encoding (Relative 2's Complement recommended) rather than assuming controllers agree on one** — there is no universal standard to defer to, per Ardour's own documentation of the fragmentation, so silence on this point guarantees the exact Morningstar-workaround problem microcosm.md already flags.
11. **Treat this document's own JSON preset schema as the first artifact of the community patch repo** — publish the schema (with a real JSON Schema or equivalent validator) before the first factory preset ships, so every contributed patch from day one is validated against the same contract firmware and plugin both consume.

---

## Sources

**ZOIA / Empress Effects**
- ZOIA Patch Binary Format Technical Document (PDF, fetched and read in full for this research): https://github.com/meanmedianmoge/zoia_lib/blob/master/documentation/Binary%20Format.pdf
- `meanmedianmoge/zoia_lib` (patch manager, PatchStorage API client): https://github.com/meanmedianmoge/zoia_lib
- PatchStorage, ZOIA/Euroburo platform page: https://patchstorage.com/platform/zoia/

**Surge XT**
- Issue #6627, proposal to replace `.fxp` with a text-based (XML/JSON) format: https://github.com/surge-synthesizer/surge/issues/6627
- Issue #7510, soft-takeover ("snap mode") feature request for MIDI-learned parameters: https://github.com/surge-synthesizer/surge/issues/7510
- PR #7639, merged implementation of soft takeover (2024-05-06): https://github.com/surge-synthesizer/surge/pull/7639
- Surge Architecture.md (headless `src/common`, cited already in vst-and-shared-dsp.md): https://github.com/surge-synthesizer/surge/blob/main/doc/Surge%20Architecture.md

**VCV Rack / Cardinal**
- VCV Rack Manifest / plugin versioning docs: https://vcvrack.com/manual/Manifest
- Rack v2 `patch.cpp` source: https://github.com/VCVRack/Rack/blob/v2/src/patch.cpp
- Rack v2 Plugin API Guide: https://vcvrack.com/manual/PluginGuide

**Red Panda**
- Particle 2 web editor: https://www.redpandalab.com/editor/
- Particle 2.0.1 firmware/manual/web editor announcement: https://www.redpandalab.com/blog/particle-201-firmware-manual-and-web-editor/
- Particle 2 Owner's Manual (ManualsLib mirror, web-editor SysEx section): https://www.manualslib.com/manual/1573171/Red-Panda-Particle-2.html

**OWL / Rebel Technology**
- `RebelTechnology/OwlProgram` (SDK repo): https://github.com/RebelTechnology/OwlProgram
- `LibSource/Patch.h` (parameter binding API — `getFloatParameter`, curve/smoothing/hysteresis), fetched directly from GitHub via `gh api`
- `LibSource/PatchParameter.h` (parameter value container), fetched directly from GitHub via `gh api`
- `LibSource/OpenWareMidiControl.h` (parameter IDs A–H/AA–DH, SysEx command set), fetched directly from GitHub via `gh api`

**VST3**
- VST3 Parameters and Automation (developer portal): https://steinbergmedia.github.io/vst3_dev_portal/pages/Technical+Documentation/Parameters+Automation/Index.html
- VST3 `.vstpreset` format overview (developer portal): https://steinbergmedia.github.io/vst3_dev_portal/pages//Technical+Documentation/Locations+Format/Preset+Format.html
- `vst3_public_sdk/source/vst/vstpresetfile.h` (chunk type constants, header format), fetched directly from raw GitHub
- IRemapParamID (parameter-ID migration mechanism), developer portal change history: https://steinbergmedia.github.io/vst3_dev_portal/pages/Technical+Documentation/Change+History/3.7.11/IRemapParamID.html
- EditController and Parameter Management summary (DeepWiki over `vst3_public_sdk`): https://deepwiki.com/steinbergmedia/vst3_public_sdk/2.2-editcontroller-and-parameter-management

**CLAP**
- `clap/include/clap/ext/params.h` (parameter model, flags, rescan semantics, migration guidance), fetched directly from GitHub: https://github.com/free-audio/clap/blob/main/include/clap/ext/params.h
- `clap/include/clap/factory/preset-discovery.h` (preset indexing extension), fetched directly from GitHub: https://github.com/free-audio/clap/blob/main/include/clap/factory/preset-discovery.h
- CLAP tutorial part 2 (parameter implementation walkthrough): https://nakst.gitlab.io/tutorial/clap-part-2.html

**Daisy Seed / libDaisy firmware storage**
- `libDaisy/src/util/PersistentStorage.h` (full class source, incl. blocking-Save and no-wear-leveling TODOs), fetched directly from GitHub via `gh api`
- `libDaisy/src/per/qspi.h` (IS25LP064A/IS25LP080D chip identification, 4 KB erase / 256 B page specs), summarized via WebFetch of the GitHub source
- Daisy Bootloader (256 KB QSPI offset, `APP_TYPE=BOOT_QSPI` size limits): https://github.com/electro-smith/DaisyBootloader, https://electro-smith.github.io/libDaisy/md_doc_2md_2__a7___getting-_started-_daisy-_bootloader.html
- libDaisy `FatFSInterface` class reference: https://electro-smith.github.io/libDaisy/classdaisy_1_1_fat_f_s_interface.html
- libDaisy `WavWriter` class reference (buffered SD write pattern): https://electro-smith.github.io/libDaisy/classdaisy_1_1_wav_writer.html
- Daisy Community forum, "Saving values to Flash memory using PersistentStorage class on Daisy Pod" (search-snippet level only — full thread returned HTTP 403 to automated fetch): https://community.daisy.audio/t/saving-values-to-flash-memory-using-persistentstorage-class-on-daisy-pod/4306

**MIDI**
- Ardour Manual, "Generic MIDI and Encoders" (relative-CC encoding fragmentation, no auto-detection): https://manual.ardour.org/using-control-surfaces/generic-midi/working-with-encoders/
- MIDI.org, "MIDI 2.0 Protocol: Using Relative Registered/Assignable Controllers": https://midi.org/midi-2-0-protocol-using-relative-registered-assignable-controls

**Soft takeover / DAW takeover modes**
- Ableton Live 12 Reference Manual, MIDI and Key Remote Control (official Takeover Mode definitions — None/Pickup/Value Scaling), quoted directly: https://www.ableton.com/en/manual/midi-and-key-remote-control/

**Cross-referenced Brainscape internal research**
- `docs/research/microcosm.md` — rec #8, §7 ("Known preset friction"), §8.1 (CC7/CC10 collision), §12.2 item 10 (Morningstar relative-nav workaround), §13.1 (two-tier macro model), §13.2 (modes as data), §13.6 (soft takeover recommendation)
- `docs/research/vst-and-shared-dsp.md` — proposed `dsp/` interface, Recommended Repo Layout, RT-safety/cross-thread parameter update patterns
- `docs/research/granular-pedal-landscape.md` — gap table ("Desktop plugin twin sharing DSP/presets with the hardware," "Preset/patch-sharing community")

---

## Open questions

1. **Exact QSPI sector-erase and page-program timing for the IS25LP064A/IS25LP080D was not independently verified in this research pass.** The ISSI datasheet PDF was inaccessible to automated fetching (bot-detection page). libDaisy's own source confirms 4 KB minimum erase granularity and 256-byte page size, and that `Save()` blocks synchronously, but the actual millisecond-scale duration of a save (needed to size a "saving…" UI animation, or to confirm whether it's audible as a main-loop stall at all) should be measured on real hardware or pulled from the datasheet directly rather than assumed from this document's general NOR-flash-family knowledge.
2. **Whether a QSPI `Erase()`/`Write()` call actually stalls instruction fetch (and therefore risks a real fault, not just a UX stutter) under `APP_TYPE=BOOT_QSPI`** — i.e., running application code memory-mapped from the same QSPI chip being erased — was reasoned about from general STM32H7 XIP/QSPI architecture knowledge, not confirmed against Daisy-specific testing or an Electrosmith statement. This is a **build/hardware-in-the-loop test Brainscape should run before relying on `BOOT_QSPI` plus in-place preset saving**, not something safe to assume either way from documentation alone.
3. **The full internal structure of Surge's proposed post-`.fxp` format (issue #6627) is unresolved even in Surge's own tracker** — no final schema, migration plan, or timeline was visible in the fetched issue content, so it could not be used as a worked example of a real migration in progress, only as a validating data point that the format choice itself (`.fxp` → text) matches this document's own recommendation.
4. **This document does not benchmark any concrete JSON parsing library against Daisy Seed's embedded constraints** (`-fno-exceptions -fno-rtti`, newlib-nano, limited flash/RAM) — §7 deliberately keeps JSON parsing out of `dsp/` and into `firmware/`/`plugin/` specifically so this choice doesn't gate the shared core, but a follow-up research pass should evaluate embedded-friendly C++ JSON libraries (e.g. ones with no-exception build modes) before firmware implementation begins.
5. **No comparable pedal or plugin project's exact multi-parameter "macro" data schema could be found and evaluated end-to-end** (§4) — the proposed schema in this document is original synthesis from adjacent precedents (OWL's per-target curve primitive, CLAP's `module` path, NI's macro concept), not a verified, battle-tested format from an existing shipped product. It should be treated as a first draft to validate against real DSP parameter counts once the granular engine's actual parameter list exists, not as a finished spec.
6. **Whether Ableton's exact "Value Scaling" math (the convergence algorithm, not just its description) is worth implementing as a second takeover mode for Brainscape was not researched in technical depth** — this document recommends shipping only Pickup at v1 (§6.3) partly to avoid needing that answer yet.
7. **The Daisy Community forum thread on `PersistentStorage` returned HTTP 403 to automated fetching on every attempt in this session** — the class's own source code (read directly, and quoted in §3.1) was sufficient to answer the core questions, but any community-reported real-world gotchas discussed only in that thread's replies (rather than the class source itself) were not captured here.
8. **WebSearch quota was exhausted partway through this research session** (200/200 calls used), after which all further research relied on WebFetch and direct GitHub API/source access (`gh api`, raw file fetches). This was sufficient to reach primary sources for every focus area in the brief, but a few secondary/color topics originally planned (e.g. Line 6 Helix's preset format, norns' `.pset` text format as an additional human-readable-format precedent) could not be pursued and are flagged here as leads for a future pass rather than claims made without verification.
