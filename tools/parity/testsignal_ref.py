#!/usr/bin/env python3
"""Independent reference of the test-signal generator, version 1 (determinism profile §5.13).

A second implementation of the generator's specification (the comments and constants
of dsp/include/brainscape/TestSignal.h and the vector definitions), in a language with
other integer semantics, so the known-answer values in dsp/tests/test_testsignal.cpp
("standard vectors match the independent reference implementation") have a second
derivation. Python integers are exact and `>>` floors, so FloorShift needs no helper;
C++'s truncating division is spelled out where the spec uses it. A kVersion bump
changes this file and the test's values together.

  python3 tools/parity/testsignal_ref.py [FRAMES]

renders every standard vector with activeFrames = FRAMES (default 480000, the test's
10 s) and prints its note count, the FNV-1a 64 the test pins and the SHA-256 of the
float input (interleaved little-endian float32 of q * 2^-23), which is the golden
harness's inputHash for a vector whose render length equals its active span
(soft_notes_10s: 76c5857fbdbf32d9...).
"""
import hashlib
import struct
import sys

M32 = 0xFFFFFFFF
Q23_MAX = (1 << 23) - 1
Q23_MIN = -(1 << 23)
FS = 1 << 23
SAMPLE_RATE = 48000
MAX_VOICES = 16
MAX_PERIOD = 1024
PLUCK_LOSS = 65280  # Q16 gain per string pass on top of the 2-tap average


def mix32(z):
    z = ((z ^ (z >> 16)) * 0x85EBCA6B) & M32
    z = ((z ^ (z >> 13)) * 0xC2B2AE35) & M32
    return z ^ (z >> 16)


def splitmix32(seed, n):
    """Draw n of the SplitMix32 stream whose state starts at seed."""
    return mix32((seed + (n + 1) * 0x9E3779B9) & M32)


def noise24(seed, n):
    return (splitmix32(seed, n) >> 8) - (1 << 23)


def trunc_div(a, b):
    q = abs(a) // abs(b)
    return q if (a >= 0) == (b > 0) else -q


def saturate(v):
    return Q23_MAX if v > Q23_MAX else Q23_MIN if v < Q23_MIN else v


def sine_q30(phase):
    """Refined parabola y + 0.225 * (y|y| - y), y = 4x(1 - |x|), in Q30."""
    x = (phase if phase < 0x80000000 else phase - (1 << 32)) >> 1
    y = (4 * x * ((1 << 30) - abs(x))) >> 30
    y2 = (y * abs(y)) >> 30
    return y + trunc_div(225 * (y2 - y), 1000)


