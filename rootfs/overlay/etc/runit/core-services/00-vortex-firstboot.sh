# Vortex first boot: the image was assembled on a PC by unpacking Void's
# armv7 packages without running their setup scripts.  Run them now, once.
if [ -e /etc/vortex/.firstboot ]; then
	msg "Vortex: first boot, configuring packages (a few minutes)..."
	echo "Vortex: first-boot setup, please wait..." > /dev/tty0 2>/dev/null
	mount -o remount,rw / 2>/dev/null
	xbps-reconfigure -fa > /var/log/vortex-firstboot.log 2>&1
	# groups created by package scripts (seatd) for the shell's web apps
	usermod -aG _seatd vortex >> /var/log/vortex-firstboot.log 2>&1
	depmod -a >> /var/log/vortex-firstboot.log 2>&1
	rm -f /etc/vortex/.firstboot
	sync
fi
