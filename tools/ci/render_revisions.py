#!/usr/bin/env python3
"""Renders each sound revision a pull request introduces below its head's (determinism profile
§5.12, companion §3.4; .github/workflows/sound-rev-render.yml).

The sound-revision gate (sound_rev_gate.py) checks a lower revision's golden file only by its
key and the commit that wrote it, and parity.yml and host.yml render only the head's, so each
revision commit is pushed and passes them as the pull request's head before the next is pushed.
This renders the lower ones too, on two of those toolchains, Linux GCC and the emulated
Cortex-M7, at every state at each revision a later one is built on. It does not replace that
rule: parity and host also cover Clang, arm64, MSVC (SSE2 and AVX2) and AppleClang, the M7 at
other block sizes, more block patterns, the hostile FP environment, the parity stream and the
unit tests.

  list    runs the gate with --list-revisions over the pull request (in CI, the merge commit's
          first parent and the merge commit) and checks the listing it writes: each revision
          introduced below the head's and the commits to render it at, every commit at that
          revision that a commit at another revision has as a parent (the next revision's bump,
          or a merge into a later revision). The gate's verdict is sound-rev.yml's and does not
          matter here (no labels are given, so its package rule may fail); a listing the gate
          does not write, as on a history it cannot read, fails. --github-output writes count=N
          (the revisions listed) for the workflow, which skips the toolchains and the render at 0.
  render  checks out each listed commit in a worktree of its own, builds that commit's own
          golden harness from it and runs it in --mode check against that commit's own
          golden.json:
            host (g++ by default)  48-frame blocks with ablations on (the mint's run),
                                   512-frame blocks and the mixed pattern 48,1,127,32;
            Cortex-M7              the M7 oracle ELF (the commit's own arm toolchain file, the
                                   pinned arm-none-eabi on PATH) at 48-frame blocks over the
                                   whole corpus, under --emulator (default
                                   "qemu-arm -cpu cortex-m7"; "" runs the ELF directly, through
                                   a binfmt handler, with QEMU_CPU=cortex-m7 set for it).
          The builds run one after another with --jobs parallel compiles, then the renders
          --jobs at a time, the M7's first.

A listing without revisions (the pull request introduces at most one) passes with nothing to
render. Everything else fails closed (exit 1): a listing that is missing or malformed, a revision
with no commit to render, a commit that cannot be checked out or whose header or golden.json is
not at the listed revision (a state built on whose golden file was never minted for it, or was
taken back) or lists no preset, a harness that does not configure or build, and a render that
exits non-zero, whose report does not name exactly the presets its golden file lists, that does
not print every preset matching, or that runs past --timeout minutes. Reports, logs and the WAV
files of mismatching presets go to --out, one directory per revision and commit.
Commit subjects are the pull request's text: control characters in them print as spaces.

  render_revisions.py list --base REV [--head REV] --listing FILE [--github-output FILE]
  render_revisions.py render --listing FILE [--work DIR] [--out DIR] [--jobs N] [--cxx CXX]
                             [--emulator CMD] [--timeout MIN] [--summary FILE]
"""
import argparse
import concurrent.futures
import glob
import hashlib
import json
import os
import shlex
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sound_rev_gate as gate  # noqa: E402  (the header and golden-file rules, in one place)

GATE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "sound_rev_gate.py")
# (leg, build, harness arguments, what it renders), the M7's first: it takes longest.
LEGS = [
    ("m7-b48", "m7", ["--no-ablate", "--block", "48"], "Cortex-M7 (emulated), 48-frame blocks"),
    ("host-b48", "host", ["--block", "48"], "host, 48-frame blocks, ablations on"),
    ("host-b512", "host", ["--no-ablate", "--block", "512"], "host, 512-frame blocks"),
    ("host-mixed", "host", ["--no-ablate", "--pattern", "48,1,127,32"], "host, blocks 48, 1, 127, 32"),
]
WAV_LEGS = ("m7-b48", "host-b48")


def fail(msg):
    sys.exit(f"render_revisions: {msg}")


def clean(text):
    """The pull request's text on one line: every control character a space."""
    return gate.CONTROL.sub(" ", text).strip()


