#!/bin/sh
# Runtime tests: install the fixture app with vortex-kai-install, check the
# result, then exercise the KaiOS shim in Chromium (if Playwright is present).
set -eu
cd "$(dirname "$0")/../.."
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
export VORTEX_APPS_DIR="$T/apps" VORTEX_SHIM="$PWD/runtime/kaios-shim.js" VORTEX_CONF=/nonexistent

(cd shell/tests/fixtures/apps && zip -qr "$T/plain.zip" hello)
# OmniSD layout: outer zip holding application.zip + metadata.json
(cd shell/tests/fixtures/apps/hello && zip -qr "$T/application.zip" .)
printf '{"version":1,"manifestURL":"app://hello/manifest.webapp"}' > "$T/metadata.json"
(cd "$T" && zip -q omni.zip application.zip metadata.json)

fail=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fail=1; fi; }

runtime/vortex-kai-install --id hello --file "$T/plain.zip" >/dev/null
check "plain zip installs" '[ -f "$T/apps/hello/manifest.webapp" ]'
check "shim copied" '[ -f "$T/apps/hello/vortex-kaios-shim.js" ]'
check "shim injected after doctype" 'head -c 80 "$T/apps/hello/index.html" | grep -q "^<!doctype html><script src=\"/vortex-kaios-shim.js\">"'
runtime/vortex-kai-install --id hello --file "$T/plain.zip" >/dev/null
check "reinstall injects once" '[ "$(grep -o vortex-kaios-shim "$T/apps/hello/index.html" | wc -l)" -eq 1 ]'
runtime/vortex-kai-install --id omni --file "$T/omni.zip" >/dev/null
check "OmniSD package installs" '[ -f "$T/apps/omni/manifest.webapp" ] && [ -f "$T/apps/omni/.vortex-metadata.json" ]'
check "bad id rejected" '! runtime/vortex-kai-install --id ../x --file "$T/plain.zip" 2>/dev/null'
check "no temp dirs left" '[ -z "$(find "$T/apps" -maxdepth 1 -name ".install-*")" ]'
runtime/vortex-kai-remove omni >/dev/null
check "remove" '[ ! -e "$T/apps/omni" ]'

if node -e "require('playwright')" 2>/dev/null || NODE_PATH=$(npm root -g 2>/dev/null) node -e "require('playwright')" 2>/dev/null; then
	NODE_PATH=${NODE_PATH:-$(npm root -g)} node runtime/tests/shim-test.js "$T/apps/hello" || fail=1
else
	echo "skip shim browser test (playwright not installed)"
fi
exit $fail
