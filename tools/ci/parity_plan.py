#!/usr/bin/env python3
"""Reads .github/workflows/parity.yml into what its parity-host and parity-m7 jobs build and
render on each leg (render_revisions.py and sound-rev-render.yml; determinism profile §5.12).

render_revisions.py renders each sound revision a pull request carries below its head's with
the renders that revision's own commit's parity.yml makes, so this reads any commit's copy. It
fails closed: read() raises PlanError on any shape it does not know, so a change to parity.yml
that the render cannot follow stops the render instead of leaving a render or a setting out.

  workflow    env: holds only PARITY_HASHES_GATING and PARITY_AUDITS_GATING.
  parity-host keys name, strategy (a matrix of include entries only, each with leg and os and
              at most cxx, cxxflags and symbols), runs-on: ${{ matrix.os }}, defaults (bash),
              env (exactly LEG, LEG_CXX, LEG_SYMBOLS and CXXFLAGS from the matrix), steps and
              timeout-minutes. Its steps: actions/checkout@v4 with no options; any setup steps
              (returned, for sound-rev-render.yml to repeat); Configure, Build the golden
              harness (id: build) and Select the harness mode, word for word but for the -D
              options and the targets; then the render steps, one after another; then steps
              that run no harness.
  parity-m7   keys name, runs-on (a fixed runner), steps and timeout-minutes. Its steps: the
              checkout; the setup steps (the pinned toolchain and qemu-arm caches and
              installers, returned); Configure (firmware flags), Build the golden harness ELFs
              (id: build) and Select the harness mode, word for word but for the -D options
              and the targets; the render steps; then steps that run no harness.
  render step keys name, if, continue-on-error and run. if: is absent or
              ${{ !cancelled() && steps.build.outcome == 'success' }}, on parity-host with at
              most one "&& matrix.leg == 'LEG'"; continue-on-error is absent or
              ${{ env.PARITY_HASHES_GATING != 'true' }}. Each line of its script is a harness
              call ("$BIN" on parity-host; qemu-arm -cpu cortex-m7 and one of the three
              harness ELFs on parity-m7), optionally followed by || { echo "..."; rc=1; }; a
              one-level for loop's header or done; rc=0 or exit $rc; mkdir -p; echo "...";
              an assignment of a command's output to a variable other than BIN, MODE or LEG;
              or, after a stream call, python3 tools/hil/parity_check.py --log <its --out>.
              A harness call names each of --mode "$MODE" (not the stream), --tag ("$LEG", or
              m7-qemu) and --report (--out for the stream) once, and may add --wav-dir and
              --note; every other word is the render's own argument, where a loop variable
              stands for each of its values and no other variable may appear.

A host leg's compiler follows from its matrix entry: CXX=g++ is GCC, clang++ Clang, and none
the platform's own (MSVC on Windows, AppleClang on macOS). Run as a script, it prints the plan
of a parity.yml (default: this checkout's).

  parity_plan.py [PARITY_YML]
"""
import collections
import json
import os
import re
import shlex
import sys


class PlanError(Exception):
    """A parity.yml whose shape this reader does not know."""


# ---------------------------------------------------------------------------------------------
# A YAML subset: block mappings and sequences, plain, single and double-quoted scalars, literal
# and folded block scalars, and flow sequences of plain scalars. Anything else (anchors, tags,
# flow mappings, multi-line plain or quoted scalars, tabs in indentation) raises PlanError.
# Scalars stay strings.

KEY = re.compile(r"([A-Za-z_][\w-]*)[ \t]*:(?:[ \t]+(.*))?$")


