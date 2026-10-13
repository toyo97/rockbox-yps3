#!/bin/sh
# Builds a big-endian ARMv4T libgcc.a for the Samsung YP-S3 port.
#
# The ARM GNU toolchains (and Rockbox's own) only have little-endian
# multilibs. This assembles the endian-aware assembly parts of libgcc from
# the GCC sources and adds the C helpers in libgcc-be-extra.c.
#
# usage: build.sh OUTPUT_DIR    (creates OUTPUT_DIR/libgcc.a)
set -e
OUT=${1:?output directory}
CC=${CC:-arm-elf-eabi-gcc}
AR=${AR:-arm-elf-eabi-ar}
GCCREV=releases/gcc-13.2.0
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

for f in lib1funcs.S ieee754-df.S ieee754-sf.S bpabi.S; do
    curl -fsSL -o "$WORK/$f" \
        "https://raw.githubusercontent.com/gcc-mirror/gcc/$GCCREV/libgcc/config/arm/$f"
done

FLAGS="-mbig-endian -mcpu=arm940t -marm -O2"
FUNCS="_udivsi3 _divsi3 _umodsi3 _modsi3 _dvmd_tls _call_via_rX _interwork_call_via_rX \
 _lshrdi3 _ashrdi3 _ashldi3 _clzsi2 _ctzsi2 \
 _arm_negdf2 _arm_addsubdf3 _arm_muldivdf3 _arm_cmpdf2 _arm_unorddf2 \
 _arm_fixdfsi _arm_fixunsdfsi _arm_truncdfsf2 \
 _arm_negsf2 _arm_addsubsf3 _arm_muldivsf3 _arm_cmpsf2 _arm_unordsf2 \
 _arm_fixsfsi _arm_fixunssfsi \
 _aeabi_lcmp _aeabi_ulcmp _aeabi_ldivmod _aeabi_uldivmod"
for fn in $FUNCS; do
    $CC $FLAGS -DL$fn -c "$WORK/lib1funcs.S" -o "$WORK/$fn.o"
done
$CC $FLAGS -c "$HERE/libgcc-be-extra.c" -o "$WORK/extra.o"
rm -f "$OUT/libgcc.a"
$AR rcs "$OUT/libgcc.a" "$WORK"/*.o
echo "built $OUT/libgcc.a"
