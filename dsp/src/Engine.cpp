#include "detail/FpProfilePrivate.h"

#include "brainscape/Engine.h"

#include <atomic>
#include <cassert>
#include <cstring>
#include <new>
#include <type_traits>

#include "blob/Blob.h"
#include "brainscape/Preset.h"
#include "brainscape/SoundRevision.h"
#include "detail/Canonical.h"
#include "detail/DetMath.h"
#include "detail/FpEnvGuard.h"
#include "detail/GrainMath.h"
#include "detail/Granular.h"
#include "detail/MixLaw.h"
#include "detail/ModeEvalBody.h"
#include "detail/OnsetDetector.h"
#include "detail/PostChain.h"
#include "detail/Smoother.h"

namespace brainscape {

namespace {

constexpr bool TableIsContiguous() {
  for (size_t i = 0; i < kNumParams; ++i) {
    if (static_cast<uint32_t>(kParamTable[i].id) != i + 1) return false;
  }
  return true;
}
static_assert(TableIsContiguous(), "kParamTable must be ordered by contiguous ids from 1");

// ── The parameter table's per-kind invariants (docs/design/mode-compiler.md §4.1) ────────

constexpr bool SameName(const char* a, const char* b) {
  for (; *a != '\0' && *a == *b; ++a, ++b) {
  }
  return *a == *b;
}

constexpr bool RowIsWellFormed(const ParamDescriptor& d) {
  if (d.kind == ParamKind::Retired) return d.name == nullptr;  // a tombstone keeps only its id
  if (d.name == nullptr || d.name[0] == '\0' || d.unit == nullptr) return false;
  if (!(d.min < d.max) || !(d.def >= d.min) || !(d.def <= d.max)) return false;
  if ((d.domain & ~kAllParamDomains) != 0u) return false;
  // A Leaf row names the revision that made it one, and this build plays it; no other kind
  // has a revision. Rows that hold a value (Leaf, Global) rebuild something; Macro and
  // Performance rows act through their own events.
  switch (d.kind) {
    case ParamKind::Leaf:
      return d.sinceRev >= 1u && d.sinceRev <= kSoundRevision && d.domain != kDomainNone;
    case ParamKind::Global: return d.sinceRev == 0u && d.domain != kDomainNone;
    case ParamKind::Macro:
    case ParamKind::Performance: return d.sinceRev == 0u && d.domain == kDomainNone;
    case ParamKind::Reserved: return d.sinceRev == 0u;
    case ParamKind::Retired: break;
  }
  return false;
}

constexpr bool TableIsWellFormed() {
  for (size_t i = 0; i < kNumParams; ++i) {
    if (!RowIsWellFormed(kParamTable[i])) return false;
    for (size_t j = 0; j < i; ++j) {
      if (kParamTable[i].name != nullptr && kParamTable[j].name != nullptr &&
          SameName(kParamTable[i].name, kParamTable[j].name)) {
        return false;  // names are the hosts' and the schema's keys
      }
    }
  }
  return true;
}
static_assert(TableIsWellFormed(),
              "kParamTable: a row breaks its kind's rules (range, default, domain, sinceRev) or "
              "repeats a name");

// The rows the engine stores (pending_ and active_): Leaf and Global (design §4.1). Every
// other kind is a no-op for SetParam and LoadPreset.
constexpr bool IsStored(ParamKind k) { return k == ParamKind::Leaf || k == ParamKind::Global; }

constexpr size_t CountStored() {
  size_t n = 0;
  for (const ParamDescriptor& d : kParamTable) n += IsStored(d.kind) ? 1u : 0u;
  return n;
}
constexpr size_t kNumStored = CountStored();

struct StoredRows {
  uint16_t row[kNumStored];      // slot -> row index (id - 1)
  uint16_t slot[kNumParams];     // row index -> slot, or kNumStored for rows not stored
};
constexpr StoredRows MakeStoredRows() {
  StoredRows s{};
  uint16_t   k = 0;
  for (size_t i = 0; i < kNumParams; ++i) {
    if (IsStored(kParamTable[i].kind)) {
      s.row[k]  = static_cast<uint16_t>(i);
      s.slot[i] = k++;
    } else {
      s.slot[i] = static_cast<uint16_t>(kNumStored);
    }
  }
  return s;
}
constexpr StoredRows kStored = MakeStoredRows();

// The slot of a stored row's id, or kNumStored.
constexpr size_t SlotOf(ParamId id) {
  const auto raw = static_cast<uint32_t>(id);
  return raw >= 1u && raw <= kNumParams ? kStored.slot[raw - 1u] : kNumStored;
}
constexpr const ParamDescriptor& RowOfSlot(size_t slot) { return kParamTable[kStored.row[slot]]; }

// Leaf ordinals are slots too (Leaf rows are stored), so a complete preset's values map to
// slots through the leaf's id.
static_assert(kNumLeafParams <= kNumStored, "every Leaf row is stored");

// A change rebuilds what its row's domain bits name (design §7.2, R1), so a stored row needs a
// domain (TableIsWellFormed) and every domain bit a rebuild (RebuildDirty's switch).
static_assert(kAllParamDomains == 0x3Fu, "a new parameter domain needs its rebuild in RebuildDirty");

// The cutoff's minimum is the wet kill (design §7.2): the Filter macro's universal endpoint.
constexpr float kCutoffKillHz =
    kParamTable[static_cast<size_t>(ParamId::FilterCutoffHz) - 1u].min;

// The active mode and its CTRL (design §7.3): one active mode, copied in at a load's frame from
// the staged PresetState, in the Warm arena (PlanMemory) rather than the DTCM-bound Impl. The
// macro positions it carries are pickup references the engine never reads; the expression
// assignments drive Expression events.
struct ActiveMode {
  ModeBlob     mode;
  ControlState control;
};
static_assert(std::is_trivially_destructible<ActiveMode>::value, "the Warm arena never destructs");
constexpr size_t kActiveModeBytes = (sizeof(ActiveMode) + 15u) & ~size_t{15};

// Two modes are the same when their content is (design §7.3): compared word by word up to
// modeHash, which the engine never trusts. ModeBlob has no implicit padding (Mode.h), so every
// byte is a field, and equal modes are equal word for word.
static_assert(offsetof(ModeBlob, modeHash) % 4u == 0u, "ModeBlob compares by words");
bool SameModeContent(const ModeBlob& a, const ModeBlob& b) noexcept {
  const auto* x = reinterpret_cast<const unsigned char*>(&a);
  const auto* y = reinterpret_cast<const unsigned char*>(&b);
  uint32_t    d = 0;
  for (size_t i = 0; i < offsetof(ModeBlob, modeHash); i += 4u) {
    uint32_t u, v;
    std::memcpy(&u, x + i, sizeof u);
    std::memcpy(&v, y + i, sizeof v);
    d |= u ^ v;
  }
  return d == 0u;
}

// The smoothed wet gain's target (design §7.2): exactly 0 at the cutoff minimum, which kills the
// wet signal after the post chain, else the mode's level match (wet_trim_db) and the player's
// effect volume (a device setting) as one gain. The one place the three meet, so a lone cutoff
// change engages the kill at its frame and a lone trim change while killed stays muted.
inline float WetGainTarget(float trimDb, float effectVolumeDb, float cutoffHz) noexcept {
  if (!(cutoffHz > kCutoffKillHz)) return 0.0f;
  const float db = trimDb + effectVolumeDb;
  // exp2, not pow: one kernel instead of two (schedule-time transcendentals are charged in
  // design §8). dB -> linear.
  return detmath::Exp2F(db * 0.16609640474436813f);
}

// SetParam/GetParam are documented lock-free from any thread (design §9 threading
// table); make the assumption a compile error on the day it stops being true.
static_assert(std::atomic<float>::is_always_lock_free,
              "SetParam/GetParam must be lock-free on this target");

// The onset event list must cover every hop boundary in the largest legal block
// (maxBlockSize <= kFeedbackDelayFrames, enforced in Init) — if the FIFO constant
// is ever raised, this is the assert that keeps onsets from being dropped.
static_assert(kFeedbackDelayFrames / detail::kOnsetHop + 2u <=
                  detail::TriggerEvents::kMaxOnsets,
              "TriggerEvents::kMaxOnsets must cover the largest legal block");

static_assert(kMaxGrains == detail::kGranularMaxGrains, "public and core voice counts differ");
static_assert(kMaxPitchEntries == detail::kGranularMaxPitch, "a pitch set's cap differs");
static_assert(kFastCutFrames == detail::kGranularFastCutFrames, "public and core fades differ");

// Grain write-head guards keep reads out of the frames Pass 1 writes ahead of the
// live head within one block; that window must cover the largest legal block.
static_assert(kFeedbackDelayFrames <= detail::kBlockWriteAheadFrames,
              "the far-rail write-ahead window must cover maxBlockSize");

constexpr float kInvScale = 1.0f / 32767.0f;

constexpr float kMaxFinite = 0x1.fffffep127f;  // FLT_MAX

// Bound on the onset detector's input. Its FFT and hop energy square the input, so a
// finite sample past about 2e19 overflowed them and latched +inf into the whitening
// memory, turning onset triggering off until Reset (review finding). 2^16 (+96 dBFS)
// never touches real audio and keeps every square finite.
constexpr float kDetectorBound = 0x1p16f;

// Init and PlanMemory accept these rates. Below ~40 Hz the ms-sized post buffers round
// to zero length and a DelaySlice write walks off its arena (review finding); above
// 384 kHz is outside anything the design supports. The bounds also keep every length
// derived from the rate in range for its integer conversion (determinism profile §3.10).
constexpr double kMinSampleRate = 8000.0;
constexpr double kMaxSampleRate = 384000.0;

inline bool SampleRateSupported(double sr) noexcept {
  return sr >= kMinSampleRate && sr <= kMaxSampleRate;  // false for NaN
}

// Rounding-mode-independent, NaN-safe int16 quantizer. Not lrintf: lrintf follows
// the dynamic FP rounding mode and is a libm call in the hot loop on Cortex-M7
// (review finding, verified with arm-none-eabi-gcc). The guard now pins round to
// nearest, but the quantizer stays independent of the mode regardless (determinism
// profile §5.3). Round-half-away-from-zero via a plain truncating convert.
// Clamp is symmetric at ±32767 so the ring's float range stays exactly [-1, 1],
// and the negated comparisons map NaN to a defined endpoint on every platform, which
// also keeps the conversion in range (§3.10).
inline int16_t QuantizeS16(float x) noexcept {
  float s = x * 32767.0f;
  if (!(s < 32767.0f)) s = 32767.0f;
  if (!(s > -32767.0f)) s = -32767.0f;
  return static_cast<int16_t>(s >= 0.0f ? s + 0.5f : s - 0.5f);
}

inline bool IsPowerOfTwo(uint32_t v) noexcept { return v != 0 && (v & (v - 1)) == 0; }

// ±1 LSB TPDF dither in the float domain (design §12.3). Applied to the ring write
// so int16 rounding has no fixed points in the feedback loop — without it a single
// impulse leaves a permanent tone (measured −70 dBFS at fb 0.95; review finding).
// Keys use the shared grainmath purpose stride so dither draws never collide with
// grain-birth draws, and fold the full 64-bit counter as RandUnit does (determinism
// profile §5.6): the truncated key repeated the dither every 2^29 samples.
inline float Tpdf(int64_t absSample, grainmath::Draw purpose) noexcept {
  const uint32_t key = grainmath::DrawKey(absSample, purpose);
  const float u1 =
      static_cast<float>(grainmath::Hash32(key) >> 8) * (1.0f / 16777216.0f);
  const float u2 =
      static_cast<float>(grainmath::Hash32(key ^ 0x6A09E667u) >> 8) * (1.0f / 16777216.0f);
  return (u1 + u2 - 1.0f) * kInvScale;
}

using detail::CanonicalValue;  // detail/Canonical.h: the one canonicalization rule

// Bodies of the free-function entry points; the public functions below only add the
// guard (detail/FpEnvGuard.h explains the split).
BRAINSCAPE_FP_BODY MemoryPlan PlanMemoryBody(const EngineConfig& cfg) noexcept {
  MemoryPlan plan{};
  if (!SampleRateSupported(cfg.sampleRate)) return plan;  // Init refuses the config too
  // Hot (DTCM-class): window LUT + wet accumulators. The grain pool itself lives
  // inside the Engine object — firmware places the Engine instance in DTCM.
  plan.bytes[static_cast<size_t>(Tier::Hot)] =
      (static_cast<size_t>(detail::kWindowLutSize) + 2u * cfg.maxBlockSize) * sizeof(float);
  plan.align[static_cast<size_t>(Tier::Hot)] = 16;
  // Warm (AXI-class): feedback FIFO + taming diffuser + mod lines + reverb tank
  // + onset-detector analysis/FFT/whitening state.
  // Then the active mode (design §7.3).
  plan.bytes[static_cast<size_t>(Tier::Warm)] =
      (static_cast<size_t>(kFeedbackDelayFrames) * 2u +
       detail::FeedbackTamer::WarmFloats(cfg.sampleRate) +
       detail::PostChain::WarmFloats(cfg.sampleRate) + detail::OnsetDetector::WarmFloats()) *
          sizeof(float) +
      kActiveModeBytes;
  plan.align[static_cast<size_t>(Tier::Warm)] = 16;
  // Bulk (SDRAM-class): history ring + post-delay buffer; looper A+B when the
  // looper lands. Frame counts are bounded by Init (<= 2^26), so these products
  // cannot overflow a 32-bit size_t on the embedded target.
  plan.bytes[static_cast<size_t>(Tier::Bulk)] =
      static_cast<size_t>(cfg.historyFrames) * 2u * sizeof(int16_t) +
      static_cast<size_t>(cfg.looperFrames) * 2u * sizeof(int16_t) * 2u +
      static_cast<size_t>(detail::PostChain::BulkFloats(cfg.sampleRate)) * sizeof(float);
  // Cache-line aligned: the SD/DMA coherency rule needs 32-byte-aligned ranges (design §7).
  plan.align[static_cast<size_t>(Tier::Bulk)] = 32;
  return plan;
}

BRAINSCAPE_FP_BODY float CanonicalizeBody(ParamId id, float plainValue) noexcept {
  const ParamDescriptor* d = FindParam(id);
  return d != nullptr ? CanonicalValue(*d, plainValue) : 0.0f;
}

// A stored leaf this build plays (design §7.3 step 2): its row is a Leaf that became one at
// or before this build's revision. Any other id in a preset is unknown and ignored: an id
// this build lacks, and a Macro, Performance, Global, Reserved or Retired row, none of which
// a preset stores (§4.1).
const ParamDescriptor* PlayableLeaf(uint32_t id) noexcept {
  const ParamDescriptor* d = FindParam(static_cast<ParamId>(id));
  return d != nullptr && d->kind == ParamKind::Leaf && d->sinceRev <= kSoundRevision ? d
                                                                                      : nullptr;
}

// Stored performance fields this build cannot play (design §7.3 step 4): every field away from
// its default, until W2 plays them.
uint32_t UnsupportedPerformance(const PerformanceState& p) noexcept {
  const PerformanceState def{};
  return static_cast<uint32_t>(p.reverse != def.reverse) +
         static_cast<uint32_t>(p.timeMode != def.timeMode) +
         static_cast<uint32_t>(p.subdiv != def.subdiv) +
         static_cast<uint32_t>(p.tempoSource != def.tempoSource) +
         static_cast<uint32_t>(p.usPerQuarter != def.usPerQuarter);
}

// Design §7.3 steps 0, 1 and 2, and step 4's count (determinism profile §5.10 with the per-kind
// rules of §4.1): the mode and CTRL must pass ValidateMode's structural and semantic rules, else
// invalidMode and nothing more; then every Leaf row's default, then every stored leaf,
// canonicalized, by ascending id (values[] is indexed by leaf ordinal, ascending by id, and is
// applied in that order). Global rows are not part of a preset and keep their values. A Leaf
// row the preset lacks is missing only if it existed at the preset's sound revision (sinceRev),
// so a leaf a later revision adds does not make an older package inexact; a revision of 0 (a
// state not from a package) or above this build's counts as this build's, so a leafless state
// is never exact. The report counts what makes the load inexact.
void ResolvePreset(const PresetState& preset, float* values, LoadReport* report) noexcept {
  *report = LoadReport{};
  if (!blob::ValidateStructure(preset, kSupportedModeFeatures, nullptr)) {
    report->invalidMode = true;
    return;
  }
  bool seen[kNumLeafParams] = {};
  for (size_t i = 0; i < kNumLeafParams; ++i) values[i] = FindParam(LeafId(i))->def;
  const uint32_t count =
      preset.leafCount < PresetState::kMaxLeaves ? preset.leafCount : PresetState::kMaxLeaves;
  report->unknownIds = preset.leafCount - count;
  for (uint32_t k = 0; k < count; ++k) {
    const PresetLeaf&      leaf = preset.leaves[k];
    const ParamDescriptor* d    = PlayableLeaf(leaf.id);
    if (d == nullptr) {
      ++report->unknownIds;
      continue;
    }
    const size_t i = LeafIndex(leaf.id);
    if (seen[i]) {
      ++report->duplicateIds;
      continue;
    }
    seen[i]       = true;
    const float v = CanonicalValue(*d, leaf.value);
    uint32_t    stored, canonical;
    std::memcpy(&stored, &leaf.value, sizeof stored);
    std::memcpy(&canonical, &v, sizeof canonical);
    if (canonical != stored) ++report->changedValues;
    values[i] = v;
  }
  const uint32_t rev =
      preset.soundRev == 0u || preset.soundRev > kSoundRevision ? kSoundRevision : preset.soundRev;
  for (size_t i = 0; i < kNumLeafParams; ++i) {
    if (!seen[i] && FindParam(LeafId(i))->sinceRev <= rev) ++report->missingIds;
  }
  report->unsupported = UnsupportedPerformance(preset.performance);
  report->exact = report->unknownIds == 0 && report->missingIds == 0 &&
                  report->duplicateIds == 0 && report->changedValues == 0 &&
                  report->unsupported == 0;
}

BRAINSCAPE_FP_BODY bool CheckPresetBody(const PresetState& preset, LoadReport* report) noexcept {
  float values[kNumLeafParams];
  ResolvePreset(preset, values, report);
  return report->exact;
}

}  // namespace

// The engine state behind Engine's opaque storage. Its methods are the bodies of the
// Engine's entry points; Engine forwards to them inside the FP environment guard.
struct Engine::Impl {
  BRAINSCAPE_FP_BODY bool Init(const EngineConfig&, const Arenas&) noexcept;
  BRAINSCAPE_FP_BODY void Reset() noexcept;
  BRAINSCAPE_FP_BODY void Restart() noexcept;
  BRAINSCAPE_FP_BODY void ClearHistory() noexcept;
  BRAINSCAPE_FP_BODY void Process(const ProcessContext&) noexcept;
  BRAINSCAPE_FP_BODY void SetParam(ParamId id, float plainValue) noexcept;
  BRAINSCAPE_FP_BODY bool LoadPreset(const PresetState&, LoadMode, LoadReport*,
                                      SwitchStyle) noexcept;
  float GetParam(ParamId id) const noexcept;  // a load, no FP arithmetic

