#!/usr/bin/env python3
"""Self-test of sound_rev_gate.py on synthetic pull requests (determinism profile §5.12,
and the package rule of mode-compiler.md §8.3).

Builds a scratch repository holding a revision header, a golden file and a few other
files, writes a base tree and one head tree per case (trees, not commits: the gate only
diffs and reads them), and requires the gate's verdict for each. The package rule's cases
run against a second base whose golden file has a preset that plays a committed package
and which has a corpus and a factory manifest. A case that passes when it should fail means
the gate lost a trigger. Exit 1 on any wrong verdict.

  test_sound_rev_gate.py
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile

GATE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "sound_rev_gate.py")
HEADER = "dsp/include/brainscape/SoundRevision.h"
GOLDEN = "dsp/tests/golden/golden.json"


def header(rev):
    return f"#pragma once\nnamespace brainscape {{\ninline constexpr uint32_t kSoundRevision = {rev};\n}}\n"


def golden(rev, presets):
    """Presets are (name, hash, secondHashes) or, for one that plays a committed package,
    (name, hash, secondHashes, soundHash, controlHash)."""
    def entry(p):
        e = {"name": p[0], "hash": p[1], "secondHashes": p[2]}
        if len(p) > 3:
            e.update(soundHash=p[3], controlHash=p[4])
        return e
    return json.dumps({"format": "brainscape-golden/1", "soundRevision": rev, "vectors": [
        {"name": "plucks_12s", "presets": [entry(p) for p in presets]}]}, indent=2) + "\n"


def manifest(docs):
    """A bspc manifest: (path, package hash, sound_hash, control_hash) per document, by path."""
    return "".join(f"{p * 32} {s * 32} {c * 32} {d}\n" for d, p, s, c in sorted(docs))


BASE_PRESETS = [("default", "aa" * 32, ["a0", "a1"]), ("clean_delay", "bb" * 32, ["b0", "b1"])]
BASE = {
    HEADER: header(1),
    GOLDEN: golden(1, BASE_PRESETS),
    "dsp/src/PostChain.cpp": "// post chain\n",
    "dsp/tests/test_engine.cpp": "// tests\n",
    "dsp/tests/CMakeLists.txt": "# tests\n",
    "dsp/CMakeLists.txt": "# engine\n",
    "CMakeLists.txt": "# superbuild\n",
    "plugin/CMakeLists.txt": "# plugin\n",
    "cmake/BrainscapeFpProfile.cmake": "# profile\n",
    "cmake/fp-forbidden-flags.txt": "-ffast-math\n",
    "tools/cmake/arm-none-eabi-toolchain.cmake": "# arm\n",
    "firmware/main.cpp": "// firmware\n",
    "docs/STATUS.md": "status\n",
}
SOUND = "// a coefficient changed\n"
REGOLDEN2 = golden(2, [("default", "cc" * 32, ["c0", "c1"]), BASE_PRESETS[1]])

# Each failing case names the failure it must report, so it cannot pass on another one.
HARD = "without a kSoundRevision bump (hard trigger"
PATH = "path(s) changed without a kSoundRevision bump"
STALE = "regenerate it with"
G_HASH = golden(1, [("default", "cc" * 32, ["a0", "a1"]), BASE_PRESETS[1]])
G_BOTH = golden(1, [("default", "cc" * 32, ["c0", "c1"]), BASE_PRESETS[1]])
G_SECOND = golden(1, [("default", "aa" * 32, ["a0", "zz"]), BASE_PRESETS[1]])
G_ADDED = golden(1, BASE_PRESETS + [("new", "dd" * 32, ["d0"])])
ARM = "tools/cmake/arm-none-eabi-toolchain.cmake"

# (name, changes to the base files (None deletes), labels, 0 to pass or the failure it reports)
CASES = [
    ("docs only", {"docs/STATUS.md": "new status\n"}, "", 0),
    ("firmware only", {"firmware/main.cpp": "// more firmware\n"}, "", 0),
    ("dsp/src without a bump", {"dsp/src/PostChain.cpp": SOUND}, "", PATH),
    ("dsp/src without a bump, other labels", {"dsp/src/PostChain.cpp": SOUND}, '["docs","engine"]', PATH),
    ("dsp/src, sound-neutral label (JSON list)", {"dsp/src/PostChain.cpp": SOUND}, '["sound-neutral"]', 0),
    ("dsp/src, sound-neutral label (comma list)", {"dsp/src/PostChain.cpp": SOUND}, "docs,sound-neutral", 0),
    ("new dsp/include file without a bump", {"dsp/include/brainscape/New.h": "// new\n"}, "", PATH),
    ("profile CMake without a bump", {"cmake/BrainscapeFpProfile.cmake": "# changed\n"}, "", PATH),
    ("forbidden-flag list without a bump", {"cmake/fp-forbidden-flags.txt": "-Ofast\n"}, "", PATH),
    ("arm toolchain file without a bump", {ARM: "# -O2\n"}, "", PATH),
    ("engine CMake file without a bump", {"dsp/CMakeLists.txt": "# a definition\n"}, "", PATH),
    ("root CMake file without a bump", {"CMakeLists.txt": "# a definition\n"}, "", PATH),
    ("engine CMake file, sound-neutral", {"dsp/CMakeLists.txt": "# a definition\n"}, "sound-neutral", 0),
    ("test and plugin CMake files only",
     {"dsp/tests/CMakeLists.txt": "# a test\n", "plugin/CMakeLists.txt": "# a format\n"}, "", 0),
    ("arm toolchain file, sound-neutral", {ARM: "# -O2\n"}, "sound-neutral", 0),
    ("dsp/src with a bump and a regenerated golden file",
     {"dsp/src/PostChain.cpp": SOUND, HEADER: header(2), GOLDEN: REGOLDEN2}, "", 0),
    ("bump without regenerating the golden file", {"dsp/src/PostChain.cpp": SOUND, HEADER: header(2)}, "",
     STALE),
    ("bump that deletes the golden file", {HEADER: header(2), GOLDEN: None}, "", "there is no"),
    ("golden hash change without a bump", {GOLDEN: G_HASH}, "", HARD),
    ("golden hash change without a bump, sound-neutral", {GOLDEN: G_BOTH}, "sound-neutral", HARD),
    ("golden per-second hash change without a bump", {GOLDEN: G_SECOND}, "", HARD),
    ("golden preset dropped without a bump", {GOLDEN: golden(1, BASE_PRESETS[:1])}, "", HARD),
    ("golden preset added without a bump", {GOLDEN: G_ADDED, "dsp/tests/test_engine.cpp": "// more\n"}, "", 0),
    ("revision skips a number", {HEADER: header(3), GOLDEN: golden(3, BASE_PRESETS)}, "", "skipped a number"),
    ("revision goes down", {HEADER: header(0), GOLDEN: golden(0, BASE_PRESETS)}, "", "went down"),
    ("golden keyed to another revision", {GOLDEN: golden(2, BASE_PRESETS)}, "", STALE),
]

# The package rule (mode-compiler.md §8.3). "engram" plays a committed package, which the
# corpus manifest lists; the factory manifest lists two packages. A case may give the pull
# request's description as a fifth item.
FACTORY = "firmware/factory/MANIFEST"
CORPUS = "dsp/tests/golden/presets/MANIFEST"
PKG = ("engram", "ee" * 32, ["e0", "e1"], "51" * 32, "c1" * 32)
FACTORY_DOCS = [("engram.json", "a1", "51", "c1"), ("haze.json", "a2", "52", "c2")]
CORPUS_DOCS = [("engram.json", "a1", "51", "c1"), ("sweep.json", "a5", "55", "c5")]
PACKAGE_BASE = dict(BASE, **{
    GOLDEN: golden(1, BASE_PRESETS + [PKG]),
    FACTORY: manifest(FACTORY_DOCS),
    CORPUS: manifest(CORPUS_DOCS),
    "compiler/src/Compile.cpp": "// compile\n",
    "dsp/tests/golden/presets/engram.json": "{}\n",
})
PKG_LABEL = "package-change"
CAUSE = "Moves the macro table.\n\nPackage-change: the compiler writes MACR targets in authored order\n"
NEEDS_LABEL = 'label the pull request "package-change"'
NEEDS_CAUSE = "the description names no cause"


def pkg_golden(rev=1, render="ee", sound="51", control="c1", others=BASE_PRESETS, package=True):
    """The package base's golden file with engram's render and package hashes replaced."""
    p = ("engram", render * 32, ["e0", "e1"] if render == "ee" else [render[0] + "0", render[0] + "1"])
    return golden(rev, others + [p + ((sound * 32, control * 32) if package else ())])


def factory(*changes, drop=None, base=FACTORY_DOCS):
    """The factory manifest with (path, package, sound, control) entries replaced or added."""
    docs = {d[0]: d for d in base}
    docs.update({d[0]: d for d in changes})
    docs.pop(drop, None)
    return manifest(docs.values())


def corpus(*changes, drop=None):
    """The corpus manifest with (path, package, sound, control) entries replaced or added."""
    return factory(*changes, drop=drop, base=CORPUS_DOCS)


PACKAGE_CASES = [
    ("compiler change, every package unchanged", {"compiler/src/Compile.cpp": "// faster\n"}, "", 0),
    ("package preset re-rendered, its package unchanged", {GOLDEN: pkg_golden(render="ff")}, "", HARD),
    ("package preset re-rendered, its package unchanged, package-change label",
     {GOLDEN: pkg_golden(render="ff")}, PKG_LABEL, HARD, CAUSE),
    ("package preset's sound hash and render changed, label and cause",
     {GOLDEN: pkg_golden(render="ff", sound="5f"), "compiler/src/Compile.cpp": "// new order\n"},
     PKG_LABEL, 0, CAUSE),
    ("package preset's sound hash and render changed, no label",
     {GOLDEN: pkg_golden(render="ff", sound="5f")}, "", NEEDS_LABEL, CAUSE),
    ("package preset's sound hash and render changed, label without a cause",
     {GOLDEN: pkg_golden(render="ff", sound="5f")}, PKG_LABEL, NEEDS_CAUSE, "Moves the macro table.\n"),
    ("package preset's sound hash changed, an empty cause line",
     {GOLDEN: pkg_golden(sound="5f")}, PKG_LABEL, NEEDS_CAUSE, "Package-change:   \n"),
    ("package preset's sound hash changed, sound-neutral instead of package-change",
     {GOLDEN: pkg_golden(sound="5f")}, "sound-neutral", NEEDS_LABEL, CAUSE),
    ("package preset's control hash changed alone, label and a bulleted cause",
     {GOLDEN: pkg_golden(control="cf")}, '["docs","package-change"]', 0, "- package-change: CTRL padding\n"),
    ("package preset's control hash changed alone, no label", {GOLDEN: pkg_golden(control="cf")}, "", NEEDS_LABEL),
    ("package change does not excuse another preset's render",
     {GOLDEN: pkg_golden(render="ff", sound="5f", others=[("default", "cc" * 32, ["c0", "c1"]), BASE_PRESETS[1]])},
     PKG_LABEL, HARD, CAUSE),
    ("package preset back to a parameter list, render changed, label and cause",
     {GOLDEN: pkg_golden(render="ff", package=False)}, PKG_LABEL, 0, CAUSE),
    ("package preset back to a parameter list, no label", {GOLDEN: pkg_golden(package=False)}, "", NEEDS_LABEL),
    ("package preset dropped without a bump", {GOLDEN: golden(1, BASE_PRESETS)}, PKG_LABEL, HARD, CAUSE),
    ("a preset gains a package", {GOLDEN: golden(1, [BASE_PRESETS[0] + ("5a" * 32, "ca" * 32), BASE_PRESETS[1], PKG])},
     "", 0),
    ("a new package preset", {GOLDEN: golden(1, BASE_PRESETS + [PKG, ("haze", "dd" * 32, ["d0"], "52" * 32,
                                                                       "c2" * 32)])}, "", 0),
    ("bump and re-mint with a package change, no label",
     {"dsp/src/PostChain.cpp": SOUND, HEADER: header(2), GOLDEN: pkg_golden(rev=2, render="ff", sound="5f")}, "",
     NEEDS_LABEL),
    ("bump and re-mint with a package change, label and cause",
     {"dsp/src/PostChain.cpp": SOUND, HEADER: header(2), GOLDEN: pkg_golden(rev=2, render="ff", sound="5f")},
     PKG_LABEL, 0, CAUSE),
    ("bump and re-mint, packages unchanged", {HEADER: header(2), GOLDEN: pkg_golden(rev=2, render="ff")}, "", 0),
    ("factory package's sound_hash changed, no label", {FACTORY: factory(("haze.json", "a3", "5f", "c2"))}, "",
     NEEDS_LABEL),
    ("factory package's control_hash changed, label and cause", {FACTORY: factory(("haze.json", "a3", "52", "cf"))},
     PKG_LABEL, 0, CAUSE),
    ("factory package re-stamped (package hash only)", {FACTORY: factory(("haze.json", "a3", "52", "c2"))}, "", 0),
    ("factory package added", {FACTORY: factory(("warp.json", "a4", "54", "c4"))}, "", 0),
    ("factory package dropped", {FACTORY: factory(drop="haze.json")}, "", NEEDS_LABEL),
    ("factory manifest deleted", {FACTORY: None}, "", NEEDS_LABEL),
    ("factory manifest malformed", {FACTORY: "not a manifest line\n"}, PKG_LABEL, "is not a manifest line", CAUSE),
    # A corpus package changed and golden.json not re-minted (a CTRL-only change, or a STAT
    # change the render does not hear): the corpus manifest still shows it.
    ("corpus package's sound_hash changed, golden.json not re-minted, no label",
     {CORPUS: corpus(("engram.json", "a3", "5f", "c1"))}, "", NEEDS_LABEL),
    ("corpus package's control_hash changed, golden.json not re-minted, label and cause",
     {CORPUS: corpus(("engram.json", "a3", "51", "cf"))}, PKG_LABEL, 0, CAUSE),
    ("corpus package re-stamped (package hash only)", {CORPUS: corpus(("sweep.json", "a6", "55", "c5"))}, "", 0),
    ("corpus package added", {CORPUS: corpus(("warp.json", "a4", "54", "c4"))}, "", 0),
    ("corpus package dropped", {CORPUS: corpus(drop="sweep.json")}, "", NEEDS_LABEL),
    # An unbumped engine change beside a package change: the render is the engine's, whatever
    # the labels (the hard trigger), since nothing tells which of the two moved it.
    ("sound-neutral engine change beside a CTRL-only package change, render changed",
     {"dsp/src/PostChain.cpp": SOUND, GOLDEN: pkg_golden(render="ff", control="cf")},
     '["sound-neutral","package-change"]', HARD, CAUSE),
    ("sound-neutral engine change beside a STAT package change, render changed",
     {"dsp/src/PostChain.cpp": SOUND, GOLDEN: pkg_golden(render="ff", sound="5f")},
     '["sound-neutral","package-change"]', HARD, CAUSE),
    ("sound-neutral engine CMake change beside corpus and factory package changes, render changed",
     {"dsp/CMakeLists.txt": "# a definition\n", GOLDEN: pkg_golden(render="ff", sound="5f"),
      CORPUS: corpus(("engram.json", "a3", "5f", "c1")), FACTORY: factory(("haze.json", "a3", "5f", "c2"))},
     '["sound-neutral","package-change"]', HARD, CAUSE),
    ("sound-neutral engine change beside a CTRL-only package change, render unchanged",
     {"dsp/src/PostChain.cpp": SOUND, GOLDEN: pkg_golden(control="cf")},
     '["sound-neutral","package-change"]', 0, CAUSE),
]


def git(repo, *args):
    p = subprocess.run(["git", "-C", repo, *args], capture_output=True, text=True)
    if p.returncode != 0:
        sys.exit(f"test_sound_rev_gate: git {' '.join(args)} failed: {p.stderr.strip()}")
    return p.stdout.strip()


def tree(repo, files):
    """Writes exactly `files` into the work tree and returns the tree object id."""
    for name in os.listdir(repo):
        path = os.path.join(repo, name)
        if name == ".git":
            continue
        if os.path.isdir(path):
            shutil.rmtree(path)
        else:
            os.remove(path)
    for path, text in files.items():
        full = os.path.join(repo, path)
        os.makedirs(os.path.dirname(full), exist_ok=True)
        with open(full, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(text)
    git(repo, "add", "-A")
    return git(repo, "write-tree")


def main():
    failures = total = 0
    with tempfile.TemporaryDirectory() as repo, tempfile.TemporaryDirectory() as scratch:
        git(repo, "init", "-q")
        body_file = os.path.join(scratch, "body.txt")
        for base_files, cases in ((BASE, CASES), (PACKAGE_BASE, PACKAGE_CASES)):
            base = tree(repo, base_files)
            for case in cases:
                name, changes, labels, want = case[:4]
                with open(body_file, "w", encoding="utf-8", newline="\n") as fh:
                    fh.write(case[4] if len(case) > 4 else "")
                files = dict(base_files)
                for path, text in changes.items():
                    if text is None:
                        files.pop(path, None)
                    else:
                        files[path] = text
                head = tree(repo, files)
                p = subprocess.run([sys.executable, GATE, "--base", base, "--head", head, "--labels", labels,
                                    "--body-file", body_file],
                                   cwd=repo, capture_output=True, text=True, encoding="utf-8",
                                   env=dict(os.environ, PYTHONIOENCODING="utf-8"))
                fails = [ln[len("- **FAIL:** "):] for ln in p.stdout.splitlines() if ln.startswith("- **FAIL:** ")]
                if want == 0:
                    ok = p.returncode == 0 and not fails
                else:  # a FAIL line, or an input the gate refuses to read (stderr)
                    ok = p.returncode == 1 and (any(want in f for f in fails) or want in p.stderr)
                failures += not ok
                total += 1
                print(f"{'ok  ' if ok else 'FAIL'} {name}: exit {p.returncode}")
                for f in fails:
                    print(f"       {f}")
                if not ok:
                    print(p.stdout + p.stderr)
    print(f"{total - failures}/{total} cases gave the expected verdict")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
