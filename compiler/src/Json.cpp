#include "Json.h"

#include <set>
#include <utility>

#include "Text.h"

namespace bsc::json {

// ── Value ────────────────────────────────────────────────────────────────────────────────

const Value* Value::Find(std::string_view key) const {
  for (const Member& m : members) {
    if (m.key == key) return &m.value;
  }
  return nullptr;
}

Value Value::Null() { return Value{}; }

Value Value::Bool(bool b) {
  Value v;
  v.type    = Type::Bool;
  v.boolean = b;
  return v;
}

Value Value::Number(std::string text) {
  Value v;
  v.type = Type::Number;
  v.text = std::move(text);
  return v;
}

Value Value::String(std::string text) {
  Value v;
  v.type = Type::String;
  v.text = std::move(text);
  return v;
}

Value Value::Array() {
  Value v;
  v.type = Type::Array;
  return v;
}

Value Value::Object() {
  Value v;
  v.type = Type::Object;
  return v;
}

Value& Value::Add(std::string key, Value value) {
  Member m;
  m.key   = std::move(key);
  m.value = std::move(value);
  members.push_back(std::move(m));
  return members.back().value;
}

Value& Value::Push(Value value) {
  items.push_back(std::move(value));
  return items.back();
}

// ── UTF-8 and integers ───────────────────────────────────────────────────────────────────

namespace {

// The length of the UTF-8 sequence at s[i] (1-4), or 0 if it is not one (RFC 3629).
size_t Utf8Length(std::string_view s, size_t i) {
  const auto c = static_cast<uint8_t>(s[i]);
  if (c < 0x80u) return 1;
  size_t  extra;
  uint8_t lo = 0x80u, hi = 0xBFu;  // the second byte's range
  if (c >= 0xC2u && c <= 0xDFu) {
    extra = 1;
  } else if (c >= 0xE0u && c <= 0xEFu) {
    extra = 2;
    if (c == 0xE0u) lo = 0xA0u;  // no overlong form
    if (c == 0xEDu) hi = 0x9Fu;  // no surrogate
  } else if (c >= 0xF0u && c <= 0xF4u) {
    extra = 3;
    if (c == 0xF0u) lo = 0x90u;
    if (c == 0xF4u) hi = 0x8Fu;  // nothing past U+10FFFF
  } else {
    return 0;
  }
  if (s.size() - i <= extra) return 0;
  const auto second = static_cast<uint8_t>(s[i + 1]);
  if (second < lo || second > hi) return 0;
  for (size_t k = 2; k <= extra; ++k) {
    const auto b = static_cast<uint8_t>(s[i + k]);
    if (b < 0x80u || b > 0xBFu) return 0;
  }
  return extra + 1;
}

void AppendUtf8(std::string& out, uint32_t cp) {
  if (cp < 0x80u) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800u) {
    out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
    out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
  } else if (cp < 0x10000u) {
    out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
    out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
  } else {
    out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
    out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
  }
}

bool IsDigit(char c) { return c >= '0' && c <= '9'; }

}  // namespace

bool ValidUtf8(std::string_view s) {
  size_t i = 0;
  while (i < s.size()) {
    const size_t n = Utf8Length(s, i);
    if (n == 0) return false;
    i += n;
  }
  return true;
}

IntStatus ParseInt(std::string_view t, int64_t* out) {
  size_t     i        = 0;
  const bool negative = !t.empty() && t[0] == '-';
  if (negative) ++i;
  if (i >= t.size() || !IsDigit(t[i])) return IntStatus::NotInteger;
  if (t[i] == '0' && i + 1 < t.size()) return IntStatus::NotInteger;  // a fraction or exponent
  uint64_t magnitude = 0;
  for (; i < t.size(); ++i) {
    if (!IsDigit(t[i])) return IntStatus::NotInteger;
    const auto d = static_cast<uint64_t>(t[i] - '0');
    if (magnitude > (UINT64_MAX - d) / 10u) return IntStatus::Range;
    magnitude = magnitude * 10u + d;
  }
  const uint64_t limit = negative ? (uint64_t{1} << 63) : (uint64_t{1} << 63) - 1u;
  if (magnitude > limit) return IntStatus::Range;
  *out = negative ? static_cast<int64_t>(~magnitude + 1u) : static_cast<int64_t>(magnitude);
  return IntStatus::Ok;
}

