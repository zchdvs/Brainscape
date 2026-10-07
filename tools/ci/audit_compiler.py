#!/usr/bin/env python3
"""The compiler audit (mode-compiler.md §8.3, §1.4 principle 3): brainscape_compiler converts
numbers only through its own exact code (compiler/src/Number.*), never through the C or C++
library, whose float conversions differ by platform, consult the locale or are missing on
macOS (record §2.3), so package bytes cannot depend on the host.

  audit_compiler.py sources [DIR...]
      The source ban, over the brainscape_compiler target's sources (default compiler/src;
      the tests are exempt, since they cross-check against std::from_chars, to_chars and
      printf). Comments and string literals are skipped. Fails (exit 1) on an include of
      <charconv>, <cmath>, <math.h> (or <tgmath.h>, <ctgmath>), a stream or locale header
      (<sstream>, <iomanip>, <iostream>, <istream>, <ostream>, <fstream>, <strstream>,
      <locale>, <clocale>, <format>), or a use of the strto*, wcsto*, ato* and sto* families,
      std::to_string, the printf and scanf families, to_chars, from_chars, string streams,
      stream number manipulators or the other float formatters (ecvt, fcvt, gcvt, strfrom*).

  audit_compiler.py imports --toolchain gcc|clang|appleclang [--nm NM] FILES...
      The import check, for the GCC and Clang legs (on MSVC the float conversions are
      header-only and import nothing that names them, record §2.8): fails on any symbol the
      archive imports from libm, the strto/wcsto/ato families (strtof and strtod above all),
      the printf and scanf families, the float formatters, or a std::to_chars, from_chars,
      to_string, string-stream or num_put/num_get definition.

  audit_compiler.py self-test [--toolchain T --nm NM --object OBJ]
      The source ban against built-in cases that it must and must not flag; with --object,
      the import check against an object built from tools/ci/compiler_audit_selftest.cpp,
      which it must reject (a check that cannot fail proves nothing).
"""
import argparse
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from audit_symbols import LIBM, nm_symbols  # noqa: E402  (same directory)

SOURCE_SUFFIXES = (".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inc", ".ipp")
BANNED_HEADERS = {
    "charconv": "number conversion outside compiler/src/Number",
    "cmath": "floating-point library", "math.h": "floating-point library",
    "tgmath.h": "floating-point library", "ctgmath": "floating-point library",
    "sstream": "stream number formatting", "iomanip": "stream number formatting",
    "iostream": "stream number formatting", "istream": "stream number formatting",
    "ostream": "stream number formatting", "fstream": "stream number formatting",
    "strstream": "stream number formatting", "format": "library number formatting",
    "locale": "locale-dependent formatting", "clocale": "locale-dependent formatting",
}
BANNED_NAMES = [
    (re.compile(r"\b(?:__isoc\d+_)?(?:strto|wcsto)\w+"), "the strto*/wcsto* family"),
    (re.compile(r"\b(?:_?w?ato(?:f|i|l|ll|i64)(?:_l)?|_atodbl|_atoflt|_atoldbl)\b"), "the ato* family"),
    (re.compile(r"\bsto(?:i|l|ll|ul|ull|f|d|ld)\b"), "the sto* family"),
    (re.compile(r"\bto_w?string\b"), "std::to_string"),
    (re.compile(r"\b\w*printf\w*"), "the printf family"),
    (re.compile(r"\b\w*scanf\w*"), "the scanf family"),
    (re.compile(r"\b(?:to_chars|from_chars)\b"), "<charconv> conversion"),
    (re.compile(r"\b(?:w?[io]?stringstream|w?[io]?strstream|stringbuf|setprecision|setw|setfill|"
                r"num_put|num_get|hexfloat|defaultfloat|scientific)\b"), "stream number formatting"),
    (re.compile(r"\bstd\s*::\s*(?:fixed|cout|cerr|clog|cin|format|vformat|locale|setlocale)\b"),
     "stream or locale formatting"),
    (re.compile(r"\b(?:setlocale|localeconv|_?[efg]cvt(?:_s)?|strfrom[fdl])\b"), "a float formatter or locale"),
]

# Imports a conforming compiler archive never has (besides libm).
IMPORT_C = re.compile(
    r"^(?:__isoc\d+_)?(?:strto|wcsto)\w*$|^__(?:strto|wcsto)\w*$|^_?w?ato(?:f|i|l|ll|i64)(?:_l)?$|"
    r"^(?:__isoc\d+_|__)?\w*(?:printf|scanf)\w*$|^_?[efg]cvt(?:_r|_s)?$|^strfrom[fdl]$|^(?:setlocale|localeconv)$")
