#!/usr/bin/env python3
"""Silicon parity check: the golden corpus rendered on the Daisy Seed against golden.json.

Drives the firmware's parity image over USB serial (or reads a captured stream) and compares
every preset's whole-render SHA-256, per-second hashes and coverage counters, every vector's
input hash, and the header's sound revision, versions and engine configuration with
dsp/tests/golden/golden.json, as brainscape_golden --mode check does
(docs/design/determinism-profile.md §6.1, §6.6). Stdlib only.

  parity_check.py --port auto [--run "run"] [--save run.log]   # drive the device (or COM5)
  parity_check.py --log run.log [--run "run pedal"]            # check a capture
  brainscape_parity_stream --quick | parity_check.py --log -   # host or qemu stream

The device command is "run" (the harness's configuration: 48-frame blocks, maxBlockSize 512,
a clean FP environment), optionally with "pedal" (maxBlockSize 48, the live engine's
configuration), "hostile" (FZ|DN and round toward zero in the caller's FPSCR), "quick" (no
long vectors) and "only=VECTOR[/PRESET],...". The stream's header must say it ran what the
command asked for (with --port always, with --log when --run is given).

The stream itself is checked as strictly as the hashes (format brainscape-parity-stream/2):
every line numbered without a gap, every line well formed, as many preset and vector lines
as parity-end counts, a vector line for every preset's vector, no "resync" notice (the
device's USB serial dropped a line), and from a device the "idle" line after parity-end with
droppedBytes 0. --expect-archive compares the engine archive the device reports with the
one the firmware build hashed (build/fw/firmware/engine-archives.sha256) or with a hash.

Exit status: 0 PASS; 1 FAIL (a difference from the golden file, a render failure, presets
missing from a whole-corpus run, a configuration other than the one asked for, or an engine
archive other than the expected one); 2 the stream is unusable: none, or incomplete or
damaged in transport, with no difference among what did arrive (rerun it).
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
STREAM_FORMAT = "brainscape-parity-stream/2"
HEADER_INTS = ("soundRevision", "generatorVersion", "corpusVersion", "sampleRate", "historyFrames")
HEADER_BOOLS = ("stereoInput", "ditherRingWrite")
VECTOR_KEYS = ("seq", "name", "generatorVersion", "frames", "inputHash", "ringSizes")
PRESET_KEYS = ("seq", "vector", "name", "rendered", "hash", "secondHashes", "counters")
END_KEYS = ("seq", "presets", "vectors", "lines", "renderFailures")
IDLE_WAIT = 10.0  # seconds to wait for the device's idle line after parity-end


class Stream:
    def __init__(self):
        self.hello = None
        self.begin = None
        self.end = None
        self.idle = None
        self.vectors = []
        self.presets = []
        self.seqs = []          # seq of every numbered line from parity-begin to parity-end
        self.garbled = []       # lines between parity-begin and parity-end that are not JSON objects
        self.malformed = []     # JSON objects missing a required key
        self.resyncs = []       # the device's USB serial dropped lines
        self.errors = []        # {"type":"error"} lines
        self.port_lost = None   # the serial port vanished (a reset?)


def first_diff(a, b):
    for i, (x, y) in enumerate(zip(a, b)):
        if x != y:
            return i
    return -1 if len(a) == len(b) else min(len(a), len(b))


def collect(source, save, run_command, timeout):
    """Reads the stream; tees raw lines to `save`. Returns a Stream, or None if no device."""
    st = Stream()
    out = open(save, "w", encoding="utf-8") if save else None
    device = isinstance(source, hilserial.Port)
    try:
        if device:
            source.drain()
            st.hello = hilserial.hello(source)
            if st.hello is None:
                print("no hello from %s: is the parity image running? (firmware/README.md)" % source.name)
                return None
            if st.hello.get("image") != "parity":
                print("%s runs the %s image, not parity" % (source.name, st.hello.get("image")))
                return None
            if out:
                out.write(json.dumps(st.hello) + "\n")
            source.write_line(run_command)
        last = time.monotonic()
        while True:
            wait = IDLE_WAIT if st.end is not None else timeout
            line = source.read_line(timeout=1.0)
            if line is None:
                if isinstance(source, hilserial.LogSource):
                    break
                if time.monotonic() - last > wait:
                    if st.end is None:
                        print("\nno data for %d s: giving up" % wait)
                    break
                continue
            last = time.monotonic()
            if out:
                out.write(line + "\n")
                out.flush()
            obj = hilserial.parse(line)
            inside = st.begin is not None and st.end is None
            if obj is None:
                if inside and line.strip():
                    st.garbled.append(line[:120])
                continue
            kind = obj.get("type")
            if kind == "hello" and st.hello is None:
                st.hello = obj
            elif kind == "resync":
                st.resyncs.append(obj)
            elif kind == "error":
                st.errors.append(obj.get("message"))
                print("device error: %s" % obj.get("message"))
            elif kind == "parity-begin":
                st.begin = obj
                st.seqs.append(obj.get("seq"))
                if device:
                    print("rendering on the device (the whole corpus takes several minutes)...")
            elif kind == "vector" and inside:
                st.seqs.append(obj.get("seq"))
                if all(k in obj for k in VECTOR_KEYS):
                    st.vectors.append(obj)
                else:
                    st.malformed.append(line[:120])
            elif kind == "preset" and inside:
                st.seqs.append(obj.get("seq"))
                if all(k in obj for k in PRESET_KEYS):
                    st.presets.append(obj)
                    if device:
                        print("  %s/%s rendered" % (obj.get("vector"), obj.get("name")), flush=True)
                else:
                    st.malformed.append(line[:120])
            elif kind == "parity-end" and inside:
                st.end = obj
                st.seqs.append(obj.get("seq"))
                if not all(k in obj for k in END_KEYS):
                    st.malformed.append(line[:120])
                if not device:
                    continue  # a capture: read it to the end (its idle line, if any)
            elif kind == "idle" and st.end is not None:
                st.idle = obj
                if device:
                    break
            elif inside:
                st.malformed.append(line[:120])
    except hilserial.PortGone as e:
        st.port_lost = str(e)
    finally:
        if out:
            out.close()
    return st


def expected_config(run_command):
    """What a device "run ..." command must produce in the header."""
    words = run_command.split()
    if not words or words[0] != "run":
        return None
    return {"maxBlockSize": 48 if "pedal" in words else 512,
            "fpEnv": "hostile" if "hostile" in words else "clean",
            "quick": "quick" in words,
            "subset": any(w.startswith("only=") for w in words)}


def load_expected_archives(spec):
    """--expect-archive: a hash, or a sha256sum-style file (engine-archives.sha256)."""
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


def check(golden, st, expect, archives):
    """(parity differences, transport problems, notes)."""
    diffs, transport, notes = [], [], []
    begin, end = st.begin, st.end
    if begin.get("format") != STREAM_FORMAT:
        transport.append("stream format %r, expected %r (an older parity image or harness?)" % (
            begin.get("format"), STREAM_FORMAT))
    for key in HEADER_INTS + HEADER_BOOLS:
        if golden.get(key) != begin.get(key):
            diffs.append("header %s: device %r, golden %r" % (key, begin.get(key), golden.get(key)))
    if expect is not None:
        for key, want in expect.items():
            got = begin.get(key)
            if got != want:
                diffs.append("run configuration %s: the stream says %r, the command asked for %r" % (key, got, want))
    # Provenance: the engine archive the device (or the stream) reports.
    if archives is not None:
        reported = ((st.hello or {}).get("build") or {}).get("engineArchiveSha256") or \
                   (begin.get("platform") or {}).get("engineArchiveSha256")
        want = archives.get("libbrainscape_dsp.a")
        if reported is None:
            diffs.append("engine archive: the stream does not report one, --expect-archive wants %s" % want)
        elif want is not None and reported.lower() != want:
            diffs.append("engine archive: the device runs %s, --expect-archive wants %s" % (reported, want))
    build = (st.hello or {}).get("build") or {}
    if build.get("dirty"):
        notes.append("the firmware was built from a tree with uncommitted changes (build.dirty)")

    # The stream's own integrity.
    if st.garbled:
        transport.append("%d line(s) between parity-begin and parity-end are not JSON objects, e.g. %r" % (
            len(st.garbled), st.garbled[0]))
    if st.malformed:
        transport.append("%d malformed line(s) (a required key missing), e.g. %r" % (len(st.malformed), st.malformed[0]))
    if st.resyncs:
        r = st.resyncs[-1]
        transport.append("the device's USB serial dropped %s byte(s) in %s line(s) (resync notices: %d)" % (
            r.get("droppedBytes"), r.get("droppedLines"), len(st.resyncs)))
    seqs = st.seqs
    if any(not isinstance(s, int) for s in seqs):
        transport.append("lines without a sequence number")
    else:
        gaps = [(a, b) for a, b in zip(seqs, seqs[1:]) if b != a + 1]
        if seqs and seqs[0] != 0:
            transport.append("the stream does not start at line 0 (starts at %d)" % seqs[0])
        if gaps:
            transport.append("line numbers jump %d time(s), first from %d to %d: lines were lost" % (
                len(gaps), gaps[0][0], gaps[0][1]))
    if end is None:
        transport.append("the stream ended without parity-end (incomplete run)")
    else:
        if end.get("presets") != len(st.presets):
            transport.append("parity-end counts %s preset(s), %d arrived" % (end.get("presets"), len(st.presets)))
        if end.get("vectors") != len(st.vectors):
            transport.append("parity-end counts %s vector(s), %d arrived" % (end.get("vectors"), len(st.vectors)))
        if isinstance(end.get("lines"), int) and isinstance(end.get("seq"), int) and end["lines"] != end["seq"]:
            transport.append("parity-end: lines %s, seq %s" % (end.get("lines"), end.get("seq")))
        if end.get("renderFailures"):
            diffs.append("%d render failure(s) on the device" % end["renderFailures"])
    device = (st.hello or {}).get("image") == "parity"
    if device:
        if st.idle is None:
            transport.append("no idle line after parity-end: the device's transport loss count is unknown")
        elif st.idle.get("droppedBytes") or st.idle.get("droppedLines"):
            transport.append("the device's USB serial dropped %s byte(s) in %s line(s) during the run" % (
                st.idle.get("droppedBytes"), st.idle.get("droppedLines")))
    if st.port_lost:
        transport.append("the serial port went away mid-run (%s): the device reset? Reconnect and run "
                         "`console.py --port auto info` for its lastFault" % st.port_lost)
    if st.errors:
        transport.append("device error(s): %s" % "; ".join(str(e) for e in st.errors))

    # Parity.
    gvec = {v["name"]: v for v in golden.get("vectors", [])}
    seen_vectors = set()
    for v in st.vectors:
        seen_vectors.add(v["name"])
        g = gvec.get(v["name"])
        if g is None:
            diffs.append("%s: not in the golden file" % v["name"])
            continue
        for key in ("generatorVersion", "frames", "inputHash", "ringSizes"):
            if g.get(key) != v.get(key):
                diffs.append("%s: %s differs (device %r, golden %r)" % (v["name"], key, v.get(key), g.get(key)))
    for vname in sorted({p["vector"] for p in st.presets} - seen_vectors):
        transport.append("%s: preset lines without their vector line (its input hash went unchecked)" % vname)
    for p in st.presets:
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
        seen = {(p["vector"], p["name"]) for p in st.presets}
        missing = ["%s/%s" % (v["name"], p["name"]) for v in golden.get("vectors", [])
                   for p in v.get("presets", []) if (v["name"], p["name"]) not in seen]
        if missing:
            # Lost in transport if the stream itself shows a loss, else never rendered.
            (transport if transport else diffs).append(
                "%d golden preset(s) missing from a whole-corpus run: %s" % (len(missing), ", ".join(missing[:6])))
    return diffs, transport, notes


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="serial port of the Seed (COM5, /dev/ttyACM0, ..., or auto)")
    ap.add_argument("--log", help="a captured stream instead of a port ('-' for stdin)")
    ap.add_argument("--golden", default=DEFAULT_GOLDEN, help="golden file (default: %(default)s)")
    ap.add_argument("--run", help='device command (default "run"; see above). With --log: the command the '
                                  "capture must have run")
    ap.add_argument("--save", help="also write the raw stream to this file")
    ap.add_argument("--expect-archive", metavar="SHA256|FILE",
                    help="the engine archive the device must run: a SHA-256, or the firmware build's "
                         "engine-archives.sha256")
    ap.add_argument("--timeout", type=float, default=600.0,
                    help="seconds without data before giving up (default %(default)s)")
    args = ap.parse_args()

    with open(args.golden, encoding="utf-8") as f:
        golden = json.load(f)
    archives = load_expected_archives(args.expect_archive)
    run_command = args.run or "run"
    expect = expected_config(run_command) if (args.port or args.run) else None
    if (args.port or args.run) and expect is None:
        ap.error('--run must be a "run ..." command')
    source = hilserial.open_source(args.port, args.log)
    try:
        st = collect(source, args.save, run_command, args.timeout)
    finally:
        source.close()
    if st is None:
        return 2
    for line in hilserial.describe_hello(st.hello):
        print(line)
    if st.begin is None:
        if st.port_lost:
            print("the serial port went away (%s): the device reset? Reconnect and run "
                  "`console.py --port auto info` for its lastFault" % st.port_lost)
        print("VERDICT: NO STREAM (no parity-begin line)")
        return 2

    begin = st.begin
    clock = begin.get("clock", {})
    hz = clock.get("hz") or 0
    print("run: %s, blocks %s, maxBlockSize %s, %s, FP environment %s, tag %s" % (
        begin.get("format"), begin.get("blockPattern"), begin.get("maxBlockSize"), begin.get("start"),
        begin.get("fpEnv"), begin.get("tag")))
    et = begin.get("engineToolchain", {})
    print("engine: %s %s %s, fp flags %s" % (et.get("compiler"), et.get("version"), et.get("target"),
                                           et.get("fpFlagsHash")))
    diffs, transport, notes = check(golden, st, expect, archives)
    bad = {d.split(":")[0] for d in diffs}
    total_frames = total_cycles = 0
    print("%-46s %-16s %s" % ("preset", "sha256", "result" + ("  (x realtime)" if hz else "")))
    for p in st.presets:
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
    for t in transport:
        print("  - transport: " + t)
    for n in notes:
        print("  - note: " + n)
    if hz and total_cycles:
        print("rendered %.0f s of audio in %.0f s (%.2fx realtime, %s at %d Hz)" % (
            total_frames / 48000.0, total_cycles / hz, (total_frames / 48000.0) / (total_cycles / hz),
            clock.get("name"), hz))
    scope = "subset" if begin.get("quick") or begin.get("subset") else "whole corpus"
    if diffs:
        print("VERDICT: FAIL - %d difference(s) listed above (against %s, sound revision %s, %s)%s" % (
            len(diffs), os.path.basename(args.golden), golden.get("soundRevision"), scope,
            "; the stream was also damaged in transport" if transport else ""))
        return 1
    if transport:
        print("VERDICT: FAIL (TRANSPORT) - the stream is incomplete or damaged (%d problem(s)); the %d preset(s) "
              "that arrived match %s, but this run proves nothing: rerun it" % (
                  len(transport), len(st.presets), os.path.basename(args.golden)))
        return 2
    print("VERDICT: PASS - %d preset(s) match %s bit for bit (sound revision %s, %s)" % (
        len(st.presets), os.path.basename(args.golden), golden.get("soundRevision"), scope))
    return 0


if __name__ == "__main__":
    sys.exit(main())
