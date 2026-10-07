import math
def ev(m, lo, hi, curve=1.0, inlo=0.0, inhi=1.0):
    u = 0.0 if m <= inlo else 1.0 if m >= inhi else (m-inlo)/(inhi-inlo)
    c = u**curve
    return lo + (hi-lo)*c
pos = {"activity":0, "repeats":0.577, "shape":0, "time":0.479, "space":0.12, "filter":1}
print("repeats->feedback", ev(pos["repeats"],0,0.92,1.3), "stored 0.45")
print("time->base_ms", ev(pos["time"],40,1500,2), "stored 375")
print("space->delay.mix (default macro)", ev(pos["space"],0,0.5), "stored default 0")
print("space->reverb.mix", ev(pos["space"],0,1), "stored 0.12")
print("filter->cutoff", ev(pos["filter"],40,20000,4))
# default filter macro curve-4 positions
for p in (0.1,0.2,0.3,0.5,0.75,0.9):
    print("filter pos",p,"Hz",round(ev(p,40,20000,4),1))
# feedback steady-state energy gain for sustained uncorrelated input
for g in (0.45,0.85,0.9,0.92,0.95):
    print("fb",g,"energy gain dB", round(10*math.log10(1/(1-g*g)),1))
# FIFO drift for Engram
base=375; fifo=512/48
taps=[base]
for k in range(1,6): taps.append(taps[-1]+base+fifo)
print("echo times", [round(t,1) for t in taps], "ideal", [base*(k+1) for k in range(6)])
print("lag at 4th repeat ms", round(taps[4]-base*5,1))
