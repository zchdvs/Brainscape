#!/usr/bin/env python3
"""Self-test of render_revisions.py and parity_plan.py (.github/workflows/sound-rev-render.yml,
determinism profile §5.12).

Each case prints ok or FAIL, and any FAIL exits 1:

  yaml        parity_plan.load_yaml() reads every workflow in .github/workflows as PyYAML does
              (when PyYAML is installed; scalars compared as text), reads the subset's shapes,
              and refuses what it does not know (anchors, tags, flow mappings, multi-line plain
              or quoted scalars, tabs, a key twice, a more-indented folded line, keep chomping).
  parity      parity_plan.read() reads this checkout's parity.yml into the legs, builds and
              renders recorded below (a change to parity.yml's renders shows here); it follows a
              render added in another word order or with ${BIN}, a new -D option, a new loop
              value and a setup step, and refuses each change it cannot follow: a render
              condition other than one leg (||, runner.os), a harness found through a variable
              or named outside the render steps, qemu-arm on a variable, BIN reassigned or twice
              on a line, an unknown line or option in a render step, a stream parity_check.py
              does not check, a render after a non-render step, continue-on-error: true, a
              --mode other than "$MODE", new job, workflow or matrix keys and env, checkout
              options, and another configure command. sound-rev-render.yml: the render job is
              the list job's matrix and repeats parity-host's and parity-m7's setup steps (but
              for name and if; the M7's only on M7 jobs, a mutated parity.yml's Xcode step shows
              as missing), and the required job sound-rev-render has no needs, may read the
              run's jobs and waits for them; matrix_entries() writes one job per host leg and
              one M7 job per listed state.
  listings    read_listing() takes the gate's listings and refuses every malformed one (exit 1);
              `list` fails without writing a count when the gate cannot list, and writes the
              count, the listing and the render matrix for a synthetic pull request; `check`
              counts a listing and refuses one for another head, a leg the head does not run and
              a state the listing does not name.
  wait        judge() over this run's jobs: waits for the list job and every needed render job,
              passes with nothing to render or every render passed, fails on a list or render
              job that did not pass, on render jobs skipped although the listing has revisions
              and on a job the listing does not need; counts a render job at the latest attempt
              that ran it but not below the list job's. `wait` itself: passes once the jobs
              pass, fails on a failed job, when a needed job never appears, when the jobs cannot
              be read, and without the API's environment.
  render      the fail-closed paths, on a synthetic pull request built with
              test_sound_rev_gate.py's helpers (its commits carry this checkout's parity.yml),
              with a stand-in harness (a Python script) in place of the CMake build and a
              stand-in parity_check.py: a render that mismatches, covers fewer or other presets
              than the golden file, writes no report, does not print every preset matching or
              times out; a stream that parity_check.py fails, or passes for fewer presets or not
              as the whole corpus; a revision with no commit, a commit that does not exist, one
              at another revision, one whose golden file is keyed to the revision before, one
              whose golden file lists no preset and one whose parity.yml cannot be read; a build
              that does not configure (the real CMake call); a compiler other than the leg's or
              without its flags; a leg the head does not run, or run on another platform than
              parity's; a listing for another head; and --only naming a state the listing does
              not. Every one must exit 1 and name its failure; the passing runs must pass, render
              every run of the plan (a commit's own parity.yml's, or the head's for a leg its own
              does not run), render only the --only state, and leave no worktree behind. And
              `list` and `render` print a commit subject the console's code page cannot encode
              (a Windows runner's cp1252) without failing.

  test_render_revisions.py
"""
import contextlib
import io
import json
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import parity_plan as pp  # noqa: E402
import render_revisions as rr  # noqa: E402
import test_sound_rev_gate as t  # noqa: E402  (the synthetic repository's helpers)

ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
WORKFLOWS = os.path.join(ROOT, ".github", "workflows")
PARITY = os.path.join(WORKFLOWS, "parity.yml")
WORKFLOW = os.path.join(WORKFLOWS, "sound-rev-render.yml")
RESULTS = []


def case(name, wrong):
    """Records a case: `wrong` is the list of what is wrong (empty to pass)."""
    RESULTS.append(not wrong)
    print(f"{'ok  ' if not wrong else 'FAIL'} {name}")
    for w in wrong:
        print(f"       {w}")


def read(path):
    with open(path, encoding="utf-8") as fh:
        return fh.read().replace("\r\n", "\n")


def refused(fn, *args):
    """The PlanError's message, or None when fn returns."""
    try:
        fn(*args)
    except pp.PlanError as err:
        return str(err)
    return None


# ---------------------------------------------------------------------------------------------
# yaml.

def as_text(x):
    """PyYAML's document with every scalar as text (and its YAML 1.1 `on: true` key as on)."""
    if isinstance(x, dict):
        return {("on" if k is True else str(k)): as_text(v) for k, v in x.items()}
    if isinstance(x, list):
        return [as_text(v) for v in x]
    return None if x is None else {True: "true", False: "false"}.get(x, x) if isinstance(x, bool) else str(x)


