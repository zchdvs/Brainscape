#!/usr/bin/env python3
"""Self-test of render_revisions.py (.github/workflows/sound-rev-render.yml, determinism profile
§5.12).

Three parts; each case prints ok or FAIL, and any FAIL exits 1:

  parity      render_revisions.py's legs and renders are parity.yml's: sound-rev-render.yml's
              render matrix lists parity-host's legs on their runners and parity-m7's; HOST_LEGS
              gives each parity-host leg its matrix's cxx and cxxflags; configure_flags() and the
              harness targets are parity-host's and parity-m7's; plan() makes on each leg exactly
              the harness invocations parity-host's and parity-m7's steps make on it (their
              arguments besides --mode, --tag, --report, --wav-dir, --note and the output file,
              each loop expanded); and the M7 leg uses parity-m7's caches and pinned installers.
              A change to parity.yml's legs or renders fails here until the render follows it.
  listings    read_listing() takes the gate's listings and refuses every malformed one (exit 1);
              `list` fails without writing a count when the gate cannot list, and writes the
              count and the listing for a synthetic pull request; `check` counts a listing and
              refuses one for another head.
  render      the fail-closed paths, on a synthetic pull request built with
              test_sound_rev_gate.py's helpers, with a stand-in harness (a Python script) in place
              of the CMake build and a stand-in parity_check.py in the synthetic commits: a
              render that mismatches, covers fewer or other presets than the golden file, writes
              no report, does not print every preset matching or times out; a stream that
              parity_check.py fails, or passes for fewer presets or not as the whole corpus; a
              revision with no commit, a commit that does not exist, one at another revision, one
              whose golden file is keyed to the revision before and one whose golden file lists no
              preset; a build that does not configure (the real CMake call, on a commit without a
              CMakeLists.txt); a compiler other than the leg's or without its flags; a leg run on
              another platform than parity's; and a listing for another head. Every one must exit
              1 and name its failure; the passing runs must pass, render every run of the leg's
              plan, and leave no worktree behind.

  test_render_revisions.py
"""
import collections
import contextlib
import io
import json
import os
import re
import shlex
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import render_revisions as rr  # noqa: E402
import test_sound_rev_gate as t  # noqa: E402  (the synthetic repository's helpers)

ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
PARITY = os.path.join(ROOT, ".github", "workflows", "parity.yml")
WORKFLOW = os.path.join(ROOT, ".github", "workflows", "sound-rev-render.yml")
RESULTS = []


def case(name, wrong):
    """Records a case: `wrong` is the list of what is wrong (empty to pass)."""
    RESULTS.append(not wrong)
    print(f"{'ok  ' if not wrong else 'FAIL'} {name}")
    for w in wrong:
        print(f"       {w}")


# ---------------------------------------------------------------------------------------------
# parity: the legs and renders against parity.yml.

def read(path):
    with open(path, encoding="utf-8") as fh:
        return fh.read()


def job(text, name):
    """A top-level job's text in a workflow."""
    m = re.search(rf"^  {re.escape(name)}:\n(.*?)(?=^  [\w-]+:\n|\Z)", text, re.M | re.S)
    if m is None:
        raise ValueError(f"no job {name}")
    return m.group(1)


def matrix(block):
    """A job's matrix include list as dicts (leg, os, cxx, cxxflags, ...)."""
    body = block[block.index("include:"):block.index("runs-on:")]
    legs = []
    for line in body.splitlines():
        m = re.match(r"^\s+- leg: (\S+)\s*$", line)
        if m:
            legs.append({"leg": m.group(1)})
            continue
        m = re.match(r"^\s+([\w-]+): (\S+)\s*$", line)
        if m and legs:
            legs[-1][m.group(1)] = m.group(2)
    return legs


def steps(block):
    """A job's steps as text, shell continuations and folded lines joined."""
    parts = re.split(r"^      - ", block, flags=re.M)[1:]
    return [re.sub(r"\s*\n\s*", " ", re.sub(r"\s*\\\n\s*", " ", p)) for p in parts]


