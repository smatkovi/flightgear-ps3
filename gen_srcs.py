#!/usr/bin/env python3
"""List the library/program sources named in a tree of automake Makefile.am files.
Usage: gen_srcs.py VAR root [exclude-substring ...]   -> prints 'VAR := abs paths'"""
import os, re, sys
var, root, excl = sys.argv[1], sys.argv[2], sys.argv[3:]
out = []
for d, _, files in sorted(os.walk(root)):
    if 'Makefile.am' not in files: continue
    txt = open(os.path.join(d, 'Makefile.am'), errors='replace').read().replace('\\\n', ' ')
    mkvars = dict((m.group(1), m.group(2)) for m in re.finditer(r'^\s*(\w+)\s*=(.*)$', txt, re.M))
    for _ in range(3):
        txt = re.sub(r'\$\((\w+)\)', lambda m: mkvars.get(m.group(1), ''), txt)
    for m in re.finditer(r'^\s*(\w+)_SOURCES\s*\+?=(.*)$', txt, re.M):
        name = m.group(1)
        if not (name.startswith('lib') or name == 'fgfs'): continue
        for f in m.group(2).split():
            if not re.search(r'\.(cxx|cpp|c)$', f): continue
            p = os.path.normpath(os.path.join(d, f))
            if os.path.exists(p) and not any(e in p for e in excl) and p not in out:
                out.append(p)
print(var + ' := \\\n  ' + ' \\\n  '.join(out))
