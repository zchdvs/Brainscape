// The same object with integer-only formatting, as an error message would use.
#include <cstdio>
#include <string>
int PrintInt(char* b, int x) { return std::snprintf(b, 32, "%d", x); }
std::string StrInt(int x) { return std::to_string(x); }