def invocations(step, pattern):
    """(harness, arguments) for each harness invocation in a step, loops expanded and --mode,
    --tag and what follows it dropped."""
    loops = dict(re.findall(r"for (\w+) in ([^;]+); do", step))
    out = []
    for harness, rest in re.findall(pattern, step):
        args = shlex.split(rest)
        if "--mode" in args:
            i = args.index("--mode")
            del args[i:i + 2]
        variables = [a[1:] for a in args if a.startswith("$")]
        if not variables:
            out.append((harness, tuple(args)))
            continue
        (var,) = variables
        out += [(harness, tuple(v if a == f"${var}" else a for a in args)) for v in loops[var].split()]
    return out


HOST_CALL = r'"\$BIN"()( --mode "\$MODE".*?) --tag'
M7_CALL = r"\b(brainscape_golden_flush|brainscape_golden|brainscape_parity_stream)\.elf((?: (?!--tag)\S+)*) --tag"


def parity_cases():
    text, ours = read(PARITY), read(WORKFLOW)
    host, m7 = job(text, "parity-host"), job(text, "parity-m7")
    legs = matrix(host)
    names = [x["leg"] for x in legs]
    case("rr.HOST_LEGS lists parity-host's legs in its order",
         [] if names == list(rr.HOST_LEGS) else [f"parity-host: {names}; HOST_LEGS: {list(rr.HOST_LEGS)}"])
    case("each host leg's CXX and CXXFLAGS are parity-host's matrix's",
         [f"{x['leg']}: parity {x.get('cxx', '')!r} {x.get('cxxflags', '')!r}, ours {rr.HOST_LEGS.get(x['leg'])}"
          for x in legs if rr.HOST_LEGS.get(x["leg"], (None, None))[:2] != (x.get("cxx", ""), x.get("cxxflags", ""))])
    m7_os = re.search(r"^    runs-on: (\S+)", m7, re.M).group(1)
    want = [(x["leg"], x["os"]) for x in legs] + [(rr.M7_LEG, m7_os)]
    got = [(x["leg"], x["os"]) for x in matrix(job(ours, "render"))]
    case("sound-rev-render.yml's render matrix is parity-host's legs and runners, then parity-m7's",
         [] if got == want else [f"render matrix {got}, parity {want}"])

    # The configure options and the harness targets.
    def defines(block, step_name):
        step = next(s for s in steps(block) if s.startswith(f"name: {step_name}"))
        return re.findall(r"-D\S+", step)

    def ours_flags(leg):
        return [f.replace(os.sep, "/").replace("WT/", "") for f in rr.configure_flags(leg, "WT")]

    wrong = []
    for leg, block, step in [("linux-x64-gcc", host, "Configure"), (rr.M7_LEG, m7, "Configure (firmware flags)")]:
        if defines(block, step) != ours_flags(leg):
            wrong.append(f"{leg}: parity configures {defines(block, step)}, ours {ours_flags(leg)}")
    for leg, block in [("linux-x64-gcc", host), (rr.M7_LEG, m7)]:
        build = next(s for s in steps(block) if "id: build" in s)
        targets = sorted(re.search(r"--target ((?:brainscape_\w+ ?)+)", build).group(1).split())
        plan_targets = sorted({rr.HARNESS_TARGETS[r.harness] for r in rr.plan(leg)})
        if targets != plan_targets:
            wrong.append(f"{leg}: parity builds {targets}, the plan renders {plan_targets}")
    case("the configure options and harness targets are parity-host's and parity-m7's", wrong)

    # The renders, leg by leg.
    wrong = []
    for leg in rr.LEGS:
        found = collections.Counter()
        for step in steps(host if leg in rr.HOST_LEGS else m7):
            only = re.search(r"matrix\.leg == '([\w-]+)'", step)
            if only and only.group(1) != leg:
                continue
            if leg in rr.HOST_LEGS:
                found.update(("brainscape_golden", a) for _, a in invocations(step, HOST_CALL))
            else:
                found.update(invocations(step, M7_CALL))
        planned = collections.Counter((rr.HARNESS_TARGETS[r.harness], tuple(r.args)) for r in rr.plan(leg))
        if found != planned:
            wrong.append(f"{leg}: parity only {sorted(found - planned)}, the plan only {sorted(planned - found)}")
        names = [r.name for r in rr.plan(leg)]
        if len(set(names)) != len(names) or rr.WAV_RUN not in names:
            wrong.append(f"{leg}: run names {names} repeat or lack {rr.WAV_RUN}")
    if "tools/hil/parity_check.py --log" not in read(PARITY):
        wrong.append("parity-m7 no longer checks the stream with tools/hil/parity_check.py --log")
    case("plan() makes on every leg exactly the renders parity-host and parity-m7 make on it", wrong)

    render = job(ours, "render")
    pinned = re.findall(r"^\s+key: (\S+)$", m7, re.M) + re.findall(r"^\s+run: (bash tools/ci/install_\S+ \S+)$", m7, re.M)
    case("the M7 leg uses parity-m7's cache keys and pinned installers",
         [f"missing: {p}" for p in pinned if p not in render] + ([] if len(pinned) == 4 else [f"parity-m7: {pinned}"]))
    aggregate = job(ours, "sound-rev-render")
    case("the required check is the job sound-rev-render, which always runs and needs both jobs",
         [w for w, ok in (("name", "    name: sound-rev-render\n" in aggregate),
                          ("if: always()", "    if: always()\n" in aggregate),
                          ("needs", "    needs: [list, render]\n" in aggregate),
                          ("triggers", "types: [opened, synchronize, reopened, edited]" in ours),
                          ("concurrency", "cancel-in-progress: true" in ours)) if not ok])