IMPORT_CXX = re.compile(r"to_chars|from_chars|to_w?string|stringstream|stringbuf|num_put|num_get")


# ── The source ban ───────────────────────────────────────────────────────────────────────────

RAW_STRING = re.compile(r'(?:u8|u|U|L)?R"([^()\\ \t\n]{0,16})\(')
CHAR_PREFIX = re.compile(r"(?:u8|u|U|L)$")


def strip(text, keep_strings):
    """C++ text with comments (and, unless keep_strings, string and character literals)
    replaced by spaces; newlines stay, so line numbers do."""
    out, i, n = [], 0, len(text)

    def blank(s):
        return "".join("\n" if ch == "\n" else " " for ch in s)

    while i < n:
        c = text[i]
        if text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(blank(text[i:j]))
            i = j
            continue
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(blank(text[i:j]))
            i = j
            continue
        prev = text[i - 1] if i else ""
        m = RAW_STRING.match(text, i) if c in "uULR" and not (prev.isalnum() or prev == "_") else None
        if m:
            end = text.find(")" + m.group(1) + '"', m.end())
            end = n if end < 0 else end + len(m.group(1)) + 2
            out.append(text[i:end] if keep_strings else blank(text[i:end]))
            i = end
            continue
        quote = c in "\"'"
        if c == "'":
            # A digit separator (1'000) is no literal; a prefixed one (L'x', u8'x') is.
            word = re.search(r"[A-Za-z0-9_]+$", text[max(0, i - 8):i])
            quote = word is None or (CHAR_PREFIX.fullmatch(word.group(0)) is not None)
        if quote:
            j = i + 1
            while j < n and text[j] != c and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            j = min(j + 1, n)
            out.append(text[i:j] if keep_strings else blank(text[i:j]))
            i = j
            continue
        out.append(c)
        i += 1
    return "".join(out)


INCLUDE_TOKEN = re.compile(r'<\s*([^<>\s]+)\s*>|"([^"\s]+)"')


def scan_source(text):
    """[(line, what, why)] for every banned include or name in one source text."""
    findings = []
    code_with_strings = strip(text, keep_strings=True).splitlines()
    code = strip(text, keep_strings=False).splitlines()
    for n, line in enumerate(code_with_strings, 1):
        if not re.match(r"\s*#", line):
            continue
        for m in INCLUDE_TOKEN.finditer(line):
            header = (m.group(1) or m.group(2)).replace("\\", "/").rsplit("/", 1)[-1]
            if header in BANNED_HEADERS:
                findings.append((n, f"<{header}>", BANNED_HEADERS[header]))
    for n, line in enumerate(code, 1):
        for rx, why in BANNED_NAMES:
            for m in rx.finditer(line):
                findings.append((n, m.group(0), why))
    return findings


def source_files(roots):
    files = []
    for root in roots:
        if os.path.isfile(root):
            files.append(root)
            continue
        if not os.path.isdir(root):
            sys.exit(f"audit_compiler: no such file or directory: {root}")
        for d, _, fs in os.walk(root):
            files += [os.path.join(d, f) for f in fs if f.endswith(SOURCE_SUFFIXES)]
    return sorted(files)


def cmd_sources(roots):
    files = source_files(roots)
    total = 0
    for path in files:
        with open(path, encoding="utf-8", errors="replace") as fh:
            findings = scan_source(fh.read())
        for line, what, why in findings:
            print(f"FAIL  {path}:{line}: {what} ({why})")
        if not findings:
            print(f"ok    {path}")
        total += len(findings)
    print(f"\n{total} banned include(s) or name(s) in {len(files)} file(s) under {', '.join(roots)} "
          f"(mode-compiler.md §8.3 requires 0)")
    return 1 if total or not files else 0


# ── The import check ─────────────────────────────────────────────────────────────────────────

def plain(name, macho):
    return name[1:] if macho and name.startswith("_") else name


def banned_import(name, macho):
    p = plain(name, macho)
    if LIBM.match(p):
        return "libm"
    if IMPORT_C.match(p):
        return "C library number conversion"
    if p.startswith("_Z") and IMPORT_CXX.search(p):
        return "C++ library number conversion"
    return None