YAML_READ = [
    ("a mapping, a sequence of mappings and nested blocks",
     "a: 1\nb:\n  - x: y\n    z:\n      - p\n      - 'q r'\n  - plain\nc:\n- d\n",
     {"a": "1", "b": [{"x": "y", "z": ["p", "q r"]}, "plain"], "c": ["d"]}),
    ("literal and folded block scalars, comments and blank lines",
     "# head\nr: |\n  one\n\n  # not a comment\n  two\nf: >\n  a\n  b\n\n  c\ne: x  # a comment\n",
     {"r": "one\n\n# not a comment\ntwo\n", "f": "a b\nc\n", "e": "x"}),
    ("quoted scalars and a flow sequence",
     "s: 'it''s ${{ x }}'\nd: \"a\\tb\"\nl: [opened, synchronize]\nn:\n",
     {"s": "it's ${{ x }}", "d": "a\tb", "l": ["opened", "synchronize"], "n": None}),
]
YAML_REFUSED = [
    ("an anchor", "a: &x 1\nb: *x\n"),
    ("a tag", "a: !!str 1\n"),
    ("a flow mapping", "a: {b: 1}\n"),
    ("a plain scalar over two lines", "a: one\n  two\n"),
    ("a single-quoted scalar over two lines", "a: 'one\n  two'\n"),
    ("a tab in the indentation", "a:\n\tb: 1\n"),
    ("a key twice", "a: 1\na: 2\n"),
    ("a more-indented line in a folded scalar", "a: >\n  x\n    y\n"),
    ("keep chomping", "a: |+\n  x\n"),
    ("a line that is not a key", "a: 1\njust text\n"),
]


def yaml_cases():
    try:
        import yaml
    except ImportError:
        yaml = None
    wrong = []
    for name in sorted(os.listdir(WORKFLOWS)):
        text = read(os.path.join(WORKFLOWS, name))
        try:
            mine = pp.load_yaml(text)
        except pp.PlanError as err:
            wrong.append(f"{name}: {err}")
            continue
        if yaml is not None and mine != as_text(yaml.safe_load(text)):
            wrong.append(f"{name}: read otherwise than PyYAML reads it")
    case("load_yaml reads every workflow" + (" as PyYAML does" if yaml else " (PyYAML not installed: not compared)"),
         wrong)
    for name, text, want in YAML_READ:
        got = refused(pp.load_yaml, text) or pp.load_yaml(text)
        case(f"load_yaml reads {name}", [] if got == want else [f"{got!r}, not {want!r}"])
    for name, text in YAML_REFUSED:
        why = refused(pp.load_yaml, text)
        case(f"load_yaml refuses {name}", [] if why and "YAML line" in why else [f"read as {pp.load_yaml(text)!r}"])


# ---------------------------------------------------------------------------------------------
# parity: this checkout's parity.yml, and the changes the reader follows or refuses.

HOST_LEGS = [("linux-x64-gcc", "ubuntu-24.04", "g++", "", "GNU"),
             ("linux-x64-clang", "ubuntu-24.04", "clang++", "", "Clang"),
             ("linux-arm64-gcc", "ubuntu-24.04-arm", "g++", "", "GNU"),
             ("windows-x64-msvc", "windows-2022", "", "", "MSVC"),
             ("windows-x64-msvc-avx2", "windows-2022", "", "/arch:AVX2", "MSVC"),
             ("macos-arm64-appleclang", "macos-14", "", "", "AppleClang"),
             ("macos-arm64-appleclang-latest", "macos-latest", "", "", "AppleClang")]
NA = ("--no-ablate",)
EVERY_HOST = [("golden", (), True), ("golden", NA + ("--block", "512"), False),
              ("golden", NA + ("--fp-env", "hostile"), False)]
GCC_ALSO = ([("golden", NA + ("--pattern", p), False) for p in "1 7 32 37 64 127 48,1,127,32 300,512,5,64".split()]
            + [("golden", NA + ("--random-blocks", s), False) for s in "12"]
            + [("golden", NA + ("--delivery", "split", "--pattern", p), False) for p in ("48", "512", "300,512,5,64")]
            + [("golden", NA + ("--delivery", "split", "--random-blocks", "3"), False),
               ("golden", NA + ("--fresh-engine",), False)])
M7 = [("golden", NA, True), ("golden", NA + ("--fp-env", "hostile"), False),
      ("golden", NA + ("--pattern", "512"), False),
      ("golden", NA + ("--pattern", "48,1,127,32"), False), ("golden", NA + ("--random-blocks", "1"), False),
      ("stream", ("--max-block", "48", "--placement"), False), ("flush", ("--force-flush-control",) + NA, False)]
HOST_BUILD = (("-DCMAKE_BUILD_TYPE=Release", "-DBRAINSCAPE_BUILD_TESTS=ON"), ("brainscape_golden",))
M7_BUILD = (("-DCMAKE_TOOLCHAIN_FILE=tools/cmake/arm-none-eabi-toolchain.cmake", "-DCMAKE_BUILD_TYPE=Release",
             "-DBRAINSCAPE_BUILD_TESTS=OFF", "-DBRAINSCAPE_BUILD_M7_ORACLE=ON", "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"),
            ("brainscape_golden", "brainscape_golden_flush", "brainscape_parity_stream"))

# Text in this checkout's parity.yml the mutations below change (the first occurrence: parity-host's).
H_CHECKOUT = "      - uses: actions/checkout@v4\n      - name: Configure\n"
H_HOSTILE = '''      - name: Render under a hostile caller FP environment
        if: ${{ !cancelled() && steps.build.outcome == 'success' }}
        continue-on-error: ${{ env.PARITY_HASHES_GATING != 'true' }}
        run: '"$BIN" --mode "$MODE" --no-ablate --fp-env hostile --tag "$LEG" --report "parity-report/$LEG-hostile.json"'
'''
H_GCC_IF = "steps.build.outcome == 'success' && matrix.leg == 'linux-x64-gcc' }}"
H_ENV = "      CXXFLAGS: ${{ matrix.cxxflags }}\n"
H_DEFINES = "cmake -B build -DCMAKE_BUILD_TYPE=Release -DBRAINSCAPE_BUILD_TESTS=ON\n"
H_FRESH = '''          "$BIN" --mode "$MODE" --no-ablate --fresh-engine --tag "$LEG" \\
            --report "parity-report/$LEG-fresh.json" || { echo "fresh engine: exit $?"; rc=1; }
'''
H_AUDIT = "      # Static audits that need this leg's toolchain (profile §6.3).\n"
W_ENV = '  PARITY_AUDITS_GATING: "true"\n'
MSVC_ENTRY = "          - leg: windows-x64-msvc\n            os: windows-2022\n"
M7_HOSTILE = '''          qemu-arm -cpu cortex-m7 build-m7/dsp/tests/golden/brainscape_golden.elf --mode "$MODE" \\
            --no-ablate --fp-env hostile --tag m7-qemu --report parity-report/m7-qemu-hostile.json
'''
M7_LOOP = "for b in 512 48,1,127,32; do"
M7_STREAM_CHECK = "          python3 tools/hil/parity_check.py --log parity-report/m7-qemu-stream.jsonl\n"
M7_RUNS_ON = ("    runs-on: ubuntu-24.04\n    steps:\n      - uses: actions/checkout@v4\n"
              "      - name: Cache the pinned arm")
