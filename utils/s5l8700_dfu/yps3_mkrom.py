#!/usr/bin/env python3
"""Samsung YP-S3 firmware images (S3.ROM), host only, no device access.

    yps3_mkrom.py info FILE
    yps3_mkrom.py extract BACKUP OUT     original firmware from a backup of
                                         the hidden area (hidden_area_ecc.img)
    yps3_mkrom.py combine OF.ROM rockboot.ebn OUT
                                         original firmware + Rockbox bootloader
    yps3_mkrom.py verify [--original | --combined] FILE
    yps3_mkrom.py copy --original|--combined FILE DEST
                                         check, clear FWOK, write DEST through
                                         a temporary file, read it back
    yps3_mkrom.py selftest BACKUP        extract + verify + combine round trip

Image format (Samsung's stage 2 bootloader, docs/YPS3-INSTALL-DESIGN.md):
  0x000  ldr pc, [pc, #-4]; .word entry
  0x020  text header "Key : value" lines up to 0x1f0, NUL padded
  0x1fc  "FWOK", stamped by stage 2 after writing; cleared in our files
  Total Size bytes in all; Check Sum = sum of bytes 0x200..Total Size.
Stage 2 copies /S3.ROM from the music drive into both image copies of
the hidden area at the next start, then deletes it. A normal start (its
fast path) loads only the code part, ROM Size bytes plus a few sectors,
to 0x08000000. combine appends rockboot at 0x1800000 and sets ROM Size to
Total Size, so that every start loads everything (the start animation
takes longer). The original firmware's bytes stay untouched: with
rockboot in the gap between its code and its data part (ROM Size grown
over it) the original firmware hung (tested October 2026).
"""
import hashlib, os, re, struct, sys

# where hidden_area.img / hidden_area_ecc.img start (yps3_backup.py)
BACKUP_FIRST_SECTOR = 7868416
IMAGE_A_SECTOR = 7872512            # area 1, copy A
IMAGE_AREA_BYTES = 61440 * 512
# stage 2 refuses more than 0x1E00000 (the checked and upgrade paths load
# Total Size bytes; the fast path ROM Size/512 + 9 sectors after sector 0)
MAX_TOTAL = 0x1E00000 - 10 * 512

LDR_PC = 0xE51FF004
OF_ENTRY = 0x08001000
LOAD_ADDR = 0x08000000
ROCKBOOT_OFFSET = 0x1800000         # rockboot runs from 0x09800000

# the image this player shipped with, as stored (with FWOK)
KNOWN_OF = {
    "37d8edeb978d507280c172e8f227fca8fa7b3442d6401a1d805996a6e3562dcd":
        "Build Time Tue Mar 24 16:54:23 2009, from this player's backup",
}

HDR_START, HDR_END = 0x20, 0x1F0
REQUIRED = ("ROMSize", "DataAddress", "TotalSize", "CheckSum")

class ImageError(Exception):
    pass

def checksum(img, total):
    return sum(img[0x200:total]) & 0xFFFFFFFF

def parse_header(img):
    """Header fields like stage 2 parses them: key with blanks removed,
    value from the first non-blank to the end of the line."""
    if len(img) < 0x200:
        raise ImageError("shorter than the 512 byte header")
    text = img[HDR_START:HDR_END].split(b"\0")[0].split(b"\x1a")[0]
    fields, order = {}, []
    for line in text.decode("latin1").split("\n"):
        if ":" not in line:
            continue
        k, v = line.split(":", 1)
        k = re.sub(r"[ \t\r]", "", k)
        fields[k] = v.strip(" \t\r")
        order.append(k)
    for k in REQUIRED:
        if k not in fields or not re.fullmatch(r"\d+", fields[k]):
            raise ImageError("header has no numeric '%s'" % k)
    return fields

def num(fields, k):
    return int(fields[k])

def entry(img):
    w0, w1 = struct.unpack(">II", img[:8])
    if w0 != LDR_PC:
        raise ImageError("word 0 is %08x, not ldr pc, [pc, #-4]" % w0)
    return w1

def set_header_value(img, key, value):
    """Replace one numeric value in the header text, keeping the layout."""
    text = bytes(img[HDR_START:HDR_END])
    pat = re.compile(rb"(%s[ \t]*:[ \t]*)(\d+)" %
                     rb"[ \t]*".join(re.escape(c.encode()) for c in key
                                     if c != " "))
    m = pat.search(text)
    if not m:
        raise ImageError("can't find '%s' in the header" % key)
    new = text[:m.start(2)] + str(value).encode() + text[m.end(2):]
    end = new.index(b"\0") if b"\0" in new else len(new)
    if end >= HDR_END - HDR_START:
        raise ImageError("header text too long")
    img[HDR_START:HDR_END] = new[:end].ljust(HDR_END - HDR_START, b"\0")

