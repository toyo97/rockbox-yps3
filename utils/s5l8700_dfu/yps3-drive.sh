# Shared by yps3-install.sh and yps3-restore.sh: checks that a directory is
# the YP-S3's music drive and copies an S3.ROM image to it. Linux, bash.

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
MKROM="python3 $HERE/yps3_mkrom.py"

die() {
    echo "error: $*" >&2
    exit 1
}

# check_music_drive MOUNTPOINT IMAGE_BYTES [--force]
# The player has two drives: the music drive (gigabytes) and a 40 MB system
# drive. Samsung's bootloader only looks for S3.ROM on the music drive.
check_music_drive() {
    local mnt=$1 need=$2 force=$3 line total avail target
    [ -d "$mnt" ] || die "$mnt is not a directory"
    line=$(df -P -B1 "$mnt" | awk 'NR == 2 { print $2, $4, $6 }')
    read -r total avail target <<< "$line"
    [ "$(realpath "$mnt")" = "$(realpath "$target")" ] ||
        die "$mnt is not a mount point (it is inside $target)"
    [ "$total" -ge $((512 * 1024 * 1024)) ] ||
        die "$mnt has only $((total / 1024 / 1024)) MB: this looks like the" \
            "40 MB system drive, use the music drive"
    [ "$avail" -ge $((need + 1024 * 1024)) ] ||
        die "$mnt needs $((need / 1024 / 1024 + 1)) MB free," \
            "has $((avail / 1024 / 1024)) MB"
    if [ -z "$(find "$mnt" -maxdepth 1 \( -iname music -o -iname .rockbox \) \
               -print -quit)" ]; then
        [ "$force" = --force ] ||
            die "$mnt has neither Music nor .rockbox; if it really is the" \
                "player's music drive, run again with --force"
    fi
    [ -z "$(find "$mnt" -maxdepth 1 -iname s3j.rom -print -quit)" ] ||
        die "$mnt/S3J.ROM exists: the bootloader might use it instead," \
            "move it away first"
}

# copy_image original|combined IMAGE MOUNTPOINT
copy_image() {
    $MKROM copy "--$1" "$2" "$3/S3.ROM" || die "S3.ROM not written"
    sync
}

next_steps() {
    cat <<EOF

Next:
  1. Eject the drive (file manager, or: udisksctl unmount -b /dev/sdX1).
  2. Unplug the USB cable and switch the player off (hold the power slider).
  3. Switch it on. The bootloader finds S3.ROM, writes it (this takes a
     while, don't switch off) and starts the new firmware. S3.ROM is then
     deleted from the drive.
EOF
}
