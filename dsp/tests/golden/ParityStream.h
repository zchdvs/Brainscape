#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "JsonLine.h"
#include "Render.h"

// The golden corpus streamed as JSON lines (docs/design/determinism-profile.md §6.6, the
// hardware-in-the-loop check): the same vectors, presets, event scripts and renders as
// brainscape_golden, each preset's whole-render SHA-256, per-second hashes and counters on
// one line as soon as it is rendered, for a target that cannot write a report file. The
// firmware's parity image streams it over USB serial, brainscape_parity_stream prints it on
// the host and under qemu-arm, and tools/hil/parity_check.py compares it with golden.json.
//
// Format brainscape-parity-stream/1, one JSON object per line, integers and strings only:
//   {"type":"parity-begin", ...header: the golden file's header fields, the run's
//    configuration, the harness and engine toolchains, the clock, the platform...}
//   {"type":"vector", "name", "source", "generatorVersion", "frames", "inputHash",
//    "ringSizes", "notes"}
//   {"type":"preset", "vector", "name", "rendered", "hash", "secondHashes", "counters",
//    "frames", "cycles"}
//   {"type":"parity-end", "presets", "renderFailures", "cycles"}
// "cycles" is 0 when the clock is "none".
namespace brainscape::golden {

class LineSink {
 public:
  virtual ~LineSink() = default;
  // One complete JSON object, without its newline.
  virtual void Line(const std::string& json) = 0;
};

struct StreamOptions {
  // The vectors and presets to render: empty for all, else VECTOR or VECTOR/PRESET names.
  std::vector<std::string> only;
  bool                     quick = false;  // skip the long vectors, as --quick does
  std::string              tag   = "local";
  // A free-running cycle counter and its rate, or none.
  uint64_t (*clock)()   = nullptr;
  uint64_t    clockHz   = 0;
  std::string clockName = "none";
  // Raw JSON object text describing the platform (board, build, memory), or empty.
  std::string platformJson;
};

struct StreamResult {
  int presets        = 0;
  int renderFailures = 0;
};

// Renders every selected preset of the corpus with `renderer` (whose configuration the
// header reports) and writes the stream to `sink`.
StreamResult StreamCorpus(Renderer& renderer, const StreamOptions& options, LineSink& sink);

}  // namespace brainscape::golden
