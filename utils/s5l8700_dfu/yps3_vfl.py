#!/usr/bin/env python3
#             __________               __   ___.
#   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
#   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
#   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
#   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
#                     \/            \/     \/    \/            \/
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.
#
# This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
# KIND, either express or implied.
#
"""Research model of the Whimory VFL of the Samsung YP-S3, reading the NAND
through the test bootloader (see yps3_nanddump.py). Read only.

Layout (K9LBG08U0M, one chip enable):
  * 2 dies ("banks") of 4096 blocks, 128 pages of 4096 + 128 bytes each.
    Raw page number = die * 4096 * 128 + block * 128 + page.
  * virtual block v = physical blocks 2v (plane 0) and 2v+1 (plane 1) in
    each die, bad ones remapped to reserved blocks 50 + i via the list in
    the per-die VFL context.
  * virtual page vpn = v * 256 + page * 2 + die, 8 KiB: plane 0, plane 1.
  * VFL context: page 0 of physical blocks 42/44/46/48 of each die; the
    valid one has the lowest non zero (inverted) counter in spare[12:16].
"""

import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import yps3_nanddump as nd  # noqa: E402

BLOCKS_PER_DIE = 4096
PAGES = 128
PAGE = 4096
SPARE = 128
CXT_BLOCKS = (42, 44, 46, 48)
RESERVED_BASE = 50


class Nand:
    def __init__(self, cache_dir=None):
        self.dev = nd.open_device()
        self.cache_dir = cache_dir
        if cache_dir:
            os.makedirs(cache_dir, exist_ok=True)

    def raw(self, die, block, page):
        n = (die * BLOCKS_PER_DIE + block) * PAGES + page
        if self.cache_dir:
            path = os.path.join(self.cache_dir, f"{n:07d}.bin")
            if os.path.exists(path):
                with open(path, "rb") as f:
                    return f.read()
        data = nd.read_page(self.dev, n)
        if self.cache_dir:
            with open(path, "wb") as f:
                f.write(data)
        return data


class VFL:
    def __init__(self, nand):
        self.nand = nand
        self.cxt = []
        self.remap = []
        for die in range(2):
            best = None
            for blk in CXT_BLOCKS:
                d = nand.raw(die, blk, 0)
                usn = struct.unpack(">I", d[PAGE + 12:PAGE + 16])[0]
                if usn in (0, 0xFFFFFFFF):
                    continue
                if best is None or usn < best[0]:
                    best = (usn, blk, d)
            if best is None:
                raise RuntimeError(f"no VFL context on die {die}")
            cxt = best[2][:PAGE]
            self.cxt.append(cxt)
            count = struct.unpack(">H", cxt[0x96:0x98])[0]
            bad = struct.unpack(f">{count * 2}H", cxt[0x98:0x98 + count * 4])
            self.remap.append({b: RESERVED_BASE + i for i, b in enumerate(bad)
                               if b != 0})
            print(f"die {die}: VFL context in block {best[1]}, counter "
                  f"{~best[0] & 0xFFFFFFFF:#x}, {len(self.remap[-1])} remapped",
                  file=sys.stderr)

    def phys_block(self, die, vb, plane):
        b = vb * 2 + plane
        return self.remap[die].get(b, b)

    def read_vpn(self, vpn):
        """8 KiB of data and the two spare areas"""
        vb, rest = divmod(vpn, 256)
        page, die = divmod(rest, 2)
        out, spares = b"", []
        for plane in range(2):
            d = self.nand.raw(die, self.phys_block(die, vb, plane), page)
            out += d[:PAGE]
            spares.append(d[PAGE:])
        return out, spares


if __name__ == "__main__":
    vfl = VFL(Nand(os.path.join(os.path.dirname(__file__), "../../nanddump/cache")))
    for die in range(2):
        words = struct.unpack(">24H", vfl.cxt[die][:48])
        print(f"die {die} cxt head:", " ".join(f"{w:04x}" for w in words))


SECTORS_PER_VPAGE = 16
VPAGES_PER_VBLOCK = 256
SECTORS_PER_LBLOCK = SECTORS_PER_VPAGE * VPAGES_PER_VBLOCK
MAP_CHUNKS = 8
FTL_META_BLOCKS = 10


