#!/usr/bin/env python3
"""DWT measurement report for the Daisy Seed bench images (firmware/README.md). Stdlib only.

Drives the bench image over USB serial, or reads captured runs, and renders what the
determinism profile's hardware-gated decisions need (docs/design/determinism-profile.md §4.2,
§7, §8.3 Q1/Q2/Q5/Q9; grain-engine.md §8): cycles per 48-frame block against the 480 MHz /
48 kHz budget (10,000 cycles per sample), per-stage costs and an upper bound on the per-birth
cost, the Restart clear against the SDRAM's own floor, FPU subnormal latency, the flush
idiom's cost, the silent-tail test with its decision rule, the §7.3 explicit-FMA rule and the
§7.1 code-placement comparison. Several logs (brainscape_bench, _xip, _hooks) are compared
side by side; every section says which log its numbers come from.

  bench_report.py --port auto [--run "run"] [--save bench.log]
  bench_report.py --log bench.log [--log bench_xip.log --log bench_hooks.log] [--markdown out.md]
                  [--expect-archive build/fw/firmware/engine-archives.sha256]

A run is INCOMPLETE, and the exit status 1, when its stream misses bench-begin, bench-end or
an expected suite line, holds a line that is not a JSON object, carries a "resync" notice or
a nonzero drop count (the device's USB serial lost lines), reports an engine archive other
than --expect-archive's, or ran with a cache off (its budget figures are then withheld).
Exit 2: no bench data at all.
"""
import argparse
import json
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hilserial  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_GOLDEN = os.path.normpath(os.path.join(HERE, "..", "..", "dsp", "tests", "golden", "golden.json"))
FLUSH_SITES_PER_SAMPLE = 45  # determinism-profile.md §4.3 (derived count, record §4.3)
RULE_RATIO, RULE_FLAGGED_PCT = 1.2, 0.5  # profile §4.2's decision rule

# Lines each suite emits (firmware/bench/main.cpp); "tail" is 2, or 4 in the hooks build.
SUITE_LINES = {"memory": {"memory": 1}, "micro": {"micro": 26, "flush-micro": 6}, "restart": {"restart": 8},
               "blocks": {"blocks": 18}, "stages": {"stage": 9}, "births": {"births": 4}, "tail": {"tail": 2}}
RUN_SUITES = ("memory", "micro", "restart", "blocks", "stages", "births")


class Run:
    def __init__(self, name):
        self.name = name
        self.hello = None
        self.begin = None
        self.end = None
        self.lines = []
        self.garbled = []
        self.resyncs = []
        self.errors = []
        self.problems = []
        self.notes = []
        self.port_lost = None

    def of(self, kind):
        return [o for o in self.lines if o.get("type") == kind]

    @property
    def hooks(self):
        return bool((self.begin or {}).get("hooks") or (self.hello or {}).get("hooks"))

    @property
    def code(self):
        return (self.begin or {}).get("engineCode") or (self.hello or {}).get("engineCode") or "?"

    @property
    def budget_ok(self):
        cpu = (self.begin or {}).get("cpu") or {}
        return bool(cpu.get("icache")) and bool(cpu.get("dcache"))

    @property
    def label(self):
        b = self.begin or {}
        h = self.hello or {}
        target = b.get("target") or h.get("target") or self.name
        return "%s (engine code %s%s)" % (target, self.code, ", hooks" if self.hooks else "")