  using Smoother = detail::Smoother;

  // R1 (design §7.2): a change to a stored row marks each domain its row's bitmask names, and
  // RebuildDirty rebuilds each marked domain once, before the next frame renders. Nothing is
  // dispatched by ID.
  void MarkDirty(size_t slot) noexcept { dirty_ |= RowOfSlot(slot).domain; }
  void RebuildGranularParams() noexcept;  // control-rate; runs only when a granular
                                          // param actually changed (keeps exp2/pow
                                          // off the steady-state audio path)
  void RebuildPostParams() noexcept;      // same discipline for the post chain
  void RebuildDirty() noexcept;
  float Active(ParamId id) const noexcept { return active_[SlotOf(id)]; }

  // The pieces of Process: what SetParam, SetFreeze and Trigger queued, applied at the
  // block's first frame; one event, which leaves a Freeze in *freeze for the frame's
  // events to settle; and the render of frames [start, start + count) of the block, which
  // advances the sample counter.
  void DrainPending() noexcept;
  void ApplyEvent(const BlockEvent&, bool* freeze) noexcept;
  void RenderFrames(const ProcessContext&, uint32_t start, uint32_t count) noexcept;

  // A canonical value from now on: the pending value too, so the next block start does
  // not re-apply an older SetParam.
  void SetValue(size_t slot, float value) noexcept;
  // A macro's or the expression pedal's leaves (design §3.4), as SetParam events would set them.
  void SetLeaves(const PresetLeaf* leaves, size_t n) noexcept;
  void SetFrozen(bool on) noexcept;
  // Design §7.3 step 3: the preset's mode and CTRL become the active ones, compared with the
  // active mode by content, and every domain rebuilds.
  void InstallMode(const PresetState& preset) noexcept;
  void ApplySpillover(const float* values, const PresetState& preset, SwitchStyle style) noexcept;