class FTL:
    """Read-only model of the Whimory FTL found in Samsung's bootloader."""

    def __init__(self, vfl):
        self.vfl = vfl
        # the FTL meta block list comes from the newest of the two VFL contexts
        c0 = struct.unpack(">I", vfl.cxt[0][:4])[0]
        c1 = struct.unpack(">I", vfl.cxt[1][:4])[0]
        cxt = vfl.cxt[0] if c0 >= c1 else vfl.cxt[1]
        meta = struct.unpack(f">{FTL_META_BLOCKS}H", cxt[4:4 + 2 * FTL_META_BLOCKS])
        best = None
        for vb in meta:
            page, usn = self._last_meta_page(vb)
            if page is not None and (best is None or usn < best[2]):
                best = (vb, page, usn)
        self.cxt, _ = vfl.read_vpn(best[0] * VPAGES_PER_VBLOCK + best[1])
        print(f"FTL context at vblock {best[0]:#x} page {best[1]}", file=sys.stderr)
        c = self.cxt
        self.map_vb = struct.unpack(f">{MAP_CHUNKS + 1}H", c[0xD0:0xD0 + 2 * (MAP_CHUNKS + 1)])
        self.map_pg = struct.unpack(f">{MAP_CHUNKS + 1}H", c[0x154:0x154 + 2 * (MAP_CHUNKS + 1)])
        log_vb = struct.unpack(">12H", c[0x70:0x88])
        log_lbn = struct.unpack(">12H", c[0x88:0xA0])
        self.logs = {lbn: vb for vb, lbn in zip(log_vb, log_lbn) if vb != 0xFFFF}
        self.map_cache = {}
        self.log_cache = {}

    def _last_meta_page(self, vb):
        lo, hi, found = 0, VPAGES_PER_VBLOCK - 1, (None, None)
        while lo <= hi:
            mid = (lo + hi) // 2
            _, sp = self.vfl.read_vpn(vb * VPAGES_PER_VBLOCK + mid)
            if sp[0][:32] != b"\xff" * 32 and sp[0][16] == 0:
                found = (mid, struct.unpack(">I", sp[0][12:16])[0])
                lo = mid + 1
            else:
                hi = mid - 1
        return found

    def map(self, lbn):
        chunk, idx = lbn % MAP_CHUNKS, lbn // MAP_CHUNKS
        if chunk not in self.map_cache:
            d, _ = self.vfl.read_vpn(self.map_vb[chunk] * VPAGES_PER_VBLOCK +
                                     self.map_pg[chunk])
            self.map_cache[chunk] = d[0x200:0x800]
        return struct.unpack(">H", self.map_cache[chunk][2 * idx:2 * idx + 2])[0]

    def log_pages(self, lbn):
        """logical page -> vpn, for the log block of lbn (newest wins).
        Spare bytes 0x10-0x1f hold a status per 512 byte sector, 0xff means
        the sector is not in this page; this model assumes full pages."""
        if lbn not in self.log_cache:
            table = {}
            vb = self.logs.get(lbn)
            if vb is not None:
                for p in range(VPAGES_PER_VBLOCK):
                    _, sp = self.vfl.read_vpn(vb * VPAGES_PER_VBLOCK + p)
                    if sp[0][:32] == b"\xff" * 32:
                        continue
                    lp = struct.unpack(">I", sp[0][12:16])[0]
                    if lp < VPAGES_PER_VBLOCK:
                        table[lp] = vb * VPAGES_PER_VBLOCK + p
            self.log_cache[lbn] = table
        return self.log_cache[lbn]

    def read_vpage(self, lbn, page):
        vpn = self.log_pages(lbn).get(page)
        if vpn is None:
            vpn = self.map(lbn) * VPAGES_PER_VBLOCK + page
        d, _ = self.vfl.read_vpn(vpn)
        return d

    def read(self, lsn, count=1):
        out = b""
        while count:
            lbn, rest = divmod(lsn, SECTORS_PER_LBLOCK)
            page, sec = divmod(rest, SECTORS_PER_VPAGE)
            d = self.read_vpage(lbn, page)
            n = min(count, SECTORS_PER_VPAGE - sec)
            out += d[sec * 512:(sec + n) * 512]
            lsn += n
            count -= n
        return out
