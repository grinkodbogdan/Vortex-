#!/bin/sh
# Make a bootable SD card image for lk2nd from the rootfs and kernel.
#
#   rootfs/mkimage.sh [-o OUTDIR] [-s SIZE_MB] [-r ROOTDEV]
#
# ROOTDEV defaults to /dev/mmcblk1p2 (second partition of the SD card on
# MSM8909).  The kernel boots without an initramfs, so it must be a device
# path the kernel itself understands.
#
# Layout (msdos, as lk2nd expects on MSM8909):
#   p1  ext2  VTXBOOT  64 MiB   extlinux/extlinux.conf, zImage, dtb
#   p2  ext4  vortex   rest     the root filesystem
#
# No loop devices or root needed: filesystems are built with mke2fs -d and
# spliced into the image with dd.  Requires e2fsprogs >= 1.43 and sfdisk.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
OUT=$HERE/../out
SIZE=1024
ROOTDEV=/dev/mmcblk1p2
while getopts o:s:r: opt; do
	case $opt in
	o) OUT=$OPTARG ;;
	s) SIZE=$OPTARG ;;
	r) ROOTDEV=$OPTARG ;;
	*) exit 2 ;;
	esac
done
ROOT=$OUT/rootfs
K=$OUT/kernel
DTB=qcom-msm8909-nokia-leo.dtb
BOOT_MB=64

die() { printf 'error: %s\n' "$*" >&2; exit 1; }
[ -d "$ROOT" ] || die "no rootfs in $ROOT (run rootfs/build.sh)"
[ -f "$K/zImage" ] || die "no kernel in $K (run kernel/build.sh)"
[ -f "$K/dtbs/$DTB" ] || die "missing $K/dtbs/$DTB"

T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

mkdir -p "$T/boot/extlinux"
cp "$K/zImage" "$T/boot/zImage"
cp "$K/dtbs/$DTB" "$T/boot/$DTB"
cat > "$T/boot/extlinux/extlinux.conf" <<EOF
timeout 1
default vortex
label vortex
	kernel /zImage
	fdt /$DTB
	append root=$ROOTDEV rootfstype=ext4 rootwait rw console=ttyMSM0,115200 quiet loglevel=3 firmware_class.path=/run/vortex/firmware
EOF

ROOT_MB=$((SIZE - BOOT_MB - 1))
mke2fs -q -t ext2 -L VTXBOOT -d "$T/boot" "$T/boot.img" "${BOOT_MB}M"
mke2fs -q -t ext4 -L vortex -O ^metadata_csum,^64bit -d "$ROOT" "$T/root.img" "${ROOT_MB}M"

IMG=$OUT/vortex-nokia-leo.img
rm -f "$IMG"
truncate -s "${SIZE}M" "$IMG"
sfdisk -q "$IMG" <<EOF
label: dos
start=1MiB, size=${BOOT_MB}MiB, type=83, bootable
start=$((1 + BOOT_MB))MiB, type=83
EOF
dd if="$T/boot.img" of="$IMG" bs=1M seek=1 conv=notrunc status=none
dd if="$T/root.img" of="$IMG" bs=1M seek=$((1 + BOOT_MB)) conv=notrunc status=none
echo "wrote $IMG"
