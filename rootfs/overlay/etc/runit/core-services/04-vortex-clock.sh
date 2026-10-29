# Vortex: the phone boots with its clock at 1970.  Until the network sets it
# (vortex-timesync), go forward to the last saved time, or at least to when
# the image was built.
for f in /var/lib/vortex/clock /etc/vortex/build-time; do
	[ -e "$f" ] || continue
	t=$(stat -c %Y "$f" 2>/dev/null) || continue
	[ "$t" -gt "$(date +%s)" ] && date -s "@$t" >/dev/null 2>&1
done
unset f t
