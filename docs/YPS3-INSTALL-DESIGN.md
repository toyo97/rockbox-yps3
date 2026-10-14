# Samsung YP-S3: permanent install and restore, design note

Status: design only. Nothing here has been run on a player. The goal is a
Rockbox install that starts without the USB cable, and a restore to the
original firmware (OF) that the owner can run alone with one documented
script.

Confidence markers: **[H]** read in code and confirmed by dump data,
**[M]** read in the decompiled code only, **[L]** inferred, to be checked
on the player. Addresses `09e0xxxx` are functions in Samsung's stage 2
bootloader (`nanddump/bootloader.c`, Ghidra); "sector" means a 512-byte
logical FTL sector.

## 1. How the player boots today

```
mask ROM ──> stage 1 (raw NAND, DFU-format image, 32 KB)
         ──> stage 2 "BOOTLOADER for BLUES (Whimory V2.2.0)", raw blocks 3-6,
             runs at 0x09E00000, has the whole Whimory VFL/FTL and TFS4 FAT
         ──> OF image, read through the FTL from the hidden area into
             0x08000000, entered at 0x08000000
```

* Stage 1 and stage 2 live in raw blocks below 42, which Rockbox never
  writes (`NAND_FIRST_WRITABLE_BLOCK` in `nand-yps3.c`). **This design
  never writes them either.** [H]
