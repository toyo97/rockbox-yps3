# Rockbox on the Samsung YP-S3: port notes

This branch revives the Rockbox port to the Samsung YP-S3. Bertrik Sikken
started the port in 2009-2010. Upstream Rockbox deleted it in February 2026
(commit `1a33d7990`, "Remove Meizu M3/M6SL/M6SP and Samsung YP-S3 ports")
because it no longer compiled and it had never been able to read the
internal flash.

**Status (October 2026): Rockbox runs on the player, started over USB
(DFU). Music plays from the internal flash, in stereo, and the FM radio
works. Rockbox writes to the music partition (settings, USB file
transfer); the firmware area is only written by the player's own
bootloader (see "For the owner" below).**

### What works on the hardware

| Part | Status |
|---|---|
| DFU loading (`s5l8700_dfu.py`) | works |
| LCD (type 2), backlight, button lights, touch keys, RTC, ADC | works |
| SDRAM (32 MB, timings from the original stage 1 loader) | works |
| CPU 196 MHz with caches, 100 Hz tick | works (see pitfalls) |
| Whimory VFL/FTL, read only (`ftl-yps3.c`) | works, mount in ~0.6 s |
| NAND DMA, hardware ECC | work, enabled after start-up self-tests |
| USB test bootloader: NAND reader, upload-and-run | works |
| Full Rockbox: menus, file browser | works |
| Playback (WM1800 codec, I2S DMA), headphones | works, stereo |
| FM radio (tuner answers with DEVICEID 0x1B63) | works, scan finds stations |
| USB mass storage | works, reads ~1.6-1.9 MB/s, writes ~0.56 MB/s |
| Writing to the flash (music partition) | works |
| Permanent install (`S3.ROM`, hold switch) | started from SDRAM over DFU only, see below |
| Battery calibration, power off on battery | not done |

## For the owner: install, go back, rescue

**Not all of this has been tried on the player yet; steps marked
[NOT YET TESTED] are unverified.** Background and open questions:
`docs/YPS3-INSTALL-DESIGN.md`.

### How it works

* The player's own bootloader looks for a file called `S3.ROM` in the top
  folder of the music drive every time it is switched on. If it finds one,
  it checks it, writes it as the firmware, deletes it and starts it.
  Installing Rockbox and going back both work this way. The bootloader
  itself is never changed.
* The scripts only check an image and copy it to the drive as `S3.ROM`.
  Nothing on the player changes until it is switched off and on.
* After the install, the **hold switch** chooses at power-on:
  **hold off: Rockbox, hold on: original firmware.**
* DFU mode is built into the player's chip and always works, whatever is
  on the flash. It lets the PC start a program on the player over USB
  without writing anything.

### What must be kept