M7_AUDIT = "      - name: Audit fused instructions (Armv7-M)\n"
NEW_RENDER = ('      - name: Render with a ring\n'
              '        if: ${{ !cancelled() && steps.build.outcome == \'success\' }}\n'
              '        run: \'"${BIN}" --no-ablate --ring 1048576 --mode "$MODE" --tag "$LEG" '
              '--report "parity-report/$LEG-ring.json"\'\n')
XCODE = ("      - name: Select Xcode 15.4\n        if: runner.os == 'macOS'\n"
         "        run: sudo xcode-select -s /Applications/Xcode_15.4.app\n")


def sub(old, new):
    """A mutation replacing the first `old` in parity.yml's text."""
    def mutate(text):
        if old not in text:
            raise KeyError(f"parity.yml no longer has {old[:60]!r}: update this mutation")
        return text.replace(old, new, 1)
    return mutate


def leg_plan(plan, leg):
    return [(r.harness, r.args, r.wav) for r in pp.leg_renders(plan, leg)]


# (what changes, the mutation, and the PlanError text it must raise, or a check of the plan
# that returns what is wrong).
FOLLOWED = [
    ("a render added on every host leg, in another word order and with ${BIN}", sub(H_HOSTILE, H_HOSTILE + NEW_RENDER),
     lambda p: [leg for leg in p.legs if ("golden", NA + ("--ring", "1048576"), False) not in leg_plan(p, leg)]),
    ("a new -D option", sub(H_DEFINES, H_DEFINES[:-1] + " -DBRAINSCAPE_X=1\n"),
     lambda p: [] if "-DBRAINSCAPE_X=1" in pp.leg_build(p, "linux-x64-clang").defines else ["not in the build"]),
    ("a new value in an M7 loop", sub(M7_LOOP, "for b in 512 48,1,127,32 256; do"),
     lambda p: [] if ("golden", NA + ("--pattern", "256"), False) in leg_plan(p, pp.M7_LEG) else ["not rendered"]),
    ("a setup step before Configure (the workflow must repeat it)", sub(H_CHECKOUT, H_CHECKOUT.replace(
        "      - name: Configure\n", XCODE + "      - name: Configure\n")),
     lambda p: [] if [s.get("name") for s in p.host_setup] == ["Select Xcode 15.4"] else [f"setup {p.host_setup}"]),
]
REFUSED = [
    ("a render condition on two legs (||)", sub(H_GCC_IF, "steps.build.outcome == 'success' && "
     "(matrix.leg == 'linux-x64-gcc' || matrix.leg == 'linux-arm64-gcc') }}"), "a condition the render does not know"),
    ("a render condition on runner.os", sub(H_GCC_IF, "steps.build.outcome == 'success' && runner.os == 'Linux' }}"),
     "a condition the render does not know"),
    ("an M7 harness ELF through a variable",
     sub(M7_HOSTILE, "          ELF=build-m7/dsp/tests/golden/brainscape_golden.elf\n"
         + M7_HOSTILE.replace("build-m7/dsp/tests/golden/brainscape_golden.elf", '"$ELF"')),
     "other than as qemu-arm"),
    ("qemu-arm on a variable after the renders", sub(M7_AUDIT, '      - name: Render more\n        run: qemu-arm -cpu '
     'cortex-m7 "$GOLDEN" --mode "$MODE" --no-ablate --block 256 --tag m7-qemu --report x.json\n' + M7_AUDIT),
     "runs qemu-arm on something other than a literal ELF path"),
    ("a harness named in a setup step", sub(M7_RUNS_ON, M7_RUNS_ON.replace(
        "      - name: Cache the pinned arm",
        '      - run: echo "GOLDEN=build-m7/dsp/tests/golden/brainscape_golden.elf" >> "$GITHUB_ENV"\n'
        '      - name: Cache the pinned arm')), "names a harness outside the render steps"),
    ("a render after a step that does not render", sub(H_AUDIT, H_AUDIT + NEW_RENDER),
     "names a harness outside the render steps"),
    ("BIN reassigned in a render step", sub(H_FRESH, "          BIN=build/other\n" + H_FRESH),
     "uses BIN other than as one harness call"),
    ("BIN twice on one line", sub(H_FRESH, H_FRESH.replace('"$BIN" --mode', '"$BIN" "$BIN" --mode')),
     "uses BIN other than as one harness call"),
    ("an unknown line in a render step", sub(H_FRESH, "          export CXXFLAGS=-O0\n" + H_FRESH),
     "a line the render does not know"),
    ("a render argument that is not a loop variable", sub(H_FRESH, H_FRESH.replace("--fresh-engine", '--only "$ONLY"')),
     "is not a loop variable"),
    ("another --mode", sub(H_FRESH, H_FRESH.replace('--mode "$MODE"', "--mode report")), "--mode report"),
    ("a stream that parity_check.py does not check", sub(M7_STREAM_CHECK, ""), "parity_check.py does not check"),
    ("a render that may fail without failing the job", sub(H_HOSTILE, H_HOSTILE.replace(
        "continue-on-error: ${{ env.PARITY_HASHES_GATING != 'true' }}", "continue-on-error: true")),
     "continue-on-error"),
    ("a new parity-host env variable", sub(H_ENV, H_ENV + "      CMAKE_GENERATOR: Ninja\n"), "parity-host: env"),
    ("a new workflow env variable", sub(W_ENV, W_ENV + '  CXXFLAGS: "-O0"\n'), "workflow env"),
    ("a new matrix key", sub(MSVC_ENTRY, MSVC_ENTRY + "            generator: Ninja\n"), "a matrix entry"),
    ("a checkout option", sub(H_CHECKOUT, H_CHECKOUT.replace(
        "checkout@v4\n", "checkout@v4\n        with:\n          submodules: true\n")),
     "actions/checkout@v4 without options"),
    ("a container for parity-host", sub(H_ENV, H_ENV + "    container: ubuntu:22.04\n"), "keys ['container']"),
    ("an env for parity-m7",
     sub(M7_RUNS_ON, M7_RUNS_ON.replace("    steps:\n", "    env:\n      QEMU_CPU: max\n    steps:\n")),
     "parity-m7: keys ['env']"),
    ("another configure command", sub(H_DEFINES, "cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release\n"),
     "no configure step the render knows"),
    ("an anchor", sub(W_ENV, '  PARITY_AUDITS_GATING: &gate "true"\n'), "YAML line"),
]