# ---------------------------------------------------------------------------------------------
# listings.

A, B, C = "a" * 40, "b" * 40, "c" * 40
GOOD = {"format": rr.gate.LISTING_FORMAT, "base": A, "head": B, "baseRevision": 3, "headRevision": 7,
        "revisions": [{"revision": 4, "introduced": [C], "render": [C]},
                      {"revision": 5, "introduced": [A], "render": [A, B]}]}


def variant(**changes):
    doc = json.loads(json.dumps(GOOD))
    for k, v in changes.items():
        if k.startswith("rev_"):
            doc["revisions"][0][k[4:]] = v
        else:
            doc[k] = v
    return doc


MALFORMED = [
    ("not JSON", "{"),
    ("a list", []),
    ("another format", variant(format="brainscape-sound-revisions/1")),
    ("a short base id", variant(base="abc1234")),
    ("a head id with an option", variant(head="--orphan")),
    ("a revision number as text", variant(headRevision="7")),
    ("a revision number as a boolean", variant(baseRevision=True)),
    ("revisions not a list", variant(revisions={})),
    ("a revision without commits to render", variant(rev_render=None)),
    ("commits to render as text", variant(rev_render=C)),
    ("a commit to render that is an option", variant(rev_render=["--force"])),
    ("a commit to render twice", variant(rev_render=[C, C])),
    ("introducing commits as text", variant(rev_introduced=C)),
    ("a revision number as text in a revision", variant(rev_revision="4")),
    ("two entries for one revision", variant(revisions=[GOOD["revisions"][0], GOOD["revisions"][0]])),
]


def exits(fn, *args):
    """(exit code, message) of a function that may sys.exit; output swallowed."""
    with contextlib.redirect_stdout(io.StringIO()) as out:
        try:
            code, msg = fn(*args), ""
        except SystemExit as e:
            code, msg = (1, str(e.code)) if isinstance(e.code, str) else (e.code, "")
    return code, msg, out.getvalue()


