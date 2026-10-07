#!/usr/bin/env python3
"""Sound-revision gate for a pull request (determinism profile §5.12, companion §3.4).

Compares two commits, the pull request's base and its head (in CI, the merge commit's
first parent and the merge commit), and fails (exit 1) when:

  * hard trigger: a golden hash changed (dsp/tests/golden/golden.json: a preset's hash or
    per-second hashes differ, or a preset was dropped) and kSoundRevision was not bumped.
    No label overrides it. A package preset's changed hash counts only when its package's
    hashes are unchanged (the package rule, below) or the diff also touches a sound-relevant
    path (the path trigger's) without a bump: then the gate cannot tell whether the engine
    or the package moved the render, so the render is the engine's, whatever the labels;
  * path trigger: the diff touches dsp/src/, dsp/include/, the CMake files that build the
    engine (dsp/CMakeLists.txt, the root CMakeLists.txt, cmake/BrainscapeFpProfile.cmake),
    cmake/fp-forbidden-flags.txt or the arm toolchain file, and kSoundRevision was not
    bumped, unless the pull request carries the "sound-neutral" label (refactors, comments,
    tests; restricting who may apply it is the repository's job, not this script's);
  * the package rule (mode-compiler.md §8.3): a committed package's soundHash or
    controlHash changed (a golden preset's, or a corpus or factory package's in
    dsp/tests/golden/presets/MANIFEST or firmware/factory/MANIFEST, which bspc_roundtrip.py
    requires and checks against the committed packages), or a package was dropped, and the
    pull request does not carry the "package-change" label with its cause named on a
    "Package-change: <cause>" line of its description. A compiler or document change alters
    what a document means, not what the engine plays for a package, so it needs visibility,
    not a bump; a bump does not waive it, and "sound-neutral" does not either;
  * kSoundRevision went down or skipped a number: a bump is exactly one;
  * the head's golden file is keyed to another revision than the head's kSoundRevision:
    a bump regenerates it with the harness's --mode mint.

New presets, packages, counters and corpus versions change no committed hash, so they pass,
and so does a re-stamped package (sound_rev and package hash only).

  sound_rev_gate.py --base REV [--head REV] [--labels JSON_OR_COMMA_LIST]
                    [--body-file FILE] [--summary FILE]
"""
import argparse
import json
import re
import subprocess
import sys

REVISION_HEADER = "dsp/include/brainscape/SoundRevision.h"
GOLDEN = "dsp/tests/golden/golden.json"
TRIGGER_DIRS = ("dsp/src/", "dsp/include/")
TRIGGER_FILES = ("CMakeLists.txt", "dsp/CMakeLists.txt", "cmake/BrainscapeFpProfile.cmake",
                 "cmake/fp-forbidden-flags.txt", "tools/cmake/arm-none-eabi-toolchain.cmake")
NEUTRAL_LABEL = "sound-neutral"
REVISION = re.compile(r"\bkSoundRevision\s*=\s*(\d+)\s*;")
# The package rule (mode-compiler.md §8.3). bspc's manifest format: package hash, sound_hash,
# control_hash and path per line (`bspc roundtrip --write-manifest`). bspc_roundtrip.py
# requires each set's MANIFEST and that it matches the set's committed packages, so a package
# cannot change without its line here changing; golden.json's soundHash and controlHash, which
# the harness's mint writes, tie a golden preset's render to its package.
PACKAGE_LABEL = "package-change"
PACKAGE_MANIFESTS = ("dsp/tests/golden/presets/MANIFEST", "firmware/factory/MANIFEST")
PACKAGE_CAUSE = re.compile(r"^[ \t]*(?:[-*][ \t]+)?package-change:[ \t]*(\S.*?)[ \t]*$",
                           re.IGNORECASE | re.MULTILINE)
MANIFEST_LINE = re.compile(r"^([0-9a-f]{64}) ([0-9a-f]{64}) ([0-9a-f]{64}) (\S.*)$")


def git(*args, check=True):
    p = subprocess.run(["git", *args], capture_output=True, text=True, encoding="utf-8")
    if check and p.returncode != 0:
        sys.exit(f"sound_rev_gate: git {' '.join(args)} failed: {p.stderr.strip()}")
    return p


def show(rev, path):
    """The file at a revision, or None when it does not exist there."""
    p = git("show", f"{rev}:{path}", check=False)
    return p.stdout if p.returncode == 0 else None


def revision(rev):
    text = show(rev, REVISION_HEADER)
    if text is None:
        return None
    m = REVISION.search(text)
    if m is None:
        sys.exit(f"sound_rev_gate: no kSoundRevision in {REVISION_HEADER} at {rev}")
    return int(m.group(1))