class _Yaml:
    def __init__(self, text):
        self.lines = text.replace("\r\n", "\n").split("\n")
        self.i = 0

    def error(self, why):
        raise PlanError(f"YAML line {self.i + 1}: {why}")

    def eof(self):
        return self.i >= len(self.lines)

    def skip(self):
        """Moves past blank and comment lines."""
        while not self.eof():
            text = self.lines[self.i].strip()
            if text and not text.startswith("#"):
                return
            self.i += 1

    def indent(self):
        line = self.lines[self.i]
        n = len(line) - len(line.lstrip(" "))
        if line[n:n + 1] == "\t":
            self.error("a tab in the indentation")
        return n

    def dash(self, ind):
        body = self.lines[self.i][ind:]
        return body == "-" or body.startswith("- ")

    def block(self, ind):
        return self.sequence(ind) if self.dash(ind) else self.mapping(ind)

    def sequence(self, ind):
        out = []
        while True:
            self.skip()
            if self.eof() or self.indent() < ind:
                return out
            if self.indent() > ind:
                self.error("unexpected indentation")
            if not self.dash(ind):
                return out
            rest = self.lines[self.i][ind + 1:]
            item = rest.lstrip(" ")
            if not item or item.startswith("#"):
                self.i += 1
                self.skip()
                out.append(None if self.eof() or self.indent() <= ind else self.block(self.indent()))
            elif KEY.match(item):
                # A mapping that starts on the dash's line: read it as if the dash were a space.
                sub = ind + 1 + len(rest) - len(item)
                self.lines[self.i] = " " * sub + item
                out.append(self.mapping(sub))
            else:
                self.i += 1
                out.append(self.scalar(item))

    def mapping(self, ind):
        out = {}
        while True:
            self.skip()
            if self.eof() or self.indent() < ind:
                return out
            if self.indent() > ind:
                self.error("unexpected indentation")
            if self.dash(ind):
                return out
            m = KEY.match(self.lines[self.i][ind:])
            if m is None:
                self.error("not a 'key: value' line")
            key, value = m.group(1), (m.group(2) or "").strip()
            if key in out:
                self.error(f"the key {key} twice")
            self.i += 1
            if not value or value.startswith("#"):
                self.skip()
                if self.eof():
                    out[key] = None
                elif self.indent() > ind:
                    out[key] = self.block(self.indent())
                elif self.indent() == ind and self.dash(ind):
                    out[key] = self.sequence(ind)
                else:
                    out[key] = None
            elif value[0] in "|>":
                out[key] = self.block_scalar(value, ind)
            else:
                out[key] = self.scalar(value)

    def block_scalar(self, header, ind):
        m = re.fullmatch(r"([|>])(-?)(?:[ \t]+#.*)?", header)
        if m is None:
            self.error(f"a block scalar header this reader does not know: {header!r}")
        lines, content = [], None
        while not self.eof():
            line = self.lines[self.i]
            if not line.strip():
                lines.append("")
                self.i += 1
                continue
            n = len(line) - len(line.lstrip(" "))
            if n <= ind:
                break
            if content is None:
                content = n
            if n < content:
                self.error("a block scalar line less indented than its first")
            lines.append(line[content:])
            self.i += 1
        while lines and not lines[-1]:
            lines.pop()
        if m.group(1) == "|":
            text = "\n".join(lines)
        else:
            if any(line[:1] in (" ", "\t") for line in lines):
                self.error("a more-indented line in a folded scalar")
            text, gap = "", False
            for line in lines:
                if not line:
                    text += "\n"
                    gap = True
                    continue
                text += line if not text or gap else " " + line
                gap = False
        return text + ("\n" if lines and not m.group(2) else "")

    def scalar(self, s):
        if s.startswith("'"):
            inner = s[1:-1] if len(s) >= 2 and s.endswith("'") else None
            if inner is None or "'" in inner.replace("''", ""):
                self.error("a single-quoted scalar this reader does not know (over several lines?)")
            return inner.replace("''", "'")
        if s.startswith('"'):
            try:
                value = json.loads(s)
            except ValueError:
                value = None
            if not isinstance(value, str):
                self.error("a double-quoted scalar this reader does not know")
            return value
        if s.startswith("["):
            if not s.endswith("]") or re.search(r"[\[\]{}'\"]", s[1:-1]):
                self.error("a flow sequence this reader does not know")
            return [x.strip() for x in s[1:-1].split(",") if x.strip()]
        if s[0] in "{&*!%@`?|>":
            self.error(f"YAML this reader does not know: {s[:20]!r}")
        return re.sub(r"[ \t]+#.*$", "", s).strip()


def load_yaml(text):
    """The document, as dicts, lists and strings."""
    y = _Yaml(text)
    y.skip()
    if y.eof():
        return {}
    node = y.block(y.indent())
    y.skip()
    if not y.eof():
        y.error("text after the document")
    return node


# ---------------------------------------------------------------------------------------------
# parity.yml.

