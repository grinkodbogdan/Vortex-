# Vortex: expose the phone's own Qualcomm firmware (Wi-Fi, modem, video)
# before udev coldplug (02-udev.sh) probes the remote processors.
#
# The blobs live on the stock "modem" (vfat) and "persist" partitions.  udev
# has not run yet, so find the partitions through sysfs.  The kernel is
# booted with firmware_class.path=/run/vortex/firmware.

vortex_partdev() {
	for d in /sys/class/block/*; do
		grep -qx "PARTNAME=$1" "$d/uevent" 2>/dev/null && { echo "/dev/${d##*/}"; return 0; }
	done
	return 1
}

mkdir -p /run/vortex/firmware
for part in modem persist; do
	dev=$(vortex_partdev $part) || continue
	mkdir -p /run/vortex/$part
	mount -o ro "$dev" /run/vortex/$part 2>/dev/null || continue
done
for f in /run/vortex/modem/image/* /run/vortex/persist/WCNSS_qcom_wlan_nv.bin; do
	[ -e "$f" ] && ln -sf "$f" /run/vortex/firmware/
done
if [ -e /run/vortex/firmware/WCNSS_qcom_wlan_nv.bin ]; then
	mkdir -p /run/vortex/firmware/wlan/prima
	ln -sf /run/vortex/firmware/WCNSS_qcom_wlan_nv.bin /run/vortex/firmware/wlan/prima/
fi
unset -f vortex_partdev
