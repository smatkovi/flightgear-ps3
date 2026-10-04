#!/usr/bin/env python3
"""Symbolize PPU addresses with the nearest preceding text symbol (nm -n output on stdin).
Usage: ppu-nm -n -C fgfs.elf | sym.py addr..."""
import sys, bisect
syms = []
for line in sys.stdin:
    p = line.rstrip('\n').split(' ', 2)
    if len(p) == 3 and p[0] and p[1] in ('T', 't') and 'long_branch' not in p[2]:
        syms.append((int(p[0], 16), p[2]))
syms.sort()
addrs = [a for a, _ in syms]
for a in sys.argv[1:]:
    v = int(a, 16)
    i = bisect.bisect_right(addrs, v) - 1
    print('%s  %s+0x%x' % (a, syms[i][1], v - syms[i][0]) if i >= 0 else a)