def run(cmd, log, env=None, timeout=None):
    """Runs a command with its output appended to `log`. Returns the exit code, or None when it
    cannot start or runs past `timeout` seconds."""
    with open(log, "a", encoding="utf-8") as fh:
        fh.write(f"$ {' '.join(shlex.quote(c) for c in cmd)}\n")
        fh.flush()
        try:
            return subprocess.run(cmd, env=env, stdout=fh, stderr=subprocess.STDOUT, timeout=timeout).returncode
        except (OSError, subprocess.TimeoutExpired) as err:
            fh.write(f"render_revisions: {err}\n")
            return None


def first_line(cmd):
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
    except OSError as err:
        return f"unavailable ({err})"
    lines = p.stdout.strip().splitlines()
    return lines[0].strip() if p.returncode == 0 and lines else f"unavailable (exit {p.returncode})"


def tail(path, n=None):
    try:
        with open(path, encoding="utf-8", errors="replace") as fh:
            lines = fh.read().splitlines()
    except OSError:
        return []
    return lines[-n:] if n else lines


def describe(commit):
    """A commit as its `short hash` and subject."""
    p = subprocess.run(["git", "log", "-1", "--format=%h %s", commit], capture_output=True, text=True,
                       encoding="utf-8", errors="replace")
    if p.returncode != 0:
        return f"`{commit[:7]}`"
    short, _, subject = p.stdout.strip().partition(" ")
    return f"`{short}` {clean(subject)}"


def read_listing(path):
    """The gate's --list-revisions file, checked; fails closed on anything else."""
    try:
        with open(path, encoding="utf-8") as fh:
            doc = json.load(fh)
    except (OSError, ValueError) as err:
        fail(f"no revision listing at {path} ({err}): the gate did not list the pull request's revisions")
    ok = (isinstance(doc, dict) and doc.get("format") == gate.LISTING_FORMAT
          and all(isinstance(doc.get(k), str) for k in ("base", "head"))
          and all(isinstance(doc.get(k), int) for k in ("baseRevision", "headRevision"))
          and isinstance(doc.get("revisions"), list))
    for r in doc["revisions"] if ok else []:
        ok = ok and (isinstance(r, dict) and isinstance(r.get("revision"), int)
                     and isinstance(r.get("introduced"), list) and isinstance(r.get("render"), list)
                     and all(isinstance(c, str) and c for c in r["render"])
                     and len(set(r["render"])) == len(r["render"]))
    if ok and len({r["revision"] for r in doc["revisions"]}) != len(doc["revisions"]):
        ok = False
    if not ok:
        fail(f"{path} is not a {gate.LISTING_FORMAT} listing")
    return doc


def cmd_list(args):
    if os.path.exists(args.listing):
        os.remove(args.listing)
    log = args.listing + ".gate.txt"
    with open(log, "w", encoding="utf-8") as fh:
        p = subprocess.run([sys.executable, GATE, "--base", args.base, "--head", args.head,
                            "--list-revisions", args.listing], stdout=fh, stderr=subprocess.STDOUT)
    if not os.path.exists(args.listing):
        sys.stderr.write("".join(f"{line}\n" for line in tail(log, 40)))
        fail(f"the gate wrote no revision listing (exit {p.returncode}); its output is above")
    doc = read_listing(args.listing)
    revs = doc["revisions"]
    print(f"Sound revision {doc['baseRevision']} at the base, {doc['headRevision']} at the head "
          f"(`{doc['head'][:7]}`), whose golden file parity and host render.")
    if not revs:
        print("No revision below the head's: the pull request introduces at most one, so nothing is rendered here.")
    for r in revs:
        for c in r["render"]:
            print(f"- r{r['revision']}: rendered at {describe(c)}")
        if not r["render"]:
            print(f"- r{r['revision']}: no commit to render (the render fails)")
    print(f"The gate's report without labels is in {log}; sound-rev-gate gives the verdict.")
    if args.github_output:
        with open(args.github_output, "a", encoding="utf-8") as fh:
            fh.write(f"count={len(revs)}\n")
    return 0


def header_revision(path):
    """kSoundRevision in a header file as the gate reads it, or None."""
    try:
        with open(path, encoding="utf-8") as fh:
            code = gate.COMMENT.sub(" ", fh.read())
    except OSError:
        return None
    m = gate.REVISION.search(code)
    return int(m.group(1)) if m and len(gate.REVISION_NAME.findall(code)) == 1 else None


