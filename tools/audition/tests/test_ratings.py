#!/usr/bin/env python3
"""Tests of the ratings log and its carry-forward (tools/audition/ratings.py) over synthetic
render indexes in bspc render's format: rows found and declared, a rating recorded with its
render hashes, carried forward when they hold (a re-stamp, a bump that changes nothing), marked
re-listen with the first differing second when they do not, renders added since a rating, the
exit criteria and AUDITION.md's freshness."""
import contextlib
import hashlib
import io
import json
import os
import shutil
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import ratings  # noqa: E402


def read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


def h(text):
    return hashlib.sha256(text.encode()).hexdigest()


def render(script, name, seed, seconds=20, role="engaged"):
    secs = [h("%s/%s/%d" % (name, seed, k)) for k in range(seconds)]
    return {"script": script, "name": name, "role": role, "ok": True, "hash": h(name + "/" + seed + "/all"),
            "seconds": secs, "peakDbfs": -3.0, "loudnessLufs": -18.0, "tailSeconds": 2.0, "tailFinite": True}


def index(pid, family="echoic", rev=3, sound="a" * 64, renders=None, failed=False, declare=None, skipped=None):
    if renders is None:
        renders = [render(s, "%s.x.plucks" % s, "v1") for s in ratings.SCRIPTS]
    scripts = {}
    for r in renders:
        if r["role"] != "determinism":
            scripts.setdefault(r["script"], {"renders": 0, "hash": h(r["script"])})["renders"] += 1
    return {
        "format": ratings.INDEX_FORMAT,
        "soundRevision": rev,
        "preset": {"id": pid, "name": pid.split(".")[-1].capitalize(), "family": family, "source": "x.json",
                   "soundRev": rev, "soundHash": sound, "controlHash": "c" * 64, "packageHash": "p" * 64},
        "declare": declare or {"class": "attack", "self_oscillating": False, "needs_attacks": False},
        "onsetMode": False,
        "scripts": scripts,
        "renders": renders,
        "skipped": skipped or [],
        "prescreen": {"ran": True, "failed": failed, "checks": []},
    }


class RatingsTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="bsa-ratings-")
        self.log = os.path.join(self.dir, "factory", "AUDITION.json")
        self.renders = os.path.join(self.dir, "renders")
        os.makedirs(self.renders)

    def tearDown(self):
        shutil.rmtree(self.dir, ignore_errors=True)

    def run_cli(self, *args):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            rc = ratings.main(list(args))
        return rc, out.getvalue() + err.getvalue()

    def write_index(self, idx):
        d = os.path.join(self.renders, idx["preset"]["id"])
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, "audition.json"), "w", encoding="utf-8") as f:
            json.dump(idx, f)

    def log_json(self):
        with open(self.log, encoding="utf-8") as f:
            return json.load(f)

    def rate(self, pid, verdict="keep", knobs=4):
        args = ["rate", "--log", self.log, "--renders", self.renders, pid, "--chord", "yes",
                "--level", "+1.5 LU", "--verdict", verdict]
        for k in ratings.KNOBS:
            args += ["--knob", "%s=%d" % (k, knobs)]
        return self.run_cli(*args)

    def test_init_carry_discovers_and_declare(self):
        self.assertEqual(self.run_cli("init", "--log", self.log)[0], 0)
        self.assertEqual(self.run_cli("init", "--log", self.log)[0], 2)  # never overwrites
        self.write_index(index("factory.engram"))
        rc, out = self.run_cli("carry", "--log", self.log, "--renders", self.renders)
        self.assertEqual(rc, 0, out)
        row = self.log_json()["presets"]["factory.engram"]
        self.assertEqual(row["status"], "unrated")
        self.assertEqual(row["family"], "echoic")
        rc, _ = self.run_cli("declare", "--log", self.log, "factory.engram", "--class", "pad",
                             "--self-oscillating")
        self.assertEqual(rc, 0)
        d = self.log_json()["presets"]["factory.engram"]["declare"]
        self.assertEqual(d, {"class": "pad", "self_oscillating": True, "needs_attacks": False})
        # A rating needs renders made with the log's declarations.
        rc, out = self.rate("factory.engram")
        self.assertEqual(rc, 2)
        self.assertIn("declarations", out)

    def test_rate_records_the_renders_and_md(self):
        self.run_cli("init", "--log", self.log)
        self.write_index(index("factory.engram"))
        rc, out = self.rate("factory.engram")
        self.assertEqual(rc, 0, out)
        r = self.log_json()["presets"]["factory.engram"]["rating"]
        self.assertEqual(r["sound_rev"], 3)
        self.assertEqual(len(r["renders"]), 12)
        rec = r["renders"]["S3.x.plucks"]
        self.assertEqual(rec["hash"], h("S3.x.plucks/v1/all"))
        self.assertEqual(len(rec["seconds"]), 20 * ratings.SECOND_DIGITS)
        self.assertFalse(r["prescreen_failed"])
        md = read(ratings.md_path(self.log))
        self.assertIn("| factory.engram | echoic | attack | rated | 3 | aaaaaaaaaaaa |", md)
        self.assertEqual(self.run_cli("md", "--log", self.log, "--check")[0], 0)
        with open(ratings.md_path(self.log), "a", encoding="utf-8") as f:
            f.write("hand edit\n")
        self.assertEqual(self.run_cli("md", "--log", self.log, "--check")[0], 1)

    def test_rate_refuses_partial_renders_and_bad_knobs(self):
        self.run_cli("init", "--log", self.log)
        self.write_index(index("factory.a", renders=[render("S0", "S0.x", "v1")]))
        rc, out = self.rate("factory.a")
        self.assertEqual(rc, 2)
        self.assertIn("S1", out)
        self.write_index(index("factory.b"))
        rc, out = self.run_cli("rate", "--log", self.log, "--renders", self.renders, "factory.b", "--chord",
                               "yes", "--level", "ok", "--verdict", "keep", "--knob", "activity=6")
        self.assertEqual(rc, 2)
        rc, out = self.run_cli("rate", "--log", self.log, "--renders", self.renders, "factory.b", "--chord",
                               "yes", "--level", "ok", "--verdict", "keep", "--knob", "activity=4")
        self.assertEqual(rc, 2)
        self.assertIn("missing", out)
        # A macro the mode leaves undefined needs no rating.
        skipped = ["S6: macro.filter is undefined in this mode"]
        rs = [render(s, "%s.x" % s, "v1") for s in ratings.SCRIPTS if s != "S6"]
        self.write_index(index("factory.c", renders=rs, skipped=skipped))
        args = ["rate", "--log", self.log, "--renders", self.renders, "factory.c", "--chord", "no",
                "--level", "ok", "--verdict", "revise"]
        for k in ratings.KNOBS[:5]:
            args += ["--knob", k + "=3"]
        self.assertEqual(self.run_cli(*args)[0], 0)

    def test_carry_forward_holds_on_a_restamp(self):
        self.run_cli("init", "--log", self.log)
        self.write_index(index("factory.engram", rev=3, sound="a" * 64))
        self.rate("factory.engram")
        # Sound revision 4: the same renders, a re-stamped package.
        self.write_index(index("factory.engram", rev=4, sound="b" * 64))
        rc, out = self.run_cli("carry", "--log", self.log, "--renders", self.renders, "--check")
        self.assertEqual(rc, 1)  # a row would change
        rc, out = self.run_cli("carry", "--log", self.log, "--renders", self.renders)
        self.assertEqual(rc, 0)
        self.assertIn("carried forward to sound_rev 4", out)
        row = self.log_json()["presets"]["factory.engram"]
        self.assertEqual(row["status"], "rated")
        self.assertEqual(row["rating"]["sound_rev"], 4)
        self.assertEqual(row["rating"]["carried"], [{"from_rev": 3, "from_hash": "a" * 64, "to_rev": 4,
                                                     "to_hash": "b" * 64}])
        # Idempotent.
        self.assertEqual(self.run_cli("carry", "--log", self.log, "--renders", self.renders, "--check")[0], 0)

    def test_carry_marks_relisten_with_the_first_differing_second(self):
        self.run_cli("init", "--log", self.log)
        base = [render(s, "%s.x.plucks" % s, "v1") for s in ratings.SCRIPTS]
        self.write_index(index("factory.lull", renders=base))
        self.rate("factory.lull")
        changed = [dict(r) for r in base]
        r3 = dict(changed[3])
        r3["hash"] = h("changed")
        r3["seconds"] = r3["seconds"][:4] + [h("x%d" % k) for k in range(4, 20)]
        changed[3] = r3
        r7 = dict(changed[7])
        r7["hash"] = h("changed7")
        r7["seconds"] = [h("y")] + r7["seconds"][1:]
        changed[7] = r7
        self.write_index(index("factory.lull", rev=4, sound="b" * 64, renders=changed))
        rc, out = self.run_cli("carry", "--log", self.log, "--renders", self.renders)
        self.assertEqual(rc, 0)
        row = self.log_json()["presets"]["factory.lull"]
        self.assertEqual(row["status"], "re-listen")
        self.assertEqual([(x["render"], x["from_second"]) for x in row["relisten"]],
                         [("S3.x.plucks", 4), ("S7.x.plucks", 0)])
        self.assertEqual(row["rating"]["sound_rev"], 3)  # not carried
        md = read(ratings.md_path(self.log))
        self.assertIn("`factory.lull`: S3.x.plucks from 4 s; S7.x.plucks from 0 s", md)
        # Re-rated on the new renders, the row is current again.
        self.assertEqual(self.rate("factory.lull")[0], 0)
        self.assertEqual(self.log_json()["presets"]["factory.lull"]["status"], "rated")

    def test_a_gone_render_relistens_and_a_new_one_does_not(self):
        self.run_cli("init", "--log", self.log)
        base = [render(s, "%s.x.plucks" % s, "v1") for s in ratings.SCRIPTS]
        self.write_index(index("factory.a", renders=base))
        self.rate("factory.a")
        grown = base + [render("S11", "S11.at-factory.new.plucks", "v1")]
        self.write_index(index("factory.a", renders=grown))
        self.run_cli("carry", "--log", self.log, "--renders", self.renders)
        row = self.log_json()["presets"]["factory.a"]
        self.assertEqual(row["status"], "rated")
        self.assertEqual(row["new_renders"], ["S11.at-factory.new.plucks"])
        self.write_index(index("factory.a", renders=base[:-1]))
        self.run_cli("carry", "--log", self.log, "--renders", self.renders)
        row = self.log_json()["presets"]["factory.a"]
        self.assertEqual(row["status"], "re-listen")
        self.assertEqual(row["relisten"][0]["why"], "gone")

    def test_determinism_renders_are_not_rated(self):
        self.run_cli("init", "--log", self.log)
        rs = [render(s, "%s.x" % s, "v1") for s in ratings.SCRIPTS]
        rs.append(render("S0", "S0.x@again", "v1", role="determinism"))
        self.write_index(index("factory.a", renders=rs))
        self.rate("factory.a")
        self.assertNotIn("S0.x@again", self.log_json()["presets"]["factory.a"]["rating"]["renders"])

    def test_exit_criteria(self):
        self.run_cli("init", "--log", self.log)
        families = ["recall", "reverie", "misfire", "echoic"]
        for i in range(10):
            pid = "factory.m%d" % i
            self.write_index(index(pid, family=families[i % 4]))
            self.rate(pid)
        rc, out = self.run_cli("exit", "--log", self.log, "--renders", self.renders)
        self.assertEqual(rc, 0, out)
        # A knob under 3 on a kept mode, then a failed pre-screen, each fail the exit.
        self.rate("factory.m0", knobs=2)
        rc, out = self.run_cli("exit", "--log", self.log)
        self.assertEqual(rc, 1)
        self.assertIn("factory.m0 activity 2", out)
        self.rate("factory.m0")
        self.write_index(index("factory.m1", family="reverie", failed=True))
        rc, out = self.run_cli("exit", "--log", self.log, "--renders", self.renders)
        self.assertEqual(rc, 1)
        self.assertIn("no objective failure among the kept: factory.m1", out)
        # Nine keeps are not enough, and a family without a keeper fails.
        self.rate("factory.m1", verdict="drop")
        rc, out = self.run_cli("exit", "--log", self.log)
        self.assertEqual(rc, 1)
        self.assertIn("9 of at least 10", out)

    def test_first_difference(self):
        self.assertEqual(ratings.first_difference("aaaaaaaabbbbbbbb", "aaaaaaaacccccccc"), 1)
        self.assertEqual(ratings.first_difference("aaaaaaaa", "aaaaaaaabbbbbbbb"), 1)
        self.assertEqual(ratings.first_difference("", "aaaaaaaa"), 0)


if __name__ == "__main__":
    unittest.main(verbosity=1)