M7_LEG = "m7-qemu"
# A host leg's platform, by the first two words of its name: (platform.system(),
# platform.machine() values).
PLATFORMS = {
    "linux-x64": ("Linux", ("x86_64", "amd64")),
    "linux-arm64": ("Linux", ("aarch64", "arm64")),
    "windows-x64": ("Windows", ("amd64", "x86_64")),
    "macos-arm64": ("Darwin", ("arm64", "aarch64")),
}
# The compiler CMake must find: by CXX, or, without one, the platform's own.
COMPILERS = {"g++": "GNU", "clang++": "Clang"}
NATIVE = {"Windows": "MSVC", "Darwin": "AppleClang"}
HARNESSES = {"brainscape_golden": "golden", "brainscape_golden_flush": "flush",
             "brainscape_parity_stream": "stream"}

WORKFLOW_ENV = {"PARITY_HASHES_GATING", "PARITY_AUDITS_GATING"}
CHECKOUT = {"uses": "actions/checkout@v4"}
HOST_KEYS = {"name", "strategy", "runs-on", "defaults", "env", "steps", "timeout-minutes"}
HOST_MATRIX_KEYS = {"leg", "os", "cxx", "cxxflags", "symbols"}
HOST_ENV = {"LEG": "${{ matrix.leg }}", "LEG_CXX": "${{ matrix.cxx }}", "LEG_SYMBOLS": "${{ matrix.symbols }}",
            "CXXFLAGS": "${{ matrix.cxxflags }}"}
HOST_CONFIGURE = re.compile(r'if \[ -n "\$LEG_CXX" \]; then export CXX="\$LEG_CXX"; fi\n'
                            r"cmake -B build((?: -D[\w./=:+-]+)+)\n")
HOST_BUILD = re.compile(r"cmake --build build --config Release --target((?: brainscape_\w+)+)")
HOST_SELECT = ('echo "BIN=$(find build -type f \\( -name brainscape_golden -o -name brainscape_golden.exe \\) '
               '| head -n 1)" >> "$GITHUB_ENV"\n'
               'if [ "$PARITY_HASHES_GATING" = true ]; then echo MODE=check; else echo MODE=report; fi '
               '>> "$GITHUB_ENV"\n'
               'mkdir -p parity-report "parity-wav/$LEG"\n')
M7_KEYS = {"name", "runs-on", "steps", "timeout-minutes"}
M7_CONFIGURE = re.compile(r"cmake -B build-m7((?: -D[\w./=:+-]+)+)\n")
M7_BUILD = re.compile(r"cmake --build build-m7 --target((?: brainscape_\w+)+)")
M7_SELECT = 'if [ "$PARITY_HASHES_GATING" = true ]; then echo MODE=check; else echo MODE=report; fi >> "$GITHUB_ENV"\n'
M7_EMULATOR = ["qemu-arm", "-cpu", "cortex-m7"]
M7_ELF = re.compile(r"build-m7/dsp/tests/golden/"
                    r"(brainscape_golden|brainscape_golden_flush|brainscape_parity_stream)\.elf")
RENDER_IF = re.compile(r"\$\{\{ !cancelled\(\) && steps\.build\.outcome == 'success'"
                       r"(?: && matrix\.leg == '([\w-]+)')? \}\}")
RENDER_CONTINUE = "${{ env.PARITY_HASHES_GATING != 'true' }}"
RENDER_KEYS = {"name", "if", "continue-on-error", "run"}
# A render script's lines besides harness calls.
LOOP = re.compile(r"for (\w+) in ([^;]+); do")
OTHER_LINES = [re.compile(p) for p in (
    r"done", r"rc=0", r"exit \$rc", r"mkdir -p [\w./$\"{} -]+", r'echo "[^"`]*"',
    r"(?!(?:BIN|MODE|LEG|CXX|CXXFLAGS|PATH)=)[A-Z][A-Z0-9_]*=(?:\$\(.*\)|\"\$\(.*\)[^\"]*\")")]