def golden_revision(path):
    try:
        with open(path, encoding="utf-8") as fh:
            return gate.keyed(fh.read())
    except OSError:
        return None


def presets(path):
    """The 'vector/preset' names a golden file or a harness report lists; empty when it cannot be
    read."""
    try:
        with open(path, encoding="utf-8") as fh:
            return set(gate.preset_hashes(json.load(fh)))
    except (OSError, ValueError, AttributeError, KeyError, TypeError):
        return set()


def find(build, names):
    for name in names:
        hits = sorted(h for h in glob.glob(os.path.join(build, "**", name), recursive=True) if os.path.isfile(h))
        if hits:
            return hits[0]
    return None


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


class Target:
    """A listed revision at one of its commits: the worktree, harnesses and what went wrong."""

    def __init__(self, rev, commit, args):
        self.rev = rev
        self.commit = commit  # None when the listing names no commit for the revision
        self.tag = f"r{rev}-{commit[:12]}" if commit else f"r{rev}"
        self.out = os.path.join(args.out, self.tag)
        self.wt = os.path.join(args.work, self.tag)
        self.problems = []
        self.bins = {}
        self.archive = None
        self.presets = set()  # the golden file's, which every render must cover
        os.makedirs(self.out, exist_ok=True)


def prepare(r, args, timings):
    """Checks the commit out in a worktree and builds its host and M7 harnesses."""
    if r.commit is None:
        r.problems.append("no commit to render: the listing names none at this revision")
        return
    log = os.path.join(r.out, "worktree.log")
    if run(["git", "worktree", "add", "--detach", r.wt, r.commit], log) != 0:
        r.problems.append(f"cannot check out {r.commit} in a worktree ({log})")
        return
    have = header_revision(os.path.join(r.wt, gate.REVISION_HEADER))
    key = golden_revision(os.path.join(r.wt, gate.GOLDEN))
    if have != r.rev or key != r.rev:
        r.problems.append(f"not at revision {r.rev}: its header reads {have!r}, its golden file is keyed to {key!r}")
        return
    r.presets = presets(os.path.join(r.wt, gate.GOLDEN))
    if not r.presets:
        r.problems.append(f"its {gate.GOLDEN} lists no preset")
        return
    configs = {
        "host": ["-DCMAKE_BUILD_TYPE=Release", "-DBRAINSCAPE_BUILD_TESTS=ON"],
        "m7": [f"-DCMAKE_TOOLCHAIN_FILE={os.path.join(r.wt, 'tools', 'cmake', 'arm-none-eabi-toolchain.cmake')}",
               "-DCMAKE_BUILD_TYPE=Release", "-DBRAINSCAPE_BUILD_TESTS=OFF", "-DBRAINSCAPE_BUILD_M7_ORACLE=ON"],
    }
    env = dict(os.environ, CXX=args.cxx)
    for build, flags in configs.items():
        t0 = time.monotonic()
        bdir = os.path.join(r.wt, f"build-{build}")
        log = os.path.join(r.out, f"build-{build}.log")
        ok = (run(["cmake", "-S", r.wt, "-B", bdir, *flags], log, env=env) == 0
              and run(["cmake", "--build", bdir, "--config", "Release", "--parallel", str(args.jobs),
                       "--target", "brainscape_golden"], log, env=env) == 0)
        binary = find(bdir, ["brainscape_golden.elf"] if build == "m7" else ["brainscape_golden",
                                                                             "brainscape_golden.exe"])
        timings.append((r.tag, f"build ({build})", time.monotonic() - t0))
        if not ok or binary is None:
            r.problems.append(f"the {build} harness does not build ({log})")
            continue
        r.bins[build] = binary
        archive = os.path.join(bdir, "dsp", "libbrainscape_dsp.a")
        if build == "m7" and os.path.isfile(archive):
            r.archive = sha256(archive)


