Samsung YP-S3: Rockbox install and rescue kit
=============================================

This folder has everything needed to install Rockbox on this player, to
go back to the original Samsung firmware, and to rescue the player when
nothing starts. It doesn't need the Rockbox source code or anyone's help.

Keep a copy of this whole folder somewhere other than this PC (USB stick,
cloud storage). Without S3-original.ROM the original firmware can't be put
back. Check the copy with:  sha256sum -c SHA256SUMS

Some steps haven't been tried on the player yet. They are marked
[NOT YET TESTED].


0. WHAT IS IN HERE
------------------

  README.txt             this guide
  SHA256SUMS             checksums of all files (sha256sum -c SHA256SUMS)
  VERSION.txt            which Rockbox build the files come from
  S3-original.ROM        the player's original firmware, taken from the
                         backup of this player (see 9.)
  S3-combined.ROM        original firmware + Rockbox bootloader (install)
  rockbox.zip            Rockbox itself (the .rockbox folder)
  rockbox.yps3           Rockbox, for starting it over USB in a rescue
  rockboot.ebn           Rockbox bootloader, for starting over USB (DFU)
  rockboot-dev.ebn       the same, but waits for USB instead of starting
                         Rockbox from the player (rescue when .rockbox is
                         broken)
  yps3-install.sh        installs Rockbox (copies files to the player)
  yps3-restore.sh        puts the original firmware back
  yps3-drive.sh          used by the two scripts above
  yps3_mkrom.py          checks firmware images (used by the scripts)
  s5l8700_dfu.py         starts a program on the player in DFU mode
  yps3_nanddump.py       talks to rockboot-dev.ebn over USB
  99-s5l8700-dfu.rules   lets normal users use DFU mode (see 1.)


1. WHAT YOU NEED
----------------

A Linux PC (the scripts use bash and Linux tools) with:

  sudo apt install python3 python3-usb dosfstools libglib2.0-bin

(Fedora: python3-pyusb dosfstools gvfs. Other systems: python3, pyusb,
fsck.vfat/mkfs.vfat, gio.)

For DFU mode (sections 7 and 8), allow your user to talk to the player
once:

  sudo cp 99-s5l8700-dfu.rules /etc/udev/rules.d/
  sudo udevadm control --reload-rules

(or put "sudo" in front of the python3 commands in sections 7 and 8).

Run all commands from inside this folder:  cd ~/YPS3-rescue-kit


2. HOW THINGS WORK
------------------

* The player's own bootloader checks for a file called S3.ROM in the
  top folder of the music drive every time it is switched on. If it finds
  one, it checks it, writes it as the new firmware, deletes it, and starts
  it. Installing and going back both work like this. The bootloader
  itself is never changed by these steps.
* The scripts only check a firmware image and copy it to the drive as
  S3.ROM. Nothing is written to the player's firmware until you switch
  the player off and on.
* After installing, the HOLD switch chooses at power-on:
    hold OFF -> Rockbox
    hold ON  -> original firmware
* DFU mode is built into the player's chip. It always works, whatever is
  on the flash, and lets the PC start a program on the player over USB.


3. FINDING THE PLAYER'S MUSIC DRIVE
-----------------------------------

When Rockbox is in USB mode (plug the cable in while Rockbox runs, it
shows a USB screen), the player appears as a USB drive. Find it with:

  lsblk -o NAME,SIZE,LABEL,MOUNTPOINT

It is the partition of about 3.7 GB (4 GB player), e.g. sdb1 mounted at
/media/YOURNAME/S3 (the name may differ). Use that mount point as
MUSIC_DRIVE below. The scripts refuse anything smaller than 512 MB, such
as the player's 40 MB system drive.

