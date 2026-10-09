#!/usr/bin/env python3
"""Sound-revision gate for a pull request (determinism profile §5.12, companion §3.4).

Compares two commits, the pull request's base and its head (in CI, the merge commit's
first parent and the merge commit), reads every commit between them (base..head: the pull
request's own commits and the merge commit, not those already in the base), and fails
(exit 1) when:

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
  * the sound-revision rule, per commit: each revision is one commit that raises
    kSoundRevision by exactly one, and its golden file is minted at that revision, by that
    commit or a later one before the next bump; a pull request may carry several consecutive
    revisions. A commit's revision is the header's one `kSoundRevision = N;` outside comments,
    and a header that does not read that way stops the gate at any commit it reads, so the
    commit must be rewritten. A commit without the header keeps the highest of its parents'
    revisions (0 before the header existed), so the commit that restores it is checked against
    the revision before it went. A commit fails when its revision is below the highest of its
    parents' (it went down) or more than one above it (it skipped a number), and introduces its
    revision when it is exactly one above (a merge commit may, when it resolves two lines by
    bumping). The revisions introduced above the base's must be the base's + 1 to the head's,
    each introduced by one commit (two are two different sounds sharing a number), and each
    minted: a commit of the pull request at that revision writes golden.json keyed to it. The
    harness's --mode mint keys the file to the kSoundRevision it was built with, so a file
    keyed to another revision than its own commit's (written before the bump, after the next
    one, or on a line without the revision) mints nothing. A commit that introduces a revision
    the base already has (a parallel line claimed its number) fails: renumber on top of the
    base. The head going below the base fails too. A golden file below the head's revision is
    checked here by its key and the commit that wrote it, not rendered: parity.yml renders the
    head's, and sound-rev-render.yml renders each lower revision on every parity leg (parity-host's
    seven and the emulated Cortex-M7, with every render that commit's own parity.yml makes on
    each) at every commit at it that a later revision is built on (--list-revisions names them).
    While branch protection requires sound-rev-render, a pull request may push several revisions
    at once; until the owner requires it, each revision commit is pushed and passes parity and
    host as the pull request's head before the next revision's commit is pushed;
  * the head's golden file is keyed to another revision than the head's kSoundRevision:
    a bump regenerates it with the harness's --mode mint.

New presets, packages, counters and corpus versions change no committed hash, so they pass,
and so does a re-stamped package (sound_rev and package hash only).

The walk needs the whole history between base and head: in a shallow clone (actions/checkout
fetches one commit unless told fetch-depth: 0), or when a commit or file it reads is missing,
the gate fails and asks for the full history. It never passes on a history it cannot read.
The rule holds on main only when pull requests land as merge commits: a squash merge of one
that carries r2 and r3 lands a single commit that raises the revision by two.

Commit subjects, the head's golden.json, labels and the description are the pull request's
text: the report prints every control character in a line as a space, so none of them can start
a line of its own (a line starting "::" is a workflow command to the Actions runner).

--list-revisions FILE also writes, as JSON, each revision the pull request introduces below the
head's: the commits that introduce it (one, in a pull request the rule passes) and the ones to
render, every commit of the walk at that revision that a commit of the walk at another revision
has as a parent (the next revision's bump, or a merge into a later revision): each state at that
revision a later one is built on, on whichever line, whatever its golden.json says (a tip whose
golden.json is not keyed to the revision is render_revisions.py's to fail). A revision below the
head's always has one. tools/ci/render_revisions.py reads it. The file is written once the walk
has read the whole history, whatever the verdict; the gate writes no file when it cannot read the
history. The verdict and the exit code are the same with or without it.

  sound_rev_gate.py --base REV [--head REV] [--labels JSON_OR_COMMA_LIST]
                    [--body-file FILE] [--summary FILE] [--list-revisions FILE]
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
REVISION_NAME = re.compile(r"\bkSoundRevision\b")
COMMENT = re.compile(r"//[^\n]*|/\*.*?\*/", re.S)
# Control characters and the Unicode line breaks: printed as they are, a carriage return in a
# commit subject would start a line of its own.
CONTROL = re.compile(r"[\x00-\x1f\x7f\x85\u2028\u2029]+")
# The package rule (mode-compiler.md §8.3). bspc's manifest format: package hash, sound_hash,
# control_hash and path per line (`bspc roundtrip --write-manifest`). bspc_roundtrip.py
# requires each set's MANIFEST and that it matches the set's committed packages, so a package
# cannot change without its line here changing; golden.json's soundHash and controlHash, which
# the harness's mint writes, tie a golden preset's render to its package.
PACKAGE_LABEL = "package-change"
PACKAGE_MANIFESTS = ("dsp/tests/golden/presets/MANIFEST", "firmware/factory/MANIFEST")
PACKAGE_CAUSE = re.compile(r"^[ \t]*(?:[-*][ \t]+)?package-change:[ \t]*(\S.*?)[ \t]*$",
                           re.IGNORECASE | re.MULTILINE)
LISTING_FORMAT = "brainscape-sound-revisions/2"
MANIFEST_LINE = re.compile(r"^([0-9a-f]{64}) ([0-9a-f]{64}) ([0-9a-f]{64}) (\S.*)$")
FULL_HISTORY = ("fetch the full history (actions/checkout with fetch-depth: 0) and run the gate again; "
                "it does not pass on a history it cannot read")
BLOBS = {}
SUBJECTS = {}


def git(*args, check=True):
    p = subprocess.run(["git", *args], capture_output=True, text=True, encoding="utf-8")
    if check and p.returncode != 0:
        sys.exit(f"sound_rev_gate: git {' '.join(args)} failed: {p.stderr.strip()}")
    return p


def unread(what, p):
    """Fails closed on a commit or file the repository does not have."""
    sys.exit(f"sound_rev_gate: cannot read {what} ({'; '.join(p.stderr.strip().splitlines())}): {FULL_HISTORY}.")


def blob_id(rev, path):
    """The object id of a file at a revision, or None when it does not exist there.

    Fails closed when the revision, or a tree on the file's path, is not in the repository: a
    file a shallow or partial clone lacks is not a file the history lacks.
    """
    p = git("ls-tree", "--full-tree", "-z", rev, "--", path, check=False)
    if p.returncode != 0:
        unread(f"{path} at {rev}", p)
    for entry in p.stdout.split("\0"):
        meta, _, name = entry.partition("\t")
        if name == path and meta.split(" ")[1] == "blob":
            return meta.split(" ")[2]
    return None


def blob(oid):
    """A file's text by its object id."""
    if oid not in BLOBS:
        p = git("cat-file", "blob", oid, check=False)
        if p.returncode != 0:
            unread(f"object {oid}", p)
        BLOBS[oid] = p.stdout
    return BLOBS[oid]