def listing_cases(scratch, repo, ids):
    def write(doc, name="listing.json"):
        path = os.path.join(scratch, name)
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(doc if isinstance(doc, str) else json.dumps(doc))
        return path

    for name, doc in [("the gate's listing", GOOD), ("no revisions", variant(revisions=[])),
                      ("a revision with no commit to render (render fails it)", variant(rev_render=[]))]:
        doc, msg, _ = exits(rr.read_listing, write(doc))
        case(f"read_listing takes {name}", [] if isinstance(doc, dict) and not msg else [msg])
    for name, doc in MALFORMED:
        code, msg, _ = exits(rr.read_listing, write(doc))
        case(f"read_listing refuses {name}", [] if code == 1 and ("is not a" in msg or "no revision listing" in msg)
             else [f"exit {code}: {msg!r}"])
    code, msg, _ = exits(rr.read_listing, os.path.join(scratch, "missing.json"))
    case("read_listing refuses a missing file", [] if code == 1 and "no revision listing" in msg else [msg])

    # `list` and `check` as the workflow runs them, in the synthetic repository.
    def script(*args):
        return subprocess.run([sys.executable, os.path.join(HERE, "render_revisions.py"), *args], cwd=repo,
                              capture_output=True, text=True, encoding="utf-8", errors="replace")

    out, listing = os.path.join(scratch, "github-output.txt"), os.path.join(scratch, "pr.json")
    open(out, "w").close()
    p = script("list", "--base", "0" * 40, "--head", ids["head"], "--listing", listing, "--github-output", out)
    case("list fails, and writes no count, when the gate cannot list",
         [] if p.returncode == 1 and "wrote no revision listing" in p.stderr and read(out) == ""
         else [f"exit {p.returncode}, output {read(out)!r}: {p.stderr[-300:]}"])
    p = script("list", "--base", ids["fork"], "--head", ids["head"], "--listing", listing, "--github-output", out)
    got = dict(line.split("=", 1) for line in read(out).splitlines())
    want = [{"revision": 2, "introduced": [ids["r2"]], "render": [ids["r2"]]}]
    case("list writes the count and the listing on one line",
         [] if p.returncode == 0 and got.get("count") == "1" and json.loads(got.get("listing", "{}")).get(
             "revisions") == want else [f"exit {p.returncode}: {got} {p.stderr[-300:]}"])
    open(out, "w").close()
    p = script("check", "--listing", listing, "--head", ids["head"], "--github-output", out)
    case("check counts the listing for its head",
         [] if p.returncode == 0 and read(out) == "count=1\n" else [f"exit {p.returncode}: {read(out)!r} {p.stderr}"])
    p = script("check", "--listing", listing, "--head", ids["r3"])
    case("check refuses a listing for another head",
         [] if p.returncode == 1 and "the listing is for" in p.stderr else [f"exit {p.returncode}: {p.stderr}"])
    return listing


# ---------------------------------------------------------------------------------------------
# render: the fail-closed paths, with a stand-in harness.

# The stand-in harness: `fake.py KIND ARGS`, as brainscape_golden(_flush) or the parity stream.
# FAKE_MODE applies to the run whose report or stream file is named FAKE_RUN (all when unset) in
# the target directory named FAKE_TAG (all when unset); every other run passes.
FAKE = r'''
import json, os, sys, time
kind, args = sys.argv[1], sys.argv[2:]
opt = lambda name: args[args.index(name) + 1] if name in args else None
out = opt("--report") or opt("--out")
run, tag = os.path.splitext(os.path.basename(out))[0], os.path.basename(os.path.dirname(out))
mode = os.environ.get("FAKE_MODE", "pass")
if os.environ.get("FAKE_RUN", run) != run or os.environ.get("FAKE_TAG", tag) != tag:
    mode = "pass"
if kind == "stream":
    with open(out, "w") as fh:
        fh.write("stream " + mode + "\n")
    sys.exit(3 if mode == "streamcrash" else 0)
golden = json.load(open(opt("--golden")))
names = [(v["name"], p["name"]) for v in golden["vectors"] for p in v["presets"]]
if mode == "missing":
    names = names[1:]
if mode == "extra":
    names.append(("plucks_12s", "not_in_golden"))
if mode != "noreport":
    vectors = {}
    for v, p in names:
        vectors.setdefault(v, []).append({"name": p, "hash": "00"})
    with open(out, "w") as fh:
        json.dump({"vectors": [{"name": v, "presets": ps} for v, ps in vectors.items()]}, fh)
if mode == "slow":
    time.sleep(30)
print("# golden: MISMATCH" if mode == "mismatch" else "# golden: preset noise" if mode == "silent"
      else "# golden: every rendered preset matches")
sys.exit(2 if mode == "mismatch" else 0)
'''
# The stand-in tools/hil/parity_check.py the synthetic commits carry.
PARITY_CHECK = r'''
import json, sys
args = sys.argv[1:]
log, golden = args[args.index("--log") + 1], json.load(open(args[args.index("--golden") + 1]))
mode = open(log).read().split()[1]
n = sum(len(v["presets"]) for v in golden["vectors"])
if mode == "streamfail":
    print("VERDICT: FAIL - 1 difference(s) listed above")
    sys.exit(1)
if mode == "streamsilent":
    sys.exit(0)
print("VERDICT: PASS - %d preset(s) match golden.json bit for bit, 0 package(s) match MANIFEST "
      "(sound revision %s, %s)" % (n - (mode == "streamshort"), golden["soundRevision"],
                                  "subset" if mode == "streamsubset" else "whole corpus"))
'''


