#!/bin/sh
# Build the Vortex root filesystem for the Nokia 6300 4G (armv7l-musl).
#
#   sudo rootfs/build.sh [-o OUTDIR] [-m MIRROR]
#
# How it works without an ARM machine or emulator:
#   - Void armv7 packages are *unpacked only* (xbps-install -U); nothing ARM
#     runs at build time.  The phone runs xbps-reconfigure on first boot
#     (core-services/00-vortex-firstboot.sh).
#   - The kernel and modules are postmarketOS' prebuilt
#     linux-postmarketos-qcom-msm8916 (covers MSM8909), unpacked from the apk.
#   - vortex-shell is cross-compiled with clang against a Void musl sysroot.
#   - The panel init firmware is compiled with tools/phone/mipi-dbi-fw.py.
#
# Needs (host): root, curl, tar, clang + lld, make, python3.
# Output: OUTDIR/rootfs/ and OUTDIR/kernel/ (zImage, dtbs, modules).
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$HERE/.." && pwd)
OUT=$TOP/out/phone
MIRROR=${MIRROR:-https://repo-default.voidlinux.org}
while getopts o:m: opt; do
	case $opt in
	o) OUT=$OPTARG ;;
	m) MIRROR=$OPTARG ;;
	*) exit 2 ;;
	esac
done
ROOT=$OUT/rootfs
SYS=$OUT/sysroot
K=$OUT/kernel
export XBPS_ARCH=armv7l-musl

# Pinned inputs
PMOS=https://mirror.postmarketos.org/postmarketos/master/armv7
KPKG=linux-postmarketos-qcom-msm8916-6.12.1-r7
KVER=6.12.1-msm8916
LK2ND=lk2nd-msm8916-23.1-r0
GPUFW=firmware-qcom-adreno-a300-20260110-r1
PMAPORTS=690ccca91f4b5d60bd9e306f8449cd80e3daf682
PANEL=nokia,leo-gc9305-hlt-24-panel
DTB=qcom-msm8909-nokia-leo.dtb

