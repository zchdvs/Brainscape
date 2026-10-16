#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "Document.h"

// Lint and derive (docs/design/mode-compiler.md §2.7, §3.5, §8.2): passes over a read document
// that are not part of Compile. Where they need floating point they call guarded dsp/ functions
// (EvalMacro, NearGuardMs, FormatPlain), which give the same bits on every host; the compiler's
// own code stays integer-only.
namespace bsc {

struct LintOptions {
  // `lint --factory` (CI over firmware/factory/): L4, L7-L9 and L5's empty source set are
  // errors.
  bool factory = false;
};

// L1-L9 and L12-L14 over a document that reads without errors (L10 and L11 are the CPU plan's,
// cpu-budget.md §7.4).
//   L1 a subnormal was written (it compiles to +0);
//   L2 the smallest base_ms a layer's leaf and macros reach is below size_ms * (r - 1) for
//      a pitch entry, so the near guard moves those grains back (engine §3); for a layer with a
//      base_sync, the synced base delay at its shortest over the tempo range and the Subdivs
//      (300 BPM, ×8; docs/design/clock.md §11.1);
//   L3 activity, repeats or time has no targets;
//   L4 a targeted leaf (not under editor.detached) shows another value than its macro gives at
//      the stored position, which is what "further than its display resolution" means here:
//      the display text (FormatPlain) differs; or a macro position is omitted (compiled 0.5);
//   L5 no free-running source (periodic, clock) and no onset: silent until triggered; or no
//      source at all: the mode never plays a grain;
//   L6 mark positioning with decay_ms 0 holds the last note;
//   L7 a macro targets a Shift-secondary leaf (post.mod.depth, post.mod.rate_hz,
//      post.reverb.time, post.reverb.mode, post.filter.res);
//   L8 the Filter macro does not run the cutoff from its minimum (the wet kill) to its maximum
//      (bypass), or the Space macro adds wet at 0;
//   L9 a product string (id, name, author, description, tags, display names) holds another
//      maker's mark from the denylist (design §11.2, record §2.2; a floor, not a search);
//   L12 a layer with a base_sync whose feedback.amount is above 0 at the stored value or any
//      end a macro or the expression pedal reaches: grain-feedback repeats fall 10.67 ms later
//      per pass than the grid, tempo-exact repeats belong on the post delay (clock.md §6.2, D6);
//   L13 a macro target or expression assignment on a leaf a sync overrides at every position:
//      post.delay.time_ms while post.delay.sync is nonzero at every value the controls reach, a
//      layer's base_ms while its base_sync is not off (clock.md §6.5);
//   L14 a synced field whose reachable codes fold at the stored tempo and Subdiv, where a sweep
//      of the code is not monotone (clock.md §5.2, §5.3).
// All warnings; --factory makes none of L12-L14 an error.
std::vector<Finding> Lint(const Document& doc, const LintOptions& options = {});

// The denylist L9 matches, whole words, case-insensitively.
const std::vector<std::string>& Denylist();

// `derive` (§3.5): each targeted leaf not under editor.detached becomes EvalMacro at its
// macro's stored position, macros in ascending id (of two macros on one leaf the later wins, as
// a later move does). With `solve`, each macro's position is first solved from the leaf of its
// first non-detached target whose range is not a single value (SolvePosition). `log` receives
// one line per changed position and per leaf whose value changed in the end (a leaf two macros
// target is logged once, with the macro that wrote it last), naming leaves as schema 1 does
// (`layer0.position.spray_ms`, the form editor.detached and macro targets take).
void Derive(Document* doc, bool solve, std::vector<std::string>* log);

// "Solve position" (§3.5): the canonical position (bits) whose value for target `index` of
// `macroId` lands nearest `leafBits`, by bisection (EvalMacro is monotonic) and exact distance
// comparisons. Of the positions that land equally near, `current` (the stored position, when it
// is a canonical position in [0, 1]) wins, so a leaf derived from its position solves back to
// that position; otherwise the smallest, also past either end of the range.
inline constexpr uint32_t kNoPosition = 0xFFFFFFFFu;
uint32_t SolvePosition(const brainscape::ModeBlob& mode, uint32_t macroId, uint32_t index,
                       uint32_t leafBits, uint32_t current = kNoPosition);

}  // namespace bsc