// ── Reader ───────────────────────────────────────────────────────────────────────────────

namespace {

class Parser {
 public:
  Parser(std::string_view text, ParseError* error) : s_(text), error_(error) {
    lineStarts_.push_back(0);
    for (size_t i = 0; i < s_.size(); ++i) {
      if (s_[i] == '\n') lineStarts_.push_back(i + 1);
    }
  }

  bool Run(Value* out) {
    if (s_.size() >= 3 && static_cast<uint8_t>(s_[0]) == 0xEFu &&
        static_cast<uint8_t>(s_[1]) == 0xBBu && static_cast<uint8_t>(s_[2]) == 0xBFu) {
      return Fail(0, "a byte-order mark is not allowed (UTF-8 without BOM)");
    }
    SkipSpace();
    if (i_ >= s_.size()) return Fail(i_, "empty document");
    if (!ParseValue(out, 0)) return false;
    SkipSpace();
    if (i_ != s_.size()) return Fail(i_, "unexpected text after the document");
    return true;
  }

 private:
  bool Fail(size_t at, const std::string& message) {
    if (error_ != nullptr) {
      Position(at, &error_->line, &error_->column);
      error_->pointer.clear();
      for (const std::string& token : path_) error_->pointer += "/" + PointerToken(token);
      error_->message = message;
    }
    return false;
  }

  // Line and column (1-based; the column counts characters, a malformed byte as one).
  // Counting resumes from the last position asked on the same line, since the parser asks in
  // reading order: a document on one long line costs linear time, not quadratic.
  void Position(size_t at, uint32_t* line, uint32_t* column) {
    size_t lo = 0, hi = lineStarts_.size();
    while (hi - lo > 1) {
      const size_t mid = (lo + hi) / 2;
      if (lineStarts_[mid] <= at) {
        lo = mid;
      } else {
        hi = mid;
      }
    }
    size_t   from = lineStarts_[lo];
    uint32_t col  = 1;
    if (cacheLine_ == lo && cacheAt_ <= at) {
      from = cacheAt_;
      col  = cacheColumn_;
    }
    for (size_t k = from; k < at && k < s_.size(); ++k) {
      if ((static_cast<uint8_t>(s_[k]) & 0xC0u) != 0x80u) ++col;
    }
    cacheLine_   = lo;
    cacheAt_     = at;
    cacheColumn_ = col;
    *line        = static_cast<uint32_t>(lo + 1);
    *column      = col;
  }

