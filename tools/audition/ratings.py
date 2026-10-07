#!/usr/bin/env python3
"""The factory audition's ratings log and its carry-forward (docs/design/mode-compiler.md §11.3).

The log is one JSON file (by default firmware/factory/AUDITION.json) with AUDITION.md generated
beside it. It holds one row per preset, keyed by the preset `id`:

  * what the mode declares for its audition: its input class (attack or pad), whether it is meant
    to self-oscillate, whether it is documented as needing attacks. `bspc render --declarations`
    reads them from this file;
  * the owner's rating: one chord sounds finished (yes or no), each knob 1-5, the level against
    bypass, keep, revise or drop, notes; recorded with the sound_rev, sound_hash and the hash of
    every S0-S11 render the rating was made on (with its per-second hashes, shortened), and
    whether the pre-screen had failed.

After a sound-revision bump or a re-stamp, `refresh` re-renders S0-S11 with `bspc render` and
`carry` compares: a rating whose renders all kept their hashes carries forward (its sound_rev and
sound_hash move to the new ones and the move is recorded); any other row is marked "re-listen",
naming each changed render and its first differing second. Renders the rating never heard (S11
against a mode added to the set since) do not break the carry; they are listed as new.

  ratings.py init     [--log LOG]
  ratings.py declare  [--log LOG] ID [--class attack|pad] [--self-oscillating | --no-self-oscillating]
                                     [--needs-attacks | --no-needs-attacks]
  ratings.py rate     [--log LOG] --renders DIR ID --chord yes|no
                      --knob activity=4 --knob repeats=3 ... --level TEXT --verdict keep|revise|drop
                      [--notes TEXT]
  ratings.py carry    [--log LOG] --renders DIR [--check]
  ratings.py refresh  [--log LOG] --renders DIR --bspc BSPC [--no-wav] DOCUMENT...
  ratings.py exit     [--log LOG] [--renders DIR]
  ratings.py md       [--log LOG] [--check]

Exit codes: 0 success; 1 a check failed (carry --check: a row would change; md --check: AUDITION.md
is stale; exit: the criteria are not met); 2 usage or input errors. Standard library only.
"""
import argparse
import hashlib
import json
import os
import subprocess
import sys

FORMAT = "brainscape-ratings/1"
INDEX_FORMAT = "brainscape-audition-index/1"
DEFAULT_LOG = os.path.join("firmware", "factory", "AUDITION.json")
SCRIPTS = ["S%d" % i for i in range(12)]
KNOBS = ["activity", "repeats", "shape", "time", "space", "filter"]
FAMILIES = ["recall", "reverie", "misfire", "echoic"]
VERDICTS = ["keep", "revise", "drop"]
SECOND_DIGITS = 8  # each second's hash is kept to 8 hex digits: enough to find the first change
# §11.3's exit: at least 10 modes kept, one per family, every knob rated 3 or more, no objective
# failure.
EXIT_KEEPS = 10
EXIT_KNOB_MIN = 3


class UsageError(Exception):
    pass


# ── The log ─────────────────────────────────────────────────────────────────────────────────

def empty_log():
    return {"format": FORMAT, "presets": {}}


def load_log(path):
    if not os.path.exists(path):
        raise UsageError("%s does not exist (ratings.py init)" % path)
    with open(path, encoding="utf-8") as f:
        log = json.load(f)
    if log.get("format") != FORMAT or not isinstance(log.get("presets"), dict):
        raise UsageError("%s is not a %s log" % (path, FORMAT))
    return log


def dump_json(obj):
    return json.dumps(obj, indent=2, sort_keys=False, ensure_ascii=False) + "\n"


def save_log(path, log):
    log["presets"] = dict(sorted(log["presets"].items()))
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(dump_json(log))
    with open(md_path(path), "w", encoding="utf-8", newline="\n") as f:
        f.write(render_md(log))


def md_path(log_path):
    return os.path.join(os.path.dirname(log_path), "AUDITION.md")


def new_row():
    return {
        "name": "",
        "family": "none",
        "declare": {"class": "attack", "self_oscillating": False, "needs_attacks": False},
        "status": "unrated",
        "rating": None,
        "relisten": [],
        "new_renders": [],
    }


