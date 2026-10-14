# Rockbox on the Samsung YP-S3: port notes

This tree revives the Rockbox port to the Samsung YP-S3.
[Bertrik Sikken](https://github.com/bertrik) started the port in 2009-2010.
Upstream Rockbox deleted it in February 2026 (commit `1a33d7990`, "Remove
Meizu M3/M6SL/M6SP and Samsung YP-S3 ports") because it no longer compiled
and it had never been able to read the internal flash. For an overview and
downloads, see the [README](README.md).

**Status: Rockbox is installed permanently next to the original firmware
and works day to day on a 4 GB player: playback, FM radio, USB mass
storage, saving settings and files. Only that one player has been tested.**

### What works on the hardware

| Part | Status |
|---|---|
| DFU loading (`s5l8700_dfu.py`) | works |
| LCD (type 2) with sleep, backlight, button lights, touch keys, RTC, ADC | works |
| SDRAM (32 MB, timings from the original stage 1 loader) | works |
| CPU 196 MHz with caches, 100 Hz tick | works (see pitfalls) |
| Whimory VFL/FTL (`ftl-yps3.c`), reads and writes | works, mount in ~0.6 s |
| NAND DMA, hardware ECC | work, enabled after start-up self-tests |
| Full Rockbox: menus, file browser, database, themes, plugins | works |
| Playback (WM1800 codec, I2S DMA), headphones | works, stereo |
| FM radio (tuner answers with DEVICEID 0x1B63) | works, scan finds stations |
| USB mass storage | works, reads ~1.6-1.9 MB/s, writes ~0.56 MB/s |
| Permanent install (`S3.ROM`) | works: Rockbox, or the original firmware with the power slider |
| Charging, battery level, power off at low battery | works; the discharge curve is not finely calibrated |
| FM recording | WAV files are written but silent; under investigation |
| Video (mpegplayer) | being tested |
| LCD type 1 | written in 2009, never tested |

## Install, go back, rescue

**Steps marked [NOT YET TESTED] are unverified; everything else was done
on the tested player.** Background and open questions:
`docs/YPS3-INSTALL-DESIGN.md`.

### How it works

* The player's own bootloader looks for a file called `S3.ROM` in the top
  folder of the music drive every time it is switched on. If it finds one,
  it checks it, writes it as the firmware, deletes it and starts it.
  Installing Rockbox and going back both work this way. The bootloader
  itself is never changed.
* The scripts only check an image and copy it to the drive as `S3.ROM`.
  Nothing on the player changes until it is switched off and on.
* After the install, the player starts Rockbox. For the original firmware,
  keep the power slider pushed through the whole start animation; when it
stops on its last frame, let go.
* DFU mode is built into the player's chip and always works, whatever is
  on the flash. It lets the PC start a program on the player over USB
  without writing anything.

### What must be kept

* `nanddump/backup/` (git doesn't track it): the backup of your player's
  flash, made with `yps3_backup.py` (see "Back up the flash" below). `hidden_area_ecc.img` in it is **the
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

### Back up the flash

Before anything else, copy the parts of the flash that hold the original
firmware and the file system tables. This only reads from the player.

1. Put the player in DFU mode (see "Rescue" below for how).
2. Start the development bootloader (`build-yps3-dev/rockboot.ebn`, or
   `dev/rockboot.ebn` from the CI download), wait until it answers, then
   copy:

```sh
python3 utils/s5l8700_dfu/s5l8700_dfu.py run build-yps3-dev/rockboot.ebn
until python3 utils/s5l8700_dfu/yps3_nanddump.py info; do sleep 5; done
python3 utils/s5l8700_dfu/yps3_backup.py nanddump/backup
```

3. Keep `nanddump/backup/` somewhere safe, away from this PC too. Switch
   the player off and on to get back to the original firmware.

Samsung's firmware is not distributed with this repository; your backup
(or an official Samsung `S3.ROM` for the YP-S3) is the only source of it.

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
`hidden_area.img`, the copy without error correction. On the tested
player the original firmware was built Tue Mar 24 16:54:23 2009, sha256
`b0a98ac9cfde9b34f6a262a31224f8b5abe667f84c5fd0d5ae68ffab955e54bc`;
other players may have other versions. An official Samsung `S3.ROM` for
the YP-S3 can replace it. `yps3_mkrom.py info FILE` shows what an image is.

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
**Tested: it worked.**

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
unplug, switch off, switch on. The player writes the new firmware,
`S3.ROM` disappears and Rockbox starts.

Daily use (both tested): switch on as usual for Rockbox.
For the original firmware, keep the power slider pushed through the whole
start animation; when it stops on its last frame, let go. Power and hold
are the same slider, so the hold position can't be used to choose. With
Rockbox installed the start animation takes longer than before (the
player's bootloader loads the whole 25 MB image instead of 9 MB).

### 3. Go back to the original firmware

Through Rockbox's USB drive (the normal way):

1. Start Rockbox, plug the cable in, find `MUSIC_DRIVE`.
2. `utils/s5l8700_dfu/yps3-restore.sh MUSIC_DRIVE`
   (default image: `nanddump/install/S3-original.ROM`)
3. Eject, unplug, switch off, switch on. The original firmware starts; its
   settings may be reset. Delete `.rockbox` from the drive if you like.

Through MTP, when the original firmware starts but Rockbox doesn't
(tested):

1. Start the original firmware (power slider pushed through the start
   animation, see Install), plug the cable in, and open the
   player once in the file manager so that it gets mounted.
2. `gio mount -l` shows the player's address, like
   `mtp://Samsung_Electronics_Co._Ltd._S3_<serial>/`. Its storage
   folder `S3` is the music drive's top folder (`gio list` on it shows
   `Music` and `.rockbox`).
3. `utils/s5l8700_dfu/yps3-restore.sh --mtp mtp://<device>/S3/`
4. Disconnect in the file manager, unplug, switch off, switch on: a short
   upgrade screen, then the original firmware; `S3.ROM` is gone
   afterwards. If not, use the rescue below.

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
(seen with the original firmware). Charge the battery
first, unplug the cable, and don't switch off until a firmware has
started. Afterwards `S3.ROM` is gone from the drive (checked).

If there was no upgrade screen and `S3.ROM` is still there after switching
off and on:

* make sure the player was really off and the cable unplugged;
* check that the file is in the top folder of the music drive, named
  exactly `S3.ROM`, about 25 MB, with no `S3J.ROM` next to it;
* **[NOT YET TESTED]** the bootloader may only look for updates after the
  original firmware has been connected to a PC once: start it, plug it in,
  unplug, and try again;
* otherwise stop: nothing has changed and the player works as before.
  `S3.ROM` can stay or be deleted.

## The hardware

| Part | What it is | Driver |
|---|---|---|
| SoC | Samsung S5L8700, ARM940T core, run big-endian like the Meizu players | CPU, timers, interrupts, DMA, I2C, ADC shared with the iPod Nano 2G and Meizu ports |
| Memory | 256 KB IRAM, 32 MB SDRAM (Samsung K4M561633G) | `yps3/sdram-yps3.c` |
| LCD | 176x220, RGB565, on the NOR/SRAM bus; two panel types, detected on P0.4 | `yps3/lcd-yps3.c`, type 2 tested |
| Keys | Melfas capacitive touch keys, power/hold slider | `yps3/button-yps3.c` |
| FM radio | Si470x-compatible tuner (DEVICEID 0x1B63), I2C address 0x20 | generic `si4700.c` |
| Audio codec | Wolfson WM1800, I2C address 0x34; undocumented, register map of the WM8960 | `drivers/audio/wm1800.c` |
| NAND | Samsung K9LBG08U0M MLC, 4 KB pages + 128 B spare, Whimory FTL | `yps3/nand-yps3.c`, `yps3/ftl-yps3.c` |
| USB | S5L8700 device controller (S3C-HSUDC style, not the Nano 2G's OTG core) | `yps3/usb-yps3.c` |
| Battery, charger, headphones, RTC | ADC, GPIO, S35390A-like RTC on I2C | `yps3/power-yps3.c`, `yps3/powermgmt-yps3.c` |

## DFU mode

* With the USB cable plugged in: hold the power slider down, press the
  reset hole with a pin for 1-2 s, release the pin, then shortly after the
  power slider (releasing both almost together starts the firmware
  instead). The screen stays dark.
* The player then appears as USB device **`0419:0145`**, the same as the
  Meizu M6SL. `s5l8700_dfu.py` uploads an image to RAM and runs it, with
  the protocol of the old `meizu_dfu` tool. DFU is in the chip's mask ROM,
  so it works whatever is on the flash.
* On Windows, install the WinUSB driver for the DFU device with Zadig.

## Notes from the port

Tested on a 4 GB player: LCD type 2, one K9LBG08U0M chip.

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
  the design behind Rockbox's Nano 2G FTL. `ftl-yps3.c` follows its
  formats (VFL context, map tables, log blocks), and the writer keeps the
  flash readable by the original firmware. `utils/s5l8700_dfu/ftlsim` runs
  the FTL code on a PC, for tests and benchmarks.

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

mkdir ../build-manual && cd ../build-manual
../tools/configure --target=samsungyps3 --type=m
make manual                   # -> rockbox-manual.pdf (needs LaTeX)
```

The GitHub Actions workflow (`.github/workflows/yps3-build.yml`) runs the
same steps on every push.

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

1. FM recording: the files are silent. The HW info screen shows what the
   I2S receiver delivers (`rec n ... nz ...` lines) to tell missing data
   from zeros.
2. mpegplayer on big-endian ARM: confirm the assembler on hardware, and the
   start-up stutter while the buffer fills.
3. Battery: calibrate the discharge curve.
4. Untested hardware: LCD type 1 panels, other capacities and flash chips.

## References

* Original porting thread: <https://forums.rockbox.org/index.php?topic=22418.0>
* Wiki: <https://www.rockbox.org/wiki/SamsungYPS3>
* Deleted code: `git show 1a33d7990^:firmware/target/arm/s5l8700/yps3/`
* Nano 2G FTL (Whimory): `firmware/target/arm/s5l8700/ipodnano2g/ftl-nano2g.c`