* DFU mode is in the mask ROM, so it survives anything written to the
  NAND. [H for the boot ROM, L that it can't be disabled by NAND contents]

### 1.1 The hidden area [H]

Stage 2 (`09e00b20`, `spm_init`) splits the end of the FTL's 7995392
logical sectors like this. The OF uses the same formula (`080e64c8`).

| Area (stage 2 index) | First sector | Sectors | Content |
|---|---|---|---|
| visible data | 0 | 7872448 | MBR, music partition (FAT32), system partition (FAT16, 7786496 + 81920) |
| 7 (whole hidden area) | 7872448 | 122944 | |
| 0 config | 7872448 | 64 | boot flags, see 1.4 |
| 1 image copy A | 7872512 | 61440 | the OF image |
| 2 / 3 | 7872512 / 7892992 | 20480 / 40960 | first 10 MiB (code) / rest (data) of copy A |
| 4 image copy B | 7933952 | 61440 | second copy, identical |
| 5 / 6 | 7933952 / 7954432 | 20480 / 40960 | same split for copy B |

Helpers: `09e00f50(area, sector, count, buf)` reads,
`09e00ef0(area, sector, count, buf)` writes. Both refuse ranges outside
the area.

### 1.2 Image format [H]

The OF image is stored at the start of area 1 (and of area 4):

| Offset | Content |
|---|---|
| 0x000 | `e51ff004` `08001000`: `ldr pc, [pc, #-4]`, entry point 0x08001000 |
| 0x008-0x01f | spaces |
| 0x020-0x1ef | text header, `Key : value` lines, ends at NUL or 0x1A (parser `09e02c40`/`09e02a14`) |
| 0x1f0, 0x1f1 | "magic code" bytes read by `09e0179c`, both 0 in our dump (see 1.3) |
| 0x1fc | `FWOK` once the image has been verified and marked |
| 0x200-TotalSize | code (from 0x1000) and data (from `Data Address`) |

Our player's header (both copies identical):

```
Builder : kichan.sung
Build Time : Tue Mar 24 16:54:23 2009
ROM Size : 9044384
Data Address : 10485760
Total Size : 25160329
Check Sum : 2694794985
Build Dir : NeoFirm
```

* `Check Sum` is the 32-bit sum of the bytes from 0x200 to `Total Size`.
  The header is not covered, so `FWOK` and the entry literal can change
  without changing it.
* The bytes between `ROM Size` and `Data Address` are **not padding**
  (1.44 MB of data), so nothing can be hidden there.
* Area 1 holds stale data after `Total Size`. The image is only read up
  to `Total Size`.
* `Total Size` must be at most 0x1E00000 (`09e030f0`). The fast load path
  reads `Total Size/512 + 9` sectors after sector 0, so keep it a few
  sectors below that limit.

### 1.3 Stage 2 boot sequence (`09e018d4`) [M]

1. Service-key check (`09e01334`/`09e01444`). A 7-key code on the touch
   keys selects "USB Mode" or a full NAND erase ("Whimory Erase
   completed"). Normal users never get there. **[L] what the code is.**
2. Mount the file systems. **If partition 0 can't be mounted, stage 2
   re-partitions and formats both partitions** (music and system,
   `09e01d8c`). It never formats the hidden area. This is the main risk
   for user data if the music FAT gets damaged, and it applies today
   already.
3. Load area 0 into RAM ("Check Config", `09e016d8`).
4. **"D. FIRMWARE UPGRADE CHECK"** (`09e035ec`), run when config word 0 is
   not 0xFEFE or config word 2 is 1. The dump has `0000fefe 0000beef
   00000001`, so it runs on this player [H for the values, L whether
   word 2 stays 1]. It looks for `/a/S3.ROM` or `/a/S3J.ROM`. When one is
   found:
   1. Read it into RAM (0x08000000). The file size must equal `Total Size`
      and the checksum must match.
   2. Write `Total Size/512 + 1` sectors of the image to **area 1 and
      area 4**.
   3. Stamp `FWOK` at 0x1FC and rewrite the first 16 sectors of both
      copies.
   4. Delete the file, then jump to 0x08000000 (the image is already in
      RAM).

   On any error it shows "Upgrade Failed", deletes the file and goes on
   with the normal boot.
5. "Check Magic Code" (`09e0179c`): read sector 0 of area 1. In our dump,
   bytes 0x1F0/0x1F1 are 0, which selects the **fast path** (`09e02764`).
   It checks `FWOK` in sector 0, then loads the rest of the image to
   0x08000200 **without the checksum**. **[M] the byte offsets come from
   Ghidra's stack layout.**
6. Otherwise, the checked path (`09e018b4`) loads area 1 and verifies the
   checksum. If that fails, it loads area 4, checks `FWOK` and rewrites
   area 1 from it ("E. LOAD IMAGE", `09e0346c`).
7. Before the jump (`09e351c4`), stage 2 masks all interrupts in the
   interrupt controller, then enters 0x08000000 with a plain indirect
   call. **[L] cache and MPU state at that point.**
8. If no image loads, it falls back to stage 2's "USB Mode".

`/a/` is TFS4 volume 0, which is partition 0 of the MBR. That's the **music
partition**, the same one Rockbox exposes and writes. **[M] the volume
letter mapping is the TFS4 convention, not yet seen in the code.** The 40
MiB system partition is volume 1. `/a/SYSDATA.bin` is a second upgrade
path that only writes area 3. We don't use it.

### 1.4 Area 0 (config) [M]

16 sectors, starting with `0000fefe 0000beef 00000001 00000001 ...` (10240
non-0xFF bytes). Stage 2 rewrites it in USB Mode and when it changes word 1.
The OF reads the hidden area in `__OemBmSetLocalConfig`
(`spm_hidden_nand_read`). It then resets its registry, `SYSTEM.VER` and
MTP database when it decides the firmware changed. This is the usual
"settings reset after a firmware update" and is expected after an install
or a restore. **This design never writes area 0.**

## 2. Options

| | Option | Writes | Verdict |
|---|---|---|---|
| A | Patch stage 1/2 in raw blocks 3-6 to start Rockbox | boot blocks | **rejected**: only DFU can recover a mistake, and those blocks are refused on purpose |
| B | Write rockboot into the hidden area with Rockbox's own FTL writer | areas 1/4 through our FTL code | possible fallback; needs the hidden-area guard in `ftl-yps3.c` lifted |
| **C** | **Let stage 2's own upgrade path install a combined image from `S3.ROM`** | areas 1/4, by Samsung's code | **recommended** |
| D | Keep the OF image, and have a Rockbox app on the OF side start Rockbox | none | not possible: no known way to run code from the OF |

### Option C: combined image through `S3.ROM`

Combined image layout (`Total Size` at most about 30 MiB):

```
0x0000000  OF header, unchanged except:
             word at 0x004 -> chain stub address (was 0x08001000)
             Total Size / Check Sum -> new values
0x0000200  OF image bytes 0x200 .. 25160329, byte for byte
0x1800000  chain stub + rockboot (about 100 KB), 4-byte aligned
```

The OF stays where it expects to be (entry 0x08001000, data at 10 MiB), so
nothing in the OF is relocated or patched. Stage 2 loads the whole image
and jumps to 0x08000000, then the `ldr pc` goes to the stub.

**Chain stub** (a few instructions at the start of the appended part; it
clobbers only `ip` and the flags):

```
    ldr   ip, =0x3CF00044      @ PDAT4
    ldr   ip, [ip]
    tst   ip, #8               @ P4.3 = hold switch, 1 = on
    ldrne pc, =0x08001000      @ hold on: original firmware, state untouched
    b     rockboot_start       @ hold off: Rockbox
```

* **Hold switch ON at power-up starts the OF; OFF starts Rockbox.** It
  needs one GPIO read, no I2C, no touch keys, and it leaves every register
  and memory byte the OF might rely on as stage 2 left them. **[L] PCON4
  is still an input after stage 2, and stage 2 ignores the hold switch at
  power-up.** The power-up test in 4.1 checks both. A touch-key choice
  would need rockboot's I2C driver and would dirty the state before the
  OF starts.
* **rockboot in chain mode**: `crt0.S` already copies the image to its IRAM
  link address with position-independent code, so it runs from 0x09800000
  as is. Changes needed:
  1. A build flag (e.g. `YPS3_CHAIN`) or a run-time test (pc in SDRAM)
     that skips the first-boot work stage 1 already did. In particular,
     don't reprogram the SDRAM controller or the PLL in a way that loses
     refresh. **[L] which steps are safe to repeat** (clock init has been
     repeated over DFU without trouble; SDRAM init over a running SDRAM
     hasn't).
  2. Turn off caches/MPU and mask interrupts first (stage 2 masks them,
     but the cache and MPU state are unknown).
  3. If `/.rockbox/rockbox.yps3` is missing or fails to load: show the
     error and ask to switch the hold switch on and restart. Starting the
     OF from inside rockboot after it took over IRAM and clocks isn't
     known to work. **[L] whether the OF does a full hardware init.**
  4. The DFU length word at 0x24 of rockboot is irrelevant here.
     `Total Size` covers the whole appended part.
* The OF sees a different `Total Size` and entry literal in its RAM copy of
  the header, plus extra bytes above its image. **[L] the OF doesn't read
  those.** Its `Check Sum` is still consistent.

## 3. What is written

| Step | Who writes | Where | Notes |
|---|---|---|---|
| copy `S3.ROM` to the music drive | Rockbox USB (or the OF's MTP) | music FAT32 only | Rockbox's writer is limited to the MBR's first partition |
| upgrade | stage 2 | sectors 7872512 .. +N and 7933952 .. +N, N = Total Size/512 + 1; then the first 16 sectors of both again | through Samsung's FTL, the same code that writes the OF's data |
| upgrade | stage 2 | deletes `S3.ROM` | music FAT32 |
| first OF boot after a change | OF | its registry / `SYSTEM.VER` / MTP DB on the system partition | settings reset (1.4) |

Never written: raw blocks 0-49, the VFL context, area 0, stage 1/2. Areas 1/4
past N keep stale data, as they do today.

## 4. Procedures

### 4.1 Before anything else: a no-op upgrade (validates restore)

The safest first write puts **the original image** back through the upgrade
path. The outcome is the same as before, and it proves the parts this
design depends on: `/a/` is the music drive, the file name, the upgrade
check runs, and a restore works.

1. `yps3_mkrom.py extract nanddump/backup/hidden_area_ecc.img S3.ROM`
   (tool to write, host only): take area 1 from the backup, check the
   header, size and checksum. Expect
   `sha256 37d8edeb978d507280c172e8f227fca8fa7b3442d6401a1d805996a6e3562dcd`,
   25160329 bytes (as stored, with `FWOK`). The tool then **clears
   0x1FC-0x1FF** in the file. The checksum doesn't cover them, and without
   `FWOK` a copy that a power loss left half-written is refused at boot
   instead of being started (stage 2 stamps `FWOK` only after both copies
   are written).
   **Use `hidden_area_ecc.img`, never `hidden_area.img`**: the plain image
   has 506 sectors with uncorrected bit errors and fails the checksum.
2. Copy `S3.ROM` to the root of the music drive (Rockbox USB drive, or MTP
   in the OF), eject, power off, power on.
3. Expect the upgrade screens, then the OF. `S3.ROM` must be gone. If it
   is still there, the upgrade check didn't run: read area 0 (read only,
   development bootloader) and check config word 2 before going further.
4. Read areas 1 and 4 with the development bootloader (read only) and
   compare them with the backup up to N sectors.
5. Same session, also read only: check the hold switch's state through the
   development bootloader, and note whether the OF starts normally with
   hold on.

### 4.2 Install

1. Build rockboot in chain mode, then
   `yps3_mkrom.py combine S3-original.ROM rockboot.bin S3.ROM` (fixes the
   entry literal, `Total Size` and `Check Sum`, refuses sizes over the
   limit, checks the result by parsing it back).
2. Test the combined image over DFU first: development bootloader,
   `yps3_nanddump.py load --addr 0x08000000 S3.ROM` with the jump at
   0x08000000. This is not the exact stage 2 hand-over state, but it
   catches layout and stub bugs. Try with hold on and off.
3. Copy `.rockbox` and `S3.ROM` to the music drive, eject, restart.
   Stage 2 installs it.
4. Check: hold off starts Rockbox, hold on starts the OF. Read areas 1/4
   back and compare with `S3.ROM`.

### 4.3 Restore (the owner's script)

`utils/s5l8700_dfu/yps3-restore.sh MOUNTPOINT [IMAGE]`, which only needs
Python 3 and the mounted music drive:

1. Without `IMAGE`, use `S3-original.ROM` next to the script. With an
   official Samsung `S3.ROM`, use that instead.
2. Check: `Total Size` equals the file size, `Check Sum` matches, the entry
   literal is 0x08001000 (refuse a combined image), and the sha256 if it is
   the known original. Clear 0x1FC-0x1FF in the copy (see 4.1.1).
3. Copy it to `MOUNTPOINT/S3.ROM`, `sync`, and print: "eject, power off,
   power on; the player shows the upgrade and starts the original
   firmware. Delete `/.rockbox` afterwards if you like."

Get the drive mounted:
* normally through Rockbox's USB mode (hold off) or the OF's MTP (hold on);
* if neither starts: DFU mode (pin in the reset hole + power), then
  `s5l8700_dfu.py run rockboot.ebn` and
  `yps3_nanddump.py load --addr 0x08000000 rockbox.yps3`, connect USB in
  Rockbox, and run the script. Stage 2 runs the upgrade check **before**
  it starts any image, so a broken image in areas 1/4 can't stop the
  restore. A wrapper `yps3-rescue.sh` should do these steps.

The restore writes only areas 1/4 (N sectors) and deletes the file.
Bytes past N in the hidden area stay as they are; stage 2 and the OF
never read them.

Keep `S3-original.ROM` (and the backup directory) in at least two places
outside the repository.

## 5. Risks

| Risk | Effect | Mitigation |
|---|---|---|
| Chain stub or rockboot crashes after install | player doesn't start (hold off), OF still starts with hold on if the stub works | restore via DFU + Rockbox USB (4.3); test over DFU first (4.2.2) |
| Stub broken, both paths dead | no start at all | stage 2 still runs the upgrade check first: restore via DFU (4.3) |
| Stage 2 doesn't pick up `S3.ROM` (config word 2, file name, volume) | nothing happens | found by the no-op upgrade (4.1) before any change; fallback is option B |
| Music FAT damaged by Rockbox's writer | stage 2 reformats both partitions at the next start (data lost, player still starts) | existing FTL write tests; back up music |
| Power loss during the upgrade | copy A or B partly written | `S3.ROM` files are made with 0x1FC cleared, so a half-written copy has no `FWOK` (step 2 overwrites the old sector 0 first) and boot stops in stage 2's USB Mode instead of jumping into it. Then copy the file again (4.3). **[L] whether the file is still there after a power loss** (it is deleted only at the end) |
| OF reacts to the changed header or the extra bytes | OF misbehaves with hold on | **[L]**; restore removes it |
| Hold switch input not as expected after stage 2 | wrong choice at start-up | checked in 4.1.5; the stub can configure PCON4 itself |

## 6. Tooling to write

| Tool | Device access | Purpose |
|---|---|---|
| `utils/s5l8700_dfu/yps3_mkrom.py` | none | `info`, `extract` (from the ECC backup), `combine`, `verify` |
| `utils/s5l8700_dfu/yps3-restore.sh` | none (writes one file to a mounted drive) | the owner's restore (4.3) |
| `utils/s5l8700_dfu/yps3-rescue.sh` | DFU + USB load, read only on the NAND | brings up Rockbox over DFU so the drive can be mounted |
| `yps3_nanddump.py hidden` (new subcommand) | read only | dump area 0, compare areas 1/4 with a file |
| rockboot chain mode + stub | n/a | 2.C |
| `YPS3-PORT.md` / manual page | n/a | the install and restore procedure for users |

## 7. Open questions

1. Is config word 2 (area 0) always 1, or does the OF clear it? (4.1.3)
2. Is `/a/` the music partition? Which of `S3.ROM` / `S3J.ROM` is tried
   first? (decompiled lengths 11/12 bytes) (4.1)
3. Fast path or checked path at boot: the 0x1F0/0x1F1 offsets in
   `09e0179c`. Disassemble `09e0179c` to confirm.
4. Cache/MPU state at the jump (`09e3517c`), and which rockboot init steps
   are safe when stage 1/2 already ran.
5. Does the OF ever read its image header or the hidden area beyond area 0
   (e.g. data from area 3 instead of RAM)? Search the OF for callers of
   `spm_hidden_nand_read`.
6. The 7-key service code and what stage 2's USB Mode exposes. It would be
   a second recovery path that doesn't need DFU.