* `nanddump/backup/` (git doesn't track it): the backup of this player's
  flash, made with `yps3_backup.py`. `hidden_area_ecc.img` in it is **the
  only copy of the original firmware.** Keep a copy of the directory
  somewhere other than this PC.
* The build directories `build-yps3` (bootloader), `build-yps3-dev`
  (bootloader with `-DYPS3_NO_AUTOBOOT` in `GCCOPTS`) and `build-yps3-full`
  (main build, after `make zip`), or rebuild them (see Building).

All commands below run from the top of this repository.

### Prerequisites

A Linux PC with Python 3, pyusb, dosfstools and gio:

```sh
sudo apt install python3 python3-usb dosfstools libglib2.0-bin
```

For DFU mode, allow normal users to talk to the player once (or put `sudo`
in front of the DFU commands):

```sh
sudo cp utils/s5l8700_dfu/99-s5l8700-dfu.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
```

### Prepare the images

```sh
mkdir -p nanddump/install
python3 utils/s5l8700_dfu/yps3_mkrom.py extract \
    nanddump/backup/hidden_area_ecc.img nanddump/install/S3-original.ROM
python3 utils/s5l8700_dfu/yps3_mkrom.py combine nanddump/install/S3-original.ROM \
    build-yps3/rockboot.ebn nanddump/install/S3-combined.ROM
sha256sum nanddump/install/S3-original.ROM
```

`extract` checks the image against the checksum stored in it and refuses
`hidden_area.img`, the copy without error correction. The sha256 of this
player's original firmware must be
`b0a98ac9cfde9b34f6a262a31224f8b5abe667f84c5fd0d5ae68ffab955e54bc`
(built Tue Mar 24 16:54:23 2009). An official Samsung `S3.ROM` for the
YP-S3 can replace it. `yps3_mkrom.py info FILE` shows what an image is.

### Finding the music drive

In Rockbox, plug the USB cable in: Rockbox shows its USB screen and the
player appears as a drive. Find it with

```sh
lsblk -o NAME,SIZE,LABEL,MOUNTPOINT
```

It is the partition of about 3.7 GB (4 GB player), e.g. `sdb1` mounted at
`/media/$USER/S3`; Rockbox shows only this partition. That mount point is
`MUSIC_DRIVE` below. As a safety net against picking another disk, the
scripts refuse anything under 512 MB. The original firmware shows no drive
but an MTP device (`mtp://...`).

### 1. First: the no-op upgrade (proves the way back works)

Once, before installing: write the original firmware over itself. Nothing
changes, but it proves that the player takes `S3.ROM` from the drive.
**Done on this player (October 2026): it worked.**

1. Charge the battery. Get the music drive mounted: start Rockbox over DFU
   (Rescue, way A) and plug the cable in.
2. `utils/s5l8700_dfu/yps3-restore.sh MUSIC_DRIVE`
3. Eject, unplug the cable, switch off (hold the power switch until the
   screen goes dark), switch on.
4. The player shows a short upgrade screen, then the original firmware
   starts, and `S3.ROM` is gone from the drive. If the original firmware
   doesn't start after the upgrade screen, see "When S3.ROM stays".

### 2. Install

```sh
utils/s5l8700_dfu/yps3-install.sh MUSIC_DRIVE
```

(defaults: `nanddump/install/S3-combined.ROM` and
`build-yps3-full/rockbox.zip`, which it unpacks to the drive). Eject,
unplug, switch off, switch on with hold off. The player writes the new
firmware, `S3.ROM` disappears and Rockbox starts.

Daily use: switch on with hold **off** for Rockbox. For the original
firmware switch on with hold **on**, then move hold off once it has
started (or its keys stay locked).

### 3. Go back to the original firmware

Through Rockbox's USB drive (the normal way):

1. Start Rockbox, plug the cable in, find `MUSIC_DRIVE`.
2. `utils/s5l8700_dfu/yps3-restore.sh MUSIC_DRIVE`
   (default image: `nanddump/install/S3-original.ROM`)
3. Eject, unplug, switch off, switch on. The original firmware starts; its
   settings may be reset. Delete `.rockbox` from the drive if you like.

Through MTP, when the original firmware starts but Rockbox doesn't
**[NOT YET TESTED: whether the bootloader sees a file copied through the
original firmware's MTP mode; the MTP listing does show the drive's top
folder, with `.rockbox`, so it looks likely]**:

1. Start the original firmware (hold on), plug the cable in.
2. `gio mount -l` shows the player's `mtp://` address; `gio list
   mtp://.../` shows its folders; pick the one that holds `Music`.
3. `utils/s5l8700_dfu/yps3-restore.sh --mtp mtp://.../FOLDER/`
4. Disconnect in the file manager, unplug, switch off, switch on. If
   `S3.ROM` is still there afterwards, use the rescue below.

### 4. Rescue: nothing starts (DFU mode)

Entering DFU mode:

1. Plug the USB cable in.
2. Hold the power switch down and keep holding it.
3. Press a pin into the reset hole for 1-2 seconds, then release the pin.
4. Wait a moment, then release the power switch. If both are released
   almost together, the player starts its firmware normally instead; just
   try again and wait a little longer before letting go of the power
   switch.

The screen stays dark. Check that the PC sees it:

```sh
python3 utils/s5l8700_dfu/s5l8700_dfu.py list
```

It must show `0419:0145 Samsung YP-S3 ...` (so does `lsusb`).

**Way A**, Rockbox on the drive is fine:

```sh
python3 utils/s5l8700_dfu/s5l8700_dfu.py run build-yps3/rockboot.ebn
```

(if nothing happens, add `--sig RON`). Rockbox starts from the drive. If no
USB screen and no drive appear within half a minute, unplug and plug the
cable in again. Then go back as in section 3.

**Way B**, Rockbox on the drive is broken too:

```sh
python3 utils/s5l8700_dfu/s5l8700_dfu.py run build-yps3-dev/rockboot.ebn
until python3 utils/s5l8700_dfu/yps3_nanddump.py info; do sleep 5; done
python3 utils/s5l8700_dfu/yps3_nanddump.py load --addr 0x08000000 --no-reset \
    build-yps3-full/rockbox.yps3
```

The second line asks the started loader for its status every 5 seconds
until it answers, which can take a minute or two (the tool resets the USB
connection by itself). Stop it with Ctrl+C after 5 minutes and start over
from DFU mode. The third line starts Rockbox from the PC. Replug the cable
if no drive appears, then go back as in section 3, or see section 5 if the
drive can't be mounted.

Starting a program over DFU writes nothing; switching off brings the
player back to how it was.

### 5. Broken music drive

The drive appears in `lsblk` but can't be mounted or shows errors:

1. Start Rockbox and get its USB drive (rescue, way B).
2. `lsblk -o NAME,SIZE,LABEL,MOUNTPOINT`: the 3.7 GB partition, say
   `sdb1`. Check twice that it is the player and not one of your disks.
3. `udisksctl unmount -b /dev/sdb1` if it is mounted.
4. Repair: `sudo fsck.vfat -a /dev/sdb1`
5. Only if that fails, reformat. **This erases all music and `.rockbox`
   on the player:** `sudo mkfs.vfat -F 32 /dev/sdb1`. Replug, create a
   folder `Music` on the drive, then go back as in section 3 (or install
   again). **[NOT YET TESTED] whether the original firmware accepts a drive
   formatted this way.**

**[NOT YET TESTED]** The player's bootloader reformats the music and system
drives by itself when it can't read them at power-on (this also erases all
music), so a very broken drive may already be repaired after a restart.

