#pragma once
#include <cstdint>

#include "brainscape/Engine.h"

// The shadow ledger (docs/design/cpu-budget.md §4.3 step 8, §5): the cost governor's accounting
// without its admission rules. From the engine's counts over a block (Engine::GrainStats, the
// difference of two snapshots) it predicts the cycles the governor would charge for the block:
//
//   reserve * frames / 48                       S: fixed work, the onset hop in every window,
//                                               events, calls (§5.3)
//   + sum over weight classes of frames * w     each voice-frame rendered, its path and rate
//                                               class's weight (§5.1)
//   + births * c_b + passes * c_pass            per birth, per repeat pass begun (§7.2)
//   + fade frames * c_fade                      a FastCut fade's extra product per frame
//   + sum over post stages of frames * K_s      each stage's frames (§5.1's P(f))
//
// The bench images log it next to each block's DWT cycles: bench session 2's G3 asks that DWT
// never exceed it and that it stay within 1.3 x DWT at the median (§8.3). Nothing here changes an
// output bit. The constants are §5.3's provisional values: P2's conservative weights for the
// speed pack's code (worst envelope leg, +15 %, every ring line missing), column A's c_b, session
// 1's post-stage costs and §5.3's S. Session 2 measures each; the final values move into the
// shared cost model with the governor (§4, steps 11 and 12). Integer arithmetic, so the host and
// the Seed predict the same.
namespace brainscape::fw::bench {

struct LedgerConstants {
  // Cycles per voice-frame by Engine::kCostClasses: unity; Hermite and linear by rate class.
  uint32_t weight[Engine::kCostClasses] = {54, 119, 128, 145, 70, 79, 96};
  uint32_t birth   = 4500;  // c_b
  uint32_t pass    = 300;   // c_pass: a pass's DecayGain and its run's split (provisional)
  uint32_t fade    = 4;     // per frame under a FastCut fade (provisional)
  // K_s per frame, by Engine::kPostStages: modulation, delay, reverb, filter
  // (session-1/bench-all.md:276-280).
  uint32_t post[Engine::kPostStages] = {192, 319, 559, 200};
  uint32_t reserve = 148000;  // S per 48 frames
};

inline constexpr LedgerConstants kProvisionalLedger{};

// The counts of a span: b - a, field by field.
inline Engine::GrainStats StatsDelta(const Engine::GrainStats& a, const Engine::GrainStats& b) {
  Engine::GrainStats d;
  d.births       = b.births - a.births;
  d.burstBirths  = b.burstBirths - a.burstBirths;
  d.skips        = b.skips - a.skips;
  d.repeatPasses = b.repeatPasses - a.repeatPasses;
  d.steals       = b.steals - a.steals;
  d.spareBirths  = b.spareBirths - a.spareBirths;
  d.birthFlushes = b.birthFlushes - a.birthFlushes;
  d.renderRuns   = b.renderRuns - a.renderRuns;
  for (uint32_t c = 0; c < Engine::kCostClasses; ++c) d.voiceFrames[c] = b.voiceFrames[c] - a.voiceFrames[c];
  d.fadeFrames = b.fadeFrames - a.fadeFrames;
  for (uint32_t s = 0; s < Engine::kPostStages; ++s) d.postFrames[s] = b.postFrames[s] - a.postFrames[s];
  return d;
}

// The cycles the ledger charges for a span of `frames` frames whose counts are `d`.
inline uint64_t LedgerCycles(const Engine::GrainStats& d, uint32_t frames,
                             const LedgerConstants& k = kProvisionalLedger) {
  uint64_t c = static_cast<uint64_t>(k.reserve) * frames / 48u;
  for (uint32_t i = 0; i < Engine::kCostClasses; ++i) c += d.voiceFrames[i] * k.weight[i];
  c += d.births * k.birth + d.repeatPasses * k.pass + d.fadeFrames * k.fade;
  for (uint32_t s = 0; s < Engine::kPostStages; ++s) c += d.postFrames[s] * k.post[s];
  return c;
}

}  // namespace brainscape::fw::bench