  EngineConfig cfg_{};
  int16_t*     ring_       = nullptr;  // interleaved stereo, historyFrames frames
  float*       windowLut_  = nullptr;  // Hot arena: kWindowLutSize half-cosine entries
  float*       wetL_       = nullptr;  // Hot arena: maxBlockSize each
  float*       wetR_       = nullptr;
  float*       fbFifo_     = nullptr;  // Warm arena: interleaved stereo,
                                       // kFeedbackDelayFrames frames (NOT maxBlockSize —
                                       // see the constant's rationale in Engine.h)
  ActiveMode*  mode_       = nullptr;  // Warm arena: the active mode and CTRL (design §7.3)
  uint32_t     mask_       = 0;
  uint32_t     writeFrame_ = 0;
  // mix_: the Mix knob, smoothed (its law: detail/MixLaw.h); wetGain_: the wet signal's gain
  // after the post chain (trim, effect volume, the cutoff kill: WetGainTarget); feedback_;
  // norm_: the voice normalization.
  Smoother     mix_, wetGain_, feedback_, norm_;
  int64_t      sampleCounter_ = 0;
  int64_t      epochStart_    = 0;  // random draws are keyed on sampleCounter_ - epochStart_
  // Due triggers by source, fired one per frame, footswitch first (audio thread). The mode's
  // sources gate them when they are due (RenderFrames, design §7.5): footswitch counts every
  // source but MidiNote.
  uint32_t     pendingFootswitch_ = 0;
  uint32_t     pendingMidi_       = 0;
  uint32_t     modeSwitches_  = 0;  // loads whose mode differed by content (ModeSwitches)
  bool         ready_         = false;
  uint8_t      dirty_         = kAllParamDomains;  // ParamDomain bits to rebuild
  bool         frozen_        = false;
  uint32_t     frozenAnchor_  = 0;
  // Nothing rendered since Init or ClearHistory cleared the ring and the post buffers, so
  // Restart need not clear them again.
  bool         historyClear_  = false;

  detail::GranularCore   granular_;
  detail::GranularParams gp_{};
  detail::PostChain      post_;
  detail::PostParams     pp_{};
  detail::FeedbackTamer  tamer_;
  detail::OnsetDetector  detector_;

