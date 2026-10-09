#!/usr/bin/env python3
"""Renders each sound revision a pull request introduces below its head's on one of parity's
legs (determinism profile §5.12, companion §3.4; .github/workflows/sound-rev-render.yml).

The sound-revision gate (sound_rev_gate.py) checks a lower revision's golden file only by its
key and the commit that wrote it, and parity.yml renders only the head's. This renders each
lower one, at every state at it a later revision is built on, on every leg parity.yml's
parity-host and parity-m7 jobs run, with the renders that state's own parity.yml makes there:
what parity would have checked had that commit been the pull request's head, so that a pull
request carrying several revisions is checked in one push. While branch protection requires
the check sound-rev-render, a pull request may push several revisions at once; until the owner
requires it, each revision commit is pushed and passes parity and host as the pull request's
head before the next is pushed.

  list    runs the gate with --list-revisions over the pull request (in CI, the merge commit's
          first parent and the merge commit) and checks the listing it writes: each revision
          introduced below the head's and the commits to render it at, every commit at that
          revision that a commit at another revision has as a parent (the next revision's bump,
          or a merge into a later revision). The gate's verdict is sound-rev.yml's and does not
          matter here (no labels are given, so its package rule may fail); a listing the gate
          does not write, as on a history it cannot read, fails, and so does a head whose
          parity.yml parity_plan.py cannot read. --github-output writes count=N (the revisions
          listed), listing=<the listing on one line> and matrix=<the render jobs>: one per
          parity-host leg of the head's parity.yml, on its runner, rendering every listed
          commit, and one emulated-M7 job per listed commit (the M7's renders are the longest).
  check   reads a listing (the list job's output, written to a file) as render does, checks
          that --leg is one of the head's and --only one of the listing's, and writes count=N
          to --github-output: a render job runs it first and installs nothing at 0.
  render  --leg LEG checks out each listed commit (or the one --only REV:COMMIT names) in a
          worktree of its own, reads that commit's own parity.yml (parity_plan.py; the head's
          when its own does not run LEG, and the render fails when its own cannot be read),
          builds that commit's harness as that parity.yml builds it on LEG (its CMake options,
          the leg's CXX and CXXFLAGS and so its compiler: GCC, Clang, MSVC through CMake's
          Visual Studio generator, a multi-config build, or AppleClang; on the M7, the commit's
          own arm toolchain file with the pinned arm-none-eabi on PATH) and makes, in --mode
          check against that commit's own golden.json, every render that parity.yml makes on
          LEG: on parity-host at present 48-frame blocks with ablations, 512-frame blocks and
          a hostile caller FP environment on every leg, and on linux-x64-gcc also the other
          block sizes and patterns, random sizes, split event delivery and fresh engines; on
          the M7 its ELFs under --emulator (default "qemu-arm -cpu cortex-m7"; "" runs them
          directly, through a binfmt handler, with QEMU_CPU=cortex-m7 set): 48 and 512-frame
          blocks, {48, 1, 127, 32}, random sizes, a hostile caller FPSCR, the firmware parity
          image's stream checked by the commit's own tools/hil/parity_check.py, and flushing
          forced on inside the guard. The builds run one after another with --jobs parallel
          compiles, then the renders --jobs at a time, the longest first.
  wait    the check branch protection requires: lists the revisions itself and polls this
          workflow run's jobs (the GitHub API, GITHUB_REPOSITORY, GITHUB_RUN_ID and
          GITHUB_RUN_ATTEMPT, with GH_TOKEN or GITHUB_TOKEN) until the list job and every
          render job the listing needs have finished. It passes when the list job passed and
          the listing has no revision, or when every render job passed; it fails as soon as
          one did not, when one the listing does not need ran, when a needed one has not
          appeared --appear minutes after the list job finished, or when the jobs cannot be
          read. A render job counts at the latest attempt that ran it, and not below the list
          job's attempt (re-running all jobs waits for the new renders). Its job has no
          `needs`, so the check exists, pending, from the start of every run and a newer run
          supersedes an older run's result at once.

A listing without revisions (the pull request introduces at most one) passes with nothing to
render. Everything else fails closed (exit 1): a listing that is missing or malformed or names
another head than --head; a leg the head's parity.yml does not run, or one run on another OS
or architecture than parity runs it on; a revision with no commit to render; a commit that
cannot be checked out, whose header or golden.json is not at the listed revision (a state built
on whose golden file was never minted for it, or was taken back) or lists no preset, or whose
parity.yml cannot be read; a harness that does not configure or build, or that builds with
another compiler or without the leg's flags; a render that exits non-zero, whose report does
not name exactly the presets its golden file lists, that does not print every preset matching,
or that runs past --timeout minutes; and a stream that parity_check.py does not pass for every
preset of the golden file. Reports, logs and the WAV files of mismatching presets (of the
render parity keeps them for) go to --out, one directory per revision and commit; the
worktrees and builds under --work are removed at the end unless --keep. Commit subjects are
the pull request's text: control characters in them print as spaces, and the output is UTF-8
on every platform (a Windows runner's console code page cannot print every subject).

  render_revisions.py list --base REV [--head REV] --listing FILE [--github-output FILE]
  render_revisions.py check --listing FILE [--head REV] [--leg LEG] [--only REV:COMMIT]
                            [--github-output FILE]
  render_revisions.py render --leg LEG --listing FILE [--head REV] [--only REV:COMMIT]
                             [--work DIR] [--out DIR] [--jobs N] [--emulator CMD]
                             [--timeout MIN] [--summary FILE] [--keep]
  render_revisions.py wait --listing FILE [--head REV] [--summary FILE] [--poll SEC]
                           [--appear MIN] [--retries N]
"""
import argparse
import collections
import concurrent.futures
import glob
import hashlib
import json
import os
import platform
import re
import shlex
import shutil
import stat
import subprocess
import sys
import tempfile
import time
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import parity_plan as pp  # noqa: E402  (parity.yml's legs, builds and renders)
import sound_rev_gate as gate  # noqa: E402  (the header and golden-file rules, in one place)

