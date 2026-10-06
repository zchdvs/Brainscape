import glob, os, collections, sys
W = os.path.dirname(os.path.abspath(__file__))
rows = {}
for f in sorted(glob.glob(os.path.join(W, 'out', 'hash', '*.txt'))):
    tag = os.path.basename(f)[:-4]
    d = {}
    for line in open(f):
        if line.startswith('#'): continue
        p = line.split()
        if len(p) >= 2: d[p[0]] = p[1]
    rows[tag] = d
presets = ['default','heavy','fb05','selfosc','strum','freeze','postall','pitch12','automation','clean','ALL']
golden = rows.get('msvc_det_precise')
print('%-26s ' % 'build' + ' '.join('%-8s' % p[:8] for p in presets))
for tag, d in rows.items():
    cells = []
    for p in presets:
        h = d.get(p, '-')
        if golden and golden.get(p) == h: cells.append('   =    ')
        else: cells.append(h[:8])
    print('%-26s ' % tag + ' '.join(cells))