def show(rev, path):
    """The file at a revision, or None when it does not exist there."""
    oid = blob_id(rev, path)
    return None if oid is None else blob(oid)


def revision(rev):
    """kSoundRevision at a revision, or None when the header does not exist there.

    The header must name it once outside comments, as `kSoundRevision = N;`: a comment, or a
    second definition the preprocessor may pick, would otherwise decide the number at commits
    no compiler or harness checks.
    """
    text = show(rev, REVISION_HEADER)
    if text is None:
        return None
    code = COMMENT.sub(" ", text)
    m = REVISION.search(code)
    if m is None or len(REVISION_NAME.findall(code)) != 1:
        sys.exit(f"sound_rev_gate: {REVISION_HEADER} at {describe(rev)} does not name kSoundRevision once "
                 f"outside comments, as `kSoundRevision = N;`")
    return int(m.group(1))


def golden(rev):
    text = show(rev, GOLDEN)
    if text is None:
        return None
    try:
        return json.loads(text)
    except ValueError as err:
        sys.exit(f"sound_rev_gate: {GOLDEN} at {rev} is not JSON: {err}")


def keyed(text):
    """The sound revision a golden file says it is for, or None when the text is not one."""
    try:
        g = json.loads(text)
    except ValueError:
        return None
    return g.get("soundRevision") if isinstance(g, dict) else None


