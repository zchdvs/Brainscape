from fractions import Fraction as F
import struct
def val(b):
    return F(struct.unpack('<f', struct.pack('<I', b))[0])
def rn32(q):
    # correctly rounded (half-even) binary32 of a positive rational q, via exhaustive neighbour search near guess
    import math
    g = struct.unpack('<I', struct.pack('<f', float(q)))[0]
    best=None
    for b in range(g-3, g+4):
        d = abs(val(b)-q)
        if best is None or d < best[0] or (d == best[0] and b % 2 == 0): best=(d,b)
    return best[1]
b = 0x15AE43FD
for t in ["7.038531e-26", "7.0385307e-26", "7.03853069e-26"]:
    q = F(t)
    print(t, "correct binary32:", hex(rn32(q)), " via binary64:", hex(struct.unpack('<I', struct.pack('<f', float(q)))[0]))
print("exact value of 0x15AE43FD:", "%.20e" % float(val(b)))
