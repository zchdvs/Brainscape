#!/usr/bin/env python3
"""Self-test of dac_test.py (the limiter's DAC test, firmware/DAC_TEST.md) on synthetic recordings.

The program's codes come from dac_test.py's model of the generator, which the first check
compares with the generator itself (the host test firmware_tone --program-json, when its path is
given): every step's wave, level, level code, length and one period of codes. A synthetic
recording then plays those codes through a model of the chain: the DAC's ideal reconstruction
(the codes' harmonics below 24 kHz), an optional clip of the reconstructed waveform (a DAC that
clips inter-sample peaks, or an output stage that clips below full scale), a gain, the interface's
anti-alias filter (nothing at or above 0.45 of its rate), its noise, its clock against the Seed's,
a fractional delay, whatever the Seed played before the program, mains hum, a DC offset, and the
interface's own full scale and word length. Each case requires dac_test.py's verdicts and its key
numbers:

  clean         48 kHz stereo 24-bit, the two outputs 0.3 dB apart: PASS; the 0 dBFS level, and
                in dBu, Vrms and Vpp from the interface's full scale given in dBu or in Vrms
  clipFS        48 kHz mono float, +35 ppm: a DAC that clips at full scale: the fs/4 test FAILs
                by 1.74 dB, clean up to +0.01 dBTP, the chain PASSes
  headroom96k   96 kHz mono 32-bit extensible, -60 ppm, junk before the program: a DAC with
                1.5 dB of inter-sample headroom: FAIL, clean up to +1.51 dBTP, 36 kHz reported
  analogClip    an output stage that clips at 0.97 of full scale: the chain FAILs, the fs/4 test
                is INCONCLUSIVE
  overload      the interface's gain too high: the recording clips, INVALID (exit 2)
  hum           50 Hz hum with its 3rd and 5th harmonics at -70 dBFS on both inputs, the gain
                low (the 0 dBFS sine at -20 dBFS): the noise floor shows it, PASS
  dcOffset      a -45 dBFS DC offset at the same gain: the program found, PASS
  deadChannel   the second input not connected, or carrying the first's crosstalk at -60 dB:
                analysed on the first, PASS
  padded        a DAW's exact zeros before the recording: found, PASS
  truncated, notFound, tooSlow   exit 2

and the WAV reader against every format it takes. Exit 1 on any failure.

  test_dac_test.py [--tone-binary PATH/TO/firmware_tone] [--keep DIR]
"""
import argparse
import array
import cmath
import io
import json
import math
import os
import random
import shutil
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dac_test as dt  # noqa: E402

FAILURES = []


def check(ok, what):
    if not ok:
        FAILURES.append(what)
        print("FAIL: " + what)


def near(value, want, tol):
    return value is not None and abs(value - want) <= tol


# ---- The generator, against the host test's copy -------------------------------------------

def check_program(binary):
    out = subprocess.run([os.path.abspath(binary), "--program-json"], check=True, capture_output=True, text=True).stdout
    prog = json.loads(out)
    check(prog["version"] == dt.PROGRAM_VERSION, "program version %s != %s" % (prog["version"], dt.PROGRAM_VERSION))
    check(prog["sampleRate"] == dt.SEED_RATE and prog["period"] == dt.PERIOD and prog["codecMax"] == dt.CODEC_MAX,
          "the generator's constants")
    check(prog["frames"] == dt.PROGRAM_FRAMES, "program length %s != %s" % (prog["frames"], dt.PROGRAM_FRAMES))
    check(len(prog["steps"]) == len(dt.STEPS), "step count %d != %d" % (len(prog["steps"]), len(dt.STEPS)))
    for theirs, ours in zip(prog["steps"], dt.STEPS):
        same = (theirs["wave"] == ours.wave and theirs["centiDb"] == ours.centi_db and theirs["q"] == ours.q
                and theirs["frames"] == ours.frames
                and theirs["codes"] == [dt.code(ours.wave, ours.q, n) for n in range(dt.PERIOD)])
        check(same, "step %d differs between Tone.h and dac_test.py: %s / %s %d %d %d" % (
            ours.index, {k: theirs[k] for k in ("wave", "centiDb", "q", "frames")}, ours.wave, ours.centi_db,
            ours.q, ours.frames))
    print("program: %d steps, %d frames, the same in Tone.h and dac_test.py" % (len(dt.STEPS), dt.PROGRAM_FRAMES))