STREAM_CHECK = re.compile(r"python3 tools/hil/parity_check\.py --log (\S+)")
FAILURE_TAIL = re.compile(r'\{ echo "[^"`]*"; rc=1; \}')
# Arguments of a harness call that are not the render's own: one value each.
CONTROL = {"--mode", "--tag", "--report", "--out", "--wav-dir", "--note"}
HOST_BIN = ("$BIN", "${BIN}")
BIN_REF = re.compile(r"\$\{?BIN\b")
BIN_SET = re.compile(r"\bBIN=")
HARNESS_NAME = re.compile(r"\bbrainscape_(?:golden|golden_flush|parity_stream)\b")
# Outside the render steps, qemu-arm runs only as a version query or on a literal ELF path.
QEMU_COMMAND = re.compile(r"(?:^|(?<=[\s(;|&]))qemu-arm(?=\s|$)", re.M)
M7_OTHER_CALL = re.compile(r"qemu-arm (?:--version\b|-cpu cortex-m7 build-m7/[\w./-]+\.elf(?=\s|$))")

Leg = collections.namedtuple("Leg", "name os cxx cxxflags compiler")
Render = collections.namedtuple("Render", "harness args wav")  # harness: golden, flush or stream
Build = collections.namedtuple("Build", "cxx cxxflags compiler defines targets")
Plan = collections.namedtuple("Plan", "legs host_setup host_build host_renders m7_os m7_setup m7_build m7_renders")


def _need(ok, why):
    if not ok:
        raise PlanError(why)


def _step_text(step):
    return step.get("name") or step.get("uses") or "a step"


def _render_script(script, call, where):
    """The harness calls of one render step's script: [(words, loops)], each call's words with
    the loop variables in force ({var: values}). Every line must be one this reader knows."""
    text = re.sub(r"\\\n[ \t]*", " ", script)
    text = re.sub(r"(\|\||&&)[ \t]*\n[ \t]*", r"\1 ", text)
    calls, loop, streams = [], None, []
    for raw in text.split("\n"):
        line = raw.strip()
        if not line:
            continue
        m = LOOP.fullmatch(line)
        if m:
            _need(loop is None, f"{where}: a loop inside a loop")
            loop = (m.group(1), m.group(2).split())
            continue
        if line == "done":
            _need(loop is not None, f"{where}: done without a loop")
            loop = None
            continue
        m = STREAM_CHECK.fullmatch(line)
        if m:
            _need(m.group(1) in streams, f"{where}: parity_check.py checks {m.group(1)}, which no stream call wrote")
            streams.remove(m.group(1))
            continue
        if call(line) is None:
            _need(any(p.fullmatch(line) for p in OTHER_LINES),
                  f"{where}: a line the render does not know: {line[:120]!r}")
            continue
        lexer = shlex.shlex(line, posix=True, punctuation_chars=True)
        lexer.whitespace_split = True
        try:
            words = list(lexer)
        except ValueError as err:
            raise PlanError(f"{where}: cannot split {line[:120]!r} ({err})")
        ends = [i for i, w in enumerate(words) if w and set(w) <= set("|&;<>()")]
        cut = ends[0] if ends else len(words)
        if cut < len(words):
            tail = line[line.index("||"):] if words[cut] == "||" else ""
            _need(tail and FAILURE_TAIL.fullmatch(tail[2:].strip()),
                  f"{where}: a harness call followed by something other than || {{ echo ...; rc=1; }}: {line[:120]!r}")
        kind, words = call(line, words[:cut])
        calls.append((kind, words, dict([loop]) if loop else {}))
        if kind == "stream":
            streams.append(_option(words, "--out"))
    _need(loop is None, f"{where}: a loop without done")
    _need(not streams, f"{where}: a stream that parity_check.py does not check")
    return calls


def _option(words, name):
    for i, w in enumerate(words[:-1]):
        if w == name:
            return words[i + 1]
    return None