def workflow_problems(plan, wf):
    """What sound-rev-render.yml's render job does not repeat of parity's setup steps."""
    steps = wf["jobs"]["render"]["steps"]

    def bare(s):
        return {k: v for k, v in s.items() if k not in ("name", "if")}
    ours = [bare(s) for s in steps]
    wrong, at = [], -1
    for job, setup in (("parity-host", plan.host_setup), ("parity-m7", plan.m7_setup)):
        for s in setup:
            if bare(s) not in ours:
                wrong.append(f"{job}'s setup step {s.get('name')!r} is not repeated in the render job")
                continue
            i = ours.index(bare(s))
            if job == "parity-m7" and "matrix.leg == 'm7-qemu'" not in (steps[i].get("if") or ""):
                wrong.append(f"{s.get('name')!r} runs on more than the M7 jobs")
            if i < at:
                wrong.append(f"{s.get('name')!r} is out of parity's order")
            at = i
    return wrong


def parity_cases():
    text = read(PARITY)
    plan = pp.read(text)
    legs = [(x.name, x.os, x.cxx, x.cxxflags, x.compiler) for x in plan.legs.values()]
    case("parity_plan reads parity-host's legs, runners and compilers",
         [] if legs == HOST_LEGS else [f"{legs}"])
    wrong = []
    for leg in plan.legs:
        want = [(h, a, w) for h, a, w in EVERY_HOST + (GCC_ALSO if leg == "linux-x64-gcc" else [])]
        if leg_plan(plan, leg) != want:
            wrong.append(f"{leg}: {leg_plan(plan, leg)}")
    if leg_plan(plan, pp.M7_LEG) != M7:
        wrong.append(f"m7-qemu: {leg_plan(plan, pp.M7_LEG)}")
    if plan.host_build != HOST_BUILD or plan.m7_build != M7_BUILD or plan.m7_os != "ubuntu-24.04":
        wrong.append(f"builds {plan.host_build} {plan.m7_build} on {plan.m7_os}")
    case("parity_plan reads every render and build parity-host and parity-m7 make, leg by leg", wrong)

    for name, mutate, check in FOLLOWED:
        try:
            got = pp.read(mutate(text))
            wrong = check(got)
        except (pp.PlanError, KeyError) as err:
            wrong = [str(err)]
        case(f"parity_plan follows {name}", wrong)
    for name, mutate, want in REFUSED:
        try:
            why = refused(pp.read, mutate(text))
        except KeyError as err:
            why = str(err)
        case(f"parity_plan refuses {name}", [] if why and want in why else [f"{why!r}, not {want!r}"])

    wf = pp.load_yaml(read(WORKFLOW))
    jobs = wf["jobs"]
    render, required = jobs["render"], jobs.get("sound-rev-render") or {}
    case("the render job repeats parity-host's and parity-m7's setup steps (but name and if), the M7's on M7 jobs",
         workflow_problems(plan, wf))
    xcode = pp.read(FOLLOWED[3][1](text))
    case("a setup step parity adds is missing from the render job until it repeats it",
         [] if workflow_problems(xcode, wf) == ["parity-host's setup step 'Select Xcode 15.4' is not repeated in the "
                                                "render job"] else [f"{workflow_problems(xcode, wf)}"])
    checks = [
        ("the list job", jobs["list"].get("name") == "sound-rev-render (list)"),
        ("render: its name", render.get("name") == "sound-rev-render (${{ matrix.name }})"),
        ("render: the list job's matrix",
         render.get("strategy", {}).get("matrix") == "${{ fromJSON(needs.list.outputs.matrix) }}"),
        ("render: skipped at 0",
         render.get("needs") == "list" and render.get("if") == "needs.list.outputs.count != '0'"),
        ("render: the matrix's runner", render.get("runs-on") == "${{ matrix.os }}"),
        ("checkouts with the whole history",
         all(j["steps"][0] == {"uses": "actions/checkout@v4", "with": {"fetch-depth": "0"}} for j in jobs.values())),
        ("sound-rev-render: its name", required.get("name") == "sound-rev-render"),
        ("sound-rev-render: no needs, so its check exists from the start",
         "needs" not in required and "if" not in required),
        ("sound-rev-render: reads the run's jobs",
         required.get("permissions") == {"contents": "read", "actions": "read"}),
        ("sound-rev-render: lists and waits", [s.get("run", "").split()[:3] for s in required.get("steps", [])[1:]] ==
         [["python3", "tools/ci/render_revisions.py", "list"], ["python3", "tools/ci/render_revisions.py", "wait"]]),
        ("triggers", wf["on"] == {"pull_request": {"types": ["opened", "synchronize", "reopened", "edited"]}}),
        ("concurrency", wf.get("concurrency", {}).get("cancel-in-progress") == "true"),
        ("no other job", set(jobs) == {"list", "render", "sound-rev-render"}),
    ]
    case("sound-rev-render.yml: the list job's matrix, and the required job without needs that waits for it",
         [name for name, ok in checks if not ok])

    a, b = "a" * 40, "b" * 40
    doc = {"revisions": [{"revision": 4, "introduced": [a], "render": [a]}, {"revision": 5, "introduced": [b],
                                                                              "render": [a, b]},
                         {"revision": 6, "introduced": [b], "render": []}]}
    entries = rr.matrix_entries(doc, plan)
    want = ([{"name": x[0], "leg": x[0], "os": x[1], "only": "", "artifact": x[0]} for x in HOST_LEGS]
            + [{"name": f"m7-qemu, {tag}", "leg": "m7-qemu", "os": "ubuntu-24.04", "only": only,
                "artifact": f"m7-qemu-{tag}"}
               for tag, only in ((f"r4-{a[:10]}", f"4:{a}"), (f"r5-{a[:10]}", f"5:{a}"), (f"r5-{b[:10]}", f"5:{b}"),
                                 ("r6", "6:"))])
    case("matrix_entries: one job per host leg and one M7 job per listed state; none without revisions",
         [] if entries == want and rr.matrix_entries({"revisions": []}, plan) == [] else [f"{entries}"])


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
    """(exit code, message, output) of a function that may sys.exit; output swallowed."""
    with contextlib.redirect_stdout(io.StringIO()) as out:
        try:
            code, msg = fn(*args), ""
        except SystemExit as e:
            code, msg = (1, str(e.code)) if isinstance(e.code, str) else (e.code, "")
    return code, msg, out.getvalue()


