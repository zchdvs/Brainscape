#!/usr/bin/env python3
"""Console for the Daisy Seed live-audio image (firmware/README.md). Stdlib only.

Sends the live image's text commands over USB serial and prints its replies readably:

  console.py --port auto                       # interactive: type commands, Ctrl-C to quit
  console.py --port auto list "preset 5" stats # one-shot: send each argument, print replies

Commands (one per line): info | list | params | get | preset N [exact] | set NAME VALUE |
freeze on|off | trigger | stats [reset] | dfu. NAME is a descriptor name (layer0.size_ms), a
ParamId (GrainSizeMs) or a short alias: delay mix feedback out size density spray pitch
spread reverse jitter sustain skew smooth pan modrate moddepth delaytime delayfb delaymix
reverbtime reverbmix cutoff res morph sens onset marks.

Other images answer "info" too, so the console also identifies whatever is flashed.
"""
import argparse
import os
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hilserial  # noqa: E402


def pretty(obj):
    kind = obj.get("type")
    if kind == "hello":
        return "\n".join(hilserial.describe_hello(obj))
    if kind == "stats":
        b = obj.get("budgetCycles") or 1
        return ("cpu: mean %.1f %%, peak %.1f %% of the 48-frame budget (%d / %d cycles, %d callbacks, %d over "
                "budget); onsets %d; preset %s%s; queue refused %d" % (
                    100.0 * obj["meanCycles"] / b, 100.0 * obj["peakCycles"] / b, obj["meanCycles"],
                    obj["peakCycles"], obj["callbacks"], obj["overBudget"], obj["onsets"], obj["preset"],
                    ", frozen" if obj.get("frozen") else "", obj["queueRefused"]))
    if kind == "presets":
        lines = ["presets:"]
        for p in obj["presets"]:
            lines.append("  %s%d  %s  (%s)" % ("*" if p["n"] == obj.get("current") else " ", p["n"], p["name"],
                                              p["source"]))
        return "\n".join(lines)
    if kind == "param":
        p = obj["param"]
        rng = " [%s .. %s, default %s] %s" % (p["min"], p["max"], p["default"], p.get("unit", "")) if "min" in p else ""
        return "  %-28s %-15s %s%s" % (p["name"], p["id"], p["value"], rng)
    if kind == "preset":
        extra = " in %d us (audio muted meanwhile)" % obj["microseconds"] if "microseconds" in obj else ""
        return "preset %d: %s (%s load%s)%s" % (obj["n"], obj["name"], obj["load"],
                                                 "" if obj.get("ok") else ", NOT exact", extra)
    if kind == "set":
        return "%s = %s (%s)" % (obj["name"], obj["value"], obj["bits"])
    if kind == "error":
        return "error: " + obj.get("message", "")
    return None


def reader(port, stop):
    while not stop.is_set():
        line = port.read_line(timeout=0.2)
        if line is None:
            continue
        obj = hilserial.parse(line)
        text = pretty(obj) if obj is not None else None
        print(text if text is not None else line, flush=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", required=True, help="serial port of the Seed (COM5, /dev/ttyACM0, ..., or auto)")
    ap.add_argument("commands", nargs="*", help="commands to send, then exit (default: interactive)")
    args = ap.parse_args()
    port = hilserial.Port(args.port, timeout=0.2)
    stop = threading.Event()
    thread = threading.Thread(target=reader, args=(port, stop), daemon=True)
    thread.start()
    try:
        if args.commands:
            for c in args.commands:
                port.write_line(c)
                time.sleep(0.4)
            time.sleep(0.6)
        else:
            port.write_line("info")
            print("connected to %s; type commands (help: list, params, preset N, set NAME VALUE, freeze on|off, "
                  "trigger, stats), Ctrl-C to quit" % args.port)
            for line in sys.stdin:
                if line.strip():
                    port.write_line(line.strip())
    except KeyboardInterrupt:
        pass
    finally:
        stop.set()
        thread.join(timeout=1.0)
        port.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