  void SkipSpace() {
    while (i_ < s_.size() &&
           (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r')) {
      ++i_;
    }
  }

  bool ParseValue(Value* out, uint32_t depth) {
    *out = Value{};
    Position(i_, &out->line, &out->column);
    const char c = s_[i_];
    if (c == '{' || c == '[') {
      if (depth + 1 > kMaxDepth) return Fail(i_, "nested deeper than 16 levels");
      return c == '{' ? ParseObject(out, depth + 1) : ParseArray(out, depth + 1);
    }
    if (c == '"') {
      out->type = Type::String;
      return ParseString(&out->text);
    }
    if (c == '-' || IsDigit(c)) return ParseNumber(out);
    if (Literal("true")) {
      out->type    = Type::Bool;
      out->boolean = true;
      return true;
    }
    if (Literal("false")) {
      out->type = Type::Bool;
      return true;
    }
    if (Literal("null")) return true;
    if (c == '/') return Fail(i_, "comments are not JSON");
    if (static_cast<uint8_t>(c) >= 0x80u && Utf8Length(s_, i_) == 0) {
      return Fail(i_, "invalid UTF-8");
    }
    return Fail(i_, "expected a value");
  }

  bool Literal(const char* word) {
    size_t n = 0;
    while (word[n] != '\0') ++n;
    if (s_.size() - i_ < n || s_.compare(i_, n, word) != 0) return false;
    // A literal ends where a value may end: "truex" is not "true".
    if (i_ + n < s_.size()) {
      const char next = s_[i_ + n];
      if ((next >= 'a' && next <= 'z') || (next >= 'A' && next <= 'Z') || IsDigit(next)) {
        return false;
      }
    }
    i_ += n;
    return true;
  }

  // -?(0|[1-9][0-9]*)(.[0-9]+)?([eE][+-]?[0-9]+)?, kept as text.
  bool ParseNumber(Value* out) {
    const size_t start = i_;
    if (s_[i_] == '-') ++i_;
    if (i_ >= s_.size() || !IsDigit(s_[i_])) return Fail(i_, "a number needs a digit");
    if (s_[i_] == '0') {
      ++i_;
      if (i_ < s_.size() && IsDigit(s_[i_]))
        return Fail(start, "a number cannot have a leading zero");
    } else {
      while (i_ < s_.size() && IsDigit(s_[i_])) ++i_;
    }
    if (i_ < s_.size() && s_[i_] == '.') {
      ++i_;
      if (i_ >= s_.size() || !IsDigit(s_[i_])) return Fail(i_, "a fraction needs a digit");
      while (i_ < s_.size() && IsDigit(s_[i_])) ++i_;
    }
    if (i_ < s_.size() && (s_[i_] == 'e' || s_[i_] == 'E')) {
      ++i_;
      if (i_ < s_.size() && (s_[i_] == '+' || s_[i_] == '-')) ++i_;
      if (i_ >= s_.size() || !IsDigit(s_[i_])) return Fail(i_, "an exponent needs a digit");
      while (i_ < s_.size() && IsDigit(s_[i_])) ++i_;
    }
    out->type = Type::Number;
    out->text.assign(s_.substr(start, i_ - start));
    return true;
  }

  int Hex4(size_t at) const {
    if (s_.size() - at < 4) return -1;
    int v = 0;
    for (size_t k = 0; k < 4; ++k) {
      const char c = s_[at + k];
      int        d;
      if (IsDigit(c)) {
        d = c - '0';
      } else if (c >= 'a' && c <= 'f') {
        d = c - 'a' + 10;
      } else if (c >= 'A' && c <= 'F') {
        d = c - 'A' + 10;
      } else {
        return -1;
      }
      v = v * 16 + d;
    }
    return v;
  }

  bool ParseString(std::string* out) {
    const size_t open = i_;
    ++i_;  // the opening quote
    out->clear();
    for (;;) {
      if (i_ >= s_.size()) return Fail(open, "unterminated string");
      const auto c = static_cast<uint8_t>(s_[i_]);
      if (c == '"') {
        ++i_;
        return true;
      }
      if (c < 0x20u) return Fail(i_, "a raw control character in a string (escape it)");
      if (c == '\\') {
        if (i_ + 1 >= s_.size()) return Fail(open, "unterminated string");
        const char e = s_[i_ + 1];
        switch (e) {
          case '"':
            out->push_back('"');
            break;
          case '\\':
            out->push_back('\\');
            break;
          case '/':
            out->push_back('/');
            break;
          case 'b':
            out->push_back('\b');
            break;
          case 'f':
            out->push_back('\f');
            break;
          case 'n':
            out->push_back('\n');
            break;
          case 'r':
            out->push_back('\r');
            break;
          case 't':
            out->push_back('\t');
            break;
          case 'u': {
            const int u = Hex4(i_ + 2);
            if (u < 0) return Fail(i_, "\\u needs four hex digits");
            uint32_t cp = static_cast<uint32_t>(u);
            if (cp >= 0xDC00u && cp <= 0xDFFFu) return Fail(i_, "an unpaired low surrogate");
            if (cp >= 0xD800u && cp <= 0xDBFFu) {
              const size_t next = i_ + 6;
              const int    low  = s_.size() - next >= 2 && s_[next] == '\\' && s_[next + 1] == 'u'
                                      ? Hex4(next + 2)
                                      : -1;
              if (low < 0xDC00 || low > 0xDFFF) return Fail(i_, "an unpaired high surrogate");
              cp = 0x10000u + ((cp - 0xD800u) << 10) + (static_cast<uint32_t>(low) - 0xDC00u);
              i_ += 6;
            }
            AppendUtf8(*out, cp);
            i_ += 6;
            continue;
          }
          default:
            return Fail(i_, "an unknown escape");
        }
        i_ += 2;
        continue;
      }
      const size_t n = Utf8Length(s_, i_);
      if (n == 0) return Fail(i_, "invalid UTF-8");
      out->append(s_.substr(i_, n));
      i_ += n;
    }
  }

  bool ParseArray(Value* out, uint32_t depth) {
    out->type = Type::Array;
    ++i_;  // [
    SkipSpace();
    if (i_ < s_.size() && s_[i_] == ']') {
      ++i_;
      return true;
    }
    for (;;) {
      SkipSpace();
      if (i_ >= s_.size()) return Fail(i_, "unterminated array");
      path_.push_back(Dec(out->items.size()));
      out->items.emplace_back();
      if (!ParseValue(&out->items.back(), depth)) return false;
      path_.pop_back();
      SkipSpace();
      if (i_ >= s_.size()) return Fail(i_, "unterminated array");
      if (s_[i_] == ',') {
        ++i_;
        SkipSpace();
        if (i_ < s_.size() && s_[i_] == ']') return Fail(i_, "a trailing comma");
        continue;
      }
      if (s_[i_] == ']') {
        ++i_;
        return true;
      }
      return Fail(i_, "expected ',' or ']'");
    }
  }

  bool ParseObject(Value* out, uint32_t depth) {
    out->type = Type::Object;
    ++i_;  // {
    SkipSpace();
    if (i_ < s_.size() && s_[i_] == '}') {
      ++i_;
      return true;
    }
    std::set<std::string> keys;  // duplicates in O(n log n), whatever the object's size
    for (;;) {
      SkipSpace();
      if (i_ >= s_.size()) return Fail(i_, "unterminated object");
      if (s_[i_] != '"') return Fail(i_, "expected a key in double quotes");
      Member       m;
      const size_t keyAt = i_;
      Position(keyAt, &m.line, &m.column);
      if (!ParseString(&m.key)) return false;
      if (!keys.insert(m.key).second) {
        path_.push_back(m.key);
        return Fail(keyAt, "duplicate key \"" + m.key + "\"");
      }
      SkipSpace();
      if (i_ >= s_.size() || s_[i_] != ':') return Fail(i_, "expected ':'");
      ++i_;
      SkipSpace();
      if (i_ >= s_.size()) return Fail(i_, "expected a value");
      path_.push_back(m.key);
      if (!ParseValue(&m.value, depth)) return false;
      path_.pop_back();
      out->members.push_back(std::move(m));
      SkipSpace();
      if (i_ >= s_.size()) return Fail(i_, "unterminated object");
      if (s_[i_] == ',') {
        ++i_;
        SkipSpace();
        if (i_ < s_.size() && s_[i_] == '}') return Fail(i_, "a trailing comma");
        continue;
      }
      if (s_[i_] == '}') {
        ++i_;
        return true;
      }
      return Fail(i_, "expected ',' or '}'");
    }
  }

  std::string_view         s_;
  ParseError*              error_;
  size_t                   i_ = 0;
  std::vector<size_t>      lineStarts_;
  std::vector<std::string> path_;
  size_t                   cacheLine_   = static_cast<size_t>(-1);
  size_t                   cacheAt_     = 0;
  uint32_t                 cacheColumn_ = 1;
};

}  // namespace

bool Parse(std::string_view text, Value* out, ParseError* error) {
  Parser p(text, error);
  return p.Run(out);
}

// ── Writer ───────────────────────────────────────────────────────────────────────────────

void AppendQuoted(std::string& out, std::string_view s) {
  static const char kHex[] = "0123456789abcdef";
  out.push_back('"');
  for (const char ch : s) {
    const auto c = static_cast<uint8_t>(ch);
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (c < 0x20u) {
          out += "\\u00";
          out.push_back(kHex[c >> 4]);
          out.push_back(kHex[c & 15u]);
        } else {
          out.push_back(ch);
        }
    }
  }
  out.push_back('"');
}

