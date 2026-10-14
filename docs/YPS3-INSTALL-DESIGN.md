# Samsung YP-S3: permanent install and restore, design note

Status: implemented (chain mode in `crt0.S`, tools in `utils/s5l8700_dfu`).
October 2026: the no-op upgrade worked (the original image written through
stage 2's upgrade path); the combined image has only run from SDRAM over
DFU, it hasn't been installed yet. The goal is a
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
* The bytes between `ROM Size` and `Data Address` are not padding (1.44
  MB of data), but nothing ever uses them: a normal start loads only
  `ROM Size` bytes (see 1.3), and the OF itself only reads the first 16
  sectors of area 1 and area 3 (its data part, from `Data Address`)
  through its own copy of the area helpers (`080e6960` read, `080e68e4`
  write). [H for the code, M that nothing else reads them]
* Area 1 holds stale data after `Total Size`. The image is only read up
  to `Total Size`.
* `Total Size` must be at most 0x1E00000 (`09e030f0`); the checked path
  loads `Total Size` bytes, the fast path `ROM Size/512 + 9` sectors
  after sector 0.
* The OF sets byte 0x1F1 of area 1 sector 0 to 1 (rewriting the first 16
  sectors) when `/CONFIG.DAT` exists (`of_main.c` near line 113095),
  which makes stage 2 take another load path (1.3, step 5).

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
   00000001`, so it runs on this player [H, confirmed by the no-op
   upgrade]. It looks for `/a/S3.ROM` or `/a/S3J.ROM`. When one is
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
   It checks `FWOK` in sector 0, then loads **`ROM Size/512 + 9` sectors**
   (header field +0x50; the checked and upgrade paths use `Total Size`,
   +0x58) from sector 1 to 0x08000200, **without the checksum**. So a
   normal start loads only the OF's code; the OF reads its data part
   itself. **Confirmed on the player (October 2026)**: a combined image
   with rockboot past `ROM Size` started Rockbox right after the upgrade
   (the upgrade path jumps with the whole file in RAM) and not on the
   next start.
6. Otherwise, the checked path (`09e018b4`) loads area 1 and verifies the
   checksum. If that fails, it loads area 4, checks `FWOK` and rewrites
   area 1 from it ("E. LOAD IMAGE", `09e0346c`).
7. Before the jump (`09e351c4`), stage 2 masks all interrupts in the
   interrupt controller, then enters 0x08000000 with a plain indirect
   call. **[L] cache and MPU state at that point.**
8. If no image loads, it falls back to stage 2's "USB Mode".

`/a/` is TFS4 volume 0, which is partition 0 of the MBR. That's the **music
partition**, the same one Rockbox exposes and writes (confirmed by the
no-op upgrade). The 40 MiB system partition is volume 1.
`/a/SYSDATA.bin` is a second upgrade path that only writes area 3. We
don't use it.

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
             word at 0x004 -> 0x088A1000, rockboot (was 0x08001000)
             ROM Size -> end of rockboot (9044384 -> about 9140000)
             Check Sum -> new value; Total Size unchanged
0x0000200  OF code, byte for byte, up to the old ROM Size (0x8A01A0)
0x08A1000  rockboot.ebn as built (about 100 KB), 4-byte aligned, over
           unused bytes of the gap before Data Address
0x0A00000  OF data part (Data Address), byte for byte
```

The OF stays where it expects to be (entry 0x08001000, data at 10 MiB), so
nothing in the OF is relocated or patched. A normal start loads `ROM Size`
bytes, now including rockboot, so a start takes about as long as before;
then the `ldr pc` goes to the stub. (The first layout put rockboot at
0x1800000, past `ROM Size`: only loaded by the upgrade start.) Setting
`ROM Size = Total Size` would also work but loads 25 MB on every start
instead of 9 MB.

**Chain stub** (the first instructions of `_newstart` in rockboot's crt0):

```
    PCON1 &= ~0xF               @ P1.0 (power slider) input, 4 bits per pin
    if (PDAT1 & 1) {            @ power slider pushed at the jump
        while (PDAT1 & 1) ;     @ wait until it is let go
        ldr pc, =0x08001000     @ original firmware
    }
    ...                         @ Rockbox
```

* **The power slider pushed through the start animation selects the OF;**
  the animation stops on its last frame until the slider is let go, then
  the OF starts with nothing pressed. Otherwise Rockbox. One GPIO, no I2C,
  no touch keys; only r0/r1 and PCON1's P1.0 field change before the OF
  (its entry code sets all registers itself, `0x08001000`: CPSR, CP15
  control = 0xF8, caches invalidated, sp; its GPIO init sets P1.0 to input
  too, `080e7e30`). Stage 2 never reads the power key (only PDAT1 bit 1,
  USB), so a long push doesn't switch it off. **[L] the hardware doesn't
  cut power on a long push either.**
* The first version used the hold switch (P4.3). On this player power and
  hold are one slider, and the OF didn't start with hold on (stuck on
  stage 2's last frame, seen twice); the OF polls hold at run time
  (`08637a78`, used by `0816ae34`, `08223a94`, `08223c70`) but no start-up
  wait was found in the code.
* **rockboot in chain mode** (implemented in `crt0.S`, commit "chain mode
  behind the original firmware"). The normal `rockboot.ebn` is used as is,
  with no separate build:
  1. **Detected at run time**: `_newstart` checks where it runs. From IRAM
     (DFU) nothing changes. From SDRAM (0x08000000-0x09FFFFFF) it is chain
     loaded, and the stub above runs first, written in crt0 itself.
  2. Rockbox path: IRQ/FIQ off, data cache cleaned, caches and MPU off,
     icache/dcache flushed. Only then does the existing self-copy to IRAM
     run and jump there; with stage 2's caches still on, that copy could
     go stale. MIUCON bit 0 is then set (IRAM at 0 for the vectors, as
     after DFU). **[L] that stage 2 leaves it otherwise.**
  3. **PLL and SDRAM are set up again, as after DFU.** This is deliberate:
     the code runs from IRAM by then, and nothing in SDRAM has to survive
     because the OF is no longer needed and `rockbox.yps3` is loaded
     afresh. The values are the ones stage 1 itself uses. Skipping them
     would leave stage 2's clocks (233 MHz), which the drivers don't
     expect.
  4. If `/.rockbox/rockbox.yps3` is missing or fails to load, the
     bootloader says how to start the OF instead. It doesn't try to
     start the OF itself. **[L] whether the OF does a full hardware
     init.**
  5. The DFU length word at 0x24 of rockboot is irrelevant here.
     `Total Size` covers the whole appended part.
* The OF sees a different `Total Size` and entry literal in its RAM copy of
  the header, plus extra bytes above its image. **[L] the OF doesn't read
  those.** Its `Check Sum` is still consistent.

## 3. What is written

| Step | Who writes | Where | Notes |
|---|---|---|---|
| copy `S3.ROM` to the music drive | Rockbox USB or the OF's MTP (both tested) | music FAT32 only | Rockbox's writer is limited to the MBR's first partition |
| upgrade | stage 2 | sectors 7872512 .. +N and 7933952 .. +N, N = Total Size/512 + 1; then the first 16 sectors of both again | through Samsung's FTL, the same code that writes the OF's data |
| upgrade | stage 2 | deletes `S3.ROM` | music FAT32 |
| first OF boot after a change | OF | its registry / `SYSTEM.VER` / MTP DB on the system partition | settings reset (1.4) |

Never written: raw blocks 0-49, the VFL context, area 0, stage 1/2. Areas 1/4
past N keep stale data, as they do today.

## 4. Procedures

The owner's procedures (preparing the images, the no-op upgrade, install,
going back, DFU rescue, broken music drive) are in `YPS3-PORT.md`, "For
the owner". The tools: `utils/s5l8700_dfu/yps3_mkrom.py` (`info`,
`extract`, `combine`, `verify`, `copy`, `selftest`),
`yps3-install.sh` and `yps3-restore.sh` (also `--mtp`). Image files
live in `nanddump/install/` (not tracked by git).

What the tools do that matters here:
* `extract` takes area 1 from `hidden_area_ecc.img` (never
  `hidden_area.img`: 506 sectors with uncorrected bit errors, it fails the
  checksum) and checks header, size and checksum. As stored, with `FWOK`:
  sha256 `37d8edeb978d507280c172e8f227fca8fa7b3442d6401a1d805996a6e3562dcd`,
  25160329 bytes.
* Every `S3.ROM` they write has **0x1FC-0x1FF cleared**. The checksum
  doesn't cover them, and without `FWOK` a copy that a power loss left
  half-written is refused at boot instead of being started (stage 2 stamps
  `FWOK` only after both copies are written).
* A restore writes only areas 1/4 (N sectors) and deletes the file. Bytes
  past N stay as they are; stage 2 and the OF never read them.

Developer checks, read only, with the development bootloader:
* after the no-op upgrade: area 0 config word 2 if `S3.ROM` stayed; areas
  1 and 4 compared with the backup up to N sectors; the hold switch state;
* before the install, the combined image over DFU:
  `yps3_nanddump.py load --addr 0x08000000 --no-reset S3-combined.ROM`
  (jumps to 0x08000000, the `ldr pc` goes to rockboot, so the chain path
  runs). **Done (October 2026): with hold off it chained into rockboot and
  Rockbox started normally. With hold on the OF started (blinking S3 logo)
  but hung at the logo for minutes.** This is not stage 2's hand-over state
  (caches, MIUCON, clocks, whatever stage 2 sets up for the OF), so the
  real behaviour with hold on is only known after the install (see the
  risks);
* after the install: hold off starts Rockbox, hold on the OF; areas 1/4
  read back and compared with `S3-combined.ROM`.

## 5. Risks

| Risk | Effect | Mitigation |
|---|---|---|
| Chain stub or rockboot crashes after install | player doesn't start (hold off), OF still starts with hold on if the stub works | restore via DFU + Rockbox USB; test over DFU first (section 4) |
| Stub broken, both paths dead | no start at all | stage 2 still runs the upgrade check first: restore via DFU |
| Stage 2 doesn't pick up `S3.ROM` (config word 2, file name, volume) | nothing happens | ruled out for now by the no-op upgrade (it worked); if the OF clears config word 2 later, connecting the OF to a PC once may set it again **[L]**; fallback is option B |
| Music FAT damaged by Rockbox's writer | stage 2 reformats both partitions at the next start (data lost, player still starts) | existing FTL write tests; back up music |
| Power loss during the upgrade | copy A or B partly written | `S3.ROM` files are made with 0x1FC cleared, so a half-written copy has no `FWOK` (step 2 overwrites the old sector 0 first) and boot stops in stage 2's USB Mode instead of jumping into it. Then copy the file again. **[L] whether the file is still there after a power loss** (it is deleted only at the end) |
| OF reacts to the changed header or the extra bytes | OF misbehaves with hold on | **[L]**; restore removes it |
| OF hangs when started through the stub (hold on). Seen over DFU from the development loader's state (hung at the S3 logo); real boot from stage 2 not tried | original firmware unusable while Rockbox is installed | Rockbox (hold off) still starts; restore through its USB drive: stage 2 handles `S3.ROM` before it starts any image. If it happens, look at what stage 2 sets up before the jump (`09e3517c`, interrupt state) and restore that in the stub |
| Hold switch input not as expected after stage 2 | wrong choice at start-up | checked after the no-op upgrade; the stub can configure PCON4 itself |

## 6. Tooling still missing

| Tool | Device access | Purpose |
|---|---|---|
| `yps3_nanddump.py hidden` (new subcommand) | read only | dump area 0, compare areas 1/4 with a file |

## 7. Open questions

1. Answered by the no-op upgrade (October 2026): the upgrade check runs,
   `/a/` is the music partition (MBR partition 0), `S3.ROM` with `FWOK`
   cleared is accepted, the OF starts after a short upgrade screen, and
   `S3.ROM` is deleted afterwards (the OF's MTP listing of the drive's top
   folder, which also shows `.rockbox`, no longer has it). Still open:
   whether config word 2 stays 1 (the OF might clear it), which of
   `S3.ROM` / `S3J.ROM` is tried first (decompiled lengths 11/12 bytes).
   A file copied over the OF's MTP (storage folder `S3` =
   `/a/`) is picked up too (tested).
3. Answered: normal starts take the fast path (`ROM Size` bytes), see
   1.3. Still open: what exactly the OF's `/CONFIG.DAT` byte (0x1F1)
   changes at the next start.
4. Cache/MPU state at the jump (`09e3517c`), and which rockboot init steps
   are safe when stage 1/2 already ran.
5. Does the OF ever read its image header or the hidden area beyond area 0
   (e.g. data from area 3 instead of RAM)? Search the OF for callers of
   `spm_hidden_nand_read`.
6. The 7-key service code and what stage 2's USB Mode exposes. It would be
   a second recovery path that doesn't need DFU.
