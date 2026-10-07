// The parity stream of the firmware's parity image (ParityStream.h), on the host and under
// qemu-arm: the same harness code that the Daisy Seed runs, printing JSON lines that
// tools/hil/parity_check.py compares with golden.json.
//
//   brainscape_parity_stream [--only VECTOR[/PRESET],...] [--quick] [--max-block 48|512]
//                            [--fp-env clean|hostile] [--placement] [--tag TAG] [--out FILE]
//
// --placement renders the way the firmware's parity image does (firmware/parity/main.cpp):
// the engine arenas and the Engine object in caller-owned storage of the firmware's sizes
// and alignments (firmware/platform/Placement.h), through Renderer(cfg, Placement), with a
// first Renderer rendering the shortest vector's first preset into the same storage and
// destroyed before the streamed one is built, as two "run" commands on the device are.
//
// Exit status: 0 when every selected preset rendered, 1 when one did not, 3 on bad
// arguments or output. Whether the hashes match is parity_check.py's verdict.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#if !defined(__arm__)
#include <chrono>
#endif

#include "Corpus.h"
#include "ParityStream.h"
#include "Render.h"
#include "platform/Placement.h"  // firmware/: the parity image's arena sizes

using namespace brainscape::golden;

namespace {

class FileSink final : public LineSink {
 public:
  explicit FileSink(FILE* f) : f_(f) {}
  void Line(const std::string& json) override {
    std::fwrite(json.data(), 1, json.size(), f_);
    std::fputc('\n', f_);
    std::fflush(f_);
  }

 private:
  FILE* f_;
};

#if !defined(__arm__)
uint64_t SteadyNs() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count());
}
#endif

class NullSink final : public LineSink {
 public:
  void Line(const std::string&) override {}
};

// Caller-owned storage of `bytes` at `align`, as the firmware's static arrays are.
struct Block {
  std::vector<unsigned char> raw;
  void*                      at = nullptr;
  Block(size_t bytes, size_t align) : raw(bytes + align) {
    const auto p = reinterpret_cast<uintptr_t>(raw.data());
    at           = raw.data() + ((align - p % align) % align);
  }
};

// The firmware's placement (firmware/platform/Placement.cpp), on the heap.
struct FirmwareStorage {
  Block hot{brainscape::fw::kHotArenaBytes, 32};
  Block warm{brainscape::fw::kWarmArenaBytes, 32};
  Block bulk{brainscape::fw::kBulkArenaBytes, 32};
  Block engine{brainscape::fw::kEngineSlotBytes, brainscape::fw::kEngineSlotAlign};
  Renderer::Placement Get() {
    Renderer::Placement p;
    p.arenas.base[0]  = hot.at;
    p.arenas.bytes[0] = brainscape::fw::kHotArenaBytes;
    p.arenas.base[1]  = warm.at;
    p.arenas.bytes[1] = brainscape::fw::kWarmArenaBytes;
    p.arenas.base[2]  = bulk.at;
    p.arenas.bytes[2] = brainscape::fw::kBulkArenaBytes;
    p.engine          = engine.at;
    return p;
  }
};

std::vector<std::string> Split(const char* s) {
  std::vector<std::string> out;
  std::string              cur;
  for (; *s != '\0'; ++s) {
    if (*s == ',') {
      out.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(*s);
    }
  }
  out.push_back(cur);
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  StreamOptions o;
  RenderConfig  rc;  // the canonical run: 48-frame blocks, 2^22 ring, engine events
  std::string   out;
  bool          placement = false;
  for (int i = 1; i < argc; ++i) {
    const char* a    = argv[i];
    const bool  more = i + 1 < argc;
    if (!std::strcmp(a, "--only") && more) {
      o.only = Split(argv[++i]);
    } else if (!std::strcmp(a, "--quick")) {
      o.quick = true;
    } else if (!std::strcmp(a, "--placement")) {
      placement = true;
    } else if (!std::strcmp(a, "--tag") && more) {
      o.tag = argv[++i];
    } else if (!std::strcmp(a, "--out") && more) {
      out = argv[++i];
    } else if (!std::strcmp(a, "--max-block") && more) {
      rc.maxBlockSize = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
    } else if (!std::strcmp(a, "--fp-env") && more && !std::strcmp(argv[i + 1], "hostile")) {
      rc.fpEnv = FpEnv::Hostile;
      ++i;
    } else if (!std::strcmp(a, "--fp-env") && more && !std::strcmp(argv[i + 1], "clean")) {
      rc.fpEnv = FpEnv::Clean;
      ++i;
    } else {
      std::fprintf(stderr,
                   "usage: brainscape_parity_stream [--only VECTOR[/PRESET],...] [--quick]\n"
                   "       [--max-block 48|512] [--fp-env clean|hostile] [--placement] [--tag TAG]\n"
                   "       [--out FILE]\n");
      return 3;
    }
  }
  if (rc.maxBlockSize < 48 || rc.maxBlockSize > 512) {
    std::fprintf(stderr, "--max-block must be 48..512\n");
    return 3;
  }
#if defined(__arm__)
  o.platformJson = "{\"name\":\"qemu-arm user mode\",\"cpu\":\"cortex-m7 (QEMU_CPU or -cpu)\"}";
#else
  o.clock        = SteadyNs;
  o.clockHz      = 1000000000u;
  o.clockName    = "steady_clock_ns";
  o.platformJson = "{\"name\":\"host\"}";
#endif
  FILE* f = stdout;
  if (!out.empty() && (f = std::fopen(out.c_str(), "wb")) == nullptr) {
    std::fprintf(stderr, "cannot write %s\n", out.c_str());
    return 3;
  }
  std::unique_ptr<FirmwareStorage> storage;
  std::unique_ptr<Renderer>        renderer;
  if (placement) {
    o.platformJson.pop_back();
    o.platformJson += ",\"placement\":\"firmware arena sizes, Renderer(cfg, Placement), second renderer\"}";
    storage = std::make_unique<FirmwareStorage>();
    {
      // A first "run" over the same storage: the shortest vector's first preset.
      const std::vector<VectorCase> corpus   = BuildCorpus();
      const VectorCase*             shortest = &corpus.front();
      for (const VectorCase& v : corpus) {
        if (v.frames < shortest->frames) shortest = &v;
      }
      StreamOptions first = o;
      first.only          = {std::string(shortest->name) + "/" + shortest->presets.front().name};
      Renderer warmup(rc, storage->Get());
      NullSink none;
      if (!warmup.ok() || StreamCorpus(warmup, first, none).renderFailures != 0) {
        std::fprintf(stderr, "the first renderer on the firmware placement failed\n");
        return 3;
      }
    }
    renderer = std::make_unique<Renderer>(rc, storage->Get());
  } else {
    renderer = std::make_unique<Renderer>(rc);
  }
  if (!renderer->ok()) {
    std::fprintf(stderr, "cannot allocate the engine arenas\n");
    return 3;
  }
  FileSink           sink(f);
  const StreamResult r = StreamCorpus(*renderer, o, sink);
  if (f != stdout) std::fclose(f);
  std::fprintf(stderr, "# parity stream: %d presets, %d render failures\n", r.presets,
               r.renderFailures);
  return r.renderFailures > 0 ? 1 : 0;
}