def history(base, head):
    """The commits of base..head, parents first, as (commit, its kSoundRevision, the highest of
    its parents', the revision it mints or None, its golden.json's object id or None), notes on
    commits that remove the header, and {commit: its parents}.

    A commit without the header keeps the highest of its parents' revisions; a parent in the
    base without it counts as 0. A commit writes golden.json when its file differs from every
    parent's, so a merge that keeps one side's file writes nothing, and mints only when the file
    it writes is keyed to its own revision. Fails closed on a shallow clone or a commit it
    cannot read.
    """
    if git("rev-parse", "--is-shallow-repository").stdout.strip() != "false":
        sys.exit(f"sound_rev_gate: the repository is a shallow clone, so the pull request's commits "
                 f"cannot all be read: {FULL_HISTORY}.")
    p = git("rev-list", "--topo-order", "--reverse", "--parents", f"{base}..{head}", check=False)
    if p.returncode != 0:
        unread(f"the commits of {base}..{head}", p)
    states, walked = {}, {}

    def state(commit):
        """(kSoundRevision, None without the header; golden.json's object id or None)."""
        if commit not in states:
            states[commit] = (revision(commit), blob_id(commit, GOLDEN))
        return states[commit]

    def rev_of(commit):
        """A walked commit's revision as resolved below; one of the base's, its header's or 0."""
        return walked[commit] if commit in walked else state(commit)[0] or 0

    out, notes, parents_of = [], [], {}
    for line in p.stdout.splitlines():
        commit, *parents = line.split()
        parents_of[commit] = parents
        rev, gold = state(commit)
        prev = max((rev_of(c) for c in parents), default=0)
        if rev is None:
            rev = prev
            if any(state(c)[0] is not None for c in parents):
                notes.append(f"{REVISION_HEADER} is gone at {describe(commit)}, so that commit counts as its "
                             f"parents' revision, {prev}.")
        walked[commit] = rev
        writes = gold is not None and all(state(c)[1] != gold for c in parents)
        key = keyed(blob(gold)) if writes else None
        out.append((commit, rev, prev, key if key == rev else None, gold))
    return out, notes, parents_of


def describe(commit):
    """A commit as its `short hash` and subject, with any control character in the subject (the
    pull request's text) as a space."""
    if commit not in SUBJECTS:
        SUBJECTS[commit] = CONTROL.sub(" ", git("log", "-1", "--format=`%h` %s", commit).stdout).strip()
    return SUBJECTS[commit]


def revision_failures(commits, base_rev, head_rev):
    """The sound-revision rule over the walked commits.

    Returns (failures, {revision: the commits that introduce it}, {revision: the commits that
    mint it, at that revision}).
    """
    failures, introduced, mints = [], {}, {}
    for commit, rev, prev, minted, _ in commits:
        if rev < prev:
            failures.append(f"kSoundRevision went down at {describe(commit)}, {prev} -> {rev}.")
        elif rev > prev + 1:
            failures.append(f"kSoundRevision skipped a number at {describe(commit)}, {prev} -> {rev}: each "
                            f"revision is one commit that raises it by exactly one.")
        elif rev == prev + 1:
            introduced.setdefault(rev, []).append(commit)
        if minted is not None:
            mints.setdefault(minted, []).append(commit)
    # A commit that went down or skipped already explains a gap in the numbers or a head below
    # the base.
    stepped = bool(failures)
    if head_rev < base_rev and not stepped:
        failures.append(f"kSoundRevision went down, {base_rev} -> {head_rev}.")
    missing = [str(r) for r in range(base_rev + 1, head_rev + 1) if r not in introduced]
    if missing and not stepped:
        failures.append(f"Sound revision(s) {', '.join(missing)} between the base's {base_rev} and the head's "
                        f"{head_rev} are introduced by no commit of the pull request: each revision is one "
                        f"commit that raises kSoundRevision by exactly one.")
    for r, cs in sorted(introduced.items()):
        if len(cs) > 1:
            failures.append(f"Sound revision {r} is introduced by {len(cs)} commits, "
                            f"{'; '.join(describe(c) for c in cs)}: two different sounds cannot share a "
                            f"number; renumber one of them on top of the other.")
        if r <= base_rev:
            failures += [f"{describe(c)} introduces sound revision {r}, a number the base already has "
                         f"(kSoundRevision {base_rev} there): a parallel line claimed it; renumber the pull "
                         f"request's revisions on top of the base's, from {base_rev + 1}." for c in cs]
        if r not in mints:
            failures.append(f"Sound revision {r} never minted its golden file: no commit of the pull request at "
                            f"that revision writes {GOLDEN} keyed to it; mint it at that revision with "
                            f"brainscape_golden --mode mint.")
    return failures, introduced, mints


def listing(commits, parents, introduced, head_rev):
    """Each revision introduced below the head's, for sound-rev-render: the commits that
    introduce it, and the ones to render, in the walk's order: every walked commit at that
    revision that a walked commit at another revision has as a parent (the next revision's bump,
    or a merge into a later revision). Those are the states at that revision the pull request
    builds on, a side line merged after the next bump included, so neither the walk's order nor
    a golden file's key picks them. Their golden files are not read here: render_revisions.py
    fails a tip whose golden.json is not keyed to its revision."""
    rev = {c: r for c, r, *_ in commits}
    order = {c: i for i, (c, *_) in enumerate(commits)}
    tips = {}
    for c, r, *_ in commits:
        for p in parents[c]:
            if rev.get(p, r) != r:  # a parent outside the walk is the base's, never a tip
                tips.setdefault(rev[p], set()).add(p)
    return [{"revision": r, "introduced": cs, "render": sorted(tips.get(r, ()), key=order.get)}
            for r, cs in sorted(introduced.items()) if r < head_rev]


