#!/usr/bin/env python3
"""DWT measurement report for the Daisy Seed bench images (firmware/README.md). Stdlib only.

Drives the bench image over USB serial, or reads captured runs, and renders what the
determinism profile's hardware-gated decisions need (docs/design/determinism-profile.md §4.2,
§7, §8.3 Q1/Q2/Q5/Q9; grain-engine.md §8): cycles per 48-frame block against the 480 MHz /
48 kHz budget (10,000 cycles per sample), per-stage and per-birth costs, the Restart clear,
FPU subnormal latency at FZ = 0 and 1, the flush idiom's cost, and the silent-tail test with
its decision rule. Several logs (brainscape_bench, _xip, _hooks) are compared side by side.

  bench_report.py --port auto [--run "run"] [--save bench.log]
  bench_report.py --log bench.log [--log bench_xip.log --log bench_hooks.log] [--markdown out.md]
"""
import argparse
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hilserial  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_GOLDEN = os.path.normpath(os.path.join(HERE, "..", "..", "dsp", "tests", "golden", "golden.json"))
FLUSH_SITES_PER_SAMPLE = 45  # determinism-profile.md §4.3 (derived count, record §4.3)


class Run:
    def __init__(self, name):
        self.name = name
        self.hello = None
        self.begin = None
        self.lines = []

    def of(self, kind):
        return [o for o in self.lines if o.get("type") == kind]

    @property
    def label(self):
        b = self.begin or {}
        h = self.hello or {}
        target = b.get("target") or h.get("target") or self.name
        return "%s (engine code %s%s)" % (target, b.get("engineCode") or h.get("engineCode", "?"),
                                          ", hooks" if b.get("hooks") or h.get("hooks") else "")


def read_lines(source, run, save=None, command=None, timeout=900.0):
    out = open(save, "w", encoding="utf-8") if save else None
    if isinstance(source, hilserial.Port):
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
            continue
        kind = obj.get("type")
        if kind == "hello":
            run.hello = run.hello or obj
        elif kind == "bench-begin":
            run.begin = obj
        elif kind == "error":
            print("device error: %s" % obj.get("message"))
        run.lines.append(obj)
        if isinstance(source, hilserial.Port) and kind in ("blocks", "stage", "births", "tail", "restart"):
            print("  %s %s %s" % (kind, obj.get("config") or obj.get("op"), obj.get("cache", "")), flush=True)
        if kind == "bench-end":
            break
    if out:
        out.close()
    return True


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


