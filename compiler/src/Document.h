#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "Json.h"
#include "brainscape/Mode.h"
#include "brainscape/Params.h"
#include "brainscape/Preset.h"
#include "brainscape/PresetState.h"

// The preset document (docs/design/mode-compiler.md §2): schema 1 read into its decoded form,
// and written back in canonical form (§6.4). The schema itself is one table in code,
// Schema.cpp's Visit functions, walked by a reader and a writer, so the keys, their order,
// types, ranges, defaults and waves are written once.
namespace bsc {

// ── Findings (§2.7) ──────────────────────────────────────────────────────────────────────
// Errors E1-E12 stop compilation; lint findings L1-L9 never do, except that `lint --factory`
// makes L4 and L7-L9 errors. X1 marks an internal inconsistency (a compiler bug). As built:
//   E1  strict JSON (Json.h)              E7   caps (layers, pitch entries, tags, macros ...)
//   E2  unknown key, newer schema         E8   references, and a leaf of an absent element
//   E3  JSON type, integer form, a        E9   macro target curve, in_range, range ends;
//       required key missing                   expression ends and curve
//   E4  a value outside its range: leaves, E10 a written shape macro without targets
//       structure, string lengths and     E11 two layers' slot shares and voices
//       characters, binary32 overflow     E12 MODE within 4 KiB, the package within 16 KiB
//   E5  vocabulary, duplicates in a set
//   E6  defined but unsupported here, naming the feature and the wave that plays it
// bspc's package checks add V1 (the package does not decode or validate), V2 (its JSON section
// does not compile to it), V3 (decompile cannot say it in schema 1) and M1 (session migration).
struct Location {
  std::string pointer;  // RFC 6901 JSON pointer into the document
  uint32_t    line   = 0;
  uint32_t    column = 0;
};

struct Finding {
  std::string code;  // "E1".."E12", "L1".."L9", "X1"
  bool        error = true;
  Location    at;
  std::string message;
};

// "file:line:column: error E4 at /layers/0/size_ms: message".
std::string Describe(const Finding& f, std::string_view file);
bool        HasErrors(const std::vector<Finding>& findings);

// ── The document ─────────────────────────────────────────────────────────────────────────
// Every field defaulted, every value canonical. `state` holds the leaves (every Leaf row of a
// present element, ascending, as STAT stores them), the mode, CTRL and the performance state;
// the rest is META, the stamp and editor data.
struct Document {
  uint32_t                 schemaVersion = brainscape::kSchemaVersion;
  std::string              id, name;
  brainscape::PresetFamily family   = brainscape::PresetFamily::None;
  bool                     stamped  = false;  // sound_rev and sound_hash are written
  uint32_t                 soundRev = 0;
  brainscape::Digest32     soundHash;
  std::string              author, description;
  std::vector<std::string> tags;
  std::string              displayName[brainscape::kMaxMacros];  // by macro id - 69; "" = none
  std::unique_ptr<brainscape::PresetState> state;                // never null
  // editor (§2.2): validated, kept, never compiled.
  bool                  hasRatioGen = false;
  json::Value           ratioGen;
  std::vector<uint32_t> detached;  // leaf ids, ascending

  // What reading found, for lint and the error messages (not part of the document's content).
  std::map<std::string, Location> where;  // "leaf:<id>", "macro:<id>", "target:<index>", ...
  std::vector<Finding>            notes;  // L1 (a subnormal was written)
  std::vector<uint32_t>           omittedPositions;  // macro ids compiled at 0.5 (L4)

  Document();
  Document(const Document& other);
  Document& operator=(const Document& other);
  Document(Document&&) noexcept            = default;
  Document& operator=(Document&&) noexcept = default;

  // The stored value of leaf `id` (bits), or its default when the state does not hold it.
  uint32_t        LeafBits(uint32_t id) const;
  void            SetLeafBits(uint32_t id, uint32_t bits);  // a leaf the state holds
  const Location* Where(const std::string& key) const;
};

// ── Reading and writing ──────────────────────────────────────────────────────────────────
struct ReadOptions {
  // The mode features this build plays (Mode.h); tests widen it to exercise later waves'
  // vocabulary. Leaves of Reserved rows stay unsupported whatever this says.
  uint32_t supportedFeatures = brainscape::kSupportedModeFeatures;
};

// Steps 2-4 of §8.2 on a parsed document: keys, types and version (E2, E3, E5), defaults,
// numbers through the exact reader, canonicalized and range-checked (E4), support (E6), caps
// (E7), references (E8), macro targets (E9), the shape macro (E10) and the layer budget (E11).
// Returns false with the errors in *findings (which also receives nothing else).
bool ReadDocument(const json::Value& root, const ReadOptions& options, Document* out,
                  std::vector<Finding>* findings);
// Step 1 (E1) and the rest.
bool ReadDocumentText(std::string_view text, const ReadOptions& options, Document* out,
                      std::vector<Finding>* findings);

// The canonical content of `doc` (§6.4): every Leaf row of a present element, structure fields
// that differ from their defaults, the core (identity, sources, each layer's position source and
// pitch set, the six performance macros, macro positions), META, the stamp when there is one,
// editor data unless `withEditor` is false.
json::Value WriteDocument(const Document& doc, bool withEditor = true);
std::string FormatDocument(const Document& doc, bool withEditor = true);

// ── Schema facts shared with lint, derive and the tests ──────────────────────────────────
// Whether leaf `id`'s element exists in `mode` (§1.3): the second layer, a layer's SVF or
// crush, a modulator, the step table; every other leaf's always does.
bool ElementPresent(const brainscape::ModeBlob& mode, uint32_t id);
// The row a schema-1 leaf name names (a Leaf or Reserved row the document addresses by its
// path), or null. Rows 27 and 28 are structure in schema 1 (scheduler.sources lists onset; a
// layer's position.source), so their names are not leaves of the document.
const brainscape::ParamDescriptor* SchemaLeaf(std::string_view name);
bool                               IsSchemaLeaf(uint32_t id);
// The document path of a leaf name ("layer0.size_ms" -> "layers[0].size_ms"), and its JSON
// pointer ("/layers/0/size_ms").
std::string LeafPath(std::string_view name);
std::string LeafPointer(std::string_view name);
// The wave that makes a Reserved row a Leaf ("W1", "W2", "W3"), for E6.
const char* LeafWave(uint32_t id);
// The macro ids' names: "activity" .. "aux2" for 69..76; null for other ids.
const char* MacroName(uint32_t id);
uint32_t    MacroIdByName(std::string_view name);  // 0 if none
// Leaves held as structure in schema 1 and their value from the structure (rows 27, 28 while
// they are Leaf rows, until sound revision 2 retires them).
bool     StructureLeaf(uint32_t id);
uint32_t StructureLeafBits(const brainscape::ModeBlob& mode, uint32_t id);

// Floats as bits (no floating-point arithmetic in the compiler, §1.4 principle 3).
uint32_t BitsOf(const float& f);
float    FloatOf(uint32_t bits);
// An unsigned integer that orders finite binary32 values as their values do.
uint32_t OrderKey(uint32_t bits);
bool     LessBits(uint32_t a, uint32_t b);
// Text of a canonical float, as canonical JSON writes it.
std::string NumberText(uint32_t bits);

}  // namespace bsc
