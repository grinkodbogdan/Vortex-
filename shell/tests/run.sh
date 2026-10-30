#!/bin/sh
# Headless smoke tests: drive vortex-shell with key scripts and keep
# screenshots in tests/out for review.
set -eu
cd "$(dirname "$0")/.."
OUT=tests/out
rm -rf "$OUT"
mkdir -p "$OUT/apps" "$OUT/cache"
cp -r tests/fixtures/apps/. "$OUT/apps/"
cp tests/fixtures/store.json "$OUT/cache/store.json"
cat > "$OUT/vortex.conf" <<CONF
font = $(pwd)/../assets/fonts/Roboto-Regular.ttf
apps_dir = $(pwd)/$OUT/apps
cache_dir = $(pwd)/$OUT/cache
install_cmd = $(pwd)/tests/fake-helper.sh
remove_cmd = $(pwd)/tests/fake-helper.sh
run_cmd = $(pwd)/tests/fake-helper.sh
call_cmd = $(pwd)/tests/fake-helper.sh
store_url = file://$(pwd)/tests/fixtures/store.json
screen_timeout = 0
CONF

# Second config whose apps dir has the store app "10000h" installed, for
# opening an app from its store page.
mkdir -p "$OUT/apps2"
cp -r tests/fixtures/apps/hello "$OUT/apps2/10000h"
sed "s|apps_dir = .*|apps_dir = $(pwd)/$OUT/apps2|" "$OUT/vortex.conf" > "$OUT/vortex2.conf"

fail=0
run() {
	name=$1; shift
	if ./vortex-shell-headless -c "${CONF:-$OUT/vortex.conf}" -k /dev/null --script "$*" 2>"$OUT/$name.log"; then
		echo "ok   $name"
	else
		echo "FAIL $name"; cat "$OUT/$name.log"; fail=1
	fi
}

run home       "shot:$OUT/home.ppm"
run launcher   "ok shot:$OUT/launcher.ppm right right down shot:$OUT/launcher-nav.ppm"
run store      "soft_left shot:$OUT/store.ppm down ok shot:$OUT/store-list.ppm ok shot:$OUT/store-detail.ppm down down shot:$OUT/store-detail-scroll.ppm"
run search     "soft_left ok 7 wait:1000 6 6 6 wait:1000 shot:$OUT/search.ppm ok shot:$OUT/search-results.ppm"
run install    "soft_left down ok ok ok wait:300 shot:$OUT/installing.ppm wait:1200 shot:$OUT/installed.ppm"
run settings   "soft_right right right shot:$OUT/settings.ppm"
run sysinfo    "soft_right up up ok shot:$OUT/sysinfo.ppm"
# Bluetooth devices (fake doas/vortex-radio in tests/fake-bin): paired first
PATH="$(pwd)/tests/fake-bin:$PATH" run bluetooth "soft_right down down down down down ok wait:600 shot:$OUT/bluetooth.ppm"
run dialer     "1 2 3 shot:$OUT/dialer.ppm"
# 12.5 × 4 = 50, then ÷ 0 -> Error
run calculator "ok right right right right ok 1 2 star 5 left 4 shot:$OUT/calc-op.ppm ok shot:$OUT/calc.ppm right 0 ok shot:$OUT/calc-err.ppm"
run power      "hold:end shot:$OUT/power.ppm"
# the store retries by itself after a failed download: the database only
# appears 2 s after the store is opened (like the network coming up late)
mkdir -p "$OUT/cache3"
sed -e "s|cache_dir = .*|cache_dir = $(pwd)/$OUT/cache3|" \
    -e "s|store_url = .*|store_url = file://$(pwd)/$OUT/late.json|" "$OUT/vortex.conf" > "$OUT/vortex3.conf"
(sleep 2; cp tests/fixtures/store.json "$OUT/late.json") &
CONF=$OUT/vortex3.conf run store-retry "soft_left wait:400 shot:$OUT/store-error.ppm wait:6000 wait:600 wait:600 shot:$OUT/store-retried.ppm"

# regression: the store is unloaded while an app runs; returning to the
# store page afterwards must not touch freed records
CONF=$OUT/vortex2.conf run store-open "soft_left down ok ok soft_left wait:1500 wait:300 shot:$OUT/store-after-app.ppm"
# the first installed app comes after the 7 built-in ones
run options    "ok right right right right right right right soft_left shot:$OUT/options.ppm down ok shot:$OUT/confirm.ppm"

# Messages: conversations (unread first message), then a thread
mkdir -p "$OUT/sms" "$OUT/run-none"
cp tests/fixtures/sms/messages.tsv "$OUT/sms/"
VORTEX_SMS_DIR=$OUT/sms VORTEX_RUN_DIR=$OUT/run-none run messages "ok right right right right right ok shot:$OUT/messages.ppm ok shot:$OUT/thread.ppm"
# an incoming call takes the screen; the status bar shows the mobile signal
VORTEX_SMS_DIR=$OUT/sms VORTEX_RUN_DIR=$(pwd)/tests/fixtures/run-call run call "wait:300 shot:$OUT/call.ppm"
# no torch LED on the test machine: says so
run torch      "ok right right right right right right ok shot:$OUT/torch.ppm"

# boot logo, mid-animation, with the first-boot message
echo "Setting up Vortex (first boot only). This takes a few minutes..." > "$OUT/splash.msg"
if ./vortex-splash --ppm "$OUT/splash.ppm" -t 1800 -m "$OUT/splash.msg" 2>"$OUT/splash.log"; then
	echo "ok   splash"
else
	echo "FAIL splash"; cat "$OUT/splash.log"; fail=1
fi

# every screenshot must exist and not be blank
for f in "$OUT"/*.ppm; do
	if [ "$(tail -c 4000 "$f" | od -An -tu1 | tr -s ' ' '\n' | sort -u | wc -l)" -lt 3 ]; then
		echo "FAIL $f looks empty"; fail=1
	fi
done
if command -v convert >/dev/null 2>&1; then
	for f in "$OUT"/*.ppm; do convert "$f" "${f%.ppm}.png"; done
fi
exit $fail