def golden(rev):
    text = show(rev, GOLDEN)
    if text is None:
        return None
    try:
        return json.loads(text)
    except ValueError as err:
        sys.exit(f"sound_rev_gate: {GOLDEN} at {rev} is not JSON: {err}")


def preset_hashes(g):
    out = {}
    for v in g.get("vectors", []):
        for p in v.get("presets", []):
            out[f'{v["name"]}/{p["name"]}'] = (p.get("hash"), p.get("secondHashes"))
    return out


def golden_packages(g):
    """{'vector/preset': (soundHash, controlHash)} for the presets that play a committed package."""
    out = {}
    for v in (g or {}).get("vectors", []):
        for p in v.get("presets", []):
            if "soundHash" in p or "controlHash" in p:
                out[f'{v["name"]}/{p["name"]}'] = (p.get("soundHash"), p.get("controlHash"))
    return out


def manifest(rev, path):
    """{path: (sound_hash, control_hash)} from a bspc manifest, or None when it does not exist."""
    text = show(rev, path)
    if text is None:
        return None
    out = {}
    for n, line in enumerate(text.splitlines(), 1):
        m = MANIFEST_LINE.match(line)
        if m is None:
            sys.exit(f"sound_rev_gate: {path}:{n} at {rev} is not a manifest line "
                     f"(package hash, sound_hash, control_hash, path)")
        out[m.group(4)] = (m.group(2), m.group(3))
    return out


def package_changes(base_golden, head_golden, base_manifests, head_manifests):
    """Committed packages whose soundHash or controlHash differs, or that are gone, in the head.

    Returns (changes, golden presets whose package changed). A package the base does not
    have (a new preset, a preset that gains a package, a new factory package) is no change.
    """
    changes, changed = [], set()
    old, new = golden_packages(base_golden), golden_packages(head_golden)
    head_presets = preset_hashes(head_golden or {})
    for name, (s, c) in sorted(old.items()):
        if name not in head_presets:
            continue  # a dropped preset is the hard trigger's
        if name not in new:
            changes.append(f"{GOLDEN} {name}: no longer plays a package")
        elif new[name] != (s, c):
            changes.append(f"{GOLDEN} {name}: " + hash_moves(("soundHash", "controlHash"), (s, c), new[name]))
        else:
            continue
        changed.add(name)
    for path in PACKAGE_MANIFESTS:
        before, after = base_manifests.get(path) or {}, head_manifests.get(path) or {}
        for doc, hashes in sorted(before.items()):
            if doc not in after:
                changes.append(f"{path} {doc}: dropped")
            elif after[doc] != hashes:
                changes.append(f"{path} {doc}: " + hash_moves(("sound_hash", "control_hash"), hashes, after[doc]))
    return changes, changed


def hash_moves(names, old, new):
    return ", ".join(f"{k} {str(a)[:16]} -> {str(b)[:16]}" for k, a, b in zip(names, old, new) if a != b)


def golden_changes(base, head, packaged=frozenset()):
    """Presets of the base golden file whose hashes differ or are gone in the head's.

    Returns (engine changes, changes attributed to the preset's package): a changed render of
    a preset whose package changed is the package's (the package rule), not the engine's.
    """
    if base is None:
        return [], []
    old, new = preset_hashes(base), preset_hashes(head or {})
    changes, attributed = [], []
    for name, (h, secs) in sorted(old.items()):
        if name not in new:
            change = f"{name}: dropped"
        elif new[name][0] != h:
            change = f"{name}: hash {str(h)[:16]} -> {str(new[name][0])[:16]}"
        elif new[name][1] != secs:
            change = f"{name}: per-second hashes differ"
        else:
            continue
        (attributed if name in packaged and name in new else changes).append(change)
    return changes, attributed


def read_body(path):
    if not path:
        return ""
    try:
        with open(path, encoding="utf-8") as fh:
            return fh.read()
    except OSError as err:
        sys.exit(f"sound_rev_gate: cannot read the pull request's description {path}: {err}")


def parse_labels(text):
    text = (text or "").strip()
    if not text:
        return set()
    if text.startswith("["):
        return {str(x) for x in json.loads(text)}
    return {x.strip() for x in text.split(",") if x.strip()}


