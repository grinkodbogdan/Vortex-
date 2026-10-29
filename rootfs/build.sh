#!/bin/sh
# Build the Vortex root filesystem for the Nokia 6300 4G (armv7l-musl).
#
#   sudo rootfs/build.sh [OPTIONS]
#
#   -o DIR     output directory (default: out/)
#   -r REPO    extra xbps repository, e.g. void-packages/hostdir/binpkgs
#              holding the wpewebkit and cog packages (repeatable)
#   -k DIR     kernel build dir from kernel/build.sh (default: out/kernel)
#   -m MIRROR  Void mirror (default: https://repo-default.voidlinux.org)
#
# Requirements (host): xbps (or xbps-static), qemu-arm user emulation with
# binfmt_misc, an armv7l-linux-musleabihf cross compiler, python3, curl.
# On Void: xbps-install xbps qemu-user-static binfmt-support
#          cross-armv7l-linux-musleabihf python3 curl
#
# Output: DIR/rootfs/ (the tree) and DIR/vortex-rootfs.tar.gz.
# Disk images are made by rootfs/mkimage.sh.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$HERE/.." && pwd)
OUT=$TOP/out
KDIR=
MIRROR=https://repo-default.voidlinux.org
REPOS=
ARCH=armv7l-musl
CROSS=${CROSS:-armv7l-linux-musleabihf-}

# Pinned postmarketOS revision for the panel init sequence.
PMAPORTS=690ccca91f4b5d60bd9e306f8449cd80e3daf682
PANEL=nokia,leo-gc9305-hlt-24-panel
MIPI_DBI_CMD=1cbd40135a8c7f25d7b444a7fac77fd3c3ad471e

while getopts o:r:k:m: opt; do
	case $opt in
	o) OUT=$OPTARG ;;
	r) REPOS="$REPOS -R $OPTARG" ;;
	k) KDIR=$OPTARG ;;
	m) MIRROR=$OPTARG ;;
	*) exit 2 ;;
	esac
done
KDIR=${KDIR:-$OUT/kernel}
ROOT=$OUT/rootfs

