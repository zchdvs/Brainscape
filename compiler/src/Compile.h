#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "Document.h"
#include "brainscape/Preset.h"

// The compiler (docs/design/mode-compiler.md §8.2): a preset document to a .bsp package, a
// pure function of the JSON and this build's constants (§8.3), in integers only. Also the
// reverse (decompile), the canonical form (fmt), the stamp, verification of packages and the
// first difference between two.
namespace bsc {

using Bytes = std::vector<uint8_t>;

struct CompileOptions {
  ReadOptions read;
  // Step 8's checks of the package written: DecodePreset and ValidateMode by default. Tests
  // that widen read.supportedFeatures pass the dsp tests' DecodePresetWith/ValidateModeWith
  // bound to the same features.
  bool (*decode)(const void* bytes, size_t length, brainscape::PresetState* out,
                 brainscape::PresetDiagnostic* d, brainscape::PackageInfo* info,
                 brainscape::PresetMeta* meta, uint32_t supported) = nullptr;
  bool (*validate)(const brainscape::PresetState& state, uint32_t supported,
                   brainscape::PresetDiagnostic* d)                = nullptr;
};

struct CompileResult {
  bool                 ok = false;
  Bytes                package;  // the .bsp bytes
  std::string          json;     // its JSON section: the formatted, stamped document
  Document             doc;      // the document as read
  brainscape::Digest32 soundHash, controlHash, packageHash;
  std::vector<Finding> findings;  // errors (E1-E12, X1)
};

// §8.2's eight steps: parse (E1); keys, types, version, defaults (E2, E3, E5); numbers (E4);
// support, caps and references (E6-E11); the decoded state and META; the encoding, whose JSON
// section is the formatted document stamped with this build's sound_rev and the package's
// sound_hash; the hashes; and decoding and validating the result, which must give the same
// state. The header's FACTORY flag is set for ids under "factory." (§6.1; a pure function of
// the document).
CompileResult Compile(std::string_view text, const CompileOptions& options = {});
CompileResult CompileDocument(const Document& doc, const CompileOptions& options = {});

// The canonical form of a document (§6.4): Fmt(J). The stamp is kept as written.
bool FormatText(std::string_view text, std::string* out, std::vector<Finding>* findings,
                const ReadOptions& options = {});

// A decoded package, as DecodePreset gives it.
struct DecodedPackage {
  bool                                     ok = false;
  brainscape::PresetDiagnostic             diagnostic;
  std::unique_ptr<brainscape::PresetState> state;
  brainscape::PackageInfo                  info;
  brainscape::PresetMeta                   meta;
  std::string                              json;  // the JSON section, if any
  bool                                     hasJson = false;
};
DecodedPackage DecodePackage(const uint8_t* bytes, size_t length,
                             const CompileOptions& options = {});
std::string    DescribeDiagnostic(const brainscape::PresetDiagnostic& d);

// The document a package's STAT, MODE, CTRL and META describe, editor data lost (§8.2
// decompile): stamped with the header's sound_rev and sound_hash. False, with findings, when
// the package holds what schema 1 cannot say (a newer schema, no META, a leaf the document has
// no key for, rows 27 and 28 disagreeing with the structure).
bool DocumentFromPackage(const uint8_t* bytes, const DecodedPackage& p, Document* out,
                         std::vector<Finding>* findings);

// `decompile`: the JSON section, or the document rebuilt from the package when it has none,
// its JSON_STALE flag is set, or `rebuild` asks.
struct DecompileResult {
  bool                 ok      = false;
  bool                 rebuilt = false;
  std::string          json;
  std::vector<Finding> findings;
};
DecompileResult Decompile(const uint8_t* bytes, size_t length, bool rebuild = false,
                          const CompileOptions& options = {});

// `verify` (§8.2): the package decodes and validates, and its JSON section (unless stale)
// compiles to the same STAT and MODE. Findings V1 (the package), V2 (its JSON), notes for
// CTRL, META and header differences.
std::vector<Finding> Verify(const uint8_t* bytes, size_t length,
                            const CompileOptions& options = {});

// `diff`: the first differing field of two packages, by name ("" when they are identical).
std::string Diff(const uint8_t* a, size_t aLength, const uint8_t* b, size_t bLength,
                 const CompileOptions& options = {});

// SHA-256 of bytes, as lowercase hex.
std::string Sha256Hex(const uint8_t* bytes, size_t length);

}  // namespace bsc