msg() { printf '==> %s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }
[ "$(id -u)" = 0 ] || die "run as root (file ownership in the image)"
mkdir -p "$OUT"

# ------------------------------------------------------------------ xbps
XI=$OUT/xbps/usr/bin/xbps-install.static
if [ ! -x "$XI" ]; then
	msg "fetching static xbps"
	mkdir -p "$OUT/xbps"
	curl -fsSL "$MIRROR/static/xbps-static-latest.x86_64-musl.tar.xz" | tar -xJ -C "$OUT/xbps"
fi
xbps_unpack() { # ROOTDIR PKG...
	r=$1; shift
	mkdir -p "$r"
	# keep downloads across rebuilds
	yes | "$XI" -S -U -r "$r" -c "$OUT/xbps-cache" -R "$MIRROR/current/musl" "$@" \
		>"$OUT/xbps-$(basename "$r").log" 2>&1 ||
		grep -q "already installed" "$OUT/xbps-$(basename "$r").log" ||
		{ tail -20 "$OUT/xbps-$(basename "$r").log"; die "xbps failed"; }
}

msg "unpacking Void armv7l-musl packages (not configured until first boot)"
rm -rf "$ROOT"
# shellcheck disable=SC2046 # the package list is meant to be split
xbps_unpack "$ROOT" $(sed 's/#.*//' "$HERE/packages.txt")
[ -e "$ROOT/usr/bin/sh" ] || ln -s dash "$ROOT/usr/bin/sh"

# ---------------------------------------------------------------- kernel
msg "fetching postmarketOS kernel, lk2nd and GPU firmware"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
for p in $KPKG $LK2ND $GPUFW; do
	curl -fsSL -o "$T/$p.apk" "$PMOS/$p.apk"
	mkdir -p "$T/$p"
	tar -xzf "$T/$p.apk" -C "$T/$p" --ignore-zeros 2>/dev/null || true
done
rm -rf "$K"
mkdir -p "$K/dtbs"
cp "$T/$KPKG/boot/vmlinuz" "$K/zImage"
cp "$T/$KPKG/boot/dtbs/$DTB" "$K/dtbs/"
cp "$T/$KPKG/boot/config" "$K/config"
echo "$KVER" > "$K/kernel.release"
cp "$T/$LK2ND/boot/lk2nd.img" "$K/lk2nd.img"
mkdir -p "$ROOT/usr/lib/modules"
cp -a "$T/$KPKG/usr/lib/modules/$KVER" "$ROOT/usr/lib/modules/"
cp -a "$T/$GPUFW/usr/lib/firmware/." "$ROOT/usr/lib/firmware/" 2>/dev/null ||
	cp -a "$T/$GPUFW/lib/firmware/." "$ROOT/usr/lib/firmware/" 2>/dev/null || true

msg "panel init sequence"
curl -fsSL -o "$T/panel.txt" \
	"https://gitlab.postmarketos.org/postmarketOS/pmaports/-/raw/$PMAPORTS/device/archived/device-nokia-leo/$PANEL.txt"
mkdir -p "$ROOT/usr/lib/firmware"
python3 "$TOP/tools/phone/mipi-dbi-fw.py" "$ROOT/usr/lib/firmware/$PANEL.bin" "$T/panel.txt"

# ---------------------------------------------------------------- vortex
msg "cross-compiling vortex-shell (armv7, static)"
xbps_unpack "$SYS" musl-devel kernel-libc-headers gcc busybox-static
CC="clang --target=armv7l-linux-musleabihf --sysroot=$SYS -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard"
make -s -C "$TOP/shell" clean
make -s -C "$TOP/shell" CC="$CC" LDFLAGS="-fuse-ld=lld -static -s"
install -Dm755 "$TOP/shell/vortex-shell" "$ROOT/usr/bin/vortex-shell"
make -s -C "$TOP/shell" clean

for f in vortex-kai-install vortex-kai-remove vortex-kai-run vortex-call vortex-webview vortex-webview-cage; do
	install -Dm755 "$TOP/runtime/$f" "$ROOT/usr/libexec/vortex/$f"
done
install -Dm755 "$TOP/runtime/vortex-wifi" "$ROOT/usr/bin/vortex-wifi"
install -Dm755 "$TOP/runtime/vortex-power" "$ROOT/usr/bin/vortex-power"
install -Dm644 "$TOP/runtime/kaios-shim.js" "$ROOT/usr/share/vortex/kaios-shim.js"
install -Dm644 "$TOP/assets/fonts/Roboto-Regular.ttf" "$ROOT/usr/share/vortex/fonts/Roboto-Regular.ttf"
cp -a "$HERE/overlay/." "$ROOT/"
chmod 600 "$ROOT/etc/doas.conf"
cat >> "$ROOT/etc/vortex/runtime.conf" <<'EOF'

# --- Void ships WebKitGTK (not WPE) for armv7: run it in Cage ---
VORTEX_WEB_ENGINE=/usr/libexec/vortex/vortex-webview-cage
EOF

# ---------------------------------------------------------------- system
msg "users, services, first-boot setup"
# Packages are not configured yet, so add the shell's user by hand.  The
# groups video/input/audio come with base-files.
grep -q '^vortex:' "$ROOT/etc/group" || echo 'vortex:x:990:' >> "$ROOT/etc/group"
grep -q '^vortex:' "$ROOT/etc/passwd" ||
	echo 'vortex:x:990:990:Vortex shell:/var/lib/vortex:/sbin/nologin' >> "$ROOT/etc/passwd"
grep -q '^vortex:' "$ROOT/etc/shadow" 2>/dev/null || echo 'vortex:!:19000::::::' >> "$ROOT/etc/shadow"
for g in video input audio; do
	sed -i "s/^\($g:[^:]*:[0-9]*:\)\(.*\)$/\1\2,vortex/; s/^\($g:[^:]*:[0-9]*:\),vortex$/\1vortex/" "$ROOT/etc/group"
done
mkdir -p "$ROOT/var/lib/vortex/apps" "$ROOT/var/lib/vortex/data" "$ROOT/var/cache/vortex/web"
chown -R 990:990 "$ROOT/var/lib/vortex" "$ROOT/var/cache/vortex"
touch "$ROOT/etc/vortex/.firstboot"

echo vortex > "$ROOT/etc/hostname"
cat >> "$ROOT/etc/rc.conf" <<'EOF'

# Vortex
TIMEZONE="UTC"
# keep udevd running after boot (costs ~1.5 MB); see core-services/90-vortex.sh
VORTEX_UDEV_RESIDENT=0
EOF
cat > "$ROOT/etc/fstab" <<'EOF'
# The initramfs mounts LABEL=vortex as /.
LABEL=vortex    /      ext4    rw,noatime,commit=60         0      1
tmpfs           /tmp   tmpfs   nosuid,nodev,size=32m        0      0
EOF

SV=$ROOT/etc/runit/runsvdir/default
mkdir -p "$SV"
rm -f "$SV"/agetty-*
for s in vortex-shell seatd wpa_supplicant dhcpcd rmtfs qrtr-ns pd-mapper tqftpserv usb-shell; do
	[ -d "$ROOT/etc/sv/$s" ] && ln -sf "/etc/sv/$s" "$SV/$s"
done
# Wi-Fi starts off (vortex-wifi on brings it up): saves ~4 MB and battery.
touch "$ROOT/etc/sv/wpa_supplicant/down" "$ROOT/etc/sv/dhcpcd/down"
# Serial console (ttyMSM0, 115200) for development.
if [ "${VORTEX_SERIAL_GETTY:-0}" = 1 ]; then
	ln -sf /etc/sv/agetty-ttyMSM0 "$SV/" 2>/dev/null ||
		{ cp -a "$ROOT/etc/sv/agetty-ttyS0" "$ROOT/etc/sv/agetty-ttyMSM0" && ln -sf /etc/sv/agetty-ttyMSM0 "$SV/"; }
fi

# Weight loss: docs, headers, static libs, locales.
rm -rf "$ROOT/usr/share/man" "$ROOT/usr/share/doc" "$ROOT/usr/share/info" \
	"$ROOT/usr/share/gtk-doc" "$ROOT/usr/include"
find "$ROOT/usr/lib" -name '*.a' -delete
find "$ROOT/usr/share/locale" -mindepth 1 -maxdepth 1 ! -name 'en*' -exec rm -rf {} + 2>/dev/null || true
rm -rf "$ROOT/var/cache/xbps"
du -sh "$ROOT"
msg "done: $ROOT (next: rootfs/mkimage.sh)"