def cmd_imports(toolchain, nm, files):
    macho = toolchain == "appleclang"
    undef, defined = {}, set()
    for f in files:
        u, d = nm_symbols(nm, f)
        undef.update(u)
        defined |= d
    findings = 0
    for obj in sorted(undef):
        bad = sorted((s, why) for s in undef[obj] if s not in defined
                     for why in [banned_import(s, macho)] if why)
        for s, why in bad:
            print(f"FAIL  {obj}: {why}: {s}")
        if not bad:
            print(f"ok    {obj}")
        findings += len(bad)
    print(f"\n{findings} banned import(s) ({toolchain}; mode-compiler.md §8.3 requires 0)")
    return 1 if findings else 0


# ── Self-test ────────────────────────────────────────────────────────────────────────────────

MUST_FLAG = [
    "#include <charconv>", "#  include <cmath>", "#include<math.h>", '#include "math.h"',
    "#include <sstream>", "#include <iomanip>", "#include <locale>",
    "float f = std::strtof(s, nullptr);", "double d = strtod(s, &e);", "long v = ::strtol(s, 0, 10);",
    "int i = atoi(s);", "double d = std::atof(s);", "int i = std::stoi(t);", "float f = std::stof(t);",
    "auto s = std::to_string(1.5f);", "std::snprintf(b, 8, \"%g\", x);", "printf(\"%d\", 1);",
    "sscanf(s, \"%f\", &f);", "std::to_chars(b, e, f);", "std::from_chars(b, e, f);",
    "std::ostringstream o;", "o << std::setprecision(9) << x;", "o << std::fixed << x;",
    "_gcvt(x, 9, b);", "strfromf(b, 16, \"%g\", f);", "std::setlocale(LC_ALL, \"C\");",
]
MUST_PASS = [
    "// strtof, std::to_string and printf are banned here",
    "/* std::to_chars(b, e, f); #include <cmath> */",
    "const char* s = \"printf strtod <cmath> to_string\";",
    "const char* r = R\"x(sscanf(\"%f\") std::stoi)x\";",
    "std::atomic<int> a; int store = 1'000'000; char c = 'p'; char d = L'x';",
    "int restore(int stol_count);",
    "#include <cstdint>", "#include \"Number.h\"", "#include <string>",
    "bool ok = bsc::ReadFloat(text, &bits);",
]


def self_test(toolchain, nm, obj):
    failures = 0
    for src in MUST_FLAG:
        if not scan_source(src + "\n"):
            print(f"FAIL  not flagged: {src}")
            failures += 1
    for src in MUST_PASS:
        found = scan_source(src + "\n")
        if found:
            print(f"FAIL  flagged: {src}: {found}")
            failures += 1
    print(f"source ban: {len(MUST_FLAG) + len(MUST_PASS) - failures}/{len(MUST_FLAG) + len(MUST_PASS)} "
          f"cases as expected")
    if obj:
        macho = toolchain == "appleclang"
        undef, _ = nm_symbols(nm, obj)
        names = {plain(s, macho) for syms in undef.values() for s in syms}
        caught = sorted(s for syms in undef.values() for s in syms if banned_import(s, macho))
        print(f"import check: the self-test object imports {', '.join(caught) or 'nothing banned'}")
        for need in ("strtof", "strtod"):
            if not any(re.fullmatch(rf"(?:__isoc\d+_)?{need}", n) for n in names):
                print(f"FAIL  the self-test object does not import {need}: is it built with -O0 and no LTO?")
                failures += 1
        if not any(LIBM.match(n) for n in names):
            print("FAIL  the self-test object imports no libm function")
            failures += 1
        if not caught:
            failures += 1
    return 1 if failures else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("sources", help="the source ban")
    s.add_argument("roots", nargs="*", default=["compiler/src"])
    i = sub.add_parser("imports", help="the import check (GCC and Clang legs)")
    i.add_argument("--toolchain", required=True, choices=("gcc", "clang", "appleclang"))
    i.add_argument("--nm", default="nm")
    i.add_argument("files", nargs="+")
    t = sub.add_parser("self-test", help="both checks against cases they must catch")
    t.add_argument("--toolchain", default="gcc", choices=("gcc", "clang", "appleclang"))
    t.add_argument("--nm", default="nm")
    t.add_argument("--object", help="an object built from tools/ci/compiler_audit_selftest.cpp")
    args = ap.parse_args()
    if args.cmd == "sources":
        return cmd_sources(args.roots)
    if args.cmd == "imports":
        return cmd_imports(args.toolchain, args.nm, args.files)
    return self_test(args.toolchain, args.nm, args.object)


if __name__ == "__main__":
    sys.exit(main())
