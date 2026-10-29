#!/bin/sh
# End-to-end test in the VM: boot Vortex, open the Banana Store, search
# with T9, install the first hit, open it, close it with End, and check
# that nothing crashed.  Screenshots go to WORKDIR/e2e/.
#
#   tools/vm/e2e-store-app.sh "2 2 wait:1500 2 wait:1500 6 6 wait:1500 5 5"   # "bank" -> BankiTube
#
# APP_WAIT (s) is how long to give the app to start (TCG is slow).
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=${WORK:-$HERE/../../out/vm}
T9=${1:?usage: e2e-store-app.sh "T9 KEYS"}
APP_WAIT=${APP_WAIT:-100}
CTL="$HERE/ctl.py $WORK"
OUT=$WORK/e2e
rm -rf "$OUT"
mkdir -p "$OUT"

pgrep -f "qemu-system-x86_64.*$WORK/vortex-vm.img" >/dev/null && { echo "VM already running" >&2; exit 1; }
VORTEX_VM_HEADLESS=1 "$HERE/run.sh" "$WORK" >"$OUT/qemu.log" 2>&1 &
sleep 3

step=0
shot() { step=$((step + 1)); $CTL shot "$OUT/$(printf %02d $step)-$1.png"; echo "shot $step $1"; }

echo "waiting for the shell"
i=0
until [ $i -ge 60 ]; do
	$CTL shot "$OUT/boot.ppm" 2>/dev/null || true
	[ "$(convert "$OUT/boot.ppm" -format %k info: 2>/dev/null || echo 0)" -gt 50 ] && break
	sleep 10
	i=$((i + 1))
done
rm -f "$OUT/boot.ppm"
shot home
$CTL keys f1 wait:20000;                         shot store
# shellcheck disable=SC2086 # T9 is a key list
$CTL keys ret wait:1500 $T9 wait:1500;           shot search
$CTL keys ret wait:1500 ret wait:1500;           shot detail
$CTL keys ret wait:2000;                         shot installing
sleep 25;                                        shot installed
$CTL keys f1; sleep "$APP_WAIT";                 shot app
$CTL keys f4 wait:8000;                          shot after-end

echo "checking for crashes"
VX_TIMEOUT=30 $CTL sh 'dmesg | grep -ci segfault; for p in /proc/[0-9]*; do tr "\0" " " < $p/cmdline 2>/dev/null | grep -q "^cage" && echo leftover-cage; done; echo end' >"$OUT/check.txt"
cat "$OUT/check.txt"
python3 - "$WORK" <<'EOF'
import socket, sys, time
s = socket.socket(socket.AF_UNIX); s.connect(sys.argv[1] + "/monitor.sock")
time.sleep(0.2); s.sendall(b"quit\n"); time.sleep(1)
EOF
montage "$OUT"/[0-9]*.png -tile 9x1 -geometry +4+4 -background '#555' "$OUT/all.png"
echo "screenshots: $OUT/all.png"
