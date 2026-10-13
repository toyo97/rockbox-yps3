#!/usr/bin/env python3
"""FTL sector access through the YP-S3 test bootloader (ECC corrected).
    yps3_ftl.py status
    yps3_ftl.py read SECTOR COUNT OUT        read sectors to a file
    yps3_ftl.py enable / disable             allow writing (music partition)
    yps3_ftl.py rewrite SECTOR COUNT         write the same data back, verify
    yps3_ftl.py write SECTOR FILE            write a file's data, verify
    yps3_ftl.py merge LBN                    merge the log of a logical block
"""
import sys, time
sys.path.insert(0, __import__("os").path.dirname(__import__("os").path.abspath(__file__)))
import yps3_nanddump as nd

R_READ, R_WRITE, R_ENABLE, R_STATUS = 0x58, 0x59, 0x5A, 0x5B

def read(dev, sector, count):
    out = b""
    while count:
        n = min(count, 8)
        s = sector
        out += nd.vendor_in(dev, R_READ, s & 0xFFFF, s >> 16, n * 512)
        sector += n
        count -= n
    return out

def write(dev, sector, data):
    for off in range(0, len(data), 4096):
        chunk = data[off:off + 4096]
        s = sector + off // 512
        nd.vendor_out(dev, R_WRITE, s & 0xFFFF, s >> 16, chunk)

def status(dev):
    return nd.vendor_in(dev, R_STATUS, 0, 0, 256).decode(errors="replace").rstrip("\0")

def main():
    dev = nd.open_device()
    cmd = sys.argv[1]
    if cmd == "status":
        print(status(dev))
    elif cmd == "enable":
        nd.vendor_out(dev, R_ENABLE, 0x5752, 0, b"")
        print(status(dev))
    elif cmd == "merge":
        nd.vendor_out(dev, 0x5C, int(sys.argv[2], 0), 0, b"")
        print(status(dev))
    elif cmd == "disable":
        nd.vendor_out(dev, R_ENABLE, 0, 0, b"")
        print(status(dev))
    elif cmd == "read":
        s, n = int(sys.argv[2], 0), int(sys.argv[3], 0)
        t = time.time()
        with open(sys.argv[4], "wb") as f:
            done = 0
            while done < n:
                k = min(n - done, 2048)
                f.write(read(dev, s + done, k))
                done += k
        print("read %d sectors in %.0f s" % (n, time.time() - t))
    elif cmd in ("rewrite", "write"):
        s = int(sys.argv[2], 0)
        if cmd == "rewrite":
            data = read(dev, s, int(sys.argv[3], 0))
        else:
            data = open(sys.argv[3], "rb").read()
            data += b"\0" * (-len(data) % 512)
        write(dev, s, data)
        print(status(dev))
        back = read(dev, s, len(data) // 512)
        print("verify:", "OK" if back == data else "MISMATCH")
    else:
        print(__doc__)

if __name__ == "__main__":
    main()
