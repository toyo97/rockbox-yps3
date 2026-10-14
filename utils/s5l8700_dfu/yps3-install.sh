#!/bin/bash
# Installs Rockbox on a Samsung YP-S3 next to the original firmware.
#
#   yps3-install.sh [--force] MUSIC_DRIVE S3-combined.ROM [rockbox.zip]
#
# S3-combined.ROM comes from
#   yps3_mkrom.py combine S3-original.ROM rockboot.ebn S3-combined.ROM
# rockbox.zip (optional) is unpacked to the drive (.rockbox).
# Only copies files to the mounted drive; the player's bootloader does
# the rest at the next start. Undo with yps3-restore.sh.
set -e
. "$(dirname "$0")/yps3-drive.sh"

force=
[ "$1" = --force ] && { force=--force; shift; }
[ $# -eq 2 ] || [ $# -eq 3 ] ||
    die "usage: $0 [--force] MUSIC_DRIVE S3-combined.ROM [rockbox.zip]"
mnt=$1 image=$2 zip=$3

$MKROM verify --combined "$image" || die "$image is not a combined image"
need=$(stat -c %s "$image")
if [ -n "$zip" ]; then
    python3 - "$zip" <<'EOF' || die "$zip has no .rockbox/rockbox.yps3"
import sys, zipfile
sys.exit(".rockbox/rockbox.yps3" not in zipfile.ZipFile(sys.argv[1]).namelist())
EOF
    need=$((need + $(python3 -c 'import sys, zipfile
print(sum(i.file_size for i in zipfile.ZipFile(sys.argv[1]).infolist()))' "$zip")))
fi
check_music_drive "$mnt" "$need" "$force"

if [ -n "$zip" ]; then
    python3 -m zipfile -e "$zip" "$mnt" || die "unpacking $zip failed"
    echo "$zip unpacked to $mnt"
fi
[ -f "$mnt/.rockbox/rockbox.yps3" ] ||
    echo "warning: $mnt/.rockbox/rockbox.yps3 is missing, Rockbox won't start"
copy_image combined "$image" "$mnt"
next_steps
cat <<EOF
  4. With the hold switch OFF the player starts Rockbox, with the hold switch
     ON it starts the original firmware.
To go back to the original firmware only: yps3-restore.sh.
EOF