GATE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "sound_rev_gate.py")
PARITY_YML = ".github/workflows/parity.yml"
COMMIT_ID = re.compile(r"[0-9a-f]{40}|[0-9a-f]{64}")
ONLY = re.compile(r"(\d+):([0-9a-f]{40}|[0-9a-f]{64}|)")
M7_LEG = pp.M7_LEG
# sound-rev-render.yml's job names: the list job, and "sound-rev-render (<matrix name>)".
WORKFLOW = "sound-rev-render"
LIST_JOB = f"{WORKFLOW} (list)"
HARNESS_TARGETS = {"golden": "brainscape_golden", "flush": "brainscape_golden_flush",
                   "stream": "brainscape_parity_stream"}
MATCH_LINE = "# golden: every rendered preset matches"
STREAM_PASS = re.compile(r"^VERDICT: PASS - (\d+) preset\(s\) match .*, whole corpus\)$", re.M)
# A render: its name (its report's), the harness (golden, flush or stream), the arguments
# besides --mode, --golden, --tag, --report, --wav-dir and --note, what it renders, its rough
# cost (the renders run the longest first) and whether parity keeps its mismatching WAV files.
Run = collections.namedtuple("Run", "name harness args what weight wav")


def run_name(render):
    """A short name for a render, from its arguments (b48, b512, hostile, split-b48, ...)."""
    if render.harness != "golden":
        return render.harness
    args, parts = list(render.args), []

    def take(option):
        if option in args:
            i = args.index(option)
            value = args[i + 1] if i + 1 < len(args) else ""
            del args[i:i + 2]
            return value
        return None

    def flag(option):
        if option in args:
            args.remove(option)
            return True
        return False

    delivery, block, pattern, seed, fp = (take(o) for o in ("--delivery", "--block", "--pattern", "--random-blocks",
                                                           "--fp-env"))
    fresh = flag("--fresh-engine")
    flag("--no-ablate")
    parts.append(delivery or "")
    parts.append(f"b{block}" if block else f"b{pattern.replace(',', '-')}" if pattern
                 else f"random{seed}" if seed else "")
    parts += [fp or "", "fresh" if fresh else ""] + [a.lstrip("-") for a in args]
    name = "-".join(p for p in parts if p) or "b48"
    return re.sub(r"[^\w.-]+", "_", name)[:60]


def runs_for(plan, leg):
    """The renders a parity_plan.Plan makes on a leg, as Runs with names of their own."""
    out, seen = [], collections.Counter()
    for r in pp.leg_renders(plan, leg):
        base = run_name(r)
        seen[base] += 1
        name = base if seen[base] == 1 else f"{base}-{seen[base]}"
        what = " ".join([HARNESS_TARGETS[r.harness], *r.args]) + (" (checked by parity_check.py)"
                                                                     if r.harness == "stream" else "")
        weight = 2 if r.harness == "flush" or (r.harness == "golden" and "--no-ablate" not in r.args) else 1
        out.append(Run(name, r.harness, list(r.args), what, weight, r.wav))
    return out


def configure_flags(setup, wt):
    """The leg's CMake options for a worktree's build: a relative toolchain file is the worktree's."""
    out = []
    for d in setup.defines:
        name, _, value = d.partition("=")
        if name == "-DCMAKE_TOOLCHAIN_FILE" and value and not os.path.isabs(value):
            d = f"{name}={os.path.join(wt, *value.split('/'))}"
        out.append(d)
    return out


def build_env(setup, leg):
    """The environment parity configures the leg in: its CXX (or none, for the platform's own
    compiler) and CXXFLAGS on a host leg; neither on the M7, whose toolchain file names the
    compiler and its flags."""
    env = dict(os.environ)
    for name in ("CXX", "CXXFLAGS"):
        env.pop(name, None)
    if leg != M7_LEG:
        if setup.cxx:
            env["CXX"] = setup.cxx
        env["CXXFLAGS"] = setup.cxxflags
    return env


def platform_problem(leg):
    """Why this machine is not where parity runs the leg, or None."""
    if leg == M7_LEG:
        return None
    system, machines = pp.PLATFORMS["-".join(leg.split("-")[:2])]
    here = (platform.system(), platform.machine().lower())
    if here[0] != system or here[1] not in machines:
        return f"{leg} runs on {system} {machines[0]}, and this is {here[0]} {here[1]}"
    return None


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