def _render(kind, words, loops, tag, where):
    """A harness call's words (after the harness) as a Render, its loop expanded: [Render]."""
    seen, args, wav = collections.Counter(), [], False
    i = 0
    while i < len(words):
        w = words[i]
        if w in CONTROL:
            _need(i + 1 < len(words), f"{where}: {w} without a value")
            value = words[i + 1]
            seen[w] += 1
            if w == "--mode":
                _need(value == "$MODE", f"{where}: --mode {value}, not \"$MODE\"")
            elif w == "--tag":
                _need(value == tag, f"{where}: --tag {value}, not {tag}")
            elif w == "--wav-dir":
                wav = True
            i += 2
            continue
        args.append(w)
        i += 1
    want = {"--tag": 1, "--out": 1} if kind == "stream" else {"--mode": 1, "--tag": 1, "--report": 1}
    for name, n in want.items():
        _need(seen[name] == n, f"{where}: a call with {name} {seen[name]} times, not once")
    for name in set(seen) - set(want) - {"--wav-dir", "--note"}:
        _need(False, f"{where}: a {kind} call with {name}")
    _need(seen["--wav-dir"] <= 1, f"{where}: --wav-dir twice")
    variables = {a for a in args if "$" in a}
    for v in variables:
        name = re.fullmatch(r"\$\{?(\w+)\}?", v)
        _need(name and name.group(1) in loops and v in (f"${name.group(1)}", f"${{{name.group(1)}}}"),
              f"{where}: an argument {v!r} that is not a loop variable")
    if not variables:
        return [Render(kind, tuple(args), wav)]
    _need(len(variables) == 1, f"{where}: two loop variables in one call")
    (v,) = variables
    values = loops[re.fullmatch(r"\$\{?(\w+)\}?", v).group(1)]
    return [Render(kind, tuple(x if a == v else a for a in args), wav) for x in values]


def _host_call(line, words=None):
    """None when a line runs no harness; with its words, the call's (harness, words after it)."""
    if not (BIN_REF.search(line) or BIN_SET.search(line)):
        return None
    if words is None:
        return True
    _need(len(BIN_REF.findall(line)) == 1 and not BIN_SET.search(line) and words and words[0] in HOST_BIN,
          f"a parity-host line that uses BIN other than as one harness call: {line[:120]!r}")
    return "golden", words[1:]


def _m7_call(line, words=None):
    if not HARNESS_NAME.search(line):
        return None
    if words is None:
        return True
    _need(len(HARNESS_NAME.findall(line)) == 1 and words[:3] == M7_EMULATOR and len(words) > 3
          and M7_ELF.fullmatch(words[3]),
          f"a parity-m7 line that names a harness other than as qemu-arm -cpu cortex-m7 <its ELF>: {line[:120]!r}")
    return HARNESSES[M7_ELF.fullmatch(words[3]).group(1)], words[4:]


def _strings(node):
    """Every string in a step."""
    if isinstance(node, dict):
        return [s for v in node.values() for s in _strings(v)]
    if isinstance(node, list):
        return [s for v in node for s in _strings(v)]
    return [node] if isinstance(node, str) else []


def _mentions(step, job):
    """Whether a step names a harness (or, on parity-host, BIN)."""
    text = "\n".join(_strings(step))
    return bool(HARNESS_NAME.search(text) or (job == "parity-host" and (BIN_REF.search(text) or BIN_SET.search(text))))


def _renders(steps, start, call, tag, job, legs):
    """The render steps from steps[start]: [(only leg or None, Render)], and the index after them."""
    out, i = [], start
    while i < len(steps) and isinstance(steps[i], dict) and _mentions(steps[i], job):
        step = steps[i]
        where = f"{job} step {_step_text(step)!r}"
        _need(set(step) <= RENDER_KEYS and isinstance(step.get("run"), str),
              f"{where}: keys {sorted(set(step) - RENDER_KEYS)} a render step does not have, or no run")
        only = None
        if step.get("if") is not None:
            m = RENDER_IF.fullmatch(step["if"])
            _need(m is not None, f"{where}: a condition the render does not know: {step['if']!r}")
            only = m.group(1)
            _need(only is None or (legs is not None and only in legs),
                  f"{where}: a condition on {only!r}, which is not a leg of {job}")
        _need(step.get("continue-on-error") in (None, RENDER_CONTINUE),
              f"{where}: continue-on-error {step.get('continue-on-error')!r}")
        calls = _render_script(step["run"], call, where)
        _need(calls, f"{where}: names a harness and calls none")
        for kind, words, loops in calls:
            out += [(only, r) for r in _render(kind, words, loops, tag, where)]
        i += 1
    _need(out, f"{job}: no render step after the setup")
    return out, i


def _outside(steps, job):
    """The steps other than the build, select and render steps: none may name a harness, and on
    parity-m7 none may run qemu-arm but on a literal ELF path."""
    for step in steps:
        _need(not _mentions(step, job),
              f"{job} step {_step_text(step)!r} names a harness outside the render steps the render reads")
        run = step.get("run") if isinstance(step, dict) else None
        if job == "parity-m7" and isinstance(run, str):
            text = re.sub(r"\\\n[ \t]*", " ", run)
            for m in QEMU_COMMAND.finditer(text):
                _need(M7_OTHER_CALL.match(text, m.start()),
                      f"{job} step {_step_text(step)!r} runs qemu-arm on something other than a literal "
                      f"ELF path: {text[m.start():m.start() + 120]!r}")


