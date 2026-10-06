#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

// The JSON subset the golden files use: objects, arrays, strings, integers, booleans
// and null. No floating-point numbers, so reading and writing a golden file does no
// floating-point work on any target.
namespace brainscape::golden {

struct Json {
  enum class Type : uint8_t { Null, Bool, Int, String, Array, Object };

  Type                                     type = Type::Null;
  bool                                     boolean = false;
  int64_t                                  integer = 0;
  std::string                              string;
  std::vector<Json>                        items;    // Array
  std::vector<std::pair<std::string, Json>> members;  // Object, in insertion order

  static Json Of(Type t) {
    Json j;
    j.type = t;
    return j;
  }
  static Json Int(int64_t v) {
    Json j    = Of(Type::Int);
    j.integer = v;
    return j;
  }
  static Json Bool(bool v) {
    Json j    = Of(Type::Bool);
    j.boolean = v;
    return j;
  }
  static Json Str(std::string v) {
    Json j   = Of(Type::String);
    j.string = std::move(v);
    return j;
  }
  static Json Arr() { return Of(Type::Array); }
  static Json Obj() { return Of(Type::Object); }

  Json& Set(const std::string& key, Json v) {
    for (auto& m : members) {
      if (m.first == key) { m.second = std::move(v); return m.second; }
    }
    members.emplace_back(key, std::move(v));
    return members.back().second;
  }
  Json& Push(Json v) { items.push_back(std::move(v)); return items.back(); }
  const Json* Find(const std::string& key) const {
    for (const auto& m : members) {
      if (m.first == key) return &m.second;
    }
    return nullptr;
  }
};

namespace detail {

inline void Escape(const std::string& s, std::string* out) {
  out->push_back('"');
  for (const char c : s) {
    if (c == '"' || c == '\\') { out->push_back('\\'); out->push_back(c); }
    else if (static_cast<unsigned char>(c) < 0x20) {
      char u[8];
      std::snprintf(u, sizeof u, "\\u%04x", static_cast<unsigned>(c));
      out->append(u);
    } else {
      out->push_back(c);
    }
  }
  out->push_back('"');
}

inline bool IsScalarArray(const Json& j) {
  for (const Json& v : j.items) {
    if (v.type != Json::Type::Int && v.type != Json::Type::Bool) return false;
  }
  return true;
}

inline void Write(const Json& j, int indent, std::string* out) {
  const std::string pad(static_cast<size_t>(indent) * 2u, ' ');
  switch (j.type) {
    case Json::Type::Null: out->append("null"); break;
    case Json::Type::Bool: out->append(j.boolean ? "true" : "false"); break;
    case Json::Type::Int: out->append(std::to_string(j.integer)); break;
    case Json::Type::String: Escape(j.string, out); break;
    case Json::Type::Array:
      if (j.items.empty()) { out->append("[]"); break; }
      if (IsScalarArray(j)) {  // counters and ring sizes stay on one line
        out->push_back('[');
        for (size_t i = 0; i < j.items.size(); ++i) {
          if (i) out->append(", ");
          Write(j.items[i], 0, out);
        }
        out->push_back(']');
        break;
      }
      out->append("[\n");
      for (size_t i = 0; i < j.items.size(); ++i) {
        out->append(pad).append("  ");
        Write(j.items[i], indent + 1, out);
        out->append(i + 1 < j.items.size() ? ",\n" : "\n");
      }
      out->append(pad).push_back(']');
      break;
    case Json::Type::Object:
      if (j.members.empty()) { out->append("{}"); break; }
      out->append("{\n");
      for (size_t i = 0; i < j.members.size(); ++i) {
        out->append(pad).append("  ");
        Escape(j.members[i].first, out);
        out->append(": ");
        Write(j.members[i].second, indent + 1, out);
        out->append(i + 1 < j.members.size() ? ",\n" : "\n");
      }
      out->append(pad).push_back('}');
      break;
  }
}

class Parser {
 public:
  explicit Parser(const std::string& text) : s_(text) {}

  bool Parse(Json* out, std::string* error) {
    const bool ok = Value(out, 0) && (Space(), pos_ == s_.size());
    if (!ok && error != nullptr) *error = "JSON error near byte " + std::to_string(pos_);
    return ok;
  }

