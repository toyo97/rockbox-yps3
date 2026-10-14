#!/bin/bash
# Installs Rockbox on a Samsung YP-S3 next to the original firmware.
#
#   yps3-install.sh [--force] MUSIC_DRIVE [S3-combined.ROM [rockbox.zip]]
#
# Defaults: nanddump/install/S3-combined.ROM, made with
#   yps3_mkrom.py combine nanddump/install/S3-original.ROM \
#       build-yps3/rockboot.ebn nanddump/install/S3-combined.ROM
# and build-yps3-full/rockbox.zip, unpacked to the drive (.rockbox).
# See "For the owner" in YPS3-PORT.md.
# Only copies files to the mounted drive; the player's bootloader does
# the rest at the next start. Undo with yps3-restore.sh.
set -e
. "$(dirname "$0")/yps3-drive.sh"

force=
[ "$1" = --force ] && { force=--force; shift; }
[ $# -ge 1 ] && [ $# -le 3 ] ||
    die "usage: $0 [--force] MUSIC_DRIVE [S3-combined.ROM [rockbox.zip]]"
mnt=$1 image=${2:-$INSTALL_DIR/S3-combined.ROM}
zip=${3:-$TOP/build-yps3-full/rockbox.zip}
[ -f "$image" ] || die "$image not found (see the top of $0)"
[ -f "$zip" ] || die "$zip not found (build it with make zip)"

$MKROM verify --combined "$image" || die "$image is not a combined image"
need=$(stat -c %s "$image")
python3 - "$zip" <<'EOF2' || die "$zip has no .rockbox/rockbox.yps3"
import sys, zipfile
sys.exit(".rockbox/rockbox.yps3" not in zipfile.ZipFile(sys.argv[1]).namelist())
EOF2
need=$((need + $(python3 -c 'import sys, zipfile
print(sum(i.file_size for i in zipfile.ZipFile(sys.argv[1]).infolist()))' "$zip")))
check_music_drive "$mnt" "$need" "$force"

python3 -m zipfile -e "$zip" "$mnt" || die "unpacking $zip failed"
echo "$zip unpacked to $mnt"
copy_image combined "$image" "$mnt"
next_steps
cat <<EOF
  4. With the hold switch OFF the player starts Rockbox, with the hold switch
     ON it starts the original firmware.
To go back to the original firmware only: yps3-restore.sh.
EOF
