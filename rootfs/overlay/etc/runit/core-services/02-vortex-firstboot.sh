# Vortex first boot: the image was assembled on a PC by unpacking Void's
# armv7 packages without running their setup scripts.  Run them now, once.
# Progress shows under the boot logo (vortex-splash reads the message file).
if [ -e /etc/vortex/.firstboot ]; then
	mount -o remount,rw / 2>/dev/null
	say() { echo "Vortex: $*"; echo "Vortex: $*" > /dev/kmsg 2>/dev/null; }
	echo "Setting up Vortex (first boot only). This takes a few minutes..." > /dev/.vortex-splash.msg 2>/dev/null
	say "first-boot setup, this takes a few minutes..."
	xbps-reconfigure -fa > /var/log/vortex-firstboot.log 2>&1
	say "packages configured (exit $?)"
	# groups created by package scripts (seatd) for the shell's web apps
	usermod -aG _seatd vortex >> /var/log/vortex-firstboot.log 2>&1
	depmod -a >> /var/log/vortex-firstboot.log 2>&1
	# keypad remapping (etc/udev/hwdb.d): apply it now, not only next boot
	udevadm hwdb --update >> /var/log/vortex-firstboot.log 2>&1
	udevadm trigger --subsystem-match=input --action=change
	udevadm settle -t 5
	rm -f /etc/vortex/.firstboot
	sync
	say "first-boot setup done"
	: > /dev/.vortex-splash.msg 2>/dev/null
fi