def script(repo, *args, env=None):
    return subprocess.run([sys.executable, os.path.join(HERE, "render_revisions.py"), *args], cwd=repo, env=env,
                          capture_output=True, text=True, encoding="utf-8", errors="replace")


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
    out, listing = os.path.join(scratch, "github-output.txt"), os.path.join(scratch, "pr.json")
    open(out, "w").close()
    p = script(repo, "list", "--base", "0" * 40, "--head", ids["head"], "--listing", listing, "--github-output", out)
    case("list fails, and writes no count, when the gate cannot list",
         [] if p.returncode == 1 and "wrote no revision listing" in p.stderr and read(out) == ""
         else [f"exit {p.returncode}, output {read(out)!r}: {p.stderr[-300:]}"])
    p = script(repo, "list", "--base", ids["fork"], "--head", ids["head"], "--listing", listing, "--github-output", out)
    got = dict(line.split("=", 1) for line in read(out).splitlines())
    want = [{"revision": 2, "introduced": [ids["r2"]], "render": [ids["r2"]]}]
    names = [e["name"] for e in json.loads(got.get("matrix", "{}")).get("include", [])]
    case("list writes the count, the listing and the render matrix, each on one line",
         [] if p.returncode == 0 and got.get("count") == "1" and json.loads(got.get("listing", "{}")).get(
             "revisions") == want and names == [x[0] for x in HOST_LEGS] + [f"m7-qemu, r2-{ids['r2'][:10]}"]
         else [f"exit {p.returncode}: {got} {p.stderr[-300:]}"])
    open(out, "w").close()
    p = script(repo, "check", "--listing", listing, "--head", ids["head"], "--leg", "m7-qemu", "--only",
               f"2:{ids['r2']}", "--github-output", out)
    case("check counts the listing for its head, leg and state",
         [] if p.returncode == 0 and read(out) == "count=1\n" else [f"exit {p.returncode}: {read(out)!r} {p.stderr}"])
    for name, args, want in [("a listing for another head", ["--head", ids["r3"]], "the listing is for"),
                             ("a leg the head's parity.yml does not run", ["--leg", "linux-riscv64-gcc"],
                              "is not a leg of the head's"),
                             ("a state the listing does not name", ["--only", f"2:{ids['x2']}"],
                              "the listing has no r2"),
                             ("a malformed state", ["--only", "2:abc"], "is not REV:COMMIT")]:
        p = script(repo, "check", "--listing", listing, *args)
        case(f"check refuses {name}",
             [] if p.returncode == 1 and want in p.stderr else [f"exit {p.returncode}: {p.stderr}"])
    return listing


# ---------------------------------------------------------------------------------------------
# wait: the required check over this run's jobs.

LIST = rr.LIST_JOB
W = ["sound-rev-render (linux-x64-gcc)", "sound-rev-render (m7-qemu, r4-0123456789)"]
SKIPPED = "sound-rev-render (${{ matrix.name }})"


def job(name, conclusion="success", status="completed", attempt=1, ident=1):
    return {"name": name, "status": status, "conclusion": conclusion if status == "completed" else None,
            "run_attempt": attempt, "id": ident}


