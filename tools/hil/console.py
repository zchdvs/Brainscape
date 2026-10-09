#!/usr/bin/env python3
"""Console for the Daisy Seed live-audio image (firmware/README.md). Stdlib only.

Sends the live image's text commands over USB serial and prints its replies readably:

  console.py --port auto                       # interactive: type commands, Ctrl-C to quit
  console.py --port auto list "preset 5" stats # one-shot: send each argument, print replies

Commands (one per line): info | list | params | get | preset N [exact|cut] | set NAME VALUE |
onset on|off | marks on|off | macro NAME POS | expression POS | freeze on|off | trigger |
stats [reset] | dfu. NAME is a descriptor name (layer0.size_ms), a ParamId (GrainSizeMs) or a
short alias: delay mix feedback trim out size density spray pitch transpose spread reverse
jitter sustain skew smooth pan modrate moddepth delaytime delayfb delaymix reverbtime reverbmix
cutoff res morph sens repeat decay voices skip burst spacing volume. Since sound revision 2 onset grains and mark positioning are mode
structure: "onset" and "marks" load a mode (so do "set onset V" and "set marks V", V >= 0.5 on);
macro NAME is activity, repeats, shape, time, space, filter, aux1 or aux2.

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
                "budget); onsets %d; mode switches %s; preset %s%s%s; queue refused %d" % (
                    100.0 * obj["meanCycles"] / b, 100.0 * obj["peakCycles"] / b, obj["meanCycles"],
                    obj["peakCycles"], obj["callbacks"], obj["overBudget"], obj["onsets"],
                    obj.get("modeSwitches", "?"), obj["preset"], structure(obj),
                    ", frozen" if obj.get("frozen") else "", obj["queueRefused"]))
    if kind == "presets":
        lines = ["presets:"]
        for p in obj["presets"]:
            lines.append("  %s%d  %s  (%s)%s" % ("*" if p["n"] == obj.get("current") else " ", p["n"], p["name"],
                                                p["source"], structure(p)))
        return "\n".join(lines)
    if kind == "mode":
        return "mode: onset %s, marks %s (from %s, %s load)" % (
            "on" if obj.get("onset") else "off", "on" if obj.get("marks") else "off", obj.get("from"),
            obj.get("load"))
    if kind == "macro":
        return "%s -> %s (%s leaf/leaves set)" % (obj["name"], obj["position"], obj["leaves"])
    if kind == "expression":
        return "expression -> %s (%s leaf/leaves set)" % (obj["position"], obj["leaves"])
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
    if kind == "resync":
        return "(the device's USB serial dropped %s line(s) so far)" % obj.get("droppedLines")
    if kind == "rebooting":
        return "rebooting into the %s" % obj.get("to", "bootloader")
    return None


def structure(obj):
    """The mode structure a reply reports (sound revision 2), as text."""
    parts = [name for name in ("onset", "marks") if obj.get(name)]
    return "" if "onset" not in obj else ", " + ("+".join(parts) if parts else "default mode")


def reader(port, stop):
    while not stop.is_set():
        try:
            line = port.read_line(timeout=0.2)
        except hilserial.PortGone:
            print("the serial port went away (the device rebooted, reset after a fault, or was unplugged); "
                  "reconnect and send info: its lastFault says why", flush=True)
            stop.set()
            return
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
            print("connected to %s; type commands (help: list, params, preset N, set NAME VALUE, onset|marks on|off, "
                  "macro NAME POS, expression POS, freeze on|off, trigger, stats), Ctrl-C to quit" % args.port)
            for line in sys.stdin:
                if line.strip():
                    port.write_line(line.strip())
    except (KeyboardInterrupt, hilserial.PortGone):
        pass
    finally:
        stop.set()
        thread.join(timeout=1.0)
        port.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
