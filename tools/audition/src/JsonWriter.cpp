#include "JsonWriter.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace bsa {

std::string Quoted(std::string_view s) {
  std::string out = "\"";
  for (const char ch : s) {
    const auto c = static_cast<unsigned char>(ch);
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof buf, "\\u%04x", c);
          out += buf;
        } else {
          out += ch;
        }
    }
  }
  return out + "\"";
}

std::string FloatText(float v) {
  if (!std::isfinite(v)) return "null";
  char buf[64];
  for (int digits = 1; digits <= 9; ++digits) {  // the shortest that reads back exactly
    std::snprintf(buf, sizeof buf, "%.*g", digits, static_cast<double>(v));
    if (std::strtof(buf, nullptr) == v) break;
  }
  const double a = std::fabs(static_cast<double>(v));
  if (std::strchr(buf, 'e') != nullptr && a >= 1e-4 && a < 1e9) {
    // Plain decimals where they are short: 100, not 1e+02.
    for (int decimals = 0; decimals <= 12; ++decimals) {
      std::snprintf(buf, sizeof buf, "%.*f", decimals, static_cast<double>(v));
      if (std::strtof(buf, nullptr) == v) break;
    }
  }
  return buf;
}

std::string BitsHex(float v) {
  uint32_t u;
  std::memcpy(&u, &v, 4);
  char buf[16];
  std::snprintf(buf, sizeof buf, "%08x", static_cast<unsigned>(u));
  return buf;
}

void JsonWriter::Value() {
  if (afterKey_) {
    afterKey_ = false;
    return;
  }
  if (stack_.empty()) return;
  Level& l = stack_.back();
  out_ += l.empty ? "\n" : ",\n";
  l.empty = false;
  out_.append(2 * stack_.size(), ' ');
}

JsonWriter& JsonWriter::BeginObject() {
  Value();
  out_ += "{";
  stack_.push_back({false, true});
  return *this;
}

JsonWriter& JsonWriter::EndObject() {
  const bool empty = stack_.back().empty;
  stack_.pop_back();
  if (!empty) {
    out_ += "\n";
    out_.append(2 * stack_.size(), ' ');
  }
  out_ += "}";
  return *this;
}

JsonWriter& JsonWriter::BeginArray() {
  Value();
  out_ += "[";
  stack_.push_back({true, true});
  return *this;
}

JsonWriter& JsonWriter::EndArray() {
  const bool empty = stack_.back().empty;
  stack_.pop_back();
  if (!empty) {
    out_ += "\n";
    out_.append(2 * stack_.size(), ' ');
  }
  out_ += "]";
  return *this;
}

JsonWriter& JsonWriter::Key(std::string_view key) {
  Value();
  out_ += Quoted(key) + ": ";
  afterKey_ = true;
  return *this;
}

JsonWriter& JsonWriter::String(std::string_view s) {
  Value();
  out_ += Quoted(s);
  return *this;
}

JsonWriter& JsonWriter::Int(int64_t v) {
  Value();
  out_ += std::to_string(v);
  return *this;
}

JsonWriter& JsonWriter::Uint(uint64_t v) {
  Value();
  out_ += std::to_string(v);
  return *this;
}

JsonWriter& JsonWriter::Double(double v, int decimals) {
  Value();
  if (!std::isfinite(v)) {
    out_ += "null";
    return *this;
  }
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.*f", decimals, v);
  if (std::strcmp(buf, "-0") == 0 || (std::strncmp(buf, "-0.", 3) == 0 &&
                                      std::strspn(buf + 3, "0") == std::strlen(buf + 3))) {
    out_ += buf + 1;  // no negative zero in print
  } else {
    out_ += buf;
  }
  return *this;
}

JsonWriter& JsonWriter::Float(float v) {
  Value();
  out_ += FloatText(v);
  return *this;
}

JsonWriter& JsonWriter::Bool(bool b) {
  Value();
  out_ += b ? "true" : "false";
  return *this;
}

JsonWriter& JsonWriter::Null() {
  Value();
  out_ += "null";
  return *this;
}

JsonWriter& JsonWriter::Raw(std::string_view text) {
  Value();
  out_ += text;
  return *this;
}

}  // namespace bsa
