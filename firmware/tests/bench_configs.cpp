// brainscape_bench_configs: the bench images' configurations (firmware/bench/Configs.h) rendered
// on the host, one line per configuration: the SHA-256 of its output, which the bench image's
// line for the same configuration must equal (docs/design/cpu-budget.md §8.3, G1), the engine's
// counts over the render, and the shadow ledger's charge for its worst block
// (firmware/bench/ShadowLedger.h), the bound the Seed's DWT is held to (G3).
//
//   brainscape_bench_configs [quick] [blocks|stages|births|tail|restart|stress|post|events|all]
//   brainscape_bench_configs --check     (ctest firmware_bench_configs: every configuration builds,
//                                         loads exactly and renders 24 blocks)
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "bench/Configs.h"
#include "bench/ShadowLedger.h"
#include "brainscape/Engine.h"
#include "brainscape/HostArenas.h"

using namespace brainscape;
using namespace brainscape::fw::bench;

namespace {

struct Ledger {
  Engine*            engine = nullptr;
  Engine::GrainStats before{};
  uint64_t           worst = 0;  // the ledger's largest block
  uint32_t           worstBlock = 0;
  uint32_t           limit = 0;  // --check: blocks to render
};

void RunBlock(void* user, uint32_t b, const Engine::ProcessContext& ctx) {
  auto* l = static_cast<Ledger*>(user);
  if (l->limit != 0 && b >= l->limit) return;
  l->engine->Process(ctx);
  const Engine::GrainStats now = l->engine->Stats();
  const uint64_t           c   = LedgerCycles(StatsDelta(l->before, now), kBlock);
  l->before                    = now;
  if (c > l->worst) {
    l->worst      = c;
    l->worstBlock = b;
  }
}

std::string Line(Bench& bench, const Spec& s, uint32_t limit, bool* ok) {
  Ledger l;
  l.engine = &bench.engine();
  l.limit  = limit;
  const Engine::GrainStats st0 = bench.engine().Stats();
  l.before                     = st0;
  const Bench::Rendered r      = bench.Render(s, RunBlock, &l);
  const Engine::GrainStats d   = StatsDelta(st0, bench.engine().Stats());
  if (!r.loaded || r.eventsRefused != 0) *ok = false;
  char buf[320];
  std::snprintf(buf, sizeof buf,
                " blocks=%u births=%llu steals=%llu bursts=%llu passes=%llu skips=%llu ledgerMax=%llu@%u%s%s  ",
                r.blocks, static_cast<unsigned long long>(d.births), static_cast<unsigned long long>(d.steals),
                static_cast<unsigned long long>(d.burstBirths), static_cast<unsigned long long>(d.repeatPasses),
                static_cast<unsigned long long>(d.skips), static_cast<unsigned long long>(l.worst), l.worstBlock,
                r.loaded ? "" : " NOT-EXACT", r.eventsRefused != 0 ? " EVENTS-REFUSED" : "");
  return r.hash + buf + s.config;
}

}  // namespace

int main(int argc, char** argv) {
  bool        quick = false, check = false;
  std::string suite = "all";
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "quick") == 0) {
      quick = true;
    } else if (std::strcmp(argv[i], "--check") == 0) {
      check = quick = true;
    } else {
      suite = argv[i];
    }
  }
  EngineConfig ec;
  ec.maxBlockSize = kBlock;  // the pedal's engine, as the bench image runs it
  host::HeapArenas arenas(PlanMemory(ec));
  auto             engine = std::make_unique<Engine>();
  if (!arenas.ok() || !engine->Init(ec, arenas.get())) {
    std::printf("Engine::Init failed\n");
    return 1;
  }
  Bench       bench;
  const char* why = "";
  if (!bench.Setup(engine.get(), &why)) {
    std::printf("Setup failed: %s\n", why);
    return 1;
  }
  std::vector<std::pair<const char*, std::vector<Spec>>> suites;
  const auto want = [&](const char* name) { return suite == "all" || suite == name; };
  if (want("blocks")) suites.push_back({"blocks", bench.Blocks(quick)});
  if (want("stages")) suites.push_back({"stages", bench.Stages(quick)});
  if (want("births")) suites.push_back({"births", bench.Births(quick)});
  if (want("tail") && !quick) suites.push_back({"tail", bench.Tail()});
  if (want("restart")) suites.push_back({"restart", {bench.RestartRender()}});
  if (want("stress")) suites.push_back({"stress", bench.Stress(quick)});
  if (want("post")) suites.push_back({"post", bench.Post(quick)});
  if (want("events")) suites.push_back({"events", bench.EventCosts(quick)});
  bool ok = !suites.empty();
  for (const auto& entry : suites) {
    for (const Spec& s : entry.second) {
      std::printf("%s %s\n", entry.first, Line(bench, s, check ? 24u : 0u, &ok).c_str());
      std::fflush(stdout);
    }
  }
  if (check) std::printf("%s\n", ok ? "bench configurations: ok" : "bench configurations: FAILED");
  return ok ? 0 : 1;
}
