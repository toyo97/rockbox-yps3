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
"""Read the NAND flash of a Samsung YP-S3 running the Rockbox test
bootloader (bootloader/samsung_yps3.c), started with s5l8700_dfu.py.

Read only: the bootloader has no request that writes to the flash.

Usage:
    yps3_nanddump.py info
    yps3_nanddump.py page 127              hex dump of one page + spare
    yps3_nanddump.py dump 0 1024 out.bin   pages 0..1023, 4224 bytes each
    yps3_nanddump.py spares 0 8192 out.bin first page spare of each block
    yps3_nanddump.py mem 0x22000000 256    hex dump of memory
    yps3_nanddump.py exit                  back to the status screen

Page numbers are raw NAND pages (4096 data + 128 spare bytes,
128 pages per block). Add --bank N for other chips (8 GB models).
"""

import argparse
import sys
import time

VID, PID = 0x0419, 0x0145
REQ_INFO, REQ_READ_PAGE, REQ_READ_IDS, REQ_READ_MEM, REQ_EXIT = (
    0x50, 0x51, 0x52, 0x53, 0x5F)
PAGE_DATA, PAGE_SPARE = 4096, 128
PAGE_TOTAL = PAGE_DATA + PAGE_SPARE
PAGES_PER_BLOCK = 128
TIMEOUT_MS = 5000


def open_device():
    try:
        import usb.core
    except ImportError:
        sys.exit("pyusb is required: pip install pyusb")
    dev = usb.core.find(idVendor=VID, idProduct=PID)
    if dev is None:
        sys.exit("device not found; is the test bootloader running?")
    return dev


def vendor_in(dev, req, value, index, length):
    return bytes(dev.ctrl_transfer(0xC0, req, value, index, length,
                                   TIMEOUT_MS))


def read_part(dev, page, col, length, bank=0):
    index = ((page >> 16) & 0xF) | (bank << 4) | ((col // 128) << 8)
    return vendor_in(dev, REQ_READ_PAGE, page & 0xFFFF, index, length)


def read_page(dev, page, bank=0):
    """4096 data bytes followed by the 128 byte spare area. Linux limits a
    control transfer to 4096 bytes, so this takes two requests."""
    return (read_part(dev, page, 0, PAGE_DATA, bank) +
            read_part(dev, page, PAGE_DATA, PAGE_SPARE, bank))


def hexdump(data, base=0):
    for i in range(0, len(data), 16):
        chunk = data[i:i + 16]
        text = "".join(chr(c) if 32 <= c < 127 else "." for c in chunk)
        print(f"{base + i:08x}  {chunk.hex(' ')}  {text}")


def cmd_info(dev, args):
    print(vendor_in(dev, REQ_INFO, 0, 0, 64).decode(errors="replace"))
    ids = vendor_in(dev, REQ_READ_IDS, 0, 0, 16)
    for bank in range(4):
        print(f"bank {bank}: {ids[4 * bank:4 * bank + 4].hex(' ')}")


def cmd_page(dev, args):
    data = read_page(dev, args.page, args.bank)
    print(f"page {args.page} (block {args.page // PAGES_PER_BLOCK}, "
          f"page {args.page % PAGES_PER_BLOCK}) data:")
    hexdump(data[:PAGE_DATA])
    print("spare:")
    hexdump(data[PAGE_DATA:], PAGE_DATA)


def progress(done, total, start):
    rate = done * PAGE_TOTAL / max(time.time() - start, 1e-3) / 1024
    print(f"\r{done}/{total} pages, {rate:.0f} KiB/s", end="", flush=True)


def cmd_dump(dev, args):
    start = time.time()
    with open(args.output, "wb") as f:
        for i in range(args.count):
            f.write(read_page(dev, args.start + i, args.bank))
            if i % 16 == 0 or i == args.count - 1:
                progress(i + 1, args.count, start)
    print(f"\nwrote {args.count} pages of {PAGE_TOTAL} bytes to {args.output}")


def cmd_spares(dev, args):
    """Spare area (and the first 16 data bytes) of the first page of each
    block: a quick overview of how the FTL uses the flash."""
    start = time.time()
    with open(args.output, "wb") as f:
        for i in range(args.count):
            page = (args.start + i) * PAGES_PER_BLOCK
            f.write(read_part(dev, page, 0, 16, args.bank) +
                    read_part(dev, page, PAGE_DATA, PAGE_SPARE, args.bank))
            if i % 64 == 0 or i == args.count - 1:
                progress(i + 1, args.count, start)
    print(f"\nwrote {args.count} records of {16 + PAGE_SPARE} bytes "
          f"(16 data bytes + spare) to {args.output}")


def cmd_mem(dev, args):
    out = b""
    while len(out) < args.length:
        n = min(4096, args.length - len(out))
        addr = args.addr + len(out)
        out += vendor_in(dev, REQ_READ_MEM, addr & 0xFFFF, addr >> 16, n)
    hexdump(out, args.addr)


def cmd_exit(dev, args):
    vendor_in(dev, REQ_EXIT, 0, 0, 0)


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--bank", type=int, default=0)
    sub = p.add_subparsers(dest="cmd", required=True)
    sub.add_parser("info")
    s = sub.add_parser("page")
    s.add_argument("page", type=lambda x: int(x, 0))
    s = sub.add_parser("dump")
    s.add_argument("start", type=lambda x: int(x, 0))
    s.add_argument("count", type=lambda x: int(x, 0))
    s.add_argument("output")
    s = sub.add_parser("spares")
    s.add_argument("start", type=lambda x: int(x, 0), help="first block")
    s.add_argument("count", type=lambda x: int(x, 0), help="number of blocks")
    s.add_argument("output")
    s = sub.add_parser("mem")
    s.add_argument("addr", type=lambda x: int(x, 0))
    s.add_argument("length", type=lambda x: int(x, 0))
    sub.add_parser("exit")
    args = p.parse_args()
    dev = open_device()
    {"info": cmd_info, "page": cmd_page, "dump": cmd_dump,
     "spares": cmd_spares, "mem": cmd_mem, "exit": cmd_exit}[args.cmd](dev, args)


if __name__ == "__main__":
    main()