def read_text(path):
    try:
        with open(path, encoding="utf-8", errors="replace") as fh:
            return fh.read()
    except OSError:
        return ""


def tail(path, n=None):
    lines = read_text(path).splitlines()
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

    def ids(xs):
        return isinstance(xs, list) and all(isinstance(c, str) and COMMIT_ID.fullmatch(c) for c in xs)

    ok = (isinstance(doc, dict) and doc.get("format") == gate.LISTING_FORMAT
          and ids([doc.get("base"), doc.get("head")])
          and all(type(doc.get(k)) is int for k in ("baseRevision", "headRevision"))
          and isinstance(doc.get("revisions"), list))
    for r in doc["revisions"] if ok else []:
        ok = ok and (isinstance(r, dict) and type(r.get("revision")) is int
                     and ids(r.get("introduced")) and ids(r.get("render"))
                     and len(set(r["render"])) == len(r["render"]))
    if ok and len({r["revision"] for r in doc["revisions"]}) != len(doc["revisions"]):
        ok = False
    if not ok:
        fail(f"{path} is not a {gate.LISTING_FORMAT} listing")
    return doc


def check_head(doc, head):
    """Fails unless the listing is for `head` (None: any)."""
    if head is None:
        return
    p = subprocess.run(["git", "rev-parse", "--verify", f"{head}^{{commit}}"], capture_output=True, text=True)
    if p.returncode != 0:
        fail(f"cannot read {head}: {p.stderr.strip()}")
    if p.stdout.strip() != doc["head"]:
        fail(f"the listing is for {doc['head']}, and {head} is {p.stdout.strip()}: list this checkout's revisions")


def head_plan(commit):
    """The parity_plan.Plan of a commit's parity.yml; fails when it cannot be read."""
    p = subprocess.run(["git", "show", f"{commit}:{PARITY_YML}"], capture_output=True, text=True,
                       encoding="utf-8", errors="replace")
    if p.returncode != 0:
        fail(f"cannot read {PARITY_YML} at {commit}: {p.stderr.strip()}")
    try:
        return pp.read(p.stdout)
    except pp.PlanError as err:
        fail(f"the head's {PARITY_YML} is not one parity_plan.py can read: {err}")


def target_tag(rev, commit):
    """A listed revision at one of its commits, as a short name (Windows keeps MAX_PATH)."""
    return f"r{rev}-{commit[:10]}" if commit else f"r{rev}"


def targets(doc):
    """(revision, commit or None) for every state the listing names; None where a revision has
    none, which fails it."""
    return [(e["revision"], c) for e in doc["revisions"] for c in (e["render"] or [None])]


def select(doc, only):
    """The listing's targets, or the one --only REV:COMMIT names (COMMIT empty: the revision
    without a commit)."""
    every = targets(doc)
    if not only:
        return every
    m = ONLY.fullmatch(only)
    if m is None:
        fail(f"--only {only!r} is not REV:COMMIT")
    want = (int(m.group(1)), m.group(2) or None)
    if want not in every:
        fail(f"the listing has no r{want[0]} at {want[1] or 'no commit'}: --only names a state it does not list")
    return [want]


def matrix_entries(doc, plan):
    """The render jobs a listing needs: one per parity-host leg of the head's plan, rendering
    every listed state, and one emulated-M7 job per listed state. None without revisions."""
    if not doc["revisions"]:
        return []
    out = [{"name": leg.name, "leg": leg.name, "os": leg.os, "only": "", "artifact": leg.name}
           for leg in plan.legs.values()]
    for rev, commit in targets(doc):
        tag = target_tag(rev, commit)
        out.append({"name": f"{M7_LEG}, {tag}", "leg": M7_LEG, "os": plan.m7_os, "only": f"{rev}:{commit or ''}",
                    "artifact": f"{M7_LEG}-{tag}"})
    return out


def write_output(path, **values):
    if path:
        with open(path, "a", encoding="utf-8") as fh:
            fh.writelines(f"{k}={v}\n" for k, v in values.items())


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
    entries = matrix_entries(doc, head_plan(doc["head"]))
    revs = doc["revisions"]
    print(f"Sound revision {doc['baseRevision']} at the base, {doc['headRevision']} at the head "
          f"(`{doc['head'][:7]}`), whose golden file parity renders.")
    if not revs:
        print("No revision below the head's: the pull request introduces at most one, so nothing is rendered here.")
    for r in revs:
        for c in r["render"]:
            print(f"- r{r['revision']}: rendered at {describe(c)}")
        if not r["render"]:
            print(f"- r{r['revision']}: no commit to render (the render fails)")
    for e in entries:
        print(f"  job {WORKFLOW} ({e['name']}) on {e['os']}")
    print(f"The gate's report without labels is in {log}; sound-rev-gate gives the verdict.")
    # The listing holds commit ids and numbers only, and the matrix those and parity.yml's leg
    # and runner names, which parity_plan.py allows only as plain words: safe output values.
    write_output(args.github_output, count=len(revs), listing=json.dumps(doc, separators=(",", ":")),
                 matrix=json.dumps({"include": entries}, separators=(",", ":")))
    return 0


