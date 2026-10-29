# Vortex: expose the phone's own Qualcomm firmware (Wi-Fi, modem, video)
# before udev coldplug (02-udev.sh) probes the remote processors.
#
# The blobs live on the stock "modem"/"apnhlos" (vfat) partitions, the Wi-Fi
# calibration (WCNSS_qcom_wlan_nv.bin) on "persist" when the phone has one.
# udev has not run yet, so find the partitions through sysfs.  The kernel is
# booted with firmware_class.path=/run/vortex/firmware, which is searched
# before /usr/lib/firmware.  Modelled on postmarketOS' msm-firmware-loader.
#
# Without a calibration file of the phone's own, wcnss_ctrl falls back to
# /usr/lib/firmware/wlan/prima/WCNSS_qcom_wlan_nv.bin: the Nokia 8000 4G
# file that postmarketOS also uses for the 6300 4G (rootfs/build.sh).

vortex_partdev() {
	for d in /sys/class/block/*; do
		grep -qx "PARTNAME=$1" "$d/uevent" 2>/dev/null && { echo "/dev/${d##*/}"; return 0; }
	done
	return 1
}

FW=/run/vortex/firmware
mkdir -p $FW
for part in modem apnhlos persist; do
	dev=$(vortex_partdev $part) || continue
	mkdir -p /run/vortex/$part
	mount -o ro,nodev,noexec,nosuid "$dev" /run/vortex/$part 2>/dev/null || continue
done
for f in /run/vortex/modem/image/* /run/vortex/apnhlos/image/*; do
	[ -e "$f" ] && [ ! -e "$FW/${f##*/}" ] && ln -s "$f" "$FW/"
done
# some kernels ask for .mbn rather than the split .mdt
for f in "$FW"/*.mdt; do
	[ -e "$f" ] && [ ! -e "${f%.mdt}.mbn" ] && ln -s "$f" "${f%.mdt}.mbn"
done
for f in /run/vortex/persist/WCNSS_qcom_wlan_nv.bin /run/vortex/persist/wlan/prima/WCNSS_qcom_wlan_nv.bin; do
	if [ -e "$f" ]; then
		mkdir -p $FW/wlan/prima
		ln -sf "$f" $FW/wlan/prima/WCNSS_qcom_wlan_nv.bin
		break
	fi
done
{
	echo "firmware: $(ls $FW | tr '\n' ' ')"
	[ -e $FW/wcnss.mdt ] || echo "firmware: no wcnss.mdt on modem/apnhlos: Wi-Fi will not start"
} > /dev/kmsg 2>/dev/null
unset -f vortex_partdev
unset FW
