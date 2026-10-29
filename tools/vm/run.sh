#!/bin/sh
# Boot the Vortex VM built by tools/vm/build.sh.
#
#   tools/vm/run.sh [WORKDIR]            window (if a display is available)
#   VORTEX_VM_HEADLESS=1 tools/vm/run.sh   no window; control it through
#                                          WORKDIR/monitor.sock (QEMU monitor:
#                                          screendump, sendkey) and the serial
#                                          root shell on WORKDIR/serial.sock
#
# Keys (PC keyboard -> phone): arrows, Enter = OK, F1/F2 = soft keys,
# Esc = back, F3 = call, F4 = end (hold for the power menu), digits = keypad.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=${1:-$HERE/../../out/vm}
ACCEL=tcg
[ -w /dev/kvm ] && ACCEL=kvm

set -- -accel "$ACCEL" -m 2048 -smp 4 \
	-kernel "$WORK/vmlinuz" -initrd "$WORK/initramfs.img" \
	-append "root=/dev/vda rw console=ttyS0,115200 quiet loglevel=3 vt.global_cursor_default=0" \
	-drive "file=$WORK/vortex-vm.img,format=raw,if=virtio" \
	-netdev user,id=n0 -device virtio-net-pci,netdev=n0 \
	-vga none -device bochs-display,xres=240,yres=320 \
	-device virtio-rng-pci

if [ "${VORTEX_VM_HEADLESS:-0}" = 1 ]; then
	exec qemu-system-x86_64 "$@" -display none \
		-monitor "unix:$WORK/monitor.sock,server,nowait" \
		-serial "unix:$WORK/serial.sock,server,nowait"
else
	exec qemu-system-x86_64 "$@" -serial stdio
fi
