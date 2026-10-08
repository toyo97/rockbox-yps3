#!/usr/bin/env python3
#             __________               __   ___.
#   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
#   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
#   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
#   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
#                     \/            \/     \/    \/            \/
#
# Copyright (C) 2008 by William Poetra Yoga Hadisoeseno and Frank Gevaerts
#   (protocol, from utils/meizu_dfu/meizu_dfu.c)
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.
#
# This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
# KIND, either express or implied.
#
"""Run a binary from RAM on a Samsung S5L8700 based player (Samsung YP-S3,
Meizu M3/M6) through the DFU mode of the boot ROM.

Nothing is written to the flash of the player: the image is uploaded to
internal RAM and started. Unplugging the player (or a reset) brings back
the original firmware.

Usage:
    s5l8700_dfu.py list
    s5l8700_dfu.py run rockboot.ebn [--vid 0x0419] [--pid 0x0141]
    s5l8700_dfu.py wrap rockboot.ebn out.dfu      (only build the image)

Needs pyusb (pip install pyusb) and libusb. On Windows the DFU device must
be bound to the WinUSB driver first, e.g. with Zadig.
"""

import argparse
import struct
import sys
import time
import zlib

BLOCK_SIZE = 2048
TIMEOUT_MS = 0xA000

# The Meizu players enumerate with Samsung's semiconductor vendor id in DFU
# mode. The YP-S3 is expected to do the same, but this is not confirmed;
# "list" shows every candidate so the ids can be reported.
DFU_VENDOR_IDS = (0x0419,)
KNOWN_PRODUCT_IDS = {
    0x0141: "Meizu M3 / M6 (S5L8700 DFU)",
    0x0145: "Meizu M6SL (S5L8700 DFU)",
}
OTHER_INTERESTING_VIDS = {
    0x04E8: "Samsung Electronics (normal firmware mode?)",
}

DFU_DNLOAD = 1
DFU_GETSTATUS = 3
DFU_CLRSTATUS = 4

STATE_DNLOAD_IDLE = 5
STATE_MANIFEST = 7

REQ_OUT = 0x21  # host to device, class, interface
REQ_IN = 0xA1   # device to host, class, interface


def wrap_image(data, signature=b"UFD"):
    """Patch the length field and append the DFU suffix with its CRC,
    exactly like meizu_dfu does."""
    img = bytearray(data)
    if img[0x20:0x24] not in (b"DFUC", b"CUFD"):
        print("warning: no 'DFUC' marker at offset 0x20, is this an "
              "S5L8700 image?", file=sys.stderr)
    total = len(img) + 16
    img[0x24:0x28] = struct.pack("<I", total)
    suffix = struct.pack("<HHHH", 0x0100, 0x0140, 0x0419, 0x0100)
    suffix += signature[:3] + bytes([16])
    img += suffix
    # meizu_dfu: reflected CRC32, init 0xffffffff, no final xor
    crc = zlib.crc32(bytes(img)) ^ 0xFFFFFFFF
    img += struct.pack("<I", crc)
    return bytes(img)


def import_usb():
    try:
        import usb.core
        import usb.util
    except ImportError:
        sys.exit("pyusb is required: pip install pyusb")
    return usb


def cmd_list(_args):
    usb = import_usb()
    found = False
    for dev in usb.core.find(find_all=True):
        vid, pid = dev.idVendor, dev.idProduct
        note = None
        if vid in DFU_VENDOR_IDS:
            note = KNOWN_PRODUCT_IDS.get(pid, "S5L87xx boot ROM? (unknown pid)")
        elif vid in OTHER_INTERESTING_VIDS:
            note = OTHER_INTERESTING_VIDS[vid]
        if note:
            found = True
            print(f"{vid:04x}:{pid:04x}  {note}")
            try:
                print(f"    product: {dev.product!r}  "
                      f"manufacturer: {dev.manufacturer!r}")
            except Exception:  # strings may need permissions
                pass
    if not found:
        print("No Samsung device found. Is the player in DFU mode and "
              "connected? (on Linux you may need root or a udev rule)")


def find_device(usb, vid, pid):
    if vid is not None and pid is not None:
        return usb.core.find(idVendor=vid, idProduct=pid)
    for dev in usb.core.find(find_all=True):
        if dev.idVendor in DFU_VENDOR_IDS and (pid is None or dev.idProduct == pid):
            return dev
    return None


def get_status(dev):
    ret = dev.ctrl_transfer(REQ_IN, DFU_GETSTATUS, 0, 0, 6, TIMEOUT_MS)
    return bytes(ret)


def wait_state(dev, state):
    deadline = time.time() + 10
    while time.time() < deadline:
        st = get_status(dev)
        if len(st) >= 5 and st[4] == state:
            return st
    raise RuntimeError(f"timeout waiting for DFU state {state}, last status "
                       f"{st.hex()}")


def get_cpu(dev):
    ret = dev.ctrl_transfer(REQ_IN, 0xFF, 0x0002, 0, 0x3F, TIMEOUT_MS)
    return bytes(ret).split(b"\0")[0].decode("ascii", "replace")


def cmd_run(args):
    usb = import_usb()
    with open(args.image, "rb") as f:
        img = wrap_image(f.read(), args.sig.encode())

    dev = find_device(usb, args.vid, args.pid)
    if dev is None:
        sys.exit("No DFU device found, try 'list'")
    print(f"Found {dev.idVendor:04x}:{dev.idProduct:04x}")
    try:
        if dev.is_kernel_driver_active(0):
            dev.detach_kernel_driver(0)
    except Exception:
        pass
    try:
        dev.set_configuration()
    except Exception:
        pass  # already configured

    print("CPU:", get_cpu(dev))
    print("CPU:", get_cpu(dev))

    nblocks = (len(img) + BLOCK_SIZE - 1) // BLOCK_SIZE
    for idx in range(nblocks):
        chunk = img[idx * BLOCK_SIZE:(idx + 1) * BLOCK_SIZE]
        dev.ctrl_transfer(REQ_OUT, DFU_DNLOAD, idx, 0, chunk, TIMEOUT_MS)
        wait_state(dev, STATE_DNLOAD_IDLE)
        print(f"\rsent block {idx + 1}/{nblocks}", end="", flush=True)
    print()
    dev.ctrl_transfer(REQ_OUT, DFU_DNLOAD, nblocks, 0, b"", TIMEOUT_MS)
    try:
        wait_state(dev, STATE_MANIFEST)
    except Exception as e:  # the device may already be gone
        print("note:", e)
    print("Done, the image should be running now.")


def cmd_wrap(args):
    with open(args.image, "rb") as f:
        img = wrap_image(f.read(), args.sig.encode())
    with open(args.output, "wb") as f:
        f.write(img)
    print(f"wrote {len(img)} bytes to {args.output}")


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    sub.add_parser("list", help="list candidate USB devices")
    r = sub.add_parser("run", help="upload an image to RAM and run it")
    r.add_argument("image")
    r.add_argument("--vid", type=lambda x: int(x, 0))
    r.add_argument("--pid", type=lambda x: int(x, 0))
    r.add_argument("--sig", default="UFD",
                   help="DFU suffix signature, UFD (default) or RON")
    w = sub.add_parser("wrap", help="only write the wrapped image to a file")
    w.add_argument("image")
    w.add_argument("output")
    w.add_argument("--sig", default="UFD")
    args = p.parse_args()
    {"list": cmd_list, "run": cmd_run, "wrap": cmd_wrap}[args.cmd](args)


if __name__ == "__main__":
    main()
