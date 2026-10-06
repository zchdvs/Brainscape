#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "Corpus.h"
#include "brainscape/Memory.h"

// Renders one (vector, preset) pair through the current public Engine API from the
// exact-restart state, hashing the output as it goes (docs/design/determinism-
// profile.md §6.1: SHA-256 of the interleaved little-endian float32 output, plus one
// per second) and counting coverage.
namespace brainscape::golden {

struct RenderConfig {
  std::vector<uint32_t> blockPattern{48};  // grid from frame 0, repeated; each <= 512
  uint32_t              historyLog2 = 22;  // 2^22 is the canonical ring (profile §2.3)
};

struct RenderOutput {
  std::string              hash;
  std::vector<std::string> secondHashes;
  int64_t                  counters[static_cast<size_t>(Counter::kCount)] = {};
  int64_t                  firstOnsetBlock = -1;  // block start, so only block-accurate
};

// The output samples, kept only to write a WAV for a preset that misses its golden.
struct Capture {
  std::vector<float> l, r;
};

class Renderer {
 public:
  explicit Renderer(const RenderConfig& cfg);
  ~Renderer();
  Renderer(const Renderer&) = delete;
  Renderer& operator=(const Renderer&) = delete;

  bool ok() const { return ok_; }
  uint32_t HistoryFrames() const { return 1u << cfg_.historyLog2; }

  bool Render(const VectorCase& v, const std::vector<testsignal::Note>& notes,
              const PresetCase& p, RenderOutput* out, Capture* capture = nullptr);

 private:
  RenderConfig cfg_;
  Arenas       arenas_{};
  void*        raw_[kNumTiers] = {};
  bool         ok_ = false;
};

// SHA-256 of the vector's input, interleaved little-endian float32.
std::string InputHash(const VectorCase& v, const std::vector<testsignal::Note>& notes);

std::vector<testsignal::Note> VectorNotes(const VectorCase& v);

}  // namespace brainscape::golden