def _setup_until(steps, job, configure):
    """steps[0] is the checkout; the setup steps up to the configure step, and its index."""
    _need(isinstance(steps, list) and steps and steps[0] == CHECKOUT,
          f"{job}: the first step is not actions/checkout@v4 without options")
    for i in range(1, len(steps)):
        run = steps[i].get("run") if isinstance(steps[i], dict) else None
        if isinstance(run, str) and configure.fullmatch(run):
            return steps[1:i], i
    raise PlanError(f"{job}: no configure step the render knows")


def _fixed(step, job, name, keys, run):
    _need(isinstance(step, dict) and step.get("name") == name and set(step) == keys,
          f"{job}: the step after configure is not {name!r} with exactly {sorted(keys)}")
    m = run.fullmatch(step["run"]) if hasattr(run, "fullmatch") else (step["run"] == run)
    _need(m, f"{job} step {name!r}: a script the render does not know: {step['run']!r}")
    return m


def read(text):
    """The Plan of a parity.yml's text; raises PlanError."""
    doc = load_yaml(text)
    _need(isinstance(doc, dict) and isinstance(doc.get("jobs"), dict), "no jobs")
    env = doc.get("env") or {}
    _need(isinstance(env, dict) and set(env) <= WORKFLOW_ENV,
          f"workflow env {sorted(env) if isinstance(env, dict) else env!r}: only {sorted(WORKFLOW_ENV)} are known")
    host, m7 = doc["jobs"].get("parity-host"), doc["jobs"].get("parity-m7")
    _need(isinstance(host, dict) and isinstance(m7, dict), "no parity-host or parity-m7 job")

    # parity-host.
    _need(set(host) <= HOST_KEYS, f"parity-host: keys {sorted(set(host) - HOST_KEYS)} the render does not know")
    strategy = host.get("strategy") or {}
    matrix = strategy.get("matrix") if isinstance(strategy, dict) else None
    _need(isinstance(matrix, dict) and set(matrix) == {"include"} and isinstance(matrix["include"], list)
          and set(strategy) <= {"fail-fast", "matrix"},
          "parity-host: a strategy other than a matrix of include entries")
    legs = collections.OrderedDict()
    for entry in matrix["include"]:
        _need(isinstance(entry, dict) and {"leg", "os"} <= set(entry) <= HOST_MATRIX_KEYS
              and all(isinstance(v, str) for v in entry.values()),
              f"parity-host: a matrix entry the render does not know: {entry!r}")
        name, cxx = entry["leg"], entry.get("cxx", "")
        _need(re.fullmatch(r"[a-z0-9]+(?:-[a-z0-9]+)+", name) and name not in legs and name != M7_LEG,
              f"parity-host: a leg name {name!r} that is not a new lowercase name")
        _need(re.fullmatch(r"[\w.-]+", entry["os"]), f"parity-host {name}: a runner {entry['os']!r}")
        place = PLATFORMS.get("-".join(name.split("-")[:2]))
        _need(place is not None, f"parity-host {name}: a platform the render does not know")
        compiler = COMPILERS.get(cxx) if cxx else NATIVE.get(place[0])
        _need(compiler is not None, f"parity-host {name}: a compiler (cxx {cxx!r}) the render does not know")
        legs[name] = Leg(name, entry["os"], cxx, entry.get("cxxflags", ""), compiler)
    _need(legs, "parity-host: no leg")
    _need(host.get("runs-on") == "${{ matrix.os }}", "parity-host: runs-on is not ${{ matrix.os }}")
    _need(host.get("defaults") == {"run": {"shell": "bash"}}, "parity-host: defaults other than bash")
    _need(host.get("env") == HOST_ENV, f"parity-host: env {host.get('env')!r}, not exactly {HOST_ENV!r}")
    steps = host.get("steps")
    host_setup, i = _setup_until(steps, "parity-host", HOST_CONFIGURE)
    _need(i + 3 < len(steps), "parity-host: no steps after configure")
    _need(set(steps[i]) == {"name", "run"} and steps[i].get("name") == "Configure",
          "parity-host: the configure step is not 'Configure' with exactly name and run")
    defines = HOST_CONFIGURE.fullmatch(steps[i]["run"]).group(1).split()
    targets = _fixed(steps[i + 1], "parity-host", "Build the golden harness", {"name", "id", "run"},
                     HOST_BUILD).group(1).split()
    _need(steps[i + 1].get("id") == "build", "parity-host: the build step's id is not build")
    _fixed(steps[i + 2], "parity-host", "Select the harness mode", {"name", "run"}, HOST_SELECT)
    host_renders, end = _renders(steps, i + 3, _host_call, "$LEG", "parity-host", legs)
    _outside(host_setup + steps[end:], "parity-host")
    _need("brainscape_golden" in targets, "parity-host: the build step does not build brainscape_golden")
    host_build = (tuple(defines), tuple(targets))

    # parity-m7.
    _need(set(m7) <= M7_KEYS, f"parity-m7: keys {sorted(set(m7) - M7_KEYS)} the render does not know")
    m7_os = m7.get("runs-on")
    _need(isinstance(m7_os, str) and re.fullmatch(r"[\w.-]+", m7_os), f"parity-m7: a runner {m7_os!r}")
    steps = m7.get("steps")
    m7_setup, i = _setup_until(steps, "parity-m7", M7_CONFIGURE)
    _need(i + 3 < len(steps), "parity-m7: no steps after configure")
    _need(set(steps[i]) == {"name", "run"} and steps[i].get("name") == "Configure (firmware flags)",
          "parity-m7: the configure step is not 'Configure (firmware flags)' with exactly name and run")
    m7_defines = M7_CONFIGURE.fullmatch(steps[i]["run"]).group(1).split()
    m7_targets = _fixed(steps[i + 1], "parity-m7", "Build the golden harness ELFs", {"name", "id", "run"},
                        M7_BUILD).group(1).split()
    _need(steps[i + 1].get("id") == "build", "parity-m7: the build step's id is not build")
    _fixed(steps[i + 2], "parity-m7", "Select the harness mode", {"name", "run"}, M7_SELECT)
    m7_renders, end = _renders(steps, i + 3, _m7_call, M7_LEG, "parity-m7", None)
    m7_renders = [r for _, r in m7_renders]
    _outside(m7_setup + steps[end:], "parity-m7")
    for job, renders, built in (("parity-host", [r for _, r in host_renders], targets),
                                ("parity-m7", m7_renders, m7_targets)):
        for r in renders:
            target = next(t for t, h in HARNESSES.items() if h == r.harness)
            _need(target in built, f"{job}: it renders with {target}, which its build step does not build")
    return Plan(legs, host_setup, host_build, host_renders, m7_os, m7_setup, (tuple(m7_defines), tuple(m7_targets)),
                m7_renders)