 private:
  void Space() {
    while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\n' || s_[pos_] == '\r' ||
                                s_[pos_] == '\t')) {
      ++pos_;
    }
  }
  bool Literal(const char* word) {
    size_t i = 0;
    for (; word[i] != '\0'; ++i) {
      if (pos_ + i >= s_.size() || s_[pos_ + i] != word[i]) return false;
    }
    pos_ += i;
    return true;
  }
  bool String(std::string* out) {
    if (pos_ >= s_.size() || s_[pos_] != '"') return false;
    ++pos_;
    while (pos_ < s_.size() && s_[pos_] != '"') {
      char c = s_[pos_++];
      if (c == '\\') {
        if (pos_ >= s_.size()) return false;
        c = s_[pos_++];
        if (c == 'n') c = '\n';
        else if (c == 't') c = '\t';
        else if (c == 'u') {  // the writer only emits \u00XX
          if (pos_ + 4 > s_.size()) return false;
          unsigned v = 0;
          for (size_t i = 0; i < 4; ++i) {
            const char h = s_[pos_ + i];
            const int  d = h >= '0' && h <= '9' ? h - '0'
                           : h >= 'a' && h <= 'f' ? h - 'a' + 10
                           : h >= 'A' && h <= 'F' ? h - 'A' + 10 : -1;
            if (d < 0) return false;
            v = v * 16u + static_cast<unsigned>(d);
          }
          if (v > 0x7Fu) return false;
          c = static_cast<char>(v);
          pos_ += 4;
        } else if (c != '"' && c != '\\' && c != '/') {
          return false;
        }
      }
      out->push_back(c);
    }
    if (pos_ >= s_.size()) return false;
    ++pos_;
    return true;
  }
  bool Value(Json* out, int depth) {
    if (depth > 32) return false;
    Space();
    if (pos_ >= s_.size()) return false;
    const char c = s_[pos_];
    if (c == '{') {
      ++pos_;
      *out = Json::Obj();
      Space();
      if (pos_ < s_.size() && s_[pos_] == '}') { ++pos_; return true; }
      for (;;) {
        Space();
        std::string key;
        if (!String(&key)) return false;
        Space();
        if (pos_ >= s_.size() || s_[pos_++] != ':') return false;
        Json v;
        if (!Value(&v, depth + 1)) return false;
        out->members.emplace_back(std::move(key), std::move(v));
        Space();
        if (pos_ >= s_.size()) return false;
        if (s_[pos_] == ',') { ++pos_; continue; }
        if (s_[pos_] == '}') { ++pos_; return true; }
        return false;
      }
    }
    if (c == '[') {
      ++pos_;
      *out = Json::Arr();
      Space();
      if (pos_ < s_.size() && s_[pos_] == ']') { ++pos_; return true; }
      for (;;) {
        Json v;
        if (!Value(&v, depth + 1)) return false;
        out->items.push_back(std::move(v));
        Space();
        if (pos_ >= s_.size()) return false;
        if (s_[pos_] == ',') { ++pos_; continue; }
        if (s_[pos_] == ']') { ++pos_; return true; }
        return false;
      }
    }
    if (c == '"') {
      *out = Json::Str("");
      return String(&out->string);
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
      const bool neg = c == '-';
      if (neg) ++pos_;
      if (pos_ >= s_.size() || s_[pos_] < '0' || s_[pos_] > '9') return false;
      uint64_t v = 0;
      while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') {
        v = v * 10u + static_cast<uint64_t>(s_[pos_++] - '0');
        if (v > (uint64_t{1} << 62)) return false;
      }
      if (pos_ < s_.size() && (s_[pos_] == '.' || s_[pos_] == 'e' || s_[pos_] == 'E')) {
        return false;  // integers only
      }
      *out = Json::Int(neg ? -static_cast<int64_t>(v) : static_cast<int64_t>(v));
      return true;
    }
    if (Literal("true")) { *out = Json::Bool(true); return true; }
    if (Literal("false")) { *out = Json::Bool(false); return true; }
    if (Literal("null")) { *out = Json(); return true; }
    return false;
  }

  const std::string& s_;
  size_t             pos_ = 0;
};

}  // namespace detail

inline std::string ToText(const Json& j) {
  std::string out;
  detail::Write(j, 0, &out);
  out.push_back('\n');
  return out;
}

inline bool FromText(const std::string& text, Json* out, std::string* error) {
  return detail::Parser(text).Parse(out, error);
}

}  // namespace brainscape::golden
