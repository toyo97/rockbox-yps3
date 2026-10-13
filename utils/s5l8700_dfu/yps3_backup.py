#!/usr/bin/env python3
"""Backs up everything on a YP-S3's NAND that writing to the music partition
could put at risk, through the test bootloader (read only):
  * raw pages (data + spare) of the VFL context blocks (42-49), the 10 FTL
    meta vblocks and the 12 log vblocks, both dies, both planes;
  * the hidden firmware area and the 40 MB system partition, as logical
    sectors through the FTL model.
Usage: yps3_backup.py OUTDIR"""
import os, sys, struct, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import yps3_vfl as V

out = sys.argv[1]
os.makedirs(out, exist_ok=True)
nand = V.Nand(None)
vfl = V.VFL(nand)
ftl = V.FTL(vfl)

def raw_block(die, block, f):
    for p in range(V.PAGES):
        f.write(nand.raw(die, block, p))

t0 = time.time()
with open(os.path.join(out, "vflcxt_blocks42-49.raw"), "wb") as f:
    for die in range(2):
        for b in range(42, 50):
            raw_block(die, b, f)
print("vfl context blocks done", int(time.time() - t0), "s", flush=True)

c0 = struct.unpack(">I", vfl.cxt[0][:4])[0]
c1 = struct.unpack(">I", vfl.cxt[1][:4])[0]
cxt = vfl.cxt[0] if c0 >= c1 else vfl.cxt[1]
meta = list(struct.unpack(">10H", cxt[4:24]))
logs = [vb for vb in ftl.logs.values()]
vblocks = [("meta", vb) for vb in meta] + [("log", vb) for vb in logs]
with open(os.path.join(out, "vblocks.txt"), "w") as idx:
    for kind, vb in vblocks:
        name = "%s_vb%04d.raw" % (kind, vb)
        idx.write("%s %d\n" % (name, vb))
        with open(os.path.join(out, name), "wb") as f:
            for die in range(2):
                for plane in range(2):
                    raw_block(die, vfl.phys_block(die, vb, plane), f)
        print(name, int(time.time() - t0), "s", flush=True)

# logical areas: system partition (FAT16) and the hidden firmware area
for name, start, count in (("system_partition.img", 7786496, 81920),
                           ("hidden_area.img", 7868416, 7995392 - 7868416)):
    with open(os.path.join(out, name), "wb") as f:
        for off in range(0, count, 256):
            f.write(ftl.read(start + off, min(256, count - off)))
    print(name, int(time.time() - t0), "s", flush=True)
print("backup complete")