def row_of(log, preset_id):
    return log["presets"].setdefault(preset_id, new_row())


# ── Render indexes (bspc render's DIR/<id>/audition.json) ───────────────────────────────────

def load_index(renders, preset_id):
    path = os.path.join(renders, preset_id, "audition.json")
    if not os.path.exists(path):
        return None
    with open(path, encoding="utf-8") as f:
        index = json.load(f)
    if index.get("format") != INDEX_FORMAT:
        raise UsageError("%s is not a %s file" % (path, INDEX_FORMAT))
    return index


def indexes(renders):
    out = {}
    if not os.path.isdir(renders):
        raise UsageError("%s is not a directory" % renders)
    for name in sorted(os.listdir(renders)):
        index = load_index(renders, name)
        if index is not None and index["preset"]["id"]:
            out[index["preset"]["id"]] = index
    return out


def render_records(index):
    """{render name: {"hash", "seconds"}} for the renders a rating is made on."""
    out = {}
    for r in index["renders"]:
        if r["role"] == "determinism" or not r["ok"]:
            continue
        out[r["name"]] = {
            "script": r["script"],
            "hash": r["hash"],
            "seconds": "".join(h[:SECOND_DIGITS] for h in r["seconds"]),
        }
    return out


def scripts_present(index):
    present = set(index["scripts"].keys())
    for s in index.get("skipped", []):
        present.add(s.split(":", 1)[0])
    return present


def prescreen_failed(index):
    p = index.get("prescreen", {})
    if not p.get("ran"):
        return None
    return bool(p.get("failed"))


def first_difference(old, new):
    """The first second whose hash differs, from two concatenated 8-digit lists."""
    a = [old[i:i + SECOND_DIGITS] for i in range(0, len(old), SECOND_DIGITS)]
    b = [new[i:i + SECOND_DIGITS] for i in range(0, len(new), SECOND_DIGITS)]
    for k in range(min(len(a), len(b))):
        if a[k] != b[k]:
            return k
    return min(len(a), len(b))


def script_order(script, name):
    return (SCRIPTS.index(script) if script in SCRIPTS else len(SCRIPTS), name)


def renders_digest(records):
    """12 hex digits naming a rating's render set, for the table."""
    text = "".join("%s %s\n" % (name, records[name]["hash"]) for name in sorted(records))
    return hashlib.sha256(text.encode("utf-8")).hexdigest()[:12]


# ── The carry-forward ───────────────────────────────────────────────────────────────────────

def carry(log, found):
    """Applies the carry-forward to `log` from the indexes in `found`; returns the change lines."""
    changes = []
    for pid, index in found.items():
        row = row_of(log, pid)
        if not row["name"]:
            changes.append("%s: new row (unrated)" % pid)
        row["name"] = index["preset"]["name"]
        row["family"] = index["preset"]["family"]
    for pid in sorted(log["presets"]):
        row = log["presets"][pid]
        rating = row.get("rating")
        if rating is None:
            continue
        index = found.get(pid)
        if index is None:
            changes.append("%s: not rendered; the rating stands unchecked" % pid)
            continue
        new = render_records(index)
        relisten = []
        for name in sorted(rating["renders"], key=lambda n: script_order(rating["renders"][n]["script"], n)):
            old = rating["renders"][name]
            cur = new.get(name)
            if cur is None:
                relisten.append({"render": name, "script": old["script"], "from_second": 0, "why": "gone"})
            elif cur["hash"] != old["hash"]:
                relisten.append({"render": name, "script": old["script"],
                                 "from_second": first_difference(old["seconds"], cur["seconds"]),
                                 "why": "changed"})
        added = sorted(n for n in new if n not in rating["renders"])
        before = (row["status"], row["relisten"], row["new_renders"], rating["sound_rev"], rating["sound_hash"])
        if relisten:
            row["status"] = "re-listen"
            row["relisten"] = relisten
        else:
            row["status"] = "rated"
            row["relisten"] = []
            rev, sh = index["preset"]["soundRev"], index["preset"]["soundHash"]
            if (rev, sh) != (rating["sound_rev"], rating["sound_hash"]):
                rating.setdefault("carried", []).append(
                    {"from_rev": rating["sound_rev"], "from_hash": rating["sound_hash"],
                     "to_rev": rev, "to_hash": sh})
                rating["sound_rev"], rating["sound_hash"] = rev, sh
        row["new_renders"] = added
        after = (row["status"], row["relisten"], row["new_renders"], rating["sound_rev"], rating["sound_hash"])
        if before != after:
            if relisten:
                changes.append("%s: re-listen: %s" % (pid, relisten_text(relisten)))
            else:
                changes.append("%s: carried forward to sound_rev %d%s" % (
                    pid, rating["sound_rev"], ", %d new renders" % len(added) if added else ""))
    return changes