def cmd_check(args):
    doc = read_listing(args.listing)
    check_head(doc, args.head)
    if doc["revisions"] and args.leg:
        plan = head_plan(doc["head"])
        if not pp.has_leg(plan, args.leg):
            fail(f"{args.leg} is not a leg of the head's {PARITY_YML}")
    states = select(doc, args.only) if doc["revisions"] else []
    print(f"{len(doc['revisions'])} revision(s) below the head's r{doc['headRevision']}, "
          f"{len(states)} state(s) to render here.")
    write_output(args.github_output, count=len(doc["revisions"]))
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


def compiler_of(bdir):
    """(CMAKE_CXX_COMPILER_ID, its version, its path, CMAKE_CXX_FLAGS) of a configured build;
    empty strings where it cannot be read."""
    text = "".join(read_text(f) for f in glob.glob(os.path.join(bdir, "CMakeFiles", "*", "CMakeCXXCompiler.cmake")))
    found = [re.search(rf'^set\(CMAKE_CXX_{k} "([^"]*)"\)', text, re.M) for k in ("COMPILER_ID", "COMPILER_VERSION",
                                                                                 "COMPILER")]
    flags = re.search(r"^CMAKE_CXX_FLAGS:STRING=(.*)$", read_text(os.path.join(bdir, "CMakeCache.txt")), re.M)
    return tuple(m.group(1) if m else "" for m in found) + (flags.group(1).strip() if flags else "",)


def compiler_problem(leg, info, setup):
    """Why a configured build is not the leg's (another compiler, or the leg's flags missing), or
    None."""
    cid, version, path, flags = info
    if cid != setup.compiler:
        return f"it configured with {cid or 'an unknown compiler'} {version} ({path}), not {setup.compiler}"
    if leg == M7_LEG and not os.path.basename(path).startswith("arm-none-eabi-"):
        return f"it configured with {path}, not arm-none-eabi-g++"
    missing = [f for f in setup.cxxflags.split() if f not in flags.split()]
    if missing:
        return f"CMAKE_CXX_FLAGS is {flags!r}, without {' '.join(missing)}"
    return None


def remove_tree(path):
    def retry(func, p, _):
        os.chmod(p, stat.S_IWRITE)
        func(p)
    if os.path.isdir(path):
        if sys.version_info >= (3, 12):
            shutil.rmtree(path, onexc=retry)
        else:
            shutil.rmtree(path, onerror=retry)


class Target:
    """A listed revision at one of its commits: the worktree, its plan, harnesses and what went
    wrong."""

    def __init__(self, rev, commit, args):
        self.rev = rev
        self.commit = commit  # None when the listing names no commit for the revision
        # Short: Windows runners keep MAX_PATH, and the build's deepest paths sit below these.
        self.tag = target_tag(rev, commit)
        self.out = os.path.join(args.out, self.tag)
        self.wt = os.path.join(args.work, self.tag)
        self.bdir = self.wt + ".b"
        self.golden = os.path.normpath(os.path.join(self.wt, gate.GOLDEN))
        self.problems = []
        self.runs = []  # the renders of the parity.yml it is rendered by
        self.setup = None  # parity_plan.Build
        self.source = ""  # which parity.yml that is
        self.bins = {}  # harness -> the command that runs it
        self.archive = None
        self.compiler = None
        self.checked_out = False
        self.presets = set()  # the golden file's, which every render must cover
        os.makedirs(self.out, exist_ok=True)


def checkout(r):
    """Checks the commit out in a worktree and reads its revision and presets."""
    if r.commit is None:
        r.problems.append("no commit to render: the listing names none at this revision")
        return False
    log = os.path.join(r.out, "worktree.log")
    if run(["git", "worktree", "add", "--detach", r.wt, r.commit], log) != 0:
        r.problems.append(f"cannot check out {r.commit} in a worktree ({log})")
        return False
    r.checked_out = True
    have = header_revision(os.path.join(r.wt, gate.REVISION_HEADER))
    key = golden_revision(r.golden)
    if have != r.rev or key != r.rev:
        r.problems.append(f"not at revision {r.rev}: its header reads {have!r}, its golden file is keyed to {key!r}")
        return False
    r.presets = presets(r.golden)
    if not r.presets:
        r.problems.append(f"its {gate.GOLDEN} lists no preset")
        return False
    return True


def prepare(r, leg, head):
    """Reads the commit's own parity.yml: the build and renders it makes on the leg, or, when it
    does not run the leg, the head's."""
    text = read_text(os.path.join(r.wt, *PARITY_YML.split("/")))
    if not text:
        r.problems.append(f"it has no {PARITY_YML}")
        return False
    try:
        own = pp.read(text)
    except pp.PlanError as err:
        r.problems.append(f"its {PARITY_YML} is not one parity_plan.py can read ({err}), so what parity checked "
                          f"at it is not known")
        return False
    plan, r.source = ((own, "its own parity.yml") if pp.has_leg(own, leg)
                      else (head, f"the head's parity.yml, since its own does not run {leg}"))
    r.setup = pp.leg_build(plan, leg)
    r.runs = runs_for(plan, leg)
    return True


