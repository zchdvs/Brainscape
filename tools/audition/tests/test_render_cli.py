#!/usr/bin/env python3
"""`bspc render` end to end (tools/bspc, tools/audition; docs/design/mode-compiler.md §8.2, §11.3),
and the ratings log over its renders: the files a render writes and what they hold (16-bit WAVs,
recipes whose hashes are the index's, the same hashes without WAVs), refused usage, declarations
read from a ratings log, and the carry-forward after a document changes what it plays.

  test_render_cli.py --bspc BSPC --data compiler/tests/data --work DIR
"""
import argparse
import json
import os
import shutil
import struct
import subprocess
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
RATINGS = os.path.join(os.path.dirname(HERE), "ratings.py")
sys.path.insert(0, os.path.dirname(HERE))
import ratings  # noqa: E402

ARGS = None


def run(*cmd):
    p = subprocess.run(list(cmd), capture_output=True, text=True, encoding="utf-8")
    return p.returncode, p.stdout + p.stderr


def load(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


class RenderCliTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        shutil.rmtree(ARGS.work, ignore_errors=True)
        os.makedirs(ARGS.work)
        cls.engram = os.path.join(ARGS.data, "engram.json")

    def bspc(self, *args):
        return run(ARGS.bspc, "render", *args)

    def test_files_hashes_and_wavs(self):
        out = os.path.join(ARGS.work, "wav")
        rc, text = self.bspc("--script", "S0,S8", "--metrics", "-o", out, self.engram)
        self.assertEqual(rc, 0, text)
        self.assertIn("Engaged              pass", text)
        d = os.path.join(out, "factory.engram")
        index = load(os.path.join(d, "audition.json"))
        self.assertEqual(index["format"], "brainscape-audition-index/1")
        self.assertEqual(index["preset"]["id"], "factory.engram")
        self.assertEqual(sorted(index["scripts"]), ["S0", "S8"])
        self.assertFalse(index["prescreen"]["failed"])
        names = [r["name"] for r in index["renders"]]
        self.assertIn("S0.engaged.plucks", names)
        self.assertIn("S0.engaged.plucks@mixed-blocks", names)
        self.assertIn("S8.freeze.plucks", names)
        for r in index["renders"]:
            if r["role"] == "determinism":
                self.assertNotIn("recipe", r)
                continue
            recipe = load(os.path.join(d, r["recipe"]))
            self.assertEqual(recipe["outputSha256"], r["hash"])
            self.assertEqual(recipe["outputSegmentSha256"], r["seconds"])
            self.assertEqual(recipe["format"], "brainscape-audition/2")
            self.assertTrue(recipe["load"]["exact"])
            wav = os.path.join(d, r["wav"])
            with open(wav, "rb") as f:
                head = f.read(44)
            self.assertEqual(head[:4], b"RIFF")
            fmt, channels, rate, _, _, bits = struct.unpack("<HHIIHH", head[20:36])
            self.assertEqual((fmt, channels, rate, bits), (1, 2, 48000, 16))
            frames = struct.unpack("<I", head[40:44])[0] // 4
            self.assertEqual(frames, recipe["frames"])
            self.assertEqual(os.path.getsize(wav), 44 + 4 * frames)
        freeze = load(os.path.join(d, "S8.freeze.plucks.json"))
        self.assertEqual([(e["frame"], e["type"], e["value"]) for e in freeze["events"]["list"]],
                         [(192000, "Freeze", 1), (672000, "Freeze", 0)])
        # Without WAVs: the same renders, the same hashes.
        out2 = os.path.join(ARGS.work, "nowav")
        rc, text = self.bspc("--script", "S0,S8", "--no-wav", "-o", out2, self.engram)
        self.assertEqual(rc, 0, text)
        index2 = load(os.path.join(out2, "factory.engram", "audition.json"))
        self.assertEqual([r["hash"] for r in index2["renders"]], [r["hash"] for r in index["renders"]])
        self.assertFalse(any(n.endswith(".wav") for n in os.listdir(os.path.join(out2, "factory.engram"))))
        self.assertFalse(index2["prescreen"]["ran"])

    def test_usage_is_refused(self):
        for args in (["--script", "S12"], ["--script", "S0,,S1"], ["--class", "drum"], ["--metric"],
                     ["--script", "S0", "--script", "S1"]):
            rc, text = self.bspc(*(args + [self.engram]))
            self.assertEqual(rc, 2, (args, text))
        rc, _ = self.bspc(os.path.join(ARGS.work, "missing.json"))
        self.assertEqual(rc, 2)
        bad = os.path.join(ARGS.work, "bad-log.json")
        with open(bad, "w") as f:
            f.write('{"format": "x"}')
        rc, text = self.bspc("--declarations", bad, self.engram)
        self.assertEqual(rc, 2, text)
        for declare in ('{"class": "drone"}', '{"self_oscillating": "yes"}', '{"needs_attacks": 1}'):
            with open(bad, "w") as f:
                f.write('{"format": "brainscape-ratings/1", "presets": {"factory.engram": {"declare": %s}}}'
                        % declare)
            rc, text = self.bspc("--declarations", bad, self.engram)
            self.assertEqual(rc, 2, (declare, text))
            self.assertIn("declare.", text)
        with open(bad, "w") as f:
            f.write("# Factory audition log\n\nNo data block.\n")
        rc, text = self.bspc("--declarations", bad, self.engram)
        self.assertEqual(rc, 2, text)
        self.assertIn("no ratings data block", text)
        rc, text = self.bspc(self.engram, self.engram)
        self.assertEqual(rc, 2, text)  # one id twice

    def test_declarations_and_the_carry_forward(self):
        log = os.path.join(ARGS.work, "factory", "AUDITION.md")
        rc, text = run(sys.executable, RATINGS, "init", "--log", log)
        self.assertEqual(rc, 0, text)
        rc, text = run(sys.executable, RATINGS, "declare", "--log", log, "factory.engram", "--class", "pad")
        self.assertEqual(rc, 0, text)
        renders = os.path.join(ARGS.work, "all")
        rc, text = self.bspc("--script", "all", "--metrics", "--no-wav", "--declarations", log, "-o", renders,
                             self.engram)
        self.assertIn(rc, (0, 1), text)
        index = load(os.path.join(renders, "factory.engram", "audition.json"))
        self.assertEqual(rc == 1, index["prescreen"]["failed"])
        self.assertEqual(index["declare"]["class"], "pad")
        self.assertIn("S1.activity.soft_notes", [r["name"] for r in index["renders"]])
        self.assertEqual(len(index["scripts"]), 12)
        rate = [sys.executable, RATINGS, "rate", "--log", log, "--renders", renders, "factory.engram",
                "--chord", "yes", "--level", "+1.6 LU", "--verdict", "keep"]
        for k in ("activity", "repeats", "shape", "time", "space", "filter"):
            rate += ["--knob", k + "=4"]
        rc, text = run(*rate)
        self.assertEqual(rc, 0, text)
        rc, text = run(sys.executable, RATINGS, "carry", "--log", log, "--renders", renders, "--check")
        self.assertEqual(rc, 0, text)  # nothing moved
        # The document changes what it plays (Mix 0.35 to 0.36): every rating's renders move.
        changed = os.path.join(ARGS.work, "engram.json")
        with open(self.engram, encoding="utf-8") as f:
            doc = f.read()
        self.assertIn('"mix": 0.35', doc)
        with open(changed, "w", encoding="utf-8", newline="\n") as f:
            f.write(doc.replace('"mix": 0.35', '"mix": 0.36'))
        renders2 = os.path.join(ARGS.work, "all2")
        rc, text = self.bspc("--script", "S0", "--no-wav", "--declarations", log, "-o", renders2, changed)
        self.assertEqual(rc, 0, text)
        self.assertIn("stale", text)
        rc, text = run(sys.executable, RATINGS, "carry", "--log", log, "--renders", renders2)
        self.assertEqual(rc, 0, text)
        row = ratings.load_log(log)["presets"]["factory.engram"]
        self.assertEqual(row["status"], "re-listen")
        gone = [x for x in row["relisten"] if x["why"] == "gone"]
        moved = [x for x in row["relisten"] if x["why"] == "changed"]
        self.assertTrue(gone)  # S1-S11 were not rendered this time
        self.assertIn("S0.engaged.plucks", [x["render"] for x in moved])
        self.assertNotIn("S0.engaged.silence", [x["render"] for x in moved])  # silence stays silent
        # Back on the original renders, the rating holds again.
        rc, text = run(sys.executable, RATINGS, "carry", "--log", log, "--renders", renders)
        self.assertEqual(rc, 0, text)
        self.assertEqual(ratings.load_log(log)["presets"]["factory.engram"]["status"], "rated")


def main():
    global ARGS
    p = argparse.ArgumentParser()
    p.add_argument("--bspc", required=True)
    p.add_argument("--data", required=True)
    p.add_argument("--work", required=True)
    ARGS, rest = p.parse_known_args()
    ARGS.bspc = os.path.abspath(ARGS.bspc)
    unittest.main(argv=[sys.argv[0]] + rest, verbosity=1)


if __name__ == "__main__":
    main()