  // Leaf and Global rows only, by slot (kStored): what SetParam stored and what the
  // engine applies.
  std::atomic<float>    pending_[kNumStored]{};
  std::atomic<bool>     freezePending_{false};
  std::atomic<uint32_t> onsetCount_{0};
  std::atomic<uint32_t> manualFootswitch_{0};  // Trigger() calls, by source
  std::atomic<uint32_t> manualMidi_{0};
  float                 active_[kNumStored]{};
};

Engine::Engine() noexcept {
  static_assert(sizeof(Impl) <= kEngineImplBytes, "raise kEngineImplBytes in Engine.h");
  static_assert(alignof(Impl) <= kEngineImplAlign, "raise kEngineImplAlign in Engine.h");
  // Engine's implicit destructor never runs ~Impl.
  static_assert(std::is_trivially_destructible<Impl>::value,
                "Impl must stay trivially destructible");
  ::new (static_cast<void*>(impl_)) Impl();
}

Engine::Impl& Engine::impl() noexcept { return *std::launder(reinterpret_cast<Impl*>(impl_)); }
const Engine::Impl& Engine::impl() const noexcept {
  return *std::launder(reinterpret_cast<const Impl*>(impl_));
}

// Entry points that run floating-point code: the guard writes the profile's control
// word and restores the caller's on return (determinism profile §4.1).
bool Engine::Init(const EngineConfig& cfg, const Arenas& arenas) noexcept {
  const detail::FpEnvGuard guard;
  return impl().Init(cfg, arenas);
}
void Engine::Reset() noexcept {
  const detail::FpEnvGuard guard;
  impl().Reset();
}
void Engine::Restart() noexcept {
  const detail::FpEnvGuard guard;
  impl().Restart();
}
void Engine::ClearHistory() noexcept {
  const detail::FpEnvGuard guard;
  impl().ClearHistory();
}
void Engine::Process(const ProcessContext& ctx) noexcept {
  const detail::FpEnvGuard guard;
  impl().Process(ctx);
}
void Engine::SetParam(ParamId id, float value, uint32_t /*sampleOffset*/) noexcept {
  const detail::FpEnvGuard guard;
  impl().SetParam(id, value);
}
bool Engine::LoadPreset(const PresetState& preset, LoadMode mode, LoadReport* report,
                        SwitchStyle style) noexcept {
  const detail::FpEnvGuard guard;
  return impl().LoadPreset(preset, mode, report, style);
}
float Engine::GetParam(ParamId id) const noexcept { return impl().GetParam(id); }

void Engine::SetFreeze(bool on) noexcept {
  impl().freezePending_.store(on, std::memory_order_relaxed);
}
bool Engine::GetFreeze() const noexcept {
  return impl().freezePending_.load(std::memory_order_relaxed);
}

void Engine::Trigger(TriggerSource src, float /*velocity*/, uint32_t /*sampleOffset*/) noexcept {
  // Gated by the mode's sources only when due (RenderFrames), so a load between this call and
  // the trigger's frame decides it as a Trigger event at that frame would be decided.
  (src == TriggerSource::MidiNote ? impl().manualMidi_ : impl().manualFootswitch_)
      .fetch_add(1u, std::memory_order_relaxed);
}

Engine::GrainStats Engine::Stats() const noexcept {
  const detail::GranularStats& s = impl().granular_.Stats();
  GrainStats out;
  out.births      = s.births;
  out.burstBirths = s.burstBirths;
  out.skips       = s.skips;
  return out;
}

uint32_t Engine::ConsumeOnsetCount() noexcept {
  return impl().onsetCount_.exchange(0u, std::memory_order_relaxed);
}

int64_t Engine::SampleCounter() const noexcept { return impl().sampleCounter_; }
int64_t Engine::EpochStart() const noexcept { return impl().epochStart_; }
uint32_t Engine::ModeSwitches() const noexcept { return impl().modeSwitches_; }

const ParamDescriptor* Descriptors(size_t* count) noexcept {
  if (count != nullptr) *count = kNumParams;
  return kParamTable;
}

const ParamDescriptor* Engine::Descriptors(size_t* count) noexcept {
  return brainscape::Descriptors(count);
}

const ParamDescriptor* FindParam(ParamId id) noexcept {
  const uint32_t raw = static_cast<uint32_t>(id);
  if (raw < 1 || raw > kNumParams) return nullptr;
  return &kParamTable[raw - 1];
}

float Canonicalize(ParamId id, float plainValue) noexcept {
  const detail::FpEnvGuard guard;
  return CanonicalizeBody(id, plainValue);
}

MemoryPlan PlanMemory(const EngineConfig& cfg) noexcept {
  const detail::FpEnvGuard guard;
  return PlanMemoryBody(cfg);
}

bool CheckPreset(const PresetState& preset, LoadReport* report) noexcept {
  const detail::FpEnvGuard guard;
  LoadReport               local;
  return CheckPresetBody(preset, report != nullptr ? report : &local);
}

bool Engine::Impl::Init(const EngineConfig& cfg, const Arenas& arenas) noexcept {
  ready_ = false;
  if (!SampleRateSupported(cfg.sampleRate)) return false;
  if (cfg.maxBlockSize == 0) return false;
  if (cfg.maxBlockSize > kFeedbackDelayFrames) return false;  // wrappers chunk larger buffers
  if (!IsPowerOfTwo(cfg.historyFrames)) return false;
  // Bounds guard both usefulness (design fixes the ring at 2^22; the ratio ceiling
  // makes anything past 2^26 meaningless) and 32-bit size_t overflow in PlanMemory,
  // which would otherwise wrap to a small plan and bypass arena validation entirely
  // (review finding).
  if (cfg.historyFrames < 8u || cfg.historyFrames > (1u << 26)) return false;
  if (cfg.looperFrames > (1u << 26)) return false;

  const MemoryPlan plan = PlanMemoryBody(cfg);
  for (size_t t = 0; t < kNumTiers; ++t) {
    if (plan.bytes[t] == 0) continue;
    if (arenas.base[t] == nullptr || arenas.bytes[t] < plan.bytes[t]) return false;
    // Alignment is part of the plan, not advice: a misaligned Bulk base violates the
    // SD/DMA cache-coherency rule (design §7) and misaligned int16 access is UB.
    if ((reinterpret_cast<uintptr_t>(arenas.base[t]) & (plan.align[t] - 1u)) != 0) return false;
  }

  cfg_  = cfg;
  ring_ = static_cast<int16_t*>(arenas.base[static_cast<size_t>(Tier::Bulk)]);
  auto* hot  = static_cast<float*>(arenas.base[static_cast<size_t>(Tier::Hot)]);
  windowLut_ = hot;
  wetL_      = hot + detail::kWindowLutSize;
  wetR_      = wetL_ + cfg.maxBlockSize;
  // Warm layout: [feedback FIFO][taming diffuser][mod lines + reverb tank].
  auto* warm = static_cast<float*>(arenas.base[static_cast<size_t>(Tier::Warm)]);
  fbFifo_    = warm;
  warm += static_cast<size_t>(kFeedbackDelayFrames) * 2u;
  tamer_.Init(warm, cfg.sampleRate);
  warm += detail::FeedbackTamer::WarmFloats(cfg.sampleRate);
  // Bulk layout: [history ring][looper A+B (future)][post-delay floats].
  auto* postBulk = reinterpret_cast<float*>(
      reinterpret_cast<char*>(arenas.base[static_cast<size_t>(Tier::Bulk)]) +
      static_cast<size_t>(cfg.historyFrames) * 2u * sizeof(int16_t) +
      static_cast<size_t>(cfg.looperFrames) * 2u * sizeof(int16_t) * 2u);
  post_.Init(warm, postBulk, cfg.sampleRate);
  warm += detail::PostChain::WarmFloats(cfg.sampleRate);
  detector_.Init(warm, cfg.sampleRate);
  warm += detail::OnsetDetector::WarmFloats();
  // The default mode (Mode.h), which plays as sound revision 1 did, until a load brings one.
  mode_          = ::new (static_cast<void*>(warm)) ActiveMode();
  modeSwitches_  = 0;
  mask_          = cfg.historyFrames - 1u;
  writeFrame_    = 0;
  sampleCounter_ = 0;
  epochStart_    = 0;
  pendingFootswitch_ = 0;
  pendingMidi_       = 0;
  frozen_        = false;
  frozenAnchor_  = 0;
  freezePending_.store(false, std::memory_order_relaxed);

  // The Bulk arena (SDRAM on hardware) has undefined contents at boot. Clear the
  // history ring here — and only the ring; looper buffers are cleared explicitly by
  // the caller when that subsystem lands (design §7).
  std::memset(ring_, 0, static_cast<size_t>(cfg.historyFrames) * 2u * sizeof(int16_t));
  std::memset(fbFifo_, 0, static_cast<size_t>(kFeedbackDelayFrames) * 2u * sizeof(float));

  // Half-cosine smoothing LUT: maps the unit-peak piecewise envelope value to its
  // cosine-eased equivalent. Mean over a linear ramp is 0.5 — identical to the
  // raw leg — which is what makes the (1+sustain)/2 window mean exact (GrainMath.h).
  for (uint32_t i = 0; i < detail::kWindowLutSize; ++i) {
    const double x = static_cast<double>(i) / static_cast<double>(detail::kWindowLutSize - 1);
    const double c  = detmath::CosPi(x);  // cos(pi*x): exact half-turn reduction
    const double om = 1.0 - c;
    windowLut_[i]   = static_cast<float>(0.5 * om);
  }

  granular_.Init(ring_, mask_, windowLut_);
  granular_.ClearStats();  // counts since Init (Engine::Stats)

  mix_.SetTau(10.0f, cfg.sampleRate);
  wetGain_.SetTau(10.0f, cfg.sampleRate);
  feedback_.SetTau(10.0f, cfg.sampleRate);
  norm_.SetTau(100.0f, cfg.sampleRate);  // design §3: τ ≈ 100 ms

  // Every stored row's default, Global rows included: Init is the one place that resets a
  // device setting (design §4.1).
  for (size_t i = 0; i < kNumStored; ++i) {
    const float def = RowOfSlot(i).def;
    pending_[i].store(def, std::memory_order_relaxed);
    active_[i] = def;
  }
  dirty_ = kAllParamDomains;
  RebuildDirty();
  post_.Reset(pp_);  // primes the post-chain mix smoothers from the ACTUAL params
  mix_.Prime(mix_.target);
  wetGain_.Prime(wetGain_.target);
  feedback_.Prime(feedback_.target);
  norm_.Prime(norm_.target);

  historyClear_ = true;  // the ring above, the post buffers in post_.Init
  ready_        = true;
  return true;
}

void Engine::Impl::Reset() noexcept {
  if (!ready_) return;
  granular_.Reset();
  tamer_.Reset();
  detector_.Reset();
  onsetCount_.store(0u, std::memory_order_relaxed);
  manualFootswitch_.store(0u, std::memory_order_relaxed);
  manualMidi_.store(0u, std::memory_order_relaxed);
  pendingFootswitch_ = 0;
  pendingMidi_       = 0;
  std::memset(fbFifo_, 0, static_cast<size_t>(kFeedbackDelayFrames) * 2u * sizeof(float));
  for (size_t i = 0; i < kNumStored; ++i) {
    active_[i] = pending_[i].load(std::memory_order_relaxed);
  }
  dirty_ = kAllParamDomains;
  RebuildDirty();
  // RT-safe post reset: small state + smoother priming only — clearing the
  // 750 KiB SDRAM post-delay here cost 2-4 consecutive audio deadlines (review
  // finding). The full buffer clear lives in ClearHistory (non-RT).
  post_.Reset(pp_);
  mix_.Prime(mix_.target);
  wetGain_.Prime(wetGain_.target);
  feedback_.Prime(feedback_.target);
  norm_.Prime(norm_.target);
}

void Engine::Impl::Restart() noexcept {
  if (!ready_) return;
  // What Init clears and Reset keeps; Reset below does the rest. Freeze goes off first,
  // because the normalization rebuilt in Reset depends on it.
  if (!historyClear_) ClearHistory();
  tamer_.ClearDiffusers();  // stale diffusers alone nulled feedback presets at -51 dB
  sampleCounter_ = 0;
  epochStart_    = 0;
  writeFrame_    = 0;
  frozen_        = false;
  frozenAnchor_  = 0;
  freezePending_.store(false, std::memory_order_relaxed);
  Reset();
}

void Engine::Impl::ClearHistory() noexcept {
  if (!ready_) return;
  std::memset(ring_, 0, static_cast<size_t>(cfg_.historyFrames) * 2u * sizeof(int16_t));
  post_.ClearBuffers();  // the post delay/reverb tails are history too
  historyClear_ = true;
}

bool Engine::Impl::LoadPreset(const PresetState& preset, LoadMode mode, LoadReport* report,
                              SwitchStyle style) noexcept {
  LoadReport r;
  float      values[kNumLeafParams];
  ResolvePreset(preset, values, &r);  // steps 0-2, and step 4's count
  if (ready_ && !r.invalidMode) {      // an invalid mode applies nothing (step 0)
    r.applied = true;
    if (mode == LoadMode::Exact) {
      // The leaves become pending and the mode active; Restart turns freeze off, drains the
      // leaves and rebuilds every domain from the new mode, snapping every smoother. Global
      // rows keep their pending values (design §4.1).
      for (size_t i = 0; i < kNumLeafParams; ++i) {
        pending_[SlotOf(LeafId(i))].store(values[i], std::memory_order_relaxed);
      }
      InstallMode(preset);
      Restart();
    } else {
      ApplySpillover(values, preset, style);
    }
  }
  if (report != nullptr) *report = r;
  return r.applied && r.exact;
}

void Engine::Impl::InstallMode(const PresetState& preset) noexcept {
  if (!SameModeContent(mode_->mode, preset.mode)) {
    ++modeSwitches_;
    // A different mode resets the sequencing state a load of the same mode keeps (design
    // §7.3): the pitch-cycle index, bursts, the step position and modulator phases, which the
    // waves that build them reset here (the bursts in progress since sound revision 4, the
    // pitch cycle's position since 5).
    // Scheduler phase, marks, grains and due triggers always carry over.
    granular_.ResetSequencing();
  }
  std::memcpy(&mode_->mode, &preset.mode, sizeof(ModeBlob));
  std::memcpy(&mode_->control, &preset.control, sizeof(ControlState));
  dirty_ = kAllParamDomains;  // the structure feeds the rebuilds (design §7.3 step 3)
}

// A Spillover load at the current frame (determinism profile §5.10; design §7.3): the complete
// preset and its mode as one change, the grains sounding now as `style` says (Trails: they
// finish as resolved at birth; FastCut: they fade out over kFastCutFrames), freeze off, and the
// random-number epoch restarted here. Marks, the scheduler phase, smoothers and every buffer
// carry over, so the output never reconverges with an Exact load's (§2.4), but it stays a
// deterministic function of the event stream. `values` holds every leaf by ordinal; Global rows
// are untouched.
void Engine::Impl::ApplySpillover(const float* values, const PresetState& preset,
                                  SwitchStyle style) noexcept {
  for (size_t i = 0; i < kNumLeafParams; ++i) SetValue(SlotOf(LeafId(i)), values[i]);
  InstallMode(preset);
  if (style == SwitchStyle::FastCut) granular_.FastCut(sampleCounter_);
  freezePending_.store(false, std::memory_order_relaxed);
  SetFrozen(false);
  epochStart_ = sampleCounter_;
}

void Engine::Impl::SetValue(size_t slot, float value) noexcept {
  pending_[slot].store(value, std::memory_order_relaxed);
  if (value != active_[slot]) {
    active_[slot] = value;
    MarkDirty(slot);
  }
}

void Engine::Impl::SetLeaves(const PresetLeaf* leaves, size_t n) noexcept {
  for (size_t i = 0; i < n; ++i) {
    const size_t slot = SlotOf(static_cast<ParamId>(leaves[i].id));
    if (slot == kNumStored || RowOfSlot(slot).kind != ParamKind::Leaf) continue;
    SetValue(slot, CanonicalValue(RowOfSlot(slot), leaves[i].value));
  }
}

void Engine::Impl::SetFrozen(bool on) noexcept {
  if (on == frozen_) return;
  frozen_ = on;
  if (frozen_) frozenAnchor_ = writeFrame_;  // pin the anchor at engage (design §2.4)
  dirty_ |= kDomainGranular;  // the normalization exponent depends on it
}

void Engine::ClearLooper() noexcept {
  // No looper buffers yet. When they land, this stays an explicit user gesture —
  // never called from a plugin prepare path (design §7).
}

void Engine::Impl::RebuildPostParams() noexcept {
  const auto get = [&](ParamId id) { return active_[SlotOf(id)]; };
  pp_.modRateHz    = get(ParamId::ModRateHz);
  pp_.modDepth     = get(ParamId::ModDepth);
  pp_.delayFrames  = static_cast<float>(get(ParamId::DelayTimeMs) * 0.001 * cfg_.sampleRate);
  pp_.delayFb      = get(ParamId::DelayFb);
  pp_.delayMix     = get(ParamId::DelayMix);
  pp_.reverbTime = get(ParamId::ReverbTime);
  pp_.reverbMix  = get(ParamId::ReverbMix);
  const float rawCutoff = get(ParamId::FilterCutoffHz);
  // Bypass decides on the RAW knob value (fully CW = bypass, design §2.6); the
  // engaged cutoff is clamped against the actual rate so the knob cannot silently
  // pin past sr/4 at low sample rates (review finding).
  pp_.filterBypass = rawCutoff >= FindParam(ParamId::FilterCutoffHz)->max - 0.5f;
  const auto maxHz = static_cast<float>(cfg_.sampleRate * 0.45);
  pp_.filterCutoff = rawCutoff < maxHz ? rawCutoff : maxHz;
  pp_.filterRes    = get(ParamId::FilterRes);
  pp_.filterMorph  = get(ParamId::FilterMorph);
}

void Engine::Impl::RebuildGranularParams() noexcept {
  const auto get = [&](ParamId id) { return active_[SlotOf(id)]; };
  const double sr = cfg_.sampleRate;

  gp_.baseDelayFrames = static_cast<double>(get(ParamId::DelayMs)) * 0.001 * sr;
  gp_.sprayFrames     = static_cast<float>(get(ParamId::SprayMs) * 0.001 * sr);
  // ONE rounded integer drives grain length, spacing, and the voice budget —
  // spacing from the unrounded float opened duty-cycle holes across 80% of the
  // size_ms range (review finding, up to 48.8% silence).
  const double sizeFrames = get(ParamId::GrainSizeMs) * 0.001 * sr;
  assert(sizeFrames >= 0.0 && sizeFrames <= 0.5 * sr + 1.0);  // profile §3.10: 1-500 ms
  auto total = static_cast<uint32_t>(detmath::RoundHalfAwayI32(sizeFrames));
  if (total < 1u) total = 1u;
  gp_.totalFrames = total;

  const float overlap = get(ParamId::Overlap);
  float target        = static_cast<float>(kMaxGrains) * overlap * overlap * overlap;
  if (target < 1.0f) target = 1.0f;
  if (target > static_cast<float>(kMaxGrains)) target = static_cast<float>(kMaxGrains);
  // The 1-frame inter-arrival floor caps sustainable voices at the grain length:
  // normalizing to an unreachable target read up to -12.9 dB low (review finding).
  if (target > static_cast<float>(total)) target = static_cast<float>(total);
  gp_.targetVoices = target;

  gp_.jitter      = get(ParamId::Jitter);
  // The pitch (design §7.5, R10; sound revision 5): layer 0's pitch set, each entry plus the
  // transpose leaf (ID 8) as an offset over it, picked at birth by `cycle` or `random`; detune
  // and the ±24 st clamp follow at birth. The validated mode's set has 1-8 entries of weight
  // 1-16. The default set {0: 1}'s 0 + t is t exactly for every canonical t, so it plays sound
  // revision 1's pitch.
  const ModeBlob& mode      = mode_->mode;
  const PitchSet& set       = mode.pitch[0];
  const float     transpose = get(ParamId::TransposeSt);
  assert(set.count >= 1u && set.count <= detail::kGranularMaxPitch);
  gp_.pitchCount     = set.count;
  gp_.pitchWeightSum = 0;
  for (uint32_t i = 0; i < set.count; ++i) {
    gp_.pitchSt[i]     = set.entries[i].st + transpose;
    gp_.pitchWeight[i] = set.entries[i].weight;
    gp_.pitchWeightSum += set.entries[i].weight;
  }
  gp_.pitchRandom = mode.layers[0].pitchSelect == PitchSelect::Random;
  gp_.spreadCents = get(ParamId::SpreadCents);
  gp_.reverseProb = get(ParamId::ReverseProb);
  gp_.sustain      = get(ParamId::WindowSustain);
  gp_.skew         = get(ParamId::WindowSkew);
  gp_.smoothness   = get(ParamId::WindowSmooth);
  gp_.panSpread    = get(ParamId::PanSpread);
  // Structure, from the active mode (design §7.3; rows 27 and 28 retired into it at r2). The
  // validated mode's sources and position are this build's: onset on or off, live or mark.
  gp_.onsetTrigger = (mode.schedule.sources & kSourceOnset) != 0u;
  gp_.posFromMark  = mode.layers[0].source == PositionSource::Mark;
  // Trigger sources, bursts and intermittency (design §7.5, R9; sound revision 4). The free-
  // running scheduler runs only with `periodic`; footswitch and MIDI triggers are gated where
  // they fall due (RenderFrames). Counting leaves are read as RoundHalfAwayI32 of the canonical
  // value, in range because canonicalization clamped it (§3.7).
  gp_.periodic      = (mode.schedule.sources & kSourcePeriodic) != 0u;
  gp_.intermittency = get(ParamId::Intermittency);
  const int32_t burst = detmath::RoundHalfAwayI32(static_cast<double>(get(ParamId::BurstCount)));
  assert(burst >= 1 && burst <= 16);
  gp_.burstCount = static_cast<uint32_t>(burst);
  // max(1, round(spacing_ms * 48)) at 48 kHz: sr / 1000 is exact there, so the product is.
  const double spacingFrames = static_cast<double>(get(ParamId::BurstSpacingMs)) * (sr / 1000.0);
  assert(spacingFrames >= 0.0 && spacingFrames <= 0.5 * sr + 1.0);  // profile §3.10: 0-500 ms
  const int32_t spacing = detmath::RoundHalfAwayI32(spacingFrames);
  gp_.burstSpacing      = spacing >= 1 ? static_cast<uint32_t>(spacing) : 1u;

  // Coherence-aware normalization exponent (design §3): unity-rate, zero-spray
  // grains all read the SAME source sample and sum coherently (1/N); anything
  // that spreads their read positions decorrelates them toward 1/sqrt(N).
  // Every term is continuous and expressed as accumulated divergence — the v1
  // heuristic's boolean pitch term was a +17.85 dB cliff at 0.001 st, its
  // frame-keyed spray term saturated at 1.33 ms (and differed per sample rate),
  // and its jitter term had the wrong sign: at unity rate a grain's output is
  // independent of its birth time, so timing jitter decorrelates nothing
  // (review findings, all measured).
  // The pitch term takes the set's largest |ratio - 1| (design §7.5), each entry clamped as a
  // grain clamps it; for the one-entry default set that is revision 1's term.
  float pitchSpan = 0.f;
  for (uint32_t i = 0; i < gp_.pitchCount; ++i) {
    float st = gp_.pitchSt[i];
    if (st > 24.f) st = 24.f;
    if (st < -24.f) st = -24.f;
    const float d = detmath::Abs(grainmath::SemitonesToRatio(st) - 1.0f);
    if (d > pitchSpan) pitchSpan = d;
  }
  const float kDecorrFrames = static_cast<float>(0.003 * sr);  // ~3 ms of divergence = full
  auto clamp01 = [](float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); };
  float decorr = 0.f;
  decorr += clamp01(pitchSpan * static_cast<float>(total) / kDecorrFrames);
  decorr += clamp01(get(ParamId::SprayMs) * (1.0f / 30.0f));  // ms-based: rate-independent
  decorr += clamp01(detmath::Abs(grainmath::SemitonesToRatio(gp_.spreadCents * 0.01f) - 1.0f) *
                    static_cast<float>(total) / kDecorrFrames);
  decorr += gp_.reverseProb;
  // Freeze pins the anchor, turning identical grains into time-shifted copies of
  // one window — fully decorrelated. Without this term, engaging freeze on a
  // coherent preset dropped the wet path up to 16.7 dB (review finding).
  // POS_MARK is the same geometry (all grains anchored to one ring frame, born
  // at different times) — without its term the Strum family read up to 18 dB
  // quiet across the overlap knob (review finding).
  if (frozen_ || gp_.posFromMark) decorr = 1.0f;
  decorr        = clamp01(decorr);
  const float p = 1.0f - 0.5f * decorr;
  // N, the voices normalized for (design §7.5): the free-running target, or without a free-
  // running source the grains one trigger births.
  const float voices = gp_.periodic ? gp_.targetVoices : static_cast<float>(gp_.burstCount);
  norm_.target       = detmath::PowF(voices, -p);
}