def check(img, kind=None):
    """Checks an image; kind is None, 'original' or 'combined'.
    Returns (fields, list of warnings). Raises ImageError."""
    f = parse_header(img)
    total = num(f, "TotalSize")
    warn = []
    if total != len(img):
        raise ImageError("Total Size %d but the file has %d bytes"
                         % (total, len(img)))
    if total > MAX_TOTAL:
        raise ImageError("Total Size %d is over the limit %d"
                         % (total, MAX_TOTAL))
    e = entry(img)
    if e == OF_ENTRY and \
            not num(f, "ROMSize") < num(f, "DataAddress") < total:
        raise ImageError("ROM Size < Data Address < Total Size doesn't hold")
    cs = checksum(img, total)
    if cs != num(f, "CheckSum"):
        raise ImageError("checksum %d, header says %s" % (cs, f["CheckSum"]))
    if any(img[HDR_END:0x1FC]):
        raise ImageError("bytes 0x1f0-0x1fb are not 0 (stage 2 reads "
                         "0x1f0/0x1f1 to pick its load path)")
    if img[0x1FC:0x200] == b"FWOK":
        warn.append("FWOK is set at 0x1fc; files for S3.ROM should have it "
                    "cleared (extract and combine do)")
    if e == OF_ENTRY:
        found = "original"
    elif LOAD_ADDR + 0x200 <= e < LOAD_ADDR + total:
        found = "combined"
        off = e - LOAD_ADDR
        rb = img[off:]
        if len(rb) < 0x28 or rb[0] != 0xEA or rb[0x20:0x24] != b"CUFD":
            raise ImageError("no rockboot image at 0x%x" % off)
        if off < num(f, "DataAddress"):
            raise ImageError("rockboot at 0x%x is inside the original "
                             "firmware's code area: it hangs with that "
                             "layout" % off)
        if num(f, "ROMSize") != total:
            raise ImageError("ROM Size %s isn't Total Size: a normal start "
                             "(stage 2's fast path) loads only ROM Size "
                             "bytes, not rockboot" % f["ROMSize"])
    else:
        raise ImageError("unknown entry point %08x" % e)
    if kind and kind != found:
        art = {"original": "an original", "combined": "a combined"}
        raise ImageError("expected %s image, this is %s one"
                         % (art[kind], art[found]))
    f["_kind"] = found
    return f, warn

def known(img):
    """Description of a known original image, whatever its 0x1fc bytes."""
    if len(img) < 0x200:
        return None
    stored = bytearray(img)
    stored[0x1FC:0x200] = b"FWOK"
    return KNOWN_OF.get(hashlib.sha256(stored).hexdigest())

def describe(img, name):
    f, warn = check(img)
    print("%s: %s image" % (name, f["_kind"]))
    for k in ("Builder", "BuildTime", "ROMSize", "DataAddress", "TotalSize",
              "CheckSum", "BuildDir"):
        if k in f:
            print("  %-12s %s" % (k, f[k]))
    print("  entry        %08x" % entry(img))
    print("  sha256       %s" % hashlib.sha256(img).hexdigest())
    if known(img):
        print("  known image: " + known(img))
    for w in warn:
        print("  warning: " + w)

def clear_fwok(img):
    img[0x1FC:0x200] = b"\0\0\0\0"

def extract(backup):
    off = (IMAGE_A_SECTOR - BACKUP_FIRST_SECTOR) * 512
    with open(backup, "rb") as fp:
        fp.seek(off)
        area = fp.read(IMAGE_AREA_BYTES)
    if len(area) < 0x200:
        raise ImageError("%s is too short for a hidden area backup" % backup)
    total = num(parse_header(area), "TotalSize")
    stored = bytearray(area[:total])
    sha = hashlib.sha256(stored).hexdigest()
    check(stored, "original")       # checksum: catches uncorrected bit errors
    if sha not in KNOWN_OF:
        print("note: not the known image of this player (sha256 %s)" % sha)
    clear_fwok(stored)
    check(stored, "original")
    return stored, sha

def combine(of, rockboot):
    """The original image byte for byte, 0xff up to 0x1800000, rockboot.
    Entry -> rockboot, ROM Size = Total Size (a normal start loads
    everything), Check Sum, FWOK cleared."""
    of = bytearray(of)
    check(of, "original")
    if len(of) > ROCKBOOT_OFFSET:
        raise ImageError("original image is larger than 0x%x"
                         % ROCKBOOT_OFFSET)
    if len(rockboot) < 0x28 or rockboot[0] != 0xEA or \
            rockboot[0x20:0x24] != b"CUFD":
        raise ImageError("doesn't look like rockboot.ebn")
    img = of + b"\xff" * (ROCKBOOT_OFFSET - len(of)) + rockboot
    img += b"\xff" * (-len(img) % 4)
    img[4:8] = struct.pack(">I", LOAD_ADDR + ROCKBOOT_OFFSET)
    set_header_value(img, "Total Size", len(img))
    set_header_value(img, "ROM Size", len(img))
    set_header_value(img, "Check Sum", checksum(img, len(img)))
    clear_fwok(img)
    check(img, "combined")
    return img

