#!/bin/bash
# Puts the original Samsung firmware back on a YP-S3 (removes the Rockbox
# bootloader installed by yps3-install.sh).
#
#   yps3-restore.sh [--force] MUSIC_DRIVE [S3-original.ROM]
#
# S3-original.ROM: made once with
#   yps3_mkrom.py extract nanddump/backup/hidden_area_ecc.img S3-original.ROM
# or an official Samsung S3.ROM. Default: S3-original.ROM next to this script.
# Only copies a checked file to the mounted drive; the player's bootloader
# writes it at the next start.
set -e
. "$(dirname "$0")/yps3-drive.sh"

force=
[ "$1" = --force ] && { force=--force; shift; }
[ $# -eq 1 ] || [ $# -eq 2 ] ||
    die "usage: $0 [--force] MUSIC_DRIVE [S3-original.ROM]"
mnt=$1 image=${2:-$HERE/S3-original.ROM}
[ -f "$image" ] || die "$image not found (see the top of $0)"

$MKROM verify --original "$image" ||
    die "$image is not an original firmware image"
check_music_drive "$mnt" "$(stat -c %s "$image")" "$force"
copy_image original "$image" "$mnt"
next_steps
cat <<EOF
  4. The original firmware starts (its settings may be reset). Delete the
     .rockbox folder from the drive if you don't want it any more.
EOF
