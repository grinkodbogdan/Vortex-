# Vortex boot-time setup (sourced by runit stage 1 after Void's core services)

# Compressed swap in RAM: the web engine is the only big consumer and it
# compresses well.  Size it to ~40% of RAM.
if modprobe -q zram 2>/dev/null || [ -e /sys/block/zram0 ]; then
	mem_kb=$(awk '/MemTotal/ {print $2}' /proc/meminfo)
	echo lz4 > /sys/block/zram0/comp_algorithm 2>/dev/null || true
	echo $((mem_kb * 2 / 5))K > /sys/block/zram0/disksize
	mkswap /dev/zram0 >/dev/null && swapon -p 100 /dev/zram0
fi

# udev is only needed for coldplug: Void's 02-udev.sh already triggered and
# settled every device.  The phone has no hotplug worth 1.5 MB of RAM, so
# stop the daemon (its database in /run/udev stays for libinput).
# Set VORTEX_UDEV_RESIDENT=1 in /etc/rc.conf to keep it.
if [ "${VORTEX_UDEV_RESIDENT:-0}" != 1 ] && command -v udevadm >/dev/null; then
	udevadm control --exit 2>/dev/null || true
fi

# Battery life: scale CPU frequency with load.
for g in /sys/devices/system/cpu/cpufreq/policy*/scaling_governor; do
	[ -w "$g" ] && echo schedutil > "$g" 2>/dev/null
done
