#!/usr/bin/env python3
"""bspc-roundtrip (mode-compiler.md §8.3, §10.1; companion §3.4): every committed preset
document compiles to its committed package byte for byte and is canonical, on every host leg,
and every leg writes the same sorted manifest of package hashes.

  bspc_roundtrip.py run --bspc PATH [--out FILE]
      Runs `bspc roundtrip` over each document set below that exists, in the set's own
      directory (so manifest paths are the set's), with `--expect MANIFEST` where the set
      keeps one. A set that commits packages needs a .bsp beside every .json and a .json
      beside every .bsp. `bspc roundtrip` checks each document: it compiles (to its committed
      .bsp when there is one), passes fmt --check with a current stamp, decompiles to itself,
      rebuilds without its JSON section, and its JSON section recompiles to the same bytes.
      Writes the sets' manifests, each path prefixed with its set, sorted by path, to --out.

  bspc_roundtrip.py compare --legs A,B,... [--summary FILE] DIR
      parity-summary's check: the manifests under DIR (bspc-manifest-<leg>.txt) of every
      listed leg exist and are byte-identical; prints the first differences by document.

Document sets:
  compiler/tests/data        the compiler's example documents; MANIFEST holds their hashes
                             (no package is committed before sound revision 2, §7.6 item 3)
  dsp/tests/golden/presets   the golden corpus's package presets (§10.3): each .json beside
                             its .bsp; frozen/ is exempt (never rebuilt or re-stamped)
  firmware/factory           the factory presets: each .json beside its .bsp, and MANIFEST,
                             which the sound-revision gate's package rule reads
"""
import argparse
import os
import subprocess
import sys
import tempfile

# (directory, commits packages, excluded subdirectories)
SETS = [
    ("compiler/tests/data", False, ()),
    ("dsp/tests/golden/presets", True, ("frozen",)),
    ("firmware/factory", True, ()),
]
MANIFEST = "MANIFEST"
HASHES = 195  # three 64-digit hashes and their spaces, then the path


def documents(root, excluded):
    """(json, bsp) paths relative to root, '/'-separated and sorted."""
    jsons, bsps = [], []
    for d, dirs, files in os.walk(root):
        rel = os.path.relpath(d, root).replace(os.sep, "/")
        dirs[:] = sorted(x for x in dirs if not (rel == "." and x in excluded))
        for f in files:
            path = f if rel == "." else f"{rel}/{f}"
            if f.endswith(".json"):
                jsons.append(path)
            elif f.endswith(".bsp"):
                bsps.append(path)
    return sorted(jsons), sorted(bsps)


def run(bspc, out):
    lines, failures, ran = [], 0, 0
    for root, packaged, excluded in SETS:
        if not os.path.isdir(root):
            print(f"{root}: absent, skipped")
            continue
        jsons, bsps = documents(root, excluded)
        if not jsons and not bsps:
            print(f"{root}: no documents, skipped")
            continue
        stems = {p[:-len(".json")] for p in jsons}
        orphans = [p for p in bsps if p[:-len(".bsp")] not in stems]
        missing = [p for p in jsons if packaged and p[:-len(".json")] + ".bsp" not in bsps]
        for p in orphans:
            print(f"::error::{root}/{p}: a committed package without its document")
        for p in missing:
            print(f"::error::{root}/{p}: no committed package beside it (bspc compile, then commit the .bsp)")
        failures += len(orphans) + len(missing)
        if not jsons:
            continue
        with tempfile.TemporaryDirectory() as tmp:
            got = os.path.join(tmp, "manifest.txt")
            cmd = [bspc, "roundtrip", "--write-manifest", got]
            if os.path.isfile(os.path.join(root, MANIFEST)):
                cmd += ["--expect", MANIFEST]
            cmd += ["--"] + jsons
            print(f"{root}: bspc roundtrip over {len(jsons)} document(s)"
                  + (f", against {MANIFEST}" if "--expect" in cmd else ""), flush=True)
            rc = subprocess.run(cmd, cwd=root).returncode
            ran += 1
            if rc != 0:
                print(f"::error::{root}: bspc roundtrip exit {rc}")
                failures += 1
            if os.path.isfile(got):
                with open(got, encoding="utf-8", newline="") as fh:
                    for line in fh.read().splitlines():
                        lines.append(line[:HASHES] + root + "/" + line[HASHES:])
    lines.sort(key=lambda ln: ln[HASHES:])
    text = "".join(ln + "\n" for ln in lines)
    if out:
        with open(out, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(text)
    print(f"\n{len(lines)} document(s) in {ran} set(s); {failures} failure(s)")
    if ran == 0:
        print("::error::no document set found: run from the repository root")
        failures += 1
    return 1 if failures else 0


def compare(directory, legs, summary):
    manifests, out, problems = {}, ["# bspc-roundtrip: package manifests across legs", ""], 0
    for leg in legs:
        path = os.path.join(directory, f"bspc-manifest-{leg}.txt")
        if not os.path.isfile(path):
            out.append(f"- **`{leg}`: no manifest** (the leg failed before writing it)")
            problems += 1
            continue
        with open(path, encoding="utf-8", newline="") as fh:
            manifests[leg] = fh.read()
    if manifests:
        ref = legs[0] if legs[0] in manifests else sorted(manifests)[0]
        want = {ln[HASHES:]: ln[:HASHES] for ln in manifests[ref].splitlines()}
        out.append(f"Reference `{ref}`: {len(want)} document(s).")
        out.append("")
        for leg, text in manifests.items():
            if leg == ref:
                continue
            if text == manifests[ref]:
                out.append(f"- `{leg}`: identical")
                continue
            problems += 1
            have = {ln[HASHES:]: ln[:HASHES] for ln in text.splitlines()}
            diffs = [f"{p}: differs" for p in sorted(want) if p in have and have[p] != want[p]]
            diffs += [f"{p}: missing" for p in sorted(want) if p not in have]
            diffs += [f"{p}: extra" for p in sorted(have) if p not in want]
            out.append(f"- **`{leg}` differs** from `{ref}`: " + ("; ".join(diffs[:10]) or "line endings or order")
                       + ("; ..." if len(diffs) > 10 else ""))
    out += ["", "PASS" if not problems else f"FAIL: {problems} leg(s)"]
    text = "\n".join(out) + "\n"
    print(text)
    if summary:
        with open(summary, "a", encoding="utf-8") as fh:
            fh.write(text)
    return 1 if problems else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run", help="bspc roundtrip over every document set (one leg)")
    r.add_argument("--bspc", required=True)
    r.add_argument("--out", help="write the combined manifest here")
    c = sub.add_parser("compare", help="require every leg's manifest identical (parity-summary)")
    c.add_argument("--legs", required=True, help="comma list; the first is the reference")
    c.add_argument("--summary", help="also append the Markdown to this file ($GITHUB_STEP_SUMMARY)")
    c.add_argument("directory")
    args = ap.parse_args()
    if args.cmd == "run":
        return run(os.path.abspath(args.bspc), args.out)
    return compare(args.directory, [x for x in args.legs.split(",") if x], args.summary)


if __name__ == "__main__":
    sys.exit(main())