def relisten_text(relisten, shown=3):
    parts = ["%s from %d s%s" % (r["render"], r["from_second"], " (gone)" if r["why"] == "gone" else "")
             for r in relisten[:shown]]
    if len(relisten) > shown:
        parts.append("and %d more" % (len(relisten) - shown))
    return "; ".join(parts)


# ── The exit criteria ───────────────────────────────────────────────────────────────────────

def exit_report(log, found=None):
    """(met, lines): §11.3's exit criteria over the log (and the latest pre-screens, if given)."""
    keeps = []
    for pid, row in sorted(log["presets"].items()):
        r = row.get("rating")
        if r is not None and row["status"] == "rated" and r["verdict"] == "keep":
            keeps.append((pid, row, r))
    lines = []
    ok_count = len(keeps) >= EXIT_KEEPS
    lines.append("%s %d of at least %d modes kept" % ("ok  " if ok_count else "MISS", len(keeps), EXIT_KEEPS))
    families = {f: [pid for pid, row, _ in keeps if row["family"] == f] for f in FAMILIES}
    ok_fam = all(families[f] for f in FAMILIES)
    lines.append("%s a keeper in every family: %s" % ("ok  " if ok_fam else "MISS", ", ".join(
        "%s %d" % (f, len(families[f])) for f in FAMILIES)))
    low = ["%s %s %d" % (pid, k, r["knobs"].get(k, 0)) for pid, _, r in keeps for k in KNOBS
           if r["knobs"].get(k, 0) < EXIT_KNOB_MIN]
    lines.append("%s every kept mode's knobs rated %d or more%s" % (
        "ok  " if not low else "MISS", EXIT_KNOB_MIN, ": " + ", ".join(low) if low else ""))
    failing = []
    for pid, _, r in keeps:
        failed = r.get("prescreen_failed")
        if found is not None and pid in found:
            latest = prescreen_failed(found[pid])
            failed = latest if latest is not None else failed
        if failed is None or failed:
            failing.append(pid + ("" if failed else " (no pre-screen)"))
    lines.append("%s no objective failure among the kept%s" % (
        "ok  " if not failing else "MISS", ": " + ", ".join(failing) if failing else ""))
    relisten = sorted(pid for pid, row in log["presets"].items() if row["status"] == "re-listen")
    if relisten:
        lines.append("note %d rows to re-listen: %s" % (len(relisten), ", ".join(relisten)))
    met = ok_count and ok_fam and not low and not failing
    return met, lines


# ── AUDITION.md ─────────────────────────────────────────────────────────────────────────────

def cell(text):
    return str(text).replace("|", "\\|").replace("\n", " ")


