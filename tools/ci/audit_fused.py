#!/usr/bin/env python3
"""Fused multiply-add audit of dsp/ objects (determinism profile §3.2, §6.3).

Counts fused instructions per object in a disassembly and fails (exit 1) when any
object's count differs from the expected one (0 while §7.3's explicit FMA is not
adopted). --self-test names an object built from a known contracting kernel; the
pattern must find at least one fused instruction there, or the audit is blind. Audit
a build whose target HAS fused instructions (x86-64-v3, /arch:AVX2, Armv7E-M with
FPv5, AArch64): baseline x86-64 cannot fail.

  audit_fused.py [--objdump OBJDUMP | --dumpbin DUMPBIN] --arch armv7|aarch64|x86
                 [--expect N] [--self-test OBJ] FILES...

With --dumpbin, pass MSVC objects (.obj), not the .lib.
"""
import argparse
import re
import subprocess
import sys

PATTERNS = {
    # Armv7 VFMA/VFMS/VFNMA/VFNMS; the unfused VMLA/VMLS are allowed.
    "armv7": re.compile(r"\bvfn?m[as]\b"),
    # AArch64 scalar FMADD/FMSUB/FNMADD/FNMSUB and vector FMLA/FMLS (Clang emits those in
    # vectorized loops), plus the widening FMLAL/FMLSL.
    "aarch64": re.compile(r"\bfn?m(add|sub|la|ls)\b|\bfml[as]l2?\b"),
    "x86": re.compile(r"\bvfn?m(add|sub|addsub|subadd)(132|213|231)?[ps][sd]\b"),
}
# "  addr:  bytes  mnemonic operands" in GNU objdump, llvm-objdump and dumpbin alike.
# objdump prints lowercase hex and dumpbin uppercase, so only the mnemonic can match.
INSN = re.compile(r"^\s*[0-9a-fA-F]+:\s(.*)$")
HEADERS = {
    "objdump": re.compile(r"^(\S.*?):\s+file format"),
    "dumpbin": re.compile(r"^Dump of file (.+?)\s*$"),
}


def count(tool, dumpbin, path, pattern):
    """{object: fused instruction count} from `objdump -d` or `dumpbin /disasm`."""
    cmd = [tool, "/nologo", "/disasm", path] if dumpbin else [tool, "-d", path]
    out = subprocess.run(cmd, capture_output=True, text=True, check=True).stdout
    header = HEADERS["dumpbin" if dumpbin else "objdump"]
    counts, cur = {}, path
    for line in out.splitlines():
        m = header.match(line)
        if m:
            cur = m.group(1)
            counts.setdefault(cur, 0)
            continue
        m = INSN.match(line)
        if m and pattern.search(m.group(1)):
            counts[cur] = counts.get(cur, 0) + 1
    return counts


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("files", nargs="+")
    ap.add_argument("--objdump", default="objdump")
    ap.add_argument("--dumpbin", help="use MSVC dumpbin /disasm instead of objdump")
    ap.add_argument("--arch", required=True, choices=sorted(PATTERNS))
    ap.add_argument("--expect", type=int, default=0, help="expected count per object")
    ap.add_argument("--self-test", help="object compiled from a contracting kernel")
    args = ap.parse_args()
    pattern = PATTERNS[args.arch]
    tool, dumpbin = (args.dumpbin, True) if args.dumpbin else (args.objdump, False)

    if args.self_test:
        n = sum(count(tool, dumpbin, args.self_test, pattern).values())
        print(f"self-test {args.self_test}: {n} fused instruction(s)")
        if n == 0:
            print("FAIL  the pattern found nothing in a known contracting kernel")
            return 2

    total = bad = 0
    for f in args.files:
        for obj, n in sorted(count(tool, dumpbin, f, pattern).items()):
            total += n
            status = "ok  " if n == args.expect else "FAIL"
            bad += n != args.expect
            print(f"{status}  {obj}: {n}")
    print(f"\n{total} fused instruction(s) in total ({args.arch}); "
          f"{bad} object(s) differ from the expected {args.expect}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