def has_leg(plan, leg):
    return leg == M7_LEG or leg in plan.legs


def leg_renders(plan, leg):
    """The renders the plan makes on a leg, in parity.yml's order."""
    if leg == M7_LEG:
        return list(plan.m7_renders)
    return [r for only, r in plan.host_renders if only in (None, leg)]


def leg_build(plan, leg):
    """How the plan builds a leg: Build(cxx, cxxflags, compiler, defines, targets)."""
    if leg == M7_LEG:
        return Build("", "", "GNU", *plan.m7_build)
    spec = plan.legs[leg]
    return Build(spec.cxx, spec.cxxflags, spec.compiler, *plan.host_build)


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    path = argv[0] if argv else os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", ".github",
                                             "workflows", "parity.yml")
    with open(path, encoding="utf-8") as fh:
        plan = read(fh.read())
    for leg in [*plan.legs, M7_LEG]:
        b = leg_build(plan, leg)
        where = plan.m7_os if leg == M7_LEG else plan.legs[leg].os
        print(f"{leg} on {where}: {b.compiler} (CXX {b.cxx or '-'}, CXXFLAGS {b.cxxflags or '-'}), "
              f"{' '.join(b.defines)}, targets {' '.join(b.targets)}")
        for r in leg_renders(plan, leg):
            print(f"  {r.harness} {' '.join(r.args)}{' (WAV files)' if r.wav else ''}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
