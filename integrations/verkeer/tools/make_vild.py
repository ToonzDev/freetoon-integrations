"""Build vild.tsv (location code -> road, name) from NDW's VILD release.

  python3 tools/make_vild.py VILD6.14.A.zip > vild.tsv

VILD is NDW open data (CC0 1.0, https://opendata.ndw.nu/). Only the fields
the app shows are kept; names are folded to ASCII (the Toon's UI fonts)."""
import sys, struct, zipfile, unicodedata

def fold(s):
    s = unicodedata.normalize('NFKD', s)
    return ''.join(c for c in s if ord(c) < 128).replace('\t', ' ').strip()

z = zipfile.ZipFile(sys.argv[1])
name = [n for n in z.namelist() if n.upper().startswith('VILD') and n.lower().endswith('.dbf')][0]
b = z.read(name)
n, hl, rl = struct.unpack('<IHH', b[4:12])
fields, o = [], 32
while b[o] != 0x0d:
    fields.append((b[o:o+11].split(b'\0')[0].decode(), b[o+16])); o += 32
PREFIX = {'Knooppunt': 'Knp. ', 'Aansluiting': '', 'Afrit': '', 'Verbindingsweg': ''}
print('# VILD %s (NDW, CC0): loc<TAB>road<TAB>name' % name[:-4])
for i in range(n):
    r = b[hl + i*rl + 1: hl + (i+1)*rl]
    d, p = {}, 0
    for f, ln in fields:
        d[f] = r[p:p+ln].decode('latin1').strip(); p += ln
    if not d['LOC_TYPE'].startswith('P'):          # point locations only
        continue
    nm = fold(d['FIRST_NAME'])
    if not nm:
        continue
    print('%s\t%s\t%s%s' % (d['LOC_NR'], fold(d['ROADNUMBER']), PREFIX.get(d['LOC_DES'], ''), nm))
