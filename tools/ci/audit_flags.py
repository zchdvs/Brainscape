#!/usr/bin/env python3
"""Floating-point flag audit over compile_commands.json (determinism profile §3.2, §3.4).

Every translation unit is checked for forbidden flags (the fast-math family, narrowed
literals, non-IEEE denormal modes, x87, LTO: cmake/fp-forbidden-flags.txt, the list the
configure-time check reads too) and for contraction on; engine translation units
(dsp/src/) must also carry the profile's required flags, judged by the LAST occurrence
where a later flag overrides an earlier one. A -ffp-model= sets contraction too (precise
means on), so -ffp-contract=off must follow every one of them (§3.2).
compile_commands.json comes from the Ninja and Makefile generators only
(-DCMAKE_EXPORT_COMPILE_COMMANDS=ON). Exit 1 on any finding.

  audit_flags.py build/compile_commands.json [--engine-dir dsp/src/]
"""
import argparse
import json
import os
import re
import shlex
import sys

FORBIDDEN_LIST = os.path.join(os.path.dirname(os.path.abspath(__file__)), os.pardir, os.pardir,
                              "cmake", "fp-forbidden-flags.txt")


def load_forbidden(path=FORBIDDEN_LIST):
    """One regex per line; wrapped exactly as cmake/BrainscapeFpProfile.cmake wraps them."""
    with open(path, encoding="ascii") as fh:
        patterns = [ln.strip() for ln in fh if ln.strip() and not ln.lstrip().startswith("#")]
    if not patterns:
        sys.exit(f"audit_flags: no entries in {path}")
    return re.compile("(^|[ ;:>,\"'])(" + "|".join(patterns) + ")($|[ ;>,\"'])")


FORBIDDEN = load_forbidden()
REQUIRED = {
    "gcc": ["-fno-math-errno", "-fno-fast-math"],
    "clang": ["-fno-math-errno"],
    "arm": ["-mcpu=cortex-m7", "-mthumb", "-mfpu=fpv5-d16", "-mfloat-abi=hard", "-O3",
            "-fno-math-errno", "-fno-exceptions", "-fno-rtti"],
}
# The contraction each Clang -ffp-model= implies.
MODEL_CONTRACT = {"precise": "on", "fast": "fast", "aggressive": "fast", "strict": "off"}


def family(argv):
    exe = os.path.basename(argv[0]).lower()
    if exe in ("cl", "cl.exe"):
        return "msvc"
    if "arm-none-eabi" in exe:
        return "arm"
    if "clang" in exe:
        return "clang"
    return "gcc"


def last(argv, prefix):
    """Value of the last `prefix<value>` argument, or None."""
    vals = [a[len(prefix):] for a in argv if a.startswith(prefix)]
    return vals[-1] if vals else None


def contraction(argv):
    """(effective contraction, whether a -ffp-model= follows the last -ffp-contract=)."""
    value, model_after = None, False
    for a in argv:
        if a.startswith("-ffp-contract="):
            value, model_after = a[len("-ffp-contract="):], False
        elif a.startswith("-ffp-model="):
            value, model_after = MODEL_CONTRACT.get(a[len("-ffp-model="):], value), True
    return value, model_after


def audit(argv, engine):
    fam = family(argv)
    found = []
    for a in argv[1:]:
        m = FORBIDDEN.search(a)
        if m:
            found.append(f"forbidden {m.group(2)}")
    if fam == "msvc":
        flags = [a.lower().replace("-", "/", 1) if a.startswith("-") else a.lower() for a in argv]
        # Any spelling cl accepts, unless the shared list already reported it.
        seen = {x.split()[-1].lower().replace("-", "/", 1) for x in found}
        for f in ("/fp:fast", "/fp:contract", "/gl"):
            if f in flags and f not in seen:
                found.append(f"forbidden {f}")
        model = [f for f in flags if f.startswith("/fp:")]
        if engine and model and model[-1] not in ("/fp:precise", "/fp:strict"):
            found.append(f"{model[-1]} is not /fp:precise or /fp:strict")
        return fam, found
    contract, model_after = contraction(argv)
    if contract not in (None, "off"):  # fast, on, fast-honor-pragmas
        found.append(f"forbidden -ffp-contract={contract} (effective)")
    if engine:
        if contract != "off":
            found.append("missing -ffp-contract=off" if contract is None else
                         f"-ffp-contract={contract} instead of off")
        elif model_after:
            found.append("-ffp-model= after -ffp-contract=off")
        for req in REQUIRED[fam]:
            if req.startswith("-O"):
                opt = [a for a in argv if a.startswith("-O")]
                if not opt or opt[-1] != req:
                    found.append(f"missing {req} (effective {opt[-1] if opt else 'none'})")
            elif req.startswith("-fno-"):
                # -fno-X holds only if no later -fX turns it back on.
                pair = [a for a in argv if a in (req, "-f" + req[5:])]
                if not pair or pair[-1] != req:
                    found.append(f"missing {req}" if not pair else f"{pair[-1]} overrides {req}")
            elif "=" in req:
                prefix, want = req.split("=", 1)
                got = last(argv, prefix + "=")
                if got != want:
                    found.append(f"missing {req}" if got is None else f"{prefix}={got} overrides {req}")
            elif req == "-mthumb":
                isa = [a for a in argv if a in ("-mthumb", "-marm")]
                if not isa or isa[-1] != req:
                    found.append(f"missing {req}" if not isa else f"{isa[-1]} overrides {req}")
            elif req not in argv:
                found.append(f"missing {req}")
    return fam, found


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("compile_commands")
    ap.add_argument("--engine-dir", default="dsp/src/", help="path fragment marking engine TUs")
    args = ap.parse_args()
    with open(args.compile_commands, encoding="utf-8") as fh:
        entries = json.load(fh)

    findings = 0
    for e in entries:
        argv = e["arguments"] if "arguments" in e else shlex.split(e["command"])
        path = e["file"].replace("\\", "/")
        engine = args.engine_dir in path
        fam, found = audit(argv, engine)
        rel = path.split("/dsp/", 1)[-1] if "/dsp/" in path else path
        label = f"{'engine' if engine else 'other '} {fam:5s} {rel}"
        if found:
            findings += len(found)
            print(f"FAIL  {label}: " + "; ".join(found))
        else:
            print(f"ok    {label}")
    print(f"\n{findings} flag finding(s) in {len(entries)} translation unit(s)")
    return 1 if findings else 0


if __name__ == "__main__":
    sys.exit(main())
