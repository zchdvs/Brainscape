// The tempo core on every leg (docs/design/clock.md §8.1, §8.2): fixed seeded streams through
// TempoCore at two block patterns and through the reference model, which must agree, and a digest
// of TempoCore's outputs (hits, states, snapshots, counters), of the integer helpers on random
// operands and of the MIDI translator on a random byte stream. Every leg, the emulated Cortex-M7
// with its 32-bit long and soft 64-bit division included, must reproduce kTempoDigest: the tempo
// core is integer-only, so any difference is a bug. No test framework, so qemu-arm runs it.
//
//   tempo_tool --check     exit 1 on a mismatch with the reference or the committed digest
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#if defined(_MSC_VER)
#include <crtdbg.h>
#include <stdlib.h>
#endif

#include "TempoStreams.h"
#include "WideInt.h"
#include "brainscape/MidiClock.h"
#include "brainscape/Tempo.h"
#include "detail/IntMath.h"
#include "detail/Tempo.h"

using namespace brainscape;
using namespace brainscape::testing;

namespace {

// The committed digest every leg must reproduce (MSVC x64 Release, Debug and AVX2, GCC 11, Clang 14
// and the M7 under qemu do). A change to the tempo core's rules or to these streams changes it.
// The streams draw one random number per statement, since argument evaluation order differs
// between compilers (it did: GCC and MSVC on x64 against Clang and arm-none-eabi).
constexpr const char* kTempoDigest = "b5392cdd42931c59";

bool IntMathDigest(Fnv* f) {
  Rng r(0xA5A5u);
  bool ok = true;
  for (int i = 0; i < 200000; ++i) {
    // One draw per statement: the order of operands' evaluation must not depend on the compiler.
    uint64_t a = r.Next();
    a >>= r.Next() % 64;
    uint64_t b = r.Next();
    b >>= r.Next() % 64;
    uint64_t c = r.Next();
    c >>= r.Next() % 64;
    if (c == 0) c = 1;
    uint64_t want;
    if (RefMulDivRoundU64(a, b, c, &want)) {
      const uint64_t got = intmath::MulDivRoundU64(a, b, c);
      if (got != want) ok = false;
      f->U(got);
    }
    const int64_t sa = static_cast<int64_t>(a >> 1) * ((r.Next() & 1) ? 1 : -1);
    const int64_t sb = static_cast<int64_t>(b >> 33) * ((r.Next() & 1) ? 1 : -1);
    const int64_t sc = static_cast<int64_t>((c >> 1) | 1) * ((r.Next() & 1) ? 1 : -1);
    int64_t swant;
    if (RefMulDivRoundI64(sa, sb, sc, &swant)) {
      const int64_t sgot = intmath::MulDivRoundI64(sa, sb, sc);
      if (sgot != swant) ok = false;
      f->I(sgot);
    }
    f->I(intmath::FloorDivI64(sa, sc));
    f->I(intmath::CeilDivI64(sa, sc));
    f->I(intmath::FloorModI64(sa, sc));
  }
  for (uint32_t rate : {8000u, 44100u, 48000u, 384000u}) {
    for (uint32_t ns = tempo::kMinNsPerQuarter; ns <= tempo::kMaxNsPerQuarter; ns += 9999991u) {
      const uint64_t p = tempo::PFromNs(ns, rate);
      f->U(p);
      if (tempo::NsFromP(p, rate) != ns) ok = false;
      for (uint32_t t : {3u, 36u, 192u})
        for (int32_t oct = -3; oct <= 3; ++oct) f->U(tempo::DurationFrames(p, t, 24, oct));
    }
  }
  return ok;
}

void MidiDigest(Fnv* f) {
  Rng r(0x3Du);
  MidiClockParser p;
  const uint8_t common[] = {0xF8, 0xF8, 0xF8, 0xFA, 0xFB, 0xFC, 0xF2, 0x90, 0xB0, 0xC0,
                            0xD0, 0xF0, 0xF7, 0xFE, 0xF1, 0xF3};
  for (int i = 0; i < 100000; ++i) {
    const bool    pick = (r.Next() & 1) != 0;
    const uint64_t v = r.Next();
    const uint8_t b = pick ? common[v % 16] : static_cast<uint8_t>(v & 0x7F);
    MidiClockEvent e;
    const auto res = p.Feed(b, &e);
    f->U(static_cast<uint64_t>(res));
    if (res == MidiClockParser::Result::Event) {
      f->U(e.type);
      f->U(e.id);
      f->U(e.valueBits);
    }
    f->U(p.PositionKnown() ? p.NextTickPosition() : 0xFFFFFFFFu);
    f->U(p.Running());
    if (i % 25000 == 24999) p.Reset();
  }
}

void ResultDigest(const RunResult& r, Fnv* f) {
  f->U(r.hits.size());
  for (const GridHit& h : r.hits) {
    f->I(h.frame);
    f->I(h.position);
  }
  f->U(r.states.size());
  for (uint64_t s : r.states) f->U(s);
  for (const TempoInfo& i : r.probes) {
    f->I(i.position);
    f->U(i.nsPerQuarter);
    f->U(i.source);
    f->U(i.timeMode);
    f->U(i.subdiv);
    f->U(i.flags);
    f->I(i.lastGridFrame);
  }
  f->U(StatsDigest(r.stats));
  f->U(r.finalState);
}

bool Same(const RunResult& a, const RunResult& b) {
  if (!SameHits(a.hits, b.hits) || a.states != b.states || a.probes.size() != b.probes.size())
    return false;
  for (size_t i = 0; i < a.probes.size(); ++i)
    if (!SameInfo(a.probes[i], b.probes[i])) return false;
  return StatsDigest(a.stats) == StatsDigest(b.stats);
}

}  // namespace

