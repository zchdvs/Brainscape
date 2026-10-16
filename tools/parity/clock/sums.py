# Incremental least-squares sums under re-basing (clock.md §3.3, draft v2), checked against
# direct sums; and the int64 bounds for a window of the fitted ticks among the last 96 labels.
import random
r = random.Random(5)
win = []  # (label, frame)
S = dict(n=0, x=0, y=0, xx=0, xy=0); l0 = f0 = None
def direct():
    xs = [(l - l0, f - f0) for l, f in win]
    return dict(n=len(xs), x=sum(a for a, _ in xs), y=sum(b for _, b in xs),
                xx=sum(a*a for a, _ in xs), xy=sum(a*b for a, b in xs))
label, frame = 0, 0
for step in range(20000):
    label += 1 + (r.random() < 0.02) * r.randint(1, 5)
    frame += r.randint(300, 900)
    if l0 is None: l0, f0 = label, frame
    # add
    x, y = label - l0, frame - f0
    win.append((label, frame)); S['n'] += 1; S['x'] += x; S['y'] += y; S['xx'] += x*x; S['xy'] += x*y
    # drop ticks whose label left the last 96 labels
    while win and win[0][0] <= label - 96:
        l, f = win.pop(0); x, y = l - l0, f - f0
        S['n'] -= 1; S['x'] -= x; S['y'] -= y; S['xx'] -= x*x; S['xy'] -= x*y
    # re-base to the oldest
    if win and (win[0][0] != l0 or win[0][1] != f0):
        d, e = win[0][0] - l0, win[0][1] - f0; N = S['n']
        sx, sy, sxx, sxy = S['x'], S['y'], S['xx'], S['xy']
        S['x'] = sx - N*d; S['y'] = sy - N*e
        S['xx'] = sxx - 2*d*sx + N*d*d
        S['xy'] = sxy - e*sx - d*sy + N*d*e
        l0, f0 = win[0]
    assert S == direct(), step
print("incremental == direct over 20,000 ticks")
# bounds: x in [0,95], N <= 96, y < 95 * R (each label's gap < R), R = 384,000
R = 384000; N = 96; X = 95; Y = 95 * R
Sx = N*X; Sxx = N*X*X; Sy = N*Y; Sxy = N*X*Y
D = N*Sxx; A = N*Sxy + Sx*Sy; B = Sy*Sxx + Sx*Sxy
rhoD = (Y + R) * D + B + A * X
print("y max %.3g (int32 %.3g)" % (Y, 2**31))
for k, v in dict(Sxx=Sxx, Sy=Sy, Sxy=Sxy, D=D, A=A, B=B, rhoD=rhoD).items():
    print(k, "%.3g" % v, "fits int64" if v < 2**63 else "OVERFLOW")
