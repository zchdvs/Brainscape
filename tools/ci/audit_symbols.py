#!/usr/bin/env python3
"""Undefined-symbol audit of dsp/ objects (determinism profile §3.9, §6.3).

Lists every symbol the given archives/objects import (undefined and not defined by
another audited object) and fails (exit 1) on any symbol outside the profile's
allowlist. libm calls are flagged as such: a conforming build imports no libm at
all, because DetMath replaces every transcendental (§3.9) and inline square roots
need -fno-math-errno.

  audit_symbols.py --toolchain gcc|clang|arm|msvc [--nm NM | --dumpbin DUMPBIN] FILES...

Pass MSVC objects (.obj), not the .lib: dumpbin does not name archive members.
"""
import argparse
import re
import subprocess
import sys

ALWAYS = {"memset", "memmove", "memcpy"}
EXTRA = {
    # Plus every __aeabi_* EABI integer/conversion helper (__aeabi_uldivmod,
    # __aeabi_d2lz, __aeabi_l2d, ...); __assert_func appears in Debug builds.
    "arm": {"__assert_func"},
    "msvc": {"_fltused", "__security_cookie", "__security_check_cookie", "__GSHandlerCheck",
             "__ImageBase", "_wassert"},
    # Distribution compilers add stack protection and PIE by default.
    "gcc": {"__stack_chk_fail", "__stack_chk_guard", "_GLOBAL_OFFSET_TABLE_", "__assert_fail"},
    "clang": {"__stack_chk_fail", "__stack_chk_guard", "_GLOBAL_OFFSET_TABLE_", "__assert_fail",
              "__assert_rtn"},
}
LIBM = re.compile(
    r"^_?(sin|cos|tan|asin|acos|atan|atan2|sinh|cosh|tanh|asinh|acosh|atanh|exp|exp2|expm1|exp10|"
    r"log|log2|log10|log1p|logb|pow|sqrt|cbrt|hypot|ceil|floor|round|lround|llround|lrint|llrint|"
    r"rint|nearbyint|trunc|fmod|remainder|remquo|fabs|fma|fmin|fmax|fdim|frexp|ldexp|scalbn|modf|"
    r"sincos|erf|erfc|tgamma|lgamma|copysign|nan)[fl]?$|^__(sin|cos|exp|log|pow)\w*|^__libm\w*")


def plain(name):
    """The C name behind a Mach-O underscore or an MSVC DLL-import thunk (__imp_)."""
    if name.startswith("__imp_"):
        name = name[len("__imp_"):]
    return name[1:] if name.startswith("_") and name[1:2] != "_" else name


def nm_symbols(tool, path):
    """({object: undefined symbols}, defined symbols) from POSIX-format `nm`."""
    out = subprocess.run([tool, "-P", path], capture_output=True, text=True, check=True).stdout
    undef, defined, cur = {}, set(), path
    for line in out.splitlines():
        line = line.strip()
        if not line:
            continue
        if line.endswith(":"):  # "archive[member]:" or "member:"
            cur = line[:-1]
            undef.setdefault(cur, set())
            continue
        fields = line.split()
        if len(fields) < 2:
            continue
        name, kind = fields[0], fields[1]
        if kind == "U" or (kind in ("w", "v") and len(fields) == 2):
            undef.setdefault(cur, set()).add(name)
        else:
            defined.add(name)
    return undef, defined


def dumpbin_symbols(tool, path):
    out = subprocess.run([tool, "/nologo", "/symbols", path], capture_output=True, text=True,
                         check=True).stdout
    undef, defined, cur = {}, set(), path
    for line in out.splitlines():
        m = re.match(r"\s*Dump of file (.+)", line)
        if m:
            cur = m.group(1)
            undef.setdefault(cur, set())
            continue
        if "External" in line and "|" in line:
            name = line.split("|", 1)[1].strip().split()[0]
            if " UNDEF " in line:
                undef.setdefault(cur, set()).add(name)
            else:
                defined.add(name)
    return undef, defined


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("files", nargs="+")
    ap.add_argument("--toolchain", required=True, choices=sorted(EXTRA))
    ap.add_argument("--nm", default="nm")
    ap.add_argument("--dumpbin")
    args = ap.parse_args()

    allowed = ALWAYS | EXTRA[args.toolchain]
    undef, defined = {}, set()
    for f in args.files:
        u, d = dumpbin_symbols(args.dumpbin, f) if args.dumpbin else nm_symbols(args.nm, f)
        undef.update(u)
        defined |= d

    findings = libm = 0
    for obj in sorted(undef):
        bad = sorted(s for s in undef[obj]
                     if s not in defined and s not in allowed and plain(s) not in allowed
                     and not (args.toolchain == "arm" and s.startswith("__aeabi_")))
        if not bad:
            print(f"ok    {obj}")
            continue
        for s in bad:
            kind = "libm " if LIBM.match(plain(s)) else "other"
            libm += kind == "libm "
            print(f"FAIL  {obj}: {kind} {s}")
        findings += len(bad)
    print(f"\n{findings} imported symbol(s) outside the allowlist, {libm} of them libm "
          f"({args.toolchain}; the profile requires 0)")
    return 1 if findings else 0


if __name__ == "__main__":
    sys.exit(main())
