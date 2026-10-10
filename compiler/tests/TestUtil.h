#pragma once
// Helpers for the compiler's tests: documents built as JSON values, compile options that widen
// the support table to every wave (with the dsp tests' decoder and validator bound to it), and
// finding checks.
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "Compile.h"
#include "Document.h"
#include "Json.h"
#include "Number.h"
#include "blob/Blob.h"  // DecodePresetWith, ValidateModeWith (dsp/src, private to the tests)
#include "brainscape/Mode.h"

namespace bsctest {

namespace json = bsc::json;

inline uint32_t Bits(float v) {
  uint32_t u = 0;
  std::memcpy(&u, &v, sizeof u);
  return u;
}

inline float FromBits(uint32_t u) {
  float v = 0.f;
  std::memcpy(&v, &u, sizeof v);
  return v;
}

inline bool DecodeAll(const void* bytes, size_t length, brainscape::PresetState* out,
                      brainscape::PresetDiagnostic* d, brainscape::PackageInfo* info,
                      brainscape::PresetMeta* meta, uint32_t supported) {
  return brainscape::blob::DecodePresetWith(bytes, length, out, d, info, meta, supported);
}

inline bool ValidateAll(const brainscape::PresetState& s, uint32_t supported,
                        brainscape::PresetDiagnostic* d) {
  return brainscape::blob::ValidateModeWith(s, supported, d);
}

// Every wave's vocabulary supported (lanes C and F widen kSupportedModeFeatures; this reaches it
// now). Leaves of Reserved rows stay unsupported: their kind decides.
inline bsc::CompileOptions AllFeatures() {
  bsc::CompileOptions o;
  o.read.supportedFeatures = brainscape::kModeFeatureAll;
  o.read.globalReverse     = true;
  o.decode                 = DecodeAll;
  o.validate               = ValidateAll;
  return o;
}

inline json::Value Parse(const std::string& text) {
  json::Value      v;
  json::ParseError e;
  if (!json::Parse(text, &v, &e)) throw std::runtime_error("test JSON: " + e.message);
  return v;
}

// Sets `path` ("layers[0].position.base_ms", "global.mix") in `root`, creating objects and
// array entries on the way.
inline void Set(json::Value& root, const std::string& path, json::Value value) {
  json::Value* at = &root;
  size_t       i  = 0;
  for (;;) {
    size_t            end = path.find_first_of(".[", i);
    const std::string key = path.substr(i, end == std::string::npos ? std::string::npos : end - i);
    if (at->type != json::Type::Object) *at = json::Value::Object();
    json::Value* next = nullptr;
    for (json::Member& m : at->members) {
      if (m.key == key) next = &m.value;
    }
    at = next != nullptr ? next : &at->Add(key, json::Value::Null());
    while (end != std::string::npos && path[end] == '[') {
      const size_t close = path.find(']', end);
      const size_t index = static_cast<size_t>(std::stoul(path.substr(end + 1, close - end - 1)));
      if (at->type != json::Type::Array) *at = json::Value::Array();
      while (at->items.size() <= index) at->items.push_back(json::Value::Object());
      at  = &at->items[index];
      end = close + 1 < path.size() ? close + 1 : std::string::npos;
    }
    if (end == std::string::npos) break;
    i = end + 1;  // past the '.'
  }
  *at = std::move(value);
}

inline json::Value Num(const char* text) { return json::Value::Number(text); }
inline json::Value Str(const std::string& text) { return json::Value::String(text); }

inline std::string Minimal(const std::string& id = "test.doc") {
  return "{\"schema_version\": 1, \"id\": \"" + id + "\", \"name\": \"Test\"}";
}

inline std::vector<std::string> Codes(const std::vector<bsc::Finding>& findings) {
  std::vector<std::string> out;
  for (const bsc::Finding& f : findings) out.push_back(f.code);
  return out;
}

inline std::string All(const std::vector<bsc::Finding>& findings) {
  std::string out;
  for (const bsc::Finding& f : findings) out += bsc::Describe(f, "doc") + "\n";
  return out;
}

}  // namespace bsctest
