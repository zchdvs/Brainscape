# The revised GridFrames (clock.md §6.3, draft v2): a catch-up at every span start, then the
# grid positions k > max(tick, lastFired) with F(k) < e. Events (tempo rescales, tap placements)
# apply at their frames, which split every render. Checks block-split invariance and that the
# fired frames equal "first frame at or after the boundary" at constant tempo.
import random
K = 24 * 2**32
def cdiv(a, b): return -((-a) // b)
def fdiv(a, b): return a // b
def mdr(a, b, c): return (a * b + c // 2) // c          # MulDivRoundU64, half up
class Ph:
    def __init__(s, P): s.P, s.tick, s.acc, s.last = P, -1, P, -1
    def F(s, k, f): return f + cdiv((k - s.tick) * s.P - s.acc, K)
    def grid(s, f, e, G, out):
        g = fdiv(s.tick, G) * G                         # catch-up: largest grid position <= tick
        if g > s.last:
            out.append((g, f)); s.last = g
        k = (fdiv(max(s.tick, s.last), G) + 1) * G
        while True:
            Fk = s.F(k, f)
            if Fk >= e: break
            if out and out[-1][1] == Fk: Fk += 1        # one hit per frame (never happens here)
            out.append((k, Fk)); s.last = k; k += G
    def adv(s, n):
        s.acc += n * K
        while s.acc > s.P: s.acc -= s.P; s.tick += 1
    def rescale(s, P2):
        s.acc = max(1, mdr(s.acc, P2, s.P)); s.P = P2
        while s.acc > s.P: s.acc -= s.P; s.tick += 1    # normalise after a slower->faster change
    def place(s, k): s.tick, s.acc = k - 1, s.P          # boundary k at this frame
def render(P0, G, total, sizes, events):
    ph = Ph(P0); out = []; f = 0; ev = sorted(events); i = 0
    while f < total:
        while i < len(ev) and ev[i][0] == f:
            _, kind, x = ev[i]; i += 1
            if kind == 'tempo': ph.rescale(x)
            else:                                        # tap: nearest beat, forward skip
                b = fdiv(ph.tick, 24); xx = (ph.tick - 24 * b) * ph.P + ph.acc
                B = 24 * (b + 1) if 2 * xx >= 24 * ph.P else 24 * b
                ph.place(B); ph.last = max(ph.last, B - 1)
        n = next(sizes); nxt = ev[i][0] if i < len(ev) else total
        n = min(n, nxt - f, total - f)
        ph.grid(f, f + n, G, out); ph.adv(n); f += n
    return out
def sizes_const(n):
    while True: yield n
def sizes_rand(seed):
    r = random.Random(seed)
    while True: yield r.randint(1, 512)
def P_of(bpm, R=48000): return int(R * 60 * 2**32 / bpm)
ok = True
for bpm in (140, 137.5, 97, 300, 20):
    P = P_of(bpm); total = 48000 * 30
    for G in (3, 24, 96):
        r = random.Random(bpm * 7 + G)
        evs = []
        for _ in range(25):
            fr = r.randrange(1, total)
            if r.random() < 0.5: evs.append((fr, 'tempo', P_of(r.uniform(20, 300))))
            else: evs.append((fr, 'tap', 0))
        evs = sorted({e[0]: e for e in evs}.values())
        ref = render(P, G, total, sizes_const(1), evs)
        for sz in (sizes_const(48), sizes_const(441), sizes_const(512), sizes_rand(1), sizes_rand(2)):
            if render(P, G, total, sz, evs) != ref: ok = False; print("MISMATCH", bpm, G)
        # constant tempo: frames equal ceil(boundary)
        c = render(P, G, total, sizes_const(48), [])
        for k, fr in c:
            if fr != cdiv(k * P, K): ok = False; print("OFFGRID", bpm, G, k, fr); break
    print(bpm, "quarters in 60 s at 48-frame blocks:", len(render(P, 24, 48000 * 60, sizes_const(48), [])))
print("all equal:", ok)
