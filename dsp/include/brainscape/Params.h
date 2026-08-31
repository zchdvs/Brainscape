#pragma once
#include <cstddef>
#include <cstdint>

namespace brainscape {

// Permanent parameter identities (docs/design/grain-engine.md §6): IDs and names are
// never reused or reassigned once released — retire and rename instead. Values are
// plain (denormalized) units everywhere; normalizing to [0,1] is the plugin wrapper's
// job (VST3/CLAP convention, docs/research/preset-parameter-and-patch-format.md).
enum class ParamId : uint32_t {
  DelayMs   = 1,  // layer0.position.base_ms (skeleton: the single unity tap)
  Mix       = 2,  // global wet/dry, linear crossfade (grain-delay-theory.md §3.11)
  Feedback  = 3,  // feedback.amount (the taming chain lands with the grain engine)
  OutTrimDb = 4,  // out_trim_db
};

struct ParamDescriptor {
  ParamId     id;
  const char* name;  // stable string name — doubles as the preset schema key
  float       min, max, def;
  const char* unit;
};

// Table order must match contiguous ids starting at 1 (checked by static_assert
// in Engine.cpp) so id -> slot lookup is a subtraction.
inline constexpr ParamDescriptor kParamTable[] = {
    {ParamId::DelayMs,   "layer0.position.base_ms", 1.0f,   5000.0f, 250.0f, "ms"},
    {ParamId::Mix,       "mix",                     0.0f,   1.0f,    0.5f,   ""},
    {ParamId::Feedback,  "feedback.amount",         0.0f,   0.95f,   0.0f,   ""},
    {ParamId::OutTrimDb, "out_trim_db",             -24.0f, 24.0f,   0.0f,   "dB"},
};
inline constexpr size_t kNumParams = sizeof(kParamTable) / sizeof(kParamTable[0]);

const ParamDescriptor* Descriptors(size_t* count) noexcept;
const ParamDescriptor* FindParam(ParamId id) noexcept;

}  // namespace brainscape
