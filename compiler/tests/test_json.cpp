// The compiler's strict JSON reader and canonical layout (docs/design/mode-compiler.md §6.4,
// §6.5, §10.2's grammar suite): RFC 8259 exactly, every escape and surrogate pair, invalid
// UTF-8 and duplicate keys refused, depth 16, and errors with line, column and pointer.
#include <chrono>
#include <string>
#include <vector>

#include "Json.h"
#include "catch.hpp"

using namespace bsc;

namespace {

bool Accepts(const std::string& text, json::Value* out = nullptr) {
  json::Value      v;
  json::ParseError e;
  const bool       ok = json::Parse(text, &v, &e);
  if (ok && out != nullptr) *out = v;
  return ok;
}

json::ParseError Refusal(const std::string& text) {
  json::Value      v;
  json::ParseError e;
  REQUIRE_FALSE(json::Parse(text, &v, &e));
  return e;
}

std::string Nest(int depth) {
  return std::string(static_cast<size_t>(depth), '[') +
         std::string(static_cast<size_t>(depth), ']');
}

}  // namespace

TEST_CASE("json: RFC 8259 values are read, number text kept", "[json]") {
  json::Value v;
  REQUIRE(Accepts(" {\"a\": [true, false, null, 0, -0, 1.5, 2e10, -3E-2, 1e+5, \"s\"]}\r\n\t", &v));
  REQUIRE(v.type == json::Type::Object);
  const json::Value* a = v.Find("a");
  REQUIRE(a != nullptr);
  REQUIRE(a->items.size() == 10u);
  REQUIRE(a->items[0].boolean);
  REQUIRE(a->items[2].type == json::Type::Null);
  REQUIRE(a->items[4].text == "-0");
  REQUIRE(a->items[5].text == "1.5");
  REQUIRE(a->items[7].text == "-3E-2");
  REQUIRE(a->items[9].text == "s");
  // Scalars at the top level and every escape.
  REQUIRE(Accepts("1"));
  REQUIRE(Accepts("\"x\""));
  REQUIRE(Accepts("[]"));
  REQUIRE(Accepts("{}"));
  REQUIRE(Accepts("\"\\\" \\\\ \\/ \\b \\f \\n \\r \\t \\u0041 \\u00e9 \\u20AC\"", &v));
  REQUIRE(v.text == "\" \\ / \b \f \n \r \t A \xC3\xA9 \xE2\x82\xAC");
  // A surrogate pair is one character; \u0000 is a character too.
  REQUIRE(Accepts("\"\\ud83d\\ude00\\u0000\"", &v));
  REQUIRE(v.text == std::string("\xF0\x9F\x98\x80\0", 5));
  // Raw UTF-8 of every length.
  REQUIRE(Accepts("\"\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80\xF4\x8F\xBF\xBF\"", &v));
  REQUIRE(v.text == "\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80\xF4\x8F\xBF\xBF");
  // Depth 16 is the limit.
  REQUIRE(Accepts(Nest(16)));
}

TEST_CASE("json: everything else is refused", "[json]") {
  const char* const bad[] = {
      "",                // empty
      "   ",             // nothing
      "\xEF\xBB\xBF{}",  // a byte-order mark
      "{\"a\": 1,}",     // trailing comma
      "[1, 2,]",
      "[1 2]",
      "{\"a\" 1}",
      "{a: 1}",    // unquoted key
      "{'a': 1}",  // single quotes
      "// c\n{}",  // comments
      "/* c */ {}",
      "01",  // leading zero
      "-",
      "1.",
      ".5",
      "1e",
      "1e+",
      "+1",
      "NaN",
      "Infinity",
      "-Infinity",
      "0x10",
      "truex",
      "nul",
      "\"abc",                       // unterminated
      "\"a\x01\"",                   // raw control character
      "\"a\tb\"",                    // raw tab
      "\"\\x41\"",                   // unknown escape
      "\"\\u12\"",                   // short \u
      "\"\\ud83d\"",                 // unpaired high surrogate
      "\"\\ud83d\\u0041\"",          // high surrogate, not followed by a low one
      "\"\\ude00\"",                 // unpaired low surrogate
      "\"\xFF\"",                    // invalid UTF-8
      "\"\xC0\x80\"",                // overlong
      "\"\xE0\x80\x80\"",            // overlong
      "\"\xED\xA0\x80\"",            // a surrogate in UTF-8
      "\"\xF4\x90\x80\x80\"",        // past U+10FFFF
      "\"\xE2\x82\"",                // truncated
      "{\"a\": 1, \"a\": 2}",        // duplicate key
      "{\"a\": 1, \"\\u0061\": 2}",  // duplicate after unescaping
      "{} {}",                       // text after the document
      "[1] x",
  };
  for (const char* text : bad) {
    INFO(text);
    REQUIRE_FALSE(Accepts(text));
  }
  REQUIRE_FALSE(Accepts(Nest(17)));
  REQUIRE_FALSE(Accepts("{\"a\":" + Nest(16) + "}"));  // the object is level 1
}

