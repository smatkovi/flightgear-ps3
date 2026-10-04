#!/usr/bin/env python3
"""Build the hangar's aircraft catalog from the FlightGear 1.x aircraft archive.

Reads the downloaded archives (dl/aircraft/*.zip, see fetch_aircraft_index in
the README) and writes one line per archive:
    zip <tab> size <tab> dir <tab> sets <tab> fdm <tab> status <tab> description
dir is the archive's top-level folder, sets the names of its *-set.xml files.
Usage: aircraft_catalog.py <zip-dir> <out-file>"""
import os, re, sys, zipfile

src, out = sys.argv[1], sys.argv[2]

def field(xml, tag):
    m = re.search(r'<%s(?:\s[^>]*)?>(.*?)</%s>' % (tag, tag), xml, re.S)
    if not m:
        return ''
    t = re.sub(r'\s+', ' ', m.group(1)).strip()
    t = t.replace('&amp;', '&').replace('&lt;', '<').replace('&gt;', '>')
    return ''.join(c if 32 <= ord(c) < 127 else '?' for c in t).replace('\t', ' ')

lines = []
for name in sorted(os.listdir(src), key=str.lower):
    if not name.endswith('.zip'):
        continue
    path = os.path.join(src, name)
    try:
        z = zipfile.ZipFile(path)
    except zipfile.BadZipFile:
        print('skipping broken archive', name)
        continue
    names = z.namelist()
    tops = {n.split('/')[0] for n in names if '/' in n}
    if len(tops) != 1:
        print('skipping %s: %d top-level folders' % (name, len(tops)))
        continue
    top = tops.pop()
    sets = sorted(n for n in names if re.match(re.escape(top) + r'/[^/]+-set\.xml$', n))
    if not sets:
        print('skipping %s: no -set.xml' % name)
        continue
    xml = z.read(sets[0]).decode('latin-1', 'replace')
    fdm = field(xml, 'flight-model')
    lines.append('\t'.join([name, str(os.path.getsize(path)), top,
                            ','.join(s[len(top) + 1:-8] for s in sets),
                            fdm, field(xml, 'status'), field(xml, 'description')]))

os.makedirs(os.path.dirname(out), exist_ok=True)
with open(out, 'w') as f:
    f.write('# FlightGear 1.x aircraft archive, mirrors.ibiblio.org/flightgear/ftp/Archive/Version-1.x/Aircraft-1.9.1/\n')
    f.write('\n'.join(lines) + '\n')
print('%d aircraft in %s' % (len(lines), out))