def read_lines(source, run, save=None, command=None, timeout=900.0):
    out = open(save, "w", encoding="utf-8") if save else None
    device = isinstance(source, hilserial.Port)
    try:
        if device:
            source.drain()
            run.hello = hilserial.hello(source)
            if run.hello is None or run.hello.get("image") != "bench":
                print("no bench image answering on %s (firmware/README.md)" % source.name)
                return False
            if out:
                out.write(json.dumps(run.hello) + "\n")
            source.write_line(command)
            print("measuring on the device; a full run takes about 10-15 minutes...", flush=True)
        last = time.monotonic()
        while True:
            line = source.read_line(timeout=1.0)
            if line is None:
                if isinstance(source, hilserial.LogSource) or time.monotonic() - last > timeout:
                    break
                continue
            last = time.monotonic()
            if out:
                out.write(line + "\n")
                out.flush()
            obj = hilserial.parse(line)
            if obj is None:
                if run.begin is not None and run.end is None and line.strip():
                    run.garbled.append(line[:100])
                continue
            kind = obj.get("type")
            if kind == "hello":
                run.hello = run.hello or obj
                continue
            if kind == "resync":
                run.resyncs.append(obj)
                continue
            if kind == "error":
                run.errors.append(obj.get("message"))
                print("device error: %s" % obj.get("message"))
                continue
            if kind == "bench-begin":
                run.begin = obj
                continue
            if kind == "bench-end":
                run.end = obj
                break
            run.lines.append(obj)
            if device and kind in ("blocks", "stage", "births", "tail", "restart"):
                print("  %s %s %s" % (kind, obj.get("config") or obj.get("op"), obj.get("cache", "")), flush=True)
    except hilserial.PortGone as e:
        run.port_lost = str(e)
    finally:
        if out:
            out.close()
    return True


def load_expected_archives(spec):
    if spec is None:
        return None
    if re.fullmatch(r"[0-9a-fA-F]{64}", spec):
        return {"libbrainscape_dsp.a": spec.lower()}
    archives = {}
    with open(spec, encoding="utf-8") as f:
        for line in f:
            parts = line.split()
            if len(parts) == 2 and re.fullmatch(r"[0-9a-fA-F]{64}", parts[0]):
                archives[os.path.basename(parts[1])] = parts[0].lower()
    if not archives:
        raise SystemExit("--expect-archive %s: no SHA-256 found" % spec)
    return archives


def validate(run, archives):
    """Fills run.problems (the run is INCOMPLETE) and run.notes."""
    b = run.begin
    if b is None:
        run.problems.append("no bench-begin line")
    if run.end is None:
        run.problems.append("no bench-end line: the run stopped (a hang, a reset, a USB drop or a timeout)")
    if run.garbled:
        run.problems.append("%d line(s) are not JSON objects, e.g. %r" % (len(run.garbled), run.garbled[0]))
    if run.resyncs:
        run.problems.append("the device's USB serial dropped lines (%d resync notice(s))" % len(run.resyncs))
    if run.end is not None and (run.end.get("droppedBytes") or run.end.get("droppedLines")):
        run.problems.append("the device's USB serial dropped %s byte(s) in %s line(s)" % (
            run.end.get("droppedBytes"), run.end.get("droppedLines")))
    if run.port_lost:
        run.problems.append("the serial port went away (%s): the device reset? Its next hello reports "
                            "lastFault" % run.port_lost)
    if run.errors:
        run.problems.append("device error(s): %s" % "; ".join(str(e) for e in run.errors))
    if b is not None:
        suite = (b.get("suite") or "").split()
        name, quick = (suite[0] if suite else ""), "quick" in suite
        suites = RUN_SUITES + (() if quick else ("tail",)) if name == "run" else (name,)
        for s in suites:
            for kind, count in SUITE_LINES.get(s, {}).items():
                want = 4 if (kind == "tail" and run.hooks) else count
                got = len(run.of(kind))
                if got != want:
                    run.problems.append("%s: %d %s line(s), expected %d" % (s, got, kind, want))
        if not run.budget_ok:
            run.problems.append("caches: I-cache %s, D-cache %s; budget figures withheld" % (
                ((b.get("cpu") or {}).get("icache")), ((b.get("cpu") or {}).get("dcache"))))
        if b.get("firmwareDirty"):
            run.notes.append("firmware built from a tree with uncommitted changes")
        if archives is not None:
            key = "libbrainscape_dsp_fpenv_hooks.a" if run.hooks else "libbrainscape_dsp.a"
            want = archives.get(key)
            got = (b.get("engineArchiveSha256") or "").lower()
            if want is None:
                run.notes.append("--expect-archive has no hash for %s: not checked" % key)
            elif got != want:
                run.problems.append("engine archive %s, --expect-archive wants %s" % (got[:16], want[:16]))


