#!/usr/bin/env python3
"""List the material names used by FlightGear terrain files (.btg/.btg.gz, version 6/7/10)."""
import gzip, struct, sys, collections

def materials(path):
    data = gzip.open(path).read() if path.endswith('.gz') else open(path, 'rb').read()
    p = 0
    def rd(fmt):
        nonlocal p
        v = struct.unpack_from('<' + fmt, data, p)
        p += struct.calcsize('<' + fmt)
        return v
    hdr, _ = rd('II')
    ver = hdr & 0xffff
    if ver >= 10: (nobj,) = rd('i')
    elif ver >= 7: (nobj,) = rd('H')
    else: (nobj,) = rd('h')
    out = []
    for _ in range(nobj):
        (otype,) = rd('B')
        if ver >= 10: nprop, nelem = rd('II')
        elif ver >= 7: nprop, nelem = rd('HH')
        else: nprop, nelem = rd('hh')
        for _ in range(nprop):
            ptype, nbytes = rd('BI')
            if ptype == 0 and otype >= 9:
                out.append((otype, data[p:p + nbytes].decode('latin-1')))
            p += nbytes
        for _ in range(nelem):
            (nbytes,) = rd('I')
            p += nbytes
    return out

if __name__ == '__main__':
    c = collections.Counter()
    for f in sys.argv[1:]:
        for t, m in materials(f):
            c[m] += 1
    for m, n in sorted(c.items()):
        print(n, m)