msg() { printf '==> %s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

[ "$(id -u)" = 0 ] || die "run as root (file ownership in the image)"
command -v xbps-install >/dev/null || command -v xbps-install.static >/dev/null || die "xbps not found"
XI=$(command -v xbps-install || command -v xbps-install.static)
QEMU=$(command -v qemu-arm-static || command -v qemu-arm || true)
[ -n "$QEMU" ] || die "qemu-arm user emulation not found"

mkdir -p "$OUT"
rm -rf "$ROOT"
mkdir -p "$ROOT/var/db/xbps/keys"
cp -a /var/db/xbps/keys/. "$ROOT/var/db/xbps/keys/" 2>/dev/null || true

# ---------------------------------------------------------------- packages
PKGS=$(sed 's/#.*//' "$HERE/packages.txt" | tr -s ' \n' ' ')
msg "installing packages for $ARCH"
# shellcheck disable=SC2086 # word lists on purpose
XBPS_ARCH=$ARCH "$XI" -S -y -r "$ROOT" \
	-R "$MIRROR/current/musl" $REPOS $PKGS

msg "configuring packages under qemu"
cp "$QEMU" "$ROOT/usr/bin/"
chroot "$ROOT" /usr/bin/xbps-reconfigure -fa

# ------------------------------------------------------------------ vortex
msg "building vortex-shell (static)"
make -C "$TOP/shell" clean >/dev/null
make -C "$TOP/shell" CC="${CROSS}gcc" CFLAGS="-Os -pipe" LDFLAGS="-static -s"
install -Dm755 "$TOP/shell/vortex-shell" "$ROOT/usr/bin/vortex-shell"
make -C "$TOP/shell" clean >/dev/null

for f in vortex-kai-install vortex-kai-remove vortex-kai-run vortex-call; do
	install -Dm755 "$TOP/runtime/$f" "$ROOT/usr/libexec/vortex/$f"
done
install -Dm755 "$TOP/runtime/vortex-wifi" "$ROOT/usr/bin/vortex-wifi"
install -Dm755 "$TOP/runtime/vortex-power" "$ROOT/usr/bin/vortex-power"
install -Dm644 "$TOP/runtime/kaios-shim.js" "$ROOT/usr/share/vortex/kaios-shim.js"
install -Dm644 "$TOP/assets/fonts/Roboto-Regular.ttf" "$ROOT/usr/share/vortex/fonts/Roboto-Regular.ttf"
cp -a "$HERE/overlay/." "$ROOT/"
chmod 600 "$ROOT/etc/doas.conf"

# ------------------------------------------------------------ device bits
msg "generating the panel init sequence"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
curl -fsSL -o "$T/panel.txt" \
	"https://gitlab.postmarketos.org/postmarketOS/pmaports/-/raw/$PMAPORTS/device/archived/device-nokia-leo/$PANEL.txt"
curl -fsSL -o "$T/mipi-dbi-cmd" \
	"https://raw.githubusercontent.com/notro/panel-mipi-dbi/$MIPI_DBI_CMD/mipi-dbi-cmd"
python3 "$T/mipi-dbi-cmd" "$ROOT/usr/lib/firmware/$PANEL.bin" "$T/panel.txt"

if [ -d "$KDIR/modules/lib/modules" ]; then
	msg "installing kernel modules from $KDIR"
	mkdir -p "$ROOT/usr/lib/modules"
	cp -a "$KDIR/modules/lib/modules/." "$ROOT/usr/lib/modules/"
	chroot "$ROOT" depmod -a "$(cat "$KDIR/kernel.release")"
else
	msg "no kernel modules in $KDIR (run kernel/build.sh first); continuing"
fi

# ---------------------------------------------------------------- system
msg "users, services, housekeeping"
chroot "$ROOT" sh -c '
	set -e
	getent group vortex >/dev/null || groupadd -r vortex
	id vortex >/dev/null 2>&1 || useradd -r -g vortex -G video,input,audio \
		-d /var/lib/vortex -s /sbin/nologin vortex
	install -d -o vortex -g vortex /var/lib/vortex /var/lib/vortex/apps \
		/var/lib/vortex/data /var/cache/vortex
	passwd -l root >/dev/null
'
echo vortex > "$ROOT/etc/hostname"
cat >> "$ROOT/etc/rc.conf" <<'EOF'

# Vortex
TIMEZONE="UTC"
# keep udevd running after boot (costs ~1.5 MB); see core-services/90-vortex.sh
VORTEX_UDEV_RESIDENT=0
EOF
cat > "$ROOT/etc/fstab" <<'EOF'
# <device>      <dir>  <type>  <options>                    <dump> <pass>
LABEL=vortex    /      ext4    rw,noatime,commit=60         0      1
LABEL=VTXBOOT   /boot  ext2    rw,noatime,noauto            0      2
tmpfs           /tmp   tmpfs   nosuid,nodev,size=32m        0      0
EOF

SVDIR=$ROOT/etc/runit/runsvdir/default
rm -f "$SVDIR"/agetty-*
for sv in vortex-shell wpa_supplicant dhcpcd; do
	ln -sf "/etc/sv/$sv" "$SVDIR/$sv"
done
# Wi-Fi starts off (vortex-wifi on brings it up): saves ~4 MB and battery.
touch "$ROOT/etc/sv/wpa_supplicant/down" "$ROOT/etc/sv/dhcpcd/down"
# Serial console for development only.
[ "${VORTEX_SERIAL_GETTY:-0}" = 1 ] && ln -sf /etc/sv/agetty-ttyMSM0 "$SVDIR/"

# Weight loss: docs, headers, static libs, locales.
rm -rf "$ROOT/usr/share/man" "$ROOT/usr/share/doc" "$ROOT/usr/share/info" \
	"$ROOT/usr/share/gtk-doc" "$ROOT/usr/include"
find "$ROOT/usr/lib" -name '*.a' -delete
find "$ROOT/usr/share/locale" -mindepth 1 -maxdepth 1 ! -name 'en*' -exec rm -rf {} + 2>/dev/null || true
rm -f "$ROOT/usr/bin/$(basename "$QEMU")"
rm -rf "$ROOT/var/cache/xbps"

msg "packing"
tar -C "$ROOT" -czf "$OUT/vortex-rootfs.tar.gz" .
du -sh "$ROOT"
msg "done: $OUT/vortex-rootfs.tar.gz"