def render(r, leg, build, extra, args, emulator, emulator_name):
    """One harness run in --mode check; returns (ok, presets rendered, seconds, why)."""
    log = os.path.join(r.out, f"{leg}.log")
    report_file = os.path.join(r.out, f"{leg}.json")
    for stale in (log, report_file):  # what a run before this one left must not count
        if os.path.exists(stale):
            os.remove(stale)
    cmd = [r.bins[build], "--mode", "check", "--golden", os.path.join(r.wt, gate.GOLDEN),
           "--tag", f"{r.tag}-{leg}", "--report", report_file, *extra]
    if leg in WAV_LEGS:
        os.makedirs(os.path.join(r.out, f"wav-{leg}"), exist_ok=True)
        cmd += ["--wav-dir", os.path.join(r.out, f"wav-{leg}")]
    if build == "m7":
        cmd = emulator + cmd + ["--note", f"emulator={emulator_name}"]
        if r.archive:
            cmd += ["--note", f"archiveSha256={r.archive}"]
    t0 = time.monotonic()
    rc = run(cmd, log, env=dict(os.environ, QEMU_CPU="cortex-m7"), timeout=args.timeout * 60)
    secs = time.monotonic() - t0
    # The harness's own report names the presets it rendered: the whole corpus, every one the
    # commit's golden file lists, or the check proves less than it says.
    rendered = presets(report_file)
    if rc is None:
        why = f"did not finish in {args.timeout:g} min" if secs >= args.timeout * 60 else "could not start"
    elif rc != 0:
        why = f"exit {rc}"
    elif rendered != r.presets:
        why = (f"rendered {len(rendered & r.presets)} of the golden file's {len(r.presets)} presets"
               + (f" and {len(rendered - r.presets)} it does not list" if rendered - r.presets else ""))
    elif "# golden: every rendered preset matches" not in tail(log):
        why = "did not report every preset matching"
    else:
        why = ""
    return not why, len(rendered), secs, why


