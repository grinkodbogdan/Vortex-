#!/bin/sh
# Build an x86_64 Vortex VM image for testing the shell and KaiOS apps
# without a phone.
#
#   sudo tools/vm/build.sh [WORKDIR]        (default: out/vm)
#
# Same Vortex shell, runtime, configs and services as the phone, with:
#   - Void x86_64-musl instead of armv7l-musl
#   - WebKitGTK in the Cage kiosk compositor instead of WPE/Cog
#     (runtime/vortex-webview-cage), because Void does not ship WPE
#   - a 240x320 bochs framebuffer, so screens look exactly like the phone
#   - a root shell on the serial port for debugging
#
# Needs: root, curl, a C compiler (static linking), mke2fs, tar/xz.
# Env: VORTEX_VM_PROXY=http://10.0.2.2:PORT routes the guest through a proxy
#      on the host; VORTEX_VM_CA=FILE adds a CA certificate (for TLS-inspecting
#      proxies).
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$HERE/../.." && pwd)
WORK=${1:-$TOP/out/vm}
ROOT=$WORK/root
IMG=$WORK/vortex-vm.img
MIRROR=${MIRROR:-https://repo-default.voidlinux.org}
export XBPS_ARCH=x86_64-musl

msg() { printf '==> %s\n' "$*"; }
[ "$(id -u)" = 0 ] || { echo "run as root" >&2; exit 1; }
mkdir -p "$WORK" "$ROOT"

# --------------------------------------------------------------- packages
XI=$WORK/xbps/usr/bin/xbps-install.static
if [ ! -x "$XI" ]; then
	msg "fetching static xbps"
	mkdir -p "$WORK/xbps"
	curl -fsSL "$MIRROR/static/xbps-static-latest.x86_64-musl.tar.xz" | tar -xJ -C "$WORK/xbps"
fi
msg "installing Void packages"
yes | "$XI" -Sy -r "$ROOT" -R "$MIRROR/current/musl" \
	base-files runit-void coreutils findutils diffutils grep sed gawk gzip tar util-linux \
	kmod eudev shadow opendoas iproute2 dhcpcd ca-certificates curl unzip linux \
	libwebkit2gtk41 cage seatd python3-gobject mesa-dri dejavu-fonts-ttf \
	gst-plugins-base1 gst-plugins-good1 gst-plugins-bad1 gst-libav xkeyboard-config \
	>"$WORK/xbps.log" 2>&1 || grep -q "already installed" "$WORK/xbps.log" ||
	{ cat "$WORK/xbps.log"; exit 1; }

# ----------------------------------------------------------------- vortex
msg "building vortex-shell (static)"
make -s -C "$TOP/shell" clean
make -s -C "$TOP/shell" LDFLAGS="-static -s"
install -Dm755 "$TOP/shell/vortex-shell" "$ROOT/usr/bin/vortex-shell"
make -s -C "$TOP/shell" clean

for f in vortex-kai-install vortex-kai-remove vortex-kai-run vortex-call vortex-webview vortex-webview-cage; do
	install -Dm755 "$TOP/runtime/$f" "$ROOT/usr/libexec/vortex/$f"
done
install -Dm755 "$TOP/runtime/vortex-wifi" "$ROOT/usr/bin/vortex-wifi"
install -Dm755 "$TOP/runtime/vortex-power" "$ROOT/usr/bin/vortex-power"
install -Dm644 "$TOP/runtime/kaios-shim.js" "$ROOT/usr/share/vortex/kaios-shim.js"
install -Dm644 "$TOP/assets/fonts/Roboto-Regular.ttf" "$ROOT/usr/share/vortex/fonts/Roboto-Regular.ttf"
cp -a "$TOP/rootfs/overlay/." "$ROOT/"
rm -f "$ROOT/etc/runit/core-services/01-vortex-firmware.sh"   # phone only
chmod 600 "$ROOT/etc/doas.conf"

# VM profile
cat >> "$ROOT/etc/vortex/runtime.conf" <<'EOF'

# --- VM profile: WebKitGTK in Cage instead of WPE/Cog ---
VORTEX_WEB_ENGINE=/usr/libexec/vortex/vortex-webview-cage
EOF
sed -i 's/^screen_timeout = .*/screen_timeout = 0/' "$ROOT/etc/vortex/vortex.conf"
: > "$ROOT/etc/vortex/env"
if [ -n "${VORTEX_VM_PROXY:-}" ]; then
	cat > "$ROOT/etc/vortex/env" <<EOF
export http_proxy=$VORTEX_VM_PROXY https_proxy=$VORTEX_VM_PROXY
export HTTP_PROXY=$VORTEX_VM_PROXY HTTPS_PROXY=$VORTEX_VM_PROXY
export VORTEX_PROXY=$VORTEX_VM_PROXY
EOF
	cp "$ROOT/etc/vortex/env" "$ROOT/etc/profile.d/vortex-proxy.sh"
fi
if [ -n "${VORTEX_VM_CA:-}" ]; then
	cat "$VORTEX_VM_CA" >> "$ROOT/etc/ssl/certs/ca-certificates.crt"
fi

msg "users and services"
chroot "$ROOT" sh -c '
	set -e
	getent group vortex >/dev/null || groupadd -r vortex
	id vortex >/dev/null 2>&1 || useradd -r -g vortex -G video,input,audio,_seatd \
		-d /var/lib/vortex -s /sbin/nologin vortex
	mkdir -p /var/lib/vortex/apps /var/lib/vortex/data /var/cache/vortex/web
	chmod 755 /var/lib/vortex
	chown -R vortex:vortex /var/lib/vortex /var/cache/vortex
	passwd -d root >/dev/null
'
echo vortex-vm > "$ROOT/etc/hostname"
echo '/dev/vda / ext4 rw,noatime 0 1' > "$ROOT/etc/fstab"
SV=$ROOT/etc/runit/runsvdir/default
rm -f "$SV"/agetty-*
for s in vortex-shell seatd dhcpcd agetty-ttyS0; do ln -sf "/etc/sv/$s" "$SV/$s"; done
# debug: root shell on the serial console
printf 'GETTY_ARGS="--autologin root --noclear"\nBAUD_RATE=115200\nTERM_NAME=vt100\n' \
	> "$ROOT/etc/sv/agetty-ttyS0/conf"

# ------------------------------------------------------------------ image
msg "making disk image"
K=$(ls "$ROOT/usr/lib/modules" | head -n 1)
cp "$ROOT/boot/vmlinuz-$K" "$WORK/vmlinuz"
cp "$ROOT/boot/initramfs-$K.img" "$WORK/initramfs.img"
rm -f "$IMG"
mke2fs -q -t ext4 -L vortex -d "$ROOT" "$IMG" 6G
msg "done: $IMG (run tools/vm/run.sh)"