def build(r, leg, args, timings):
    """Configures and builds the commit's harnesses for the leg, as its parity.yml does."""
    t0 = time.monotonic()
    log = os.path.join(r.out, "build.log")
    env = build_env(r.setup, leg)
    remove_tree(r.bdir)  # a cache from another configure must not carry over
    ok = run(["cmake", "-S", r.wt, "-B", r.bdir, *configure_flags(r.setup, r.wt)], log, env=env) == 0
    if ok:
        info = compiler_of(r.bdir)
        r.compiler = f"{info[0]} {info[1]} ({info[2]}; CMAKE_CXX_FLAGS {info[3]!r})"
        why = compiler_problem(leg, info, r.setup)
        if why:
            r.problems.append(f"the {leg} harness is not {leg}'s: {why} ({log})")
            timings.append((r.tag, "build", time.monotonic() - t0))
            return
        ok = run(["cmake", "--build", r.bdir, "--config", "Release", "--parallel", str(args.jobs), "--target",
                  *r.setup.targets], log, env=env) == 0
    timings.append((r.tag, "build", time.monotonic() - t0))
    if not ok:
        r.problems.append(f"the {leg} harness does not configure or build ({log})")
        return
    for h in sorted({x.harness for x in r.runs}):
        name = HARNESS_TARGETS[h]
        binary = find(r.bdir, [name + ".elf"] if leg == M7_LEG else [name, name + ".exe"])
        if binary is None:
            r.problems.append(f"the {leg} build has no {name} ({log})")
            continue
        r.bins[h] = [binary]
    archive = os.path.join(r.bdir, "dsp", "libbrainscape_dsp.a")
    if leg == M7_LEG and os.path.isfile(archive):
        r.archive = sha256(archive)


def render(r, run_, args, emulator, emulator_name):
    """One render in --mode check; returns (ok, presets rendered, seconds, why)."""
    log = os.path.join(r.out, f"{run_.name}.log")
    report_file = os.path.join(r.out, f"{run_.name}.json")
    stream_file = os.path.join(r.out, f"{run_.name}.jsonl")
    for stale in (log, report_file, stream_file):  # what a run before this one left must not count
        if os.path.exists(stale):
            os.remove(stale)
    env = dict(os.environ, QEMU_CPU="cortex-m7")
    timeout = args.timeout * 60
    t0 = time.monotonic()
    if run_.harness == "stream":
        cmd = emulator + r.bins["stream"] + [*run_.args, "--tag", f"{r.tag}-{args.leg}", "--out", stream_file]
        rc = run(cmd, log, env=env, timeout=timeout)
        if rc == 0:
            rc = run([sys.executable, os.path.join(r.wt, "tools", "hil", "parity_check.py"), "--log", stream_file,
                      "--golden", r.golden], log, timeout=timeout)
        secs = time.monotonic() - t0
        m = STREAM_PASS.search(read_text(log))
        rendered = int(m.group(1)) if m else 0
        if rc is None:
            why = f"did not finish in {args.timeout:g} min" if secs >= timeout else "could not start"
        elif rc != 0:
            why = f"exit {rc}"
        elif m is None:
            why = "parity_check.py did not pass the whole corpus"
        elif rendered != len(r.presets):
            why = f"parity_check.py passed {rendered} presets of the golden file's {len(r.presets)}"
        else:
            why = ""
        return not why, rendered, secs, why
    cmd = [*r.bins[run_.harness], "--mode", "check", "--golden", r.golden, "--tag", f"{r.tag}-{args.leg}",
           "--report", report_file, *run_.args]
    if run_.wav:
        os.makedirs(os.path.join(r.out, f"wav-{run_.name}"), exist_ok=True)
        cmd += ["--wav-dir", os.path.join(r.out, f"wav-{run_.name}")]
    if args.leg == M7_LEG:
        cmd = emulator + cmd
        if run_.wav:
            cmd += ["--note", f"emulator={emulator_name}"] + (["--note", f"archiveSha256={r.archive}"]
                                                                if r.archive else [])
    rc = run(cmd, log, env=env, timeout=timeout)
    secs = time.monotonic() - t0
    # The harness's own report names the presets it rendered: the whole corpus, every one the
    # commit's golden file lists, or the check proves less than it says.
    rendered = presets(report_file)
    if rc is None:
        why = f"did not finish in {args.timeout:g} min" if secs >= timeout else "could not start"
    elif rc != 0:
        why = f"exit {rc}"
    elif rendered != r.presets:
        why = (f"rendered {len(rendered & r.presets)} of the golden file's {len(r.presets)} presets"
               + (f" and {len(rendered - r.presets)} it does not list" if rendered - r.presets else ""))
    elif MATCH_LINE not in tail(log):
        why = "did not report every preset matching"
    else:
        why = ""
    return not why, len(rendered), secs, why


def cleanup(revs, keep):
    if keep:
        return
    for r in revs:
        if r.checked_out:
            p = subprocess.run(["git", "worktree", "remove", "--force", r.wt], capture_output=True, text=True)
            if p.returncode != 0:
                print(f"   note: could not remove the worktree {r.wt}: {p.stderr.strip()}")
        remove_tree(r.bdir)