def check_model():
    """dac_test.py's own model, without the binary: what the C++ test pins too."""
    check(dt.level_code(0) == 1 << 23 and dt.level_code(-2000) == 838861 and dt.level_code(-301) == 5931846,
          "level codes")
    check(dt.PROGRAM_FRAMES == 2856000 and len(dt.STEPS) == 88, "program size")
    check([dt.code("isp", 7, n) for n in range(8)] == [7, 7, -7, -7, 7, 7, -7, -7], "isp codes")
    check([dt.code("fs4", 7, n) for n in range(4)] == [0, 7, 0, -7], "fs4 codes")
    check(dt.code("sine", 1 << 23, 12) == 1 << 23 and dt.code("sine", 1 << 23, 36) == -(1 << 23), "sine crest")
    check(near(dt.digital_amplitude("isp", 1 << 23), dt.CODEC_MAX * math.sqrt(2) / (1 << 23), 1e-12),
          "isp amplitude: the clamped code times sqrt(2)")
    check(near(dt.digital_amplitude("fs4", 1 << 23), dt.CODEC_MAX / float(1 << 23), 1e-12), "fs4 amplitude")
    check(near(dt.digital_amplitude("sine", 838861), 838861 / float(1 << 23), 2e-7), "sine amplitude")
    check(len(dt.ONSETS) == 10, "ten onsets locate the program")


# ---- WAV writing and the reader ------------------------------------------------------------

