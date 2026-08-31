# The Granular / Glitch Delay Pedal Landscape (2026)

*Research reference for Brainscape — an open-source, Daisy Seed–based granular delay pedal shipping both hardware (stereo I/O) and a desktop VST twin from one shared C++ DSP core.*

## Summary

- **Hologram Microcosm ($459)** is the reference point Brainscape is explicitly chasing. It's stereo, 11 granular/looping algorithms, tap tempo, MIDI, a 60s looper, and is broadly loved — but reviewers and forum users consistently describe it as best used for "happy accidents" rather than precise sound design, and its price is repeatedly cited as the main deterrent. [hologramelectronics.com](https://www.hologramelectronics.com/products/microcosm)
- **No pedal in this category ships a desktop plugin twin built on the same DSP.** A KVR Audio thread of guitarists explicitly hunting for a software equivalent to the Chase Bliss Mood MK2 concluded no single plugin replicates it — a direct, verified gap Brainscape can own. [kvraudio.com](https://www.kvraudio.com/forum/viewtopic.php?t=625052&start=15)
- **Nobody in the pedal space does deep, no-menu-diving granular control at a low price with true stereo.** Cheap/mono (Montreal Assembly Count to Five, $170, mono-only) trades stereo for price; expensive/stereo (Microcosm $459, Mood MKII $399, ZOIA $511+) trades price for depth; only the DIY/open Daisy world offers real depth-per-dollar, but with no polish, no presets, and steep build/flash barriers.
- **Chase Bliss (Mood MKII $399, Habit ~$399 discontinued, Onward $399)** owns the "deep, secondary-function-laden, MIDI-everything" niche — beloved by tweakers, criticized hard for steep learning curves that require keeping written notes just to remember what a knob does in a given mode. [guitar.com](https://guitar.com/reviews/effects-pedal/the-big-review-chase-bliss-mood-mkii/), [premierguitar.com](https://www.premierguitar.com/gear/reviews/chase-bliss-habit)
- **Red Panda (Particle 2 $329, Tensor $329)** is the "real granular synthesis for musicians, not menus" benchmark — praised for a lucid interface with an accessible mode and an "expert mode" for deep grain-level access, i.e. progressive disclosure done right. [redpandalab.com](https://www.redpandalab.com/products/particle)
- **Montreal Assembly Count to Five ($170)** is the low-price granular benchmark, but it's mono-only, dense/manual-required, and comes from a tiny boutique maker with limited stock — proving cheap-and-deep is possible but nobody has done cheap-and-deep-and-stereo-and-polished together.
- **Walrus Audio's Fable ($299, mono)** and **Lore ($299, mono)** show a big, well-distributed brand chasing this space in the last few years but still shipping mono-only, knob-select-algorithm interfaces that reviewers say require the manual on hand to know what mode you're in.
- **Empress ZOIA ($511+)** and **Poly Effects Beebo ($399, but now essentially unobtainable — sold out, "available soon" for a year+)** prove that fully modular, patchable granular synthesis is craved by a real audience, but complexity and manufacturing/support fragility (Beebo) are recurring failure modes.
- **Meris LVX ($679) and Mercury X ($679)** show granular as a "bonus mode" bolted onto a delay/reverb flagship rather than the star of the show — proof that even premium boutique makers treat granular as a texture layer, not a first-class instrument.
- **The real prior art for "open, hackable granular" is Eurorack, not pedals**: Mutable Instruments Clouds (and its successor Beads) was fully open-source (MIT-licensed) hardware+firmware and spawned a huge clone/alt-firmware ecosystem (Parasites, After Later Audio Typhoon/Monsoon, TOIL Clouds clone, uBurst) — the exact community-remix dynamic Brainscape should aim to replicate in pedal form. [mqtthiqs.github.io](https://mqtthiqs.github.io/parasites/clouds.html)
- **The Daisy Seed community already has scattered, mono, hobbyist-grade granular firmware** (NitroTron3's SPRAWL mode on Hothouse, GPLv3; DaisyCloudSeed which is actually a Cloud Seed reverb port, not true granular) but nothing polished, stereo, documented, or adopted at scale — Brainscape can be the first "flagship" open pedal in this space rather than one more scattered bedroom-coder repo.
- **norns (Glut/Mangl) and Organelle granular patches** prove there's a mature software/DIY granular culture in the modular/synth-nerd world that guitarists mostly don't cross into — a documented, hackable, guitar-pedal-form-factor entry point could pull that crowd toward guitar-adjacent gear.
- **Desktop granular VST plugins are a healthy, separate market** (Audio Damage Quanta 2, Output Portal, Dawesome Novum, Korg Padshop 2) but none of them are designed to feel like, sync with, or share presets with a hardware pedal — the "one DSP core, two skins" model is unclaimed territory.
- **Preset ecosystems are essentially absent** across the whole category — nobody (Hologram, Chase Bliss, Red Panda, Walrus) has a community preset-sharing site or cloud sync; ZOIA's patch-sharing culture (via its own forum/Discord) is the closest thing that exists, and it's a big reason ZOIA owners stay engaged for years.
- **Stereo is inconsistent and often the first thing cut for cost**: Microcosm, Mood MKII, ZOIA, Beebo, and LVX/MercuryX are stereo; Count to Five, Fable, Lore, and Infinite Jets are mono. Nobody offers deep stereo granular *and* an accessible sub-$300 price *and* an open/hackable platform — that's Brainscape's clearest wedge.

## Survey of Existing Products

### Hologram Electronics — Microcosm
- **Concept**: "Granular Looper & Glitch Pedal" — micro-loops, delay, pitch shifting, sampling, reverb, modulation and filtering combined into 11 algorithms with 44 preset variations, plus a 60-second phrase looper. [hologramelectronics.com/microcosm](https://www.hologramelectronics.com/products/microcosm)
- **Granular capabilities**: cascading micro-loops that can be locked to tap tempo or diffused into ambient drones; resonant lowpass filter; pitch modulation; "Hold" freeze function.
- **Controls/UI**: physical knob-per-function plus MIDI (In/Out/Thru, clock sync) and an expression input mappable to most parameters; 16 user preset slots.
- **I/O**: stereo in/out.
- **Price**: $459 official; some retailers list ~$604 (likely bundle/import pricing). [equipboard.com](https://equipboard.com/items/hologram-electronics-microcosm-5235c3bf-504e-4acc-b937-9ba9f1b67dac)
- **Sentiment**: Engadget calls it "a cheat code for making ambient music." [engadget.com](https://www.engadget.com/hologram-electronics-microcosm-guitar-effect-pedal-review-ambient-music-170002707.html) TGP/Gearspace owners describe it as "a lot of magic in one box" and more usable for pads/swells/textures than the Chase Bliss Mood, but say it rewards happy-accident exploration over goal-directed use, and that you have to "learn the unit" — algorithms and their interactions aren't obvious from the panel. [gearspace.com](https://gearspace.com/board/so-many-guitars-so-little-time/1353956-anyone-using-hologram-microcosm.html) Price is the most-cited deterrent across reviews.

### Hologram Electronics — Infinite Jets
- **Concept**: "Multi-Voice Synth & Glitch Pedal" / resynthesizer — two independent sampling channels for infinite sustain of notes/chords, resynthesized via 4 modes: Blur, Synth, Glitch, Swell. [hologramelectronics.com/infinite-jets](https://www.hologramelectronics.com/pages/infinite-jets)
- **Granular-adjacent capability**: Glitch mode stores audio in six fragments and plays them back at random/controlled intervals — closer to a synth/resynthesizer than a classic grain-cloud engine.
- **Controls/UI**: single "Dimension" macro knob whose function changes per preset; internal LFO/envelope; expression pedal mapping; knob-movement recording; 10 presets + 2 user slots.
- **I/O**: mono trigger detection is described in the manual (Mono/Poly *trigger* modes for how channels A/B alternate) — this refers to note-triggering behavior, not confirmed audio I/O; treat physical stereo/mono jack configuration as **unverified** pending a manual read of the connector diagram.
- **Price**: $299.
- **Sentiment**: One of Hologram's best-received releases, appeared on several "Best of 2017" lists; praised as constantly evolving and hard to pigeonhole (a positive in most reviews). [pedal-of-the-day.com](https://www.pedal-of-the-day.com/2019/09/14/hologram-electronics-infinite-jets-resynthesizer/)

### Chase Bliss Audio — Mood MKII
- **Concept**: "supercreative, granular ambient loop + reverb pedal" built around an always-listening micro-looper across two interacting channels. [chasebliss.com/mood-mkii](https://www.chasebliss.com/mood-mkii)
- **Granular capabilities**: micro-looping with overdub, freeze, sync, fade, filter, balance, blend, smoothing; a single "Clock" knob controls sample rate of both channels at once for harmonized pitch/time shifts (a granular-adjacent technique rather than a classic grain-density/size pair).
- **Controls/UI**: dense physical control set with **hidden secondary parameters only reachable via MIDI** ("control over every parameter, even the hidden ones") — the archetypal Chase Bliss "deep but opaque" design. MKII added MIDI-keyboard playability (transposition via MIDI = de facto granular synth mode).
- **I/O**: stereo (new in MKII; MKI was more limited).
- **Price**: $399 US / €469 EU. [guitar.com](https://guitar.com/news/gear-news/chase-bliss-mood-mkii-launched/)
- **Sentiment**: Guitar.com calls it "a knob-twiddler's delight" that can make "magical sounds you won't find anywhere else," but flags a steep learning curve and "luxury pricing" that limits accessibility. [guitar.com](https://guitar.com/reviews/effects-pedal/the-big-review-chase-bliss-mood-mkii/)

### Chase Bliss Audio — Habit (Echo Collector) — *discontinued*
- **Concept**: not a delay in the traditional sense — always-on 3-minute rolling buffer that continuously records and loses its oldest audio, styled as a delay/looper hybrid alongside Count to Five and Tensor as its stated peer group. [premierguitar.com](https://www.premierguitar.com/gear/reviews/chase-bliss-habit)
- **Granular-adjacent features**: "Dropper" causes deliberate playback errors for stuttery, granular-style fade-outs and chopped rhythmic glitches; "Trimmer" creates rhythmic repeats/glitches; "Stability Modifier" adds tape-style wow/flutter.
- **Controls/UI**: dense, described as needing the manual/notes to track state given the pedal's continuous, stateful recording behavior.
- **I/O**: unverified stereo/mono spec (not found in this pass).
- **Price**: officially discontinued (confirmed: retired in early 2025 after ~3 years in production, launched March 2022, sent off with a 500-unit "Limited Shareware Edition" that sold out immediately to newsletter subscribers — Chase Bliss's own framing was that Habit "did not take off to the same degree as its siblings, particularly not as well as [Mood]"); secondary market ~$375–$500, original retail ~$399. [guitarpedalx.com](https://www.guitarpedalx.com/news/chase-bliss-retires-its-habit-echo-collector-experimental-delay-looper-after-4-years-of-service)
- **Sentiment**: "A revelation," "super creative, very playable," generates "lots of surprises and happy accidents" — but a steep learning curve is the universal caveat. [soundgas.com](https://soundgas.com/blogs/blog/chase-bliss-habit-demos-and-review) Its commercial underperformance relative to Mood is itself a data point: even a beloved, critically-praised granular/glitch design can fail commercially if it's *too* opaque/stateful for most buyers to keep using day-to-day.

### Chase Bliss Audio — Onward (Dynamic Sampler)
- **Concept**: envelope-triggered dynamic sampler split into two parallel channels — Freeze (sustained pad) and Glitch (rhythmic, lo-fi randomized mangling) — a spin on Tom Majeski's earlier Cooper FX Outward. [guitarpedalx.com](https://www.guitarpedalx.com/news/gpx-blog/chase-blisss-latest-onward-dynamic-sampling-device-is-a-really-smart-tom-majeski-pet-project--extrapolation-from-his-former-cooper-fx-outward-pedal)
- **I/O**: stereo TRS ins/outs; supports mono, stereo, or mono-to-stereo.
- **Price**: $399 US / €469 EU.
- **Sentiment**: praised as adventurous and sampling-forward but explicitly framed by reviewers as *not for everyone* — "Chase Bliss fashion," niche by design.

### Red Panda Lab — Particle 2
- **Concept**: dedicated granular delay/pitch-shifter — the closest thing to a "musician's granular synth" in pedal form. [redpandalab.com/particle](https://www.redpandalab.com/products/particle)
- **Granular capabilities**: adjustable grain size and density with independent tap divisions; 7 pitch-quantization settings (semitones, octaves, octave+fifth, etc.); 5 delay modes (random, density, LFO, random pitch, reverse); 0–2.5s delay range; auto-freeze/stutter.
- **Controls/UI**: explicitly designed with an accessible "performance" layer plus a hidden **expert mode** exposing all raw grain parameters — praised as a progressive-disclosure model other makers should study. Also has a web-based editor and TouchOSC support for deeper/remote control.
- **I/O**: true stereo I/O via TRS.
- **Price**: $329 (some sources reference an older $299).
- **Sentiment**: consistently called well-made with a "lucid interface" and "a vast repertoire of head-spinning tones"; Gearspace users call it "the best hardware unit out for real-time [granular synthesis]." [gearspace.com](https://gearspace.com/board/electronic-music-instruments-and-electronic-music-production/1260378-red-panda-particle-v2-real-time-granular-synthesis-pedal.html) Some reviewers feel it's overpriced relative to a "normal" delay pedal, and its complexity can alienate players wanting a simple effect.

### Red Panda Lab — Tensor
- **Concept**: pitch- and time-shifting pedal with a multidirectional looper — reverse, tape-stop, pitch shift (±2 octaves), time stretch/compress independent of pitch, randomizer, MIDI. [redpandalab.com/tensor](https://www.redpandalab.com/products/tensor)
- **Granular-adjacent capability**: circular buffer (up to 9.4s mono / 4.7s stereo) that can be sliced randomly and held/looped/reversed instantly — time-manipulation rather than classic grain clouds, but squarely in the same "glitch/experimental delay" bucket users lump it into alongside Habit and Count to Five.
- **I/O**: stereo (buffer length halves in stereo mode: 9.4s mono vs 4.7s stereo).
- **Price**: $329 (older listings show $299).
- **Sentiment**: "hard to explain," "one of my favorite pedals" — praised for retaining warmth/organic tone even under heavy pitch/time manipulation.

### Montreal Assembly — Count to Five
- **Concept**: tiny-batch, three-mode experimental delay/sampler: Mode 1 is a straightforward granular delay (expression control over tape direction/buffer length/feedback), Mode 2 samples a clip with controllable granular slicing length/randomness, Mode 3 loops sampled clips with up to three simulated "tape heads." [mtlasm.com](https://mtlasm.com/product/count-to-5/)
- **Controls/UI**: dense, manual-required — a cult favorite specifically *because* of its idiosyncratic depth-per-dollar, not despite it.
- **I/O**: mono in/mono out, 1/4" TRS expression input.
- **Price**: $170 (official site currently shows a markdown from $200) — by far the cheapest pedal in this survey. [mtlasm.com](https://mtlasm.com/product/count-to-5/)
- **Sentiment**: "an amazing sound design tool," praised for depth and originality; frequently name-checked by Habit/Tensor reviewers as the genre's low-cost benchmark for weirdness-per-dollar. Limited-batch boutique manufacturing means availability is inconsistent (currently ~1 retailer stocking it per Equipboard, as of Aug 2026).

### Empress Effects — ZOIA (and Euroburo eurorack version)
- **Concept**: a fully modular "synthesizer in pedal form" — 90+ patchable modules, of which Granular Processor and Audio Buffer are community favorites for building custom granular delays/loopers from scratch. [empresseffects.com](https://empresseffects.com/products/zoia)
- **Granular capabilities**: not a fixed granular engine — a general-purpose modular canvas that can *become* a granular synth (and much else) via user patching. One reviewer says they "made a granular delay by [their] second night."
- **Controls/UI**: joystick + grid of buttons + tiny screen for patching; famously deep, described as "daunting at times" but with an almost limitless sonic ceiling. Comes with 64 factory presets to bootstrap new users.
- **I/O**: stereo (2x 1/4" in/out), MIDI I/O, expression/control port.
- **Price**: pedal from ~$511; Euroburo (eurorack version, 34HP, adds CV I/O) from ~$629.
- **Sentiment**: 5/5 across 18 reviews on Sweetwater; praised for years of continued firmware support and community-driven module additions. The interface learning curve is the recurring critique, offset by an active patch-sharing community (forum/Discord) that keeps owners engaged long-term — arguably the strongest *preset/patch ecosystem* in this entire survey.

### Poly Effects — Beebo
- **Concept**: touchscreen, Eurorack-inspired virtual modular synth in pedal form; 4-in/4-out routing, a 16-model macro-oscillator voice, and a dedicated granular "Clouds" module. [engadget.com](https://www.engadget.com/poly-effects-beebo-review-modular-guitar-pedal-touchscreen-151557875-151557578.html)
- **Granular capabilities**: the "Clouds (Granular)" module chops incoming audio into an "almost unrecognizable mass" in real time — one of the most-used modules per reviewers.
- **Controls/UI**: 5" touchscreen + modular patching, similar depth-for-effort tradeoff to ZOIA but with a screen-first rather than knob-first interaction model.
- **I/O**: 4 in / 4 out (stereo-capable and beyond).
- **Price**: $399 originally; as of mid-2026 the pedal is **sold out / unavailable for direct purchase**, with only email pre-order-notification and ~1 retailer in stock globally — a real cautionary tale about small-manufacturer supply fragility in this niche. [equipboard.com](https://equipboard.com/items/poly-beebo)
- **Sentiment**: "deep and rewarding, but a little buggy" (Engadget); ambitious touchscreen UX that reviewers found powerful but rough around the edges.

### Walrus Audio — Fable (Granular Soundscape Generator)
- **Concept**: five-program dual-DSP delay/granular hybrid — each program pairs a delay flavor (reverse, forward, analog-style, multi-tap, randomized pitch) with a matched granular treatment run through the signal twice via two DSP chips in series. [walrusaudio.com/fable](https://www.walrusaudio.com/products/fable-granular-soundscape-generator)
- **Controls/UI**: knob-selected program (1–5), regen/feedback knobs controlling the two DSP paths; reviewers note you need the manual to track which program does what.
- **I/O**: **mono in/out** — notable given Walrus is a well-resourced, well-distributed brand still shipping mono-only in this class.
- **Price**: ~$299.
- **Sentiment**: generally positive for creative sound-mangling ("choppy, melty, satisfying"); mono-only is a recurring point of disappointment in gear-forum discussion of Walrus's ambient-focused pedals generally.

### Walrus Audio — Lore (Reverse Soundscape Generator)
- **Concept**: sibling to Fable — five programs pairing reverse delay with reverse/pitched reverb, built the same dual-DSP-chip-in-series way. [guitarworld.com](https://www.guitarworld.com/reviews/walrus-audio-lore-reverse-soundscape-generator-review)
- **Controls/UI**: no presets at all — "a curious omission for a pedal of this complexity"; algorithm selected by knob position with no on-panel labeling, so "you will notice obstacles with the interface" without the manual open.
- **I/O**: mono only — reviewers explicitly say delay/reverb of this scale "really need the spatial element of stereo... to be fully realized."
- **Price**: ~$299.
- **Sentiment**: "a chameleon for your pedalboard," 4.5 stars from Guitar World, loved by ambient/shoegaze players — but the no-presets, mono-only, cryptic-algorithm-selector combination is the clearest three-part complaint pattern found anywhere in this survey.

### Electro-Harmonix — Grand Canyon / Superego(+)
- **Grand Canyon**: 12-effect delay/looper with stereo output, tap tempo, 13 presets — a general-purpose delay workhorse, not a dedicated granular engine, but included here because it's frequently shortlisted alongside granular pedals for ambient use. [ehx.com/grand-canyon](https://www.ehx.com/products/grand-canyon/)
- **Superego / Superego+**: infinite-sustain "synth engine" pedals; use granular-style synthesis techniques internally to sustain notes/chords indefinitely, but are marketed and used as sustain/pad tools rather than granular texture generators. Superego+ adds 11 effects, expression control, controllable glissando, and an effects loop.
- **Relevance to Brainscape**: EHX proves there's a mass-market, sub-$300, made-in-USA-adjacent price tier for "generate ambient texture from your guitar" pedals — but EHX has not entered the *granular-specifically-branded* segment that Hologram/Chase Bliss/Red Panda occupy.

### Meris — LVX / Mercury X
- **LVX**: a "modular delay system" with a granular engine used for stuttering, freezing, and evolving-texture effects — granular is one mode among a much larger delay/reverb/modulation toolkit, not the headline feature. [thegearforum.com](https://thegearforum.com/threads/lvx-granular-loops-and-textures.4542/page-2)
- **Mercury X**: reverb-flagship sibling reusing algorithms from the Mercury7 and the Chase Bliss CXM 1978, plus 3 new algorithms — again treats granular/texture as one of many reverb "modes" rather than a dedicated instrument.
- **Design philosophy**: Meris is explicitly praised for being "plug-and-playable while also deeply customizable without feeling stuck in submenus" — arguably the best UI/depth balance of any premium pedal in this survey, worth studying even though granular isn't its core identity.
- **Price**: ~$679 each (premium tier, well above the rest of this survey).
- **Relevance to Brainscape**: shows the ceiling of what "granular as a feature" looks like at the top of the market, and reinforces that a pedal built granular-first (rather than granular-as-an-add-on) is still a differentiated pitch even against premium competition.

## Prior Art: Open-Source & DIY Granular (the part pedal companies don't compete in)

### Mutable Instruments Clouds → Beads (Eurorack, fully open source)
Clouds was a granular audio processor (overlapping, delayed, transposed, enveloped grains from a live buffer) released with **fully open, MIT-licensed hardware and firmware**. [afterlateraudio.com](https://afterlateraudio.com/blogs/news/understanding-clouds-and-its-derivatives) That openness produced a large second-order ecosystem:
- **Parasites** — a free, open-source alternative firmware by Matthias Puech adding Oliverb (Erbe-Verb-style reverb), Resonestor (Karplus-Strong resonator), new grain envelope shapes, and stereo ping-pong delay, while leaving the stock firmware fully intact and selectable. [mqtthiqs.github.io/parasites](https://mqtthiqs.github.io/parasites/clouds.html)
- **Hardware clones**: After Later Audio Typhoon/Monsoon, uBurst (an 8HP miniaturized clone), and a documented DIY clone repo (TOILmodular/Clouds on GitHub). [github.com/TOILmodular/Clouds](https://github.com/TOILmodular/Clouds)
- **Beads** — Mutable's official spiritual-successor module to Clouds, explicitly marketed as a "next-generation evolution" of the format. [perfectcircuit.com](https://www.perfectcircuit.com/signal/mutable-instruments-beads)
- **Why this matters for Brainscape**: this is the single closest precedent for what Brainscape wants to be — a genuinely open, MIT/permissively-licensed granular platform that a community iterates on for a decade, spawning firmware forks and hardware variants, rather than a single company's closed product line. The eurorack world has already proven the model works; nobody has brought it, at this level of polish, into the guitar-pedal form factor.

### Daisy Seed community granular firmware (mono, scattered, hobbyist-grade)
- **NitroTron3** (GitHub, GPLv3) — Daisy Seed + Cleveland Music Co. Hothouse hardware kit; its "SPRAWL" mode is an 8-second SDRAM granular delay/texture engine with 8 Hann-windowed grain voices, pitch-shift/drift/scatter, and optional Bode frequency-shifter/Clouds-style reverb routing. It is **mono** (bass-voiced by default, with a guitar variant via a build flag) and shows effectively no GitHub stars/forks at time of writing — a real, working, technically sophisticated granular engine with zero community traction, largely because of discoverability and packaging, not ambition. [github.com/tronstoner/NitroTron3](https://github.com/tronstoner/NitroTron3)
- **DaisyCloudSeed / daisy-reverb** (GuitarML, MIT) — despite the "Cloud" name, this is a port of the open-source **Cloud Seed** algorithmic reverb (unrelated to Mutable's Clouds), targeting the Terrarium pedal, modified from stereo to mono. Modest traction (33 stars). Included here to flag a naming trap: "Cloud[s]" branding in the Daisy community does not reliably mean granular synthesis. [github.com/GuitarML/DaisyCloudSeed](https://github.com/GuitarML/DaisyCloudSeed)
- **Cleveland Music Co. Hothouse** and the broader **open-source-pedals** GitHub org provide the open hardware reference platform (schematics, BOM, Gerbers) most of this hobbyist firmware targets — the same kind of open-hardware-kit ecosystem Brainscape should either interoperate with or explicitly better. [github.com/clevelandmusicco/open-source-pedals](https://github.com/clevelandmusicco/open-source-pedals/tree/main/hothouse)
- **Electro-Smith's own libDaisy / DaisySP / DaisyExamples** are the base C++ libraries and hardware-support layer Brainscape's own DSP core will sit on top of; no official granular example ships in DaisyExamples today (unverified whether this has changed very recently — worth a direct repo check before launch). [github.com/electro-smith](https://github.com/electro-smith)

### norns (Glut / Mangl) and Organelle — software/synth-world granular culture
- **Glut** (by @artfwo) is a norns granular synth engine inspired by mlr/rove/Grainfields/Loomer Cumulus; **Mangl** (by @justmat) extends it into a 7-track granular sample player controllable via a Monome Arc. Both are open-source SuperCollider-based scripts in the norns community script library. [github.com/justmat/mangl](https://github.com/justmat/mangl)
- **Critter & Guitari Organelle** ships granular/spectral patches in its EXPLORE library and community Patchstorage repository — e.g. patches that turn a recorded sample into a randomized granular cloud played chromatically from the keyboard, with long (1–60s) amplitude envelopes for drones. [docs.critterandguitari.com](https://docs.critterandguitari.com/Organelle/patches/)
- **Why this matters for Brainscape**: there is a mature, technically sophisticated, *already-hackable* granular culture living in the Monome/synth-nerd world (norns, Organelle) that almost never crosses over into the guitar-pedal world. A well-documented, open, guitar-pedal-shaped granular platform is a plausible bridge that pulls that audience toward a new instrument category, the way Clouds pulled modular heads toward "guitar pedal thinking" in reverse.

### Desktop granular VST plugins (a separate, healthy market with no hardware counterpart)
Dedicated granular plugins are numerous and well-reviewed on their own terms — **Audio Damage Quanta 2** (deep, modulation-matrix-driven granular synth, VST3/AU/AAX/CLAP), **Output Portal** (XY-controller granular FX with 250+ presets, effect-only, not an instrument), **Dawesome Novum** (6-layer granular synthesis with cross-synthesis and MPE), and **Korg Padshop 2** — but none of them are built or marketed as the software twin of a hardware pedal. [kvraudio.com/best-granular-2026](https://www.kvraudio.com/the-best-granular-synthesis-plugins-in-2026-from-theory-to-practice) A KVR Audio forum thread of guitarists specifically searching for a plugin equivalent to the Chase Bliss Mood MK2 ends without finding one — participants list partial substitutes (Sound Betters Halo, Puremagnetik, Audio Damage Circa/Enso, Arturia FX Collection, NI Molekular) but explicitly conclude nothing replicates the "integrated combination of micro-looping, granular time-stretching, pitch, delay, and verb" in one plugin. [kvraudio.com](https://www.kvraudio.com/forum/viewtopic.php?t=625052&start=15) Separately, a Neural DSP community feature-request thread shows guitarists actively wishing for official Chase Bliss/Hologram plugin modeling that doesn't exist. [unity.neuraldsp.com](https://unity.neuraldsp.com/t/what-if-neural-dsp-signed-an-agreement-with-chase-bliss-or-hologram-electronics/11837)

## Gap Analysis

| Gap | Who half-solves it today | Who fully solves it | Brainscape opportunity |
|---|---|---|---|
| Deep granular control without menu-diving | Red Panda Particle 2 (performance layer + expert mode) | Nobody at scale | Adopt Particle 2's progressive-disclosure model as the baseline UI philosophy, not the ceiling |
| True stereo at a fair (<$350) price | Mood MKII ($399, borderline), Onward ($399, borderline) | Nobody under $350 | Ship real stereo I/O under $350 — the single clearest wedge in this whole survey |
| Open/hackable firmware, actively maintained | Mutable Clouds/Parasites (eurorack only), scattered Daisy hobby repos (mono, low-traction) | Nobody in guitar-pedal form factor | Be the first *polished, documented, community-onboarded* open pedal platform — treat NitroTron3/PedalPCB-style projects as a farm team to recruit contributors from, not competitors |
| Desktop plugin twin sharing DSP/presets with the hardware | Nobody — verified gap (KVR thread, Neural DSP feature request) | Nobody | Ship the VST from day one as a first-class product, not an afterthought port; let users design patches on a laptop screen and load them straight onto the pedal |
| Preset/patch-sharing community | ZOIA (forum/Discord patch culture) is the closest analog | Nobody has a purpose-built site/cloud sync | Build preset sharing into the plugin+pedal workflow from day one (export/import, a simple community repo) — ZOIA shows this is what keeps owners engaged for years |
| Reliable small-manufacturer supply | — | Nobody — even Chase Bliss (Habit discontinued) and Poly Effects (Beebo unobtainable) show boutique/limited-run granular pedals routinely go out of stock or get discontinued | Open hardware means the design never truly disappears — anyone can fab a board even if a "core team" stops selling finished units; make this an explicit selling point |
| Clear on-panel legibility (no "check the manual to know your mode") | Red Panda (labeled modes), Microcosm (labeled algorithms) | — | Walrus Lore/Fable and Mood MKII are the cautionary tales — invest in a screen or clearly labeled/LED-indicated mode state so users never need the PDF open mid-set |

**What nobody does well, synthesized**: the market segments cleanly into (a) expensive-and-deep-and-polished (Hologram, Chase Bliss, Meris, Empress) or (b) cheap-and-deep-and-rough (Count to Five, DIY Daisy firmware) or (c) modular-and-limitless-but-fragile-to-manufacture (ZOIA, Beebo). Nobody combines deep, labeled, progressive-disclosure granular control; genuine stereo I/O; a sub-$350 price; an open/hackable firmware and hardware platform; and a first-class desktop plugin twin with shared presets. That combination is Brainscape's addressable white space, not a single feature within it.

## Recommendations for Brainscape

1. **Lead with stereo, at a price nobody else hits.** Target a BOM/retail structure that lands meaningfully under Mood MKII/Onward ($399) while still being genuinely stereo — this is the single most defensible, easily-marketed claim in the whole category ("real stereo granular, open source, under $X").
2. **Copy Red Panda's two-layer UI philosophy, not Chase Bliss's or Walrus's.** Default-visible knobs should map to musically obvious parameters (size, density, pitch, feedback, mix) with sensible labeled ranges; bury advanced/expert parameters behind a clearly-marked secondary layer (long-press, alt-shift, or a screen) rather than Chase-Bliss-style "hidden, MIDI-only" parameters or Walrus-style unlabeled algorithm knobs. Never ship a mode selector without on-panel or on-screen labeling — Lore's "you need the manual to know your mode" complaint is a UI failure Brainscape should treat as a canonical anti-pattern to test against.
3. **Make the desktop VST a first-class citizen from v1, not a "someday" port.** This is a verified, unclaimed gap (KVR thread, Neural DSP feature request). Ship the plugin at or before hardware launch if possible, sharing the exact DSP core so patches sound identical and are interchangeable — this single decision is Brainscape's strongest point of differentiation versus every hardware-only competitor in this survey.
4. **Build preset/patch portability across hardware and plugin as a core workflow, not a bolt-on.** A simple file format (even just JSON) that loads on both the Daisy firmware and the VST, plus a lightweight community repo (a GitHub-based patch library is enough at launch) would leapfrog every existing pedal maker, none of whom have this, and would replicate what makes ZOIA's community so sticky.
5. **Explicitly position against Microcosm as "the open Microcosm"** — the "spiritual successor" framing already used internally is well-supported by the research: Microcosm is loved, expensive, and closed. A pedal that matches its algorithm breadth and stereo I/O while being open-source and cheaper is a legible, easy-to-explain pitch to the exact audience already primed to want it (they're already buying $459 pedals for this use case).
6. **Treat the Daisy/Hothouse/PedalPCB hobbyist community as a recruiting ground, not background noise.** Projects like NitroTron3 show real DSP sophistication with zero traction due to packaging/discoverability, not lack of skill — open-sourcing Brainscape well (good docs, a build-and-flash story as easy as Daisy Web Programmer, a welcoming contributor process modeled on Parasites/Mutable's community norms) could pull these builders in as contributors rather than leaving them fragmented across one-person repos.
7. **Learn from Beebo and Habit: open hardware is a genuine supply-resilience argument, market it as such.** Boutique/limited-run granular pedals go out of stock or get discontinued routinely (Habit discontinued outright; Beebo effectively unobtainable for over a year). Make "this design can never truly disappear — anyone can fab the board from the open Gerbers" an explicit, repeated marketing point, not just a licensing footnote.
8. **Don't neglect on-panel legibility even if there's a screen.** Meris (LVX/MercuryX) is the best-reviewed UI/depth balance in this entire survey ("deeply customizable without feeling stuck in submenus") despite being a premium closed product — study its panel layout and interaction model directly as a UX benchmark, independent of its granular depth (which is secondary/bolted-on for Meris, and should be primary for Brainscape).
9. **Consider a norns/Organelle-style "engine" mentality internally even for a guitar pedal**: Glut/Mangl show that a single well-designed granular engine can support many different "front panels"/scripts. Architecting Brainscape's DSP core the same way (one grain engine, multiple front-end personalities/presets/algorithms) will make both the hardware firmware and the VST easier to maintain long-term and easier for outside contributors to extend without touching the grain engine itself.
10. **Verify Infinite Jets' and Habit's exact I/O and current production status directly from primary sources (manual PDFs / current Chase Bliss and Hologram store pages) before quoting them publicly** — flagged as open questions below since this pass could not fully confirm those two details from primary sources.

## Sources

- Hologram Electronics — Microcosm official product page: https://www.hologramelectronics.com/products/microcosm
- Hologram Electronics — Infinite Jets official product page: https://www.hologramelectronics.com/pages/infinite-jets
- Hologram Infinite Jets user manual (ManualsLib): https://www.manualslib.com/manual/1883776/Hologram-Infinite-Jets.html
- Engadget — Hologram Microcosm review: https://www.engadget.com/hologram-electronics-microcosm-guitar-effect-pedal-review-ambient-music-170002707.html
- Guitar World — Hologram Microcosm review: https://www.guitarworld.com/reviews/hologram-electronics-microcosm-review
- Equipboard — Hologram Microcosm: https://equipboard.com/items/hologram-electronics-microcosm-5235c3bf-504e-4acc-b937-9ba9f1b67dac
- Gearspace — "Anyone using a Hologram Microcosm?": https://gearspace.com/board/so-many-guitars-so-little-time/1353956-anyone-using-hologram-microcosm.html
- The Gear Page — Hologram Electronics Microcosm thread: https://www.thegearpage.net/board/index.php?threads/hologram-electronics-microcosm.2121670/
- Pedal of the Day — Hologram Infinite Jets: https://www.pedal-of-the-day.com/2019/09/14/hologram-electronics-infinite-jets-resynthesizer/
- Chase Bliss — MOOD MKII official page: https://www.chasebliss.com/mood-mkii
- Guitar.com — Chase Bliss Mood MKII launch news: https://guitar.com/news/gear-news/chase-bliss-mood-mkii-launched/
- Guitar.com — "The Big Review: Chase Bliss Mood MkII": https://guitar.com/reviews/effects-pedal/the-big-review-chase-bliss-mood-mkii/
- Premier Guitar — Chase Bliss Habit review: https://www.premierguitar.com/gear/reviews/chase-bliss-habit
- Soundgas — Chase Bliss Habit demos and review: https://soundgas.com/blogs/blog/chase-bliss-habit-demos-and-review
- Chase Bliss — Habit official page: https://www.chasebliss.com/habit
- Chase Bliss — Onward official page: https://www.chasebliss.com/onward
- Guitar Pedal X — Chase Bliss Onward background: https://www.guitarpedalx.com/news/gpx-blog/chase-blisss-latest-onward-dynamic-sampling-device-is-a-really-smart-tom-majeski-pet-project--extrapolation-from-his-former-cooper-fx-outward-pedal
- Guitar.com — Chase Bliss Onward review: https://guitar.com/reviews/effects-pedal/hands-on-chase-bliss-onward-review/
- Red Panda Lab — Particle official product page: https://www.redpandalab.com/products/particle
- Premier Guitar — Red Panda Particle 2 review: https://www.premierguitar.com/gear/red-panda-particle-2-review
- Gearspace — "Red Panda Particle V2 - real-time granular synthesis pedal": https://gearspace.com/board/electronic-music-instruments-and-electronic-music-production/1260378-red-panda-particle-v2-real-time-granular-synthesis-pedal.html
- Red Panda Lab — Tensor official product page: https://www.redpandalab.com/products/tensor
- Guitar Girl Magazine — Red Panda Tensor review: https://guitargirlmag.com/reviews/gear-reviews/product-review-red-panda-tensor-pedal/
- Montreal Assembly — Count to 5 official product page: https://mtlasm.com/product/count-to-5/
- Delicious Audio — Montreal Assembly Count to 5 granular delay: https://delicious-audio.com/montreal-assembly-count-to-5-granular-delay/
- Effects Database — Montreal Assembly Count to 5: https://www.effectsdatabase.com/model/mtlasm/countto5
- Equipboard — Montreal Assembly Count to Five: https://equipboard.com/items/montreal-assembly-count-to-five
- Empress Effects — ZOIA official product page: https://empresseffects.com/products/zoia
- Sweetwater — Empress ZOIA product/reviews: https://www.sweetwater.com/store/detail/ZOIA--empress-zoia-modular-synthesizer-pedal/reviews
- Sweetwater — Empress ZOIA Euroburo: https://www.sweetwater.com/store/detail/ZOIAEuro--empress-zoia-euroburo-modular-eurorack-synthesizer
- Engadget — Empress ZOIA review: https://www.engadget.com/empress-effects-zoia-modular-effects-pedal-review-guitar-synth-120023473.html
- Engadget — Poly Effects Beebo review: https://www.engadget.com/poly-effects-beebo-review-modular-guitar-pedal-touchscreen-151557875-151557578.html
- Guitar.com — Poly Effects Beebo news: https://guitar.com/news/gear-news/poly-effects-beebo/
- Equipboard — Poly Effects Beebo (availability): https://equipboard.com/items/poly-beebo
- Poly Effects — official site: https://www.polyeffects.com/
- Walrus Audio — Fable official product page: https://www.walrusaudio.com/products/fable-granular-soundscape-generator
- Guitar Pedal X — Walrus Audio Fable release news: https://www.guitarpedalx.com/news/gpx-blog/walrus-audio-releases-the-fable-granular-soundscape-generator-delay-and-reverb-in-the-same-format-as-its-recent-lore
- Walrus Audio — Lore review (Guitar World): https://www.guitarworld.com/reviews/walrus-audio-lore-reverse-soundscape-generator-review
- Guitar.com — "The Big Review: Walrus Audio Lore": https://guitar.com/reviews/effects-pedal/the-big-review-walrus-audio-lore/
- Electro-Harmonix — Grand Canyon official page: https://www.ehx.com/products/grand-canyon/
- Electro-Harmonix — Superego+ official page: https://www.ehx.com/products/superego-plus/
- The Gear Forum — "LVX granular loops and textures!": https://thegearforum.com/threads/lvx-granular-loops-and-textures.4542/page-2
- Premier Guitar — Meris LVX review: https://www.premierguitar.com/gear/reviews/meris-lvx
- FutureMusic — Meris Mercury X review: https://futuremusic.com/2023/12/meris-mercury-x-review/
- Guitar Pedal X — Meris Mercury X news: https://www.guitarpedalx.com/news/meris-repeats-its-lvx-sound-designer-project-for-its-new-mercury-x-modular-reverberator---harnessing-algorithms-from-the-mercury-7-and-chase-bliss-cxm-1978-along-with-3-brand-new-ones
- Synthtopia — Mutable Instruments Clouds Parasite firmware: https://www.synthtopia.com/content/2015/06/22/parasite-brings-new-features-to-mutable-instruments-clouds-eurorack-module/
- Parasites (Clouds alternative firmware) official docs: https://mqtthiqs.github.io/parasites/clouds.html
- After Later Audio — "Understanding Clouds and Its Derivatives": https://afterlateraudio.com/blogs/news/understanding-clouds-and-its-derivatives
- GitHub — TOILmodular/Clouds (DIY Clouds clone): https://github.com/TOILmodular/Clouds
- Perfect Circuit — Mutable Instruments Beads: https://www.perfectcircuit.com/signal/mutable-instruments-beads
- GitHub — tronstoner/NitroTron3 (Daisy Seed + Hothouse, SPRAWL granular mode, GPLv3): https://github.com/tronstoner/NitroTron3
- GitHub — GuitarML/DaisyCloudSeed (Cloud Seed reverb port, MIT): https://github.com/GuitarML/DaisyCloudSeed
- GitHub — clevelandmusicco/open-source-pedals (Hothouse open hardware): https://github.com/clevelandmusicco/open-source-pedals/tree/main/hothouse
- GitHub — electro-smith (libDaisy, DaisySP, DaisyExamples org): https://github.com/electro-smith
- PedalPCB Community Forum — "Vibe coding Daisy Seed": https://forum.pedalpcb.com/threads/vibe-coding-daisy-seed.28774/
- PedalPCB Community Forum — "Grain delay": https://forum.pedalpcb.com/threads/grain-delay.25095/
- GitHub — justmat/mangl (norns granular sample player): https://github.com/justmat/mangl
- lines (llllllll.co) — Mangl thread: https://llllllll.co/t/mangl/21066
- lines (llllllll.co) — Glut thread: https://llllllll.co/t/glut/21175
- Critter & Guitari — Organelle patches documentation: https://docs.critterandguitari.com/Organelle/patches/
- KVR Audio — "Best Granular Synthesis Plugins in 2026": https://www.kvraudio.com/the-best-granular-synthesis-plugins-in-2026-from-theory-to-practice
- KVR Audio forum — "Chase Bliss Mood MK2 (Any alternatives in software?)": https://www.kvraudio.com/forum/viewtopic.php?t=625052&start=15
- Neural DSP community — Chase Bliss/Hologram plugin feature request: https://unity.neuraldsp.com/t/what-if-neural-dsp-signed-an-agreement-with-chase-bliss-or-hologram-electronics/11837

## Open Questions

- **Infinite Jets physical I/O**: could not confirm from primary sources whether the audio jacks are mono or stereo (manual excerpt only surfaced "Mono/Poly trigger mode," which governs note-triggering behavior, not the physical jack configuration) — read the full manual PDF or contact Hologram directly before citing this externally.
- **Chase Bliss Habit physical I/O**: mono/stereo spec not confirmed from primary sources in this pass.
- **Current retail status of Habit**: confirmed discontinued via Guitar Pedal X's news coverage of Chase Bliss's own retirement announcement (retired early 2025, ~3 years after its March 2022 launch); the live chasebliss.com/habit page itself was not successfully fetched to see its current banner/copy, so treat the *exact current wording* on that page as unconfirmed even though the discontinuation fact itself is solid.
- **DaisyExamples granular example**: checked directly — as of this research pass, Electro-Smith's official `electro-smith/DaisyExamples` repo directory listing (organized by hardware platform: seed, pod, petal, patch, patch_sm, field, legio, versio, cube) contains **no example with "granul", "grain", or "clouds" in its name**. This confirms there is no official reference granular implementation to benchmark against today — Brainscape's DSP core would be filling a real void in the official example set, not reinventing one.
- **Exact current Chase Bliss Onward and Habit stereo implementation details** (true independent L/R granular processing vs. dual-mono) were not verified at the DSP-signal-flow level for any Chase Bliss product — worth deeper technical verification if Brainscape wants to make specific comparative claims.
- **Precise unit-sales or install-base figures** for any product in this survey are not publicly available; all "popularity"/sentiment claims here are qualitative (review tone, forum sentiment, star ratings) rather than quantitative.
- **Norns/Organelle granular scripts' actual crossover into the guitar-pedal buying audience** is asserted as a hypothesis in the Gap Analysis, not measured — worth validating with actual community surveys (e.g., a poll in r/ambientmusic, llllllll.co, or the Daisy forum) before leaning on it heavily in marketing.
