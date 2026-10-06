# Static loop analysis of arm-none-eabi objdump output: finds innermost loops (backward
# branches) per function and reports instruction mix, so contraction-on vs -off builds
# of the SAME sources can be compared loop by loop.
#   python loops.py <objdump> <objA.o> <objB.o> [function-substring ...]
import re, subprocess, sys

BR = re.compile(r'^(b|b\.w|b\.n|beq|bne|bcs|bcc|bhs|blo|bmi|bpl|bvs|bvc|bhi|bls|bge|blt|bgt|ble|cbz|cbnz)(\.w|\.n)?$')

def parse(objdump, obj):
    out = subprocess.run([objdump, '-d', '-C', '--no-show-raw-insn', obj], capture_output=True, text=True).stdout
    funcs, cur = {}, None
    for line in out.splitlines():
        m = re.match(r'^([0-9a-f]+) <(.*)>:$', line)
        if m:
            cur = m.group(2)
            funcs[cur] = []
            continue
        m = re.match(r'^\s+([0-9a-f]+):\s+(\S+)\s*(.*)$', line)
        if m and cur is not None:
            funcs[cur].append((int(m.group(1), 16), m.group(2), m.group(3)))
    return funcs

def loops(ins):
    addrs = [a for a, _, _ in ins]
    res = []
    for a, mn, ops in ins:
        if BR.match(mn):
            t = re.search(r'\b([0-9a-f]+) <', ops)
            if t:
                tgt = int(t.group(1), 16)
                if tgt <= a and tgt >= addrs[0]:
                    res.append((tgt, a))
    # innermost: no other loop strictly inside
    inner = [l for l in res if not any((o != l and o[0] >= l[0] and o[1] <= l[1]) for o in res)]
    return sorted(set(inner))

def mix(ins, lo, hi):
    body = [(a, mn, ops) for a, mn, ops in ins if lo <= a <= hi]
    c = {'n': len(body)}
    for _, mn, _ in body:
        base = mn.split('.')[0]
        if mn.startswith(('vfma', 'vfms', 'vfnma', 'vfnms')): k = 'fused'
        elif base in ('vmla', 'vmls', 'vnmla', 'vnmls'): k = 'chainedMAC'
        elif base in ('vmul', 'vnmul'): k = 'vmul'
        elif base in ('vadd', 'vsub'): k = 'vadd/sub'
        elif base in ('vldr', 'vldmia', 'ldr', 'ldrsh', 'ldrh', 'ldrb', 'ldrd', 'ldm'): k = 'load'
        elif base in ('vstr', 'vstmia', 'str', 'strh', 'strd', 'stm'): k = 'store'
        elif mn.startswith('v') and ('f64' in mn): k = 'f64'
        elif mn.startswith('v'): k = 'otherFP'
        else: k = 'int/branch'
        c[k] = c.get(k, 0) + 1
    return c

def main():
    objdump, A, B = sys.argv[1:4]
    filt = sys.argv[4:]
    fa, fb = parse(objdump, A), parse(objdump, B)
    for name in fa:
        if filt and not any(f in name for f in filt): continue
        if name not in fb: continue
        la, lb = loops(fa[name]), loops(fb[name])
        short = re.sub(r'\(.*', '', name)
        print(f'## {short}  ({len(fa[name])} vs {len(fb[name])} insns total; innermost loops {len(la)} vs {len(lb)})')
        for i in range(max(len(la), len(lb))):
            ma = mix(fa[name], *la[i]) if i < len(la) else {}
            mb = mix(fb[name], *lb[i]) if i < len(lb) else {}
            keys = sorted(set(ma) | set(mb) - {'n'})
            desc = ' '.join(f'{k}:{ma.get(k,0)}/{mb.get(k,0)}' for k in keys if k != 'n')
            src = ''
            if i < len(la):
                r = subprocess.run([objdump.replace('objdump', 'addr2line'), '-e', A, hex(la[i][0])], capture_output=True, text=True).stdout.strip()
                src = re.sub(r'.*[\/]', '', r)
            print(f'  loop{i} @{src}: insns {ma.get("n",0)} -> {mb.get("n",0)}   [{desc}]')

main()