def cmd_render(args):
    doc = read_listing(args.listing)
    check_head(doc, args.head)
    head = f"Sound revision {doc['baseRevision']} at the base, {doc['headRevision']} at the head (`{doc['head'][:7]}`)"
    lines = [f"# Sound revisions below the head's, rendered on {args.leg}", ""]
    if not doc["revisions"]:
        lines += [f"{head}: no revision below the head's, so nothing to render. PASS"]
        report(lines, args.summary)
        return 0
    head_parity = head_plan(doc["head"])
    if not pp.has_leg(head_parity, args.leg):
        fail(f"{args.leg} is not a leg of the head's {PARITY_YML} ({', '.join([*head_parity.legs, M7_LEG])})")
    where = platform_problem(args.leg)
    if where:
        fail(f"{where}: parity runs the leg there, so its render here would prove another leg")
    states = select(doc, args.only)
    os.makedirs(args.out, exist_ok=True)
    args.work = os.path.abspath(args.work or tempfile.mkdtemp(prefix="sr-"))
    args.out = os.path.abspath(args.out)
    emulator = shlex.split(args.emulator) if args.leg == M7_LEG else []
    emulator_name = (first_line([emulator[0], "--version"]) if emulator
                     else "binfmt handler with QEMU_CPU=cortex-m7") if args.leg == M7_LEG else ""
    if args.leg == M7_LEG:
        print(f"M7 emulator: {' '.join(emulator) or '(binfmt)'}: {emulator_name}")
    print(f"{args.leg}: {args.jobs} jobs; worktrees in {args.work}; reports in {args.out}")
    t_start = time.monotonic()
    timings = []
    revs = [Target(rev, c, args) for rev, c in states]
    results = {}
    try:
        for r in revs:
            print(f"== r{r.rev}: preparing {describe(r.commit) if r.commit else '(no commit)'}", flush=True)
            if checkout(r) and prepare(r, args.leg, head_parity):
                print(f"   {len(r.runs)} render(s) from {r.source}")
                build(r, args.leg, args, timings)
            for p in r.problems:
                print(f"   FAIL: {p}")
            if r.compiler:
                print(f"   compiler: {r.compiler}")
            if r.archive:
                print(f"   libbrainscape_dsp.a (M7) {r.archive}")
        t_built = time.monotonic()
        tasks = sorted(((r, run_) for r in revs if not r.problems for run_ in r.runs),
                       key=lambda t: -t[1].weight)  # stable: the longest first, then the listing's order
        print(f"== rendering {len(tasks)} run(s), {args.jobs} at a time", flush=True)
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
            futures = {pool.submit(render, r, run_, args, emulator, emulator_name): (r, run_) for r, run_ in tasks}
            for f in concurrent.futures.as_completed(futures):
                r, run_ = futures[f]
                results[(r.tag, run_.name)] = f.result()
                ok, count, secs, why = results[(r.tag, run_.name)]
                print(f"   {r.tag} {run_.name}: {'pass' if ok else 'FAIL: ' + why} ({count} presets, {secs:.1f} s)",
                      flush=True)
                if not ok:
                    for line in tail(os.path.join(r.out, f"{run_.name}.log"), 30):
                        print(f"      {line}")
        t_end = time.monotonic()
    finally:
        cleanup(revs, args.keep)

    failed = []
    lines += [f"{head}, whose golden file parity renders. Each revision below it is rendered at every commit "
              f"at it that a later revision is built on, by that commit's own harness built as its own "
              f"parity.yml builds {args.leg}, against that commit's golden.json, with every render that "
              f"parity.yml makes on {args.leg}.", "",
              "| Revision | Commit | Run | Result | Presets | Time |", "|---|---|---|---|---|---|"]
    for r in revs:
        at = (describe(r.commit) if r.commit else "none").replace("|", "\\|")
        place = f"r{r.rev} at `{r.commit[:7]}`" if r.commit else f"r{r.rev}"
        for p in r.problems:
            failed.append(f"{place}: {p}")
            lines.append(f"| r{r.rev} | {at} | prepare | **FAIL**: {p} | | |")
        for run_ in r.runs:
            if (r.tag, run_.name) in results:
                ok, count, secs, why = results[(r.tag, run_.name)]
                if not ok:
                    failed.append(f"{place} {run_.name} ({run_.what}): {why}")
                lines.append(f"| r{r.rev} | {at} | `{run_.what}` | {'pass' if ok else '**FAIL**: ' + why} | "
                             f"{count} | {secs:.1f} s |")
            elif not r.problems:  # a run that went missing without a word is never a pass
                failed.append(f"{place} {run_.name} ({run_.what}): not run")
    lines.append("")
    lines += [f"- r{r.rev} at `{r.commit[:7]}`: {len(r.runs)} render(s) from {r.source}" for r in revs if r.source]
    compilers = sorted({r.compiler for r in revs if r.compiler})
    lines += [f"- Compiler: {c}" for c in compilers]
    lines += [f"- r{r.rev} at `{r.commit[:7]}`: libbrainscape_dsp.a (M7) `{r.archive}`" for r in revs if r.archive]
    if args.leg == M7_LEG:
        lines.append(f"- M7 emulator: {emulator_name}")
    lines += [f"- Builds {t_built - t_start:.0f} s (" + ", ".join(f"{tag} {secs:.0f} s" for tag, _, secs in timings)
              + f"), renders {t_end - t_built:.0f} s ({len(results)} runs, {args.jobs} at a time), "
              f"{t_end - t_start:.0f} s in all.", ""]
    lines += [f"- **FAIL:** {f}" for f in failed]
    lines.append("" if failed else "PASS")
    report(lines, args.summary)
    if failed and os.environ.get("GITHUB_ACTIONS") == "true":
        for f in failed:
            print(f"::error title=sound-rev-render ({args.leg})::{clean(f)}")
    return 1 if failed else 0


