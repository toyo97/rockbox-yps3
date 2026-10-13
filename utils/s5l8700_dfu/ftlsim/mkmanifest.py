import struct, sys
d = sys.argv[1] if len(sys.argv) > 1 else "."
P = 4224
raw = open(d + "/vflcxt_blocks42-49.raw", "rb").read()
lines = []
remap = []
for die in range(2):
    best = None
    for i, b in enumerate(range(42, 50)):
        off = ((die * 8) + i) * 128 * P
        lines.append("vflcxt_blocks42-49.raw %d %d %d" % (off, die, b))
        if b % 2 == 0:
            pg = raw[off:off + P]
            usn = struct.unpack(">I", pg[4096 + 12:4096 + 16])[0]
            if usn not in (0, 0xffffffff) and (best is None or usn < best[0]):
                best = (usn, pg)
    cxt = best[1]
    cnt = struct.unpack(">H", cxt[0x96:0x98])[0] * 2
    remap.append(struct.unpack(">%dH" % cnt, cxt[0x98:0x98 + 2 * cnt]))
def phys(die, vb, plane):
    b = 2 * vb + plane
    for i, x in enumerate(remap[die]):
        if x == b and b != 0:
            return 50 + i
    return b
for l in open(d + "/vblocks.txt"):
    name, vb = l.split()
    vb = int(vb)
    k = 0
    for die in range(2):
        for plane in range(2):
            lines.append("%s %d %d %d" % (name, k * 128 * P, die, phys(die, vb, plane)))
            k += 1
open(d + "/manifest.txt", "w").write("\n".join(lines) + "\n")
print(len(lines), "blocks")
