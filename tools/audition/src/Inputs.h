#pragma once
#include <cstdint>
#include <string>

#include "Render.h"
#include "brainscape/TestSignal.h"

// The audition inputs (docs/design/mode-compiler.md §11.3): the shared test-signal vectors
// (dsp/include/brainscape/TestSignal.h), integer-generated, so every machine renders the same
// input bits, each with a silent tail; and the same vectors looped for the macro sweeps. DI clips
// join later, outside git and keyed by their hashes (Q9).
namespace bsa {

using brainscape::testsignal::Vector;

inline constexpr uint32_t kSignalFrames = 10 * kRate;  // each vector sounds for 10 s
inline constexpr uint32_t kTailFrames   = 10 * kRate;  // then 10 s of silence

struct Input {
  std::string name;              // the vector's name: "plucks", "soft_notes"
  std::string description;       // for the recipe
  Stereo      audio;             // 48 kHz, on the 24-bit grid
  uint32_t    signalFrames = 0;  // frames [0, signalFrames) may sound; the levels' span
};

// The vector over `signalFrames` (its notes start and end inside them), then `tailFrames` of
// silence.
Input VectorInput(Vector v, uint32_t signalFrames = kSignalFrames, uint32_t tailFrames = kTailFrames);
// The vector's `signalFrames` repeated end to end for `frames` frames (a loop never cuts a note:
// every note ends inside the vector's span), then `tailFrames` of silence. signalFrames of the
// result is `frames`.
Input LoopedInput(Vector v, uint32_t frames, uint32_t signalFrames = kSignalFrames,
                  uint32_t tailFrames = 0);

// Every vector, in TestSignal.h's order, and its name; false for an unknown name.
inline constexpr Vector kVectors[] = {Vector::Plucks,      Vector::Strums,     Vector::SoftNotes,
                                      Vector::OnsetBursts, Vector::Saturation, Vector::Silence};
bool VectorByName(const std::string& name, Vector* out);  // the name: testsignal::VectorName

}  // namespace bsa
