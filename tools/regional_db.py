#!/usr/bin/env python3
"""Cut FlightGear 0.9.10's world airport/navaid databases down to some regions.

FlightGear 0.9.10 keeps every airport, runway, taxiway, navaid, fix and airway
segment of the world in memory: about 35 MB on the PS3, and parsing them all
slows down the start. Scenery is only included for the San Francisco Bay Area
and Vienna anyway.
Usage: regional_db.py <fg-root> <out-root> [lon0,lat0,lon1,lat1 ...]
(default boxes: California and Central Europe)."""
import gzip, os, sys

src, dst = sys.argv[1], sys.argv[2]
BOXES = [tuple(map(float, a.split(','))) for a in sys.argv[3:]] or \
        [(-125.0, 32.0, -114.0, 42.0),      # California: San Francisco Bay Area scenery
         (5.0, 43.0, 25.0, 52.0)]           # Central Europe: Vienna scenery

def inside(lat, lon):
    return any(lat0 <= lat <= lat1 and lon0 <= lon <= lon1 for lon0, lat0, lon1, lat1 in BOXES)

def rd(rel):
    with gzip.open(os.path.join(src, rel), 'rt', encoding='latin-1') as f:
        return f.read().split('\n')

def wr(rel, lines):
    path = os.path.join(dst, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with gzip.open(path, 'wt', encoding='latin-1') as f:
        f.write('\n'.join(lines) + '\n')
    print('%s: %d lines' % (rel, len(lines)))

def num(s):
    try: return float(s)
    except ValueError: return None

# apt.dat: blocks start with a type 1/16/17 header line; keep a block if one of
# its runway/taxiway lines (type 10) lies inside the box.
lines = rd('Airports/apt.dat.gz')
out, block, keep = lines[:2], [], False
for l in lines[2:]:
    t = l.split()
    if not t: continue
    if t[0] in ('1', '16', '17', '99'):
        if keep: out += block
        block, keep = [l], False
        if t[0] == '99': break
        continue
    block.append(l)
    if t[0] == '10' and len(t) > 2:
        la, lo = num(t[1]), num(t[2])
        if la is not None and lo is not None and inside(la, lo): keep = True
wr('Airports/apt.dat.gz', out + ['99'])

# nav.dat: "type lat lon ..."; fix.dat: "lat lon name"; awy.dat: "n1 lat1 lon1 n2 lat2 lon2 ..."
for rel, cols in (('Navaids/nav.dat.gz', (1, 2)), ('Navaids/fix.dat.gz', (0, 1))):
    lines = rd(rel)
    out = lines[:2]
    for l in lines[2:]:
        t = l.split()
        if len(t) > max(cols):
            la, lo = num(t[cols[0]]), num(t[cols[1]])
            if la is not None and lo is not None and inside(la, lo): out.append(l)
    wr(rel, out + ['99'])

lines = rd('Navaids/awy.dat.gz')
out = lines[:2]
for l in lines[2:]:
    t = l.split()
    if len(t) >= 6:
        a, b = (num(t[1]), num(t[2])), (num(t[4]), num(t[5]))
        if None not in a + b and (inside(*a) or inside(*b)): out.append(l)
wr('Navaids/awy.dat.gz', out + ['99'])