def envelope_q16(note, age):
    env = 65536
    if age < note["attack"]:
        env = 65536 * age // note["attack"]
    remaining = note["length"] - age
    if remaining <= note["release"]:
        env = min(env, 65536 * remaining // note["release"])
    return env


def note(**fields):
    n = dict(kind="noise", start=0, length=0, level=0, pitch=0, seed=0, attack=0, release=0,
             panL=32768, panR=32768)
    n.update(fields)
    return n


PLUCK_PERIODS = [582, 436, 327, 245, 194, 146, 109, 97, 73, 65]
CHORDS = [[582, 389, 291, 231, 194, 146],
          [582, 436, 291, 218, 183, 146],
          [490, 389, 327, 245, 194, 122]]
SCALE_MILLIHZ = [196000, 220000, 246942, 293665, 329628, 391995, 440000]


def plucks(active):
    out, k, start, length = [], 0, 1200, 21600
    while start + length <= active:
        odd = k & 1
        out.append(note(kind="pluck", start=start, length=length,
                        level=(3 << 20) + (splitmix32(0xB5, k) >> 11),
                        pitch=PLUCK_PERIODS[(k * 7) % 10], seed=0x1000 + k, release=960,
                        panL=32768 if odd else 19661, panR=19661 if odd else 32768))
        k, start = k + 1, start + 24000
    return out


def strums(active):
    out, k, start, stagger, length = [], 0, 2400, 576, 86400
    while start + 5 * stagger + length <= active:
        chord, up = CHORDS[k % 3], k & 1
        for s in range(6):
            string = 5 - s if up else s
            out.append(note(kind="pluck", start=start + s * stagger, length=length,
                            level=FS * 3 // 10, pitch=chord[string], seed=0x2000 + 8 * k + string,
                            release=2400, panL=32768 - string * 3000, panR=17768 + string * 3000))
        k, start = k + 1, start + 96000
    return out


def soft_notes(active):
    out, k, start, length = [], 0, 0, 57600
    while start + length <= active:
        odd = k & 1
        out.append(note(kind="tone", start=start, length=length, level=FS // 4,
                        pitch=SCALE_MILLIHZ[(k * 3) % 7], attack=2880, release=14400,
                        panL=32768 if odd else 24576, panR=24576 if odd else 32768))
        k, start = k + 1, start + 28800
    return out


def onset_bursts(active):
    out, k, start = [], 0, 2400
    while True:
        length = 576 + splitmix32(0x0C, k) % 384
        if start + length > active:
            return out
        pan = splitmix32(0x0E, k) % 16384
        out.append(note(kind="noise", start=start, length=length,
                        level=FS // 4 + splitmix32(0x0D, k) % (FS // 2), seed=0x5000 + k,
                        attack=48, release=96, panL=16384 + pan, panR=32768 - pan))
        start += 4320 + splitmix32(0x0B, k) % 2880
        k += 1


def saturation(active):
    out, k, start, length = [], 0, 960, 38400
    while start + length <= active:
        out.append(note(kind="noise", start=start, length=1440, level=3 * FS, seed=0x6000 + k,
                        release=240))
        for t, pitch in enumerate([110000, 164814, 220000]):
            out.append(note(kind="tone", start=start, length=length, level=FS + FS // 2 + FS // 10,
                            pitch=pitch, attack=480, release=2400, panL=32768 - t * 6000,
                            panR=20768 + t * 6000))
        k, start = k + 1, start + 48000
    return out


VECTORS = {"plucks": plucks, "strums": strums, "soft_notes": soft_notes,
           "onset_bursts": onset_bursts, "saturation": saturation, "silence": lambda active: []}


def start_voice(n):
    v = {"note": n, "age": 0, "phase": 0, "pos": 0,
         "inc": ((n["pitch"] << 32) // (SAMPLE_RATE * 1000)) & M32,
         "period": min(max(n["pitch"], 2), MAX_PERIOD)}
    if n["kind"] == "pluck":
        v["ks"] = [(noise24(n["seed"], i) * n["level"]) >> 23 for i in range(v["period"])]
    return v


def next_sample(v):
    """The voice's next mono Q23 sample, before pan."""
    n = v["note"]
    if n["kind"] == "noise":
        s = (noise24(n["seed"], v["age"]) * n["level"]) >> 23
    elif n["kind"] == "pluck":
        ks, pos = v["ks"], v["pos"]
        nxt = 0 if pos + 1 == v["period"] else pos + 1
        s = ks[pos]
        ks[pos] = ((s + ks[nxt]) * PLUCK_LOSS) >> 17
        v["pos"] = nxt
    else:
        s = (sine_q30(v["phase"]) * n["level"]) >> 30
        v["phase"] = (v["phase"] + v["inc"]) & M32
    return (s * envelope_q16(n, v["age"])) >> 16


def render(notes, frames):
    """(left, right) Q23 samples: notes sorted by start, 16 voices, the oldest stolen."""
    voices = [None] * MAX_VOICES
    nxt, left, right = 0, [], []
    for frame in range(frames):
        while nxt < len(notes) and notes[nxt]["start"] <= frame:
            n = notes[nxt]
            nxt += 1
            if n["length"] == 0:
                continue
            slot = None
            for i, v in enumerate(voices):
                if v is None:
                    slot = i
                    break
                if slot is None or v["age"] > voices[slot]["age"]:
                    slot = i
            voices[slot] = start_voice(n)
        l = r = 0
        for i, v in enumerate(voices):
            if v is None:
                continue
            s = next_sample(v)
            l += (s * v["note"]["panL"]) >> 15
            r += (s * v["note"]["panR"]) >> 15
            v["age"] += 1
            if v["age"] >= v["note"]["length"]:
                voices[i] = None
        left.append(saturate(l))
        right.append(saturate(r))
    return left, right


def fnv1a64(left, right):
    """FNV-1a 64 over the little-endian bytes of each frame's L and R samples."""
    h = 0xCBF29CE484222325
    for pair in zip(left, right):
        for byte in struct.pack("<ii", *pair):
            h = ((h ^ byte) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def input_sha256(left, right):
    """The harness's input hash: interleaved little-endian float32 of q * 2^-23."""
    sha = hashlib.sha256()
    for l, r in zip(left, right):
        sha.update(struct.pack("<ff", l / 8388608.0, r / 8388608.0))
    return sha.hexdigest()


def main():
    frames = int(sys.argv[1]) if len(sys.argv) > 1 else 480000
    for name, build in VECTORS.items():
        notes = build(frames)
        left, right = render(notes, frames)
        print(f"{name:13s} notes={len(notes):3d} fnv={fnv1a64(left, right):016X} "
              f"input={input_sha256(left, right)[:16]}", flush=True)


if __name__ == "__main__":
    main()