// Leaf and Global rows store; every other kind is a no-op (design §4.1, §7.4): a Macro
// moves through MacroMove, a Performance row through its own events.
void Engine::Impl::SetParam(ParamId id, float value) noexcept {
  const size_t slot = SlotOf(id);
  if (slot == kNumStored) return;
  // NaN must never reach the smoothers, where it is an absorbing state recoverable
  // only by Reset()/Init() (review finding, verified), nor any other engine state.
  pending_[slot].store(CanonicalValue(RowOfSlot(slot), value), std::memory_order_relaxed);
}

float Engine::Impl::GetParam(ParamId id) const noexcept {
  const size_t slot = SlotOf(id);
  if (slot == kNumStored) return 0.f;
  return pending_[slot].load(std::memory_order_relaxed);
}

void Engine::Impl::Process(const ProcessContext& ctx) noexcept {
  // No assert on ready_: the documented contract IS the zero-fill below, and an
  // assert here killed the whole suite on the Debug/sanitizer CI leg (review
  // finding). The block-size assert stays — that one is a genuine caller bug.
  assert(ctx.numFrames >= 1 && ctx.numFrames <= cfg_.maxBlockSize);
  const BlockEvent* events    = ctx.numEvents > 0 ? ctx.events : nullptr;
  const uint32_t    numEvents = events != nullptr ? ctx.numEvents : 0u;
  assert(ctx.numEvents == 0 || ctx.events != nullptr);
  bool freeze = false;  // the freeze level the events at one frame leave
  if (!ready_ || ctx.in == nullptr || ctx.out == nullptr || ctx.numFrames == 0 ||
      ctx.numFrames > cfg_.maxBlockSize) {
    // Never hand back stale host memory — and never overrun the Hot-arena wet
    // buffers on an oversized block (review finding: hosts do hand out blocks
    // larger than the prepared maximum; that was a silent heap overflow).
    if (ctx.out != nullptr) {
      for (uint32_t n = 0; n < ctx.numFrames; ++n) {
        if (ctx.out[0] != nullptr) ctx.out[0][n] = 0.f;
        if (ctx.out[1] != nullptr) ctx.out[1][n] = 0.f;
      }
    }
    // The events apply after the frames the call did not render, as offsets past a
    // block do: a dropped Freeze or load would change everything after it.
    if (ready_) {
      for (uint32_t i = 0; i < numEvents; ++i) ApplyEvent(events[i], &freeze);
    }
    return;
  }

#if !defined(NDEBUG)
  for (uint32_t i = 0; i < numEvents; ++i) {
    assert(events[i].offset < ctx.numFrames);
    assert(i == 0 || events[i].offset >= events[i - 1].offset);
    assert(i == 0 || events[i].offset != events[i - 1].offset || events[i].offset == 0 ||
           events[i].seq > events[i - 1].seq);
  }
#endif

  // The block is rendered in parts split at its events' offsets (determinism profile
  // §5.11): each part is exactly a Process call of a wrapper that splits there, so
  // block-split invariance (contract #1) carries over to event timing. Frames before an
  // event see the old state. Freeze settles once per frame, after the frame's events,
  // as DrainPending settles SetFreeze.
  historyClear_ = false;
  DrainPending();
  freeze        = frozen_;
  uint32_t pos  = 0;
  uint32_t next = 0;
  for (;;) {
    while (next < numEvents && events[next].offset <= pos) ApplyEvent(events[next++], &freeze);
    SetFrozen(freeze);
    RebuildDirty();
    uint32_t end = ctx.numFrames;
    if (next < numEvents && events[next].offset < end) end = events[next].offset;
    RenderFrames(ctx, pos, end - pos);
    pos = end;
    if (pos == ctx.numFrames) break;
  }
  // Offsets past the block apply after its last frame; the next block rebuilds, and its
  // DrainPending settles their freeze.
  while (next < numEvents) ApplyEvent(events[next++], &freeze);
}

