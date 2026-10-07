// The parity stream of the firmware's parity image (ParityStream.h), on the host and under
// qemu-arm: the same harness code that the Daisy Seed runs, printing JSON lines that
// tools/hil/parity_check.py compares with golden.json.
//
//   brainscape_parity_stream [--only VECTOR[/PRESET],...] [--quick] [--max-block 48|512]
//                            [--fp-env clean|hostile] [--tag TAG] [--out FILE]
//
// Exit status: 0 when every selected preset rendered, 1 when one did not, 3 on bad
// arguments or output. Whether the hashes match is parity_check.py's verdict.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if !defined(__arm__)
#include <chrono>
#endif

#include "ParityStream.h"
#include "Render.h"

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
  for (int i = 1; i < argc; ++i) {
    const char* a    = argv[i];
    const bool  more = i + 1 < argc;
    if (!std::strcmp(a, "--only") && more) {
      o.only = Split(argv[++i]);
    } else if (!std::strcmp(a, "--quick")) {
      o.quick = true;
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
                   "       [--max-block 48|512] [--fp-env clean|hostile] [--tag TAG] [--out FILE]\n");
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
  Renderer renderer(rc);
  if (!renderer.ok()) {
    std::fprintf(stderr, "cannot allocate the engine arenas\n");
    return 3;
  }
  FileSink           sink(f);
  const StreamResult r = StreamCorpus(renderer, o, sink);
  if (f != stdout) std::fclose(f);
  std::fprintf(stderr, "# parity stream: %d presets, %d render failures\n", r.presets,
               r.renderFailures);
  return r.renderFailures > 0 ? 1 : 0;
}
