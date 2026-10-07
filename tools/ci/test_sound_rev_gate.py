#!/usr/bin/env python3
"""Self-test of sound_rev_gate.py on synthetic pull requests (determinism profile §5.12,
and the package rule of mode-compiler.md §8.3).

Builds a scratch repository holding a revision header, a golden file and a few other
files, commits a base and, per case, the pull request's commits and the merge commit CI
makes of the base and the pull request (the gate diffs the two ends and walks every commit
between them), and requires the gate's verdict for each. The single-commit cases change the
base's files in one commit; the package rule's cases run against a second base whose golden
file has a preset that plays a committed package and which has a corpus and a factory
manifest; the history cases build pull requests of several commits and merges, for the
per-commit sound-revision rule; the last ones give the gate a shallow clone and histories
with a commit or a file missing, which it must refuse. A case that passes when it should fail
means the gate lost a trigger. Exit 1 on any wrong verdict.

  test_sound_rev_gate.py
"""
import json
import os
import pathlib
import shutil
import stat
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


def bumped(rev, line=""):
    """A sound change with kSoundRevision raised to rev, golden.json not re-minted."""
    return {"dsp/src/PostChain.cpp": f"// the sound of revision {rev}{line}\n", HEADER: header(rev)}


def mint(rev, line=""):
    """golden.json minted at rev: the default preset renders differently at each revision."""
    return {GOLDEN: golden(rev, [("default", f"{rev}{line}".ljust(64, "0"), [f"{rev}0", f"{rev}1"]),
                                 BASE_PRESETS[1]])}


def sound_revision(rev, line=""):
    """A sound revision in one commit: the change, the bump and the mint."""
    return {**bumped(rev, line), **mint(rev, line)}


# Pull requests of several commits (the per-commit sound-revision rule):
# (name, the base it merges into, commits, 0 to pass or the failure it reports[, False when the
# gate reads the pull request's last commit itself, as when run by hand, instead of the merge
# commit CI makes of it and the base]). A commit is (name, parents, changes to its first
# parent's files); "fork" is the base files' commit, and every other parent is a commit named
# before it. "main..." commits stand for the base branch moving on.
SKIP = "skipped a number"
DOWN = "went down"
TWICE = "two different sounds cannot share a number"
TAKEN = "a number the base already has"
UNMINTED = "never minted its golden file"
GAP = "are introduced by no commit"
UNREAD = "fetch the full history"
BACK = {path: BASE[path] for path in ("dsp/src/PostChain.cpp", HEADER, GOLDEN)}
WAVE = [(f"r{r}", [f"r{r - 1}" if r > 2 else "fork"], sound_revision(r)) for r in range(2, 7)]
HISTORY_CASES = [
    ("two revisions, a commit each, each minted", "fork",
     [("r2", ["fork"], sound_revision(2)), ("r3", ["r2"], sound_revision(3))], 0),
    ("a bump, its mint in the next commit, then the next revision", "fork",
     [("b2", ["fork"], bumped(2)), ("m2", ["b2"], mint(2)), ("r3", ["m2"], sound_revision(3))], 0),
    ("a bump, its mint and a re-mint, then the next revision", "fork",
     [("b2", ["fork"], bumped(2)), ("m2", ["b2"], mint(2)), ("again", ["m2"], mint(2, "again")),
      ("r3", ["again"], sound_revision(3))], 0),
    ("five revisions, a commit each", "fork", WAVE, 0),
    ("the base merged in between the pull request's revisions", "main",
     [("main", ["fork"], {"docs/STATUS.md": "main moved\n"}), ("r2", ["fork"], sound_revision(2)),
      ("sync", ["r2", "main"], {"docs/STATUS.md": "main moved\n"}), ("r3", ["sync"], sound_revision(3))], 0),
    ("one commit of several skips a number", "fork",
     [("r2", ["fork"], sound_revision(2)), ("r4", ["r2"], sound_revision(4))], SKIP),
    ("a revision taken back by a later commit", "fork", [("r2", ["fork"], sound_revision(2)), ("back", ["r2"], BACK)],
     DOWN),
    ("one revision introduced on two merged lines", "fork",
     [("a2", ["fork"], sound_revision(2, "a")), ("b2", ["fork"], sound_revision(2, "b")), ("join", ["a2", "b2"], {})],
     TWICE),
    ("a merge that bumps past both of its lines", "fork",
     [("a2", ["fork"], sound_revision(2)), ("docs", ["fork"], {"docs/STATUS.md": "docs\n"}),
      ("join", ["a2", "docs"], {**sound_revision(3), "docs/STATUS.md": "docs\n"})], 0),
    ("a parallel line's revision the base already has, then a bump past both", "main2",
     [("main2", ["fork"], sound_revision(2, "main")), ("p2", ["fork"], sound_revision(2, "pr")),
      ("sync", ["p2", "main2"], sound_revision(3))], TAKEN),
    ("a revision never minted", "fork", [("b2", ["fork"], bumped(2)), ("r3", ["b2"], sound_revision(3))], UNMINTED),
    ("run by hand: a branch on a revision the base took back", "main1",
     [("main2", ["fork"], sound_revision(2)), ("main1", ["main2"], BACK),
      ("docs", ["main2"], {"docs/STATUS.md": "docs\n"})], GAP, False),
    ("run by hand: a branch behind the base's revision", "main2",
     [("main2", ["fork"], sound_revision(2)), ("docs", ["fork"], {"docs/STATUS.md": "docs\n"})], DOWN, False),
]
ENV = dict(os.environ, GIT_AUTHOR_NAME="sound-rev self-test", GIT_AUTHOR_EMAIL="self-test@example.invalid",
           GIT_COMMITTER_NAME="sound-rev self-test", GIT_COMMITTER_EMAIL="self-test@example.invalid")


