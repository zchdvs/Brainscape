#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "brainscape/Params.h"
#include "brainscape/Preset.h"

// Packages for the package tests (test_blob.cpp), the deterministic mutation fuzzer and the
// frozen fixtures (blob_tool.cpp, host and Cortex-M7), and the libFuzzer harness: built with the
// encoder, never by assigning bytes (docs/design/mode-compiler.md §4.4, §10.2, §10.3).
namespace brainscape::blobtest {

using Bytes = std::vector<uint8_t>;

uint32_t Bits(float v);
float    FromBits(uint32_t bits);
std::string Hex(const uint8_t* bytes, size_t length);

// A default state (never a stack local: 2.6 KiB), with every Leaf row of this build at its
// default, ascending, and soundRev = kSoundRevision.
std::unique_ptr<PresetState> CompleteState();
// Sets a stored leaf's value (it must be one of the state's leaves).
void SetLeaf(PresetState& s, ParamId id, float value);
// Inserts or replaces a raw leaf, keeping ids ascending.
void PutLeaf(PresetState& s, uint32_t id, float value);

// META for `mode`: the given id, name and family, an author, a description, tags and a display
// name per defined macro (as many as `displayNames`).
Bytes MetaFor(const ModeBlob& mode, const char* id, const char* name, PresetFamily family,
              uint32_t tags, uint32_t displayNames);

// A package for `state`; aborts (REQUIRE in the tests) on failure via the returned empty vector.
Bytes Package(const PresetState& state, const Bytes* meta = nullptr, const Bytes* json = nullptr,
              uint16_t flags = 0, uint32_t schemaVersion = kSchemaVersion,
              PresetDiagnostic* diagnostic = nullptr);

// Section and chunk surgery on encoded packages, then the hashes fixed so the edit is what the
// decoder sees.
uint32_t Rd32(const uint8_t* p);
void     Wr32(uint8_t* p, uint32_t v);
// Offset of the first section with `tag` (its header), or 0.
size_t FindSection(const Bytes& b, uint32_t tag);
// Offset of MODE's chunk with `tag` (its header), or 0.
size_t FindChunk(const Bytes& b, uint32_t tag);
// Inserts a section (tag, payload, zero padding) at byte offset `at` (a section boundary).
void InsertSection(Bytes& b, size_t at, uint32_t tag, const Bytes& payload);
// Inserts a MODE chunk at byte offset `at` (a chunk boundary inside MODE) and fixes MODE's
// length and chunk count.
void InsertChunk(Bytes& b, size_t at, uint32_t tag, const Bytes& payload);
// total_bytes, section_count when `sections` > 0, sound_hash, control_hash and package_hash,
// from the bytes as they are (leniently: what cannot be found is left alone).
void Rehash(Bytes& b, uint32_t sections = 0);

// Every chunk at its cap: two layers, full pitch sets, 16 steps, two modulators, 8 routes, 4
// links, a dry duck, 8 macros with 32 targets (every one a Leaf row of this build). Its MODE is
// the largest, 1,528 bytes (§6.2). Valid under every feature (ValidateMode with them all).
void FullMode(ModeBlob* mode, PresetState* state);

// The fuzzers' seed packages: the default mode; custom macros, CTRL with expression, META,
// JSON and unknown sections; flags, no CTRL; and the full vocabulary (features this build lacks).
std::vector<Bytes> Seeds();

// The deterministic mutation fuzzer (§10.2): mutates the seeds with a SplitMix64 stream, one
// iteration in four structurally (sections and MODE chunks repeated, reordered, spliced or at
// their defaults, a META tag repeated, a hash broken, a package or MODE past its cap) with the
// hashes fixed, the rest byte by byte with the hashes fixed three times in four. It decodes with
// this build's features and with every feature, requires every accepted package to re-encode to
// its own bytes, and runs ValidateMode on what each accepts. The digest covers every verdict,
// the validator's included, so host and M7 must agree on all of them.
struct FuzzResult {
  uint64_t iterations          = 0;
  uint64_t structural          = 0;  // iterations mutated structurally
  uint64_t accepted            = 0;  // by DecodePreset
  uint64_t acceptedAll         = 0;  // with every feature
  uint64_t reencodeMismatches  = 0;
  uint64_t validated           = 0;  // accepted and ValidateMode-clean with every feature
  uint64_t validatedThisBuild  = 0;  // accepted and ValidateMode-clean on this build
  uint64_t histogram[static_cast<size_t>(PresetError::kCount)] = {};  // DecodePreset's verdicts
  // ValidateMode's verdicts (every feature) on what every feature accepts.
  uint64_t validateHistogram[static_cast<size_t>(PresetError::kCount)] = {};
  uint8_t  digest[32]          = {};
};
FuzzResult Fuzz(uint64_t iterations, uint64_t seed);
// The committed digest of Fuzz(kFuzzIterations, kFuzzSeed), on every leg.
inline constexpr uint64_t kFuzzIterations = 200000;
inline constexpr uint64_t kFuzzSeed       = 12345;
extern const char* const  kFuzzDigest;

// The frozen fixtures (§10.3) under dsp/tests/golden/presets/frozen/: never re-stamped. Each
// has the SHA-256 of its bytes and its verdict on this build.
struct Fixture {
  const char* file;
  const char* sha256;
  PresetError error;       // DecodePreset's verdict
  uint32_t    detail;      // its detail
  uint32_t    soundRev;    // when accepted
  bool        exactLoad;   // when accepted: CheckPreset's verdict on this build
  uint32_t    unknownSections;
  const char* why;
  PresetError validate;        // when accepted: ValidateMode's verdict on this build
  uint32_t    validateDetail;  // its detail
};
extern const Fixture kFixtures[];
extern const size_t  kFixtureCount;
// The bytes the fixture was made from (blob_tool --write-fixtures); the committed file is the
// reference, never this.
Bytes MakeFixture(size_t index);
// Checks one fixture's bytes; returns an empty string or what failed.
std::string CheckFixture(const Fixture& f, const Bytes& bytes);

}  // namespace brainscape::blobtest