def render_md(log):
    out = ["# Factory audition log", "",
           "Generated from `AUDITION.json` by `tools/audition/ratings.py` (docs/design/mode-compiler.md",
           "§11.3); change it through the script (`declare`, `rate`, `carry`), not by hand. Ratings are",
           "keyed by preset id and the S0-S11 render hashes they were made on; *Renders* names that set.",
           "Knobs are rated 1-5.", ""]
    head = ["Preset", "Family", "Class", "Status", "sound_rev", "sound_hash", "Renders", "Chord"] + \
           [k.capitalize() for k in KNOBS] + ["Level", "Verdict", "Notes"]
    out.append("| " + " | ".join(head) + " |")
    out.append("|" + "---|" * len(head))
    for pid, row in sorted(log["presets"].items()):
        d = row["declare"]
        cls = d["class"] + (", self-osc." if d["self_oscillating"] else "") + \
            (", needs attacks" if d["needs_attacks"] else "")
        r = row.get("rating")
        if r is None:
            cells = [pid, row["family"], cls, row["status"], "", "", "", ""] + [""] * len(KNOBS) + ["", "", ""]
        else:
            cells = [pid, row["family"], cls, row["status"], r["sound_rev"], r["sound_hash"][:12],
                     renders_digest(r["renders"]), "yes" if r["chord_finished"] else "no"] + \
                    [r["knobs"].get(k, "") for k in KNOBS] + [r["level"], r["verdict"], r.get("notes", "")]
        out.append("| " + " | ".join(cell(c) for c in cells) + " |")
    rel = [(pid, row) for pid, row in sorted(log["presets"].items()) if row["status"] == "re-listen"]
    if rel:
        out += ["", "## Re-listen", ""]
        for pid, row in rel:
            out.append("- `%s`: %s" % (pid, relisten_text(row["relisten"], shown=len(row["relisten"]))))
    new = [(pid, row) for pid, row in sorted(log["presets"].items()) if row.get("new_renders")]
    if new:
        out += ["", "## Renders the ratings have not heard", ""]
        for pid, row in new:
            out.append("- `%s`: %s" % (pid, ", ".join(row["new_renders"])))
    met, lines = exit_report(log)
    out += ["", "## Exit criteria", "", "Met." if met else "Not met.", ""]
    out += ["    " + line for line in lines]
    return "\n".join(out) + "\n"


# ── Commands ────────────────────────────────────────────────────────────────────────────────

def cmd_init(a):
    if os.path.exists(a.log):
        raise UsageError("%s exists" % a.log)
    os.makedirs(os.path.dirname(a.log) or ".", exist_ok=True)
    save_log(a.log, empty_log())
    print("wrote %s and %s" % (a.log, md_path(a.log)))
    return 0


def cmd_declare(a):
    log = load_log(a.log)
    row = row_of(log, a.id)
    d = row["declare"]
    if a.input_class:
        d["class"] = a.input_class
    if a.self_oscillating is not None:
        d["self_oscillating"] = a.self_oscillating
    if a.needs_attacks is not None:
        d["needs_attacks"] = a.needs_attacks
    save_log(a.log, log)
    print("%s: %s" % (a.id, json.dumps(d)))
    return 0


def parse_knobs(items):
    knobs = {}
    for item in items or []:
        if "=" not in item:
            raise UsageError("--knob takes NAME=RATING, not %r" % item)
        k, v = item.split("=", 1)
        if k not in KNOBS:
            raise UsageError("unknown knob %r (%s)" % (k, ", ".join(KNOBS)))
        if k in knobs:
            raise UsageError("knob %s rated twice" % k)
        try:
            n = int(v)
        except ValueError:
            raise UsageError("knob %s: %r is not 1-5" % (k, v))
        if not 1 <= n <= 5:
            raise UsageError("knob %s: %d is not 1-5" % (k, n))
        knobs[k] = n
    return knobs


def cmd_rate(a):
    log = load_log(a.log)
    index = load_index(a.renders, a.id)
    if index is None:
        raise UsageError("no renders of %s in %s (bspc render --script all --metrics)" % (a.id, a.renders))
    missing = [s for s in SCRIPTS if s not in scripts_present(index)]
    if missing:
        raise UsageError("%s: the renders lack %s (bspc render --script all)" % (a.id, ", ".join(missing)))
    knobs = parse_knobs(a.knob)
    defined = [k for k in KNOBS if not any(s.startswith("S%d:" % (KNOBS.index(k) + 1))
                                           for s in index.get("skipped", []))]
    unrated = [k for k in defined if k not in knobs]
    if unrated:
        raise UsageError("%s: rate every knob the mode defines (missing: %s)" % (a.id, ", ".join(unrated)))
    failed = prescreen_failed(index)
    row = row_of(log, a.id)
    row["name"] = index["preset"]["name"]
    row["family"] = index["preset"]["family"]
    decl = index["declare"]
    if decl != row["declare"]:
        raise UsageError("%s: rendered with declarations %s, the log says %s (render with --declarations)"
                         % (a.id, json.dumps(decl), json.dumps(row["declare"])))
    row["rating"] = {
        "sound_rev": index["preset"]["soundRev"],
        "sound_hash": index["preset"]["soundHash"],
        "renders": render_records(index),
        "prescreen_failed": failed,
        "chord_finished": a.chord == "yes",
        "knobs": knobs,
        "level": a.level,
        "verdict": a.verdict,
        "notes": a.notes or "",
    }
    row["status"] = "rated"
    row["relisten"] = []
    row["new_renders"] = []
    save_log(a.log, log)
    print("%s: rated %s on %d renders (sound_rev %d)%s" % (
        a.id, a.verdict, len(row["rating"]["renders"]), row["rating"]["sound_rev"],
        "; the pre-screen FAILED" if failed else ("; no pre-screen" if failed is None else "")))
    return 0


