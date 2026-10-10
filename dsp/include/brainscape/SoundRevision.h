#pragma once
#include <cstdint>

#include "brainscape/FpProfile.h"

namespace brainscape {

// The sound revision (docs/design/determinism-profile.md §5.12): certifies the dsp/
// sources, shared constants, canonical EngineConfig and the determinism profile's
// rules and flags, for every input. Any change that CAN change engine output bumps
// it; golden coverage is not the definition. It keys the golden-hash file (§6.1).
//
// A bump is exactly one and regenerates dsp/tests/golden/golden.json with the harness's
// --mode mint in the same pull request; sound-rev.yml fails a sound-relevant change
// without one (tools/ci/sound_rev_gate.py). Revisions before the first published one
// are internal (§1.5). 1: the first minted revision (profile §8.4 step 10). 2: the mode
// runtime (docs/design/mode-compiler.md §7.6 item 4): domain routing, rows 27 and 28 retired
// into mode structure, the wet-only trim, effect volume and cutoff kill, LoadPreset with the
// mode and CTRL, macro and expression events, Trails and FastCut mode switches. 3: the Mix law
// (mode-compiler.md §7.1 R3b, §7.6 item 5; owner question Q13, provisional): dry at unity up
// to the middle and wet at unity from it, min(1, 2(1 − m)) and min(1, 2m), instead of the
// linear crossfade; Mix 0 and 1 keep revision 2's bits. 4: wave 1's trigger sources, bursts and
// intermittency (mode-compiler.md §7.5 R9): the free-running scheduler only with `periodic`,
// footswitch and MIDI triggers only when the mode lists them, scheduler.burst and
// scheduler.intermittency as leaves 57-59; the default mode keeps revision 3's bits. 5: wave 1's
// pitch sets (mode-compiler.md §7.5 R10): layer 0 plays its set's entries, by `cycle` or
// `random`, plus the transpose leaf; the default set {0: 1} keeps revision 4's bits. 6: wave 1's
// micro-loops (mode-compiler.md §7.5 R11): layer0.position.repeat passes over one region, each
// windowed, with the far rail over the whole life, and layer0.decay_ms's 60 dB fall as the
// position reference ages, as leaves 29 and 30; repeat 1 and decay 0 keep revision 5's bits.
// 7: wave 1's voice count (mode-compiler.md §7.5 R12): at most layer0.voice_count (leaf 31)
// voices sound, free-running births beyond it refused and triggers stealing the oldest; 64
// keeps revision 6's bits. 8: the tempo core (docs/design/clock.md §11.3): events 6-10 (tap,
// tempo, MIDI clock, transport, subdivision) on an integer phasor with a tap chain and a clock
// follower; the stored performance state played (its tempo, time mode and subdivision, re-coded
// so 0 is TAP), Restart starting from it; CLOCK births on the grid, triggers with ordinal 3;
// rows 83-85. No mode before it lists `clock`, so every earlier preset keeps revision 7's bits.
// 9: synced times (docs/design/clock.md §11.3): post.delay.sync (row 63) a leaf playing §5.2's
// note values at the committed tempo and effective Subdiv, folded by octaves into 10 ms to 4 s
// and 2^-7 of it, on a line that long, and a layer's base_sync (folded into 1 ms to 5 s and 2^-7
// of it); a jump of the committed tempo or a discrete change crossfades the post delay's two heads
// over 1,024 frames, a clock's deadband commit slews it (τ = 1 s, at most 2^-10 frames a frame),
// any other change glides; global.tempo_glide (row 86) glides jumps. Row 63 at 0 and base_sync off
// play revision 8's bits.
inline constexpr uint32_t kSoundRevision = 9;

// The toolchain that compiled this dsp/ library (profile §5.12): compiler, version,
// target and the floating-point flags. For triage only, carried in the parity reply and
// in golden reports: identity is the sound revision, and every conforming toolchain
// produces the same output.
struct ToolchainId {
  const char* compiler;     // "msvc", "gcc", "clang" or "appleclang"
  const char* version;      // the compiler's own version string
  const char* target;       // architecture and system, e.g. "x86_64-linux", "armv7e-m-none"
  const char* fpFlags;      // the profile's flags and the FP and target flags of the
                            // build configuration, as CMake passed them
  const char* fpFlagsHash;  // SHA-256 of fpFlags, first 16 hex digits
};
const ToolchainId& BuildToolchain() noexcept;

}  // namespace brainscape