def triggers(paths):
    return [p for p in paths if p.startswith(TRIGGER_DIRS) or p in TRIGGER_FILES]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--base", required=True, help="the commit the pull request merges into")
    ap.add_argument("--head", default="HEAD", help="the pull request, merged (default HEAD)")
    ap.add_argument("--labels", default="", help="the pull request's labels: a JSON list or a comma list")
    ap.add_argument("--body-file", help="the pull request's description, for the package rule's cause")
    ap.add_argument("--summary", help="also append the Markdown to this file ($GITHUB_STEP_SUMMARY)")
    args = ap.parse_args()

    paths = [p for p in git("diff", "--name-only", "--no-renames", args.base, args.head).stdout.splitlines() if p]
    touched = triggers(paths)
    labels = parse_labels(args.labels)
    causes = PACKAGE_CAUSE.findall(read_body(args.body_file))
    base_rev, head_rev = revision(args.base), revision(args.head)
    if head_rev is None:
        sys.exit(f"sound_rev_gate: {REVISION_HEADER} is missing at {args.head}")
    base_rev = 0 if base_rev is None else base_rev
    bumped = head_rev > base_rev
    base_golden, head_golden = golden(args.base), golden(args.head)
    packages, packaged = package_changes(
        base_golden, head_golden, {p: manifest(args.base, p) for p in PACKAGE_MANIFESTS},
        {p: manifest(args.head, p) for p in PACKAGE_MANIFESTS})
    # A changed render is its package's only when the engine did not change unbumped beside it:
    # with both changed, nothing tells which one moved it, so it is the hard trigger's.
    engine_unbumped = bool(touched) and not bumped
    changed, attributed = golden_changes(base_golden, head_golden, frozenset() if engine_unbumped else packaged)
    withheld = golden_changes(base_golden, head_golden, packaged)[1] if engine_unbumped else []

    failures, notes = [], []
    if head_rev < base_rev:
        failures.append(f"kSoundRevision went down, {base_rev} -> {head_rev}.")
    elif head_rev > base_rev + 1:
        failures.append(f"kSoundRevision skipped a number, {base_rev} -> {head_rev}: a bump is exactly one.")
    if changed and not bumped:
        failures.append(f"{len(changed)} golden hash(es) changed without a kSoundRevision bump (hard "
                        f"trigger, profile §5.12; no label overrides it)."
                        + (f" Sound-relevant paths changed too, so {len(withheld)} package preset render(s) "
                           f"whose package also changed count as the engine's: bump, or land the engine "
                           f"change and the package change in separate pull requests." if withheld else ""))
    if touched and not bumped:
        if NEUTRAL_LABEL in labels:
            notes.append(f'Sound-relevant paths changed without a bump; the pull request carries the '
                         f'"{NEUTRAL_LABEL}" label, which a code owner must justify in review.')
        else:
            failures.append(f"{len(touched)} sound-relevant path(s) changed without a kSoundRevision bump: "
                            f'bump it, or label the pull request "{NEUTRAL_LABEL}" if it cannot change '
                            f"output (path trigger, profile §5.12).")
    if head_golden is not None and head_golden.get("soundRevision") != head_rev:
        failures.append(f'{GOLDEN} is for sound revision {head_golden.get("soundRevision")} but kSoundRevision '
                        f"is {head_rev}: regenerate it with brainscape_golden --mode mint.")
    if bumped and head_golden is None and head_rev > 0:
        failures.append(f"kSoundRevision is {head_rev} but there is no {GOLDEN}: mint it.")
    if packages:
        if PACKAGE_LABEL not in labels:
            failures.append(f"{len(packages)} committed package hash(es) changed: label the pull request "
                            f'"{PACKAGE_LABEL}" and name the cause on a "Package-change: <cause>" line of its '
                            f"description (the package rule, mode-compiler.md §8.3; a bump does not waive it).")
        elif not causes:
            failures.append(f'{len(packages)} committed package hash(es) changed and the "{PACKAGE_LABEL}" label '
                            f'is on, but the description names no cause: add a "Package-change: <cause>" line.')
        else:
            notes.append(f'Committed packages changed; the "{PACKAGE_LABEL}" label is on, which a code owner '
                         f"approves in review. Cause: " + "; ".join(causes))

    lines = ["# Sound-revision gate", "",
             f"kSoundRevision {base_rev} -> {head_rev} ({'bumped' if bumped else 'not bumped'}); "
             f"labels: {', '.join(sorted(labels)) or 'none'}.", ""]
    lines.append(f"Sound-relevant paths changed: {len(touched)}" + (":" if touched else "."))
    lines += [f"- `{p}`" for p in touched]
    lines += ["", f"Golden hashes changed: {len(changed)}" + (":" if changed else ".")]
    lines += [f"- {c}" for c in changed]
    lines += ["", f"Committed package hashes changed: {len(packages)}" + (":" if packages else ".")]
    lines += [f"- {c}" for c in packages]
    if attributed:
        lines += ["", f"Golden hashes changed with their preset's package (the package rule): {len(attributed)}:"]
        lines += [f"- {c}" for c in attributed]
    lines.append("")
    lines += [f"- {n}" for n in notes]
    lines += [f"- **FAIL:** {f}" for f in failures]
    lines.append("" if failures else "PASS")
    text = "\n".join(lines) + "\n"
    print(text)
    if args.summary:
        with open(args.summary, "a", encoding="utf-8") as fh:
            fh.write(text)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