def fake_build(fake):
    def build(r, leg, args, timings):
        timings.append((r.tag, "build", 0.0))
        r.compiler = "stand-in"
        r.bins = {h: [sys.executable, fake, h] for h in {x.harness for x in rr.plan(leg)}}
    return build


def render(scratch, repo, listing, leg, env=None, head=None, extra=()):
    """Runs render in-process in the synthetic repository: (exit code, error or summary)."""
    summary = os.path.join(scratch, "summary.md")
    if os.path.exists(summary):
        os.remove(summary)
    saved, cwd = dict(os.environ), os.getcwd()
    os.environ.update(env or {})
    os.chdir(repo)
    try:
        work = tempfile.mkdtemp(prefix="w", dir=scratch)
        code, msg, out = exits(rr.main, ["render", "--leg", leg, "--listing", listing, "--work", work,
                                         "--out", os.path.join(scratch, "out"), "--jobs", "4", "--emulator", "",
                                         "--timeout", "0.1", "--summary", summary, *(["--head", head] if head else []),
                                         *extra])
    finally:
        os.chdir(cwd)
        os.environ.clear()
        os.environ.update(saved)
    return code, msg or (read(summary) if os.path.exists(summary) else out)


def render_cases(scratch, repo, ids, pr_listing):
    fake = os.path.join(scratch, "fake.py")
    with open(fake, "w", encoding="utf-8") as fh:
        fh.write(FAKE)
    real_build = rr.build
    rr.build = fake_build(fake)

    def listing(*revisions):
        doc = {"format": rr.gate.LISTING_FORMAT, "base": ids["fork"], "head": ids["head"], "baseRevision": 1,
               "headRevision": 3, "revisions": [{"revision": r, "introduced": [ids["r2"]], "render": cs}
                                                for r, cs in revisions]}
        path = os.path.join(scratch, f"l{len(os.listdir(scratch))}.json")
        with open(path, "w", encoding="utf-8") as fh:
            json.dump(doc, fh)
        return path

    def worktrees():
        return [ln for ln in subprocess.run(["git", "-C", repo, "worktree", "list", "--porcelain"], capture_output=True,
                                            text=True).stdout.splitlines() if ln.startswith("worktree ")]

    def expect(name, code_msg, want, plan_leg=None, runs=None):
        code, text = code_msg
        wrong = []
        if want == 0:
            if code != 0 or not text.rstrip().endswith("PASS"):
                wrong.append(f"exit {code}: {text[-600:]}")
            if plan_leg and text.count("| pass |") != runs:
                wrong.append(f"{text.count('| pass |')} passing runs, not {runs}")
        elif code != 1 or want not in text:
            wrong.append(f"exit {code}, without {want!r}: {text[-600:]}")
        if len(worktrees()) != 1:
            wrong.append(f"worktrees left behind: {worktrees()}")
        case(name, wrong)

    m7 = rr.M7_LEG
    m7_runs = len(rr.plan(m7))
    one = listing((2, [ids["r2"]]))
    expect("the M7 plan passes at the listed commit", render(scratch, repo, pr_listing, m7, head=ids["head"]), 0, m7,
           m7_runs)
    expect("two commits at one revision: both rendered", render(scratch, repo, listing((2, [ids["r2"], ids["x2"]])), m7),
           0, m7, 2 * m7_runs)
    tag_x2 = f"r2-{ids['x2'][:10]}"
    expect("two commits at one revision: a mismatch at the second fails it",
           render(scratch, repo, listing((2, [ids["r2"], ids["x2"]])), m7,
                  {"FAKE_MODE": "mismatch", "FAKE_RUN": "b48", "FAKE_TAG": tag_x2}),
           f"r2 at `{ids['x2'][:7]}` b48 (48-frame blocks): exit 2")
    for mode, run, want in [
            ("mismatch", "hostile", "hostile (a hostile caller FPSCR): exit 2"),
            ("mismatch", "flush", "flush (flushing forced on inside the guard (FZ)): exit 2"),
            ("missing", "b48", "rendered 1 of the golden file's 2 presets"),
            ("extra", "b512", "rendered 2 of the golden file's 2 presets and 1 it does not list"),
            ("noreport", "random1", "rendered 0 of the golden file's 2 presets"),
            ("silent", "b48-1-127-32", "did not report every preset matching"),
            ("slow", "b48", "did not finish in 0.1 min"),
            ("streamcrash", "stream", "stream (the parity image's stream, maxBlockSize 48, its placement "
                                      "(parity_check.py)): exit 3"),
            ("streamfail", "stream", "): exit 1"),
            ("streamsilent", "stream", "parity_check.py did not pass the whole corpus"),
            ("streamsubset", "stream", "parity_check.py did not pass the whole corpus"),
            ("streamshort", "stream", "parity_check.py passed 1 presets of the golden file's 2")]:
        expect(f"a render that fails ({mode}, {run}) fails it",
               render(scratch, repo, one, m7, {"FAKE_MODE": mode, "FAKE_RUN": run}), want)
    for name, revisions, want in [
            ("a revision with no commit to render", [(2, [])], "no commit to render"),
            ("a commit the repository does not have", [(2, ["0" * 40])], "cannot check out"),
            ("a commit at another revision", [(2, [ids["r3"]])], "not at revision 2: its header reads 3"),
            ("a commit whose golden file is the revision before's", [(2, [ids["b2"]])],
             "its golden file is keyed to 1"),
            ("a commit whose golden file lists no preset", [(2, [ids["e2"]])], "lists no preset")]:
        expect(f"{name} fails it", render(scratch, repo, listing(*revisions), m7), want)
    expect("a listing without revisions passes with nothing to render",
           render(scratch, repo, listing(), "windows-x64-msvc-avx2"), 0)
    expect("a listing for another head fails it", render(scratch, repo, one, m7, head=ids["r3"]), "the listing is for")
    elsewhere = next(leg for leg in rr.HOST_LEGS if rr.platform_problem(leg))
    expect(f"a host leg on another platform than parity's ({elsewhere}) fails it",
           render(scratch, repo, one, elsewhere), "parity runs the leg there")
    here = next((leg for leg in rr.HOST_LEGS if rr.platform_problem(leg) is None), None)
    if here:
        expect(f"this platform's leg ({here}) renders its whole plan",
               render(scratch, repo, one, here), 0, here, len(rr.plan(here)))
    else:
        print(f"skip this platform's leg: parity has no leg on {rr.platform.system()} {rr.platform.machine()}")

    # The real build. The synthetic commits' toolchain file is a comment, so CMake configures the
    # M7 leg with the host's compiler (or fails without one): either way it is not the leg's.
    rr.build = real_build
    expect("a commit whose CMakeLists.txt does not parse does not configure (the real build), and fails it",
           render(scratch, repo, listing((2, [ids["c2"]])), m7), "harness does not configure or build")
    code, text = render(scratch, repo, one, m7)
    without = "harness does not configure or build" in text
    expect("a build with another compiler than the leg's (the real build) fails it"
           + (" (here: no host compiler, so it does not configure)" if without else ""),
           (code, text), "harness does not configure or build" if without else "the m7-qemu harness is not m7-qemu's")

    wrong = []
    for leg, info, want in [
            ("linux-x64-clang", ("Clang", "18.1.3", "/usr/bin/clang++", ""), None),
            ("linux-x64-clang", ("GNU", "13.3.0", "/usr/bin/g++", ""), "not Clang"),
            ("windows-x64-msvc-avx2", ("MSVC", "19.44", "cl.exe", "/arch:AVX2 /DWIN32 /D_WINDOWS /EHsc"), None),
            ("windows-x64-msvc-avx2", ("MSVC", "19.44", "cl.exe", "/DWIN32 /D_WINDOWS /EHsc"), "without /arch:AVX2"),
            ("macos-arm64-appleclang", ("Clang", "18", "/usr/bin/clang++", ""), "not AppleClang"),
            (m7, ("GNU", "10.3.1", "/opt/arm/bin/arm-none-eabi-g++", "-mcpu=cortex-m7"), None),
            (m7, ("GNU", "11.4.0", "/usr/bin/g++", ""), "not arm-none-eabi-g++")]:
        got = rr.compiler_problem(leg, info)
        if (got is None) != (want is None) or (want and want not in got):
            wrong.append(f"{leg} {info}: {got!r}, not {want!r}")
    bdir = os.path.join(scratch, "cmake-build")
    os.makedirs(os.path.join(bdir, "CMakeFiles", "3.31.6"))
    with open(os.path.join(bdir, "CMakeFiles", "3.31.6", "CMakeCXXCompiler.cmake"), "w") as fh:
        fh.write('set(CMAKE_CXX_COMPILER "C:/VS/cl.exe")\nset(CMAKE_CXX_COMPILER_ARG1 "")\n'
                 'set(CMAKE_CXX_COMPILER_ID "MSVC")\nset(CMAKE_CXX_COMPILER_VERSION "19.40.33811.0")\n')
    with open(os.path.join(bdir, "CMakeCache.txt"), "w") as fh:
        fh.write("//Flags used by the CXX compiler during all build types.\n"
                 "CMAKE_CXX_FLAGS:STRING=/arch:AVX2 /DWIN32 /D_WINDOWS /EHsc\n")
    if rr.compiler_of(bdir) != ("MSVC", "19.40.33811.0", "C:/VS/cl.exe", "/arch:AVX2 /DWIN32 /D_WINDOWS /EHsc"):
        wrong.append(f"compiler_of read {rr.compiler_of(bdir)}")
    case("compiler_problem refuses another compiler and missing leg flags; compiler_of reads CMake's files", wrong)