def report(lines, summary):
    text = "\n".join(clean(ln) for ln in lines) + "\n"
    print(text)
    if summary:
        with open(summary, "a", encoding="utf-8") as fh:
            fh.write(text)


# ---------------------------------------------------------------------------------------------
# wait: the required check.

def github_jobs():
    """Every job of this workflow run, of every attempt, from the GitHub API."""
    try:
        api = os.environ.get("GITHUB_API_URL", "https://api.github.com")
        url = f"{api}/repos/{os.environ['GITHUB_REPOSITORY']}/actions/runs/{os.environ['GITHUB_RUN_ID']}/jobs"
        token = os.environ.get("GH_TOKEN") or os.environ["GITHUB_TOKEN"]
    except KeyError as err:
        raise RuntimeError(f"{err.args[0]} is not set: wait reads this run's jobs from the GitHub API")
    jobs, page = [], 1
    while True:
        request = urllib.request.Request(f"{url}?filter=all&per_page=100&page={page}", headers={
            "Accept": "application/vnd.github+json", "Authorization": f"Bearer {token}",
            "X-GitHub-Api-Version": "2022-11-28", "User-Agent": "brainscape-sound-rev-render"})
        with urllib.request.urlopen(request, timeout=60) as response:
            doc = json.load(response)
        jobs += doc["jobs"]
        if not doc["jobs"] or len(jobs) >= doc["total_count"]:
            return jobs
        page += 1


fetch_jobs = github_jobs  # the self-test replaces it


def latest(jobs, attempt):
    """Each job name's run at the latest attempt up to `attempt` that ran it."""
    best = {}
    for j in jobs:
        a = j.get("run_attempt") or 1
        if not isinstance(j.get("name"), str) or a > attempt:
            continue
        old = best.get(j["name"])
        if old is None or (a, j.get("id") or 0) > (old.get("run_attempt") or 1, old.get("id") or 0):
            best[j["name"]] = j
    return best


def judge(jobs, want, attempt):
    """('pass', 'fail' or 'wait', why, {name: job}) for this run's jobs, the render jobs the
    listing needs (names) and this attempt."""
    best = latest(jobs, attempt)
    listing = best.get(LIST_JOB)
    if listing is None or listing.get("status") != "completed":
        return "wait", "the list job has not finished", best
    if listing.get("conclusion") != "success":
        return "fail", f"the list job concluded {listing.get('conclusion')}", best
    if not want:
        return "pass", "no revision below the head's, so nothing to render", best
    floor = listing.get("run_attempt") or 1
    renders = {name: j for name, j in best.items()
               if name.startswith(f"{WORKFLOW} (") and name != LIST_JOB and (j.get("run_attempt") or 1) >= floor}
    stray = sorted(set(renders) - set(want))
    if stray:
        return "fail", (f"job(s) the listing does not need: {', '.join(stray)} "
                        f"({', '.join(str(renders[n].get('conclusion')) for n in stray)})"), best
    bad = [f"{n} concluded {renders[n].get('conclusion')}" for n in want
           if n in renders and renders[n].get("status") == "completed" and renders[n].get("conclusion") != "success"]
    if bad:
        return "fail", "; ".join(bad), best
    missing = [n for n in want if n not in renders]
    running = [n for n in want if n in renders and renders[n].get("status") != "completed"]
    if missing or running:
        return "wait", (f"{len(want) - len(missing) - len(running)} of {len(want)} render job(s) passed; "
                        f"waiting for {', '.join(running + missing)}"), best
    return "pass", f"all {len(want)} render job(s) passed", best


