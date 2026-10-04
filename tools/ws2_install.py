#!/usr/bin/env python3
"""Copy World Scenery 2.x tiles into FlightGear 0.9.10's data.

The 2013 World Scenery 2.12 terrain files are btg version 6 and use the same
land class, runway and light materials as 0.9.10, so they load unchanged. Only
the .stg files are filtered: shared models that 0.9.10 does not have (power
pylons, VOR/DME, markers, NDBs, localizers) and unknown object types are dropped.

Usage: ws2_install.py <ws2-root> <fg-root> <tile-dir>...   e.g. e010n40/e016n48"""
import os, shutil, sys

src, fgroot, dirs = sys.argv[1], sys.argv[2], sys.argv[3:]
KNOWN = ('OBJECT_BASE', 'OBJECT', 'OBJECT_SHARED', 'OBJECT_STATIC')
kept = dropped = 0

def keep(tok, srcdir):
    if tok[0] not in KNOWN or len(tok) < 2:
        return False
    if tok[0] == 'OBJECT_SHARED':
        return os.path.exists(os.path.join(fgroot, tok[1]))
    return os.path.exists(os.path.join(srcdir, tok[1]) if tok[0] == 'OBJECT_STATIC'
                          else os.path.join(srcdir, tok[1] + '.gz')) or \
           os.path.exists(os.path.join(srcdir, tok[1]))

for kind in ('Terrain', 'Objects'):
    for d in dirs:
        s = os.path.join(src, kind, d)
        t = os.path.join(fgroot, 'Scenery', kind, d)
        if not os.path.isdir(s):
            continue
        os.makedirs(t, exist_ok=True)
        for f in sorted(os.listdir(s)):
            if not f.endswith('.stg'):
                shutil.copy2(os.path.join(s, f), t)
                continue
            out = []
            for line in open(os.path.join(s, f), encoding='latin-1'):
                tok = line.split()
                if not tok or tok[0].startswith('#'):
                    continue
                if keep(tok, s):
                    out.append(line.rstrip('\n'))
                    kept += 1
                else:
                    dropped += 1
            if out:
                with open(os.path.join(t, f), 'w') as fp:
                    fp.write('\n'.join(out) + '\n')
print('scenery: %d stg entries kept, %d dropped' % (kept, dropped))