def cmd_render(args):
    doc = read_listing(args.listing)
    head = f"Sound revision {doc['baseRevision']} at the base, {doc['headRevision']} at the head (`{doc['head'][:7]}`)"
    lines = ["# Sound revisions below the head's, rendered", ""]
    if not doc["revisions"]:
        lines += [f"{head}: no revision below the head's, so nothing to render. PASS"]
        report(lines, args.summary)
        return 0
    os.makedirs(args.out, exist_ok=True)
    args.work = os.path.abspath(args.work or tempfile.mkdtemp(prefix="sound-rev-render-"))
    args.out = os.path.abspath(args.out)
    emulator = shlex.split(args.emulator)
    emulator_name = (first_line([emulator[0], "--version"]) if emulator
                     else "binfmt handler with QEMU_CPU=cortex-m7")
    print(f"host: {first_line([args.cxx, '--version'])}")
    print(f"arm:  {first_line(['arm-none-eabi-g++', '--version'])}")
    print(f"M7 emulator: {' '.join(emulator) or '(binfmt)'}: {emulator_name}")
    print(f"{args.jobs} jobs; worktrees in {args.work}; reports in {args.out}")
    t_start = time.monotonic()
    timings = []
    revs = [Target(e["revision"], c, args) for e in doc["revisions"] for c in (e["render"] or [None])]
    for r in revs:
        print(f"== r{r.rev}: preparing {describe(r.commit) if r.commit else '(no commit)'}", flush=True)
        prepare(r, args, timings)
        for p in r.problems:
            print(f"   FAIL: {p}")
        if r.archive:
            print(f"   libbrainscape_dsp.a (M7) {r.archive}")
    t_built = time.monotonic()
    tasks = [(r, leg, build, extra) for leg, build, extra, _ in LEGS for r in revs if build in r.bins]
    results = {}
    print(f"== rendering {len(tasks)} run(s), {args.jobs} at a time", flush=True)
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = {pool.submit(render, r, leg, build, extra, args, emulator, emulator_name): (r, leg)
                   for r, leg, build, extra in tasks}
        for f in concurrent.futures.as_completed(futures):
            r, leg = futures[f]
            results[(r.tag, leg)] = f.result()
            ok, presets, secs, why = results[(r.tag, leg)]
            print(f"   {r.tag} {leg}: {'pass' if ok else 'FAIL: ' + why} ({presets} presets, {secs:.1f} s)", flush=True)
            if not ok:
                for line in tail(os.path.join(r.out, f"{leg}.log"), 30):
                    print(f"      {line}")
    t_end = time.monotonic()

    failed = []
    lines += [f"{head}, whose golden file parity and host render. Each revision below it is rendered "
              f"at every commit at it that a later revision is built on, by that commit's own harness, against "
              f"that commit's golden.json, on Linux GCC and the emulated M7 only: each revision commit still "
              f"passes parity and host as the pull request's head before the next is pushed.", "",
              "| Revision | Commit | Run | Result | Presets | Time |", "|---|---|---|---|---|---|"]
    for r in revs:
        at = (describe(r.commit) if r.commit else "none").replace("|", "\\|")
        where = f"r{r.rev} at `{r.commit[:7]}`" if r.commit else f"r{r.rev}"
        for p in r.problems:
            failed.append(f"{where}: {p}")
            lines.append(f"| r{r.rev} | {at} | prepare | **FAIL**: {p} | | |")
        for leg, _, _, what in LEGS:
            if (r.tag, leg) in results:
                ok, presets, secs, why = results[(r.tag, leg)]
                if not ok:
                    failed.append(f"{where} {leg} ({what}): {why}")
                lines.append(f"| r{r.rev} | {at} | {what} | {'pass' if ok else '**FAIL**: ' + why} | "
                             f"{presets} | {secs:.1f} s |")
            elif not r.problems:  # a run that went missing without a word is never a pass
                failed.append(f"{where} {leg} ({what}): not run")
    lines.append("")
    lines += [f"- r{r.rev} at `{r.commit[:7]}`: libbrainscape_dsp.a (M7) `{r.archive}`" for r in revs if r.archive]
    lines += [f"- M7 emulator: {emulator_name}; host compiler: {first_line([args.cxx, '--version'])}",
              f"- Builds {t_built - t_start:.0f} s ("
              + ", ".join(f"{tag} {what} {secs:.0f} s" for tag, what, secs in timings)
              + f"), renders {t_end - t_built:.0f} s ({args.jobs} at a time), {t_end - t_start:.0f} s in all.", ""]
    lines += [f"- **FAIL:** {f}" for f in failed]
    lines.append("" if failed else "PASS")
    report(lines, args.summary)
    if failed and os.environ.get("GITHUB_ACTIONS") == "true":
        for f in failed:
            print(f"::error title=sound-rev-render::{clean(f)}")
    return 1 if failed else 0


def report(lines, summary):
    text = "\n".join(clean(ln) for ln in lines) + "\n"
    print(text)
    if summary:
        with open(summary, "a", encoding="utf-8") as fh:
            fh.write(text)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="command", required=True)
    lp = sub.add_parser("list", help="list the revisions below the head's with the gate")
    lp.add_argument("--base", required=True, help="the commit the pull request merges into")
    lp.add_argument("--head", default="HEAD", help="the pull request, merged (default HEAD)")
    lp.add_argument("--listing", required=True, help="write the listing to this file")
    lp.add_argument("--github-output", help="append count=N to this file ($GITHUB_OUTPUT)")
    rp = sub.add_parser("render", help="render each listed revision at each of its commits")
    rp.add_argument("--listing", required=True, help="the listing `list` wrote")
    rp.add_argument("--work", help="where the worktrees and builds go (default: a new temporary directory)")
    rp.add_argument("--out", default="sound-rev-render", help="reports, logs and WAV files (default %(default)s)")
    rp.add_argument("--jobs", type=int, default=os.cpu_count() or 2, help="parallel compiles and renders")
    rp.add_argument("--cxx", default="g++", help="the host compiler (default %(default)s)")
    rp.add_argument("--emulator", default="qemu-arm -cpu cortex-m7",
                    help='the command that runs an M7 ELF (default "%(default)s"; "" runs it directly)')
    rp.add_argument("--timeout", type=float, default=40, help="minutes a render may take (default %(default)s)")
    rp.add_argument("--summary", help="also append the Markdown to this file ($GITHUB_STEP_SUMMARY)")
    args = ap.parse_args()
    return cmd_list(args) if args.command == "list" else cmd_render(args)


if __name__ == "__main__":
    sys.exit(main())
