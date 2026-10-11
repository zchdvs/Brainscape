#!/usr/bin/env python3
"""The limiter's DAC test: analyses a recording of the live image's `tone test` program
(firmware/DAC_TEST.md; docs/design/output-limiter.md §6.4, lane L1b, decision D10). Stdlib only.

  dac_test.py RECORDING.wav [--interface-fs-dbu DBU | --interface-fs-vrms VRMS] [--steps]
                            [--json REPORT.json]

RECORDING.wav is what the owner's audio interface recorded from the Seed's audio outputs while
`tone test` played: PCM (16, 24 or 32 bits) or IEEE float (32 or 64 bits), plain or
WAVE_FORMAT_EXTENSIBLE, mono or with one channel per Seed output, at any rate from 44.1 kHz
(96 kHz or more also shows the fs/4 tone's third harmonic). The tool finds the program in it
from its silences, measures the Seed's clock against the interface's from the reference sine, and
fits every segment's tone (amplitude, phase, DC) at its exact recorded frequency. It reports, per
channel:

  * the 0 dBFS output level: the 1 kHz sine with sample peaks at 1.0, in dBFS of the interface
    (its peak against the interface's full scale, which for a sine is also its rms against a
    full-scale sine's, AES17), and in dBu, Vrms and Vpp when the interface's full-scale level is
    given;
  * the analog chain at full scale: the 1 kHz gain at -6, -1 and 0 dBFS and over the sweep from
    -30 dBFS, against the -20 dBFS reference (within 0.10 dB), the harmonic distortion at -1 and
    0 dBFS (THD of h2-h10 at or below -60 dB) and the crest's flattening (peak at least 0.99 of the
    fundamental);
  * inter-sample peaks: the fs/4 sine at 45 degrees with sample peaks at 1.0, whose reconstructed
    peak is +3.01 dBTP, against the same tone at -12 dBFS (within 0.05 dB), with the 0-degree
    control at 1.0 (true peak 0 dBTP) and the step sweep from -8.99 to +3.01 dBTP, from which it
    estimates how far above full scale the DAC reproduces a peak; and the full-scale square
    (true peak +2.17 dBTP) as a second witness;
  * the recording: no sample within 0.1 dB of the interface's full scale, the noise floor, and
    which channels carry the program: those where the -20 dBFS sine stands 60 dB above what the
    same 1 kHz fit finds in the program's silence. Hum and other noise away from 1 kHz neither
    count against a channel nor change a measurement, since every tone is fitted at its own
    frequency.

D10's verdict: PASS keeps the limiter's ceiling at C = 1.0; FAIL means the DAC (or the Seed's
output) clips inter-sample peaks, and the ceiling must drop or the detector be oversampled before
sound revision 10 is minted. INCONCLUSIVE means the 12 kHz path compresses even without an
inter-sample peak, so the analog chain needs looking at first. The RESULT line is PASS (exit 0),
FAIL or INCONCLUSIVE (exit 1), or INVALID (exit 2: the recording clipped at the interface, or holds
no channel with the program); exit 2 also when the recording cannot be read or does not hold the
whole program.

PROGRAM below is firmware/live/Tone.h's kTestProgram (kProgramVersion 1);
tools/hil/test_dac_test.py checks the two against each other through the host test's
--program-json.
"""
import argparse
import array
import json
import math
import operator
import struct
import sys

PROGRAM_VERSION = 1
SEED_RATE = 48000
PERIOD = 48
FULL_SCALE = 1 << 23
CODEC_MAX = 8388482  # libDaisy's f2s24(1.0)
SINE_Q30 = (0, 140151432, 277904834, 410903207, 536870912, 653652607, 759250125, 851856663, 929887697,
            992008094, 1037154959, 1064555814, 1073741824)
GAP = SEED_RATE // 2

# (wave, first level in centi-dB, step in centi-dB, steps, frames per step): Tone.h's kTestProgram.
PROGRAM = (
    ("silence", 0, 0, 1, SEED_RATE),
    ("sine", -2000, 0, 1, 2 * SEED_RATE),
    ("silence", 0, 0, 1, GAP),
    ("sine", -600, 0, 1, 2 * SEED_RATE),
    ("silence", 0, 0, 1, GAP),
    ("sine", -100, 0, 1, 2 * SEED_RATE),
    ("silence", 0, 0, 1, GAP),
    ("sine", 0, 0, 1, 2 * SEED_RATE),
    ("silence", 0, 0, 1, GAP),
    ("isp", -1200, 0, 1, 2 * SEED_RATE),
    ("silence", 0, 0, 1, GAP),
    ("fs4", 0, 0, 1, 2 * SEED_RATE),
    ("silence", 0, 0, 1, GAP),
    ("isp", -301, 0, 1, 2 * SEED_RATE),
    ("silence", 0, 0, 1, GAP),
    ("isp", 0, 0, 1, 2 * SEED_RATE),
    ("silence", 0, 0, 1, GAP),
    ("square", 0, 0, 1, 2 * SEED_RATE),
    ("silence", 0, 0, 1, GAP),
    ("silence", 0, 0, 1, 2 * SEED_RATE),
    ("silence", 0, 0, 1, GAP),
    ("sine", -4000, 100, 41, GAP),
    ("silence", 0, 0, 1, GAP),
    ("isp", -1200, 50, 25, GAP),
)

# Criteria.
LINEARITY_DB = 0.10      # 1 kHz gain against the -20 dBFS reference
SWEEP_FLOOR_CDB = -3000  # sweep steps below this are reported, not judged
THD_LIMIT_DB = -60.0     # h2-h10 at -1 and 0 dBFS
FLAT_LIMIT = 0.99        # peak / fundamental at -1 and 0 dBFS
ISP_DB = 0.05            # fs/4 gain against its -12 dBFS reference
RECORDING_CLIP = 10 ** (-0.1 / 20)  # a sample this close to the interface's full scale
MIN_SNR_DB = 60.0        # the 1 kHz reference above the silence's 1 kHz, for a channel to count
DBU_VOLTS = math.sqrt(0.6)  # 0 dBu: 1 mW into 600 ohms, 0.7746 Vrms