def synthetic(repo, scratch):
    """The synthetic pull request: r2 and r3 on the base fork, CI's merge commit, and commits at
    revision 2 a listing may name: x2 (another state at r2), b2 (bumped, golden file still r1's),
    e2 (a golden file without presets) and c2 (a CMakeLists.txt CMake cannot parse)."""
    t.git(repo, "init", "-q")
    base = dict(t.BASE, **{"tools/hil/parity_check.py": PARITY_CHECK})
    ids = {"fork": t.commit(repo, base, [], "fork")}
    r2 = t.changed(base, t.sound_revision(2))
    ids["r2"] = t.commit(repo, r2, [ids["fork"]], "r2")
    ids["x2"] = t.commit(repo, t.changed(r2, {"docs/STATUS.md": "x2\n"}), [ids["r2"]], "x2")
    r3 = t.changed(r2, t.sound_revision(3))
    ids["r3"] = t.commit(repo, r3, [ids["r2"]], "r3")
    ids["head"] = t.commit(repo, r3, [ids["fork"], ids["r3"]], "Merge r3")
    ids["b2"] = t.commit(repo, t.changed(base, t.bumped(2)), [ids["fork"]], "b2")
    ids["e2"] = t.commit(repo, t.changed(base, {**t.bumped(2), t.GOLDEN: t.golden(2, [])}), [ids["fork"]], "e2")
    ids["c2"] = t.commit(repo, t.changed(r2, {"CMakeLists.txt": "project(\n"}), [ids["r2"]], "c2")
    return ids


def main():
    parity_cases()
    with tempfile.TemporaryDirectory() as repo, tempfile.TemporaryDirectory() as scratch:
        ids = synthetic(repo, scratch)
        pr_listing = listing_cases(scratch, repo, ids)
        render_cases(scratch, repo, ids, pr_listing)
    print(f"{sum(RESULTS)}/{len(RESULTS)} cases gave the expected result")
    return 0 if all(RESULTS) else 1


if __name__ == "__main__":
    sys.exit(main())
