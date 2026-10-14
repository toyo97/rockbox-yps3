#!/bin/bash
# Puts the original Samsung firmware back on a YP-S3 (removes the Rockbox
# bootloader installed by yps3-install.sh).
#
#   yps3-restore.sh [--force] MUSIC_DRIVE [S3-original.ROM]
#   yps3-restore.sh [--force] --mtp MTP_FOLDER [S3-original.ROM]
#
# MUSIC_DRIVE: where the music drive is mounted (Rockbox's USB mode).
# --mtp: the original firmware's MTP storage, MTP_FOLDER = the storage
#   folder S3 of the address gio mount -l shows, like
#   mtp://Samsung_Electronics_Co._Ltd._S3_a73f4368000000cd/S3/ (needs gio).
# S3-original.ROM: default nanddump/install/S3-original.ROM, made with
#   yps3_mkrom.py extract nanddump/backup/hidden_area_ecc.img \
#       nanddump/install/S3-original.ROM
# or an official Samsung S3.ROM. See "For the owner" in YPS3-PORT.md.
# Only copies a checked file to the player; the player's bootloader writes
# it at the next start.
set -e
. "$(dirname "$0")/yps3-drive.sh"

force= mtp=
while [ $# -gt 0 ]; do
    case $1 in
        --force) force=--force; shift ;;
        --mtp) mtp=1; shift ;;
        *) break ;;
    esac
done
[ $# -eq 1 ] || [ $# -eq 2 ] ||
    die "usage: $0 [--force] [--mtp] MUSIC_DRIVE|MTP_FOLDER [S3-original.ROM]"
target=$1 image=${2:-$INSTALL_DIR/S3-original.ROM}
[ -f "$image" ] || die "$image not found (see the top of $0)"

$MKROM verify --original "$image" ||
    die "$image is not an original firmware image"
if [ -n "$mtp" ]; then
    copy_image_mtp original "$image" "$target" "$force"
else
    check_music_drive "$target" "$(stat -c %s "$image")" "$force"
    copy_image original "$image" "$target"
fi
next_steps
cat <<EOF
  4. The original firmware starts (its settings may be reset). Delete the
     .rockbox folder from the drive if you don't want it any more.
EOF
