#!/usr/bin/env python3
"""Sound-revision gate for a pull request (determinism profile §5.12, companion §3.4).

Compares two commits, the pull request's base and its head (in CI, the merge commit's
first parent and the merge commit), and fails (exit 1) when:

  * hard trigger: a golden hash changed (dsp/tests/golden/golden.json: a preset's hash or
    per-second hashes differ, or a preset was dropped) and kSoundRevision was not bumped.
    No label overrides it;
  * path trigger: the diff touches dsp/src/, dsp/include/, the CMake files that build the
    engine (dsp/CMakeLists.txt, the root CMakeLists.txt, cmake/BrainscapeFpProfile.cmake),
    cmake/fp-forbidden-flags.txt or the arm toolchain file, and kSoundRevision was not
    bumped, unless the pull request carries the "sound-neutral" label (refactors, comments,
    tests; restricting who may apply it is the repository's job, not this script's);
  * kSoundRevision went down or skipped a number: a bump is exactly one;
  * the head's golden file is keyed to another revision than the head's kSoundRevision:
    a bump regenerates it with the harness's --mode mint.

New presets, counters and corpus versions change no golden hash, so they pass.

  sound_rev_gate.py --base REV [--head REV] [--labels JSON_OR_COMMA_LIST] [--summary FILE]
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


def golden_changes(base, head):
    """Presets of the base golden file whose hashes differ or are gone in the head's."""
    if base is None:
        return []
    old, new = preset_hashes(base), preset_hashes(head or {})
    changes = []
    for name, (h, secs) in sorted(old.items()):
        if name not in new:
            changes.append(f"{name}: dropped")
        elif new[name][0] != h:
            changes.append(f"{name}: hash {str(h)[:16]} -> {str(new[name][0])[:16]}")
        elif new[name][1] != secs:
            changes.append(f"{name}: per-second hashes differ")
    return changes


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
    ap.add_argument("--summary", help="also append the Markdown to this file ($GITHUB_STEP_SUMMARY)")
    args = ap.parse_args()

    paths = [p for p in git("diff", "--name-only", "--no-renames", args.base, args.head).stdout.splitlines() if p]
    touched = triggers(paths)
    labels = parse_labels(args.labels)
    base_rev, head_rev = revision(args.base), revision(args.head)
    if head_rev is None:
        sys.exit(f"sound_rev_gate: {REVISION_HEADER} is missing at {args.head}")
    base_rev = 0 if base_rev is None else base_rev
    bumped = head_rev > base_rev
    base_golden, head_golden = golden(args.base), golden(args.head)
    changed = golden_changes(base_golden, head_golden)

    failures, notes = [], []
    if head_rev < base_rev:
        failures.append(f"kSoundRevision went down, {base_rev} -> {head_rev}.")
    elif head_rev > base_rev + 1:
        failures.append(f"kSoundRevision skipped a number, {base_rev} -> {head_rev}: a bump is exactly one.")
    if changed and not bumped:
        failures.append(f"{len(changed)} golden hash(es) changed without a kSoundRevision bump (hard "
                        f"trigger, profile §5.12; no label overrides it).")
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

    lines = ["# Sound-revision gate", "",
             f"kSoundRevision {base_rev} -> {head_rev} ({'bumped' if bumped else 'not bumped'}); "
             f"labels: {', '.join(sorted(labels)) or 'none'}.", ""]
    lines.append(f"Sound-relevant paths changed: {len(touched)}" + (":" if touched else "."))
    lines += [f"- `{p}`" for p in touched]
    lines += ["", f"Golden hashes changed: {len(changed)}" + (":" if changed else ".")]
    lines += [f"- {c}" for c in changed]
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