def write_listing(path, base, head, base_rev, head_rev, revisions):
    """Writes the --list-revisions file: the two ends as full commit ids, their revisions and
    listing()'s revisions."""
    ends = [git("rev-parse", "--verify", f"{end}^{{commit}}").stdout.strip() for end in (base, head)]
    doc = {"format": LISTING_FORMAT, "base": ends[0], "head": ends[1], "baseRevision": base_rev,
           "headRevision": head_rev, "revisions": revisions}
    try:
        with open(path, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(json.dumps(doc, indent=2) + "\n")
    except OSError as err:
        sys.exit(f"sound_rev_gate: cannot write the revision listing {path}: {err}")


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
    ap.add_argument("--list-revisions", metavar="FILE",
                    help="also write the revisions introduced below the head's, and the commits to render "
                         "each at, to this file as JSON (for tools/ci/render_revisions.py)")
    args = ap.parse_args()

    commits, notes, parents = history(args.base, args.head)
    paths = [p for p in git("diff", "--name-only", "--no-renames", args.base, args.head).stdout.splitlines() if p]
    touched = triggers(paths)
    labels = parse_labels(args.labels)
    causes = PACKAGE_CAUSE.findall(read_body(args.body_file))
    base_rev, head_rev = revision(args.base), revision(args.head)
    if head_rev is None:
        sys.exit(f"sound_rev_gate: {REVISION_HEADER} is missing at {args.head}")
    base_rev = 0 if base_rev is None else base_rev
    bumped = head_rev > base_rev
    # The sound-revision rule first: the listing needs only the walk, so it is written before any
    # other file is read, whatever the verdict.
    failures, introduced, mints = revision_failures(commits, base_rev, head_rev)
    if args.list_revisions:
        write_listing(args.list_revisions, args.base, args.head, base_rev, head_rev,
                      listing(commits, parents, introduced, head_rev))
    base_golden, head_golden = golden(args.base), golden(args.head)
    packages, packaged = package_changes(
        base_golden, head_golden, {p: manifest(args.base, p) for p in PACKAGE_MANIFESTS},
        {p: manifest(args.head, p) for p in PACKAGE_MANIFESTS})
    # A changed render is its package's only when the engine did not change unbumped beside it:
    # with both changed, nothing tells which one moved it, so it is the hard trigger's.
    engine_unbumped = bool(touched) and not bumped
    changed, attributed = golden_changes(base_golden, head_golden, frozenset() if engine_unbumped else packaged)
    withheld = golden_changes(base_golden, head_golden, packaged)[1] if engine_unbumped else []

    unrendered = [f"r{r}" for r in sorted(introduced) if r < head_rev]
    if unrendered:
        notes.append(f"The golden file of {', '.join(unrendered)} is checked here by its key and the commit that "
                     f"wrote it, not rendered: parity renders r{head_rev}'s, and sound-rev-render renders each on "
                     f"every parity leg, at every commit at it a later revision is built on. While branch "
                     f"protection requires sound-rev-render, that covers them; until then, each revision commit "
                     f"must have passed parity and host as the pull request's head (profile §5.12).")
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
    lines.append(f"Sound revisions introduced ({len(commits)} commit(s) read): {len(introduced)}"
                 + (":" if introduced else "."))
    for r, cs in sorted(introduced.items()):
        lines += [f"- r{r}: {describe(c)}" for c in cs]
        lines += [f"  - minted at {describe(c)}" for c in mints.get(r, [])] or ["  - never minted"]
    lines += ["", f"Sound-relevant paths changed: {len(touched)}" + (":" if touched else ".")]
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
    # Subjects, the head's golden.json, labels and the description's cause are the pull request's
    # text: no control character in them may start a line of its own (a "::" line is a workflow
    # command).
    text = "\n".join(CONTROL.sub(" ", line) for line in lines) + "\n"
    print(text)
    if args.summary:
        with open(args.summary, "a", encoding="utf-8") as fh:
            fh.write(text)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
