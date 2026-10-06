#!/usr/bin/env python3
"""Floating-point flag audit over compile_commands.json (determinism profile §3.2, §3.4).

Every translation unit is checked for forbidden flags (fast-math family, contraction
on, LTO); engine translation units (dsp/src/) must also carry the profile's required
flags, judged by the LAST occurrence where a later flag overrides an earlier one.
compile_commands.json comes from the Ninja and Makefile generators only
(-DCMAKE_EXPORT_COMPILE_COMMANDS=ON). Exit 1 on any finding.

  audit_flags.py build/compile_commands.json [--engine-dir dsp/src/]
"""
import argparse
import json
import os
import shlex
import sys

FORBIDDEN_GNU = {"-ffast-math", "-Ofast", "-funsafe-math-optimizations", "-fassociative-math",
                 "-freciprocal-math", "-ffinite-math-only", "-fno-signed-zeros", "-ffp-model=fast"}
REQUIRED = {
    "gcc": ["-fno-math-errno", "-fno-fast-math"],
    "clang": ["-fno-math-errno"],
    "arm": ["-mcpu=cortex-m7", "-mthumb", "-mfpu=fpv5-d16", "-mfloat-abi=hard", "-O3",
            "-fno-math-errno", "-fno-exceptions", "-fno-rtti"],
}


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


def audit(argv, engine):
    fam = family(argv)
    found = []
    if fam == "msvc":
        flags = [a.lower().replace("-", "/", 1) if a.startswith("-") else a.lower() for a in argv]
        for f in ("/fp:fast", "/fp:contract", "/gl"):
            if f in flags:
                found.append(f"forbidden {f}")
        model = [f for f in flags if f.startswith("/fp:")]
        if engine and model and model[-1] not in ("/fp:precise", "/fp:strict"):
            found.append(f"{model[-1]} is not /fp:precise or /fp:strict")
        return fam, found
    for a in argv:
        if a in FORBIDDEN_GNU or a.startswith("-flto"):
            found.append(f"forbidden {a}")
    contract = last(argv, "-ffp-contract=")
    if contract in ("fast", "on"):
        found.append(f"forbidden -ffp-contract={contract} (effective)")
    if engine:
        if contract != "off":
            found.append("missing -ffp-contract=off" if contract is None else
                         f"-ffp-contract={contract} instead of off")
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
