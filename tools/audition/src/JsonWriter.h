#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// A small JSON writer for recipes, manifests and the pre-screen (two-space indent, LF, keys in
// the order written). Numbers: integers exactly; doubles to the digits asked for, in the C
// locale's form; non-finite doubles as null.
namespace bsa {

class JsonWriter {
 public:
  JsonWriter& BeginObject();
  JsonWriter& EndObject();
  JsonWriter& BeginArray();
  JsonWriter& EndArray();
  JsonWriter& Key(std::string_view key);
  JsonWriter& String(std::string_view s);
  JsonWriter& Int(int64_t v);
  JsonWriter& Uint(uint64_t v);
  JsonWriter& Double(double v, int decimals);  // fixed decimals
  JsonWriter& Float(float v);                  // shortest text that reads back as the same float
  JsonWriter& Bool(bool b);
  JsonWriter& Null();
  // An array of scalars on one line: written by the caller as Raw text.
  JsonWriter& Raw(std::string_view text);

  // The document, with a final newline.
  std::string Text() const { return out_ + "\n"; }

 private:
  void Value();  // separators and indentation before a value
  struct Level {
    bool array;
    bool empty;
  };
  std::string        out_;
  std::vector<Level> stack_;
  bool               afterKey_ = false;
};

std::string Quoted(std::string_view s);
std::string FloatText(float v);  // as Float writes it
std::string BitsHex(float v);    // the binary32 bits, 8 hex digits

}  // namespace bsa