def cmd_carry(a):
    log = load_log(a.log)
    before = dump_json(log)
    changes = carry(log, indexes(a.renders))
    for line in changes:
        print(line)
    if a.check:
        return 1 if dump_json(log) != before else 0
    save_log(a.log, log)
    print("carry: %d rows, %d changes" % (len(log["presets"]), len(changes)))
    return 0


def cmd_refresh(a):
    load_log(a.log)
    cmd = [a.bspc, "render", "--script", "all", "--metrics", "--declarations", a.log, "-o", a.renders]
    if a.no_wav:
        cmd.append("--no-wav")
    rc = subprocess.run(cmd + list(a.documents)).returncode
    if rc not in (0, 1):  # 1: a pre-screen check failed, which the rows record
        print("refresh: bspc render failed (exit %d)" % rc, file=sys.stderr)
        return 2
    a.check = False
    return cmd_carry(a)


def cmd_exit(a):
    log = load_log(a.log)
    met, lines = exit_report(log, indexes(a.renders) if a.renders else None)
    for line in lines:
        print(line)
    print("exit criteria %s" % ("met" if met else "not met"))
    return 0 if met else 1


def cmd_md(a):
    log = load_log(a.log)
    text = render_md(log)
    path = md_path(a.log)
    if a.check:
        current = None
        if os.path.exists(path):
            with open(path, encoding="utf-8") as f:
                current = f.read()
        if current != text:
            print("%s is stale (ratings.py md)" % path)
            return 1
        return 0
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    return 0


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = p.add_subparsers(dest="command", required=True)

    def add(name):
        s = sub.add_parser(name)
        s.add_argument("--log", default=DEFAULT_LOG)
        return s

    add("init")
    s = add("declare")
    s.add_argument("id")
    s.add_argument("--class", dest="input_class", choices=["attack", "pad"])
    s.add_argument("--self-oscillating", dest="self_oscillating", action="store_true", default=None)
    s.add_argument("--no-self-oscillating", dest="self_oscillating", action="store_false")
    s.add_argument("--needs-attacks", dest="needs_attacks", action="store_true", default=None)
    s.add_argument("--no-needs-attacks", dest="needs_attacks", action="store_false")
    s = add("rate")
    s.add_argument("id")
    s.add_argument("--renders", required=True)
    s.add_argument("--chord", required=True, choices=["yes", "no"])
    s.add_argument("--knob", action="append")
    s.add_argument("--level", required=True)
    s.add_argument("--verdict", required=True, choices=VERDICTS)
    s.add_argument("--notes")
    s = add("carry")
    s.add_argument("--renders", required=True)
    s.add_argument("--check", action="store_true")
    s = add("refresh")
    s.add_argument("--renders", required=True)
    s.add_argument("--bspc", required=True)
    s.add_argument("--no-wav", action="store_true")
    s.add_argument("documents", nargs="+")
    s = add("exit")
    s.add_argument("--renders")
    s = add("md")
    s.add_argument("--check", action="store_true")
    a = p.parse_args(argv)
    try:
        return {"init": cmd_init, "declare": cmd_declare, "rate": cmd_rate, "carry": cmd_carry,
                "refresh": cmd_refresh, "exit": cmd_exit, "md": cmd_md}[a.command](a)
    except UsageError as e:
        print("ratings.py %s: %s" % (a.command, e), file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