JUDGE = [
    ("no job yet", [], W, 1, "wait"),
    ("the list job running", [job(LIST, status="in_progress")], W, 1, "wait"),
    ("the list job failed", [job(LIST, "failure")], W, 1, "fail"),
    ("the list job cancelled", [job(LIST, "cancelled")], W, 1, "fail"),
    ("nothing to render, the list job passed", [job(LIST)], [], 1, "pass"),
    ("nothing to render, the render jobs skipped", [job(LIST), job(SKIPPED, "skipped")], [], 1, "pass"),
    ("the render jobs not created yet", [job(LIST)], W, 1, "wait"),
    ("a render job queued", [job(LIST), job(W[0]), job(W[1], status="queued")], W, 1, "wait"),
    ("every render job passed", [job(LIST), job(W[0]), job(W[1])], W, 1, "pass"),
    ("a render job failed while another runs", [job(LIST), job(W[0], "failure"), job(W[1], status="in_progress")], W, 1,
     "fail"),
    ("a render job cancelled", [job(LIST), job(W[0]), job(W[1], "cancelled")], W, 1, "fail"),
    ("a render job timed out", [job(LIST), job(W[0]), job(W[1], "timed_out")], W, 1, "fail"),
    ("a render job skipped", [job(LIST), job(W[0], "skipped"), job(W[1])], W, 1, "fail"),
    ("the render jobs skipped although the listing has revisions", [job(LIST), job(SKIPPED, "skipped")], W, 1, "fail"),
    ("a render job the listing does not need", [job(LIST), job(W[0]), job(W[1]), job("sound-rev-render (extra)")], W, 1,
     "fail"),
    ("failed jobs re-run: the earlier attempt's pass counts, the re-run replaces the failure",
     [job(LIST), job(W[0]), job(W[1], "failure"), job(W[1], attempt=2, ident=9)], W, 2, "pass"),
    ("all jobs re-run: the earlier attempt's renders do not count",
     [job(LIST), job(W[0]), job(W[1]), job(LIST, attempt=2, ident=5)], W, 2, "wait"),
    ("an attempt after this one does not count", [job(LIST), job(W[0]), job(W[1]), job(W[1], "failure", attempt=3)],
     W, 1, "pass"),
]


def wait_cases(scratch, repo, listing):
    for name, jobs, want, attempt, verdict in JUDGE:
        got = rr.judge(jobs, want, attempt)
        case(f"judge: {name}: {verdict}", [] if got[0] == verdict else [f"{got[0]}: {got[1]}"])

    with open(listing, encoding="utf-8") as fh:
        doc = json.load(fh)
    plan = pp.read(read(PARITY))
    want = [f"sound-rev-render ({e['name']})" for e in rr.matrix_entries(doc, plan)]
    summary = os.path.join(scratch, "wait.md")
    real = rr.fetch_jobs

    def wait(reads, extra=(), env=None):
        """`wait` over a sequence of reads (a list of jobs, or an exception to raise)."""
        reads = list(reads)

        def fetch():
            r = reads.pop(0) if len(reads) > 1 else reads[0]
            if isinstance(r, Exception):
                raise r
            return r
        rr.fetch_jobs = fetch
        saved, cwd = dict(os.environ), os.getcwd()
        os.environ.update(env or {"GITHUB_RUN_ATTEMPT": "1"})
        os.chdir(repo)
        if os.path.exists(summary):
            os.remove(summary)
        try:
            code, msg, out = exits(rr.main, ["wait", "--listing", listing, "--summary", summary, "--poll", "0",
                                             "--retries", "2", *extra])
        finally:
            os.chdir(cwd)
            os.environ.clear()
            os.environ.update(saved)
            rr.fetch_jobs = real
        return code, (read(summary) if os.path.exists(summary) else "") + msg

    passed = [job(LIST)] + [job(n) for n in want]
    for name, reads, extra, code_want, text in [
            ("passes once the list job and every render job passed",
             [[], [job(LIST, status="in_progress")], [job(LIST)],
              [job(LIST)] + [job(n, status="in_progress") for n in want], passed], (), 0, "PASS"),
            ("fails on a render job that failed", [[job(LIST), job(want[0], "failure")]], (), 1,
             f"{want[0]} concluded failure"),
            ("fails when a needed render job never appears", [[job(LIST)]], ("--appear", "0"), 1, "had not appeared"),
            ("fails when the jobs cannot be read", [OSError("no network")], (), 1, "cannot read this run's jobs"),
            ("reads again after a failed read", [OSError("blip"), passed], (), 0, "PASS")]:
        code, out = wait(reads, extra)
        case(f"wait {name}", [] if code == code_want and text in out else [f"exit {code}: {out[-400:]}"])
    saved, cwd = dict(os.environ), os.getcwd()
    for k in ("GITHUB_REPOSITORY", "GITHUB_RUN_ID", "GH_TOKEN", "GITHUB_TOKEN"):
        os.environ.pop(k, None)
    os.chdir(repo)
    try:
        code, msg, out = exits(rr.main, ["wait", "--listing", listing, "--poll", "0"])
    finally:
        os.chdir(cwd)
        os.environ.clear()
        os.environ.update(saved)
    case("wait fails at once without the GitHub API's environment",
         [] if code == 1 and "is not set" in out else [f"exit {code}: {out[-300:]}"])


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
# render_revisions.main in a process of its own, with the stand-in build (for `render`).
WRAPPER = r'''
import sys
sys.path.insert(0, sys.argv[1])
import render_revisions as rr
fake = sys.argv[2]
def build(r, leg, args, timings):
    timings.append((r.tag, "build", 0.0))
    r.compiler = "stand-in"
    r.bins = {h: [sys.executable, fake, h] for h in {x.harness for x in r.runs}}
rr.build = build
sys.exit(rr.main(sys.argv[3:]))
'''
SUBJECT = "Delay: feedback ceiling 0.95 \u2192 0.9 (r2)"