TEST_CASE("json: errors carry line, column (in characters) and pointer", "[json]") {
  json::ParseError e = Refusal("{\n  \"layers\": [\n    {\"size_ms\": 01}\n  ]\n}");
  REQUIRE(e.line == 3u);
  REQUIRE(e.column == 17u);
  REQUIRE(e.pointer == "/layers/0/size_ms");
  // Columns count characters: two two-byte characters before the error.
  e = Refusal("{\"\xC3\xA9\xC3\xA9\": ?}");
  REQUIRE(e.line == 1u);
  REQUIRE(e.column == 8u);
  REQUIRE(e.pointer == "/\xC3\xA9\xC3\xA9");
  e = Refusal("{\"a\": 1, \"a\": 2}");
  REQUIRE(e.pointer == "/a");
  REQUIRE(e.message.find("duplicate") != std::string::npos);
  e = Refusal("{\"a/b~c\": [x]}");
  REQUIRE(e.pointer == "/a~1b~0c/0");
  json::Value v;
  REQUIRE(Accepts("{\n  \"k\": [1,\n    2]}", &v));
  REQUIRE(v.members[0].line == 2u);
  REQUIRE(v.members[0].column == 3u);
  REQUIRE(v.members[0].value.items[1].line == 3u);
  REQUIRE(v.members[0].value.items[1].column == 5u);
  // Columns resume from the last position asked on a line, also past multi-byte characters.
  REQUIRE(Accepts("{\"\xC3\xA9\": 1, \"\xE2\x82\xAC\": [2, \"\xF0\x9F\x8E\xB8\", 3]}", &v));
  REQUIRE(v.members[1].column == 10u);
  REQUIRE(v.members[1].value.items[1].column == 19u);
  REQUIRE(v.members[1].value.items[2].column == 24u);
  e = Refusal("{\"\xC3\xA9\": 1, \"b\": 2, \"\xC3\xA9\": 3}");
  REQUIRE(e.column == 18u);
}

TEST_CASE("json: large objects and long lines read in linear time", "[json]") {
  // 60,000 keys (about 650 KB), on one line and one per line: duplicate keys are found through
  // a sorted set and columns counted from the last position asked, where a scan of the earlier
  // keys and of the line from its start took tens of seconds (lane A review).
  for (const char* separator : {", ", ",\n"}) {
    const auto  start = std::chrono::steady_clock::now();
    std::string text  = "{";
    for (int i = 0; i < 60000; ++i) {
      if (i != 0) text += separator;
      text += "\"key" + std::to_string(i) + "\": " + std::to_string(i);
    }
    text += "}";
    json::Value v;
    REQUIRE(Accepts(text, &v));
    REQUIRE(v.members.size() == 60000u);
    const bool oneLine = separator[1] == ' ';
    REQUIRE(v.members.back().line == (oneLine ? 1u : 60000u));
    REQUIRE(v.members.back().column ==
            (oneLine ? static_cast<uint32_t>(text.rfind("\"key59999\"") + 1) : 1u));
    text.back() = ',';
    text += " \"key31337\": 0}";
    const json::ParseError e = Refusal(text);
    REQUIRE(e.pointer == "/key31337");
    REQUIRE(e.message.find("duplicate") != std::string::npos);
    const auto seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    INFO("seconds: " << seconds);
    REQUIRE(seconds < 10.0);  // linear: well under a second in Release
  }
}

TEST_CASE("json: the canonical layout", "[json]") {
  json::Value v;
  REQUIRE(
      Accepts("{\"b\":{\"x\":[1,2.5,\"s\"],\"e\":[],\"o\":{}},\"a\":[{\"k\":true},[null]],"
              "\"t\":\"q\\\"\\\\\\n\\u0001\xC3\xA9/\"}",
              &v));
  REQUIRE(json::Serialize(v) ==
          "{\n"
          "  \"b\": {\n"
          "    \"x\": [1, 2.5, \"s\"],\n"
          "    \"e\": [],\n"
          "    \"o\": {}\n"
          "  },\n"
          "  \"a\": [\n"
          "    {\n"
          "      \"k\": true\n"
          "    },\n"
          "    [null]\n"
          "  ],\n"
          "  \"t\": \"q\\\"\\\\\\n\\u0001\xC3\xA9/\"\n"
          "}\n");
  // Serialize then parse is the identity on content.
  json::Value back;
  REQUIRE(Accepts(json::Serialize(v), &back));
  REQUIRE(json::Equal(v, back));
  std::string s;
  json::AppendQuoted(s, std::string("\b\f\r\t\x1F\x7F", 6));
  REQUIRE(s == "\"\\b\\f\\r\\t\\u001f\x7F\"");
}

TEST_CASE("json: integers", "[json]") {
  int64_t x = 0;
  REQUIRE(json::ParseInt("0", &x) == json::IntStatus::Ok);
  REQUIRE(x == 0);
  REQUIRE(json::ParseInt("-0", &x) == json::IntStatus::Ok);
  REQUIRE(x == 0);
  REQUIRE(json::ParseInt("9223372036854775807", &x) == json::IntStatus::Ok);
  REQUIRE(x == INT64_MAX);
  REQUIRE(json::ParseInt("-9223372036854775808", &x) == json::IntStatus::Ok);
  REQUIRE(x == INT64_MIN);
  REQUIRE(json::ParseInt("9223372036854775808", &x) == json::IntStatus::Range);
  REQUIRE(json::ParseInt("-9223372036854775809", &x) == json::IntStatus::Range);
  REQUIRE(json::ParseInt("99999999999999999999999", &x) == json::IntStatus::Range);
  REQUIRE(json::ParseInt("1.0", &x) == json::IntStatus::NotInteger);
  REQUIRE(json::ParseInt("1e2", &x) == json::IntStatus::NotInteger);
  REQUIRE(json::ParseInt("0.5", &x) == json::IntStatus::NotInteger);
  REQUIRE(json::ValidUtf8("a\xC3\xA9"));
  REQUIRE_FALSE(json::ValidUtf8("\xC3"));
}
