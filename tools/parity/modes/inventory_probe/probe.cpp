// Probe: does a SetParam event that changes ONLY OnsetTrigger (id 27) or PositionSource (28)
// take effect? Engine::Impl::ApplyParam classifies ids >= ModRateHz (16) as post-chain params,
// so the granular block (which reads 27/28) may not be rebuilt.
#include <cstdio>
#include <cstring>
#include <vector>
#include "brainscape/Engine.h"
#include "brainscape/HostArenas.h"
#include "brainscape/TestSignal.h"
using namespace brainscape;

static std::vector<float> Render(bool startOn, bool eventOn, ParamId which, uint32_t* onsets) {
  EngineConfig cfg; cfg.maxBlockSize = 48;
  host::HeapArenas ar(PlanMemory(cfg));
  Engine e; if (!e.Init(cfg, ar.get())) { std::puts("init failed"); return {}; }
  PresetState ps; ps.leafCount = 0;
  auto add = [&](ParamId id, float v){ ps.leaves[ps.leafCount++] = {static_cast<uint32_t>(id), v}; };
  // complete preset in ascending id order
  size_t n; const ParamDescriptor* d = Descriptors(&n);
  for (size_t i = 0; i < n; ++i) {
    float v = d[i].def;
    if (d[i].id == ParamId::Mix) v = 1.0f;
    if (d[i].id == ParamId::Overlap) v = 0.2f;          // sparse scheduler births
    if (d[i].id == ParamId::GrainSizeMs) v = 60.0f;
    if (d[i].id == which) v = startOn ? 1.0f : 0.0f;
    if (which == ParamId::PositionSource && d[i].id == ParamId::OnsetTrigger) v = 1.0f;
    add(d[i].id, v);
  }
  e.LoadPreset(ps, LoadMode::Exact);
  testsignal::Note notes[64];
  const uint32_t frames = 48000 * 4;
  uint32_t cnt = testsignal::BuildVector(testsignal::Vector::Plucks, frames, notes, 64);
  testsignal::Generator g; g.Start(notes, cnt < 64 ? cnt : 64);
  std::vector<float> out; out.reserve(frames * 2);
  float inL[48], inR[48], oL[48], oR[48];
  const float* in[2] = {inL, inR}; float* o[2] = {oL, oR};
  uint32_t total = 0;
  for (uint32_t f = 0; f < frames; f += 48) {
    g.Render(inL, inR, 48);
    Engine::BlockEvent ev; ev.offset = 0; ev.seq = 0; ev.type = Engine::EventType::SetParam;
    ev.id = static_cast<uint32_t>(which); ev.value = eventOn ? 1.0f : 0.0f;
    Engine::ProcessContext ctx; ctx.in = in; ctx.out = o; ctx.numFrames = 48;
    if (f == 0 && startOn != eventOn) { ctx.events = &ev; ctx.numEvents = 1; }
    e.Process(ctx);
    total += e.ConsumeOnsetCount();
    for (int i = 0; i < 48; ++i) { out.push_back(oL[i]); out.push_back(oR[i]); }
  }
  *onsets = total;
  return out;
}

int main() {
  for (ParamId which : {ParamId::OnsetTrigger, ParamId::PositionSource}) {
    uint32_t o1 = 0, o2 = 0, o3 = 0;
    auto a = Render(true, true, which, &o1);    // on from the preset
    auto b = Render(false, true, which, &o2);   // off in the preset, event turns it on at frame 0
    auto c = Render(false, false, which, &o3);  // off throughout
    size_t firstAB = a.size(), firstBC = a.size();
    for (size_t i = 0; i < a.size(); ++i) if (std::memcmp(&a[i], &b[i], 4)) { firstAB = i; break; }
    for (size_t i = 0; i < a.size(); ++i) if (std::memcmp(&b[i], &c[i], 4)) { firstBC = i; break; }
    std::printf("%s: onsets %u/%u/%u; on-from-preset vs event-on: %s (first diff frame %zu); event-on vs never-on: %s (first diff frame %zu)\n",
      which == ParamId::OnsetTrigger ? "OnsetTrigger(27)" : "PositionSource(28)", o1, o2, o3,
      firstAB == a.size() ? "IDENTICAL" : "DIFFER", firstAB / 2,
      firstBC == a.size() ? "IDENTICAL" : "DIFFER", firstBC / 2);
  }
}