class DacTestError(Exception):
    """The recording cannot be analysed (exit 2)."""


# ---- The generator's arithmetic (Tone.h) -------------------------------------------------

def level_code(centi_db):
    """round(2^23 * 10^(centiDb / 2000)), half away from zero; 0 dB is 2^23."""
    if centi_db >= 0:
        return FULL_SCALE
    return int(math.floor(math.ldexp(10.0 ** (centi_db / 2000.0), 23) + 0.5))


def code(wave, q, phase):
    """The code of `wave` at `phase` (0..47) for the level code q."""
    if wave == "sine":
        half = phase if phase < 24 else phase - 24
        k = half if half <= 12 else 24 - half
        v = (q * SINE_Q30[k] + (1 << 29)) >> 30
        return v if phase < 24 else -v
    if wave == "isp":
        return q if (phase & 2) == 0 else -q
    if wave == "fs4":
        return 0 if (phase & 1) == 0 else (q if (phase & 2) == 0 else -q)
    if wave == "square":
        return q if phase < PERIOD // 2 else -q
    return 0


def codec_code(c):
    return max(-CODEC_MAX, min(CODEC_MAX, c))


def tone_bin(wave):
    """The DFT bin (of 48) of a wave's fundamental: 1 kHz, or fs/4."""
    return 12 if wave in ("isp", "fs4") else 1


def digital_amplitude(wave, q, harmonic=1):
    """The amplitude, as a fraction of full scale, of the given harmonic of the wave's fundamental
    in the codes the codec receives (one period's DFT)."""
    b = tone_bin(wave) * harmonic
    if b >= PERIOD // 2:
        return 0.0
    re = im = 0.0
    for n in range(PERIOD):
        c = codec_code(code(wave, q, n))
        re += c * math.cos(2 * math.pi * b * n / PERIOD)
        im -= c * math.sin(2 * math.pi * b * n / PERIOD)
    return 2.0 / PERIOD * math.hypot(re, im) / FULL_SCALE


class Step:
    def __init__(self, index, segment, wave, centi_db, frames, start, in_segment, steps):
        self.index, self.segment, self.wave, self.centi_db = index, segment, wave, centi_db
        self.frames, self.start, self.in_segment, self.steps = frames, start, in_segment, steps
        self.q = 0 if wave == "silence" else level_code(centi_db)

    @property
    def sweep(self):
        return self.steps > 1

    def label(self):
        if self.wave == "silence":
            return "silence"
        db = "%+.2f" % (self.centi_db / 100.0) if self.centi_db else "0.00"
        if self.wave == "sine":
            return "1 kHz sine, %s dBFS" % db
        if self.wave == "square":
            return "1 kHz square, %s dBFS" % db
        return "fs/4 at %d deg, %s dBFS (true peak %s dBTP)" % (
            45 if self.wave == "isp" else 0, db, true_peak_text(self))


def true_peak_text(step):
    tp = step.centi_db / 100.0 + (20 * math.log10(math.sqrt(2)) if step.wave == "isp" else 0.0)
    return "%+.2f" % tp if abs(tp) >= 0.005 else "0.00"


def flatten_program():
    steps, start = [], 0
    for seg, (wave, first, inc, count, frames) in enumerate(PROGRAM):
        for i in range(count):
            steps.append(Step(len(steps), seg, wave, 0 if wave == "silence" else first + i * inc, frames, start,
                              i, count))
            start += frames
    return steps


STEPS = flatten_program()
PROGRAM_FRAMES = sum(s.frames for s in STEPS)


def find_step(wave, centi_db, sweep=False):
    for s in STEPS:
        if s.wave == wave and s.centi_db == centi_db and s.sweep == sweep:
            return s
    raise KeyError((wave, centi_db, sweep))


REF_1K = find_step("sine", -2000)
FIXED_SINES = [find_step("sine", c) for c in (-600, -100, 0)]
SINE_0 = find_step("sine", 0)
SINE_M1 = find_step("sine", -100)
REF_12K = find_step("isp", -1200)
CONTROL = find_step("fs4", 0)
ISP_0DBTP = find_step("isp", -301)
ISP_TEST = find_step("isp", 0)
SQUARE = find_step("square", 0)
NOISE = next(s for s in STEPS if s.wave == "silence" and s.frames == 2 * SEED_RATE)
SINE_SWEEP = [s for s in STEPS if s.sweep and s.wave == "sine"]
ISP_SWEEP = [s for s in STEPS if s.sweep and s.wave == "isp"]
# The loud starts the program is found by: every tone after a silence, but the sine sweep's first
# step (-40 dBFS), which a noisy interface may not lift far enough above its floor.
ONSETS = [s for s in STEPS[1:] if s.wave != "silence" and STEPS[s.index - 1].wave == "silence"
          and s.centi_db >= -2000]


# ---- WAV -----------------------------------------------------------------------------------

_GUID_TAIL = b"\x00\x00\x00\x00\x10\x00\x80\x00\x00\xaa\x00\x38\x9b\x71"


class Recording:
    def __init__(self, rate, channels, scale, description, seconds):
        self.rate, self.channels, self.scale = rate, channels, scale
        self.description, self.seconds = description, seconds