def write_wav(path, channels, rate, kind="pcm24", extensible=False, extra_chunk=False):
    """kind: pcm16, pcm24, pcm32, float32, float64. channels: lists of floats in [-1, 1]."""
    nch, n = len(channels), len(channels[0])
    bits = {"pcm16": 16, "pcm24": 24, "pcm32": 32, "float32": 32, "float64": 64}[kind]
    tag = 3 if kind.startswith("float") else 1
    inter = [channels[c][i] for i in range(n) for c in range(nch)]
    if kind == "pcm16":
        data = array.array("h", (max(-32768, min(32767, int(round(v * 32768)))) for v in inter))
    elif kind in ("pcm24", "pcm32"):
        full = 1 << 23
        a = array.array("i", ((max(-full, min(full - 1, int(round(v * full)))) << 8) for v in inter))
        if kind == "pcm32":
            data = a
        else:
            if sys.byteorder == "big":
                a.byteswap()
            raw = a.tobytes()
            b = bytearray(len(raw) // 4 * 3)
            b[0::3], b[1::3], b[2::3] = raw[1::4], raw[2::4], raw[3::4]
            data = bytes(b)
    elif kind == "float32":
        data = array.array("f", inter)
    else:
        data = array.array("d", inter)
    if not isinstance(data, bytes):
        if sys.byteorder == "big":
            data.byteswap()
        data = data.tobytes()
    align = nch * bits // 8
    if extensible:
        guid = struct.pack("<H", tag) + dt._GUID_TAIL
        fmt = struct.pack("<HHIIHHHHI", 0xFFFE, nch, rate, rate * align, align, bits, 22, bits,
                          (1 << nch) - 1) + guid
    else:
        fmt = struct.pack("<HHIIHH", tag, nch, rate, rate * align, align, bits)
    chunks = [b"fmt " + struct.pack("<I", len(fmt)) + fmt]
    if extra_chunk:
        chunks.append(b"LIST" + struct.pack("<I", 5) + b"INFOx" + b"\x00")  # odd size, padded
    chunks.append(b"data" + struct.pack("<I", len(data)) + data + (b"\x00" if len(data) & 1 else b""))
    body = b"WAVE" + b"".join(chunks)
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", len(body)) + body)


def check_reader(tmp):
    rng = random.Random(7)
    vals = [[rng.uniform(-0.9, 0.9) for _ in range(301)] for _ in range(2)]
    vals[0][:4] = [0.0, -1.0, 0.5, -0.25]
    for kind, ext, extra, tol in (("pcm16", False, True, 1.0 / 32768), ("pcm24", False, False, 2.0 ** -24),
                                  ("pcm24", True, True, 2.0 ** -24), ("pcm32", True, False, 2.0 ** -24),
                                  ("float32", False, False, 1e-7), ("float32", True, True, 1e-7),
                                  ("float64", False, False, 0.0)):
        path = os.path.join(tmp, "reader-%s-%d-%d.wav" % (kind, ext, extra))
        write_wav(path, vals, 44100, kind, ext, extra)
        rec = dt.read_wav(path)
        ok = rec.rate == 44100 and len(rec.channels) == 2 and len(rec.channels[0]) == 301
        for c in range(2):
            for i in range(301):
                ok = ok and abs(rec.channels[c][i] * rec.scale - vals[c][i]) <= tol + 1e-12
        check(ok, "the reader reads %s%s%s exactly" % (kind, " extensible" if ext else "", " with a LIST chunk" if extra else ""))
    bad = os.path.join(tmp, "bad.wav")
    with open(bad, "wb") as f:
        f.write(b"RIFF\x04\x00\x00\x00AVI ")
    try:
        dt.read_wav(bad)
        check(False, "the reader refuses a non-WAV file")
    except dt.DacTestError:
        pass


# ---- Synthetic recordings ------------------------------------------------------------------

def step_harmonics(step, rate, rho, gain, clip):
    """(harmonic of 1 kHz, amplitude, phase) of a step's analog output: the codes' ideal
    reconstruction, clipped at `clip` (Seed full scale) if given, through the interface's
    anti-alias filter, times `gain`."""
    if step.wave == "silence":
        return []
    x = [dt.codec_code(dt.code(step.wave, step.q, n)) / float(1 << 23) for n in range(dt.PERIOD)]
    harm = {}
    for k in range(1, 24):
        c = sum(x[n] * cmath.exp(-2j * math.pi * k * n / dt.PERIOD) for n in range(dt.PERIOD)) * 2 / dt.PERIOD
        if abs(c) > 1e-12:
            harm[k] = c
    top = int(0.45 * rate / (1000.0 * rho))
    if clip is None or sum(abs(c) for c in harm.values()) <= clip:
        return [(k, abs(c) * gain, cmath.phase(c)) for k, c in harm.items() if k <= top]
    m_pts = 2048
    y = [sum((c * cmath.exp(2j * math.pi * k * m / m_pts)).real for k, c in harm.items()) for m in range(m_pts)]
    if max(abs(v) for v in y) <= clip:
        return [(k, abs(c) * gain, cmath.phase(c)) for k, c in harm.items() if k <= top]
    y = [max(-clip, min(clip, v)) for v in y]
    out = []
    for h in range(1, top + 1):
        z = sum(y[m] * cmath.exp(-2j * math.pi * h * m / m_pts) for m in range(m_pts)) * 2 / m_pts
        if abs(z) > 1e-9:
            out.append((h, abs(z) * gain, cmath.phase(z)))
    return out


def synth(rate, gain_db=-12.0, rho=1.0, clip=None, noise_dbfs=-105.0, lead=1.3, junk=False, tail=0.5,
          delay=0.37, seed=1, dc=0.0, hum_dbfs=None):
    """One channel of a recording of the program, in interface full-scale units (unquantized).
    hum_dbfs: 50 Hz mains hum of that peak, with its 3rd and 5th harmonics at 1/3 and 1/5."""
    gain = 10 ** (gain_db / 20)
    t0 = (int(lead * rate) + delay) / rate  # the program's frame 0, in recording seconds
    total = int(round((t0 + dt.PROGRAM_FRAMES / float(dt.SEED_RATE) / rho + tail) * rate))
    rng = random.Random(seed)
    nrms = 10 ** (noise_dbfs / 20)
    noise = [rng.gauss(0.0, nrms) for _ in range(65521)]
    out = [0.0] * total
    first = int(math.ceil(t0 * rate))
    if junk:  # the engine's output before `tone test`: noise at -40 dBFS
        for m in range(first - int(0.002 * rate)):
            out[m] = rng.gauss(0.0, 0.01)
    tile = rho == 1.0 and rate % 1000 == 0
    for step in dt.STEPS:
        hs = step_harmonics(step, rate, rho, gain, clip)
        if not hs:
            continue
        a = int(math.ceil((t0 + step.start / float(dt.SEED_RATE) / rho) * rate - 1e-9))
        b = int(math.ceil((t0 + (step.start + step.frames) / float(dt.SEED_RATE) / rho) * rate - 1e-9))
        if tile:
            per = rate // 1000
            period = []
            for j in range(per):
                tau = (a + j) / float(rate) - t0
                period.append(sum(amp * math.cos(2 * math.pi * k * 1000.0 * tau + ph) for k, amp, ph in hs))
            reps = (b - a) // per + 1
            out[a:b] = (period * reps)[:b - a]
        else:
            w = [(2 * math.pi * k * 1000.0, amp, ph) for k, amp, ph in hs]
            cos = math.cos
            for m in range(a, b):
                tau = (m / float(rate) - t0) * rho
                out[m] = sum(amp * cos(om * tau + ph) for om, amp, ph in w)
    if hum_dbfs is not None:
        a = 10 ** (hum_dbfs / 20)
        per = rate // 50 if rate % 50 == 0 else total
        cycle = [sum(a / h * math.sin(2 * math.pi * 50.0 * h * m / rate) for h in (1, 3, 5)) for m in range(per)]
        out = [v + cycle[m % per] for m, v in enumerate(out)]
    n = len(noise)
    return [v + noise[i % n] + dc for i, v in enumerate(out)]


def run(path, *extra):
    """dac_test.py's exit code, report and printed text (its errors included)."""
    report_path = path + ".json"
    if os.path.exists(report_path):
        os.remove(report_path)
    args = [path, "--json", report_path, "--steps"] + list(extra)
    old = sys.stdout, sys.stderr
    buf = io.StringIO()
    sys.stdout = sys.stderr = buf
    try:
        code = dt.main(args)
    finally:
        sys.stdout, sys.stderr = old
    report = None
    if os.path.exists(report_path):
        with open(report_path, encoding="utf-8") as f:
            report = json.load(f)
    return code, report, buf.getvalue()


def criteria(report, channel=0):
    return {name: (verdict, text) for name, verdict, text in report["channels"][channel]["criteria"]}


def case_clean(tmp):
    ch1 = synth(48000, -12.0, seed=1)
    ch2 = synth(48000, -12.3, seed=2, delay=0.81)
    path = os.path.join(tmp, "clean.wav")
    write_wav(path, [ch1, ch2], 48000, "pcm24")
    code, rep, text = run(path, "--interface-fs-dbu", "18")
    check(code == 0 and rep["result"] == "PASS", "clean: PASS (exit %s)\n%s" % (code, text))
    check(near(rep["clockPpm"], 0.0, 0.5), "clean: clock offset %.3f ppm" % rep["clockPpm"])
    check(near(rep["programStartSeconds"], (int(1.3 * 48000) + 0.37) / 48000, 0.0005), "clean: program start")
    for c, want in ((0, -12.0), (1, -12.3)):
        m = rep["channels"][c]
        zero = m["zeroDbfs"]["recordedPeakDbfs"]
        check(near(zero, want, 0.01), "clean ch%d: 0 dBFS at %.3f dBFS, not %.1f" % (c + 1, zero, want))
        check(near(m["isp"]["test"]["deltaDb"], 0.0, 0.01), "clean ch%d: fs/4 test %+.4f dB" % (
            c + 1, m["isp"]["test"]["deltaDb"]))
        check(near(m["ispCleanToDbtp"], 3.01, 0.01), "clean ch%d: clean to %s" % (c + 1, m["ispCleanToDbtp"]))
        check(m["zeroDbfs"]["thdDb"] < -90, "clean ch%d: THD %.1f" % (c + 1, m["zeroDbfs"]["thdDb"]))
        check(near(m["noiseFloorDbfs"], -105.0, 0.3), "clean ch%d: noise %.2f" % (c + 1, m["noiseFloorDbfs"]))
        check(near(m["square"]["deltaDb"], 0.0, 0.01), "clean ch%d: square %+.4f" % (c + 1, m["square"]["deltaDb"]))
        check(criteria(rep, c)["chain"][0] == "PASS" and criteria(rep, c)["isp"][0] == "PASS", "clean ch%d verdicts" % (c + 1))
    # +18 dBu at full scale (a +18 dBu sine peaks at the interface's full scale): the Seed's 0 dBFS
    # sine, recorded at -12.0 dBFS, is +6.0 dBu.
    check(near(rep["channels"][0].get("zeroDbfsDbu"), 18.0 - 12.0, 0.02), "clean: dBu %s" % (
        rep["channels"][0].get("zeroDbfsDbu"),))
    check("D10 inter-sample peaks: PASS" in text and "RESULT: PASS" in text, "clean: the printed verdict")
    # The same recording from a Seed whose full scale is 3.6 Vpp (1.8 V peak, 1.273 Vrms, +4.31
    # dBu): an interface whose full-scale sine is 12 dB above it, given in Vrms.
    seed_vrms = 1.8 / math.sqrt(2)
    code, rep, text = run(path, "--interface-fs-vrms", "%.6f" % (seed_vrms * 10 ** (12.0 / 20)))
    m = rep["channels"][0]
    check(code == 0 and near(m.get("zeroDbfsVpp"), 3.6, 0.005) and near(m.get("zeroDbfsVrms"), seed_vrms, 0.002)
          and near(m.get("zeroDbfsDbu"), 20 * math.log10(seed_vrms / dt.DBU_VOLTS), 0.02),
          "clean: 3.6 Vpp from --interface-fs-vrms: %s Vpp, %s Vrms, %s dBu" % (
              m.get("zeroDbfsVpp"), m.get("zeroDbfsVrms"), m.get("zeroDbfsDbu")))
    check("= +4.31 dBu = 1.273 Vrms = 3.60 Vpp" in text, "clean: the printed level in volts")
    return ch1


def case_clip_fs(tmp):
    ch = synth(48000, -10.0, rho=1 + 35e-6, clip=1.0, seed=3)
    path = os.path.join(tmp, "clipfs.wav")
    write_wav(path, [ch], 48000, "float32")
    code, rep, text = run(path)
    check(code == 1 and rep["result"] == "FAIL", "clipFS: FAIL (exit %s)\n%s" % (code, text))
    check(near(rep["clockPpm"], 35.0, 0.5), "clipFS: clock %.3f ppm" % rep["clockPpm"])
    m = rep["channels"][0]
    check(near(m["isp"]["test"]["deltaDb"], -1.741, 0.02), "clipFS: fs/4 test %+.3f dB" % m["isp"]["test"]["deltaDb"])
    check(near(m["isp"]["control"]["deltaDb"], 0.0, 0.01), "clipFS: control %+.4f" % m["isp"]["control"]["deltaDb"])
    check(near(m["ispCleanToDbtp"], 0.01, 0.005), "clipFS: clean to %s dBTP" % m["ispCleanToDbtp"])
    check(near(m["square"]["deltaDb"], -0.177, 0.02), "clipFS: the square's fundamental %+.3f" % m["square"]["deltaDb"])
    cr = criteria(rep)
    check(cr["isp"][0] == "FAIL" and cr["chain"][0] == "PASS", "clipFS: verdicts %s" % cr)
    check("C <= -3.00 dBFS" in cr["isp"][1], "clipFS: the ceiling suggested: %s" % cr["isp"][1])


def case_headroom_96k(tmp):
    ch = synth(96000, -14.0, rho=1 - 60e-6, clip=10 ** (1.5 / 20), junk=True, lead=2.1, seed=4, delay=0.13)
    path = os.path.join(tmp, "headroom96k.wav")
    write_wav(path, [ch], 96000, "pcm32", extensible=True)
    code, rep, text = run(path)
    check(code == 1 and rep["result"] == "FAIL", "headroom96k: FAIL (exit %s)\n%s" % (code, text))
    check(near(rep["clockPpm"], -60.0, 0.5), "headroom96k: clock %.3f ppm" % rep["clockPpm"])
    m = rep["channels"][0]
    check(near(m["isp"]["test"]["deltaDb"], -0.674, 0.02), "headroom96k: fs/4 test %+.3f" % m["isp"]["test"]["deltaDb"])
    check(near(m["ispCleanToDbtp"], 1.51, 0.005), "headroom96k: clean to %s" % m["ispCleanToDbtp"])
    cr = criteria(rep)
    check(cr["isp"][0] == "FAIL" and cr["chain"][0] == "PASS", "headroom96k: verdicts %s" % cr)
    check("isp36k" in cr and "36 kHz" in cr["isp36k"][1], "headroom96k: the 36 kHz line")
    h36 = m["isp"]["test"]["harmonicsDb"]["3"]
    check(near(h36, 20 * math.log10(0.080297 / 1.308584), 0.2), "headroom96k: 36 kHz at %.2f dB" % h36)


def case_analog_clip(tmp):
    ch = synth(48000, -12.0, clip=0.97, seed=5)
    path = os.path.join(tmp, "analogclip.wav")
    write_wav(path, [ch], 48000, "pcm24")
    code, rep, text = run(path)
    cr = criteria(rep)
    check(code == 1 and rep["result"] == "FAIL" and cr["chain"][0] == "FAIL" and cr["isp"][0] == "INCONCLUSIVE",
          "analogClip: chain FAIL, isp INCONCLUSIVE (exit %s, %s)" % (code, cr))
    m = rep["channels"][0]
    check(m["zeroDbfs"]["thdDb"] > -60 and m["zeroDbfs"]["flatness"] < 0.99, "analogClip: THD and flattening")


def case_overload(tmp):
    ch = synth(48000, -1.0, seed=6)
    path = os.path.join(tmp, "overload.wav")
    write_wav(path, [ch], 48000, "pcm24")
    code, rep, text = run(path)
    check(code == 2 and rep["result"] == "INVALID" and not rep["recordingOk"]
          and rep["channels"][0]["recordingClipped"], "overload: the recording clipped (exit %s)" % code)
    cr = criteria(rep)
    check(cr["isp"][0] == "INCONCLUSIVE" and cr["chain"][0] == "INCONCLUSIVE", "overload: nothing judged: %s" % cr)
    check("lower the interface's gain by 11 dB" in cr["isp"][1], "overload: the gain advice: %s" % cr["isp"][1])
    check("the recording clipped" in text and "RESULT: INVALID" in text, "overload: the printed advice")


def case_hum(tmp):
    """Hum the guide calls harmless: the gate and every measurement work at 1 kHz or 12 kHz."""
    ch1 = synth(48000, -20.0, seed=12, hum_dbfs=-70.0)
    ch2 = synth(48000, -20.0, seed=13, delay=0.55, hum_dbfs=-70.0)
    path = os.path.join(tmp, "hum.wav")
    write_wav(path, [ch1, ch2], 48000, "pcm24")
    code, rep, text = run(path)
    check(code == 0 and rep["result"] == "PASS", "hum: PASS (exit %s)\n%s" % (code, text))
    for c in (0, 1):
        m = rep["channels"][c]
        check(m["signal"] and m["referenceSnrDb"] > 90.0, "hum ch%d: the program found at 1 kHz (%.1f dB)" % (
            c + 1, m["referenceSnrDb"]))
        check(near(m["noiseFloorDbfs"], -72.4, 0.5), "hum ch%d: the noise floor shows the hum (%.2f dBFS)" % (
            c + 1, m["noiseFloorDbfs"]))
        if m["signal"]:
            check(near(m["zeroDbfs"]["recordedPeakDbfs"], -20.0, 0.01) and near(m["isp"]["test"]["deltaDb"], 0.0, 0.01),
                  "hum ch%d: 0 dBFS %.3f dBFS, fs/4 test %+.4f dB" % (
                      c + 1, m["zeroDbfs"]["recordedPeakDbfs"], m["isp"]["test"]["deltaDb"]))


def case_dc_offset(tmp):
    """An offset on the interface's input must not hide the program's silences."""
    ch = synth(48000, -20.0, seed=14, dc=10 ** (-45.0 / 20))
    path = os.path.join(tmp, "dcoffset.wav")
    write_wav(path, [ch], 48000, "pcm24")
    code, rep, text = run(path)
    check(code == 0 and rep is not None and rep["result"] == "PASS", "dcOffset: PASS (exit %s)\n%s" % (code, text))
    if rep is not None:
        check(near(rep["programStartSeconds"], (int(1.3 * 48000) + 0.37) / 48000, 0.0005),
              "dcOffset: program start %.6f" % rep["programStartSeconds"])
        check(near(rep["channels"][0]["zeroDbfs"]["recordedPeakDbfs"], -20.0, 0.01), "dcOffset: 0 dBFS level")


def case_dead_channel(tmp, ch1):
    rng = random.Random(9)
    dead = [rng.gauss(0.0, 1e-5) for _ in range(len(ch1))]
    path = os.path.join(tmp, "dead.wav")
    write_wav(path, [ch1, dead], 48000, "pcm24")
    code, rep, text = run(path)
    check(code == 0 and rep["channels"][0]["signal"] and not rep["channels"][1]["signal"],
          "deadChannel: PASS on channel 1, channel 2 without signal (exit %s)" % code)
    check("check this input's wiring" in text, "deadChannel: the printed advice")
    # The first input's crosstalk at -60 dB is too weak to measure the chain by (a narrower gate
    # would judge it, and fail it on its noise-limited THD): it is not the program.
    path = os.path.join(tmp, "crosstalk.wav")
    write_wav(path, [ch1, [v * 1e-3 + d for v, d in zip(ch1, dead)]], 48000, "pcm24")
    code, rep, text = run(path)
    check(code == 0 and rep["channels"][0]["signal"] and not rep["channels"][1]["signal"],
          "deadChannel: crosstalk at -60 dB is not the program (exit %s, %.1f dB)\n%s" % (
              code, rep["channels"][1]["referenceSnrDb"], text))


def case_padded(tmp, ch1):
    """A DAW's exact zeros before the interface's first sample must not pass for its noise floor."""
    path = os.path.join(tmp, "padded.wav")
    write_wav(path, [[0.0] * (4 * 48000) + ch1], 48000, "pcm24")  # more than 5 % of the file
    code, rep, text = run(path)
    check(code == 0 and near(rep["programStartSeconds"], 4.0 + (int(1.3 * 48000) + 0.37) / 48000, 0.0005),
          "padded: PASS, the program found after the zeros (exit %s)" % code)


def case_errors(tmp, ch1):
    path = os.path.join(tmp, "truncated.wav")
    write_wav(path, [ch1[:48000 * 40]], 48000, "pcm24")
    code, _, text = run(path)
    check(code == 2 and "stops 20.80 s before the program's end" in text, "truncated: exit %s, %s" % (code, text))
    path = os.path.join(tmp, "notfound.wav")
    write_wav(path, [[0.1 * math.sin(2 * math.pi * 1000 * m / 48000) for m in range(48000 * 3)]], 48000, "pcm24")
    code, _, text = run(path)
    check(code == 2 and "could not find the DAC test program" in text, "notFound: exit %s, %s" % (code, text))
    path = os.path.join(tmp, "slow.wav")
    write_wav(path, [[0.0] * 32000], 32000, "pcm24")
    code, _, text = run(path)
    check(code == 2 and "record at 44.1 kHz or more" in text, "tooSlow: exit %s, %s" % (code, text))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tone-binary", help="the host test firmware_tone, to compare the program with Tone.h's")
    ap.add_argument("--keep", help="write the synthetic recordings here and keep them")
    args = ap.parse_args()
    check_model()
    if args.tone_binary:
        check_program(args.tone_binary)
    tmp = args.keep or tempfile.mkdtemp(prefix="dac_test_")
    os.makedirs(tmp, exist_ok=True)
    try:
        check_reader(tmp)
        ch1 = case_clean(tmp)
        case_clip_fs(tmp)
        case_headroom_96k(tmp)
        case_analog_clip(tmp)
        case_overload(tmp)
        case_hum(tmp)
        case_dc_offset(tmp)
        case_dead_channel(tmp, ch1)
        case_padded(tmp, ch1)
        case_errors(tmp, ch1)
    finally:
        if not args.keep:
            shutil.rmtree(tmp, ignore_errors=True)
    print("test_dac_test.py: %s" % ("ok" if not FAILURES else "%d FAILURE(S)" % len(FAILURES)))
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