def fake_build(fake):
    def build(r, leg, args, timings):
        timings.append((r.tag, "build", 0.0))
        r.compiler = "stand-in"
        r.bins = {h: [sys.executable, fake, h] for h in {x.harness for x in r.runs}}
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
    plan = pp.read(read(PARITY))

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

    def expect(name, code_msg, want, runs=None, present=(), absent=()):
        code, text = code_msg
        wrong = []
        if want == 0:
            if code != 0 or not text.rstrip().endswith("PASS"):
                wrong.append(f"exit {code}: {text[-600:]}")
            if runs is not None and text.count("| pass |") != runs:
                wrong.append(f"{text.count('| pass |')} passing runs, not {runs}")
        elif code != 1 or want not in text:
            wrong.append(f"exit {code}, without {want!r}: {text[-600:]}")
        wrong += [f"without {p!r}" for p in present if p not in text] + [f"with {a!r}" for a in absent if a in text]
        if len(worktrees()) != 1:
            wrong.append(f"worktrees left behind: {worktrees()}")
        case(name, wrong)

    m7 = rr.M7_LEG
    m7_runs = len(pp.leg_renders(plan, m7))
    one = listing((2, [ids["r2"]]))
    two = listing((2, [ids["r2"], ids["x2"]]))
    expect("the M7 plan passes at the listed commit", render(scratch, repo, pr_listing, m7, head=ids["head"]), 0,
           m7_runs, present=["render(s) from its own parity.yml"])
    expect("two commits at one revision: both rendered", render(scratch, repo, two, m7), 0, 2 * m7_runs)
    expect("--only: one M7 job renders only its state",
           render(scratch, repo, two, m7, extra=("--only", f"2:{ids['x2']}")), 0, m7_runs,
           present=[f"`{ids['x2'][:7]}`"], absent=[f"`{ids['r2'][:7]}`"])
    expect("--only naming a state the listing does not fails it",
           render(scratch, repo, one, m7, extra=("--only", f"2:{ids['x2']}")), "the listing has no r2")
    tag_x2 = f"r2-{ids['x2'][:10]}"
    expect("two commits at one revision: a mismatch at the second fails it",
           render(scratch, repo, two, m7, {"FAKE_MODE": "mismatch", "FAKE_RUN": "b48", "FAKE_TAG": tag_x2}),
           f"r2 at `{ids['x2'][:7]}` b48 (brainscape_golden --no-ablate): exit 2")
    for mode, run, want in [
            ("mismatch", "hostile", "hostile (brainscape_golden --no-ablate --fp-env hostile): exit 2"),
            ("mismatch", "flush", "flush (brainscape_golden_flush --force-flush-control --no-ablate): exit 2"),
            ("missing", "b48", "rendered 1 of the golden file's 2 presets"),
            ("extra", "b512", "rendered 2 of the golden file's 2 presets and 1 it does not list"),
            ("noreport", "random1", "rendered 0 of the golden file's 2 presets"),
            ("silent", "b48-1-127-32", "did not report every preset matching"),
            ("slow", "b48", "did not finish in 0.1 min"),
            ("streamcrash", "stream", "stream (brainscape_parity_stream --max-block 48 --placement (checked by "
                                      "parity_check.py)): exit 3"),
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
            ("a commit whose golden file lists no preset", [(2, [ids["e2"]])], "lists no preset"),
            ("a commit whose parity.yml the render cannot read", [(2, [ids["q2"]])],
             "is not one parity_plan.py can read (workflow env")]:
        expect(f"{name} fails it", render(scratch, repo, listing(*revisions), m7), want)
    # A commit's own parity.yml gives its renders: p2's renders blocks of 256 on the M7 too.
    code, text = render(scratch, repo, listing((2, [ids["r2"], ids["p2"]])), m7)
    expect("each commit is rendered with its own parity.yml's renders", (code, text), 0, 2 * m7_runs + 1,
           present=["brainscape_golden --no-ablate --pattern 256"])
    expect("a render its own parity.yml adds fails it too",
           render(scratch, repo, listing((2, [ids["p2"]])), m7,
                  {"FAKE_MODE": "mismatch", "FAKE_RUN": "b256"}),
           "b256 (brainscape_golden --no-ablate --pattern 256): exit 2")
    expect("a listing without revisions passes with nothing to render",
           render(scratch, repo, listing(), "windows-x64-msvc-avx2"), 0)
    expect("a listing for another head fails it", render(scratch, repo, one, m7, head=ids["r3"]), "the listing is for")
    expect("a leg the head's parity.yml does not run fails it", render(scratch, repo, one, "linux-riscv64-gcc"),
           "is not a leg of the head's")
    elsewhere = next(leg for leg in plan.legs if rr.platform_problem(leg))
    expect(f"a host leg on another platform than parity's ({elsewhere}) fails it",
           render(scratch, repo, one, elsewhere), "parity runs the leg there")
    here = [leg for leg in plan.legs if rr.platform_problem(leg) is None]
    if here:
        expect(f"this platform's leg ({here[0]}) renders its whole plan",
               render(scratch, repo, one, here[0]), 0, len(pp.leg_renders(plan, here[0])))
        # h2's parity.yml has no `spare` leg: the head's renders it there.
        spare = next((leg for leg in here if leg != "linux-x64-gcc"), None)
        if spare:
            code, text = render(scratch, repo, listing((2, [ids[f"h2-{spare}"]])), spare)
            expect(f"a commit whose parity.yml does not run the leg ({spare}) renders the head's plan there",
                   (code, text), 0, len(pp.leg_renders(plan, spare)), present=[f"since its own does not run {spare}"])
    else:
        print(f"skip this platform's leg: parity has no leg on {rr.platform.system()} {rr.platform.machine()}")

    # A subject a cp1252 console cannot print, through `list` and `render` in processes of their
    # own whose stdout is a pipe in that code page, as on a Windows runner.
    wrapper = os.path.join(scratch, "wrapper.py")
    with open(wrapper, "w", encoding="utf-8") as fh:
        fh.write(WRAPPER)
    env = {k: v for k, v in os.environ.items() if k not in ("PYTHONUTF8", "PYTHONIOENCODING")}
    env["PYTHONIOENCODING"] = "cp1252:strict"
    u_listing = os.path.join(scratch, "u.json")
    outs = []
    for args in (["list", "--base", ids["fork"], "--head", ids["uhead"], "--listing", u_listing],
                 ["render", "--leg", m7, "--listing", u_listing, "--work", tempfile.mkdtemp(prefix="u", dir=scratch),
                  "--out", os.path.join(scratch, "u-out"), "--emulator", "", "--timeout", "0.5"]):
        p = subprocess.run([sys.executable, wrapper, HERE, fake, *args], cwd=repo, env=env, capture_output=True)
        outs.append((p.returncode, p.stdout.decode("utf-8", "replace"), p.stderr.decode("utf-8", "replace")))
    case("list and render print a subject a cp1252 console cannot encode (UTF-8 out), and pass",
         [f"{w}: exit {rc}: {err[-400:] or out[-400:]}" for w, (rc, out, err) in zip(("list", "render"), outs)
          if rc != 0 or SUBJECT not in out or (w == "render" and not out.rstrip().endswith("PASS"))])

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
        got = rr.compiler_problem(leg, info, pp.leg_build(plan, leg))
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
    env = {leg: rr.build_env(pp.leg_build(plan, leg), leg) for leg in ("linux-x64-clang", "windows-x64-msvc-avx2", m7)}
    if (env["linux-x64-clang"].get("CXX"), env["linux-x64-clang"].get("CXXFLAGS")) != ("clang++", "") \
            or "CXX" in env["windows-x64-msvc-avx2"] or env["windows-x64-msvc-avx2"].get("CXXFLAGS") != "/arch:AVX2" \
            or "CXX" in env[m7] or "CXXFLAGS" in env[m7]:
        wrong.append("build_env does not set CXX and CXXFLAGS as parity-host does, or sets them on the M7")
    flags = rr.configure_flags(pp.leg_build(plan, m7), os.path.join("W", "T"))
    toolchain = os.path.join("W", "T", "tools", "cmake", "arm-none-eabi-toolchain.cmake")
    if flags[0] != "-DCMAKE_TOOLCHAIN_FILE=" + toolchain:
        wrong.append(f"configure_flags: {flags}")
    case("compiler_problem refuses another compiler and missing leg flags; compiler_of reads CMake's files; "
         "build_env and configure_flags", wrong)


