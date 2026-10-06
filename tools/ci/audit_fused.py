#!/usr/bin/env python3
"""Fused multiply-add audit of dsp/ objects (determinism profile §3.2, §6.3).

Counts fused instructions per object in a disassembly and fails (exit 1) when any
object's count differs from the expected one (0 while §7.3's explicit FMA is not
adopted). --self-test names an object built from a known contracting kernel; the
pattern must find at least one fused instruction there, or the audit is blind.

  audit_fused.py --objdump OBJDUMP --arch armv7|aarch64|x86 [--expect N]
                 [--self-test OBJ] FILES...
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


def count(objdump, path, pattern):
    """{object: fused instruction count} from `objdump -d`."""
    out = subprocess.run([objdump, "-d", path], capture_output=True, text=True, check=True).stdout
    counts, cur = {}, path
    for line in out.splitlines():
        m = re.match(r"^(\S.*?):\s+file format", line)
        if m:
            cur = m.group(1)
            counts.setdefault(cur, 0)
            continue
        # "  addr:  bytes  mnemonic operands" in GNU and LLVM objdump alike; the bytes
        # are hex pairs, so only the mnemonic can match.
        m = re.match(r"^\s*[0-9a-f]+:\s(.*)$", line)
        if m and pattern.search(m.group(1)):
            counts[cur] = counts.get(cur, 0) + 1
    return counts


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("files", nargs="+")
    ap.add_argument("--objdump", default="objdump")
    ap.add_argument("--arch", required=True, choices=sorted(PATTERNS))
    ap.add_argument("--expect", type=int, default=0, help="expected count per object")
    ap.add_argument("--self-test", help="object compiled from a contracting kernel")
    args = ap.parse_args()
    pattern = PATTERNS[args.arch]

    if args.self_test:
        n = sum(count(args.objdump, args.self_test, pattern).values())
        print(f"self-test {args.self_test}: {n} fused instruction(s)")
        if n == 0:
            print("FAIL  the pattern found nothing in a known contracting kernel")
            return 2

    total = bad = 0
    for f in args.files:
        for obj, n in sorted(count(args.objdump, f, pattern).items()):
            total += n
            status = "ok  " if n == args.expect else "FAIL"
            bad += n != args.expect
            print(f"{status}  {obj}: {n}")
    print(f"\n{total} fused instruction(s) in total ({args.arch}); "
          f"{bad} object(s) differ from the expected {args.expect}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