def read_wav(path):
    """A WAV file's channels as arrays of its integer or float samples, with the scale that maps
    them to the interface's full scale (+-1.0)."""
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError as e:
        raise DacTestError("cannot read %s: %s" % (path, e))
    if len(data) < 12:
        raise DacTestError("%s is not a WAV file" % path)
    riff, _, wave = struct.unpack_from("<4sI4s", data, 0)
    if riff not in (b"RIFF", b"RF64") or wave != b"WAVE":
        raise DacTestError("%s is not a WAV file (no RIFF/WAVE header)" % path)
    pos, fmt, pcm, rf64_data = 12, None, None, None
    while pos + 8 <= len(data):
        cid, size = struct.unpack_from("<4sI", data, pos)
        body = pos + 8
        if cid == b"ds64" and size >= 16:
            rf64_data = struct.unpack_from("<Q", data, body + 8)[0]
        elif cid == b"fmt ":
            fmt = data[body:body + size]
        elif cid == b"data":
            n = size
            if riff == b"RF64" and size == 0xFFFFFFFF and rf64_data is not None:
                n = rf64_data
            if n in (0, 0xFFFFFFFF) or body + n > len(data):
                n = len(data) - body  # streamed or truncated: what is there
            pcm = data[body:body + n]
            if fmt is not None:
                break
            size = n
        pos = body + size + (size & 1)
    if fmt is None or len(fmt) < 16:
        raise DacTestError("%s has no fmt chunk" % path)
    if pcm is None:
        raise DacTestError("%s has no data chunk" % path)
    tag, nch, rate, _, align, bits = struct.unpack_from("<HHIIHH", fmt, 0)
    valid = bits
    if tag == 0xFFFE:
        if len(fmt) < 40 or fmt[26:40] != _GUID_TAIL:
            raise DacTestError("%s: WAVE_FORMAT_EXTENSIBLE with an unknown sub-format" % path)
        valid = struct.unpack_from("<H", fmt, 18)[0] or bits
        tag = struct.unpack_from("<H", fmt, 24)[0]
    if nch < 1 or rate < 1 or bits % 8 or align != nch * bits // 8:
        raise DacTestError("%s: unsupported layout (%d channels, %d bits, block align %d)" % (path, nch, bits, align))
    width = bits // 8
    frames = len(pcm) // (width * nch)
    pcm = pcm[:frames * width * nch]
    if tag == 1 and bits == 16:
        arr, scale = array.array("h"), 1.0 / 32768
        arr.frombytes(pcm)
    elif tag == 1 and bits == 24:
        wide = bytearray(len(pcm) // 3 * 4)
        wide[1::4], wide[2::4], wide[3::4] = pcm[0::3], pcm[1::3], pcm[2::3]
        arr, scale = array.array("i"), 1.0 / 2147483648
        arr.frombytes(bytes(wide))
    elif tag == 1 and bits == 32:
        arr, scale = array.array("i"), 1.0 / 2147483648
        arr.frombytes(pcm)
    elif tag == 3 and bits == 32:
        arr, scale = array.array("f"), 1.0
        arr.frombytes(pcm)
    elif tag == 3 and bits == 64:
        arr, scale = array.array("d"), 1.0
        arr.frombytes(pcm)
    else:
        raise DacTestError("%s: unsupported sample format (tag %d, %d bits)" % (path, tag, bits))
    if arr.itemsize != (4 if bits == 24 else width):
        raise DacTestError("this Python's array type has the wrong item size for %d-bit samples" % bits)
    if sys.byteorder == "big":
        arr.byteswap()
    kind = "float" if tag == 3 else "PCM"
    desc = "%d Hz, %d-bit %s%s, %d channel%s" % (rate, bits, kind, "" if valid == bits else " (%d valid)" % valid,
                                                 nch, "" if nch == 1 else "s")
    return Recording(rate, [arr[c::nch] for c in range(nch)], scale, desc, frames / float(rate))


# ---- Measurement ---------------------------------------------------------------------------

def fit(x, start, n, omega, harmonics=()):
    """Least-squares fit of a cos(omega m) + b sin(omega m) + dc to x[start:start+n], m the absolute
    sample index, in x's units. Returns amplitude, phase (radians, of the cosine at m = 0), dc,
    the residual's mean square, the peak of |x - dc|, and the amplitude of each requested
    harmonic of omega in the residual."""
    seg = x[start:start + n]
    if len(seg) != n or n < 16:
        raise DacTestError("a measurement window runs past the end of the recording")
    sx = float(sum(seg))
    sxx = float(sum(map(operator.mul, seg, seg)))
    sxc = sxs = scc = sss = scs = sc = ss = 0.0
    cw, sw = math.cos(omega), math.sin(omega)
    m = start
    i = 0
    while i < n:
        c, s = math.cos(omega * m), math.sin(omega * m)  # exact restart every block
        for v in seg[i:i + 4096]:
            sxc += v * c
            sxs += v * s
            scc += c * c
            sss += s * s
            scs += c * s
            sc += c
            ss += s
            c, s = c * cw - s * sw, c * sw + s * cw
        i += 4096
        m = start + i
    a, b, dc = _solve3(((scc, scs, sc), (scs, sss, ss), (sc, ss, float(n))), (sxc, sxs, sx))
    resid = max(0.0, (sxx - (a * sxc + b * sxs + dc * sx)) / n)
    hi, lo = max(seg), min(seg)
    out = {"amplitude": math.hypot(a, b), "phase": math.atan2(-b, a), "dc": dc, "residual_ms": resid,
           "peak": max(hi - dc, dc - lo)}
    if harmonics:
        out["harmonics"] = _harmonics(seg, start, omega, a, b, dc, harmonics)
    return out


def _solve3(m, y):
    (a, b, c), (d, e, f), (g, h, k) = m
    det = a * (e * k - f * h) - b * (d * k - f * g) + c * (d * h - e * g)
    if det == 0:
        raise DacTestError("a measurement window is degenerate")
    inv = ((e * k - f * h, c * h - b * k, b * f - c * e), (f * g - d * k, a * k - c * g, c * d - a * f),
           (d * h - e * g, b * g - a * h, a * e - b * d))
    return tuple(sum(inv[r][j] * y[j] for j in range(3)) / det for r in range(3))


def _harmonics(seg, start, omega, a, b, dc, hs):
    """Amplitude of each harmonic h * omega in seg minus the fitted fundamental (projection)."""
    n = len(seg)
    acc = {h: [0.0, 0.0] for h in hs}
    rot = {h: (math.cos(h * omega), math.sin(h * omega)) for h in hs}
    cw, sw = math.cos(omega), math.sin(omega)
    i = 0
    while i < n:
        m = start + i
        c1, s1 = math.cos(omega * m), math.sin(omega * m)
        state = {h: [math.cos(h * omega * m), math.sin(h * omega * m)] for h in hs}
        for v in seg[i:i + 4096]:
            r = v - a * c1 - b * s1 - dc
            for h in hs:
                st = state[h]
                ac = acc[h]
                ac[0] += r * st[0]
                ac[1] += r * st[1]
                ch, sh = rot[h]
                st[0], st[1] = st[0] * ch - st[1] * sh, st[0] * sh + st[1] * ch
            c1, s1 = c1 * cw - s1 * sw, c1 * sw + s1 * cw
        i += 4096
    return {h: 2.0 / n * math.hypot(*acc[h]) for h in hs}


def db(x):
    return 20 * math.log10(x) if x > 0 else float("-inf")


class Timeline:
    """Program frames to recording samples: t = t0 + frame / 48000 / rho seconds."""

    def __init__(self, rate, t0, rho):
        self.rate, self.t0, self.rho = rate, t0, rho

    def sample(self, frame):
        return int(round((self.t0 + frame / float(SEED_RATE) / self.rho) * self.rate))

    def window(self, step, lead, length):
        """The samples of `step` from `lead` to `lead + length` seconds of program time in."""
        a = self.sample(step.start + int(lead * SEED_RATE))
        b = self.sample(step.start + int((lead + length) * SEED_RATE))
        return a, b - a


def envelope(rec, block):
    """Mean square per block of every channel together, in full-scale units, about each
    channel's DC offset, so that an offset on the interface's input does not raise the
    silences. The offset is the mean of the channel's blocks other than exact zeros (the
    program's tones are whole periods of zero mean); a block of exact zeros, a DAW's padding,
    stays 0. Returns the envelope and the offsets, in the channels' units."""
    nblocks = len(rec.channels[0]) // block
    env = [0.0] * nblocks
    offsets = []
    for ch in rec.channels:
        sums, squares = [], []
        for i in range(nblocks):
            seg = ch[i * block:(i + 1) * block]
            sums.append(sum(seg))
            squares.append(sum(map(operator.mul, seg, seg)))
        heard = [i for i in range(nblocks) if squares[i]]
        dc = float(sum(sums[i] for i in heard)) / (block * len(heard)) if heard else 0.0
        offsets.append(dc)
        for i in heard:
            env[i] += max(float(squares[i]) - 2.0 * dc * float(sums[i]) + block * dc * dc, 0.0)
    k = rec.scale * rec.scale / (block * len(rec.channels))
    return [e * k for e in env], offsets


def locate(rec):
    """Finds the program: its start in the recording (seconds) and a first time scale."""
    block = max(1, rec.rate // 1000)
    env, dc = envelope(rec, block)
    if not env:
        raise DacTestError("the recording is empty")
    # The quietest 5 % of the blocks, leaving out exact zeros (a DAW's padding before the
    # interface starts): the program's silences are 14 % of it.
    heard = sorted(e for e in env if e > 0.0) or [0.0]
    floor = max(heard[len(heard) // 20], 1e-12)
    loud = floor * 10 ** 1.5
    quiet_blocks = int(0.2 * rec.rate / block)
    onsets, run = [], quiet_blocks
    for i, e in enumerate(env):
        if e > loud:
            if run >= quiet_blocks:
                onsets.append(_refine_onset(rec, max(0, (i - 1) * block), (i + 1) * block, math.sqrt(loud), dc))
            run = 0
        else:
            run += 1
    expect = [s.start / float(SEED_RATE) for s in ONSETS]
    best, complete = None, False
    for o in onsets:
        t0 = o - expect[0]
        # Onsets past the recording's end cannot match: a recording that stops early is still
        # found, and refused below for its length.
        due = [e for e in expect if t0 + e < rec.seconds - 0.05]
        pairs = []
        for e in due:
            near = min(onsets, key=lambda d: abs(d - (t0 + e)))
            if abs(near - (t0 + e)) <= 0.05:
                pairs.append((e, near))
        if len(pairs) == len(due) and len(pairs) >= 6:
            best, complete = pairs, True
            break
        if best is None or len(pairs) > len(best):
            best = pairs
    if not complete:
        found = ", ".join("%.3f" % o for o in onsets[:24]) or "none"
        raise DacTestError(
            "could not find the DAC test program (v%d): %d of its %d tone onsets matched. Was `tone test` "
            "recorded from before it started, to the end (59.5 s)? Onsets found (s): %s" % (
                PROGRAM_VERSION, len(best or []), len(expect), found))
    # Least squares: onset = t0 + e * kappa.
    n = float(len(best))
    me = sum(e for e, _ in best) / n
    md = sum(d for _, d in best) / n
    kappa = sum((e - me) * (d - md) for e, d in best) / sum((e - me) ** 2 for e, _ in best)
    t0 = md - kappa * me
    worst = max(abs(d - (t0 + kappa * e)) for e, d in best)
    return t0, kappa, worst, floor


def _refine_onset(rec, a, b, threshold, dc):
    level = threshold / rec.scale
    best = b
    for ch, offset in zip(rec.channels, dc):
        for i in range(a, min(b, len(ch))):
            if abs(ch[i] - offset) > level:
                best = min(best, i)
                break
    return best / float(rec.rate)


def estimate_rho(rec, ch, t0, kappa):
    """The Seed's 1 kHz as the interface's clock sees it, over the reference sine: rho = f / 1000."""
    rho = 1.0 / kappa
    x = rec.channels[ch]
    for _ in range(2):
        tl = Timeline(rec.rate, t0, rho)
        omega = 2 * math.pi * 1000.0 * rho / rec.rate
        width = max(64, int(0.02 * rec.rate))
        points = []
        for k in range(15):
            a, _ = tl.window(REF_1K, 0.25 + k * 0.1, 0.0)
            f = fit(x, a, width, omega)
            points.append((a + width / 2.0, f["phase"]))
        unwrapped, prev = [], None
        for m, ph in points:
            if prev is not None:
                while ph - prev > math.pi:
                    ph -= 2 * math.pi
                while ph - prev < -math.pi:
                    ph += 2 * math.pi
            unwrapped.append((m, ph))
            prev = ph
        mm = sum(m for m, _ in unwrapped) / len(unwrapped)
        mp = sum(p for _, p in unwrapped) / len(unwrapped)
        slope = sum((m - mm) * (p - mp) for m, p in unwrapped) / sum((m - mm) ** 2 for m, _ in unwrapped)
        # x ~ cos(omega m + phase): a phase that grows by `slope` a sample is that much more omega.
        rho = (omega + slope) * rec.rate / (2 * math.pi * 1000.0)
    return rho


def measure_channel(rec, ch, tl):
    x, scale, rate = rec.channels[ch], rec.scale, rec.rate
    out = {"channel": ch + 1}

    def tone(step, lead, length, harmonics=()):
        a, n = tl.window(step, lead, length)
        f0 = 1000.0 if tone_bin(step.wave) == 1 else 12000.0
        omega = 2 * math.pi * f0 * tl.rho / rate
        hs = tuple(h for h in harmonics if h * f0 * tl.rho < 0.45 * rate)
        r = fit(x, a, n, omega, hs)
        expected = digital_amplitude(step.wave, step.q)
        amp = r["amplitude"] * scale
        res = {"label": step.label(), "wave": step.wave, "dbfs": step.centi_db / 100.0,
               "recordedPeakDbfs": db(amp), "gain": amp / expected if expected else 0.0,
               "dcDbfs": db(abs(r["dc"] * scale)), "flatness": r["peak"] / r["amplitude"] if r["amplitude"] else 0.0,
               "thdnDb": db(math.sqrt(2 * r["residual_ms"]) / r["amplitude"]) if r["amplitude"] else 0.0,
               "windowPeakDbfs": db((r["peak"] + abs(r["dc"])) * scale)}
        if hs:
            res["harmonicsDb"] = {str(h): db(v / r["amplitude"]) for h, v in r["harmonics"].items()}
            thd = math.sqrt(sum(v * v for h, v in r["harmonics"].items() if h <= 10)) / r["amplitude"]
            res["thdDb"] = db(thd)
            res["harmonicAmplitudes"] = {str(h): v * scale for h, v in r["harmonics"].items()}
        return res

    long_lead, long_len = 0.75, 0.5   # of a 2 s segment
    step_lead, step_len = 0.125, 0.25  # of a 0.5 s sweep step
    ref = tone(REF_1K, long_lead, long_len)
    out["reference1k"] = ref
    noise_a, noise_n = tl.window(NOISE, 0.5, 1.0)
    nseg = x[noise_a:noise_a + noise_n]
    if len(nseg) != noise_n:
        raise DacTestError("the recording ends before the program's noise-floor segment")
    nmean = float(sum(nseg)) / noise_n
    nms = max(float(sum(map(operator.mul, nseg, nseg))) / noise_n - nmean * nmean, 0.0) * scale * scale
    out["noiseFloorDbfs"] = 10 * math.log10(max(nms, 1e-30))
    # Whether this input carries the program, judged at 1 kHz only: the reference's amplitude
    # against the rms of what the same fit (frequency and length) finds in three windows of the
    # program's silence. Hum and noise away from 1 kHz do not count, as they do not in any
    # measurement; 60 dB leaves the 0.10 dB, 0.05 dB and -60 dB criteria measuring the chain,
    # not the noise, and refuses another input's crosstalk.
    omega_1k = 2 * math.pi * 1000.0 * tl.rho / rate
    silence = []
    for k in range(3):
        a, n = tl.window(NOISE, 0.25 + k * long_len, long_len)
        silence.append(fit(x, a, n, omega_1k)["amplitude"] ** 2)
    noise_1k = math.sqrt(sum(silence) / len(silence)) * scale
    out["noise1kDbfs"] = db(noise_1k)
    snr = db(ref["gain"] * digital_amplitude("sine", REF_1K.q) / max(noise_1k, 1e-30))
    out["referenceSnrDb"] = snr
    if snr < MIN_SNR_DB:
        out["signal"] = False
        return out
    out["signal"] = True
    g1 = ref["gain"]
    sines = {}
    for s in FIXED_SINES:
        hs = range(2, 11) if s in (SINE_0, SINE_M1) else ()
        m = tone(s, long_lead, long_len, hs)
        m["deltaDb"] = db(m["gain"] / g1)
        sines[s.centi_db] = m
    out["sines"] = sines
    out["zeroDbfs"] = sines[0]
    sweep = []
    for s in SINE_SWEEP:
        m = tone(s, step_lead, step_len)
        m["deltaDb"] = db(m["gain"] / g1)
        sweep.append(m)
    out["sineSweep"] = sweep
    sq = tone(SQUARE, long_lead, long_len, (3, 5))
    sq["deltaDb"] = db(sq["gain"] / g1)
    for h in (3, 5):
        if "harmonicAmplitudes" in sq and str(h) in sq["harmonicAmplitudes"]:
            want = digital_amplitude("square", SQUARE.q, h) * g1
            sq["h%dDeltaDb" % h] = db(sq["harmonicAmplitudes"][str(h)] / want)
    out["square"] = sq
    ref12 = tone(REF_12K, long_lead, long_len, (3,))
    out["reference12k"] = ref12
    out["response12kDb"] = db(ref12["gain"] / g1)
    g12 = ref12["gain"]
    isp = {}
    for key, s in (("control", CONTROL), ("zeroDbtp", ISP_0DBTP), ("test", ISP_TEST)):
        m = tone(s, long_lead, long_len, (3,))
        m["deltaDb"] = db(m["gain"] / g12)
        isp[key] = m
    out["isp"] = isp
    isweep = []
    for s in ISP_SWEEP:
        m = tone(s, step_lead, step_len)
        m["deltaDb"] = db(m["gain"] / g12)
        m["truePeakDbtp"] = s.centi_db / 100.0 + 20 * math.log10(math.sqrt(2))
        isweep.append(m)
    out["ispSweep"] = isweep
    return out


def judge_channel(m, cal):
    """The criteria for one channel: a list of (name, verdict, text)."""
    zero = m["zeroDbfs"]
    level = zero["recordedPeakDbfs"]
    lines = []
    text = "Seed 0 dBFS (1 kHz sine, sample peaks 1.0) = %.2f dBFS on the interface" % level
    if cal is not None:
        # cal is the rms level of the sine whose peaks reach the interface's full scale, so the
        # sine recorded with its peaks at `level` dBFS is cal + level, in dBu rms.
        dbu = cal + level
        vrms = DBU_VOLTS * 10 ** (dbu / 20)
        text += " = %+.2f dBu = %.3f Vrms = %.2f Vpp" % (dbu, vrms, vrms * 2 * math.sqrt(2))
        m["zeroDbfsDbu"], m["zeroDbfsVrms"], m["zeroDbfsVpp"] = dbu, vrms, vrms * 2 * math.sqrt(2)
    if level < -30.0:
        text += " (the interface's gain is low: about -12 dBFS here gives a lower noise floor)"
    elif level > -6.0 and not m["recordingClipped"]:
        text += " (the interface's gain is high: the +3.01 dBTP test needs headroom; about -12 dBFS here is safer)"
    lines.append(("level", "MEASURED", text))

    # The analog chain at full scale.
    worst = max((abs(s["deltaDb"]), c) for c, s in m["sines"].items())
    judged = [s for s in m["sineSweep"] if s["dbfs"] * 100 >= SWEEP_FLOOR_CDB]
    sweep_worst = max(abs(s["deltaDb"]) for s in judged)
    thd = max(m["sines"][c].get("thdDb", float("-inf")) for c in (-100, 0))
    flat = min(m["sines"][c]["flatness"] for c in (-100, 0))
    problems = []
    if worst[0] > LINEARITY_DB:
        problems.append("the 1 kHz gain at %.2f dBFS is %+.3f dB off the -20 dBFS reference" % (
            worst[1] / 100.0, m["sines"][worst[1]]["deltaDb"]))
    if sweep_worst > LINEARITY_DB:
        problems.append("the sweep departs %.3f dB from the reference" % sweep_worst)
    if thd > THD_LIMIT_DB:
        problems.append("THD %.1f dB at -1 or 0 dBFS (limit %.0f dB)" % (thd, THD_LIMIT_DB))
    if flat < FLAT_LIMIT:
        problems.append("the crest is flattened (peak %.3f of the fundamental)" % flat)
    ok = not problems
    lines.append(("chain", "PASS" if ok else "FAIL",
                  ("1 kHz gain within %.2f dB of the -20 dBFS reference at -6, -1 and 0 dBFS (worst %.3f) and over "
                   "the sweep from -30 dBFS (worst %.3f); THD %.1f dB; peak %.3f of the fundamental: no clipping "
                   "or flattening at full scale" % (LINEARITY_DB, worst[0], sweep_worst, thd, flat)) if ok else
                  "; ".join(problems) + ". The Seed's output or the interface's input compresses: record again "
                  "with the interface's gain 6 dB lower; a fault that stays is the Seed's"))

    # Inter-sample peaks (D10).
    isp = m["isp"]
    control, zero_tp, test = isp["control"]["deltaDb"], isp["zeroDbtp"]["deltaDb"], isp["test"]["deltaDb"]
    clean = None
    for s in m["ispSweep"]:
        if s["deltaDb"] < -ISP_DB:
            break
        clean = s["truePeakDbtp"]
    m["ispCleanToDbtp"] = clean
    if abs(control) > ISP_DB or zero_tp < -ISP_DB:
        verdict = "INCONCLUSIVE"
        text = ("the 12 kHz path already departs at 0 dBTP (fs/4 at 0 deg, 1.0: %+.3f dB; at 45 deg, -3.01 dBFS: "
                "%+.3f dB against the -12 dBFS reference): see the analog chain, then repeat" % (control, zero_tp))
    elif test >= -ISP_DB:
        verdict = "PASS"
        text = ("fs/4 at 45 deg with sample peaks at 1.0 (true peak +3.01 dBTP) is %+.3f dB against its -12 dBFS "
                "reference (limit -%.2f): the DAC reproduces it unclipped, and the Seed's output swings DAC full "
                "scale + 3 dB at 12 kHz. D10: keep the ceiling at C = 1.0" % (test, ISP_DB))
    else:
        verdict = "FAIL"
        if clean is None:
            reach = "not even the sweep's first step"
            ceiling = ""
        else:
            reach = "true peaks up to about %+.2f dBTP" % clean
            ceiling = ("; a ceiling C <= %.2f dBFS (%.4f) keeps an fs/4 inter-sample peak inside that" % (
                clean - 20 * math.log10(math.sqrt(2)), 10 ** ((clean - 20 * math.log10(math.sqrt(2))) / 20)))
        text = ("fs/4 at 45 deg with sample peaks at 1.0 (true peak +3.01 dBTP) is compressed by %.3f dB against its "
                "-12 dBFS reference (the 0 deg control: %+.3f dB): the DAC or the Seed's output clips inter-sample "
                "peaks. The sweep reproduces %s (within %.2f dB)%s. D10: lower C or oversample the detector "
                "before revision 10" % (-test, control, reach, ISP_DB, ceiling))
    lines.append(("isp", verdict, text))
    if m["recordingClipped"]:
        # The interface clipped: nothing above its full scale can be judged.
        lower = max(1, int(math.ceil(level + 12.0)))
        why = ("the recording reaches the interface's full scale (%.2f dBFS), so this cannot be judged: lower the "
               "interface's gain by %d dB (the 0 dBFS sine to about -12 dBFS) and record again" % (
                   m["programPeakDbfs"], lower))
        lines = [(n, "INCONCLUSIVE", why) if n in ("chain", "isp") else (n, v, t) for n, v, t in lines]
    sq = m["square"]
    sq_text = "full-scale square (true peak +2.17 dBTP): fundamental %+.3f dB against the 1 kHz reference" % sq[
        "deltaDb"]
    for h in (3, 5):
        if "h%dDeltaDb" % h in sq:
            sq_text += ", h%d %+.2f dB" % (h, sq["h%dDeltaDb" % h])
    sq_text += " against the codes' (an inter-sample clip lowers them: -0.18, -0.51, -0.81 dB at a clip at 1.0)"
    lines.append(("square", "INFO", sq_text))
    h36 = isp["test"].get("harmonicsDb", {}).get("3")
    if h36 is not None:
        lines.append(("isp36k", "INFO",
                      "36 kHz (the fs/4 tone's third harmonic, and the DAC's image at fs - fs/4): %.1f dB re the "
                      "fundamental at +3.01 dBTP, %.1f dB at the -12 dBFS reference" % (
                          h36, m["reference12k"]["harmonicsDb"]["3"])))
    return lines


def analyse(path, cal=None):
    rec = read_wav(path)
    if rec.rate < 44100:
        raise DacTestError("%s is recorded at %d Hz: record at 44.1 kHz or more (48 or 96 kHz)" % (path, rec.rate))
    t0, kappa, worst, _ = locate(rec)
    end_needed = t0 + PROGRAM_FRAMES / float(SEED_RATE) * kappa
    if end_needed > rec.seconds + 0.01:
        raise DacTestError("the recording stops %.2f s before the program's end: record all 59.5 s" % (
            end_needed - rec.seconds))
    # The clock: from the channel where the reference sine is strongest.
    best_ch, best_amp = 0, -1.0
    tl0 = Timeline(rec.rate, t0, 1.0 / kappa)
    for c in range(len(rec.channels)):
        a, n = tl0.window(REF_1K, 0.75, 0.5)
        amp = fit(rec.channels[c], a, n, 2 * math.pi * 1000.0 / kappa / rec.rate)["amplitude"]
        if amp > best_amp:
            best_ch, best_amp = c, amp
    rho = estimate_rho(rec, best_ch, t0, kappa)
    tl = Timeline(rec.rate, t0, rho)
    report = {"file": path, "format": rec.description, "seconds": rec.seconds, "programVersion": PROGRAM_VERSION,
              "programStartSeconds": t0, "onsetResidualMs": worst * 1000.0, "clockPpm": (rho - 1.0) * 1e6,
              "channels": []}
    # The recording's own full scale: the loudest sample inside the program.
    a = tl.sample(0)
    b = min(tl.sample(PROGRAM_FRAMES), len(rec.channels[0]))
    for c in range(len(rec.channels)):
        seg = rec.channels[c][a:b]
        peak = max(max(seg), -min(seg)) * rec.scale
        m = measure_channel(rec, c, tl)
        m["programPeakDbfs"] = db(peak)
        m["recordingClipped"] = peak >= RECORDING_CLIP
        if m["signal"]:
            m["criteria"] = judge_channel(m, cal)
        report["channels"].append(m)
    return report


def overall(report):
    live = [m for m in report["channels"] if m["signal"]]
    rec_ok = bool(live) and not any(m["recordingClipped"] for m in live)
    verdicts = {}
    for name in ("chain", "isp"):
        vs = [v for m in live for (n, v, _) in m["criteria"] if n == name]
        if "FAIL" in vs:
            verdicts[name] = "FAIL"
        elif "INCONCLUSIVE" in vs or not vs:
            verdicts[name] = "INCONCLUSIVE"
        else:
            verdicts[name] = "PASS"
    if not rec_ok:
        result = "INVALID"  # a recording that clipped, or holds no channel with the program
    elif verdicts["isp"] == "PASS" and verdicts["chain"] == "PASS":
        result = "PASS"
    elif "FAIL" in verdicts.values():
        result = "FAIL"
    else:
        result = "INCONCLUSIVE"
    report["recordingOk"] = rec_ok
    report["verdicts"] = verdicts
    report["result"] = result
    return result


def print_report(report, steps, out=None):
    w = (out or sys.stdout).write
    w("DAC test (output-limiter.md D10): %s\n" % report["file"])
    w("  %s, %.2f s\n" % (report["format"], report["seconds"]))
    w("  the DAC test program v%d starts at %.3f s (onsets within %.2f ms of the schedule); the Seed's clock is "
      "%+.1f ppm against the interface's\n" % (report["programVersion"], report["programStartSeconds"],
                                               report["onsetResidualMs"], report["clockPpm"]))
    if abs(report["clockPpm"]) > 500:
        w("  (more than 500 ppm: is the file's sample rate the one the interface recorded at?)\n")
    for m in report["channels"]:
        w("\nChannel %d%s\n" % (m["channel"], {1: " (Seed pin 18, Audio Out L)", 2: " (Seed pin 19, Audio Out R)"}.get(
            m["channel"], "")))
        w("  noise floor %.1f dBFS rms; program peak %.2f dBFS%s\n" % (
            m["noiseFloorDbfs"], m["programPeakDbfs"],
            "  ** the recording clipped: lower the interface's gain and record again **" if m["recordingClipped"] else ""))
        if not m["signal"]:
            w("  no usable test signal (the -20 dBFS sine is %.1f dB above the silence at 1 kHz; %.0f dB needed): "
              "check this input's wiring\n" % (m["referenceSnrDb"], MIN_SNR_DB))
            continue
        ref = m["reference1k"]
        w("  %-52s %8.2f dBFS  gain %+7.2f dB (the 1 kHz reference)\n" % (
            ref["label"], ref["recordedPeakDbfs"], db(ref["gain"])))
        for c in (-600, -100, 0):
            s = m["sines"][c]
            extra = ""
            if "thdDb" in s:
                extra = "  THD %.1f dB, THD+N %.1f dB, peak/fund %.4f" % (s["thdDb"], s["thdnDb"], s["flatness"])
            w("  %-52s %8.2f dBFS  %+7.3f dB%s\n" % (s["label"], s["recordedPeakDbfs"], s["deltaDb"], extra))
        sq = m["square"]
        w("  %-52s %8.2f dBFS  %+7.3f dB (fundamental)\n" % (sq["label"], sq["recordedPeakDbfs"], sq["deltaDb"]))
        r12 = m["reference12k"]
        w("  %-52s %8.2f dBFS  gain %+7.2f dB (the 12 kHz reference; %+.2f dB re 1 kHz)\n" % (
            r12["label"], r12["recordedPeakDbfs"], db(r12["gain"]), m["response12kDb"]))
        for key in ("control", "zeroDbtp", "test"):
            s = m["isp"][key]
            w("  %-52s %8.2f dBFS  %+7.3f dB\n" % (s["label"], s["recordedPeakDbfs"], s["deltaDb"]))
        clean = m.get("ispCleanToDbtp")
        w("  fs/4 sweep: within %.2f dB of the reference up to %s\n" % (
            ISP_DB, "%+.2f dBTP" % clean if clean is not None else "none of its steps"))
        judged = [s for s in m["sineSweep"] if s["dbfs"] * 100 >= SWEEP_FLOOR_CDB]
        w("  1 kHz sweep: worst %.3f dB from -30 to 0 dBFS (%.3f dB from -40)\n" % (
            max(abs(s["deltaDb"]) for s in judged), max(abs(s["deltaDb"]) for s in m["sineSweep"])))
        if steps:
            w("  sweep steps (level, recorded peak, against the reference):\n")
            for s in m["sineSweep"]:
                w("    1 kHz %7.2f dBFS  %8.2f dBFS  %+7.3f dB\n" % (s["dbfs"], s["recordedPeakDbfs"], s["deltaDb"]))
            for s in m["ispSweep"]:
                w("    fs/4 45 deg %7.2f dBFS (%+.2f dBTP)  %8.2f dBFS  %+7.3f dB\n" % (
                    s["dbfs"], s["truePeakDbtp"], s["recordedPeakDbfs"], s["deltaDb"]))
        for name, verdict, text in m["criteria"]:
            w("  [%s] %s\n" % (verdict, text))
    w("\nVERDICT\n")
    live = [m for m in report["channels"] if m["signal"]]
    tested = ", ".join(str(m["channel"]) for m in live) or "none"
    silent = [str(m["channel"]) for m in report["channels"] if not m["signal"]]
    w("  recording: %s (channels with the program: %s%s)\n" % (
        "OK" if report["recordingOk"] else "INVALID: it clipped at the interface, or no channel has the program",
        tested,
        "; without: %s" % ", ".join(silent) if silent else ""))
    w("  D10 0 dBFS output level: MEASURED (%s)\n" % "; ".join(
        "ch%d %.2f dBFS peak" % (m["channel"], m["zeroDbfs"]["recordedPeakDbfs"]) for m in live))
    w("  D10 inter-sample peaks: %s\n" % report["verdicts"]["isp"])
    w("  analog chain at full scale: %s\n" % report["verdicts"]["chain"])
    w("RESULT: %s\n" % report["result"])


def _jsonable(obj):
    if isinstance(obj, float):
        return obj if math.isfinite(obj) else None
    if isinstance(obj, dict):
        return {str(k): _jsonable(v) for k, v in obj.items()}
    if isinstance(obj, (list, tuple)):
        return [_jsonable(v) for v in obj]
    return obj


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("recording", help="the WAV file recorded from the Seed's outputs during `tone test`")
    cal = ap.add_mutually_exclusive_group()
    cal.add_argument("--interface-fs-dbu", type=float, metavar="DBU",
                     help="the interface input's level at 0 dBFS at the gain used, in dBu: the rms level of the sine "
                          "whose peaks reach its full scale, its 'maximum input level' (from its manual)")
    cal.add_argument("--interface-fs-vrms", type=float, metavar="VRMS",
                     help="the same in volts rms (the rms of the sine whose peaks reach full scale)")
    ap.add_argument("--steps", action="store_true", help="also print every sweep step")
    ap.add_argument("--json", metavar="PATH", help="write every measurement as JSON for the record")
    args = ap.parse_args(argv)
    fs_dbu = args.interface_fs_dbu
    if args.interface_fs_vrms is not None:
        if args.interface_fs_vrms <= 0:
            ap.error("--interface-fs-vrms must be positive")
        fs_dbu = 20 * math.log10(args.interface_fs_vrms / DBU_VOLTS)
    try:
        report = analyse(args.recording, fs_dbu)
    except DacTestError as e:
        print("dac_test.py: %s" % e, file=sys.stderr)
        return 2
    result = overall(report)
    print_report(report, args.steps)
    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump(_jsonable(report), f, indent=1)
            f.write("\n")
    return {"PASS": 0, "INVALID": 2}.get(result, 1)


if __name__ == "__main__":
    sys.exit(main())
