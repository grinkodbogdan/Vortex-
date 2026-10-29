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

fail=0
run() {
	name=$1; shift
	if ./vortex-shell-headless -c "$OUT/vortex.conf" -k /dev/null --script "$*" 2>"$OUT/$name.log"; then
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
run dialer     "1 2 3 shot:$OUT/dialer.ppm"
run power      "hold:end shot:$OUT/power.ppm"
run options    "ok right right right right soft_left shot:$OUT/options.ppm down ok shot:$OUT/confirm.ppm"

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
