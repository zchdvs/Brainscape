#!/usr/bin/env python3
"""bspc-roundtrip (mode-compiler.md §8.3, §10.1; companion §3.4): every committed preset
document compiles to its committed package byte for byte and is canonical, on every host leg,
and every leg writes the same sorted manifest of package hashes.

  bspc_roundtrip.py run --bspc PATH [--out FILE]
      Runs `bspc roundtrip` over each document set below that exists, in the set's own
      directory (so manifest paths are the set's), with `--expect MANIFEST`. Every set with
      documents must commit its MANIFEST (`bspc roundtrip --write-manifest MANIFEST`), so a
      package cannot change without its line changing; the sound-revision gate's package rule
      reads the corpus's and the factory's. A set that commits packages needs a .bsp beside
      every .json and a .json beside every .bsp. .gitattributes must keep every document and
      MANIFEST at LF on checkout ("text eol=lf": canonical JSON is LF, §6.4, and Git for
      Windows converts text to CRLF under core.autocrlf) and every package binary; that is
      checked on every leg, so a missing rule fails on Linux too. `bspc roundtrip` checks each
      document: it compiles (to its committed .bsp when there is one), passes fmt --check with
      a current stamp, decompiles to itself, rebuilds without its JSON section, and its JSON
      section recompiles to the same bytes. The factory set must also pass `bspc lint
      --factory` (mode-compiler.md §2.7: L4, L7-L9 and L5's empty source set are errors for
      factory presets). Writes the sets' manifests, each path prefixed with its set, sorted by
      path, to --out.

  bspc_roundtrip.py compare --legs A,B,... [--summary FILE] DIR
      parity-summary's check: the manifests under DIR (bspc-manifest-<leg>.txt) of every
      listed leg exist and are byte-identical; prints the first differences by document.

  bspc_roundtrip.py self-test
      Run from the repository's root: the committed .gitattributes give every set's documents,
      MANIFEST and packages the rules above, and `run`'s own checks (MANIFEST, line endings,
      packages beside documents, bspc's exit) each fail on a scratch repository built to fail
      them, with a stand-in for bspc.

Document sets:
  compiler/tests/data        the compiler's example documents; MANIFEST holds their hashes
                             (the examples commit no package)
  dsp/tests/golden/presets   the golden corpus's package presets (§10.3): each .json beside
                             its .bsp, and MANIFEST; frozen/ is exempt (never rebuilt or
                             re-stamped)
  firmware/factory           the factory presets: each .json beside its .bsp, and MANIFEST;
                             linted with `bspc lint --factory`
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

# (directory, commits packages, excluded subdirectories, factory lint)
SETS = [
    ("compiler/tests/data", False, (), False),
    ("dsp/tests/golden/presets", True, ("frozen",), False),
    ("firmware/factory", True, (), True),
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


def attribute_problems(top, paths):
    """Paths (relative to top, '/'-separated) whose .gitattributes let a checkout convert them.

    A document or MANIFEST must be "text eol=lf" and a package "binary" (text unset). Asks git,
    so the answer is the same on every host whatever its core.autocrlf.
    """
    if not paths:
        return []
    p = subprocess.run(["git", "check-attr", "-z", "--stdin", "text", "eol"], cwd=top,
                       input="".join(x + "\0" for x in paths).encode("utf-8"), capture_output=True)
    if p.returncode != 0:
        return [f"cannot read .gitattributes (git check-attr exit {p.returncode}: "
                f"{p.stderr.decode('utf-8', 'replace').strip()})"]
    fields = p.stdout.decode("utf-8").split("\0")
    attrs = {}
    for path, name, value in zip(fields[0::3], fields[1::3], fields[2::3]):
        attrs.setdefault(path, {})[name] = value
    out = []
    for path in paths:
        a = attrs.get(path, {})
        if path.endswith(".bsp"):
            if a.get("text") != "unset":
                out.append(f"{path}: .gitattributes does not mark it binary (a package is bytes: '*.bsp binary')")
        elif a.get("eol") != "lf" or a.get("text") == "unset":
            out.append(f"{path}: .gitattributes does not give it 'text eol=lf', so a Windows checkout "
                       f"(core.autocrlf) writes it with CRLF, which bspc rejects")
    return out


def check(bspc, top=".", say=print):
    """Every document set under top. bspc is the command, a list; say prints progress.
    Returns (manifest lines, error messages, sets bspc ran over)."""
    lines, errors, ran = [], [], 0
    for root, packaged, excluded, factory in SETS:
        where = os.path.join(top, root)
        if not os.path.isdir(where):
            say(f"{root}: absent, skipped")
            continue
        jsons, bsps = documents(where, excluded)
        has_manifest = os.path.isfile(os.path.join(where, MANIFEST))
        if not jsons and not bsps and not has_manifest:
            say(f"{root}: no documents, skipped")
            continue
        stems = {p[:-len(".json")] for p in jsons}
        errors += [f"{root}/{p}: a committed package without its document"
                   for p in bsps if p[:-len(".bsp")] not in stems]
        errors += [f"{root}/{p}: no committed package beside it (bspc compile, then commit the .bsp)"
                   for p in jsons if packaged and p[:-len(".json")] + ".bsp" not in bsps]
        if not has_manifest:
            errors.append(f"{root}: no {MANIFEST} (in {root}: bspc roundtrip --write-manifest {MANIFEST} "
                          f"-- <its documents>, then commit it; the package rule reads it)")
        elif not jsons:
            errors.append(f"{root}: a {MANIFEST} but no documents (delete it with them)")
        errors += attribute_problems(top, [f"{root}/{p}" for p in jsons + bsps]
                                     + ([f"{root}/{MANIFEST}"] if has_manifest else []))
        if not jsons:
            continue
        with tempfile.TemporaryDirectory() as tmp:
            got = os.path.join(tmp, "manifest.txt")
            cmd = bspc + ["roundtrip", "--write-manifest", got]
            if has_manifest:
                cmd += ["--expect", MANIFEST]
            cmd += ["--"] + jsons
            say(f"{root}: bspc roundtrip over {len(jsons)} document(s)"
                + (f", against {MANIFEST}" if has_manifest else ""), flush=True)
            rc = subprocess.run(cmd, cwd=where).returncode
            ran += 1
            if rc != 0:
                errors.append(f"{root}: bspc roundtrip exit {rc}")
            if os.path.isfile(got):
                with open(got, encoding="utf-8", newline="") as fh:
                    for line in fh.read().splitlines():
                        lines.append(line[:HASHES] + root + "/" + line[HASHES:])
        if factory:
            say(f"{root}: bspc lint --factory over {len(jsons)} document(s)", flush=True)
            rc = subprocess.run(bspc + ["lint", "--factory", "--"] + jsons, cwd=where).returncode
            if rc != 0:
                errors.append(f"{root}: bspc lint --factory exit {rc} (a factory preset's lint error: "
                              f"L4, L7-L9 or an empty source set)")
    return lines, errors, ran


def run(bspc, out):
    lines, errors, ran = check([bspc])
    for e in errors:
        print(f"::error::{e}")
    lines.sort(key=lambda ln: ln[HASHES:])
    text = "".join(ln + "\n" for ln in lines)
    if out:
        with open(out, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(text)
    print(f"\n{len(lines)} document(s) in {ran} set(s); {len(errors)} failure(s)")
    if ran == 0 and not errors:
        print("::error::no document set found: run from the repository root")
        return 1
    return 1 if errors else 0


# self-test: a stand-in for bspc that writes an empty manifest and fails on "fail.json".
STUB_BSPC = """import sys
a = sys.argv[1:]
if a[0] == "lint":
    sys.exit(1 if "lintfail.json" in a else 0)