def write(path, data):
    with open(path, "wb") as fp:
        fp.write(data)

def read(path):
    with open(path, "rb") as fp:
        return bytearray(fp.read())

def copy(kind, src, dest):
    img = read(src)
    check(img, kind)
    clear_fwok(img)
    tmp = dest + ".tmp"
    with open(tmp, "wb") as fp:
        fp.write(img)
        fp.flush()
        os.fsync(fp.fileno())
    os.replace(tmp, dest)
    if read(dest) != img:
        raise ImageError("%s reads back different" % dest)
    check(img, kind)
    print("%s: %s image written and read back, %d bytes%s" % (dest, kind,
          len(img), ", " + known(img) if known(img) else ""))

def selftest(backup):
    img, sha = extract(backup)
    f, _ = check(img, "original")
    assert sha in KNOWN_OF, "stored image sha256 %s" % sha
    assert f["CheckSum"] == "2694794985" and len(img) == 25160329
    restored = bytearray(img)
    restored[0x1FC:0x200] = b"FWOK"
    assert hashlib.sha256(restored).hexdigest() == sha
    fake = bytearray(b"\xea\x00\x00\x0f" + b"\0" * 0x1C + b"CUFD" +
                     bytes(range(256)) * 300)
    comb = combine(img, fake)
    f2, _ = check(comb, "combined")
    # the original's bytes are untouched, rockboot at 0x1800000, and a
    # normal start loads everything
    assert comb[0x200:len(img)] == img[0x200:]
    assert comb[ROCKBOOT_OFFSET:ROCKBOOT_OFFSET + len(fake)] == fake
    assert entry(comb) == LOAD_ADDR + ROCKBOOT_OFFSET
    assert f2["ROMSize"] == f2["TotalSize"] == str(len(comb))
    assert f2["DataAddress"] == f["DataAddress"]
    assert parse_header(comb)["BuildTime"] == f["BuildTime"]
    def refused(img, what):
        try:
            check(img)
        except ImageError:
            return
        raise AssertionError(what + " accepted")
    # rockboot appended but ROM Size not grown: not loaded at a normal start
    small = bytearray(comb)
    set_header_value(small, "ROM Size", f["ROMSize"])
    set_header_value(small, "Check Sum", checksum(small, len(small)))
    refused(small, "rockboot past ROM Size")
    # rockboot in the gap after the code: the original firmware hangs
    gap = bytearray(img)
    off = (num(f, "ROMSize") + 0xfff) & ~0xfff
    gap[off:off + len(fake)] = fake
    gap[4:8] = struct.pack(">I", LOAD_ADDR + off)
    set_header_value(gap, "ROM Size", off + len(fake))
    set_header_value(gap, "Check Sum", checksum(gap, len(gap)))
    refused(gap, "rockboot in the code gap")
    try:
        check(comb, "original")
        raise AssertionError("combined image passed as original")
    except ImageError:
        pass
    bad = bytearray(img)
    bad[0x1000] ^= 1
    try:
        check(bad)
        raise AssertionError("bit error not caught")
    except ImageError:
        pass
    print("selftest passed: extract reproduces checksum %s, sha256 %s; "
          "combine round trip ok" % (f["CheckSum"], sha))

def main(argv):
    if len(argv) < 2:
        sys.exit(__doc__)
    cmd, args = argv[1], argv[2:]
    try:
        if cmd == "info" and len(args) == 1:
            describe(read(args[0]), args[0])
        elif cmd == "extract" and len(args) == 2:
            img, sha = extract(args[0])
            write(args[1], img)
            print("stored image sha256 %s (%s)" %
                  (sha, KNOWN_OF.get(sha, "unknown")))
            describe(img, args[1])
        elif cmd == "combine" and len(args) == 3:
            img = combine(read(args[0]), read(args[1]))
            write(args[2], img)
            describe(img, args[2])
        elif cmd == "verify" and args:
            kind = None
            if args[0] in ("--original", "--combined"):
                kind = args.pop(0)[2:]
            if len(args) != 1:
                sys.exit(__doc__)
            f, warn = check(read(args[0]), kind)
            for w in warn:
                print("warning: " + w)
            img = read(args[0])
            print("%s: ok, %s image, %s bytes%s" % (args[0], f["_kind"],
                  f["TotalSize"], ", " + known(img) if known(img) else ""))
        elif cmd == "copy" and len(args) == 3 and \
                args[0] in ("--original", "--combined"):
            copy(args[0][2:], args[1], args[2])
        elif cmd == "selftest" and len(args) == 1:
            selftest(args[0])
        else:
            sys.exit(__doc__)
    except (ImageError, OSError) as e:
        sys.exit("error: %s" % e)

if __name__ == "__main__":
    main(sys.argv)