class Out:
    def __init__(self):
        self.text = []

    def h(self, title):
        self.text.append("")
        self.text.append("## " + title)
        self.text.append("")

    def p(self, line=""):
        self.text.append(line)

    def table(self, header, rows):
        widths = [max(len(str(x)) for x in col) for col in zip(header, *rows)] if rows else [len(h) for h in header]
        fmt = "| " + " | ".join("%%-%ds" % w for w in widths) + " |"
        self.text.append(fmt % tuple(header))
        self.text.append("|" + "|".join("-" * (w + 2) for w in widths) + "|")
        for r in rows:
            self.text.append(fmt % tuple(str(x) for x in r))


def pct(cycles, budget):
    return "%.1f%%" % (100.0 * cycles / budget) if budget else "?"


def per_sample(cycles, frames=48):
    return "%.0f" % (cycles / float(frames))


def shipping(runs):
    """The run of the shipping archive with its code in ITCM (brainscape_bench), else any non-hooks run."""
    for r in runs:
        if not r.hooks and r.code == "ITCM":
            return r
    return next((r for r in runs if not r.hooks), runs[0])


def report(runs, golden, o):
    budget = next(((r.begin or {}).get("budgetCyclesPerBlock") for r in runs if r.begin), 480000)
    hz = next(((r.begin or {}).get("clockHz") for r in runs if r.begin), 480000000)
    o.p("# Brainscape DWT measurement pass (Daisy Seed)")
    o.p()
    incomplete = [r for r in runs if r.problems]
    if incomplete:
        o.p("**INCOMPLETE:** %s. The figures below come from what arrived; rerun before relying on them." %
            "; ".join("%s: %s" % (r.name, ", ".join(r.problems)) for r in incomplete))
        o.p()
    for r in runs:
        b = r.begin or {}
        cpu = b.get("cpu") or {}
        sdram = next((m for m in cpu.get("mpu") or [] if (m.get("rbar") or "").upper().startswith("0XC0")), {})
        o.p("- **%s** (log %s): archive `%s`, firmware %s%s, clock %s Hz, measurement overhead %s cycles" % (
            r.label, r.name, (b.get("engineArchiveSha256") or "?")[:16], b.get("firmwareCommit", "?"),
            " (dirty)" if b.get("firmwareDirty") else "", b.get("clockHz"), b.get("measurementOverhead")))
        o.p("  caches: I %s, D %s (CCR %s); SDRAM MPU region RASR %s; QSPI CR %s CCR %s; mem* in %s" % (
            "on" if cpu.get("icache") else "OFF", "on" if cpu.get("dcache") else "OFF", cpu.get("ccr", "?"),
            sdram.get("rasr", "?"), (cpu.get("qspi") or {}).get("cr", "?"), (cpu.get("qspi") or {}).get("ccr", "?"),
            (cpu.get("memFunctions") or {}).get("in", "?")))
        for line in hilserial.describe_hello(r.hello):
            o.p("  " + line.strip())
        for n in r.notes:
            o.p("  note: " + n)
    o.p()
    o.p("Budget: %d cycles per 48-frame block = 10,000 cycles per sample (480 MHz / 48 kHz). "
        "Interrupts are off around each measured call (and the cold-cache maintenance)." % budget)
    budget_runs = [r for r in runs if r.budget_ok]
    if len(budget_runs) < len(runs):
        o.p("Budget figures are withheld for %s: a cache was off." % ", ".join(
            r.name for r in runs if not r.budget_ok))
    multi = len(runs) > 1

    mem_run = next((r for r in runs if r.of("memory")), None)
    if mem_run:
        m = mem_run.of("memory")[0]
        o.h("Memory (grain-engine.md §7; from %s)" % mem_run.name)
        o.table(["tier", "PlanMemory (maxBlockSize 48)", "arena", "where"], [
            ["Hot", m["hotBytes"], m["hotArena"], "DTCM"],
            ["Warm", m["warmBytes"], m["warmArena"], "AXI SRAM (D1)"],
            ["Bulk", m["bulkBytes"], m["bulkArena"], "SDRAM"],
            ["Engine object", m["engineBytes"], m["engineSlot"], "DTCM"]])
        mp = m.get("map", {})
        rows = [[k, v.get("start", ""), v.get("bytes", "")] for k, v in mp.items() if isinstance(v, dict)]
        o.p()
        o.table(["linker section", "start", "bytes"], rows)

    micro_runs = [r for r in runs if r.of("micro")]
    if micro_runs:
        o.h("FPU latency, normal vs subnormal (profile §4.2 test a, §8.3 Q1)")
        rows = []
        for r in micro_runs:
            by = {}
            for m in r.of("micro"):
                by.setdefault(m["name"], {})[m["fz"]] = m["cycles"] / float(m["instructions"])
            for k, v in by.items():
                rows.append(([r.name] if multi else []) + [k, "%.2f" % v.get(0, float("nan")),
                                                           "%.2f" % v.get(1, float("nan"))])
        o.table((["log"] if multi else []) + ["dependent chain", "cycles/instr FZ=0",
                                              "same program at FZ=1 (subnormal operands flushed to 0)"], rows)
        for r in micro_runs:
            by = {}
            for m in r.of("micro"):
                by.setdefault(m["name"], {})[m["fz"]] = m["cycles"] / float(m["instructions"])
            def g(name):
                return by.get(name, {}).get(0)
            pairs = [("vmul with subnormal operands", "vmul subnormal operand and result", "vmul normal"),
                     ("an underflowing vmul pair", "vmul pair, every other result subnormal (underflow)",
                      "vmul pair, normal throughout"),
                     ("a subnormal vadd/vsub pair (nonzero addend)",
                      "vadd/vsub pair, subnormal operands and results (nonzero addend)",
                      "vadd/vsub pair, normal throughout")]
            parts = ["%s %.2fx" % (what, g(sub) / g(norm)) for what, sub, norm in pairs if g(sub) and g(norm)]
            if parts:
                o.p()
                o.p("Subnormal penalty at FZ = 0 (%s): %s the normal latency. The FZ = 1 column runs the same "
                    "instructions with the operands flushed to zero, so it times zero arithmetic, not "
                    "subnormal arithmetic under FZ." % (r.name, "; ".join(parts)))

    flush_runs = [r for r in runs if r.of("flush-micro")]
    if flush_runs:
        o.h("The deterministic flush idiom (profile §4.3, §8.3 Q5)")
        rows = []
        for r in flush_runs:
            for chains in (8, 1):
                lines = [f for f in r.of("flush-micro") if f.get("chains", 8) == chains]
                base = next((f["cycles"] / float(f["updates"]) for f in lines if f["form"] == "none"), 0.0)
                for f in lines:
                    c = f["cycles"] / float(f["updates"])
                    extra = c - base
                    rows.append(([r.name] if multi else []) + [
                        "%d %s" % (chains, "independent (ILP hides latency)" if chains > 1 else "recursive (latency)"),
                        f["form"], "%.2f" % c, "%.2f" % extra, "%.0f" % (extra * FLUSH_SITES_PER_SAMPLE),
                        pct(extra * FLUSH_SITES_PER_SAMPLE, 10000)])
        o.table((["log"] if multi else []) + ["chains", "form", "cycles/update", "flush cost/site",
                                              "x %d sites/sample" % FLUSH_SITES_PER_SAMPLE, "of budget"], rows)
        o.p()
        o.p("The engine's %d sites per sample (the profile's derived count) sit on recursions, so the per-sample "
            "cost lies between the 8-chain figure (a lower bound: independent updates overlap the flush) and the "
            "1-chain figure (an upper bound: every flush on the critical path). Both are estimates from a measured "
            "per-site cost." % FLUSH_SITES_PER_SAMPLE)

    restart_runs = [r for r in runs if r.of("restart")]
    if restart_runs:
        o.h("Restart and loads against the SDRAM's own floor (profile §5.8, §8.3 Q9)")
        rows = []
        for r in restart_runs:
            for x in r.of("restart"):
                rows.append(([r.name] if multi else []) + [x["op"], x["state"], x["cycles"],
                                                           "%.2f" % (1000.0 * x["cycles"] / hz)])
        o.table((["log"] if multi else []) + ["operation", "state", "cycles", "ms"], rows)
        for r in restart_runs:
            rs = r.of("restart")
            restart = next((x for x in rs if x["op"] == "Restart" and x["state"].startswith("after")), None)
            raw = next((x for x in rs if x["op"].startswith("raw clear") and x["state"].startswith("memset")), None)
            floor = next((x for x in rs if x["op"].startswith("raw clear") and x["state"].startswith("STM")), None)
            if restart:
                ms = 1000.0 * restart["cycles"] / hz
                o.p()
                text = ("%s: a dirty Restart takes %.1f ms = %.0f audio blocks of 1 ms, so an Exact load mutes the "
                        "wet path that long (the profile estimated 45-160 ms)." % (r.name, ms, ms))
                if raw and floor:
                    text += (" Clearing 16 MiB of SDRAM alone takes %.1f ms with the firmware's memset and %.1f ms "
                             "with an 8-register STM loop (the core's floor without DMA)." % (
                                 1000.0 * raw["cycles"] / hz, 1000.0 * floor["cycles"] / hz))
                o.p(text + " Q9's other half, the restart watermark (§5.8), does not exist yet.")

    def blocks_rows(kind):
        rows = []
        for r in budget_runs:
            for b in r.of(kind):
                rows.append([b["config"], b.get("cache", "")] + ([r.name] if multi else []) + [
                    per_sample(b["mean"]), pct(b["mean"], budget), per_sample(b["p99"]), pct(b["p99"], budget),
                    per_sample(b["p999"]), pct(b["p999"], budget), per_sample(b["max"]), pct(b["max"], budget),
                    b["maxBlock"], "" if b.get("exactLoad", True) else "INEXACT LOAD"])
        return rows

    head = ["config", "cache"] + (["log"] if multi else []) + ["mean c/smp", "mean", "p99 c/smp", "p99",
                                                               "p99.9 c/smp", "p99.9", "max c/smp", "max",
                                                               "max at block", ""]
    rows = blocks_rows("blocks")
    if rows:
        o.h("Cycles per 48-frame block (grain-engine.md §8, profile §7.2)")
        o.table(head, rows)
        for r in budget_runs:
            hashes = {}
            for b in r.of("blocks"):
                hashes.setdefault(b["config"], set()).add(b["hash"])
            diverged = [k for k, v in hashes.items() if len(v) > 1]
            if hashes:
                o.p()
                o.p("%s: warm and cold renders of each configuration %s." % (r.name, (
                    "produced identical output" if not diverged else "DIFFER for " + ", ".join(diverged))))
            ev = [b for b in r.of("blocks") if b.get("eventsRefused")]
            if ev:
                o.p("%s: the event queue refused %d event(s) in the events configuration." % (
                    r.name, sum(b["eventsRefused"] for b in ev)))

    # Profile §7.3: adopt explicit FMA only if contraction-off threatens the budget.
    ship = shipping(budget_runs) if budget_runs else None
    pess = [b for b in ship.of("blocks") if b["config"].startswith("pess")] if ship else []
    if pess:
        o.h("Explicit FMA (profile §7.3, §8.3 Q2)")
        warm = [b for b in pess if b.get("cache") == "warm"]
        cold = [b for b in pess if b.get("cache") == "cold"]
        ww = max(warm or pess, key=lambda b: b["max"])
        worst = max(pess, key=lambda b: b["max"])
        share = ww["max"] / float(budget)
        o.p("From %s, contraction off (every build: the profile's flags forbid contraction): the worst warm-cache "
            "pessimistic block is %s (%s, p99.9 %s), the worst with cold caches %s (%s). The profile estimated "
            "77-78 %% for the pessimistic row." % (
                ship.name, pct(ww["max"], budget), ww["config"], pct(ww["p999"], budget),
                pct(worst["max"], budget) if cold else "-", worst["config"] if cold else "-"))
        if share >= 1.0:
            o.p("**Verdict (§7.3 rule):** contraction-off THREATENS the budget at 64 voices (a block over its "
                "deadline): explicit FMA in the inner loops is a candidate; build and measure it before adopting.")
        else:
            o.p("**Verdict (§7.3 rule):** contraction-off does not threaten the budget at 64 voices (%.0f %% "
                "headroom in the worst warm block): no case for explicit FMA on this evidence. A target polyphony "
                "above 64 voices needs its own row." % (100.0 * (1.0 - share)))
        o.p("This pass measures the contraction-off cost only: no build with contraction on or with explicit FMA "
            "exists to compare against, and the births suite bounds the per-birth cost from above without deciding "
            "polynomial kernels against Init-built tables (§3.9).")

    if len(budget_runs) > 1:
        itcm = next((r for r in budget_runs if not r.hooks and r.code == "ITCM"), None)
        xip = next((r for r in budget_runs if not r.hooks and r.code == "XIP"), None)
        if itcm and xip:
            o.h("Code placement: QSPI execute-in-place against ITCM (profile §7.1)")
            rows = []
            for kind in ("blocks", "stage", "births"):
                a = {(b["config"], b.get("cache")): b for b in itcm.of(kind)}
                for b in xip.of(kind):
                    k = (b["config"], b.get("cache"))
                    if k in a and a[k]["mean"] and a[k]["max"]:
                        rows.append([kind, b["config"], b.get("cache"), "%.3f" % (b["mean"] / float(a[k]["mean"])),
                                     "%.3f" % (b["p999"] / float(a[k]["p999"] or 1)),
                                     "%.3f" % (b["max"] / float(a[k]["max"]))])
            o.table(["suite", "config", "cache", "XIP / ITCM mean", "p99.9", "max"], rows)
            o.p()
            o.p("Both builds place the firmware's memcpy, memmove and memset with the engine's code (ITCM in %s, "
                "QSPI in %s), so the ratio covers them too; libgcc's helpers likewise." % (itcm.name, xip.name))

    stage_runs = [r for r in budget_runs if r.of("stage")]
    if stage_runs:
        o.h("Per-stage cost by difference (pessimistic configuration, warm cache)")
        rows = []
        for r in stage_runs:
            stages = r.of("stage")
            base = next((s for s in stages if s["config"].endswith("all stages")), None)
            for s in stages:
                cost = "" if base is None or s is base else per_sample(base["mean"] - s["mean"])
                rows.append(([r.name] if multi else []) + [s["config"], per_sample(s["mean"]), per_sample(s["p999"]),
                                                           per_sample(s["max"]), cost])
        o.table((["log"] if multi else []) + ["configuration", "mean c/smp", "p99.9 c/smp", "max c/smp",
                                              "stage cost (mean) c/smp"], rows)
        o.p()
        o.p("A stage's cost is the drop in the mean when it alone is switched off (exact bypass for the post "
            "stages). The onset detector has no bypass and is inside every figure. Maxima come from different "
            "blocks in each render, so no per-stage maximum is derived from them.")

    birth_runs = [r for r in budget_runs if r.of("births")]
    if birth_runs:
        o.h("Per-birth cost at the maximum birth rate, an upper bound (grain-engine.md §8 ScheduleGrain row)")
        rows = []
        for r in birth_runs:
            by = {}
            for b in r.of("births"):
                by.setdefault(b["voices"], {})[b["grainFrames"]] = b
            for voices, d in sorted(by.items(), reverse=True):
                fast, slow = d.get(48), d.get(960)
                if not fast or not slow:
                    continue
                db = (fast["birthsPerBlockX1000"] - slow["birthsPerBlockX1000"]) / 1000.0
                per_birth = (fast["mean"] - slow["mean"]) / db if db else float("nan")
                rate = fast["birthsPerBlockX1000"] / 1000.0
                rows.append(([r.name] if multi else []) + [
                    voices, "%.1f" % rate, "%.1f" % (slow["birthsPerBlockX1000"] / 1000.0), per_sample(fast["mean"]),
                    per_sample(slow["mean"]), "<= %.0f" % per_birth, "<= %.0f" % (per_birth * rate / 48.0)])
        o.table((["log"] if multi else []) + ["voices", "births/block (1 ms)", "births/block (20 ms)",
                                              "mean c/smp 1 ms", "mean c/smp 20 ms", "cycles per birth",
                                              "birth cost c/smp at 1 ms"], rows)
        o.p()
        o.p("The 1 ms grains also read the ring at a new spray position every birth, so the difference includes "
            "that locality cost: an upper bound on ScheduleGrain, not a kernels-against-tables measurement.")

    tail_runs = [r for r in budget_runs if r.of("tail")]
    if tail_runs:
        o.h("Silent tails and the denormal decision (profile §4.2 test b, §8.3 Q1)")
        rows = []
        for r in tail_runs:
            for t in r.of("tail"):
                seg = t.get("tail") or {}
                flagged = seg.get("flaggedBlocks")
                rows.append([t["config"], "FZ=1" if t.get("forceFlush") else "FZ=0"] + ([r.name] if multi else []) + [
                    t["blocks"], t.get("tailStartBlock", "?"), per_sample(t["max"]),
                    per_sample(seg["max"]) if seg else "?", pct(seg["max"], budget) if seg else "?",
                    per_sample(seg["p999"]) if seg else "?",
                    "-" if flagged is None else "%d / %d" % (flagged, seg.get("blocks", 0)), t["hash"][:16]])
        o.table(["tail", "flush"] + (["log"] if multi else []) + [
            "blocks", "silent from block", "whole-run max c/smp", "silent-tail max c/smp", "silent-tail max",
            "silent-tail p99.9 c/smp", "silent-tail blocks with IDC or UFC", "sha256"], rows)
        gold = None
        for v in golden.get("vectors", []):
            if v["name"] == "strums_tail_123s":
                gold = next((p for p in v["presets"] if p["name"] == "tail_post_fb"), None)
        o.p()
        for r in tail_runs:
            for t in r.of("tail"):
                if t["config"].startswith("golden:") and gold:
                    o.p("- %s, %s at %s: %s golden.json" % (r.name, t["config"], "FZ=1" if t.get("forceFlush") else "FZ=0",
                                                            "MATCHES" if t["hash"] == gold["hash"] else "DOES NOT MATCH"))
        hooks_runs = [r for r in tail_runs if r.hooks]
        for r in hooks_runs:
            tails = r.of("tail")
            for cfg in sorted({t["config"] for t in tails}):
                fz0 = next((t for t in tails if t["config"] == cfg and not t.get("forceFlush")), None)
                fz1 = next((t for t in tails if t["config"] == cfg and t.get("forceFlush")), None)
                if not fz0 or not fz1:
                    o.p("- %s: %s lacks its FZ = 0 or FZ = 1 render: no verdict" % (r.name, cfg))
                    continue
                s0, s1 = fz0.get("tail"), fz1.get("tail")
                if not s0 or not s1 or "flaggedBlocks" not in s1:
                    o.p("- %s: %s has no silent-tail statistics (older firmware): no verdict" % (r.name, cfg))
                    continue
                ratio = s0["max"] / float(s1["max"]) if s1["max"] else float("inf")
                whole_ratio = fz0["max"] / float(fz1["max"]) if fz1["max"] else float("inf")
                census = 100.0 * s1["flaggedBlocks"] / float(s1["blocks"] or 1)
                census0 = 100.0 * (s0.get("flaggedBlocks") or 0) / float(s0["blocks"] or 1)
                if cfg.startswith("golden:") and gold:
                    same = fz1["hash"] == gold["hash"] and fz0["hash"] == gold["hash"]
                    same_text = "the FZ = 1 and FZ = 0 renders %s golden.json" % ("equal" if same else "DO NOT BOTH EQUAL")
                else:
                    same = fz1["hash"] == fz0["hash"]
                    same_text = "the FZ = 1 render %s the FZ = 0 render" % ("equals" if same else "DIFFERS FROM")
                keep = ratio <= RULE_RATIO and census <= RULE_FLAGGED_PCT and same
                o.p("- %s, %s: silent-tail worst block FZ=0 / FZ=1 = %.3f (rule: <= %.1f; whole run %.3f, context "
                    "only); silent-tail blocks raising IDC or UFC at FZ = 1: %.3f %% (rule: <= %.1f %%; at FZ = 0 %.3f %%, "
                    "which Armv7-M under-reports: IDC needs FZ = 1 and UFC an inexact tiny result); %s -> **%s**" % (
                        r.name, cfg, ratio, RULE_RATIO, whole_ratio, census, RULE_FLAGGED_PCT, census0, same_text,
                        "keep gradual underflow" if keep else "widen the deterministic flush and measure again"))
        if not hooks_runs:
            o.p("FZ = 1 and the flag census need brainscape_bench_hooks (firmware/README.md).")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="serial port of the Seed running a bench image (COM5, /dev/ttyACM0, ..., or auto)")
    ap.add_argument("--log", action="append", help="captured bench run(s); repeat to compare builds")
    ap.add_argument("--run", default="run", help='device command (default "run"; "run quick" skips the tails)')
    ap.add_argument("--save", help="write the raw stream to this file (with --port)")
    ap.add_argument("--golden", default=DEFAULT_GOLDEN)
    ap.add_argument("--markdown", help="also write the report to this file")
    ap.add_argument("--expect-archive", metavar="SHA256|FILE",
                    help="the engine archive(s) the device must run: a SHA-256 (the shipping archive), or the "
                         "firmware build's engine-archives.sha256 (both archives)")
    args = ap.parse_args()
    if bool(args.port) == bool(args.log):
        ap.error("give --port or --log")
    archives = load_expected_archives(args.expect_archive)
    runs = []
    if args.port:
        run = Run(args.save or args.port)
        port = hilserial.Port(args.port)
        try:
            if not read_lines(port, run, args.save, args.run):
                return 2
        finally:
            port.close()
        runs.append(run)
    else:
        for path in args.log:
            run = Run(os.path.basename(path))
            src = hilserial.LogSource(path)
            read_lines(src, run)
            src.close()
            runs.append(run)
    runs = [r for r in runs if r.lines or r.begin]
    if not runs:
        print("no bench data")
        return 2
    for r in runs:
        validate(r, archives)
    with open(args.golden, encoding="utf-8") as f:
        golden = json.load(f)
    o = Out()
    report(runs, golden, o)
    text = "\n".join(o.text) + "\n"
    print(text)
    if args.markdown:
        with open(args.markdown, "w", encoding="utf-8") as f:
            f.write(text)
    bad = [r for r in runs if r.problems]
    if bad:
        print("INCOMPLETE: %s" % "; ".join("%s (%d problem(s))" % (r.name, len(r.problems)) for r in bad))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