def cmd_wait(args):
    doc = read_listing(args.listing)
    check_head(doc, args.head)
    entries = matrix_entries(doc, head_plan(doc["head"])) if doc["revisions"] else []
    want = [f"{WORKFLOW} ({e['name']})" for e in entries]
    attempt = int(os.environ.get("GITHUB_RUN_ATTEMPT") or 1)
    print(f"{len(doc['revisions'])} revision(s) below the head's r{doc['headRevision']}: waiting for {LIST_JOB}"
          + (f" and {len(want)} render job(s)" if want else "") + f" (attempt {attempt})", flush=True)
    errors, listed, last, best = 0, None, None, {}
    while True:
        try:
            jobs = fetch_jobs()
            errors = 0
        except RuntimeError as err:
            state, why = "fail", str(err)
            break
        except (OSError, ValueError, KeyError, TypeError) as err:  # urllib's errors are OSErrors
            errors += 1
            print(f"cannot read this run's jobs ({err}); attempt {errors} of {args.retries + 1}", flush=True)
            if errors > args.retries:
                state, why = "fail", f"cannot read this run's jobs ({err})"
                break
            time.sleep(args.poll)
            continue
        state, why, best = judge(jobs, want, attempt)
        if state != "wait":
            break
        done = best.get(LIST_JOB, {}).get("status") == "completed"
        if done and any(n not in best for n in want):
            listed = listed or time.monotonic()
            if time.monotonic() - listed > args.appear * 60:
                state, why = "fail", (f"{', '.join(n for n in want if n not in best)} had not appeared "
                                      f"{args.appear:g} min after the list job finished")
                break
        if why != last:
            print(f"waiting: {why}", flush=True)
            last = why
        time.sleep(args.poll)

    lines = [f"# {WORKFLOW}", "",
             f"Sound revision {doc['baseRevision']} at the base, {doc['headRevision']} at the head "
             f"(`{doc['head'][:7]}`): {len(doc['revisions'])} revision(s) below the head's, rendered by the jobs "
             f"below (attempt {attempt}).", "", "| Job | Result |", "|---|---|"]
    for name in [LIST_JOB, *want]:
        j = best.get(name) or {}
        lines.append(f"| {name} | {j.get('conclusion') or j.get('status') or 'not run'} |")
    lines += ["", f"- {why}", "", "PASS" if state == "pass" else f"**FAIL:** {why}"]
    report(lines, args.summary)
    if state != "pass":
        if os.environ.get("GITHUB_ACTIONS") == "true":
            print(f"::error title={WORKFLOW}::{clean(why)}")
        return 1
    return 0


def main(argv=None):
    # Commit subjects are any text; a Windows runner's console code page cannot print them all.
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="command", required=True)
    lp = sub.add_parser("list", help="list the revisions below the head's with the gate")
    lp.add_argument("--base", required=True, help="the commit the pull request merges into")
    lp.add_argument("--head", default="HEAD", help="the pull request, merged (default HEAD)")
    lp.add_argument("--listing", required=True, help="write the listing to this file")
    lp.add_argument("--github-output", help="append count=N, listing=JSON and matrix=JSON to this file "
                                            "($GITHUB_OUTPUT)")
    cp = sub.add_parser("check", help="read a listing and count its revisions")
    cp.add_argument("--listing", required=True, help="the listing `list` wrote")
    cp.add_argument("--head", help="fail unless the listing is for this commit")
    cp.add_argument("--leg", help="fail unless the head's parity.yml runs this leg")
    cp.add_argument("--only", help="fail unless the listing names this REV:COMMIT")
    cp.add_argument("--github-output", help="append count=N to this file ($GITHUB_OUTPUT)")
    rp = sub.add_parser("render", help="render each listed revision at each of its commits on one leg")
    rp.add_argument("--leg", required=True, help="the parity leg to render on (a parity-host leg, or m7-qemu)")
    rp.add_argument("--listing", required=True, help="the listing `list` wrote")
    rp.add_argument("--head", help="fail unless the listing is for this commit")
    rp.add_argument("--only", help="render only this REV:COMMIT of the listing (COMMIT empty: the revision "
                                   "without one)")
    rp.add_argument("--work", help="where the worktrees and builds go (default: a new temporary directory); "
                                   "keep it short on Windows")
    rp.add_argument("--out", default="sound-rev-render", help="reports, logs and WAV files (default %(default)s)")
    rp.add_argument("--jobs", type=int, default=os.cpu_count() or 2, help="parallel compiles and renders")
    rp.add_argument("--emulator", default="qemu-arm -cpu cortex-m7",
                    help='the command that runs an M7 ELF (default "%(default)s"; "" runs it directly)')
    rp.add_argument("--timeout", type=float, default=40, help="minutes a render may take (default %(default)s)")
    rp.add_argument("--summary", help="also append the Markdown to this file ($GITHUB_STEP_SUMMARY)")
    rp.add_argument("--keep", action="store_true", help="keep the worktrees and builds")
    wp = sub.add_parser("wait", help="wait for this run's list and render jobs and require them")
    wp.add_argument("--listing", required=True, help="the listing `list` wrote")
    wp.add_argument("--head", help="fail unless the listing is for this commit")
    wp.add_argument("--summary", help="also append the Markdown to this file ($GITHUB_STEP_SUMMARY)")
    wp.add_argument("--poll", type=float, default=30, help="seconds between reads of the jobs (default %(default)s)")
    wp.add_argument("--appear", type=float, default=20,
                    help="minutes a needed render job may take to appear after the list job (default %(default)s)")
    wp.add_argument("--retries", type=int, default=10, help="failed reads in a row tolerated (default %(default)s)")
    args = ap.parse_args(argv)
    return {"list": cmd_list, "check": cmd_check, "render": cmd_render, "wait": cmd_wait}[args.command](args)


if __name__ == "__main__":
    sys.exit(main())
