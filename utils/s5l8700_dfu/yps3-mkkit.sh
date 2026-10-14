#!/bin/bash
# Builds the YP-S3 install and rescue kit: one folder with the firmware
# images, the tools and a plain-text guide (yps3-kit-README.txt), usable
# without the source tree.
#
#   yps3-mkkit.sh [-o KIT_DIR] [-b BACKUP_DIR] [BOOT_BUILD DEV_BUILD FULL_BUILD]
#
# Defaults (relative to the top of the source tree):
#   KIT_DIR     ~/YPS3-rescue-kit (must not exist yet)
#   BACKUP_DIR  nanddump/backup (from yps3_backup.py, with hidden_area_ecc.img)
#   BOOT_BUILD  build-yps3       bootloader build (rockboot.ebn)
#   DEV_BUILD   build-yps3-dev   bootloader build with -DYPS3_NO_AUTOBOOT
#   FULL_BUILD  build-yps3-full  main build after "make zip" (rockbox.zip)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$HERE/../.." && pwd)

# the original firmware of the player the backup comes from, FWOK cleared
ORIGINAL_SHA256=b0a98ac9cfde9b34f6a262a31224f8b5abe667f84c5fd0d5ae68ffab955e54bc

die() {
    echo "error: $*" >&2
    exit 1
}

kit=$HOME/YPS3-rescue-kit
backup=$TOP/nanddump/backup
while getopts o:b: opt; do
    case $opt in
        o) kit=$OPTARG ;;
        b) backup=$OPTARG ;;
        *) die "usage: $0 [-o KIT_DIR] [-b BACKUP_DIR] [BOOT DEV FULL]" ;;
    esac
done
shift $((OPTIND - 1))
[ $# -eq 0 ] || [ $# -eq 3 ] ||
    die "usage: $0 [-o KIT_DIR] [-b BACKUP_DIR] [BOOT DEV FULL]"
boot=${1:-$TOP/build-yps3} dev=${2:-$TOP/build-yps3-dev}
full=${3:-$TOP/build-yps3-full}

[ ! -e "$kit" ] || die "$kit exists already; remove it or use -o"
[ -f "$backup/hidden_area_ecc.img" ] ||
    die "$backup/hidden_area_ecc.img not found (yps3_backup.py)"
for d in "$boot" "$dev"; do
    [ -f "$d/rockboot.ebn" ] || die "$d/rockboot.ebn not found"
done
grep -q -- "-DYPS3_NO_AUTOBOOT" "$dev/Makefile" ||
    die "$dev is not a development bootloader build (-DYPS3_NO_AUTOBOOT)"
! grep -q -- "-DYPS3_NO_AUTOBOOT" "$boot/Makefile" ||
    die "$boot is a development bootloader build, need the normal one"
[ -f "$full/rockbox.zip" ] || die "$full/rockbox.zip not found (make zip)"

mkdir -p "$kit"
trap '[ $? -eq 0 ] || echo "kit incomplete, remove $kit" >&2' EXIT

python3 "$HERE/yps3_mkrom.py" extract "$backup/hidden_area_ecc.img" \
    "$kit/S3-original.ROM" > /dev/null
sha=$(sha256sum "$kit/S3-original.ROM" | cut -d' ' -f1)
[ "$sha" = "$ORIGINAL_SHA256" ] ||
    die "S3-original.ROM has sha256 $sha, not the known $ORIGINAL_SHA256;" \
        "a different player? Then update ORIGINAL_SHA256 and the README"
python3 "$HERE/yps3_mkrom.py" verify --original "$kit/S3-original.ROM"

cp "$boot/rockboot.ebn" "$kit/rockboot.ebn"
cp "$dev/rockboot.ebn" "$kit/rockboot-dev.ebn"
cp "$full/rockbox.zip" "$kit/rockbox.zip"
python3 - "$full/rockbox.zip" "$kit/rockbox.yps3" <<'EOF'
import sys, zipfile
with zipfile.ZipFile(sys.argv[1]) as z, open(sys.argv[2], "wb") as f:
    f.write(z.read(".rockbox/rockbox.yps3"))
EOF
python3 "$HERE/yps3_mkrom.py" combine "$kit/S3-original.ROM" \
    "$kit/rockboot.ebn" "$kit/S3-combined.ROM" > /dev/null
python3 "$HERE/yps3_mkrom.py" verify --combined "$kit/S3-combined.ROM"

for f in yps3-install.sh yps3-restore.sh yps3-drive.sh yps3_mkrom.py \
         s5l8700_dfu.py yps3_nanddump.py 99-s5l8700-dfu.rules; do
    cp "$HERE/$f" "$kit/$f"
done
cp "$HERE/yps3-kit-README.txt" "$kit/README.txt"
{
    echo "made $(date '+%Y-%m-%d %H:%M') by yps3-mkkit.sh"
    echo "source: $(git -C "$TOP" describe --always --dirty 2>/dev/null)"
    echo "rockbox.zip: $(unzip -p "$kit/rockbox.zip" .rockbox/rockbox-info.txt \
                         2>/dev/null | grep -i '^version' || echo '?')"
} > "$kit/VERSION.txt"
(cd "$kit" && sha256sum -- * | grep -v ' SHA256SUMS$' > SHA256SUMS)

echo
echo "kit ready: $kit"
ls -l "$kit"
cat <<EOF

Keep a copy of this folder somewhere other than this PC (USB stick, cloud
storage): without S3-original.ROM the original firmware can't be put back.
Start with README.txt, section 4 (the no-op upgrade).
EOF
