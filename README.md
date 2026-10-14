# Rockbox for the Samsung YP-S3

This is [Rockbox](https://www.rockbox.org), the open source firmware for
digital music players, running on the **Samsung YP-S3** (2009, 176x220
screen, touch keys, FM radio). It installs next to Samsung's firmware: the
player starts Rockbox, and the original firmware is still there when you
want it.

> **About this work.** It is based on the original Rockbox and on the
> partial YP-S3 port that [Bertrik Sikken](https://github.com/bertrik)
> started in 2009. Upstream Rockbox removed that port in 2026 because it
> had never been able to read the player's internal flash. All the changes
> that made it work on my YP-S3 were implemented by Claude (Anthropic's AI
> model), with my help testing and debugging on the device.

**Status: works day to day on the author's 4 GB player. It has not been
tested on any other unit. Use it at your own risk** (see
[Warnings](#warnings)).

## What works

| Feature | Status |
|---|---|
| Music playback from the internal flash | works, stereo (tested with MP3; all Rockbox codecs are built) |
| Menus, file browser, database, themes, plugins and games | works |
| FM radio, presets, scanning | works |
| USB mass storage (copy music and settings from a PC) | works, reads ~1.8 MB/s, writes ~0.6 MB/s |
| Settings and files saved to the flash | works |
| Permanent install, original firmware kept (power slider at start-up) | works |
| Going back to the original firmware, DFU rescue | works |
| Charging, battery level, LCD sleep, power off at low battery | works (battery level is not finely calibrated) |
| FM recording | **does not work yet**: the files are silent |
| Video (mpegplayer) | being tested |
| LCD type 1 panels | untested (only type 2 was available) |

The detailed list of what was done, the hardware notes and the open
issues are in [YPS3-PORT.md](YPS3-PORT.md).

## Installing

In short:

1. **Back up your player's flash** through DFU mode. The backup holds the
   only copy of your original firmware; keep it safe. You need it to make
   the install image and to go back. Samsung's firmware is not part of
   this repository and can't be distributed with it.
2. **Make the images**: `S3-original.ROM` (your firmware, from the backup)
   and `S3-combined.ROM` (your firmware with the Rockbox bootloader).
3. **Try the way back first**: put `S3-original.ROM` on the player as
   `S3.ROM` and restart. The player's own bootloader installs it, which
   changes nothing but proves that the update path works on your unit.
4. **Install**: `yps3-install.sh` copies `S3-combined.ROM` and Rockbox to
   the player. Restart: the player updates itself and starts Rockbox.

Every step, with the exact commands, going back, and the DFU rescue when
nothing starts, is in the user manual (chapter "Installation") and in
[YPS3-PORT.md, "Install, go back, rescue"](YPS3-PORT.md#install-go-back-rescue).

**Starting the original firmware:** keep the power slider pushed through
the whole start animation, and let go when it stops on its last frame.

**DFU mode** (rescue, backups): with the USB cable plugged in, hold the
power slider down, press a pin into the reset hole for 1-2 seconds,
release the pin, and shortly after release the power slider. The screen
stays dark and the PC sees USB device `0419:0145`.

## Downloads

Every push builds the bootloader, the firmware and the manual on GitHub
Actions. Open the latest successful run under **Actions** and download:

* `yps3-rockbox`: `rockbox.zip` (the firmware, unpacked to the player by
  the install script), `rockboot.ebn` (the bootloader that goes into
  `S3-combined.ROM`), the development bootloader for backups and rescue,
  and the tools from `utils/s5l8700_dfu/`;
* `yps3-manual`: the user manual as a PDF.

## Building

You need an ARM cross compiler and a big-endian `libgcc`, which the
standard toolchains don't include:

```sh
sudo apt install gcc-arm-none-eabi make perl zip python3 python3-usb
# Rockbox looks for the arm-elf-eabi- prefix
mkdir -p ~/bin && for t in /usr/bin/arm-none-eabi-*; do
  n=$(basename $t); ln -sf $t ~/bin/${n/arm-none-eabi/arm-elf-eabi}; done
export PATH=~/bin:$PATH
tools/libgcc-be/build.sh ~/.local/rockbox-libgcc-be

mkdir build-yps3 && cd build-yps3                # bootloader
../tools/configure --target=samsungyps3 --type=b && make -j$(nproc)
mkdir ../build-yps3-full && cd ../build-yps3-full  # firmware
../tools/configure --target=samsungyps3 --type=n && make -j$(nproc) && make zip
make manual                                       # needs LaTeX
```

More detail, including the development bootloader and the development
loop over USB, is in [YPS3-PORT.md](YPS3-PORT.md#building).

## Warnings

* This firmware comes with **no warranty** (GPL version 2, see
  [docs/COPYING](docs/COPYING)). Installing it rewrites the player's
  firmware area. A failed update or a mistake with the scripts can leave
  the player unable to start until it is rescued over DFU mode, and can
  erase the music on it.
* It has only been tested on one player: a 4 GB YP-S3 with a type 2 LCD
  and one Samsung K9LBG08U0M flash chip. Other versions may need changes.
* Always keep your flash backup, and charge the battery before an update.
* This is not an official Rockbox release, and the Rockbox project doesn't
  support it. Please don't report problems with it to them.

## Credits

* The [Rockbox](https://www.rockbox.org) developers, for the firmware and
  the S5L8700 drivers shared with the iPod Nano 2G and the Meizu players.
* [Bertrik Sikken](https://github.com/bertrik), who started the YP-S3 port
  (LCD, keys, radio, power), and everyone in the
  [porting thread](https://forums.rockbox.org/index.php?topic=22418.0) and
  on the [wiki page](https://www.rockbox.org/wiki/SamsungYPS3).

Rockbox's own documentation is in [docs/](docs/README) and at
<https://www.rockbox.org>.
