# Vortex first boot: the image was assembled on a PC by unpacking Void's
# armv7 packages without running their setup scripts.  Run them now, once.
# This runs after 02-udev.sh, so the panel driver is loaded and the
# progress below is visible on the screen (console=tty0).
if [ -e /etc/vortex/.firstboot ]; then
	mount -o remount,rw / 2>/dev/null
	say() { echo "Vortex: $*"; echo "Vortex: $*" > /dev/tty0 2>/dev/null; echo "Vortex: $*" > /dev/kmsg 2>/dev/null; }
	say "first-boot setup, this takes a few minutes..."
	xbps-reconfigure -fa > /var/log/vortex-firstboot.log 2>&1
	say "packages configured (exit $?)"
	# groups created by package scripts (seatd) for the shell's web apps
	usermod -aG _seatd vortex >> /var/log/vortex-firstboot.log 2>&1
	depmod -a >> /var/log/vortex-firstboot.log 2>&1
	rm -f /etc/vortex/.firstboot
	sync
	say "first-boot setup done"
fi