### When S3.ROM stays, and what an update looks like

The player shows a short upgrade screen, then starts the new firmware
(seen with the original firmware, October 2026). Charge the battery
first, unplug the cable, and don't switch off until a firmware has
started. Afterwards `S3.ROM` is gone from the drive (checked).

If there was no upgrade screen and `S3.ROM` is still there after switching
off and on:

* make sure the player was really off and the cable unplugged;
* check that the file is in the top folder of the music drive, named
  exactly `S3.ROM`, about 25 MB, with no `S3J.ROM` next to it;
* **[NOT YET TESTED]** the bootloader may only look for updates after the
  original firmware has been connected to a PC once: start it (hold on),
  plug it in, unplug, and try again;
* otherwise stop: nothing has changed and the player works as before.
  `S3.ROM` can stay or be deleted.

## The hardware

| Part | What it is | Rockbox driver status |
|---|---|---|
| SoC | Samsung S5L8700, ARM940T core. Rockbox runs it in big-endian mode, like the Meizu players. | CPU, timers, interrupts, DMA, I2C and ADC are shared with the iPod Nano 2G and Meizu ports |
| Internal RAM | 256 KB IRAM | used by the test bootloader |
| SDRAM | Samsung K4M561633G, 256 Mbit = **32 MB** | **not initialised yet** (timings unknown) |
| LCD | 176x220, RGB565, on the NOR/SRAM bus. Two panel types exist, detected on P0.4 | type 2 working in 2009, type 1 written but never tested (`yps3/lcd-yps3.c`) |
| Keys | Melfas capacitive touch keys, plus hold and power switches | working (`yps3/button-yps3.c`) |
| Button lights, backlight | GPIO and PWM | working (`yps3/backlight-yps3.c`) |
| FM radio | Silicon Labs Si4703 (Si4700 with RDS), I2C address 0x20 | working (generic `si4700.c`) |
| Audio codec | Wolfson WM1800G, I2C address 0x34. Undocumented, register layout close to the WM8960 | volume test only, no codec driver yet |
| Headphone detect, battery/USB ADC, charger | GPIO / ADC | working |
| RTC | S35390A-like, I2C 0x60-0x6F | working in 2009 |
| NAND | Samsung K9LBG08U0M MLC: 4 KB pages + 128 B spare, 128 pages (512 KB) per block, 4 GB per chip. Chip selects 2 and 3 are driven by GPIO (P6.5, P6.6) | chip IDs worked in 2009. Raw page reads added on this branch (**untested**) |
| USB device | S5L8700 USB core (not the OTG core of the Nano 2G) | stub only |