std::string PointerToken(std::string_view key) {
  std::string out;
  for (const char c : key) {
    if (c == '~') {
      out += "~0";
    } else if (c == '/') {
      out += "~1";
    } else {
      out.push_back(c);
    }
  }
  return out;
}

namespace {

void Indent(std::string& out, uint32_t depth) { out.append(2u * depth, ' '); }

void Write(std::string& out, const Value& v, uint32_t depth) {
  switch (v.type) {
    case Type::Null:
      out += "null";
      return;
    case Type::Bool:
      out += v.boolean ? "true" : "false";
      return;
    case Type::Number:
      out += v.text;
      return;
    case Type::String:
      AppendQuoted(out, v.text);
      return;
    case Type::Array: {
      if (v.items.empty()) {
        out += "[]";
        return;
      }
      bool scalars = true;
      for (const Value& item : v.items) scalars = scalars && item.IsScalar();
      if (scalars) {
        out.push_back('[');
        for (size_t k = 0; k < v.items.size(); ++k) {
          if (k > 0) out += ", ";
          Write(out, v.items[k], depth + 1);
        }
        out.push_back(']');
        return;
      }
      out += "[\n";
      for (size_t k = 0; k < v.items.size(); ++k) {
        Indent(out, depth + 1);
        Write(out, v.items[k], depth + 1);
        out += k + 1 < v.items.size() ? ",\n" : "\n";
      }
      Indent(out, depth);
      out.push_back(']');
      return;
    }
    case Type::Object: {
      if (v.members.empty()) {
        out += "{}";
        return;
      }
      out += "{\n";
      for (size_t k = 0; k < v.members.size(); ++k) {
        Indent(out, depth + 1);
        AppendQuoted(out, v.members[k].key);
        out += ": ";
        Write(out, v.members[k].value, depth + 1);
        out += k + 1 < v.members.size() ? ",\n" : "\n";
      }
      Indent(out, depth);
      out.push_back('}');
      return;
    }
  }
}

}  // namespace

std::string Serialize(const Value& value) {
  std::string out;
  Write(out, value, 0);
  out.push_back('\n');
  return out;
}

bool Equal(const Value& a, const Value& b) {
  if (a.type != b.type) return false;
  switch (a.type) {
    case Type::Null:
      return true;
    case Type::Bool:
      return a.boolean == b.boolean;
    case Type::Number:
    case Type::String:
      return a.text == b.text;
    case Type::Array:
      if (a.items.size() != b.items.size()) return false;
      for (size_t k = 0; k < a.items.size(); ++k) {
        if (!Equal(a.items[k], b.items[k])) return false;
      }
      return true;
    case Type::Object:
      if (a.members.size() != b.members.size()) return false;
      for (size_t k = 0; k < a.members.size(); ++k) {
        if (a.members[k].key != b.members[k].key ||
            !Equal(a.members[k].value, b.members[k].value)) {
          return false;
        }
      }
      return true;
  }
  return false;
}

}  // namespace bsc::json