void Engine::Impl::DrainPending() noexcept {
  // Freeze drains FIRST because the normalization exponent depends on it (frozen
  // grains are decorrelated). Re-anchor-on-wrap (design §2.4) runs per sample inside
  // granular_.Process: decided here at block start, the splice moved with the block grid.
  SetFrozen(freezePending_.load(std::memory_order_relaxed));
  for (size_t i = 0; i < kNumStored; ++i) {
    const float p = pending_[i].load(std::memory_order_relaxed);
    if (p != active_[i]) {
      active_[i] = p;
      MarkDirty(i);
    }
  }
  pendingFootswitch_ += manualFootswitch_.exchange(0u, std::memory_order_relaxed);
  pendingMidi_ += manualMidi_.exchange(0u, std::memory_order_relaxed);
}

void Engine::Impl::ApplyEvent(const BlockEvent& e, bool* freeze) noexcept {
  switch (e.type) {
    case EventType::SetParam: {  // Leaf and Global rows only, as SetParam
      const size_t slot = SlotOf(static_cast<ParamId>(e.id));
      if (slot != kNumStored) SetValue(slot, CanonicalValue(RowOfSlot(slot), e.value));
      break;
    }
    case EventType::Freeze:
      *freeze = e.value != 0.0f;
      freezePending_.store(*freeze, std::memory_order_relaxed);
      break;
    case EventType::Trigger:  // id: its source, gated when due (RenderFrames)
      if (e.id == static_cast<uint32_t>(Engine::TriggerSource::MidiNote)) {
        ++pendingMidi_;
      } else {
        ++pendingFootswitch_;
      }
      break;
    case EventType::SpilloverLoad:
      // The producer learns whether the preset is exact from CheckPreset, when it stages
      // the preset; here only the values count, and an invalid mode applies nothing.
      if (e.preset != nullptr) {
        float      values[kNumLeafParams];
        LoadReport report;
        ResolvePreset(*e.preset, values, &report);
        if (report.invalidMode) break;
        const SwitchStyle style = e.id == static_cast<uint32_t>(SwitchStyle::FastCut)
                                      ? SwitchStyle::FastCut
                                      : SwitchStyle::Trails;
        ApplySpillover(values, *e.preset, style);
        *freeze = false;
      }
      break;
    case EventType::MacroMove: {  // design §3.4: the targets in list order, as SetParams
      PresetLeaf   out[kMaxMacroTargets];
      const size_t n = detail::EvalMacroBody(mode_->mode, static_cast<ParamId>(e.id), e.value,
                                             out, kMaxMacroTargets);
      SetLeaves(out, n);
      break;
    }
    case EventType::Expression: {  // CTRL's assignments in order
      PresetLeaf   out[kMaxExpressions * kMaxMacroTargets];
      const size_t n = detail::EvalExpressionBody(mode_->mode, mode_->control, e.value, out,
                                                  kMaxExpressions * kMaxMacroTargets);
      SetLeaves(out, n);
      break;
    }
  }
}