## What this branch adds

* The YP-S3 target is back in `tools/configure` as target 143, `samsungyps3`,
  bootloader only.
* The 2009 drivers are restored in `firmware/target/arm/s5l8700/yps3/`, and
  the bit-rot is fixed so the bootloader builds with a current GCC (13.2).
* **A linker layout for running from RAM over USB.** The old `boot.lds` put
  the code in the NOR flash window at `0x24000000`, which the YP-S3 doesn't
  have. Bertrik's thread says he used a local linker script for DFU that was
  never committed. The image is now linked into IRAM. `crt0.S` copies the
  image to its link address first, so it runs wherever the boot ROM loads it
  (the Meizu M3 boot ROM loads to `0x22020000`).
* A slot for the DFU length word at offset `0x24` of the image. Before this,
  the loader overwrote a literal-pool entry there.
* **A NAND probe** in the test bootloader (`bootloader/samsung_yps3.c`). It
  reads raw pages, shows them as hex and scans the chip for signs of the
  "Whimory" FTL that Rockbox already implements for the Nano 2G.
* `utils/s5l8700_dfu/s5l8700_dfu.py`: a Python and pyusb loader that uploads
  an image through the boot ROM's DFU mode and runs it from RAM. It uses the
  protocol of the old `meizu_dfu` tool.
* A GitHub Actions workflow that builds `rockboot.ebn` on every push. You can
  download it from the run's artifacts without installing a toolchain.

## How a Rockbox port is put together

```
apps/  lib/rbcodec/  plugins      UI, playback, codecs, database, themes
        |                         generic, reused unchanged (the vast majority of the code)
firmware/  kernel, FAT, fonts     generic
        |
firmware/target/arm/s5l8700/      SoC drivers, shared with Nano 2G and Meizu
        |
firmware/target/arm/s5l8700/yps3/ board drivers: LCD, keys, power, NAND...
firmware/export/config/samsungyps3.h   hardware description
bootloader/samsung_yps3.c         first program that runs on the device
```

A new player needs the bottom two layers. For the YP-S3, many of those
drivers are already shared or written. The remaining work is in the
storage, boot and audio paths.

## Main challenges, hardest first

1. **Reading the internal flash (the FTL).** Samsung's firmware stores its
   file system behind a flash translation layer ("Whimory"), which handles
   wear levelling, bad blocks and ECC. Rockbox can't see a single file until
   it understands that layer. Rockbox has a full Whimory implementation for
   the iPod Nano 2G (`ipodnano2g/ftl-nano2g.c`, about 2200 lines), and the
   NAND probe tests whether the YP-S3 uses the same format. The wiki found
   the string `"220W"` at NAND offset `0x7F000`, probably "Whimory 2.2.0".
   Four things can differ:
   * page size. The Nano 2G driver hard-codes 2 KB pages, but the YP-S3 chip
     has 4 KB pages with a 128-byte spare area, so at least the low-level
     layer and the spare layout need adapting;
   * the on-flash layout, which the probe checks;
   * the ECC engine, which differs between the S5L8700 and S5L8701 (the status
     register bit positions already differ, see `nand-yps3.c`);
   * byte order. The Nano 2G runs little-endian and the YP-S3 big-endian, so
     the FTL structures read from flash need byte swapping, or the CPU has to
     be switched to little-endian.
2. **Starting Rockbox without the USB cable.** Today the code only runs
   through DFU. A real install has to put a Rockbox bootloader where the boot
   ROM or Samsung's bootloader will start it. There's good news here: the wiki
   found DFU-format images (with the same `CUFD` marker at `+0x20` as our
   `rockboot.ebn`) stored raw at NAND offsets `0x82000`, `0x102000` and
   `0x180000`, followed by Samsung's bootloader at `0x188000`. So the boot ROM
   probably boots from NAND the same image format it accepts over USB, without
   encryption. We still need to confirm which copy it uses and how it checks
   it.
   **This is the step that could brick the player.** It stays recoverable only
   if DFU mode still works, which is likely because DFU lives in the mask ROM.
