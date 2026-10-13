#!/usr/bin/env python3
"""Read-only FAT32 check of the YP-S3 music partition via the test bootloader."""
import sys, struct, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import yps3_nanddump as nd, yps3_ftl as ftl
dev = nd.open_device()
cache = {}
def rd(sec, n=1):
    return ftl.read(dev, sec, n)
mbr = rd(0); P = struct.unpack('<I', mbr[0x1c6:0x1ca])[0]
bs = rd(P)
bps, spc, res, nf = struct.unpack('<HBHB', bs[11:17])
fsz = struct.unpack('<I', bs[36:40])[0]; root = struct.unpack('<I', bs[44:48])[0]
totsec = struct.unpack('<I', bs[32:36])[0]
data0 = P + res + nf * fsz
nclus = (totsec - res - nf * fsz) // spc + 2
fatfile = sys.argv[1] if len(sys.argv) > 1 else None
if fatfile and os.path.exists(fatfile):
    fat = open(fatfile, 'rb').read()
else:
    fat = b''
    for s in range(0, fsz, 2048):
        fat += rd(P + res + s, min(2048, fsz - s))
    if fatfile: open(fatfile, 'wb').write(fat)
F = struct.unpack('<%dI' % (len(fat)//4), fat)
F = [x & 0x0fffffff for x in F]
def chain(c):
    out = []
    while 2 <= c < 0x0ffffff8 and len(out) < nclus:
        out.append(c); c = F[c]
    return out, c
owner = {}
problems = []
def claim(c, path):
    ch, end = chain(c)
    if end < 0x0ffffff8:
        problems.append('%s: chain ends with %x' % (path, end))
    for x in ch:
        if x in owner:
            problems.append('%s: cluster %d cross-linked with %s' % (path, x, owner[x]))
        else:
            owner[x] = path
    return ch
def walk(clus, path):
    ch = claim(clus, path + '/')
    raw = b''.join(rd(data0 + (c - 2) * spc, spc) for c in ch)
    ended = False; lfn = []
    for i in range(len(raw) // 32):
        e = raw[i*32:i*32+32]
        if e[0] == 0:
            ended = True; continue
        if ended:
            problems.append('%s: entry %d (%02x) after end marker' % (path, i, e[0])); 
        if e[0] == 0xe5:
            lfn = []; continue
        if e[11] == 0x0f:
            lfn.append(e); continue
        name = e[:11].decode('latin1')
        if lfn:
            nm = ''.join((l[1:11]+l[14:26]+l[28:32]).decode('utf-16le','replace') for l in reversed(lfn))
            name = nm.split('\0')[0]
        lfn = []
        if e[11] & 0x08: continue
        c = struct.unpack('<H', e[26:28])[0] | struct.unpack('<H', e[20:22])[0] << 16
        size = struct.unpack('<I', e[28:32])[0]
        if name.strip() in ('.', '..'): continue
        p = path + '/' + name
        if e[11] & 0x10:
            if c < 2: problems.append('%s: dir with cluster %d' % (p, c)); continue
            walk(c, p)
        elif c >= 2:
            ch = claim(c, p)
            need = (size + spc*512 - 1) // (spc*512)
            if len(ch) != need:
                problems.append('%s: size %d needs %d clusters, chain %d' % (p, size, need, len(ch)))
        elif size:
            problems.append('%s: size %d, no cluster' % (p, size))
walk(root, '')
used = sum(1 for c in range(2, nclus) if F[c] != 0)
lost = [c for c in range(2, nclus) if F[c] != 0 and c not in owner]
print('clusters used %d, owned %d, lost %d' % (used, len(owner), len(lost)))
print('lost (first 20):', lost[:20])
for p in problems: print('PROBLEM', p)
