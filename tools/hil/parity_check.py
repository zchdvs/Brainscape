#!/usr/bin/env python3
"""Silicon parity check: the golden corpus rendered on the Daisy Seed against golden.json.

Drives the firmware's parity image over USB serial (or reads a captured stream) and compares
every preset's whole-render SHA-256, per-second hashes and coverage counters, every vector's
input hash, and the header's sound revision, versions and engine configuration with
dsp/tests/golden/golden.json, as brainscape_golden --mode check does
(docs/design/determinism-profile.md §6.1, §6.6). Stdlib only.

  parity_check.py --port COM5 [--run "run"] [--save run.log]   # drive the device
  parity_check.py --log run.log                                # check a capture
  brainscape_parity_stream --quick | parity_check.py --log -   # host or qemu stream

The device command is "run" (the harness's configuration: 48-frame blocks, maxBlockSize 512,
a clean FP environment), optionally with "pedal" (maxBlockSize 48, the live engine's
configuration), "hostile" (FZ|DN and round toward zero in the caller's FPSCR), "quick" (no
long vectors) and "only=VECTOR[/PRESET],...". Exit status: 0 PASS, 1 FAIL (a difference, a
render failure, or presets missing from a whole-corpus run), 2 no usable stream.
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
HEADER_INTS = ("soundRevision", "generatorVersion", "corpusVersion", "sampleRate", "historyFrames")
HEADER_BOOLS = ("stereoInput", "ditherRingWrite")


def first_diff(a, b):
    for i, (x, y) in enumerate(zip(a, b)):
        if x != y:
            return i
    return -1 if len(a) == len(b) else min(len(a), len(b))


def collect(source, save, run_command, timeout):
    """Reads the stream into (hello, begin, vectors, presets, end); tees raw lines to `save`."""
    hello = begin = end = None
    vectors, presets = [], []
    out = open(save, "w", encoding="utf-8") if save else None
    if isinstance(source, hilserial.Port):
        source.drain()
        hello = hilserial.hello(source)
        if hello is None:
            print("no hello from %s: is the parity image running? (firmware/README.md)" % source.name)
            return None
        if hello.get("image") != "parity":
            print("%s runs the %s image, not parity" % (source.name, hello.get("image")))
            return None
        if out:
            out.write(json.dumps(hello) + "\n")
        source.write_line(run_command)
    last = time.monotonic()
    while True:
        line = source.read_line(timeout=1.0)
        if line is None:
            if isinstance(source, hilserial.LogSource):
                break
            if time.monotonic() - last > timeout:
                print("\nno data for %d s: giving up" % timeout)
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
        if kind == "hello" and hello is None:
            hello = obj
        elif kind == "parity-begin":
            begin = obj
            if isinstance(source, hilserial.Port):
                print("rendering on the device (the whole corpus takes several minutes)...")
        elif kind == "vector":
            vectors.append(obj)
        elif kind == "preset":
            presets.append(obj)
            if isinstance(source, hilserial.Port):
                print("  %s/%s rendered" % (obj.get("vector"), obj.get("name")), flush=True)
        elif kind == "parity-end":
            end = obj
            break
        elif kind == "error":
            print("device error: %s" % obj.get("message"))
    if out:
        out.close()
    return hello, begin, vectors, presets, end


def check(golden, begin, vectors, presets, end):
    diffs = []
    for key in HEADER_INTS + HEADER_BOOLS:
        if golden.get(key) != begin.get(key):
            diffs.append("header %s: device %r, golden %r" % (key, begin.get(key), golden.get(key)))
    gvec = {v["name"]: v for v in golden.get("vectors", [])}
    for v in vectors:
        g = gvec.get(v["name"])
        if g is None:
            diffs.append("%s: not in the golden file" % v["name"])
            continue
        for key in ("generatorVersion", "frames", "inputHash", "ringSizes"):
            if g.get(key) != v.get(key):
                diffs.append("%s: %s differs (device %r, golden %r)" % (v["name"], key, v.get(key), g.get(key)))
    for p in presets:
        name = "%s/%s" % (p["vector"], p["name"])
        g = next((x for x in gvec.get(p["vector"], {}).get("presets", []) if x["name"] == p["name"]), None)
        if g is None:
            diffs.append("%s: not in the golden file" % name)
            continue
        if not p.get("rendered", False):
            diffs.append("%s: the render failed on the device" % name)
            continue
        second = first_diff(g.get("secondHashes", []), p.get("secondHashes", []))
        if g.get("hash") != p.get("hash"):
            where = "from second %d" % second if second >= 0 else "(every per-second hash matches)"
            diffs.append("%s: hash differs %s" % (name, where))
        elif second >= 0:
            diffs.append("%s: per-second hashes differ from second %d" % (name, second))
        gc, pc = g.get("counters", {}), p.get("counters", {})
        for k in sorted(set(gc) | set(pc)):
            if gc.get(k) != pc.get(k):
                diffs.append("%s: counter %s = %r, golden %r" % (name, k, pc.get(k), gc.get(k)))
    whole = not begin.get("quick") and not begin.get("subset")
    if whole:
        seen = {(p["vector"], p["name"]) for p in presets}
        for v in golden.get("vectors", []):
            for p in v.get("presets", []):
                if (v["name"], p["name"]) not in seen:
                    diffs.append("%s/%s: in the golden file but not rendered" % (v["name"], p["name"]))
    if end is None:
        diffs.append("the stream ended without parity-end (incomplete run)")
    elif end.get("renderFailures"):
        diffs.append("%d render failure(s) on the device" % end["renderFailures"])
    return diffs


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="serial port of the Seed (COM5, /dev/ttyACM0, ...)")
    ap.add_argument("--log", help="a captured stream instead of a port ('-' for stdin)")
    ap.add_argument("--golden", default=DEFAULT_GOLDEN, help="golden file (default: %(default)s)")
    ap.add_argument("--run", default="run", help='device command (default "run"; see above)')
    ap.add_argument("--save", help="also write the raw stream to this file")
    ap.add_argument("--timeout", type=float, default=600.0,
                    help="seconds without data before giving up (default %(default)s)")
    args = ap.parse_args()

    with open(args.golden, encoding="utf-8") as f:
        golden = json.load(f)
    source = hilserial.open_source(args.port, args.log)
    try:
        got = collect(source, args.save, args.run, args.timeout)
    finally:
        source.close()
    if got is None:
        return 2
    hello, begin, vectors, presets, end = got
    for line in hilserial.describe_hello(hello):
        print(line)
    if begin is None:
        print("VERDICT: NO STREAM (no parity-begin line)")
        return 2

    clock = begin.get("clock", {})
    hz = clock.get("hz") or 0
    print("run: %s, blocks %s, maxBlockSize %s, %s, FP environment %s, tag %s" % (
        begin.get("format"), begin.get("blockPattern"), begin.get("maxBlockSize"), begin.get("start"),
        begin.get("fpEnv"), begin.get("tag")))
    et = begin.get("engineToolchain", {})
    print("engine: %s %s %s, fp flags %s" % (et.get("compiler"), et.get("version"), et.get("target"),
                                           et.get("fpFlagsHash")))
    diffs = check(golden, begin, vectors, presets, end)
    bad = {d.split(":")[0] for d in diffs}
    total_frames = total_cycles = 0
    print("%-46s %-16s %s" % ("preset", "sha256", "result" + ("  (x realtime)" if hz else "")))
    for p in presets:
        name = "%s/%s" % (p["vector"], p["name"])
        speed = ""
        if hz and p.get("cycles"):
            seconds = p["cycles"] / hz
            speed = "  %.2fx" % ((p.get("frames", 0) / 48000.0) / seconds) if seconds > 0 else ""
            total_frames += p.get("frames", 0)
            total_cycles += p["cycles"]
        print("%-46s %-16s %s%s" % (name, (p.get("hash") or "-")[:16], "MISMATCH" if name in bad else "ok", speed))
    for d in diffs:
        print("  - " + d)
    if hz and total_cycles:
        print("rendered %.0f s of audio in %.0f s (%.2fx realtime, %s at %d Hz)" % (
            total_frames / 48000.0, total_cycles / hz, (total_frames / 48000.0) / (total_cycles / hz),
            clock.get("name"), hz))
    scope = "subset" if begin.get("quick") or begin.get("subset") else "whole corpus"
    if diffs:
        print("VERDICT: FAIL - %d difference(s) from %s (sound revision %s, %s)" % (
            len(diffs), os.path.basename(args.golden), golden.get("soundRevision"), scope))
        return 1
    print("VERDICT: PASS - %d preset(s) match %s bit for bit (sound revision %s, %s)" % (
        len(presets), os.path.basename(args.golden), golden.get("soundRevision"), scope))
    return 0


if __name__ == "__main__":
    sys.exit(main())