3. **SDRAM initialisation.** The full Rockbox needs the 32 MB SDRAM. The
   controller timings can be found by disassembling Samsung's firmware. The
   Meizu values in old `crt0.S` revisions are a starting point.
4. **Audio playback.** This needs an I2S/PCM driver (`pcm-s5l8700.c` exists
   and is shared) and a codec driver for the WM1800, which looks close to the
   WM8960. Rockbox has drivers for similar Wolfson parts.
5. **USB mass storage.** This needs a driver for the S5L8700 USB device core,
   which isn't written yet. Until then, files reach the player through the
   original firmware's MTP/UMS mode, as long as Rockbox can read the same FTL.
6. Smaller items: keymap (`apps/keymaps/`), battery discharge curve, charging,
   CPU frequency scaling, sleep/power-off.

A full `--type=n` build gets far today and fails on about 10 missing pieces:
keymap, audio codec, SDRAM timings and the debug screen.

## DFU mode facts (from the wiki)

* **How to enter it:** with the USB cable plugged in, hold the power switch,
  press the reset hole with a pin for 1-2 s, release the pin, then shortly
  after release the power switch (releasing both almost together boots the
  firmware instead). The screen stays dark.
* The player then appears as USB device **`0419:0145`**, the same as the
  Meizu M6SL. The old `meizu_dfu` tool works with it, and our loader uses the
  same protocol with the standard "UFD" suffix by default.
* The M6SL boot ROM loads images at the start of IRAM (`0x22000000`), where
  the bootloader is linked, so it should not need to copy itself at all. The
  self-copy only matters if the boot ROM loads it somewhere else.
* Samsung's service tool "BluesUtilNand" is a DFU image that gives raw
  access to the NAND over USB. If you can find it, it is the fastest way to
  dump the flash for FTL analysis.

### Known NAND layout (byte offsets from the wiki)

| Offset | Size | Content |
|---|---|---|
| `0x00000000` | 4 | `00000000`, an initial marker? |
| `0x0007F000` | 4 | `"220W"`, probably Whimory 2.2.0 |
| `0x00082000` | `0x8000` | DFU image (`CUFD` at `+0x20`), strings mention "NOR SST 2M" |
| `0x00102000` | `0x8000` | the same DFU image |
| `0x00180000` | `0x8000` | the same DFU image |
| `0x00188000` | `0x18C000` | Samsung bootloader: FTL code and the glowing S3 logo |
| `0x04F80000` | | tables, FAT or FTL |
| `0x05880000` | | file data (MP3) |

## Testing on a real player

The test is read-only. The bootloader runs from RAM and only sends
reset, read-ID, read-status and read-page commands to the NAND, so nothing on
the player is modified. Unplug the player to get back to the original
firmware. If it doesn't come back, hold the power slider to switch it off.

1. **Get the binary.** Download `yps3-test-bootloader` from the latest
   GitHub Actions run, or build it yourself (see the next section).
2. **Put the player in DFU mode:** with the USB cable plugged in, hold the
   power switch, press the reset hole with a pin for 1-2 s, release the
   pin, then shortly after the power switch. The screen stays dark.
3. Run `python3 s5l8700_dfu.py list`, using `sudo` on Linux. It should show
   `0419:0145`.
4. Run `python3 s5l8700_dfu.py run rockboot.ebn`. If nothing happens, try
   again with `--sig RON`. On Windows, first install the WinUSB driver for the
   DFU device with Zadig.
5. **Watch the LED behind the middle button:** it blinks once for LCD type 1
   and twice for type 2. Only type 2 has been tested, so if the screen stays
   blank, the blink count tells us which display driver to fix.
6. **Expected result:** the Rockbox logo, then a debug screen with GPIO
   values, RTC, FM, ADC and NAND IDs. Check the following:
   * Up/Down change the volume.
   * Left/Right tune the radio. Headphones act as the antenna.
   * Menu/Back change the brightness.
   * The button lights follow your finger.
   * Power switches the player off.