// Each marked domain's rebuild, once (design §7.2, R1). The switch names every domain, so a new
// one cannot land without its rebuild (the static_assert on kAllParamDomains, and -Wswitch).
void Engine::Impl::RebuildDirty() noexcept {
  const uint8_t dirty = dirty_;
  dirty_              = 0;
  for (uint32_t bit = 0; bit < 6u; ++bit) {
    const auto domain = static_cast<ParamDomain>(1u << bit);
    if ((dirty & domain) == 0u) continue;
    switch (domain) {
      case kDomainNone: break;
      case kDomainGranular: RebuildGranularParams(); break;
      case kDomainPost: RebuildPostParams(); break;
      case kDomainMix: mix_.target = Active(ParamId::Mix); break;
      case kDomainFeedback: {
        const float fb   = Active(ParamId::Feedback);
        feedback_.target = fb;
        tamer_.SetFeedback(fb, cfg_.sampleRate);  // LP corner rides regeneration
        break;
      }
      case kDomainWet:
        wetGain_.target = WetGainTarget(Active(ParamId::WetTrimDb),
                                        Active(ParamId::EffectVolumeDb),
                                        Active(ParamId::FilterCutoffHz));
        break;
      case kDomainDetector: detector_.SetSensitivity(Active(ParamId::TriggerSens)); break;
    }
  }
}

