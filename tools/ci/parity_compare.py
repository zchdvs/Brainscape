#!/usr/bin/env python3
"""Compare golden-harness reports across CI legs (determinism profile §6.1-§6.4).

Reads every brainscape-golden-report/1 JSON under the given paths and prints a
Markdown summary:

  * canonical legs (48-frame grid, 2^22 ring) against a reference leg, preset by
    preset, with the first differing second and any counter that differs;
  * input hashes, which must agree on every leg (the generator is integer-only);
  * coverage failures reported by any leg;
  * block perturbations: a leg's non-canonical runs against its own canonical run;
  * negative controls (tags starting "negctl-"), which are EXPECTED to differ from
    the reference: matching it would mean the corpus lost coverage.

Report-only by default: exit 0 whatever differs. --gate exits 1 on any canonical
mismatch, input-hash mismatch, coverage failure or negative control that matches;
it is for when a sound revision has been minted (profile §8.4 step 10).
"""
import argparse
import json
import os
import sys

FORMAT = "brainscape-golden-report/1"


def load_reports(paths):
    reports = []
    for root in paths:
        files = [root] if os.path.isfile(root) else [
            os.path.join(d, f) for d, _, fs in os.walk(root) for f in fs if f.endswith(".json")]
        for path in sorted(files):
            try:
                with open(path, encoding="utf-8") as fh:
                    data = json.load(fh)
            except (OSError, ValueError) as err:
                print(f"warning: skipping {path}: {err}", file=sys.stderr)
                continue
            if data.get("format") == FORMAT:
                data["_path"] = path
                reports.append(data)
    return reports


def is_canonical(r):
    b = r.get("build", {})
    return b.get("blockPattern") == [48] and r.get("historyFrames") == 1 << 22


def presets(r):
    """{'vector/preset': preset dict} plus {'vector': vector dict}."""
    out, vecs = {}, {}
    for v in r.get("vectors", []):
        vecs[v["name"]] = v
        for p in v.get("presets", []):
            out[f'{v["name"]}/{p["name"]}'] = p
    return out, vecs


def first_diff(a, b):
    for i, (x, y) in enumerate(zip(a, b)):
        if x != y:
            return i
    return None if len(a) == len(b) else min(len(a), len(b))