def synthetic(repo, scratch):
    """The synthetic pull request: r2 and r3 on the base fork, CI's merge commit, and commits at
    revision 2 a listing may name: x2 (another state at r2), b2 (bumped, golden file still r1's),
    e2 (a golden file without presets), c2 (a CMakeLists.txt CMake cannot parse), p2 (its
    parity.yml renders 256-frame blocks on the M7 too), q2 (a parity.yml the reader refuses) and
    h2-<leg> (a parity.yml without that leg); and a second pull request whose r2 commit's subject
    has a character outside cp1252 (u2, uhead)."""
    t.git(repo, "init", "-q")
    parity = read(PARITY)
    base = dict(t.BASE, **{"tools/hil/parity_check.py": PARITY_CHECK, rr.PARITY_YML: parity})
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
    ids["p2"] = t.commit(repo, t.changed(r2, {rr.PARITY_YML: sub(M7_LOOP, "for b in 512 48,1,127,32 256; do")(parity)}),
                         [ids["r2"]], "p2")
    ids["q2"] = t.commit(repo, t.changed(r2, {rr.PARITY_YML: sub(W_ENV, W_ENV + '  EXTRA: "1"\n')(parity)}),
                         [ids["r2"]], "q2")
    for leg, spec in pp.read(parity).legs.items():
        entry = re.search(rf"^          - leg: {re.escape(leg)}\n(?:            .*\n)+", parity, re.M).group(0)
        without = parity.replace(entry, "", 1)
        if refused(pp.read, without) is None and not pp.has_leg(pp.read(without), leg):
            ids[f"h2-{leg}"] = t.commit(repo, t.changed(r2, {rr.PARITY_YML: without}), [ids["r2"]], f"h2 {leg}")
    ids["u2"] = t.commit(repo, t.changed(r2, {"docs/STATUS.md": "u2\n"}), [ids["fork"]], SUBJECT)
    ids["u3"] = t.commit(repo, t.changed(r3, {"docs/STATUS.md": "u3\n"}), [ids["u2"]], "r3")
    ids["uhead"] = t.commit(repo, t.changed(r3, {"docs/STATUS.md": "u3\n"}), [ids["fork"], ids["u3"]], "Merge u3")
    return ids


def main():
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass
    yaml_cases()
    parity_cases()
    with tempfile.TemporaryDirectory() as repo, tempfile.TemporaryDirectory() as scratch:
        ids = synthetic(repo, scratch)
        pr_listing = listing_cases(scratch, repo, ids)
        wait_cases(scratch, repo, pr_listing)
        render_cases(scratch, repo, ids, pr_listing)
    print(f"{sum(RESULTS)}/{len(RESULTS)} cases gave the expected result")
    return 0 if all(RESULTS) else 1


if __name__ == "__main__":
    sys.exit(main())