7. **NAND probe:** press **Select**. Take photos of:
   * page 0 of each bank (Menu switches banks);
   * a few blocks further in (Right moves one block on);
   * **self-check of the raw reads:** with 4 KB pages, the wiki's byte
     offsets map to pages. On bank 0, page 127 (Down from page 128, or Up
     127 times) should start with `220W`. Page 130 should show `CUFD` (or
     `DFUC`) at data offset `0x20`. If they do, the low-level NAND read
     works;
   * the result screen of **Back** (scan). The scan takes a few seconds per
     chip.

   Report the IDs and the scan counts. This is the key information for the
   storage driver. The K9LBG08U0M should read as `B655D7EC` (bytes
   EC D7 55 B6), decoded as `4096+128 128pg/blk 8192blk`.

## Status on real hardware (October 2026)

Tested over DFU on a 4 GB player: LCD type 2, one K9LBG08U0M chip. See the
table at the top.

### Pitfalls found on the way

* The 2009 bootloader switched to SVC mode to enable interrupts. Current
  crt0 runs `main` in SYS mode, so `main` ended up on the IRQ stack and was
  corrupted by the first tick. The fix is `enable_irq()`.
* The S5L8700 flash controller takes **one address cycle per `FMADDRn`
  register**. The S5L8701 (Nano 2G) packs them into `FMADDR0`/`FMADDR1`.
* The FIFO hands the first byte in the most significant lane, because the
  SoC runs big-endian.
* The Si4700 driver could loop forever while tuning. It now gives up after 20
  tries.
* **Caches:** crt0 set up MPU regions 0-4 but left 5-7 as the original
  firmware's stage 1 had them: SDRAM and IRAM in higher priority, non
  cacheable regions. The CPU ran like a 12 MHz one. crt0 now disables
  regions 5-7 (196 MHz measured).
* PCLK = HCLK / 2 is CLKCON2 bit 8 on this chip; bit 9 (Nano 2G) doesn't
  stick. PCLK was 100 MHz and the tick twice too fast.
* The slow CPU hid timing bugs: the ECC engine's pending bit can still be
  set from the previous decode, so wait a little before polling it.
* PCM DMA: writing DMABASE0/DMATCNT0 + DMACOM0 = 4 during a transfer
  restarts the channel, it doesn't queue. Each chunk is started from the
  completion interrupt.
* The tuner (DEVICEID 0x1B63, otherwise Si470x registers) never sets STC:
  a tune is done when READCHAN matches. P4.7 high powers it.
* USB power must be reported as a charger, or Rockbox powers off when idle
  with the cable plugged in.
* After the bootloader starts from DFU, the PC still talks to the DFU
  device until a USB bus reset; `yps3_nanddump.py` does it.
* `USEC_TIMER` (`udelay`) isn't running when started from DFU.
* USB (`usb-yps3.c`, S3C-HSUDC style controller): the Rockbox USB stack
  had never run on a big-endian CPU, the setup fields and descriptors need
  byte swapping. After a bus reset, wait for the reset to end before
  setting up the controller. Per endpoint the original firmware also sets
  registers 0x84 and 0x40. The endpoint index register must survive the
  interrupt handler. The byte count of an IN packet must be written again
  after the data, or packets go out with the wrong length.
* Debugging aids: the HW info screen shows the first 64 USB events after
  plugging in and can stay up in USB mode; a PC-side mass storage probe
  with pyusb needs a udev rule for the device's ID.

### NAND / FTL findings

* The flash is used as 2 dies x 4096 blocks of 128 pages (4096 + 128 bytes).
  Whimory pairs block *N* on die 0 with block *N*+4096 on die 1 and
  interleaves pages. Spare bytes 12-15 hold `2 * page + die` (big endian).
* Spare layout:

  | Bytes | Content |
  |---|---|
  | 0 | bad block marker |
  | 2-10 | ECC of the metadata |
  | 12-31 | metadata |
  | 40-119 | 8 x 10 bytes of ECC for the 512-byte sectors |

  Factory bad blocks are all `00`; there are 15 of them.
