#!/bin/sh
# Protocol tests for vortex-adbd over TCP (localhost only) with the real adb
# client: device listing, shell commands, exit/kill, push/pull/ls, errors.
# Needs: adb (Debian/Ubuntu package "adb").
set -eu
cd "$(dirname "$0")/.."
command -v adb >/dev/null || { echo "adb not installed, skipping"; exit 0; }
T=$(mktemp -d)
PORT=${PORT:-5599}
D=127.0.0.1:$PORT
export ANDROID_ADB_SERVER_PORT=${ANDROID_ADB_SERVER_PORT:-5099}
./vortex-adbd -t "$D" > "$T/adbd.log" 2>&1 &
ADBD=$!
cleanup() { kill "$ADBD" 2>/dev/null; adb kill-server 2>/dev/null; rm -rf "$T"; }
trap cleanup EXIT
i=0
until grep -q listening "$T/adbd.log"; do
	i=$((i + 1)); [ $i -lt 500 ] || { cat "$T/adbd.log"; exit 1; }
	sleep 0.01
done
fail() { echo "FAIL $*"; cat "$T/adbd.log"; exit 1; }
ok() { echo "ok   $*"; }

adb connect "$D" >/dev/null
timeout 10 adb -s "$D" wait-for-device
adb devices -l | grep -q "$D.*device product:vortex model:Nokia_6300_4G" || fail "device listing"
ok "connect"

[ "$(timeout 10 adb -s "$D" shell 'echo $((6*7))')" = 42 ] || fail "shell command"
ok "shell command"

[ "$(timeout 10 adb -s "$D" exec-out 'printf "a\0b"' | od -An -c | tr -d ' ')" = 'a\0b' ] || fail "exec-out binary"
ok "exec-out"

head -c 300000 /dev/urandom > "$T/blob"
timeout 20 adb -s "$D" push "$T/blob" "$T/remote/sub/blob" >/dev/null || fail "push"
timeout 20 adb -s "$D" pull "$T/remote/sub/blob" "$T/blob2" >/dev/null || fail "pull"
cmp -s "$T/blob" "$T/blob2" || fail "push/pull content"
ok "push/pull"

timeout 10 adb -s "$D" ls "$T/remote/sub" | grep -q ' blob$' || fail "ls"
ok "ls"

if timeout 10 adb -s "$D" pull /nonexistent-vortex "$T/x" 2>/dev/null; then fail "pull of a missing file"; fi
ok "pull error"

timeout 2 adb -s "$D" shell "sleep 37" || true
sleep 0.5
if ps -eo args | grep -q '^sleep 37'; then fail "command left running after the client went away"; fi
ok "stream close kills the command"
