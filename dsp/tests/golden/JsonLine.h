#pragma once
#include <cstdint>
#include <string>

// One-line JSON text for streams (ParityStream.h) and the firmware's serial protocol
// (firmware/README.md): integers, booleans and escaped strings only. No floating point and
// no printf, so the firmware links neither.
namespace brainscape::golden {

inline std::string JsonString(const std::string& s) {
  static const char kHex[] = "0123456789abcdef";
  std::string       out    = "\"";
  for (const char c : s) {
    const auto u = static_cast<unsigned char>(c);
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (u < 0x20) {
      out += "\\u00";
      out += kHex[u >> 4];
      out += kHex[u & 15];
    } else {
      out += c;
    }
  }
  return out + "\"";
}

inline std::string JsonUInt(uint64_t v) {
  char buf[24];
  int  n = 0;
  do {
    buf[n++] = static_cast<char>('0' + v % 10);
    v /= 10;
  } while (v != 0);
  std::string out;
  while (n > 0) out += buf[--n];
  return out;
}

inline std::string JsonInt(int64_t v) {
  if (v >= 0) return JsonUInt(static_cast<uint64_t>(v));
  return "-" + JsonUInt(~static_cast<uint64_t>(v) + 1u);
}

// "0x%08X", as a JSON string.
inline std::string JsonHex32(uint32_t v) {
  static const char kHex[] = "0123456789ABCDEF";
  std::string       out    = "\"0x";
  for (int shift = 28; shift >= 0; shift -= 4) out += kHex[(v >> shift) & 15u];
  return out + "\"";
}

// An object's "key":value members in insertion order; Done() closes it.
class JsonObj {
 public:
  JsonObj& Raw(const char* key, const std::string& json) {
    if (s_.size() > 1) s_ += ',';
    s_ += JsonString(key);
    s_ += ':';
    s_ += json;
    return *this;
  }
  JsonObj& Str(const char* key, const std::string& v) { return Raw(key, JsonString(v)); }
  JsonObj& Int(const char* key, int64_t v) { return Raw(key, JsonInt(v)); }
  JsonObj& UInt(const char* key, uint64_t v) { return Raw(key, JsonUInt(v)); }
  JsonObj& Bool(const char* key, bool v) { return Raw(key, v ? "true" : "false"); }
  JsonObj& Hex(const char* key, uint32_t v) { return Raw(key, JsonHex32(v)); }
  std::string Done() const { return s_ + "}"; }

 private:
  std::string s_ = "{";
};

}  // namespace brainscape::golden
