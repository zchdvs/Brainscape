#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// The compiler's JSON (docs/design/mode-compiler.md §6.4, §6.5): a strict RFC 8259 reader that
// keeps every number's text until the schema knows its type, and the canonical writer's
// layout. Integer-only. The golden harness keeps its own reader (record §2.4).
//
// Reading (rule E1, §2.7): UTF-8 without a byte-order mark; no comments, trailing commas or
// other extensions; every escape, surrogate pairs included, an unpaired surrogate refused;
// no raw control character in a string; no duplicate key in an object; nesting at most 16
// deep; nothing after the value. A refusal names the line, the column (in characters) and
// the JSON pointer (RFC 6901) of the value being read.
//
// Writing (§6.4): two-space indent, LF line ends, a final newline; an array of scalars on one
// line, any other array and every object one element per line; strings escape only `"`, `\`
// and control characters (\b \f \n \r \t, others as \u00xx); numbers as their stored text.
namespace bsc::json {

enum class Type : uint8_t { Null, Bool, Number, String, Array, Object };

struct Member;

struct Value {
  Type                type    = Type::Null;
  bool                boolean = false;
  std::string         text;        // a Number's text as written, a String's decoded UTF-8
  std::vector<Value>  items;       // Array
  std::vector<Member> members;     // Object, in written order, keys unique
  uint32_t            line   = 0;  // where the value starts, 1-based; 0 when built in memory
  uint32_t            column = 0;

  const Value* Find(std::string_view key) const;
  bool         IsScalar() const { return type != Type::Array && type != Type::Object; }

  static Value Null();
  static Value Bool(bool b);
  static Value Number(std::string text);
  static Value String(std::string text);
  static Value Array();
  static Value Object();
  // Appends a member to an Object (the caller keeps keys unique).
  Value& Add(std::string key, Value value);
  // Appends an item to an Array.
  Value& Push(Value value);
};

struct Member {
  std::string key;
  uint32_t    line   = 0;  // where the key starts
  uint32_t    column = 0;
  Value       value;
};

inline constexpr uint32_t kMaxDepth = 16;

struct ParseError {
  uint32_t    line   = 0;
  uint32_t    column = 0;
  std::string pointer;
  std::string message;
};

// Parses `text` strictly; on failure *error says where and why and *out is unspecified.
bool Parse(std::string_view text, Value* out, ParseError* error);

// The canonical layout of `value`, with a final newline.
std::string Serialize(const Value& value);
// `s` as a JSON string literal, quotes included.
void AppendQuoted(std::string& out, std::string_view s);
// One JSON pointer reference token (RFC 6901): "~" as "~0", "/" as "~1".
std::string PointerToken(std::string_view key);

// Equal content, positions ignored (object members compared in order).
bool Equal(const Value& a, const Value& b);

// Strict UTF-8 (RFC 3629: no overlong form, surrogate or code point past U+10FFFF).
bool ValidUtf8(std::string_view s);

// A number token's text as a JSON integer: -?(0|[1-9][0-9]*) within int64.
enum class IntStatus : uint8_t { Ok, NotInteger, Range };
IntStatus ParseInt(std::string_view text, int64_t* out);

}  // namespace bsc::json