def compare(ref, other):
    """Per-preset comparison of `other` against `ref`: list of (name, status, detail)."""
    rp, _ = presets(ref)
    op, _ = presets(other)
    rows = []
    for name in sorted(set(rp) | set(op)):
        if name not in op:
            rows.append((name, "missing", "not rendered"))
            continue
        if name not in rp:
            rows.append((name, "extra", "not in the reference"))
            continue
        a, b = rp[name], op[name]
        counters = [f'{k} {a["counters"].get(k)}->{v}' for k, v in b.get("counters", {}).items()
                    if a.get("counters", {}).get(k) != v]
        if a["hash"] == b["hash"]:
            rows.append((name, "match", "; ".join(counters)))
        else:
            sec = first_diff(a.get("secondHashes", []), b.get("secondHashes", []))
            detail = f"first differs at s{sec}" if sec is not None else "differs"
            if counters:
                detail += "; counters: " + ", ".join(counters)
            rows.append((name, "DIFF", detail))
    return rows


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("paths", nargs="+", help="report files or directories")
    ap.add_argument("--reference", default="linux-x64-gcc", help="tag of the reference leg")
    ap.add_argument("--summary", help="also append the Markdown to this file ($GITHUB_STEP_SUMMARY)")
    ap.add_argument("--gate", action="store_true", help="exit 1 on any difference (after minting)")
    args = ap.parse_args()

    reports = load_reports(args.paths)
    lines = ["# Golden-hash parity report", ""]
    problems = 0
    if not reports:
        lines.append("No reports found.")
        return finish(lines, args, problems + 1)

    canon = {}
    for r in reports:
        tag = r.get("build", {}).get("tag", "?")
        if is_canonical(r) and not tag.startswith("negctl-"):
            canon[tag] = r
    ref_tag = args.reference if args.reference in canon else (sorted(canon)[0] if canon else None)
    head = reports[0]
    lines.append(f'Sound revision {head.get("soundRevision")}, generator v{head.get("generatorVersion")}, '
                 f'corpus v{head.get("corpusVersion")}. Mode: '
                 f'{"gating" if args.gate else "report-only (nothing is minted yet)"}.')
    lines.append("")
    if ref_tag is None:
        lines.append("No canonical (48-frame, 2^22 ring) report to compare against.")
        return finish(lines, args, problems + 1)
    ref = canon[ref_tag]
    ref_presets, ref_vectors = presets(ref)

    # Toolchains.
    lines += ["## Legs", "", "| leg | toolchain | blocks | notes | report |", "|---|---|---|---|---|"]
    for r in reports:
        b = r.get("build", {})
        notes = "; ".join(f"{k}: {v}" for k, v in b.get("notes", {}).items())
        lines.append(f'| {b.get("tag")} | {b.get("toolchain")} | {",".join(map(str, b.get("blockPattern", [])))} '
                     f'| {notes} | {os.path.basename(r["_path"])} |')
    lines.append("")

    # Canonical legs against the reference.
    lines += [f"## Canonical legs against `{ref_tag}`", ""]
    names = sorted(ref_presets)
    others = [t for t in sorted(canon) if t != ref_tag]
    lines.append("| preset | " + " | ".join(others) + " |")
    lines.append("|---|" + "---|" * len(others))
    results = {t: {n: (s, d) for n, s, d in compare(ref, canon[t])} for t in others}
    for n in names:
        cells = []
        for t in others:
            s, d = results[t].get(n, ("missing", ""))
            cells.append("=" if s == "match" else f"{s} ({d})" if d else s)
        lines.append(f"| {n} | " + " | ".join(cells) + " |")
    lines.append("")
    for t in others:
        matched = sum(1 for s, _ in results[t].values() if s == "match")
        total = len(results[t])
        lines.append(f"- `{t}`: {matched}/{total} presets bit-identical to `{ref_tag}`")
        problems += total - matched
    lines.append("")

    # Input hashes: integer-only generator, so these must agree everywhere.
    bad_inputs = []
    for r in reports:
        _, vecs = presets(r)
        for name, v in vecs.items():
            if name in ref_vectors and v.get("inputHash") != ref_vectors[name].get("inputHash"):
                bad_inputs.append(f'{r["build"].get("tag")}: {name}')
    lines += ["## Input hashes", ""]
    lines.append("All legs derived identical input bits." if not bad_inputs else
                 "**Input differs (generator not portable):** " + ", ".join(bad_inputs))
    problems += len(bad_inputs)
    lines.append("")

    # Coverage.
    cov = []
    for r in reports:
        p, _ = presets(r)
        for name, pr in p.items():
            c = pr.get("coverage", {})
            if c and not c.get("ok", True):
                cov.append(f'{r["build"].get("tag")} {name}: {"; ".join(c.get("failures", []))}')
    lines += ["## Coverage", ""]
    lines += ["Every preset exercised its features on every leg."] if not cov else [f"- **{c}**" for c in cov]
    problems += len(cov)
    lines.append("")

    # Block perturbations (profile §6.4): expected to differ until §5.7's fix lands.
    lines += ["## Block perturbations (against the same leg's 48-frame run)", ""]
    any_pert = False
    for r in reports:
        b = r.get("build", {})
        tag = b.get("tag", "?")
        if is_canonical(r) or tag.startswith("negctl-") or tag not in canon:
            continue
        any_pert = True
        rows = compare(canon[tag], r)
        diff = [f"{n} ({d})" for n, s, d in rows if s != "match"]
        pattern = ",".join(map(str, b.get("blockPattern", [])))
        lines.append(f"- `{tag}` blocks {pattern}: {len(rows) - len(diff)}/{len(rows)} identical"
                     + (": differ in " + "; ".join(diff) if diff else ""))
        problems += len(diff)
    if not any_pert:
        lines.append("No perturbation runs.")
    lines.append("")

    # Negative controls (profile §6.4): must differ, or the corpus lost coverage.
    lines += ["## Negative controls (must differ from the reference)", ""]
    any_neg = False
    for r in reports:
        tag = r.get("build", {}).get("tag", "?")
        if not tag.startswith("negctl-"):
            continue
        any_neg = True
        rows = compare(ref, r)
        diff = sum(1 for _, s, _ in rows if s != "match")
        if diff:
            lines.append(f"- `{tag}`: differs in {diff}/{len(rows)} presets, as expected")
        else:
            lines.append(f"- **`{tag}`: identical to the reference: the corpus cannot see contraction**")
            problems += 1
    if not any_neg:
        lines.append("No negative-control runs.")
    lines.append("")
    return finish(lines, args, problems)


def finish(lines, args, problems):
    text = "\n".join(lines) + "\n"
    print(text)
    if args.summary:
        with open(args.summary, "a", encoding="utf-8") as fh:
            fh.write(text)
    if args.gate and problems:
        print(f"{problems} problem(s); gating.", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