def report(runs, golden, o):
    first = runs[0]
    budget = (first.begin or {}).get("budgetCyclesPerBlock", 480000)
    hz = (first.begin or {}).get("clockHz", 480000000)
    o.p("# Brainscape DWT measurement pass (Daisy Seed)")
    o.p()
    for r in runs:
        b = r.begin or {}
        o.p("- %s: archive `%s`, firmware %s, clock %s Hz, measurement overhead %s cycles" % (
            r.label, b.get("engineArchiveSha256", "?")[:16], b.get("firmwareCommit", "?"), b.get("clockHz"),
            b.get("measurementOverhead")))
        for line in hilserial.describe_hello(r.hello):
            o.p("  " + line.strip())
    o.p()
    o.p("Budget: %d cycles per 48-frame block = 10,000 cycles per sample (480 MHz / 48 kHz). "
        "Interrupts are off around each measured call." % budget)

    mem = first.of("memory")
    if mem:
        m = mem[0]
        o.h("Memory (grain-engine.md §7)")
        o.table(["tier", "PlanMemory (maxBlockSize 48)", "arena", "where"], [
            ["Hot", m["hotBytes"], m["hotArena"], "DTCM"],
            ["Warm", m["warmBytes"], m["warmArena"], "AXI SRAM (D1)"],
            ["Bulk", m["bulkBytes"], m["bulkArena"], "SDRAM"],
            ["Engine object", m["engineBytes"], m["engineSlot"], "DTCM"]])
        mp = m.get("map", {})
        rows = [[k, v.get("start", ""), v.get("bytes", "")] for k, v in mp.items() if isinstance(v, dict)]
        o.p()
        o.table(["linker section", "start", "bytes"], rows)

    micro = first.of("micro")
    if micro:
        o.h("FPU latency, normal vs subnormal (profile §4.2 test a)")
        by = {}
        for m in micro:
            by.setdefault(m["name"], {})[m["fz"]] = m["cycles"] / float(m["instructions"])
        o.table(["dependent chain", "cycles/instr FZ=0", "cycles/instr FZ=1"],
                [[k, "%.2f" % v.get(0, float("nan")), "%.2f" % v.get(1, float("nan"))] for k, v in by.items()])
        normal = by.get("vmul normal", {}).get(0)
        sub = by.get("vmul subnormal operand and result", {}).get(0)
        under = by.get("vmul pair, every other result subnormal (underflow)", {}).get(0)
        pair = by.get("vmul pair, normal throughout", {}).get(0)
        if normal and sub and under and pair:
            o.p()
            o.p("Subnormal penalty at FZ = 0: vmul with subnormal operands %.2fx the normal latency; "
                "an underflowing vmul pair %.2fx a normal pair." % (sub / normal, under / pair))
    flush = {f["form"]: f["cycles"] / float(f["updates"]) for f in first.of("flush-micro")}
    if flush:
        base = flush.get("none", 0.0)
        o.h("The deterministic flush idiom (profile §4.3, §8.3 Q5)")
        rows = []
        for form, c in flush.items():
            extra = c - base
            rows.append([form, "%.2f" % c, "%.2f" % extra,
                         "%.0f" % (extra * FLUSH_SITES_PER_SAMPLE), pct(extra * FLUSH_SITES_PER_SAMPLE, 10000)])
        o.table(["form", "cycles/update", "flush cost/site", "x %d sites/sample" % FLUSH_SITES_PER_SAMPLE,
                 "of budget"], rows)
        o.p()
        o.p("Measured on 8 independent one-poles per step; the site count (%d per sample) is the profile's "
            "derived figure, so the per-sample total is an estimate from a measured per-site cost." %
            FLUSH_SITES_PER_SAMPLE)

    rs = first.of("restart")
    if rs:
        o.h("Restart and loads (profile §5.8, §8.3 Q9)")
        o.table(["operation", "state", "cycles", "ms"],
                [[x["op"], x["state"], x["cycles"], "%.2f" % (1000.0 * x["cycles"] / hz)] for x in rs])
        restart = next((x for x in rs if x["op"] == "Restart" and x["state"].startswith("after")), None)
        if restart:
            ms = 1000.0 * restart["cycles"] / hz
            o.p()
            o.p("A dirty Restart takes %.1f ms = %.0f audio blocks of 1 ms: an Exact load mutes the wet path that "
                "long (the profile estimated 45-160 ms)." % (ms, ms))

    def blocks_table(kind, title):
        rows = []
        for r in runs:
            for b in r.of(kind):
                rows.append([b["config"], b.get("cache", ""), r.label if len(runs) > 1 else "",
                             per_sample(b["mean"]), pct(b["mean"], budget), per_sample(b["p99"]), pct(b["p99"], budget),
                             per_sample(b["max"]), pct(b["max"], budget), b["maxBlock"],
                             "" if b.get("exactLoad", True) else "INEXACT LOAD"])
        if rows:
            o.h(title)
            o.table(["config", "cache", "build", "mean c/smp", "mean", "p99 c/smp", "p99", "max c/smp", "max",
                     "max at block", ""], rows)
        return rows

    blocks_table("blocks", "Cycles per 48-frame block (grain-engine.md §8, profile §7.2)")
    worst = [b for b in first.of("blocks") if b["config"].startswith("pess")]
    if worst:
        w = max(worst, key=lambda b: b["max"])
        o.p()
        o.p("Worst pessimistic block: %s, %s cache: %s of the budget (profile §7.2 estimated 77-78 %% for the "
            "pessimistic row, contraction off)." % (w["config"], w["cache"], pct(w["max"], budget)))
        hashes = {}
        for b in first.of("blocks"):
            hashes.setdefault(b["config"], set()).add(b["hash"])
        diverged = [k for k, v in hashes.items() if len(v) > 1]
        o.p("Warm and cold renders of each configuration %s." % (
            "produced identical output" if not diverged else "DIFFER for " + ", ".join(diverged)))

    stages = first.of("stage")
    if stages:
        base = next((s for s in stages if s["config"].endswith("all stages")), None)
        o.h("Per-stage cost by difference (pessimistic configuration, warm cache)")
        rows = []
        for s in stages:
            if base is None or s is base:
                rows.append([s["config"], per_sample(s["mean"]), per_sample(s["max"]), "", ""])
                continue
            rows.append([s["config"], per_sample(s["mean"]), per_sample(s["max"]),
                         per_sample(base["mean"] - s["mean"]), per_sample(base["max"] - s["max"])])
        o.table(["configuration", "mean c/smp", "max c/smp", "stage mean c/smp", "stage max c/smp"], rows)
        o.p()
        o.p("A stage's cost is the drop when it alone is switched off (exact bypass for the post stages). "
            "The onset detector has no bypass and is inside every figure.")

    births = first.of("births")
    if births:
        o.h("Per-birth cost at the maximum birth rate (grain-engine.md §8 ScheduleGrain row, §8.3 Q2)")
        by = {}
        for b in births:
            by.setdefault(b["voices"], {})[b["grainFrames"]] = b
        rows = []
        for voices, d in sorted(by.items(), reverse=True):
            fast, slow = d.get(48), d.get(960)
            if not fast or not slow:
                continue
            db = (fast["birthsPerBlockX1000"] - slow["birthsPerBlockX1000"]) / 1000.0
            per_birth = (fast["mean"] - slow["mean"]) / db if db else float("nan")
            rate = fast["birthsPerBlockX1000"] / 1000.0
            rows.append([voices, "%.1f" % rate, "%.1f" % (slow["birthsPerBlockX1000"] / 1000.0),
                         per_sample(fast["mean"]), per_sample(slow["mean"]), "%.0f" % per_birth,
                         "%.0f" % (per_birth * rate / 48.0)])
        o.table(["voices", "births/block (1 ms)", "births/block (20 ms)", "mean c/smp 1 ms", "mean c/smp 20 ms",
                 "cycles per birth", "birth cost c/smp at 1 ms"], rows)

    tails = [t for r in runs for t in r.of("tail")]
    if tails:
        o.h("Silent tails and the denormal decision (profile §4.2 test b)")
        rows = []
        for r in runs:
            for t in r.of("tail"):
                flagged = t.get("idcBlocks"), t.get("ufcBlocks")
                rows.append([t["config"], "FZ=1" if t.get("forceFlush") else "FZ=0", r.label if len(runs) > 1 else "",
                             t["blocks"], per_sample(t["max"]), pct(t["max"], budget), per_sample(t["p99"]),
                             "-" if flagged[0] is None else "%d / %d" % flagged, t["hash"][:16]])
        o.table(["tail", "flush", "build", "blocks", "max c/smp", "max", "p99 c/smp", "IDC / UFC blocks",
                 "sha256"], rows)
        gold = None
        for v in golden.get("vectors", []):
            if v["name"] == "strums_tail_123s":
                gold = next((p for p in v["presets"] if p["name"] == "tail_post_fb"), None)
        o.p()
        for t in tails:
            if t["config"].startswith("golden:") and gold:
                o.p("- %s at %s: %s golden.json" % (t["config"], "FZ=1" if t.get("forceFlush") else "FZ=0",
                                                     "MATCHES" if t["hash"] == gold["hash"] else "DOES NOT MATCH"))
        hooks = [t for t in tails if t.get("idcBlocks") is not None]
        for cfg in sorted({t["config"] for t in hooks}):
            fz0 = next((t for t in hooks if t["config"] == cfg and not t.get("forceFlush")), None)
            fz1 = next((t for t in hooks if t["config"] == cfg and t.get("forceFlush")), None)
            if not fz0 or not fz1:
                continue
            ratio = fz0["max"] / float(fz1["max"]) if fz1["max"] else float("inf")
            flagged = max(fz0["idcBlocks"], fz0["ufcBlocks"]) / float(fz0["blocks"]) * 100.0
            same = fz0["hash"] == fz1["hash"]
            keep = ratio <= 1.2 and flagged <= 0.5 and same
            o.p("- %s: worst block FZ=0 / FZ=1 = %.3f (rule: <= 1.2), blocks raising a subnormal flag %.3f %% "
                "(rule: <= 0.5 %%), FZ=1 render %s the FZ=0 render -> %s" % (
                    cfg, ratio, flagged, "equals" if same else "DIFFERS FROM",
                    "keep gradual underflow" if keep else "widen the deterministic flush and measure again"))
        if not hooks:
            o.p("FZ = 1 and the flag census need brainscape_bench_hooks (firmware/README.md).")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="serial port of the Seed running a bench image (COM5, /dev/ttyACM0, ..., or auto)")
    ap.add_argument("--log", action="append", help="captured bench run(s); repeat to compare builds")
    ap.add_argument("--run", default="run", help='device command (default "run"; "run quick" skips the tails)')
    ap.add_argument("--save", help="write the raw stream to this file (with --port)")
    ap.add_argument("--golden", default=DEFAULT_GOLDEN)
    ap.add_argument("--markdown", help="also write the report to this file")
    args = ap.parse_args()
    if bool(args.port) == bool(args.log):
        ap.error("give --port or --log")
    runs = []
    if args.port:
        run = Run(args.port)
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
    runs = [r for r in runs if r.lines]
    if not runs:
        print("no bench data")
        return 2
    with open(args.golden, encoding="utf-8") as f:
        golden = json.load(f)
    o = Out()
    report(runs, golden, o)
    text = "\n".join(o.text) + "\n"
    print(text)
    if args.markdown:
        with open(args.markdown, "w", encoding="utf-8") as f:
            f.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