def git(repo, *args):
    p = subprocess.run(["git", "-C", repo, *args], capture_output=True, text=True, env=ENV)
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


def commit(repo, files, parents, subject):
    """Commits exactly `files` on the given parents and returns the commit's id."""
    return git(repo, "commit-tree", tree(repo, files), "-m", subject, *[a for p in parents for a in ("-p", p)])


def changed(files, changes):
    """`files` with `changes` applied (None deletes)."""
    out = dict(files)
    for path, text in changes.items():
        if text is None:
            out.pop(path, None)
        else:
            out[path] = text
    return out


def verdict(name, want, cwd, base, head, labels="", body_file=None):
    """Runs the gate and prints whether it gave the verdict `want`; returns True when it did."""
    p = subprocess.run([sys.executable, GATE, "--base", base, "--head", head, "--labels", labels]
                       + (["--body-file", body_file] if body_file else []),
                       cwd=cwd, capture_output=True, text=True, encoding="utf-8",
                       env=dict(os.environ, PYTHONIOENCODING="utf-8"))
    fails = [ln[len("- **FAIL:** "):] for ln in p.stdout.splitlines() if ln.startswith("- **FAIL:** ")]
    if want == 0:
        ok = p.returncode == 0 and not fails
    else:  # a FAIL line, or an input the gate refuses to read (stderr)
        ok = p.returncode == 1 and (any(want in f for f in fails) or want in p.stderr)
    print(f"{'ok  ' if ok else 'FAIL'} {name}: exit {p.returncode}")
    for f in fails + p.stderr.strip().splitlines():
        print(f"       {f}")
    if not ok:
        print(p.stdout)
    return ok


def main():
    results = []
    with tempfile.TemporaryDirectory() as repo, tempfile.TemporaryDirectory() as scratch:
        git(repo, "init", "-q")
        body_file = os.path.join(scratch, "body.txt")
        # One commit on the base, merged into it as CI merges a pull request.
        for base_files, cases in ((BASE, CASES), (PACKAGE_BASE, PACKAGE_CASES)):
            base = commit(repo, base_files, [], "base")
            for case in cases:
                name, changes, labels, want = case[:4]
                with open(body_file, "w", encoding="utf-8", newline="\n") as fh:
                    fh.write(case[4] if len(case) > 4 else "")
                files = changed(base_files, changes)
                tip = commit(repo, files, [base], name)
                head = commit(repo, files, [base, tip], f"Merge {name}")
                results.append(verdict(name, want, repo, base, head, labels, body_file))
        # Pull requests of several commits.
        fork = commit(repo, BASE, [], "fork")
        for case in HISTORY_CASES:
            name, base, spec, want = case[:4]
            ids, files = {"fork": fork}, {"fork": BASE}
            for c, parents, changes in spec:
                files[c] = changed(files[parents[0]], changes)
                ids[c] = commit(repo, files[c], [ids[p] for p in parents], f"{name}: {c}")
            tip = spec[-1][0]
            head = ids[tip] if len(case) > 4 and not case[4] else commit(
                repo, files[tip], [ids[base], ids[tip]], f"Merge {name}")
            results.append(verdict(name, want, repo, ids[base], head))
        # Fail closed. The checkout the gate had before it walked commits (fetch-depth: 2) reads the
        # merge commit and its parents but none of the pull request's earlier commits.
        r2 = commit(repo, changed(BASE, sound_revision(2)), [fork], "unread: r2")
        r3 = commit(repo, changed(BASE, sound_revision(3)), [r2], "unread: r3")
        head = commit(repo, changed(BASE, sound_revision(3)), [fork, r3], "Merge unread")
        git(repo, "update-ref", "refs/heads/unread", head)
        shallow = os.path.join(scratch, "shallow")
        git(scratch, "clone", "-q", "--depth", "2", "--branch", "unread", pathlib.Path(repo).as_uri(), shallow)
        results.append(verdict("a shallow clone (fetch-depth: 2)", UNREAD, shallow, "HEAD^1", "HEAD"))
        results.append(verdict("the same history, whole", 0, repo, fork, head))
        # A commit missing, and a file of a pull request's own (a header no other case has) missing.
        u2 = commit(repo, changed(BASE, {**sound_revision(2), HEADER: header(2) + "// unread\n"}), [fork], "unread: u2")
        u3 = commit(repo, changed(BASE, sound_revision(3)), [u2], "unread: u3")
        u_head = commit(repo, changed(BASE, sound_revision(3)), [fork, u3], "Merge unread u3")
        for name, obj, case_head in (("a commit", r2, head),
                                     ("a file", git(repo, "rev-parse", f"{u2}:{HEADER}"), u_head)):
            loose = os.path.join(repo, ".git", "objects", obj[:2], obj[2:])
            os.chmod(loose, stat.S_IWRITE)  # git writes objects read-only
            os.remove(loose)
            results.append(verdict(f"a history with {name} missing", UNREAD, repo, fork, case_head))
    print(f"{sum(results)}/{len(results)} cases gave the expected verdict")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
