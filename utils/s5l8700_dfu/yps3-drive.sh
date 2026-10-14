# Shared by yps3-install.sh and yps3-restore.sh: checks that a directory is
# the YP-S3's music drive and copies an S3.ROM image to it. Linux, bash.

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
TOP=$(cd "$HERE/../.." && pwd)          # the repository
INSTALL_DIR=$TOP/nanddump/install       # images (not tracked by git)
MKROM="python3 $HERE/yps3_mkrom.py"

die() {
    echo "error: $*" >&2
    exit 1
}

# check_music_drive MOUNTPOINT IMAGE_BYTES [--force]
# Samsung's bootloader only looks for S3.ROM on the music drive (gigabytes).
# Rockbox shows only that one; the size check is a safety net against
# picking another disk (or the 40 MB system partition, if it ever shows).
check_music_drive() {
    local mnt=$1 need=$2 force=$3 line total avail target
    [ -d "$mnt" ] || die "$mnt is not a directory"
    line=$(df -P -B1 "$mnt" | awk 'NR == 2 { print $2, $4, $6 }')
    read -r total avail target <<< "$line"
    [ "$(realpath "$mnt")" = "$(realpath "$target")" ] ||
        die "$mnt is not a mount point (it is inside $target)"
    [ "$total" -ge $((512 * 1024 * 1024)) ] ||
        die "$mnt has only $((total / 1024 / 1024)) MB: not the player's" \
            "music drive"
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

# copy_image_mtp original|combined IMAGE MTP_FOLDER [--force]
# Through gio (GNOME's MTP support), to a player running the original
# firmware: MTP_FOLDER is mtp://<device>/S3/ (gio mount -l).
copy_image_mtp() {
    local kind=$1 image=$2 dir=${3%/} force=$4 list size
    command -v gio > /dev/null || die "gio is needed for --mtp (package gvfs)"
    list=$(gio list "$dir") || die "can't list $dir (see: gio mount -l)"
    if ! grep -qix music <<< "$list"; then
        [ "$force" = --force ] ||
            die "$dir has no Music folder; if it really is the player's" \
                "storage, run again with --force"
    fi
    ! grep -qix s3j.rom <<< "$list" ||
        die "$dir/S3J.ROM exists: the bootloader might use it instead," \
            "delete it first"
    mkdir -p "$INSTALL_DIR"
    # global: the trap runs after this function has returned
    mtp_tmp=$(mktemp -d "$INSTALL_DIR/mtp.XXXXXX")
    trap 'rm -rf "$mtp_tmp"' EXIT
    $MKROM copy "--$kind" "$image" "$mtp_tmp/S3.ROM" ||
        die "S3.ROM not prepared"
    gio copy "$mtp_tmp/S3.ROM" "$dir/S3.ROM" || die "copy to $dir failed"
    size=$(gio info -a standard::size "$dir/S3.ROM" |
           awk '/standard::size:/ { print $2 }')
    [ "$size" = "$(stat -c %s "$mtp_tmp/S3.ROM")" ] ||
        die "$dir/S3.ROM has $size bytes, the copy is incomplete"
    echo "$dir/S3.ROM copied, $size bytes"
}

next_steps() {
    cat <<EOF

Next:
  1. Eject the drive (file manager, or: udisksctl unmount -b /dev/sdX1;
     for MTP: gio mount -u on the player's mtp:// address).
  2. Unplug the USB cable and switch the player off (hold the power slider
     until the screen goes dark).
  3. Switch it on, with the cable unplugged and a charged battery. The
     bootloader finds S3.ROM, writes it (don't switch off meanwhile) and
     starts the new firmware. S3.ROM is then deleted from the drive: that
     is how you know it worked.
EOF
}