void Engine::Impl::RenderFrames(const ProcessContext& ctx, uint32_t start,
                                uint32_t count) noexcept {
  const float* inL  = ctx.in[0] + start;
  const float* inR  = (cfg_.stereoInput ? ctx.in[1] : ctx.in[0]) + start;
  float*       outL = ctx.out[0] + start;
  float*       outR = ctx.out[1] + start;

  const uint32_t ringStart = writeFrame_;
  const bool     dither    = cfg_.ditherRingWrite;

  // ── Pass 1: write input (+ feedback) into the ring, per sample, and feed the
  // onset detector (hop boundaries on the absolute grid, so detection is
  // split-invariant). The feedback signal is the wet output delayed by exactly
  // kFeedbackDelayFrames through a fixed-length FIFO — a shared build constant,
  // so the loop period is identical on firmware and plugin (contracts #1/#6).
  // Slot index is a mask of the absolute sample (power-of-two length) — a 64-bit
  // modulo compiled to two __aeabi_uldivmod calls per sample on Cortex-M7.
  // Deliver at most `count` due triggers and CARRY the surplus — draining the
  // counter dropped every trigger past numFrames, silently breaking the "explicit
  // triggers never drop" contract on small blocks (review finding).
  // The mode's trigger sources (design §7.5, R9): a due trigger whose source the playing mode
  // leaves out is dropped here, at the first frame it is due. A render span starts at every
  // event's frame, so a load decides the triggers due from its frame on, by any delivery.
  const uint8_t sources = mode_->mode.schedule.sources;
  if ((sources & kSourceFootswitch) == 0u) pendingFootswitch_ = 0;
  if ((sources & kSourceMidiNote) == 0u) pendingMidi_ = 0;
  const uint32_t pending = pendingFootswitch_ + pendingMidi_;
  detail::TriggerEvents ev;
  ev.manualCount = pending < count ? pending : count;
  const uint32_t fromFootswitch =
      ev.manualCount < pendingFootswitch_ ? ev.manualCount : pendingFootswitch_;
  pendingFootswitch_ -= fromFootswitch;
  pendingMidi_ -= ev.manualCount - fromFootswitch;
  for (uint32_t n = 0; n < count; ++n) {
    const int64_t abs  = sampleCounter_ + n;
    const auto    slot = static_cast<uint32_t>(abs) & (kFeedbackDelayFrames - 1u);
    const float   fb   = feedback_.Next();
    float wrL          = inL[n] + fb * fbFifo_[2u * slot];
    float wrR          = inR[n] + fb * fbFifo_[2u * slot + 1u];

    // Detector listens to the raw mono input (pre-feedback: regenerated wet must
    // not re-trigger grains — that would be a trigger feedback loop). It runs
    // unconditionally even when nothing consumes onsets: the trigger LED must
    // stay live for sensitivity calibration (research rec #7), and gating it on
    // parameters would make the whitening state parameter-history dependent.
    float det = 0.5f * (inL[n] + inR[n]);
    if (det > kDetectorBound) det = kDetectorBound;
    if (det < -kDetectorBound) det = -kDetectorBound;
    if (detector_.ProcessSample(det, abs)) {
      onsetCount_.fetch_add(1u, std::memory_order_relaxed);  // LED even if the
                                                             // event list is full
      if (ev.onsetCount < detail::TriggerEvents::kMaxOnsets) {
        ev.onsetOffset[ev.onsetCount] = n;
        // Attribute the onset's audio to the start of the hop just analyzed.
        ev.onsetMarkFrame[ev.onsetCount] =
            (ringStart + n + 1u - detail::kOnsetHop) & mask_;
        ++ev.onsetCount;
      }
    }
    if (dither) {
      // Gated at a quarter LSB, not at zero: below half an LSB plain rounding
      // already absorbs to exact 0 (no fixed points exist down there), while a
      // zero gate let the taming chain's asymptotic filter tail hold the gate
      // open and sustain an LSB-level dither loop forever. Above the gate, the
      // write value performs a downward random walk absorbed at 0 instead of
      // latching on a quantization fixed point.
      constexpr float kDitherGate = 0.25f / 32767.0f;
      const int64_t key = abs - epochStart_;
      if (wrL > kDitherGate || wrL < -kDitherGate) wrL += Tpdf(key, grainmath::Draw::DitherL);
      if (wrR > kDitherGate || wrR < -kDitherGate) wrR += Tpdf(key, grainmath::Draw::DitherR);
    }
    ring_[2u * writeFrame_]      = QuantizeS16(wrL);
    ring_[2u * writeFrame_ + 1u] = QuantizeS16(wrR);
    writeFrame_ = (writeFrame_ + 1u) & mask_;
  }

  // ── Pass 2: schedule + render the grain block (per-grain over the whole block).
  granular_.Process(gp_, ev, sampleCounter_, epochStart_, ringStart, frozen_, &frozenAnchor_,
                    count, wetL_, wetR_);

  // ── Pass 3a: normalization (smoothed), then the feedback tap — TAMED wet into
  // the FIFO (design §2.3: DC/HP/LP/saturator/diffuser sit inside the loop; the
  // tap is pre-post-chain per the §2 diagram). The tamer always runs so its
  // filter state stays split-invariant regardless of the feedback amount.
  for (uint32_t n = 0; n < count; ++n) {
    const int64_t abs  = sampleCounter_ + n;
    const auto    slot = static_cast<uint32_t>(abs) & (kFeedbackDelayFrames - 1u);
    const float   nrm  = norm_.Next();
    const float   wl   = wetL_[n] * nrm;
    const float   wr   = wetR_[n] * nrm;
    wetL_[n] = wl;
    wetR_[n] = wr;
    float tl = wl, tr = wr;
    tamer_.ProcessSample(tl, tr);
    fbFifo_[2u * slot]      = tl;
    fbFifo_[2u * slot + 1u] = tr;
  }

  // ── Pass 3b: the post chain, in place on the wet buffers (design §2.6:
  // mod -> delay -> reverb -> filter, ordered and bypassable).
  post_.Process(pp_, count, wetL_, wetR_);

  // ── Pass 3c: the wet gain, then the wet/dry mix.
  for (uint32_t n = 0; n < count; ++n) {
    // The Mix law (mode-compiler.md §7.1, R3b; sound revision 3): dry at unity up to the
    // middle, wet at unity from it, on the smoothed Mix (detail/MixLaw.h).
    const detail::MixGains mix = detail::MixLaw(mix_.Next());
    // The wet signal's gain after the post chain (design §7.2, R3): the mode's trim and the
    // player's effect volume, or exactly 0 under the cutoff kill; it never scales the dry. At
    // gain 1 every product below is exact, so a preset with no trim plays revision 1's bits.
    const float g   = wetGain_.Next();
    // Dry is never delayed (grain-delay-theory.md §3.11). Two-multiply form, not dry +
    // mix*(wet-dry): the lerp form is not bit-exact at the endpoints, which would break the Tu
    // null contract (design §10 #2); at Mix 0 and 1 the law's gains are exactly (1, 0) and
    // (0, 1), revision 2's products.
    // Both dry reads precede either write: hosts process in place (in[0] == out[0])
    // and mono input aliases inR to inL, so writing outL first corrupted every outR.
    const float dryL = inL[n];
    const float dryR = inR[n];
    const float wl   = wetL_[n] * g;
    const float wr   = wetR_[n] * g;
    float       oL   = dryL * mix.dry + wl * mix.wet;
    float       oR   = dryR * mix.dry + wr * mix.wet;
    // Finite input gives finite output (determinism profile §3.7): a sum near FLT_MAX
    // saturates instead of overflowing. One-sided compares, so NaN from unsanitized input
    // still reaches the Debug check below.
    if (oL > kMaxFinite) oL = kMaxFinite;
    if (oL < -kMaxFinite) oL = -kMaxFinite;
    if (oR > kMaxFinite) oR = kMaxFinite;
    if (oR < -kMaxFinite) oR = -kMaxFinite;
    outL[n] = oL;
    outR[n] = oR;
  }

#if !defined(NDEBUG)
  // The engine never makes NaN or infinity (determinism profile §3.7), and finite
  // input saturates at the output; a non-finite output here means non-finite input
  // that the wrapper failed to sanitize.
  for (uint32_t n = 0; n < count; ++n) {
    assert(detmath::IsFinite(outL[n]) && detmath::IsFinite(outR[n]));
  }
  assert(detmath::IsFinite(mix_.value) && detmath::IsFinite(wetGain_.value) &&
         detmath::IsFinite(feedback_.value) && detmath::IsFinite(norm_.value));
#endif

  sampleCounter_ += count;
}

}  // namespace brainscape