The original Samsung firmware doesn't show a drive but an MTP device
(appears in the file manager, address mtp://...). See section 6, "via MTP".


4. BEFORE EVERYTHING: THE NO-OP UPGRADE (proves the way back works)
--------------------------------------------------------------------

Do this once, before installing. It writes the original firmware over
itself, so nothing changes, but it proves that the player takes S3.ROM
from the drive, which is what going back relies on.

  1. Charge the battery.
  2. Get the music drive mounted (section 3; to start Rockbox without
     installing it, use section 7, way A).
  3. ./yps3-restore.sh MUSIC_DRIVE
  4. Follow the steps the script prints: eject, unplug, switch off, switch
     on.
  5. Success: the original firmware starts, and S3.ROM is gone from the
     drive. If not, see section 10.


5. INSTALL ROCKBOX
------------------

  1. Charge the battery. Get the music drive mounted (section 3).
  2. ./yps3-install.sh MUSIC_DRIVE S3-combined.ROM rockbox.zip
  3. Eject, unplug, switch off, switch on (hold switch OFF).
  4. The bootloader writes the new firmware. S3.ROM disappears from the
     drive, and then Rockbox starts.

Daily use: switch on with hold OFF for Rockbox. For the original
firmware, switch on with hold ON, then move hold OFF once it has started
(otherwise its keys stay locked).


6. GO BACK TO THE ORIGINAL FIRMWARE
-----------------------------------

Via Rockbox's USB drive (the normal way):

  1. Start Rockbox (hold OFF), plug the cable in, find MUSIC_DRIVE
     (section 3).
  2. ./yps3-restore.sh MUSIC_DRIVE
  3. Eject, unplug, switch off, switch on.
  4. The original firmware starts. Its settings may be reset. Delete the
     .rockbox folder from the drive if you like.

Via MTP, when Rockbox doesn't start but the original firmware does (hold
ON) [NOT YET TESTED: it isn't known yet whether the bootloader picks up a
file copied through the original firmware's MTP mode]:

  1. Start the original firmware, plug the cable in.
  2. gio mount -l          (shows the player's mtp:// address)
     gio list mtp://.../   (find the folder that contains Music)
  3. ./yps3-restore.sh --mtp mtp://.../FOLDER/
  4. Disconnect in the file manager, unplug, switch off, switch on.
  If S3.ROM is still there afterwards, use section 7 instead.


7. RESCUE: NOTHING STARTS (DFU mode)
------------------------------------

Entering DFU mode: plug the USB cable into the PC, then press a pin into
the reset hole on the back while holding the power switch. The screen
stays dark. Check that the PC sees it:

  python3 s5l8700_dfu.py list

It must show "0419:0145 Samsung YP-S3 ...". (lsusb shows 0419:0145 too.)
If it doesn't, try the reset hole again.

Way A, Rockbox on the drive is fine:

  python3 s5l8700_dfu.py run rockboot.ebn

(If nothing happens, try again with --sig RON at the end.) Rockbox
starts from the drive. If no USB screen and no drive appear within half a
minute, unplug and plug the cable in again. Then go on with section 6
("via Rockbox's USB drive").

Way B, Rockbox on the drive is broken too:

  python3 s5l8700_dfu.py run rockboot-dev.ebn
  until python3 yps3_nanddump.py info; do sleep 5; done
  python3 yps3_nanddump.py load --addr 0x08000000 --no-reset rockbox.yps3

The second line asks the started loader for its status every 5 seconds
until it answers; that can take a minute or two (the tool resets the USB
connection by itself). Stop it with Ctrl+C after 5 minutes and start
over from DFU mode.

This starts Rockbox from the PC instead of from the drive. Unplug and plug
the cable in again if no drive appears, then go on with section 6. If
the drive can't be mounted, see section 8.

Starting a program over DFU writes nothing to the player. Unplugging and
switching off brings it back to how it was.


8. BROKEN MUSIC DRIVE
---------------------

Symptoms: the drive appears in lsblk but can't be mounted or shows
errors, or Rockbox can't find its files.

  1. Start Rockbox and get the USB drive as in section 7, way B.
  2. lsblk -o NAME,SIZE,LABEL,MOUNTPOINT   -> the 3.7 GB partition, say
     sdb1. Check twice that it is the player and not one of your own
     disks.
  3. Unmount it if it is mounted:  udisksctl unmount -b /dev/sdb1
  4. Repair:  sudo fsck.vfat -a /dev/sdb1
  5. Only if that fails: reformat. THIS ERASES ALL MUSIC AND .rockbox
     ON THE PLAYER:
       sudo mkfs.vfat -F 32 /dev/sdb1
     Then mount it again (unplug and plug in), create a folder called
     Music, and continue with section 6. Copy .rockbox back from
     rockbox.zip later if you want Rockbox again (or install again,
     section 5).

[NOT VERIFIED] The player's own bootloader reformats the music drive and
the system drive by itself when it can't read them at power-on (this
also erases all music). So a very broken drive may already be fixed
after a restart, with the original firmware (hold ON) still starting.


9. WHERE S3-original.ROM COMES FROM
-----------------------------------

It was taken from a backup of this player's flash
(hidden_area_ecc.img, made with yps3_backup.py) and checked against the
checksum inside it. Its SHA-256 is

  b0a98ac9cfde9b34f6a262a31224f8b5abe667f84c5fd0d5ae68ffab955e54bc

(the image as stored on the player, with its "FWOK" mark, has
37d8edeb978d507280c172e8f227fca8fa7b3442d6401a1d805996a6e3562dcd).
Firmware built Tue Mar 24 16:54:23 2009. An official Samsung S3.ROM for
the YP-S3 can be used in its place:  ./yps3-restore.sh MUSIC_DRIVE S3.ROM
To check any image:  python3 yps3_mkrom.py info FILE


10. WHAT AN UPDATE LOOKS LIKE, AND WHEN S3.ROM IS STILL THERE
-------------------------------------------------------------

What the player shows while writing, and how long it takes, isn't known
yet (it writes about 50 MB, probably a minute or two). Charge the battery
first, unplug the cable, and don't switch off until a firmware has
started. Success means S3.ROM is gone from the drive.

If S3.ROM is still there after switching off and on:
  * Make sure the player was really off (hold the power switch until the
    screen goes dark) and the cable was unplugged.
  * Check the file is in the top folder of the music drive, named
    exactly S3.ROM, about 25 MB, and that there is no S3J.ROM.
  * [NOT VERIFIED] The bootloader may only look for updates after the
    original firmware has been connected to a PC once. Start the
    original firmware (hold ON), plug it in, unplug it, and try again.
  * Otherwise stop here: nothing has been changed, the player works as
    before. S3.ROM can stay on the drive or be deleted.