* Block 0 page 127 holds `"220W"` (Whimory 2.2) and tables of block numbers.
  Blocks 3-6 hold the DFU image and Samsung's bootloader.
* Blocks 42/44/46/48 are rotating one-page context records. The newest has
  the highest counter (`0x12`). Each holds 16-bit block numbers and the bad
  block list.
* Logical data is page-mapped through log blocks: physical order is not
  logical order. The FAT boot sectors were found at the start of physical
  blocks 1182 (FAT16, 40 MB, "S3" system drive) and 3799 (FAT32, 3.7 GB,
  music).
* Samsung's bootloader (blocks 4-6) contains the whole Whimory stack (TFS4
  Light, VFL, FTL) with debug strings and macro text. Its function names
  (`_LoadVFLCxt`, `VFL_Read(nVpn)`, `_GetReservedBlk`, log blocks) match
  the design behind Rockbox's Nano 2G FTL. **Next step:** disassemble it to
  learn the VFL context, map table and log block formats, then adapt
  `ftl-nano2g.c`.

## Building

The toolchains have no big-endian `libgcc`, which the full firmware needs.
Build one once with `tools/libgcc-be/build.sh ~/.local/rockbox-libgcc-be`.
`configure` looks there by default; override with `LIBGCC_BE=dir`.

```sh
# Ubuntu/Debian; Rockbox officially uses its own GCC 9.5 (tools/rockboxdev.sh)
sudo apt install gcc-arm-none-eabi
mkdir -p ~/bin && for t in /usr/bin/arm-none-eabi-*; do
  n=$(basename $t); ln -sf $t ~/bin/${n/arm-none-eabi/arm-elf-eabi}; done
export PATH=~/bin:$PATH

mkdir build-yps3 && cd build-yps3
../tools/configure --target=samsungyps3 --type=b
make -j$(nproc)          # -> rockboot.ebn (bootloader)

mkdir ../build-yps3-full && cd ../build-yps3-full
../tools/configure --target=samsungyps3 --type=n
make -j$(nproc) && make zip   # -> rockbox.yps3, rockbox.zip (.rockbox)
```

### Development loop

1. Put the player in DFU mode and run the development bootloader, which
   doesn't start the copy of Rockbox on the flash:
   `s5l8700_dfu.py run build-yps3-dev/rockboot.ebn` (`build-yps3-dev` is a
   bootloader build with `-DYPS3_NO_AUTOBOOT` added to `GCCOPTS`).
   It measures the clocks, mounts the flash, logs the results, then waits
   for USB requests.
2. Read its log with `yps3_nanddump.py info`.
3. Start a new bootloader build with `yps3_nanddump.py load
   build-yps3-dev/rockboot.ebn`, or the full firmware with
   `yps3_nanddump.py load --addr 0x08000000 --no-reset
   build-yps3-full/rockbox.yps3`. No DFU reset is needed.
4. The normal bootloader (`build-yps3`) starts `/.rockbox/rockbox.yps3` from
   the flash by itself. Hold any key to stay in the test program. Copy
   `.rockbox` from `rockbox.zip` with the original firmware over MTP, using
   `gio copy` per file (`cp` fails on MTP).

## Roadmap

1. Done: DFU loading, SDRAM, read-only FTL, full Rockbox, playback, radio.
2. FTL writes (settings, USB file transfer). Risky for the data on the
   flash, so it waits for the owner's go-ahead.
3. USB mass storage in Rockbox (the UDC is S3C2443-like, see
   `bootloader/samsung_yps3_usb.c`).
4. Battery calibration, power off on battery, LCD sleep, CPU frequency
   scaling.
5. A permanent bootloader install through the player's own `S3.ROM`
   firmware update ("For the owner" above), waiting for the owner's
   go-ahead.

## References

* Original porting thread: <https://forums.rockbox.org/index.php?topic=22418.0>
* Wiki: <https://www.rockbox.org/wiki/SamsungYPS3>
* Deleted code: `git show 1a33d7990^:firmware/target/arm/s5l8700/yps3/`
* Nano 2G FTL (Whimory): `firmware/target/arm/s5l8700/ipodnano2g/ftl-nano2g.c`
