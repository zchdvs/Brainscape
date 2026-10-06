#!/usr/bin/env python3
"""Self-test of sound_rev_gate.py on synthetic pull requests (determinism profile §5.12).

Builds a scratch repository holding a revision header, a golden file and a few other
files, writes a base tree and one head tree per case (trees, not commits: the gate only
diffs and reads them), and requires the gate's verdict for each. A case that passes when
it should fail means the gate lost a trigger. Exit 1 on any wrong verdict.

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
    return json.dumps({"format": "brainscape-golden/1", "soundRevision": rev, "vectors": [
        {"name": "plucks_12s", "presets": [{"name": n, "hash": h, "secondHashes": s} for n, h, s in presets]}]},
        indent=2) + "\n"


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
    failures = 0
    with tempfile.TemporaryDirectory() as repo:
        git(repo, "init", "-q")
        base = tree(repo, BASE)
        for name, changes, labels, want in CASES:
            files = dict(BASE)
            for path, text in changes.items():
                if text is None:
                    files.pop(path, None)
                else:
                    files[path] = text
            head = tree(repo, files)
            p = subprocess.run([sys.executable, GATE, "--base", base, "--head", head, "--labels", labels],
                               cwd=repo, capture_output=True, text=True, encoding="utf-8",
                               env=dict(os.environ, PYTHONIOENCODING="utf-8"))
            fails = [ln[len("- **FAIL:** "):] for ln in p.stdout.splitlines() if ln.startswith("- **FAIL:** ")]
            if want == 0:
                ok = p.returncode == 0 and not fails
            else:
                ok = p.returncode == 1 and any(want in f for f in fails)
            failures += not ok
            print(f"{'ok  ' if ok else 'FAIL'} {name}: exit {p.returncode}")
            for f in fails:
                print(f"       {f}")
            if not ok:
                print(p.stdout + p.stderr)
    print(f"{len(CASES) - failures}/{len(CASES)} cases gave the expected verdict")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