open(a[a.index("--write-manifest") + 1], "w").close()
sys.exit(1 if "fail.json" in a else 0)
"""
RULES = "*.json text eol=lf\nMANIFEST text eol=lf\n*.bsp binary\n"
F = "firmware/factory/"
GOOD = {".gitattributes": RULES, F + "a.json": "{}\n", F + "a.bsp": "BSP\0", F + MANIFEST: ""}
# (name, changes to GOOD (None deletes), a text the first error must contain, or None to pass)
SELF_TEST_CASES = [
    ("a well-formed set", {}, None),
    ("a well-formed set in a subdirectory", {F + "sub/b.json": "{}\n", F + "sub/b.bsp": "BSP\0"}, None),
    ("no MANIFEST", {F + MANIFEST: None}, "no MANIFEST"),
    ("no line-ending rule", {".gitattributes": "*.bsp binary\n"}, "text eol=lf"),
    ("MANIFEST without a line-ending rule", {".gitattributes": "*.json text eol=lf\n*.bsp binary\n"},
     "MANIFEST: .gitattributes does not give it 'text eol=lf'"),
    ("a document marked binary", {".gitattributes": "*.json binary\nMANIFEST text eol=lf\n*.bsp binary\n"},
     "text eol=lf"),
    ("a package not marked binary", {".gitattributes": "*.json text eol=lf\nMANIFEST text eol=lf\n"},
     "does not mark it binary"),
    ("a document without its package", {F + "b.json": "{}\n"}, "no committed package beside it"),
    ("a package without its document", {F + "b.bsp": "BSP\0"}, "a committed package without its document"),
    ("a MANIFEST without documents", {F + "a.json": None, F + "a.bsp": None}, "but no documents"),
    ("bspc fails", {F + "fail.json": "{}\n", F + "fail.bsp": "BSP\0"}, "bspc roundtrip exit 1"),
    ("a factory document fails the factory lint",
     {F + "lintfail.json": "{}\n", F + "lintfail.bsp": "BSP\0"}, "bspc lint --factory exit 1"),
]


def self_test():
    failures = total = 0
    # The repository's own .gitattributes: every set's documents, MANIFEST and packages.
    paths = []
    for root, packaged, _, _ in SETS:
        paths += [f"{root}/x.json", f"{root}/sub/x.json", f"{root}/{MANIFEST}"]
        paths += [f"{root}/x.bsp", f"{root}/sub/x.bsp"] if packaged else []
    problems = attribute_problems(".", paths)
    total += 1
    failures += bool(problems)
    print(f"{'FAIL' if problems else 'ok  '} the repository's .gitattributes cover every document set")
    for p in problems:
        print(f"       {p}")
    with tempfile.TemporaryDirectory() as scratch:
        stub = os.path.join(scratch, "bspc_stub.py")
        with open(stub, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(STUB_BSPC)
        for name, changes, want in SELF_TEST_CASES:
            repo = os.path.join(scratch, f"case{total}")
            files = dict(GOOD)
            files.update(changes)
            for path, text in files.items():
                if text is None:
                    continue
                full = os.path.join(repo, path)
                os.makedirs(os.path.dirname(full), exist_ok=True)
                with open(full, "w", encoding="utf-8", newline="\n") as fh:
                    fh.write(text)
            subprocess.run(["git", "init", "-q", repo], check=True)
            _, errors, _ = check([sys.executable, stub], top=repo, say=lambda *a, **k: None)
            ok = not errors if want is None else bool(errors) and want in errors[0]
            total += 1
            failures += not ok
            print(f"{'ok  ' if ok else 'FAIL'} {name}" + (f": {errors[0]}" if errors else ""))
            if not ok:
                for e in errors[1:]:
                    print(f"       {e}")
            shutil.rmtree(repo, ignore_errors=True)
    print(f"{total - failures}/{total} self-test cases gave the expected verdict")
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
    sub.add_parser("self-test", help="prove the checks can fail; the committed .gitattributes cover every set")
    args = ap.parse_args()
    if args.cmd == "run":
        return run(os.path.abspath(args.bspc), args.out)
    if args.cmd == "self-test":
        return self_test()
    return compare(args.directory, [x for x in args.legs.split(",") if x], args.summary)


if __name__ == "__main__":
    sys.exit(main())
