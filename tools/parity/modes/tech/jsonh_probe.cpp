// PROBE: what dsp/tests/golden/Json.h accepts, against RFC 8259 and a mode compiler's needs.
#include <cstdio>
#include <string>
#include "Json.h"
using namespace brainscape::golden;
int main() {
  struct C { const char* what; std::string text; } cs[] = {
    {"float number 0.55", "{\"a\": 0.55}"},
    {"exponent 1e3", "{\"a\": 1e3}"},
    {"leading zeros 007 (not JSON)", "{\"a\": 007}"},
    {"duplicate key", "{\"a\": 1, \"a\": 2}"},
    {"escape backslash-r", "{\"a\": \"x\\ry\"}"},
    {"escape backslash-b", "{\"a\": \"x\\by\"}"},
    {"escape backslash-u00e9 (non-ASCII)", "{\"a\": \"caf\\u00e9\"}"},
    {"raw CR 0x0D in string (not JSON)", "{\"a\": \"x\ry\"}"},
    {"raw UTF-8 e-acute", "{\"a\": \"caf\xc3\xa9\"}"},
    {"invalid UTF-8 byte 0xFF", "{\"a\": \"x\xffy\"}"},
    {"raw control char 0x01 in string (not JSON)", std::string("{\"a\": \"x\x01y\"}")},
    {"int 2^62+1", "{\"a\": 4611686018427387905}"},
    {"depth 33 arrays", std::string(33, '[') + std::string(33, ']')},
    {"trailing comma", "{\"a\": 1,}"},
  };
  for (const C& c : cs) {
    Json j; std::string err;
    const bool ok = FromText(c.text, &j, &err);
    std::string detail;
    if (ok && j.type == Json::Type::Object) detail = " members=" + std::to_string(j.members.size());
    std::printf("%-44s %s%s %s\n", c.what, ok ? "ACCEPTED" : "rejected", detail.c_str(), ok ? "" : err.c_str());
  }
}
