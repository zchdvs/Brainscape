#!/usr/bin/env python3
"""Renders each sound revision a pull request introduces below its head's on one of parity's
legs (determinism profile §5.12, companion §3.4; .github/workflows/sound-rev-render.yml).

The sound-revision gate (sound_rev_gate.py) checks a lower revision's golden file only by its
key and the commit that wrote it, and parity.yml renders only the head's. This renders each
lower one, at every state at it a later revision is built on, on every leg parity.yml's
parity-host and parity-m7 jobs run, with the renders those jobs make on that leg, so that a
pull request carrying several revisions is checked in one push. sound-rev-render.yml runs one
render job per leg and requires them all. While branch protection requires that check, a pull
request may push several revisions at once; until the owner requires it, each revision commit
is pushed and passes parity and host as the pull request's head before the next is pushed.

  list    runs the gate with --list-revisions over the pull request (in CI, the merge commit's
          first parent and the merge commit) and checks the listing it writes: each revision
          introduced below the head's and the commits to render it at, every commit at that
          revision that a commit at another revision has as a parent (the next revision's bump,
          or a merge into a later revision). The gate's verdict is sound-rev.yml's and does not
          matter here (no labels are given, so its package rule may fail); a listing the gate
          does not write, as on a history it cannot read, fails. --github-output writes count=N
          (the revisions listed) and listing=<the listing on one line> for the workflow, which
          skips the render legs at 0.
  check   reads a listing (the list job's output, written to a file) as render does and writes
          count=N to --github-output: a render leg runs it first and installs nothing at 0.
  render  --leg LEG checks out each listed commit in a worktree of its own, builds that commit's
          own golden harness from it as parity.yml builds it on LEG, and runs in --mode check,
          against that commit's own golden.json, every render parity.yml makes on LEG:
            every parity-host leg   48-frame blocks with ablations on, 512-frame blocks, and a
                                    hostile caller FP environment;
            linux-x64-gcc also      blocks of 1, 7, 32, 37, 64 and 127 frames, the patterns
                                    {48, 1, 127, 32} and {300, 512, 5, 64}, random sizes from
                                    seeds 1 and 2, wrapper-side event splitting at 48, 512,
                                    {300, 512, 5, 64} and random seed 3, and fresh engines;
            m7-qemu (parity-m7)     the M7 oracle ELFs (the commit's own arm toolchain file,
                                    the pinned arm-none-eabi on PATH) under --emulator (default
                                    "qemu-arm -cpu cortex-m7"; "" runs them directly, through a
                                    binfmt handler, with QEMU_CPU=cortex-m7 set): 48-frame
                                    blocks, a hostile caller FPSCR, 512-frame blocks,
                                    {48, 1, 127, 32}, random seed 1, the firmware parity
                                    image's stream checked by the commit's own
                                    tools/hil/parity_check.py, and flushing forced on inside
                                    the guard (every vector reproduced but the subnormal one).
          A host leg configures with parity-host's CMake options and its matrix's compiler and
          CXXFLAGS (CXX=g++ or clang++, or the platform's own: MSVC through CMake's Visual
          Studio generator, a multi-config build, or AppleClang; CXXFLAGS=/arch:AVX2 on
          windows-x64-msvc-avx2), the M7 leg with parity-m7's. The builds run one after another
          with --jobs parallel compiles, then the renders --jobs at a time, the longest first.

A listing without revisions (the pull request introduces at most one) passes with nothing to
render. Everything else fails closed (exit 1): a listing that is missing or malformed or names
another head than --head; a leg run on another OS or architecture than parity runs it on; a
revision with no commit to render; a commit that cannot be checked out or whose header or
golden.json is not at the listed revision (a state built on whose golden file was never minted
for it, or was taken back) or lists no preset; a harness that does not configure or build, or
that builds with another compiler or without the leg's flags; a render that exits non-zero,
whose report does not name exactly the presets its golden file lists, that does not print every
preset matching, or that runs past --timeout minutes; and a stream that parity_check.py does
not pass for every preset of the golden file. Reports, logs and the WAV files of mismatching
presets at 48-frame blocks go to --out, one directory per revision and commit; the worktrees
and builds under --work are removed at the end unless --keep. Commit subjects are the pull
request's text: control characters in them print as spaces.

  render_revisions.py list --base REV [--head REV] --listing FILE [--github-output FILE]
  render_revisions.py check --listing FILE [--head REV] [--github-output FILE]
  render_revisions.py render --leg LEG --listing FILE [--head REV] [--work DIR] [--out DIR]
                             [--jobs N] [--emulator CMD] [--timeout MIN] [--summary FILE]
                             [--keep]
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

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sound_rev_gate as gate  # noqa: E402  (the header and golden-file rules, in one place)

GATE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "sound_rev_gate.py")
COMMIT_ID = re.compile(r"[0-9a-f]{40}|[0-9a-f]{64}")

# parity.yml's parity-host matrix: leg -> (CXX, CXXFLAGS, the compiler CMake must find). An
# empty CXX leaves the platform's own compiler, as parity's Configure step does.
# test_render_revisions.py requires this table, and the renders below, to be parity.yml's.
HOST_LEGS = {
    "linux-x64-gcc": ("g++", "", "GNU"),
    "linux-x64-clang": ("clang++", "", "Clang"),
    "linux-arm64-gcc": ("g++", "", "GNU"),
    "windows-x64-msvc": ("", "", "MSVC"),
    "windows-x64-msvc-avx2": ("", "/arch:AVX2", "MSVC"),
    "macos-arm64-appleclang": ("", "", "AppleClang"),
    "macos-arm64-appleclang-latest": ("", "", "AppleClang"),
}
M7_LEG = "m7-qemu"  # parity-m7 (qemu-arm cortex-m7)
LEGS = [*HOST_LEGS, M7_LEG]
# The OS and machine a host leg's name says it runs on (platform.system(), platform.machine()).
PLATFORMS = {
    "linux-x64": ("Linux", ("x86_64", "amd64")),
    "linux-arm64": ("Linux", ("aarch64", "arm64")),
    "windows-x64": ("Windows", ("amd64", "x86_64")),
    "macos-arm64": ("Darwin", ("arm64", "aarch64")),
}

# The renders parity.yml makes, by its step names.
GCC_PATTERNS = ["1", "7", "32", "37", "64", "127", "48,1,127,32", "300,512,5,64"]  # other block sizes
GCC_RANDOM_SEEDS = ["1", "2"]
GCC_SPLIT_PATTERNS = ["48", "512", "300,512,5,64"]  # wrapper-side event splitting
GCC_SPLIT_RANDOM_SEED = "3"
M7_PATTERNS = ["512", "48,1,127,32"]  # parity-m7's other block sizes
M7_RANDOM_SEED = "1"
# A render: its name (the report's), the harness (golden, flush or stream), the arguments
# besides --mode, --golden, --tag, --report, --wav-dir and --note, what it renders, and its
# rough cost, by which the renders are ordered.
Run = collections.namedtuple("Run", "name harness args what weight")
HARNESS_TARGETS = {"golden": "brainscape_golden", "flush": "brainscape_golden_flush",
                   "stream": "brainscape_parity_stream"}
WAV_RUN = "b48"  # the 48-frame grid's render keeps the WAV files of mismatching presets
MATCH_LINE = "# golden: every rendered preset matches"
STREAM_PASS = re.compile(r"^VERDICT: PASS - (\d+) preset\(s\) match .*, whole corpus\)$", re.M)


def pattern_name(p):
    return "b" + p.replace(",", "-")


def plan(leg):
    """The renders parity.yml makes on a leg, for one revision."""
    if leg == M7_LEG:
        return ([Run("flush", "flush", ["--force-flush-control", "--no-ablate"],
                     "flushing forced on inside the guard (FZ)", 2),
                 Run("b48", "golden", ["--no-ablate"], "48-frame blocks", 1),
                 Run("hostile", "golden", ["--no-ablate", "--fp-env", "hostile"], "a hostile caller FPSCR", 1)]
                + [Run(pattern_name(p), "golden", ["--no-ablate", "--pattern", p], f"blocks {p}", 1)
                   for p in M7_PATTERNS]
                + [Run(f"random{M7_RANDOM_SEED}", "golden", ["--no-ablate", "--random-blocks", M7_RANDOM_SEED],
                       f"random blocks, seed {M7_RANDOM_SEED}", 1),
                   Run("stream", "stream", ["--max-block", "48", "--placement"],
                       "the parity image's stream, maxBlockSize 48, its placement (parity_check.py)", 1)])
    runs = [Run("b48", "golden", [], "48-frame blocks, ablations on", 2),
            Run("b512", "golden", ["--no-ablate", "--block", "512"], "512-frame blocks", 1),
            Run("hostile", "golden", ["--no-ablate", "--fp-env", "hostile"], "a hostile caller FP environment", 1)]
    if leg == "linux-x64-gcc":
        runs += [Run(pattern_name(p), "golden", ["--no-ablate", "--pattern", p], f"blocks {p}", 1)
                 for p in GCC_PATTERNS]
        runs += [Run(f"random{s}", "golden", ["--no-ablate", "--random-blocks", s], f"random blocks, seed {s}", 1)
                 for s in GCC_RANDOM_SEEDS]
        runs += [Run("split-" + pattern_name(p), "golden", ["--no-ablate", "--delivery", "split", "--pattern", p],
                     f"split events, blocks {p}", 1) for p in GCC_SPLIT_PATTERNS]
        runs += [Run(f"split-random{GCC_SPLIT_RANDOM_SEED}", "golden",
                     ["--no-ablate", "--delivery", "split", "--random-blocks", GCC_SPLIT_RANDOM_SEED],
                     f"split events, random blocks, seed {GCC_SPLIT_RANDOM_SEED}", 1),
                 Run("fresh", "golden", ["--no-ablate", "--fresh-engine"], "fresh engines", 1)]
    return runs


def configure_flags(leg, wt):
    """parity.yml's CMake options for the leg's build of a worktree."""
    if leg == M7_LEG:
        return [f"-DCMAKE_TOOLCHAIN_FILE={os.path.join(wt, 'tools', 'cmake', 'arm-none-eabi-toolchain.cmake')}",
                "-DCMAKE_BUILD_TYPE=Release", "-DBRAINSCAPE_BUILD_TESTS=OFF", "-DBRAINSCAPE_BUILD_M7_ORACLE=ON",
                "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"]
    return ["-DCMAKE_BUILD_TYPE=Release", "-DBRAINSCAPE_BUILD_TESTS=ON"]


def build_env(leg):
    """The environment parity.yml configures the leg in: its CXX (or none, for the platform's
    own compiler) and CXXFLAGS on a host leg; neither on the M7, whose toolchain file names the
    compiler and its flags."""
    env = dict(os.environ)
    for name in ("CXX", "CXXFLAGS"):
        env.pop(name, None)
    if leg in HOST_LEGS:
        cxx, flags, _ = HOST_LEGS[leg]
        if cxx:
            env["CXX"] = cxx
        env["CXXFLAGS"] = flags
    return env


def platform_problem(leg):
    """Why this machine is not where parity runs the leg, or None."""
    if leg == M7_LEG:
        return None
    system, machines = PLATFORMS["-".join(leg.split("-")[:2])]
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
    print(f"The gate's report without labels is in {log}; sound-rev-gate gives the verdict.")
    # The listing holds commit ids and numbers only, so one line of it is a safe output value.
    write_output(args.github_output, count=len(revs), listing=json.dumps(doc, separators=(",", ":")))
    return 0


def cmd_check(args):
    doc = read_listing(args.listing)
    check_head(doc, args.head)
    targets = sum(len(r["render"]) or 1 for r in doc["revisions"])
    print(f"{len(doc['revisions'])} revision(s) below the head's r{doc['headRevision']}, "
          f"{targets} state(s) to render.")
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


def compiler_problem(leg, info):
    """Why a configured build is not the leg's (another compiler, or the leg's flags missing), or
    None."""
    cid, version, path, flags = info
    want = HOST_LEGS[leg][2] if leg in HOST_LEGS else "GNU"
    if cid != want:
        return f"it configured with {cid or 'an unknown compiler'} {version} ({path}), not {want}"
    if leg == M7_LEG and not os.path.basename(path).startswith("arm-none-eabi-"):
        return f"it configured with {path}, not arm-none-eabi-g++"
    missing = [f for f in (HOST_LEGS[leg][1].split() if leg in HOST_LEGS else []) if f not in flags.split()]
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
    """A listed revision at one of its commits: the worktree, harnesses and what went wrong."""

    def __init__(self, rev, commit, args):
        self.rev = rev
        self.commit = commit  # None when the listing names no commit for the revision
        # Short: Windows runners keep MAX_PATH, and the build's deepest paths sit below these.
        self.tag = f"r{rev}-{commit[:10]}" if commit else f"r{rev}"
        self.out = os.path.join(args.out, self.tag)
        self.wt = os.path.join(args.work, self.tag)
        self.bdir = self.wt + ".b"
        self.golden = os.path.normpath(os.path.join(self.wt, gate.GOLDEN))
        self.problems = []
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


def build(r, leg, args, timings):
    """Configures and builds the commit's harnesses for the leg, as parity.yml does."""
    t0 = time.monotonic()
    log = os.path.join(r.out, "build.log")
    env = build_env(leg)
    harnesses = sorted({run_.harness for run_ in plan(leg)})
    remove_tree(r.bdir)  # a cache from another configure must not carry over
    ok = run(["cmake", "-S", r.wt, "-B", r.bdir, *configure_flags(leg, r.wt)], log, env=env) == 0
    if ok:
        info = compiler_of(r.bdir)
        r.compiler = f"{info[0]} {info[1]} ({info[2]}; CMAKE_CXX_FLAGS {info[3]!r})"
        why = compiler_problem(leg, info)
        if why:
            r.problems.append(f"the {leg} harness is not {leg}'s: {why} ({log})")
            timings.append((r.tag, "build", time.monotonic() - t0))
            return
        ok = run(["cmake", "--build", r.bdir, "--config", "Release", "--parallel", str(args.jobs), "--target",
                  *[HARNESS_TARGETS[h] for h in harnesses]], log, env=env) == 0
    timings.append((r.tag, "build", time.monotonic() - t0))
    if not ok:
        r.problems.append(f"the {leg} harness does not configure or build ({log})")
        return
    for h in harnesses:
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
    if run_.name == WAV_RUN:
        os.makedirs(os.path.join(r.out, f"wav-{run_.name}"), exist_ok=True)
        cmd += ["--wav-dir", os.path.join(r.out, f"wav-{run_.name}")]
    if args.leg == M7_LEG:
        cmd = emulator + cmd
        if run_.name == WAV_RUN:
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
    where = platform_problem(args.leg)
    if where:
        fail(f"{where}: parity runs the leg there, so its render here would prove another leg")
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
    revs = [Target(e["revision"], c, args) for e in doc["revisions"] for c in (e["render"] or [None])]
    runs = plan(args.leg)
    results = {}
    try:
        for r in revs:
            print(f"== r{r.rev}: preparing {describe(r.commit) if r.commit else '(no commit)'}", flush=True)
            if checkout(r):
                build(r, args.leg, args, timings)
            for p in r.problems:
                print(f"   FAIL: {p}")
            if r.compiler:
                print(f"   compiler: {r.compiler}")
            if r.archive:
                print(f"   libbrainscape_dsp.a (M7) {r.archive}")
        t_built = time.monotonic()
        tasks = sorted(((r, run_) for r in revs if not r.problems for run_ in runs),
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
              f"at it that a later revision is built on, by that commit's own harness built as parity builds "
              f"{args.leg}, against that commit's golden.json, with every render parity makes on {args.leg}.", "",
              "| Revision | Commit | Run | Result | Presets | Time |", "|---|---|---|---|---|---|"]
    for r in revs:
        at = (describe(r.commit) if r.commit else "none").replace("|", "\\|")
        place = f"r{r.rev} at `{r.commit[:7]}`" if r.commit else f"r{r.rev}"
        for p in r.problems:
            failed.append(f"{place}: {p}")
            lines.append(f"| r{r.rev} | {at} | prepare | **FAIL**: {p} | | |")
        for run_ in runs:
            if (r.tag, run_.name) in results:
                ok, count, secs, why = results[(r.tag, run_.name)]
                if not ok:
                    failed.append(f"{place} {run_.name} ({run_.what}): {why}")
                lines.append(f"| r{r.rev} | {at} | {run_.what} | {'pass' if ok else '**FAIL**: ' + why} | "
                             f"{count} | {secs:.1f} s |")
            elif not r.problems:  # a run that went missing without a word is never a pass
                failed.append(f"{place} {run_.name} ({run_.what}): not run")
    lines.append("")
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


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="command", required=True)
    lp = sub.add_parser("list", help="list the revisions below the head's with the gate")
    lp.add_argument("--base", required=True, help="the commit the pull request merges into")
    lp.add_argument("--head", default="HEAD", help="the pull request, merged (default HEAD)")
    lp.add_argument("--listing", required=True, help="write the listing to this file")
    lp.add_argument("--github-output", help="append count=N and listing=JSON to this file ($GITHUB_OUTPUT)")
    cp = sub.add_parser("check", help="read a listing and count its revisions")
    cp.add_argument("--listing", required=True, help="the listing `list` wrote")
    cp.add_argument("--head", help="fail unless the listing is for this commit")
    cp.add_argument("--github-output", help="append count=N to this file ($GITHUB_OUTPUT)")
    rp = sub.add_parser("render", help="render each listed revision at each of its commits on one leg")
    rp.add_argument("--leg", required=True, choices=LEGS, help="the parity leg to render on")
    rp.add_argument("--listing", required=True, help="the listing `list` wrote")
    rp.add_argument("--head", help="fail unless the listing is for this commit")
    rp.add_argument("--work", help="where the worktrees and builds go (default: a new temporary directory); "
                                   "keep it short on Windows")
    rp.add_argument("--out", default="sound-rev-render", help="reports, logs and WAV files (default %(default)s)")
    rp.add_argument("--jobs", type=int, default=os.cpu_count() or 2, help="parallel compiles and renders")
    rp.add_argument("--emulator", default="qemu-arm -cpu cortex-m7",
                    help='the command that runs an M7 ELF (default "%(default)s"; "" runs it directly)')
    rp.add_argument("--timeout", type=float, default=40, help="minutes a render may take (default %(default)s)")
    rp.add_argument("--summary", help="also append the Markdown to this file ($GITHUB_STEP_SUMMARY)")
    rp.add_argument("--keep", action="store_true", help="keep the worktrees and builds")
    args = ap.parse_args(argv)
    return {"list": cmd_list, "check": cmd_check, "render": cmd_render}[args.command](args)


if __name__ == "__main__":
    sys.exit(main())
