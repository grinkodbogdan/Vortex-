#!/bin/sh
# Turn the output of rootfs/build.sh into flashable images for the Nokia
# 6300 4G.
#
#   sudo rootfs/mkimage.sh [-o OUTDIR] [-s ROOTFS_MB]
#
# Writes OUTDIR/vortex-nokia-6300-4g/:
#   boot-partition.img     lk2nd + Vortex boot image at +512 KiB -> "boot" (EDL)
#   userdata.img           root filesystem (raw ext4)            -> "userdata" (EDL)
#   vortex-sdcard.img      or: the root filesystem on a microSD card
#   lk2nd.img, vortex-boot.img, vortex-rootfs.simg   the same pieces for
#                          lk2nd's fastboot ("flash boot" / "flash userdata")
#   FLASHING.md, SHA256SUMS
#
# The initramfs mounts whichever partition is labelled "vortex", so the
# same boot image works with either root filesystem.
# Needs: python3, mke2fs (e2fsprogs >= 1.43), sfdisk, img2simg, gzip, zstd.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$HERE/.." && pwd)
OUT=$TOP/out/phone
SIZE=
while getopts o:s: opt; do
	case $opt in
	o) OUT=$OPTARG ;;
	s) SIZE=$OPTARG ;;
	*) exit 2 ;;
	esac
done
ROOT=$OUT/rootfs
SYS=$OUT/sysroot
K=$OUT/kernel
DTB=qcom-msm8909-nokia-leo.dtb
DIST=$OUT/vortex-nokia-6300-4g
# Quiet boot: the panel shows the Vortex logo (vortex-splash), kernel and
# init messages go to the serial port and the USB debug console only.
CMDLINE="console=ttyMSM0,115200 quiet loglevel=3 vt.global_cursor_default=0 firmware_class.path=/run/vortex/firmware"

msg() { printf '==> %s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }
[ -d "$ROOT" ] || die "no rootfs in $ROOT (run rootfs/build.sh)"
[ -f "$K/zImage" ] && [ -f "$K/dtbs/$DTB" ] || die "no kernel in $K (run rootfs/build.sh)"
rm -rf "$DIST"
mkdir -p "$DIST"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

msg "initramfs"
I=$T/initramfs
mkdir -p "$I/bin" "$I/proc" "$I/sys" "$I/dev" "$I/newroot"
cp "$ROOT/usr/bin/busybox.static" "$I/bin/busybox" 2>/dev/null ||
	cp "$SYS/usr/bin/busybox.static" "$I/bin/busybox" 2>/dev/null ||
	die "busybox.static missing (add busybox-static to the sysroot)"
install -m755 "$TOP/tools/phone/initramfs-init" "$I/init"
install -m755 "$ROOT/usr/libexec/vortex/vortex-splash" "$I/bin/vortex-splash"
# Early modules, as postmarketOS' device-nokia-leo loads them: the panel (so
# boot messages show on screen), the charger (it reports USB VBUS; without
# it the USB controller never comes up), the fuel gauge, and the USB serial
# gadget for the debug console.
python3 "$TOP/tools/phone/initramfs-modules.py" "$ROOT/usr/lib/modules/$(cat "$K/kernel.release")" \
	"$I/lib/modules" panel-mipi-dbi pm8916-lbc pm8916-bms-vm libcomposite u_serial usb_f_acm
mkdir -p "$I/lib/firmware"
cp "$ROOT"/usr/lib/firmware/nokia,leo-*.bin "$I/lib/firmware/"
python3 "$TOP/tools/phone/mkcpio.py" "$I" "$T/initramfs.gz"

msg "boot image"
# lk2nd boots zImage with the device tree appended (pmOS deviceinfo for nokia-leo)
cat "$K/zImage" "$K/dtbs/$DTB" > "$T/zImage-dtb"
python3 "$TOP/tools/phone/mkbootimg.py" --kernel "$T/zImage-dtb" --ramdisk "$T/initramfs.gz" \
	--cmdline "$CMDLINE" --base 0x80000000 --kernel-offset 0x00008000 \
	--ramdisk-offset 0x02700000 --second-offset 0x00f00000 --tags-offset 0x02500000 \
	--pagesize 2048 -o "$DIST/vortex-boot.img"
cp "$K/lk2nd.img" "$DIST/lk2nd.img"
# One image for the whole boot partition: lk2nd boots the Android boot image
# it finds 512 KiB into the partition (where its own "fastboot flash boot"
# would put it).
[ "$(stat -c %s "$K/lk2nd.img")" -le 524288 ] || die "lk2nd.img is larger than 512 KiB"
cp "$K/lk2nd.img" "$DIST/boot-partition.img"
truncate -s 512K "$DIST/boot-partition.img"
cat "$DIST/vortex-boot.img" >> "$DIST/boot-partition.img"

msg "root filesystem"
used=$(du -sm "$ROOT" | cut -f1)
SIZE=${SIZE:-$((used + used / 4 + 256))}     # room for apps and first-boot caches
mke2fs -q -t ext4 -L vortex -O ^metadata_csum,^64bit -m 1 -d "$ROOT" "$T/rootfs.ext4" "${SIZE}M"
img2simg "$T/rootfs.ext4" "$DIST/vortex-rootfs.simg"
cp "$T/rootfs.ext4" "$DIST/userdata.img"

msg "SD card image"
truncate -s $((SIZE + 2))M "$DIST/vortex-sdcard.img"
printf 'label: dos\nstart=1MiB, type=83\n' | sfdisk -q "$DIST/vortex-sdcard.img"
dd if="$T/rootfs.ext4" of="$DIST/vortex-sdcard.img" bs=1M seek=1 conv=notrunc status=none

cp "$TOP/docs/flashing.md" "$DIST/FLASHING.md"
(cd "$DIST" && sha256sum boot-partition.img userdata.img vortex-sdcard.img lk2nd.img vortex-boot.img vortex-rootfs.simg > SHA256SUMS)
ls -la "$DIST"
msg "done: $DIST"
