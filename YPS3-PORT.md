# Rockbox on the Samsung YP-S3: port notes

This branch revives the Rockbox port to the Samsung YP-S3. Bertrik Sikken
started the port in 2009-2010. Upstream Rockbox deleted it in February 2026
(commit `1a33d7990`, "Remove Meizu M3/M6SL/M6SP and Samsung YP-S3 ports")
because it no longer compiled and it had never been able to read the
internal flash.

**Status: experimental. The only output is a hardware test bootloader that
runs from RAM over USB. It writes nothing to the player's flash, so it
can't play music yet.**

## The hardware

| Part | What it is | Rockbox driver status |
|---|---|---|
| SoC | Samsung S5L8700, ARM940T core. Rockbox runs it in big-endian mode, like the Meizu players. | CPU, timers, interrupts, DMA, I2C and ADC are shared with the iPod Nano 2G and Meizu ports |
| Internal RAM | 256 KB IRAM | used by the test bootloader |
| SDRAM | 16 MB | **not initialised yet** (timings unknown) |
| LCD | 176x220, RGB565 | working in 2009 (`yps3/lcd-yps3.c`) |
| Keys | capacitive touch keys (probably Melfas), plus a hold/power slider | working (`yps3/button-yps3.c`) |
| Button lights, backlight | GPIO and PWM | working (`yps3/backlight-yps3.c`) |
| FM radio | Silicon Labs Si4700 with RDS, on I2C | working (generic `si4700.c`) |
| Audio codec | Wolfson "WM1800". The register writes match the WM8960 layout (our inference, not confirmed) | volume test only, no codec driver yet |
| Headphone detect, battery/USB ADC | GPIO / ADC | working |
| RTC | configured as S35390A | unverified |
| NAND | 2, 4 or 8 GB in up to 4 chips. Chip selects 2 and 3 are driven by GPIO. | chip IDs worked in 2009. Raw page reads added on this branch (**untested**) |
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
   NAND probe tests whether the YP-S3 uses the same format. Three things can
   differ:
   * the on-flash layout, which the probe checks;
   * the ECC engine, which differs between the S5L8700 and S5L8701 (the status
     register bit positions already differ, see `nand-yps3.c`);
   * byte order. The Nano 2G runs little-endian and the YP-S3 big-endian, so
     the FTL structures read from flash need byte swapping, or the CPU has to
     be switched to little-endian.
2. **Starting Rockbox without the USB cable.** Today the code only runs
   through DFU. A real install has to put a Rockbox bootloader where the boot
   ROM or Samsung's bootloader will start it. We still need to learn the
   on-NAND boot format (header, checksum, possibly encryption). The official
   firmware update file (`YP-S3*.ROM`) is the best place to look.
   **This is the step that could brick the player.** It stays recoverable only
   if DFU mode still works, which is likely because DFU lives in the mask ROM.
3. **SDRAM initialisation.** The full Rockbox needs the 16 MB SDRAM. The
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

## DFU mode facts

* Samsung's YP-S3 service manual installs a Windows driver called
  `Bluesdfu_vid419_pid0145.inf`, so in DFU mode the player should appear as
  USB device **`0419:0145`**. That is the same ID as the Meizu M6SL, whose
  `meizu_dfu` mode uses the standard "UFD" suffix (the loader's default).
* The M6SL boot ROM loads images at the start of IRAM (`0x22000000`), where
  the bootloader is linked, so it should not need to copy itself at all. The
  self-copy only matters if the boot ROM loads it somewhere else.

## Testing on a real player

The test is read-only. The bootloader runs from RAM and only sends
reset, read-ID, read-status and read-page commands to the NAND, so nothing on
the player is modified. Unplug the player to get back to the original
firmware. If it doesn't come back, hold the power slider to switch it off.

1. **Get the binary.** Download `yps3-test-bootloader` from the latest
   GitHub Actions run, or build it yourself (see the next section).
2. **Put the player in DFU / recovery mode** as described on the
   [SamsungYPS3 wiki page](https://www.rockbox.org/wiki/SamsungYPS3) and
   connect it to the computer.
3. Run `python3 s5l8700_dfu.py list`, using `sudo` on Linux. Report the
   `vid:pid` it shows. If nothing appears, run `lsusb` and report the result.
4. Run `python3 s5l8700_dfu.py run rockboot.ebn`. If nothing happens, try
   again with `--sig RON`. On Windows, first install the WinUSB driver for the
   DFU device with Zadig.
5. **Expected result:** the Rockbox logo, then a debug screen with GPIO
   values, RTC, FM, ADC and NAND IDs. Check the following:
   * Up/Down change the volume.
   * Left/Right tune the radio. Headphones act as the antenna.
   * Menu/Back change the brightness.
   * The button lights follow your finger.
   * Power switches the player off.
6. **NAND probe:** press **Select**. Take photos of:
   * page 0 of each bank (Menu switches banks);
   * a few blocks further in (Right moves one block on);
   * the result screen of **Back** (scan). The scan takes a few seconds per
     chip.

   Report the IDs and the scan counts. This is the key information for the
   storage driver.

## Building

```sh
# Ubuntu/Debian; Rockbox officially uses its own GCC 9.5 (tools/rockboxdev.sh)
sudo apt install gcc-arm-none-eabi
mkdir -p ~/bin && for t in /usr/bin/arm-none-eabi-*; do
  n=$(basename $t); ln -sf $t ~/bin/${n/arm-none-eabi/arm-elf-eabi}; done
export PATH=~/bin:$PATH

mkdir build-yps3 && cd build-yps3
../tools/configure --target=samsungyps3 --type=b
make -j$(nproc)          # -> rockboot.ebn
```

## Roadmap

1. Confirm that DFU loading and the 2009 drivers work with this build.
2. Read the NAND probe results and choose a storage strategy: reuse or adapt
   `ftl-nano2g.c`, or write a new FTL.
3. Initialise the SDRAM, then get a read-only FTL and FAT mount in the
   bootloader so it can list files on the screen.
4. Load `rockbox.yps3` from the flash through DFU, and get the full Rockbox
   running with the existing UI and file browser.
5. Audio codec, then playback.
6. A permanent bootloader install: the dual-boot method and the update file
   format.
7. USB, power management and polish.

## References

* Original porting thread: <https://forums.rockbox.org/index.php?topic=22418.0>
* Wiki: <https://www.rockbox.org/wiki/SamsungYPS3>
* Deleted code: `git show 1a33d7990^:firmware/target/arm/s5l8700/yps3/`
* Nano 2G FTL (Whimory): `firmware/target/arm/s5l8700/ipodnano2g/ftl-nano2g.c`