int main(int argc, char** argv) {
#if defined(_MSC_VER)
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
  for (int type : {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT}) {
    _CrtSetReportMode(type, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(type, _CRTDBG_FILE_STDERR);
  }
#endif
  if (argc < 2 || std::strcmp(argv[1], "--check") != 0) {
    std::printf("usage: tempo_tool --check\n");
    return 2;
  }
  Fnv f;
  int failures = 0;
  if (!IntMathDigest(&f)) {
    std::printf("tempo: integer helpers differ from the 128-bit reference\n");
    ++failures;
  }
  MidiDigest(&f);
  const Family fams[] = {Family::Tempo, Family::Taps, Family::Clock, Family::Mixed};
  const char*  names[] = {"tempo", "taps", "clock", "mixed"};
  for (int fi = 0; fi < 4; ++fi) {
    for (uint64_t seed = 1; seed <= 3; ++seed) {
      const int      seconds = 30;
      const uint32_t rate = seed == 2 && fi == 3 ? 44100u : 48000u;
      const Stream   s = MakeStream(fams[fi], seed * 7919 + static_cast<uint64_t>(fi), seconds, rate);
      RunConfig cfg;
      cfg.rate = rate;
      const int64_t   end = static_cast<int64_t>(rate) * seconds;
      const RunResult a = RunCore(cfg, s.ev, end, Blocks::Const(48));
      const RunResult b = RunCore(cfg, s.ev, end, Blocks::Random(5));
      const RunResult ref = RunRef(cfg, s.ev, end);
      const bool ok = Same(a, ref) && Same(b, ref) && a.finalState == b.finalState &&
                      a.maxHitsPerSpan <= TempoCore::kMaxClockPerSpan &&
                      b.maxHitsPerSpan <= TempoCore::kMaxClockPerSpan;
      std::printf("tempo %-5s seed %llu: %zu events, %zu hits, %llu ticks: %s\n", names[fi],
                  static_cast<unsigned long long>(seed), s.ev.size(), a.hits.size(),
                  static_cast<unsigned long long>(a.stats.ticks), ok ? "matches the reference" : "MISMATCH");
      if (!ok) ++failures;
      ResultDigest(a, &f);
    }
  }
  if (I128::Overflow()) {
    std::printf("tempo: the reference overflowed 128 bits\n");
    ++failures;
  }
  char digest[17];
  std::snprintf(digest, sizeof digest, "%016llx", static_cast<unsigned long long>(f.h));
  const bool same = std::strcmp(digest, kTempoDigest) == 0;
  std::printf("tempo digest %s; committed %s: %s\n", digest, kTempoDigest, same ? "match" : "MISMATCH");
  if (!same) ++failures;
  return failures == 0 ? 0 : 1;
}
